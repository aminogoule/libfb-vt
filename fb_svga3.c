/*
 * fb_svga3.c -- libfb backend for VMware "SVGA3" (PCI 15ad:0406) on FreeBSD,
 *               linear framebuffer, no X, no DRM/KMS, no vmwgfx.
 *
 * SVGA3 vs. the older SVGA II (15ad:0405, see fb_svga.c)
 * -----------------------------------------------------
 * SVGA3 is the register-MMIO generation of VMware's virtual GPU -- the one
 * VMware hands to UEFI / arm64 guests, which have no x86 port I/O. The two
 * things that make fb_svga.c inapplicable here:
 *
 *   1. NO I/O ports. The index/value register protocol is gone. Registers are
 *      a plain 32-bit array memory-mapped in BAR0. Register index i lives at
 *      byte offset i*4 (matches vmwgfx: `u32 __iomem *rmmio; rmmio[i]`).
 *   2. NO legacy FIFO. There is no command ring. 2D and present commands are
 *      instead submitted through COMMAND BUFFERS: a 64-byte SVGACBHeader (in
 *      guest-visible memory) points at a stream of {u32 cmdId, payload...}
 *      commands, and its physical address is poked into COMMAND_HIGH/LOW. We
 *      carve the header + stream out of the VRAM tail (whose physical address
 *      we know) so the device can DMA it, enable command-buffer context 0 once
 *      at open, and use it for SVGA_CMD_UPDATE (present) and hardware
 *      SVGA_CMD_RECT_COPY (screen blit). There is no HW solid-fill command on
 *      SVGA, so fb_svga_fill is a software fill + present. If the device lacks
 *      SVGA_CAP_COMMAND_BUFFERS (or a submission fails) we fall back to a
 *      SVGA_REG_SYNC poke for present and software copy.
 *
 * Because there is no port I/O, this backend is architecture-independent: it
 * builds and runs on amd64 AND arm64 (the common SVGA3 case: FreeBSD/arm64 in
 * VMware Fusion on Apple Silicon).
 *
 * How it talks to the device
 * --------------------------
 *   - /dev/pci : find 15ad:0406, read BAR0 (registers) and BAR2 (VRAM) base
 *                and length via PCIOCGETBAR (handles 64-bit BARs).
 *   - /dev/mem : mmap BAR0 (register window) and BAR2 (linear framebuffer) at
 *                the physical addresses PCIOCGETBAR reported.
 *   - registers: read/write SVGA_REG_* as s_regs[index] (32-bit MMIO).
 *
 * Coexistence with vt(4): SVGA_REG_ENABLE=1 makes the device scan out our
 * VRAM; ENABLE=0 stops scanout (hand the screen back to vt). See
 * fb_svga_enable(), wired into the VT-switch handshake.
 *
 * Requirements at run time: root; kern.securelevel <= 0 (writable /dev/mem).
 */

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/pciio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>

#include "fb.h"
#include "fbsvga.h"

/* ------------------------------------------------------------------ *
 * SVGA register indices. Same numbering as SVGA II; on SVGA3 they are *
 * addressed as a 32-bit array in BAR0 (index i -> byte offset i*4).   *
 * ------------------------------------------------------------------ */
enum {
	SVGA_REG_ID              = 0,
	SVGA_REG_ENABLE          = 1,
	SVGA_REG_WIDTH           = 2,
	SVGA_REG_HEIGHT          = 3,
	SVGA_REG_MAX_WIDTH       = 4,
	SVGA_REG_MAX_HEIGHT      = 5,
	SVGA_REG_DEPTH           = 6,
	SVGA_REG_BITS_PER_PIXEL  = 7,
	SVGA_REG_BYTES_PER_LINE  = 12,
	SVGA_REG_FB_START        = 13,   /* phys base of framebuffer (== BAR2) */
	SVGA_REG_FB_OFFSET       = 14,   /* visible frame offset within VRAM   */
	SVGA_REG_VRAM_SIZE       = 15,
	SVGA_REG_FB_SIZE         = 16,
	SVGA_REG_CAPABILITIES    = 17,
	SVGA_REG_SYNC            = 21,   /* write => flush/present traced regions */
	SVGA_REG_BUSY            = 22,   /* non-zero while the device is working  */
	SVGA_REG_TRACES          = 45,   /* auto-present VRAM writes (legacy)   */
	SVGA_REG_COMMAND_LOW     = 48,   /* cmd-buffer header PA low | context  */
	SVGA_REG_COMMAND_HIGH    = 49    /* cmd-buffer header PA high 32 bits   */
};

/* device capabilities (SVGA_REG_CAPABILITIES) */
#define SVGA_CAP_RECT_COPY        0x00000002u
#define SVGA_CAP_COMMAND_BUFFERS  0x01000000u

/* ------------------------------------------------------------------ *
 * Command buffers -- the ONLY way to submit 2D/present commands on    *
 * SVGA3 (there is no FIFO). A 64-byte SVGACBHeader points at a stream *
 * of {u32 cmdId, payload...} commands; its physical address is poked  *
 * into COMMAND_HIGH/LOW (context id in the low bits of LOW). We place  *
 * both header and command stream in the tail of VRAM, whose physical  *
 * address we know, so the device can DMA them.                        *
 * ------------------------------------------------------------------ */

/* SVGACBHeader, laid out to match the device (natural alignment already
   yields the required 64 bytes -- asserted below, so no packing needed).
   The union in the spec is collapsed to its physical-address ("pa") form. */
typedef struct {
	uint32_t status;        /* SVGACBStatus: 0 NONE, 1 COMPLETED, ... */
	uint32_t errorOffset;
	uint64_t id;
	uint32_t flags;         /* SVGACBFlags: 0 == NONE                 */
	uint32_t length;        /* command stream length in bytes         */
	uint64_t pa;            /* physical address of the command stream */
	uint32_t offset;
	uint32_t dxContext;
	uint32_t mustBeZero[6];
} svga_cb_header_t;
typedef char svga_cb_header_size_check[sizeof(svga_cb_header_t) == 64 ? 1 : -1];

#define SVGA_CB_STATUS_NONE       0u
#define SVGA_CB_STATUS_COMPLETED  1u
#define SVGA_CB_FLAG_NONE         0u
#define SVGA_CB_CONTEXT_0         0x00u   /* regular (2D/present) commands */
#define SVGA_CB_CONTEXT_DEVICE    0x3fu   /* device-management commands    */

/* device-context command ids */
#define SVGA_DC_CMD_START_STOP_CONTEXT  1u

/* legacy 2D command ids (valid in a context-0 command buffer) */
#define SVGA_CMD_UPDATE      1u    /* {x, y, w, h}                       */
#define SVGA_CMD_RECT_COPY   3u    /* {srcX, srcY, dstX, dstY, w, h}     */

#define VMWARE_VENDOR  0x15AD
#define SVGA3_DEVICE   0x0406

/* SVGA_MAKE_ID(v) == (0x900000 << 8) | v */
#define SVGA_ID_2   0x90000002u
#define SVGA_ID_3   0x90000003u

/* SVGA_REG_ENABLE bit values */
#define SVGA_REG_ENABLE_DISABLE  0u
#define SVGA_REG_ENABLE_ENABLE   1u

#define VMWARE_VENDOR  0x15AD
#define SVGA3_DEVICE   0x0406

/* config-space BAR offsets (PCI header type 0) */
#define PCI_BAR_REGS   0x10          /* BAR0: register MMIO window */
#define PCI_BAR_VRAM   0x18          /* BAR2: linear framebuffer   */

/* ------------------------------------------------------------------ *
 * File-static device state (single-owner model, like fb_svga.c).     *
 * ------------------------------------------------------------------ */
static int       s_mem_fd   = -1;

static volatile uint32_t* s_regs = NULL;   /* mmap'd BAR0 register window */
static size_t    s_regs_len = 0;

static void*     s_vram     = NULL;         /* mmap'd BAR2 (whole VRAM)   */
static size_t    s_vram_len = 0;
static uint64_t  s_vram_phys = 0;           /* BAR2 physical base         */
static uint32_t  s_fb_offset = 0;           /* visible frame offset       */
static uint32_t  s_caps      = 0;           /* SVGA_REG_CAPABILITIES      */
static int       s_open     = 0;            /* device brought up          */

/* command-buffer scratch carved from the VRAM tail (see the CB comment) */
static int                s_cb_ok   = 0;    /* context 0 up, CBs usable   */
static volatile svga_cb_header_t* s_cb_hdr = NULL;
static volatile uint32_t*         s_cb_cmd = NULL;   /* command stream     */
static uint64_t           s_cb_hdr_phys = 0;
static uint64_t           s_cb_cmd_phys = 0;

/* register state captured at open, restored on close so the vt(4) console
   scanout comes back (SVGA3 has no VGA text emulation to fall back to) */
static int       s_saved    = 0;
static uint32_t  s_save_w, s_save_h, s_save_bpp, s_save_enable;

static __inline void svga_write(int reg, uint32_t val) {
	s_regs[reg] = val;                       /* MMIO: index i @ i*4 bytes */
}
static __inline uint32_t svga_read(int reg) {
	return s_regs[reg];
}

/* ------------------------------------------------------------------ *
 * Command-buffer submission.                                         *
 * ------------------------------------------------------------------ */

/* Submit the command stream already written at s_cb_cmd (len bytes) under the
   given context, and wait (bounded) for the device to complete it. Returns 0
   on SVGA_CB_STATUS_COMPLETED, -1 otherwise. */
static int cb_submit(uint32_t context, uint32_t len) {
	volatile svga_cb_header_t* h = s_cb_hdr;
	int i, guard = 10000000;

	h->status      = SVGA_CB_STATUS_NONE;
	h->errorOffset = 0;
	h->id          = 0;
	h->flags       = SVGA_CB_FLAG_NONE;
	h->length      = len;
	h->pa          = s_cb_cmd_phys;
	h->offset      = 0;
	h->dxContext   = 0;
	for (i = 0; i < 6; i++)
		h->mustBeZero[i] = 0;

	__sync_synchronize();   /* header + commands must land before the kick */

	/* HIGH first, then LOW -- the LOW write (carrying the context in its low
	   bits; the 64-byte-aligned header keeps them free) triggers processing. */
	svga_write(SVGA_REG_COMMAND_HIGH, (uint32_t)(s_cb_hdr_phys >> 32));
	svga_write(SVGA_REG_COMMAND_LOW,
	           (uint32_t)(s_cb_hdr_phys & 0xffffffffu) | context);

	while (h->status == SVGA_CB_STATUS_NONE && guard-- > 0)
		__sync_synchronize();

	return (h->status == SVGA_CB_STATUS_COMPLETED) ? 0 : -1;
}

/* enable(1)/disable(0) command-buffer context 0 via the device context */
static int cb_start_context0(int enable) {
	volatile uint32_t* c = s_cb_cmd;
	c[0] = SVGA_DC_CMD_START_STOP_CONTEXT;
	c[1] = (uint32_t)enable;
	c[2] = SVGA_CB_CONTEXT_0;
	return cb_submit(SVGA_CB_CONTEXT_DEVICE, 3 * (uint32_t)sizeof(uint32_t));
}

/* present a dirty rectangle (SVGA_CMD_UPDATE) via context 0 */
static int cb_update(int x, int y, int w, int h) {
	volatile uint32_t* c = s_cb_cmd;
	c[0] = SVGA_CMD_UPDATE;
	c[1] = (uint32_t)x; c[2] = (uint32_t)y;
	c[3] = (uint32_t)w; c[4] = (uint32_t)h;
	return cb_submit(SVGA_CB_CONTEXT_0, 5 * (uint32_t)sizeof(uint32_t));
}

/* hardware screen-to-screen blit (SVGA_CMD_RECT_COPY) via context 0 */
static int cb_rect_copy(int sx, int sy, int dx, int dy, int w, int h) {
	volatile uint32_t* c = s_cb_cmd;
	c[0] = SVGA_CMD_RECT_COPY;
	c[1] = (uint32_t)sx; c[2] = (uint32_t)sy;
	c[3] = (uint32_t)dx; c[4] = (uint32_t)dy;
	c[5] = (uint32_t)w;  c[6] = (uint32_t)h;
	return cb_submit(SVGA_CB_CONTEXT_0, 7 * (uint32_t)sizeof(uint32_t));
}

/* ------------------------------------------------------------------ *
 * PCI discovery: find 15ad:0406 and read BAR0/BAR2 base + length.    *
 * PCIOCGETBAR returns the full (possibly 64-bit) base with the BAR   *
 * flag bits in the low nibble, plus the region length for mmap.      *
 * ------------------------------------------------------------------ */
static int svga_find_bars(uint64_t* regs_base, uint64_t* regs_len,
                          uint64_t* vram_base, uint64_t* vram_len) {
	int                 pcifd;
	struct pci_conf     confbuf[64];
	struct pci_conf_io  pc;
	struct pci_bar_io   bar;
	struct pcisel       sel;
	unsigned            i;
	int                 found = -1;

	if ((pcifd = open("/dev/pci", O_RDWR)) == -1)
		return -1;

	memset(&pc, 0, sizeof(pc));
	pc.match_buf_len = sizeof(confbuf);
	pc.matches       = confbuf;

	if (ioctl(pcifd, PCIOCGETCONF, &pc) == -1 ||
	    pc.status == PCI_GETCONF_ERROR) {
		close(pcifd);
		return -1;
	}

	for (i = 0; i < pc.num_matches; i++) {
		if (confbuf[i].pc_vendor == VMWARE_VENDOR &&
		    confbuf[i].pc_device == SVGA3_DEVICE) {
			sel = confbuf[i].pc_sel;
			found = 0;
			break;
		}
	}
	if (found != 0) {
		close(pcifd);
		errno = ENODEV;
		return -1;
	}

	/* BAR0: register MMIO window */
	memset(&bar, 0, sizeof(bar));
	bar.pbi_sel = sel;
	bar.pbi_reg = PCI_BAR_REGS;
	if (ioctl(pcifd, PCIOCGETBAR, &bar) == -1) { close(pcifd); return -1; }
	*regs_base = bar.pbi_base & ~0xfULL;      /* strip memory-BAR flag bits */
	*regs_len  = bar.pbi_length;

	/* BAR2: linear framebuffer (VRAM) */
	memset(&bar, 0, sizeof(bar));
	bar.pbi_sel = sel;
	bar.pbi_reg = PCI_BAR_VRAM;
	if (ioctl(pcifd, PCIOCGETBAR, &bar) == -1) { close(pcifd); return -1; }
	*vram_base = bar.pbi_base & ~0xfULL;
	*vram_len  = bar.pbi_length;

	close(pcifd);
	if (*regs_len == 0 || *vram_len == 0) { errno = ENXIO; return -1; }
	return 0;
}

/* Establish the highest SVGA protocol id the device agrees to (>= 3 here). */
static int svga_negotiate_id(void) {
	svga_write(SVGA_REG_ID, SVGA_ID_3);
	if (svga_read(SVGA_REG_ID) == SVGA_ID_3) return 0;
	svga_write(SVGA_REG_ID, SVGA_ID_2);
	if (svga_read(SVGA_REG_ID) == SVGA_ID_2) return 0;
	return -1;
}

/* ------------------------------------------------------------------ *
 * fbsvga.h: mode set + open.                                         *
 * ------------------------------------------------------------------ */
framebuffer_t* fb_svga_open(int width, int height, int bpp, int db) {
	framebuffer_t* fb;
	uint64_t       regs_base, regs_len, vram_base, vram_len;
	uint32_t       mw, mh;

	if ((fb = calloc(1, sizeof(*fb))) == NULL)
		return NULL;
	fb->fd = -1;

	if (svga_find_bars(&regs_base, &regs_len, &vram_base, &vram_len) != 0)
		goto fail;

	if ((s_mem_fd = open("/dev/mem", O_RDWR)) == -1)
		goto fail;                            /* needs root */

	/* map the register window (BAR0) and VRAM (BAR2) */
	s_regs_len = (size_t)regs_len;
	s_regs = mmap(NULL, s_regs_len, PROT_READ | PROT_WRITE,
	              MAP_SHARED, s_mem_fd, (off_t)regs_base);
	if (s_regs == MAP_FAILED) { s_regs = NULL; goto fail; }

	s_vram_len  = (size_t)vram_len;
	s_vram_phys = vram_base;
	s_vram = mmap(NULL, s_vram_len, PROT_READ | PROT_WRITE,
	              MAP_SHARED, s_mem_fd, (off_t)vram_base);
	if (s_vram == MAP_FAILED) { s_vram = NULL; goto fail; }

	if (svga_negotiate_id() != 0) { errno = ENXIO; goto fail; }
	s_open = 1;

	/* snapshot the mode the firmware/console left in the SVGA registers, so
	   fb_close() can hand the scanout back instead of just blanking it */
	s_save_w      = svga_read(SVGA_REG_WIDTH);
	s_save_h      = svga_read(SVGA_REG_HEIGHT);
	s_save_bpp    = svga_read(SVGA_REG_BITS_PER_PIXEL);
	s_save_enable = svga_read(SVGA_REG_ENABLE);
	s_saved       = (s_save_w != 0 && s_save_h != 0 && s_save_enable != 0);

	/* clamp requested mode to what the device allows */
	mw = svga_read(SVGA_REG_MAX_WIDTH);
	mh = svga_read(SVGA_REG_MAX_HEIGHT);
	if (width  <= 0 || (uint32_t)width  > mw) width  = (int)mw;
	if (height <= 0 || (uint32_t)height > mh) height = (int)mh;
	if (bpp    <= 0) bpp = 32;

	/* program the mode and light up scanout */
	svga_write(SVGA_REG_WIDTH,          (uint32_t)width);
	svga_write(SVGA_REG_HEIGHT,         (uint32_t)height);
	svga_write(SVGA_REG_BITS_PER_PIXEL, (uint32_t)bpp);
	svga_write(SVGA_REG_TRACES,         1);     /* auto-present FB writes  */
	svga_write(SVGA_REG_ENABLE,         SVGA_REG_ENABLE_ENABLE);

	/* read back the authoritative geometry */
	s_fb_offset = svga_read(SVGA_REG_FB_OFFSET);

	fb->info.fb_width  = width;
	fb->info.fb_height = height;
	fb->info.fb_depth  = bpp;
	fb->bytes_pp       = (bpp + 7) / 8;
	fb->stride         = svga_read(SVGA_REG_BYTES_PER_LINE);
	fb->back_stride    = (size_t)width * (size_t)fb->bytes_pp;
	fb->info.fb_size   = (int)(fb->stride * (size_t)height);
	fb->map_size       = s_vram_len;
	fb->vram           = (uint8_t*)s_vram + s_fb_offset;   /* visible frame */

	if ((size_t)s_fb_offset + fb->stride * (size_t)height > s_vram_len) {
		errno = ENXIO;                        /* device handed back bogus geometry */
		goto fail;
	}

	/* Set up command buffers if the device supports them: carve one page from
	   the very tail of VRAM for the header + command stream (well past the
	   visible frame), then start context 0. On any failure we simply run
	   without acceleration -- present falls back to a SVGA_REG_SYNC poke and
	   copy/fill run in software. */
	s_caps  = svga_read(SVGA_REG_CAPABILITIES);
	s_cb_ok = 0;
	if (s_caps & SVGA_CAP_COMMAND_BUFFERS) {
		size_t fb_end = (size_t)s_fb_offset + fb->stride * (size_t)height;
		size_t region = 4096;                 /* one page: 64B header + stream */
		if (s_vram_len > region) {
			size_t off = (s_vram_len - region) & ~((size_t)0xfff);  /* page-aligned */
			if (off >= fb_end) {
				s_cb_hdr      = (volatile svga_cb_header_t*)((uint8_t*)s_vram + off);
				s_cb_cmd      = (volatile uint32_t*)((uint8_t*)s_vram + off + 64);
				s_cb_hdr_phys = s_vram_phys + off;
				s_cb_cmd_phys = s_cb_hdr_phys + 64;
				if (cb_start_context0(1) == 0)
					s_cb_ok = 1;
			}
		}
	}

	if (db) {
		fb->back = malloc(fb->back_stride * (size_t)height);
		/* if this fails we simply run single-buffered (draw into VRAM) */
	}
	return fb;

fail:
	fb_close(fb);
	return NULL;
}

/* Default entry point used by ppm2fb.c / fbshow.c / server.c. */
framebuffer_t* fb_open(const char* dev, int db) {
	(void)dev;                                  /* device is found via PCI */
	return fb_svga_open(1024, 768, 32, db);     /* sane default mode       */
}

void fb_close(framebuffer_t* fb) {
	if (s_open) {
		if (s_cb_ok) {
			(void)cb_start_context0(0);       /* stop command-buffer context 0 */
			s_cb_ok = 0;
		}
		if (s_saved) {
			/* restore the console's original scanout mode and present it */
			svga_write(SVGA_REG_ENABLE, SVGA_REG_ENABLE_DISABLE);
			svga_write(SVGA_REG_WIDTH,          s_save_w);
			svga_write(SVGA_REG_HEIGHT,         s_save_h);
			svga_write(SVGA_REG_BITS_PER_PIXEL, s_save_bpp);
			svga_write(SVGA_REG_ENABLE,         s_save_enable);
			svga_write(SVGA_REG_SYNC, 1);
		} else {
			svga_write(SVGA_REG_ENABLE, SVGA_REG_ENABLE_DISABLE);  /* stop scanout */
		}
		s_open = 0;
	}
	if (s_vram && s_vram != MAP_FAILED) munmap(s_vram, s_vram_len);
	if (s_regs && s_regs != MAP_FAILED) munmap((void*)s_regs, s_regs_len);
	s_vram = NULL; s_regs = NULL;
	if (s_mem_fd != -1) { close(s_mem_fd); s_mem_fd = -1; }

	if (fb == NULL)
		return;
	free(fb->back);
	free(fb);
}

void* fb_drawbuf(framebuffer_t* fb) {
	return fb->back ? fb->back : fb->vram;
}

size_t fb_pitch(framebuffer_t* fb) {
	return fb->back ? fb->back_stride : fb->stride;
}

/* fb_flip: if double-buffered, copy back->VRAM honoring the HW pitch, then
   force the device to present (fb_svga_update). */
int fb_flip(framebuffer_t* fb) {
	if (fb->back != NULL) {
		size_t   y, rows = (size_t)fb->info.fb_height;
		uint8_t* dst = (uint8_t*)fb->vram;
		uint8_t* src = (uint8_t*)fb->back;
		for (y = 0; y < rows; y++)
			memcpy(dst + y * fb->stride,
			       src + y * fb->back_stride,
			       fb->back_stride);
	}
	return fb_svga_update(fb, 0, 0, fb->info.fb_width, fb->info.fb_height);
}

/* Real-time mode change: reprogram WIDTH/HEIGHT/BPP (disable/set/enable). VRAM
   stays mapped; only the visible window (FB_OFFSET) and stride move. */
int fb_resize(framebuffer_t* fb, int width, int height) {
	uint32_t mw, mh, new_stride, need;

	if (fb == NULL || !s_open)
		return -1;

	mw = svga_read(SVGA_REG_MAX_WIDTH);
	mh = svga_read(SVGA_REG_MAX_HEIGHT);
	if (width  <= 0 || (uint32_t)width  > mw) width  = (int)mw;
	if (height <= 0 || (uint32_t)height > mh) height = (int)mh;

	svga_write(SVGA_REG_ENABLE, SVGA_REG_ENABLE_DISABLE);
	svga_write(SVGA_REG_WIDTH,          (uint32_t)width);
	svga_write(SVGA_REG_HEIGHT,         (uint32_t)height);
	svga_write(SVGA_REG_BITS_PER_PIXEL, (uint32_t)fb->info.fb_depth);
	svga_write(SVGA_REG_ENABLE, SVGA_REG_ENABLE_ENABLE);

	new_stride  = svga_read(SVGA_REG_BYTES_PER_LINE);
	need        = new_stride * (uint32_t)height;
	s_fb_offset = svga_read(SVGA_REG_FB_OFFSET);
	if (need == 0 || (size_t)s_fb_offset + need > s_vram_len) {
		errno = ENXIO;              /* device handed back a bogus geometry */
		return -1;
	}

	fb->info.fb_width  = width;
	fb->info.fb_height = height;
	fb->stride          = new_stride;
	fb->back_stride     = (size_t)width * (size_t)fb->bytes_pp;
	fb->info.fb_size    = (int)need;
	fb->vram            = (uint8_t*)s_vram + s_fb_offset;

	if (fb->back != NULL) {
		void* nb = realloc(fb->back, fb->back_stride * (size_t)height);
		if (nb == NULL)
			return -1;           /* geometry already committed; caller must
			                        stop drawing rather than trust fb->back */
		fb->back = nb;
	}

	memset(fb->vram, 0, (size_t)need);   /* new area may hold stale VRAM bytes */
	return 0;
}

/* ------------------------------------------------------------------ *
 * fbsvga.h: present + enable + (software-only) 2D.                   *
 * ------------------------------------------------------------------ */

/*
 * Present a dirty rectangle. SVGA3 has no FIFO, so the proper present is an
 * SVGA_CMD_UPDATE submitted through a command buffer (context 0) -- this both
 * names the changed region and forces the host to scan it out. If command
 * buffers are unavailable (or a submission fails) we fall back to poking
 * SVGA_REG_SYNC and draining SVGA_REG_BUSY: the register access alone forces
 * the host to flush traced framebuffer writes. Without either, pixels sit in
 * VRAM and appear "stuck" until some unrelated register touch (e.g. a keypress
 * path) happens to force a VM exit. The BUSY drain is bounded so a device that
 * never advertises BUSY can't hang.
 */
int fb_svga_update(framebuffer_t* fb, int x, int y, int w, int h) {
	int guard = 1000000;
	(void)fb;
	if (!s_open)
		return -1;
	if (s_cb_ok && cb_update(x, y, w, h) == 0)
		return 0;
	svga_write(SVGA_REG_SYNC, 1);
	while (guard-- > 0 && svga_read(SVGA_REG_BUSY))
		;
	return 0;
}

void fb_svga_enable(framebuffer_t* fb, int on) {
	if (!s_open && !on)
		return;
	if (on) {
		/* re-assert mode in case the guest lost it while switched away */
		svga_write(SVGA_REG_WIDTH,          (uint32_t)fb->info.fb_width);
		svga_write(SVGA_REG_HEIGHT,         (uint32_t)fb->info.fb_height);
		svga_write(SVGA_REG_BITS_PER_PIXEL, (uint32_t)fb->info.fb_depth);
		svga_write(SVGA_REG_TRACES,         1);
		svga_write(SVGA_REG_ENABLE, SVGA_REG_ENABLE_ENABLE);
	} else {
		svga_write(SVGA_REG_ENABLE, SVGA_REG_ENABLE_DISABLE);  /* stop scanout */
	}
}

/* HW accel available when command buffers are up and the device advertises the
   screen-to-screen copy command. (There is no HW solid-fill command on SVGA;
   fb_svga_fill is always software, then presented.) */
int fb_svga_have_accel(framebuffer_t* fb) {
	(void)fb;
	return (s_cb_ok && (s_caps & SVGA_CAP_RECT_COPY)) ? 1 : 0;
}

/* Solid fill. SVGA has no RECT_FILL command, so this is always a software fill
   into VRAM followed by a present of the touched rectangle. */
int fb_svga_fill(framebuffer_t* fb, int x, int y, int w, int h, uint32_t color) {
	int      row, col;
	uint8_t* base = (uint8_t*)fb->vram;

	if (x < 0 || y < 0 || w <= 0 || h <= 0)
		return -1;

	for (row = 0; row < h; row++) {
		uint32_t* line = (uint32_t*)(base + (size_t)(y + row) * fb->stride);
		for (col = 0; col < w; col++)
			line[x + col] = color;
	}
	return fb_svga_update(fb, x, y, w, h);
}

/* Screen-to-screen blit. Uses the hardware SVGA_CMD_RECT_COPY via a command
   buffer when available (ideal for console scroll / window drag: no CPU pixel
   traffic), else an overlap-safe software memmove within VRAM. Either way the
   destination rectangle is presented. */
int fb_svga_copy(framebuffer_t* fb, int sx, int sy, int dx, int dy, int w, int h) {
	int      row;
	uint8_t* base = (uint8_t*)fb->vram;
	size_t   bpp  = (size_t)fb->bytes_pp;

	if (w <= 0 || h <= 0)
		return -1;

	if (s_cb_ok && (s_caps & SVGA_CAP_RECT_COPY)) {
		/* the device performs the move and presents the result itself */
		if (cb_rect_copy(sx, sy, dx, dy, w, h) == 0)
			return 0;
		/* fall through to the software path on a submission error */
	}

	/* memmove rows within VRAM, overlap-safe (top-down or bottom-up) */
	if (dy <= sy) {
		for (row = 0; row < h; row++)
			memmove(base + (size_t)(dy + row) * fb->stride + (size_t)dx * bpp,
			        base + (size_t)(sy + row) * fb->stride + (size_t)sx * bpp,
			        (size_t)w * bpp);
	} else {
		for (row = h - 1; row >= 0; row--)
			memmove(base + (size_t)(dy + row) * fb->stride + (size_t)dx * bpp,
			        base + (size_t)(sy + row) * fb->stride + (size_t)sx * bpp,
			        (size_t)w * bpp);
	}
	return fb_svga_update(fb, dx, dy, w, h);
}

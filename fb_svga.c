/*
 * fb_svga.c -- libfb backend for VMware SVGA II on FreeBSD, linear framebuffer
 *              with hardware 2D, no X, no DRM/KMS, no vmwgfx.
 *
 * Drop-in for fb.c (same fb.h API) plus the fbsvga.h extensions. Build with
 * fb_svga.c INSTEAD of fb.c / fb_vga.c when the display adapter is VMware
 * SVGA II (PCI 15ad:0405, e.g. dmesg "vgapci0 ... at device 15.0").
 *
 * How it talks to the device
 * --------------------------
 *   - /dev/pci  : find 15ad:0405, read BAR0 to get the SVGA I/O port base.
 *   - /dev/io   : raise IOPL so our outl/inl to the index/value ports work.
 *   - index/value ports (BAR0): read/write SVGA_REG_* (32-bit).
 *   - /dev/mem  : mmap the VRAM BAR (linear framebuffer) and the FIFO BAR
 *                 (2D command ring), at the physical addresses the device
 *                 reports in SVGA_REG_FB_START / SVGA_REG_MEM_START.
 *
 * Coexistence with vt(4)
 * ----------------------
 * vt_vga owns the *legacy VGA* console (ports 0x3xx, text @ 0xB8000). We own
 * the *SVGA* register set. Setting SVGA_REG_ENABLE=1 makes the device scan out
 * from VRAM (our pixels); ENABLE=0 returns it to VGA emulation (vt's text).
 * That is the whole cooperation model -- see fb_svga_enable().
 *
 * !!! CONSTANTS MARKED [VERIFY] should be checked against VMware's svga_reg.h.
 *     The SVGA_REG_* / SVGA_ID_* / SVGA_FIFO_* / SVGA_CMD_UPDATE values are
 *     stable and used everywhere; the legacy RECT_FILL / RECT_COPY command
 *     numbers and cap bits are the ones worth confirming. Everything still
 *     works without them (software fallback), so accel is best-effort.
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
 * SVGA register indices (index/value protocol). Stable.              *
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
	SVGA_REG_PSEUDOCOLOR     = 8,
	SVGA_REG_RED_MASK        = 9,
	SVGA_REG_GREEN_MASK      = 10,
	SVGA_REG_BLUE_MASK       = 11,
	SVGA_REG_BYTES_PER_LINE  = 12,
	SVGA_REG_FB_START        = 13,   /* phys base of framebuffer  */
	SVGA_REG_FB_OFFSET       = 14,   /* visible frame offset in VRAM */
	SVGA_REG_VRAM_SIZE       = 15,
	SVGA_REG_FB_SIZE         = 16,
	SVGA_REG_CAPABILITIES    = 17,
	SVGA_REG_MEM_START       = 18,   /* phys base of FIFO region  */
	SVGA_REG_MEM_SIZE        = 19,
	SVGA_REG_CONFIG_DONE     = 20,
	SVGA_REG_SYNC            = 21,
	SVGA_REG_BUSY            = 22
};

#define SVGA_ID_0   0x90000000
#define SVGA_ID_1   0x90000001
#define SVGA_ID_2   0x90000002

/* I/O port offsets from BAR0 base (dword accesses). Stable. */
#define SVGA_INDEX_PORT  0
#define SVGA_VALUE_PORT  1

/* FIFO header, indices in DWORDS; the values stored there are BYTE offsets. */
enum {
	SVGA_FIFO_MIN       = 0,
	SVGA_FIFO_MAX       = 1,
	SVGA_FIFO_NEXT_CMD  = 2,
	SVGA_FIFO_STOP      = 3
};

/* FIFO 2D commands. UPDATE is stable; the rest are [VERIFY]. */
#define SVGA_CMD_UPDATE     1        /* {cmd, x, y, w, h}                      */
#define SVGA_CMD_RECT_FILL  2        /* [VERIFY] {cmd, pixel, x, y, w, h}      */
#define SVGA_CMD_RECT_COPY  3        /* [VERIFY] {cmd, sx, sy, dx, dy, w, h}   */

/* Capability bits (low legacy 2D bits). [VERIFY] */
#define SVGA_CAP_RECT_FILL  0x00000001
#define SVGA_CAP_RECT_COPY  0x00000002

#define VMWARE_VENDOR  0x15AD
#define SVGA2_DEVICE   0x0405

/* ------------------------------------------------------------------ *
 * x86 dword port I/O (index/value are 32-bit). /dev/io grants IOPL.  *
 * ------------------------------------------------------------------ */
#if defined(__amd64__) || defined(__i386__)
static __inline void svga_outl(uint16_t port, uint32_t val) {
	__asm__ __volatile__("outl %0, %1" :: "a"(val), "Nd"(port));
}
static __inline uint32_t svga_inl(uint16_t port) {
	uint32_t v;
	__asm__ __volatile__("inl %1, %0" : "=a"(v) : "Nd"(port));
	return v;
}
#else
#error "fb_svga.c requires x86 port I/O for the SVGA index/value registers"
#endif

/* ------------------------------------------------------------------ *
 * File-static device state (single-owner model, like fb_vga.c).      *
 * ------------------------------------------------------------------ */
static int       s_io_fd   = -1;
static int       s_mem_fd  = -1;
static uint16_t  s_iobase  = 0;

static volatile uint32_t* s_fifo = NULL;   /* mmap'd FIFO region        */
static size_t    s_fifo_len = 0;

static void*     s_vram = NULL;            /* mmap'd VRAM (whole BAR)   */
static size_t    s_vram_len = 0;
static uint32_t  s_fb_offset = 0;          /* visible frame offset      */
static uint32_t  s_caps = 0;

static __inline void  svga_write(int reg, uint32_t val) {
	svga_outl(s_iobase + SVGA_INDEX_PORT, (uint32_t)reg);
	svga_outl(s_iobase + SVGA_VALUE_PORT, val);
}
static __inline uint32_t svga_read(int reg) {
	svga_outl(s_iobase + SVGA_INDEX_PORT, (uint32_t)reg);
	return svga_inl(s_iobase + SVGA_VALUE_PORT);
}

/* Push the FIFO and wait until the device drained it (legacy sync). */
static void svga_sync(void) {
	svga_write(SVGA_REG_SYNC, 1);
	while (svga_read(SVGA_REG_BUSY))
		;
}

/* Append one dword to the FIFO command ring (byte-offset arithmetic). */
static void fifo_write(uint32_t v) {
	volatile uint32_t* f = s_fifo;
	uint32_t next = f[SVGA_FIFO_NEXT_CMD];
	uint32_t min  = f[SVGA_FIFO_MIN];
	uint32_t max  = f[SVGA_FIFO_MAX];

	/* If the ring is full (next about to hit stop), let the device catch up. */
	while (((next + 4 == f[SVGA_FIFO_STOP])) ||
	       (next + 4 == max && f[SVGA_FIFO_STOP] == min))
		svga_sync();

	f[next / 4] = v;
	next += 4;
	if (next == max)
		next = min;
	f[SVGA_FIFO_NEXT_CMD] = next;
}

/* ------------------------------------------------------------------ *
 * PCI discovery: find 15ad:0405 and read BAR0 (the SVGA I/O base).   *
 * ------------------------------------------------------------------ */
static int svga_find_iobase(uint16_t* iobase_out) {
	int                  pcifd;
	struct pci_conf      confbuf[64];
	struct pci_conf_io   pc;
	struct pci_io        io;
	unsigned             i;
	int                  found = -1;

	if ((pcifd = open("/dev/pci", O_RDWR)) == -1)
		return -1;

	memset(&pc, 0, sizeof(pc));
	pc.match_buf_len = sizeof(confbuf);
	pc.matches       = confbuf;

	/* One GETCONF pass is enough for a machine with a handful of devices. */
	if (ioctl(pcifd, PCIOCGETCONF, &pc) == -1 ||
	    pc.status == PCI_GETCONF_ERROR) {
		close(pcifd);
		return -1;
	}

	for (i = 0; i < pc.num_matches; i++) {
		if (confbuf[i].pc_vendor == VMWARE_VENDOR &&
		    confbuf[i].pc_device == SVGA2_DEVICE) {
			/* read BAR0 (config offset 0x10): I/O space, base in bits 15..2 */
			memset(&io, 0, sizeof(io));
			io.pi_sel   = confbuf[i].pc_sel;
			io.pi_reg   = 0x10;
			io.pi_width = 4;
			if (ioctl(pcifd, PCIOCREAD, &io) == -1)
				continue;
			if ((io.pi_data & 0x1) == 0)   /* bit0 set => I/O space BAR */
				continue;
			*iobase_out = (uint16_t)(io.pi_data & ~0x3u);
			found = 0;
			break;
		}
	}
	close(pcifd);
	return found;
}

/* Establish the highest SVGA protocol id the device agrees to. */
static int svga_negotiate_id(void) {
	svga_write(SVGA_REG_ID, SVGA_ID_2);
	if (svga_read(SVGA_REG_ID) == SVGA_ID_2) return 0;
	svga_write(SVGA_REG_ID, SVGA_ID_1);
	if (svga_read(SVGA_REG_ID) == SVGA_ID_1) return 0;
	svga_write(SVGA_REG_ID, SVGA_ID_0);
	if (svga_read(SVGA_REG_ID) == SVGA_ID_0) return 0;
	return -1;
}

/* ------------------------------------------------------------------ *
 * fbsvga.h: mode set + open.                                         *
 * ------------------------------------------------------------------ */
framebuffer_t* fb_svga_open(int width, int height, int bpp, int db) {
	framebuffer_t* fb;
	uint32_t       vram_phys, fifo_phys, mw, mh;

	if ((fb = calloc(1, sizeof(*fb))) == NULL)
		return NULL;
	fb->fd = -1;

	if (svga_find_iobase(&s_iobase) != 0) { errno = ENODEV; goto fail; }

	/* IOPL for our port I/O. */
	if ((s_io_fd = open("/dev/io", O_RDWR)) == -1)
		goto fail;                       /* needs root */

	if (svga_negotiate_id() != 0) { errno = ENXIO; goto fail; }

	/* clamp requested mode to what the device allows */
	mw = svga_read(SVGA_REG_MAX_WIDTH);
	mh = svga_read(SVGA_REG_MAX_HEIGHT);
	if (width  <= 0 || (uint32_t)width  > mw) width  = (int)mw;
	if (height <= 0 || (uint32_t)height > mh) height = (int)mh;
	if (bpp    <= 0) bpp = 32;

	/* physical bases + sizes of VRAM and FIFO, straight from the device */
	vram_phys  = svga_read(SVGA_REG_FB_START);
	s_vram_len = svga_read(SVGA_REG_VRAM_SIZE);
	fifo_phys  = svga_read(SVGA_REG_MEM_START);
	s_fifo_len = svga_read(SVGA_REG_MEM_SIZE);
	s_caps     = svga_read(SVGA_REG_CAPABILITIES);
	if (s_vram_len == 0 || s_fifo_len == 0) { errno = ENXIO; goto fail; }

	/* map both BARs through /dev/mem (offset == physical address) */
	if ((s_mem_fd = open("/dev/mem", O_RDWR)) == -1)
		goto fail;
	s_vram = mmap(NULL, s_vram_len, PROT_READ | PROT_WRITE,
	              MAP_SHARED, s_mem_fd, (off_t)vram_phys);
	if (s_vram == MAP_FAILED) { s_vram = NULL; goto fail; }
	s_fifo = mmap(NULL, s_fifo_len, PROT_READ | PROT_WRITE,
	              MAP_SHARED, s_mem_fd, (off_t)fifo_phys);
	if (s_fifo == MAP_FAILED) { s_fifo = NULL; goto fail; }

	/* program the mode */
	svga_write(SVGA_REG_WIDTH,          (uint32_t)width);
	svga_write(SVGA_REG_HEIGHT,         (uint32_t)height);
	svga_write(SVGA_REG_BITS_PER_PIXEL, (uint32_t)bpp);

	/* bring up the FIFO: reserve the 4 header dwords, empty ring. */
	s_fifo[SVGA_FIFO_MIN]      = 4 * sizeof(uint32_t);   /* 16 bytes */
	s_fifo[SVGA_FIFO_MAX]      = (uint32_t)s_fifo_len;
	s_fifo[SVGA_FIFO_NEXT_CMD] = s_fifo[SVGA_FIFO_MIN];
	s_fifo[SVGA_FIFO_STOP]     = s_fifo[SVGA_FIFO_MIN];
	svga_write(SVGA_REG_CONFIG_DONE, 1);

	/* light it up: device now scans out from VRAM */
	svga_write(SVGA_REG_ENABLE, 1);

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
	if (s_iobase != 0 && s_io_fd != -1) {
		svga_write(SVGA_REG_ENABLE, 0);          /* back to VGA emulation */
		svga_write(SVGA_REG_CONFIG_DONE, 0);
	}
	if (s_fifo && s_fifo != MAP_FAILED) munmap((void*)s_fifo, s_fifo_len);
	if (s_vram && s_vram != MAP_FAILED) munmap(s_vram, s_vram_len);
	s_fifo = NULL; s_vram = NULL;
	if (s_mem_fd != -1) { close(s_mem_fd); s_mem_fd = -1; }
	if (s_io_fd  != -1) { close(s_io_fd);  s_io_fd  = -1; }
	s_iobase = 0;

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
   present the whole screen. Single-buffered: nothing to copy, just present. */
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

/* ------------------------------------------------------------------ *
 * fbsvga.h: present + enable + hardware 2D.                          *
 * ------------------------------------------------------------------ */
int fb_svga_update(framebuffer_t* fb, int x, int y, int w, int h) {
	if (s_fifo == NULL)
		return -1;
	fifo_write(SVGA_CMD_UPDATE);
	fifo_write((uint32_t)x);
	fifo_write((uint32_t)y);
	fifo_write((uint32_t)w);
	fifo_write((uint32_t)h);
	svga_sync();
	(void)fb;
	return 0;
}

void fb_svga_enable(framebuffer_t* fb, int on) {
	(void)fb;
	if (s_iobase == 0)
		return;
	if (on) {
		/* re-assert mode in case the guest lost it while switched away */
		svga_write(SVGA_REG_WIDTH,          (uint32_t)fb->info.fb_width);
		svga_write(SVGA_REG_HEIGHT,         (uint32_t)fb->info.fb_height);
		svga_write(SVGA_REG_BITS_PER_PIXEL, (uint32_t)fb->info.fb_depth);
		svga_write(SVGA_REG_ENABLE, 1);
	} else {
		svga_write(SVGA_REG_ENABLE, 0);   /* hand the screen to vt text */
	}
}

int fb_svga_have_accel(framebuffer_t* fb) {
	(void)fb;
	return (s_caps & (SVGA_CAP_RECT_FILL | SVGA_CAP_RECT_COPY)) ? 1 : 0;
}

int fb_svga_fill(framebuffer_t* fb, int x, int y, int w, int h, uint32_t color) {
	if (x < 0 || y < 0 || w <= 0 || h <= 0)
		return -1;

	if (s_caps & SVGA_CAP_RECT_FILL) {
		fifo_write(SVGA_CMD_RECT_FILL);
		fifo_write(color);
		fifo_write((uint32_t)x);
		fifo_write((uint32_t)y);
		fifo_write((uint32_t)w);
		fifo_write((uint32_t)h);
		svga_sync();
		return 0;
	}

	/* software fallback: fill VRAM directly, then present */
	{
		int      row, col;
		uint8_t* base = (uint8_t*)fb->vram;
		for (row = 0; row < h; row++) {
			uint32_t* line = (uint32_t*)(base + (size_t)(y + row) * fb->stride);
			for (col = 0; col < w; col++)
				line[x + col] = color;
		}
	}
	return fb_svga_update(fb, x, y, w, h);
}

int fb_svga_copy(framebuffer_t* fb, int sx, int sy, int dx, int dy, int w, int h) {
	if (w <= 0 || h <= 0)
		return -1;

	if (s_caps & SVGA_CAP_RECT_COPY) {
		fifo_write(SVGA_CMD_RECT_COPY);
		fifo_write((uint32_t)sx);
		fifo_write((uint32_t)sy);
		fifo_write((uint32_t)dx);
		fifo_write((uint32_t)dy);
		fifo_write((uint32_t)w);
		fifo_write((uint32_t)h);
		svga_sync();
		return 0;
	}

	/* software fallback: memmove rows within VRAM (overlap-safe), then present */
	{
		int      row;
		uint8_t* base = (uint8_t*)fb->vram;
		size_t   bpp  = (size_t)fb->bytes_pp;
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
	}
	return fb_svga_update(fb, dx, dy, w, h);
}

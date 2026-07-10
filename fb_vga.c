/*
 * fb_vga.c -- libfb backend for a vt(4) VGA console (vtvga0), FreeBSD 10+.
 *
 * Drop-in alternative to fb.c: SAME fb.h API, so ppm.c / server.c / fbshow.c /
 * ppm2fb.c / vtcon.c are unchanged. Build with fb_vga.c INSTEAD of fb.c when
 * the console is driven by vt_vga (no KMS/scfb linear framebuffer present).
 *
 * Why this exists
 * ---------------
 * vtvga0 is NOT a linear framebuffer. The vt(4) VGA backend (vt_vga) keeps the
 * card in VGA mode 0x12 -- 640x480, 16 colours, PLANAR (4 bit-planes) -- and
 * renders the text console as glyphs into those planes. It does not implement
 * FBIOGTYPE / mmap (only vt_fb, the scfb/KMS backend, does). So the fb.c path
 * (FBIOGTYPE -> mmap off the tty -> write 32bpp) cannot work here.
 *
 * The only way to the pixels on vtvga0 from userland is the legacy VGA path:
 *   - map the 0xA0000 video window via /dev/mem  (64 KiB aperture)
 *   - poke the VGA registers via I/O ports, enabled by opening /dev/io (IOPL)
 * This is exactly the /dev/mem + /dev/io situation; see notes at the bottom.
 *
 * Design
 * ------
 * We keep a 32bpp packed BACK BUFFER (fb->back), identical to fb.c, so ppm.c's
 * blit()/cls() (which assume 0x00RRGGBB @ 32bpp) work verbatim. fb_flip() then:
 *   1) reduces each 32bpp pixel to a 4-bit VGA colour index (nearest of the 16
 *      standard EGA/VGA colours) via a precomputed 4096-entry LUT, and
 *   2) writes the frame to the planes, one plane per pass:
 *        Sequencer Map Mask = (1 << plane); each VGA byte packs the plane bit
 *        of 8 consecutive pixels.
 *
 * We do NOT do a full mode-set (vt_vga already put the card in 12h). We only
 * force the write path (Graphics Controller + Sequencer) into a known state:
 * write mode 0, bit mask 0xFF, set/reset off. We do NOT touch the DAC/palette,
 * so on release vt(4)'s own colours are intact.
 *
 * A full repaint every flip is assumed (server.c / fbshow.c do exactly that);
 * that lets us skip latch preloading (bit mask is 0xFF, every pixel written).
 *
 * Single-owner model: like vtcon.c, this backend assumes one framebuffer per
 * process (a display server owns one console), so the VGA-specific state lives
 * in file-static storage instead of bloating struct framebuffer in fb.h.
 *
 * Requirements at run time: root; kern.securelevel <= 0 (writable /dev/mem);
 * kern.vty == vt; and the console must actually be vt_vga (no KMS loaded).
 */

#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>

#include "fb.h"

/* ------------------------------------------------------------------ *
 * VGA mode 0x12 geometry (fixed for vt_vga).                          *
 * ------------------------------------------------------------------ */
#define VGA_W        640
#define VGA_H        480
#define VGA_BYTES_W  (VGA_W / 8)              /* 80 bytes per plane row  */
#define VGA_PLANE_SZ ((size_t)VGA_BYTES_W * VGA_H)  /* 38400 bytes/plane */

#define VGA_MEM_PHYS 0xA0000UL                /* legacy VGA window base  */
#define VGA_MEM_LEN  0x10000UL                /* 64 KiB aperture         */

/* VGA register ports */
#define SEQ_IDX   0x3C4
#define SEQ_DAT   0x3C5
#define GC_IDX    0x3CE
#define GC_DAT    0x3CF

/* ------------------------------------------------------------------ *
 * Port I/O. We provide our own inb/outb so we don't depend on the     *
 * availability/semantics of <machine/cpufunc.h> in userland; opening  *
 * /dev/io raises this process's IOPL so these instructions don't      *
 * fault. x86 only (which is where legacy VGA lives anyway).           *
 * ------------------------------------------------------------------ */
#if defined(__amd64__) || defined(__i386__)
static __inline void vga_outb(uint16_t port, uint8_t val) {
	__asm__ __volatile__("outb %0, %1" :: "a"(val), "Nd"(port));
}
static __inline uint8_t vga_inb(uint16_t port) {
	uint8_t v;
	__asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(port));
	return v;
}
#else
#error "fb_vga.c requires x86 port I/O (vt_vga is a legacy-VGA backend)"
#endif

static __inline void seq_write(uint8_t idx, uint8_t val) {
	vga_outb(SEQ_IDX, idx); vga_outb(SEQ_DAT, val);
}
static __inline void gc_write(uint8_t idx, uint8_t val) {
	vga_outb(GC_IDX, idx); vga_outb(GC_DAT, val);
}

/* ------------------------------------------------------------------ *
 * Standard 16-colour VGA/EGA palette (default DAC contents after 12h).*
 * We map incoming 0x00RRGGBB to the nearest of these; we do not touch  *
 * the DAC, so what we compute is what the console will actually show.  *
 * ------------------------------------------------------------------ */
static const uint8_t vga16[16][3] = {
	{  0,  0,  0}, {  0,  0,170}, {  0,170,  0}, {  0,170,170},
	{170,  0,  0}, {170,  0,170}, {170, 85,  0}, {170,170,170},
	{ 85, 85, 85}, { 85, 85,255}, { 85,255, 85}, { 85,255,255},
	{255, 85, 85}, {255, 85,255}, {255,255, 85}, {255,255,255},
};

/* ------------------------------------------------------------------ *
 * File-static VGA state (single owner, see header note).              *
 * ------------------------------------------------------------------ */
static int            s_mem_fd = -1;
static int            s_io_fd  = -1;
static volatile uint8_t* s_vram = NULL;      /* mmap'd 0xA0000 window   */
static uint8_t*       s_idx   = NULL;        /* VGA_W*VGA_H 4-bit idxs  */
static uint8_t*       s_lut   = NULL;        /* 4096-entry rgb->idx LUT */

/* Build a 12-bit (4:4:4) RGB -> nearest-index lookup table, once. */
static void build_lut(void) {
	int r, g, b;
	for (r = 0; r < 16; r++)
		for (g = 0; g < 16; g++)
			for (b = 0; b < 16; b++) {
				int R = r * 17, G = g * 17, B = b * 17; /* 0..255 */
				int best = 0, bestd = 1 << 30, i;
				for (i = 0; i < 16; i++) {
					int dr = R - vga16[i][0];
					int dg = G - vga16[i][1];
					int db = B - vga16[i][2];
					int d  = dr*dr + dg*dg + db*db;
					if (d < bestd) { bestd = d; best = i; }
				}
				s_lut[(r << 8) | (g << 4) | b] = (uint8_t)best;
			}
}

/* Put the GC/Sequencer write path into a known mode-0 state. The mode
   (12h, memory map @ A0000) is left as vt_vga set it -- we only fix the
   parts that govern how our byte writes land. */
static void vga_write_setup(void) {
	gc_write(0x00, 0x00);   /* Set/Reset          : 0                    */
	gc_write(0x01, 0x00);   /* Enable Set/Reset   : off                  */
	gc_write(0x03, 0x00);   /* Data Rotate/func   : rotate 0, replace    */
	gc_write(0x05, 0x00);   /* Mode               : write mode 0, read 0 */
	gc_write(0x08, 0xFF);   /* Bit Mask           : all 8 bits writable  */
	/* GC6 (Misc: graphics + A0000 map) and the DAC are left untouched.  */
}

framebuffer_t* fb_open(const char* dev, int db) {
	framebuffer_t* fb;

	(void)dev;   /* vt_vga has no per-tty mmap; the window is /dev/mem   */
	(void)db;    /* planar VGA is inherently back-buffered: always on    */

	if ((fb = calloc(1, sizeof(*fb))) == NULL)
		return NULL;
	fb->fd = -1;

	/* Fill geometry by hand (no FBIOGTYPE on vt_vga). We advertise 32bpp
	   so ppm.c's 32bpp blit/cls and ppm2fb's bpp check are satisfied;
	   the *hardware* is 4bpp planar, handled entirely inside fb_flip. */
	fb->info.fb_width  = VGA_W;
	fb->info.fb_height = VGA_H;
	fb->info.fb_depth  = 32;
	fb->info.fb_size   = (int)((size_t)VGA_W * VGA_H * 4);
	fb->bytes_pp       = 4;
	fb->stride         = (size_t)VGA_W * 4;      /* unused on this path */
	fb->back_stride    = (size_t)VGA_W * 4;      /* packed 32bpp rows   */
	fb->map_size       = 0;                      /* we don't mmap 'vram'*/
	fb->vram           = NULL;                   /* planar; see s_vram  */

	/* 32bpp software back buffer -- the surface ppm.c draws into. */
	fb->back = malloc(fb->back_stride * (size_t)VGA_H);
	if (fb->back == NULL)
		goto fail;

	/* rgb->index LUT and 4-bit index scratch buffer for the flip. */
	s_lut = malloc(4096);
	s_idx = malloc((size_t)VGA_W * VGA_H);
	if (s_lut == NULL || s_idx == NULL) { errno = ENOMEM; goto fail; }
	build_lut();

	/* Raise IOPL so our inb/outb work: opening /dev/io does it. */
	if ((s_io_fd = open("/dev/io", O_RDWR)) == -1)
		goto fail;   /* needs root */

	/* Map the 0xA0000 VGA window. offset == physical address on /dev/mem.
	   Needs root and kern.securelevel <= 0 for a writable mapping. */
	if ((s_mem_fd = open("/dev/mem", O_RDWR)) == -1)
		goto fail;
	s_vram = mmap(NULL, VGA_MEM_LEN, PROT_READ | PROT_WRITE,
	              MAP_SHARED, s_mem_fd, (off_t)VGA_MEM_PHYS);
	if (s_vram == MAP_FAILED) {
		s_vram = NULL;
		goto fail;
	}

	vga_write_setup();
	return fb;

fail:
	fb_close(fb);
	return NULL;
}

void fb_close(framebuffer_t* fb) {
	if (s_vram != NULL && s_vram != MAP_FAILED)
		munmap((void*)s_vram, VGA_MEM_LEN);
	s_vram = NULL;
	if (s_mem_fd != -1) { close(s_mem_fd); s_mem_fd = -1; }
	if (s_io_fd  != -1) { close(s_io_fd);  s_io_fd  = -1; }
	free(s_idx); s_idx = NULL;
	free(s_lut); s_lut = NULL;

	if (fb == NULL)
		return;
	free(fb->back);
	free(fb);
}

void* fb_drawbuf(framebuffer_t* fb) {
	return fb->back;                 /* always the 32bpp back buffer */
}

size_t fb_pitch(framebuffer_t* fb) {
	return fb->back_stride;          /* packed width*4               */
}

int fb_flip(framebuffer_t* fb) {
	const uint32_t* src = (const uint32_t*)fb->back;
	size_t          n   = (size_t)VGA_W * VGA_H;
	size_t          i;
	int             p, y, c;

	if (fb->back == NULL || s_vram == NULL || s_lut == NULL || s_idx == NULL)
		return -1;

	/* 1) reduce 32bpp back buffer -> 4-bit indices (4:4:4 quantised LUT). */
	for (i = 0; i < n; i++) {
		uint32_t px = src[i];                     /* 0x00RRGGBB          */
		unsigned r = (px >> 16) & 0xFF;
		unsigned g = (px >>  8) & 0xFF;
		unsigned b =  px        & 0xFF;
		s_idx[i] = s_lut[((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4)];
	}

	/* 2) planar blit: one plane per pass, Map Mask isolates the plane.
	   Every byte is fully written (bit mask 0xFF), so no latch preload. */
	for (p = 0; p < 4; p++) {
		seq_write(0x02, (uint8_t)(1 << p));       /* Map Mask = plane p  */
		for (y = 0; y < VGA_H; y++) {
			const uint8_t*  irow = s_idx + (size_t)y * VGA_W;
			volatile uint8_t* vrow = s_vram + (size_t)y * VGA_BYTES_W;
			for (c = 0; c < VGA_BYTES_W; c++) {
				const uint8_t* g = irow + (size_t)c * 8;
				vrow[c] = (uint8_t)(
					(((g[0] >> p) & 1) << 7) |
					(((g[1] >> p) & 1) << 6) |
					(((g[2] >> p) & 1) << 5) |
					(((g[3] >> p) & 1) << 4) |
					(((g[4] >> p) & 1) << 3) |
					(((g[5] >> p) & 1) << 2) |
					(((g[6] >> p) & 1) << 1) |
					(((g[7] >> p) & 1)));
			}
		}
	}
	return 0;
}

int fb_resize(framebuffer_t* fb, int width, int height) {
	(void)fb; (void)width; (void)height;
	/* fixed at mode 0x12 (640x480x16), see the file header -- no other mode
	   exists for this backend to switch to. */
	errno = ENOTSUP;
	return -1;
}

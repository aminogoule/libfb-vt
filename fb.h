#ifndef FB_H
#define FB_H

/*
 * libfb-vt -- console framebuffer graphics for FreeBSD 10+ (vt(4) / newcons)
 *
 * Unlike the old sc(4) + VESA approach, vt(4) exposes the framebuffer through
 * the tty device itself:
 *   - geometry comes from ioctl(fd, FBIOGTYPE, struct fbtype)
 *   - the framebuffer is mmap'd straight off that same fd at offset 0
 * No /dev/mem, no kvm/nlist symbol reading, no -lkvm.
 *
 * The video MODE is owned by the kernel (kern.vt.fb.default_mode, or whatever
 * the KMS driver set up). There is no userland VESA mode-setting under vt(4),
 * so this library does NOT switch modes -- it draws into whatever resolution
 * the console is already in.
 *
 * There is also no hardware page-flip under vt(4): "double buffering" here is a
 * software back buffer that fb_flip() blits to the screen.
 */

#include <sys/fbio.h>   /* struct fbtype, FBIOGTYPE */
#include <stddef.h>
#include <stdint.h>

struct framebuffer {
	int           fd;          /* console / tty device fd                */
	struct fbtype info;        /* geometry reported by FBIOGTYPE         */
	int           bytes_pp;    /* bytes per pixel                        */
	size_t        stride;      /* HARDWARE bytes per scanline (may be    */
	                           /* padded, derived from fb_size/fb_height)*/
	size_t        back_stride; /* packed bytes per scanline of back buf  */
	size_t        map_size;    /* size of the mmap'd region              */
	void*         vram;        /* mmap'd framebuffer (the live screen)   */
	void*         back;        /* software back buffer (NULL if single)  */
};
typedef struct framebuffer framebuffer_t;

/*
 * Open the vt(4) framebuffer.
 *   dev  -- console device to use, or NULL for "/dev/ttyv0".
 *   db   -- non-zero to allocate a software back buffer (double buffering);
 *           falls back to single buffering if the allocation fails.
 * Returns NULL on error (check errno). Call fb_close() to release everything.
 * Typically needs to run as root (to open/mmap the console).
 */
framebuffer_t* fb_open(const char* dev, int db);

/* Resets state and frees the struct. Safe on a partially-initialised fb. */
void fb_close(framebuffer_t* fb);

/* The buffer you should draw into: the back buffer if double buffered,
   otherwise the live framebuffer. */
void* fb_drawbuf(framebuffer_t* fb);

/* Bytes per scanline of the buffer returned by fb_drawbuf(). Use this as the
   row pitch when you compute pixel addresses -- do NOT assume width*bpp. */
size_t fb_pitch(framebuffer_t* fb);

/* Copy the back buffer to the screen. No-op when single buffered. */
int fb_flip(framebuffer_t* fb);

#endif /* FB_H */

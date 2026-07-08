/*
 * libfb-vt -- console framebuffer graphics for FreeBSD 10+ (vt(4))
 *
 * See fb.h for the design rationale. In short: open the tty, FBIOGTYPE for
 * geometry, mmap the fd. That's the whole "hard part" -- vt(4) does the rest.
 */

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "fb.h"

#define DEFAULT_DEV "/dev/ttyv0"

framebuffer_t* fb_open(const char* dev, int db) {
	framebuffer_t* fb;

	if ((fb = calloc(1, sizeof(*fb))) == NULL)
		return NULL;
	fb->fd = -1;	/* calloc gives 0, which is a valid fd (stdin!) -- avoid
			   accidentally close()ing it in fb_close on error */

	if (dev == NULL)
		dev = DEFAULT_DEV;

	/* vt(4) forwards framebuffer ioctl/mmap through the tty device. */
	if ((fb->fd = open(dev, O_RDWR)) == -1)
		goto fail;

	/* width, height, depth and total framebuffer size in bytes */
	if (ioctl(fb->fd, FBIOGTYPE, &fb->info) == -1)
		goto fail;

	fb->bytes_pp = (fb->info.fb_depth + 7) / 8;
	if (fb->bytes_pp <= 0 || fb->info.fb_height <= 0 ||
	    fb->info.fb_width <= 0 || fb->info.fb_size <= 0) {
		errno = ENXIO;
		goto fail;
	}

	/*
	 * The hardware scanline can be padded (stride > width*bpp), so derive
	 * the real stride from the reported total size rather than assuming.
	 */
	fb->stride      = (size_t)fb->info.fb_size / (size_t)fb->info.fb_height;
	fb->back_stride = (size_t)fb->info.fb_width * (size_t)fb->bytes_pp;
	fb->map_size    = (size_t)fb->info.fb_size;

	/* map the framebuffer straight off the fd, offset 0 */
	fb->vram = mmap(NULL, fb->map_size, PROT_READ | PROT_WRITE,
	                MAP_SHARED, fb->fd, 0);
	if (fb->vram == MAP_FAILED) {
		fb->vram = NULL;
		goto fail;
	}

	/* optional software back buffer (packed, no padding) */
	if (db)
		fb->back = malloc(fb->back_stride * (size_t)fb->info.fb_height);
		/* if this fails we simply stay single-buffered (back == NULL) */

	return fb;

fail:
	fb_close(fb);
	return NULL;
}

void fb_close(framebuffer_t* fb) {
	if (fb == NULL)
		return;
	free(fb->back);
	if (fb->vram != NULL && fb->vram != MAP_FAILED)
		munmap(fb->vram, fb->map_size);
	if (fb->fd != -1)
		close(fb->fd);
	free(fb);
}

void* fb_drawbuf(framebuffer_t* fb) {
	return fb->back ? fb->back : fb->vram;
}

size_t fb_pitch(framebuffer_t* fb) {
	return fb->back ? fb->back_stride : fb->stride;
}

int fb_flip(framebuffer_t* fb) {
	size_t          y, rows;
	const uint8_t*  src;
	uint8_t*        dst;

	if (fb->back == NULL)	/* single buffered: nothing to copy */
		return 0;

	src  = (const uint8_t*)fb->back;
	dst  = (uint8_t*)fb->vram;
	rows = (size_t)fb->info.fb_height;

	/* copy row by row so a padded hardware stride is handled correctly */
	for (y = 0; y < rows; y++)
		memcpy(dst + y * fb->stride,
		       src + y * fb->back_stride,
		       fb->back_stride);
	return 0;
}

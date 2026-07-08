/*
 * ppm.c -- PPM loading and simple 32-bit blitting for libfb-vt.
 *
 * Pixel byte order is 0x00RRGGBB as a little-endian uint32 (B,G,R,X in memory),
 * matching the common x86 scfb / KMS framebuffers. If colours come out swapped,
 * change the pack order in read_ppm().
 */

#include "ppm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Skip whitespace and #-comments; return the next significant byte or EOF. */
static int ppm_skip_ws(FILE* f) {
	int c;
	for (;;) {
		c = fgetc(f);
		if (c == '#') {			/* comment: discard to end of line */
			while ((c = fgetc(f)) != EOF && c != '\n')
				;
			continue;
		}
		if (c == ' ' || c == '\t' || c == '\n' ||
		    c == '\r' || c == '\f' || c == '\v')
			continue;
		return c;
	}
}

/* Read one unsigned decimal integer, skipping leading whitespace/comments.
   Returns 0 on success, -1 on failure. */
static int ppm_read_uint(FILE* f, int* out) {
	int c = ppm_skip_ws(f);
	int val = 0, got = 0;

	if (c == EOF)
		return -1;
	while (c >= '0' && c <= '9') {
		val = val * 10 + (c - '0');
		got = 1;
		c = fgetc(f);
	}
	if (!got)
		return -1;
	*out = val;
	return 0;
}

img32_t* read_ppm(const char* filename) {
	FILE*    ppm;
	img32_t* img;
	int      w, h, maxval, i;
	int      c1, c2, ascii;

	if ((ppm = fopen(filename, "rb")) == NULL)
		return NULL;

	c1 = fgetc(ppm);
	c2 = fgetc(ppm);
	if (c1 != 'P' || (c2 != '6' && c2 != '3')) {	/* not a PPM we grok */
		fclose(ppm);
		return NULL;
	}
	ascii = (c2 == '3');

	if (ppm_read_uint(ppm, &w) || ppm_read_uint(ppm, &h) ||
	    ppm_read_uint(ppm, &maxval) ||
	    w <= 0 || h <= 0 || maxval <= 0 || maxval > 65535) {
		fclose(ppm);
		return NULL;
	}

	img = malloc(sizeof(*img) + ((size_t)w * h - 1) * sizeof(img->pixel));
	if (img == NULL) {
		fclose(ppm);
		return NULL;
	}

	img->w = w;
	img->h = h;
	for (i = 0; i < w * h; i++) {
		int ch[3], k;
		for (k = 0; k < 3; k++) {
			int v;
			if (ascii) {
				if (ppm_read_uint(ppm, &v)) {
					free(img); fclose(ppm); return NULL;
				}
			} else {
				int hi = fgetc(ppm);
				if (hi == EOF) {
					free(img); fclose(ppm); return NULL;
				}
				if (maxval > 255) {	/* 16-bit, big-endian */
					int lo = fgetc(ppm);
					if (lo == EOF) {
						free(img); fclose(ppm); return NULL;
					}
					v = (hi << 8) | lo;
				} else {
					v = hi;
				}
			}
			ch[k] = (maxval == 255) ? v : v * 255 / maxval;
		}
		(&img->pixel)[i] = ((uint32_t)ch[0] << 16) |
		                   ((uint32_t)ch[1] << 8)  |
		                    (uint32_t)ch[2];
	}
	fclose(ppm);
	return img;
}

void blit(framebuffer_t* fb, img32_t* img, int x, int y) {
	int    img_xs = 0, img_ys = 0, img_xe = img->w, img_ye = img->h;
	int    fb_w   = fb->info.fb_width, fb_h = fb->info.fb_height;
	int    fb_xs  = x, fb_ys = y, fb_xe = x + img->w, fb_ye = y + img->h;
	int    row;
	size_t   pitch = fb_pitch(fb);
	uint8_t* dst   = (uint8_t*)fb_drawbuf(fb);

	/* crop to the viewable area */
	if (fb_xs < 0) { img_xs -= fb_xs; fb_xs = 0; if (img_xs >= img_xe) return; }
	if (fb_ys < 0) { img_ys -= fb_ys; fb_ys = 0; if (img_ys >= img_ye) return; }
	if (fb_xe > fb_w) { img_xe -= fb_xe - fb_w; fb_xe = fb_w; if (img_xe <= img_xs) return; }
	if (fb_ye > fb_h) { img_ye -= fb_ye - fb_h; fb_ye = fb_h; if (img_ye <= img_ys) return; }

	for (row = 0; row < img_ye - img_ys; row++) {
		memcpy(
			dst + (size_t)(fb_ys + row) * pitch + (size_t)fb_xs * 4,
			&(&img->pixel)[(size_t)(img_ys + row) * img->w + img_xs],
			(size_t)(img_xe - img_xs) * 4
		);
	}
}

void cls(framebuffer_t* fb, uint32_t color) {
	int      x, y;
	int      w = fb->info.fb_width, h = fb->info.fb_height;
	size_t   pitch = fb_pitch(fb);
	uint8_t* base  = (uint8_t*)fb_drawbuf(fb);

	for (y = 0; y < h; y++) {
		uint32_t* line = (uint32_t*)(base + (size_t)y * pitch);
		for (x = 0; x < w; x++)
			line[x] = color;
	}
}

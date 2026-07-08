/*
 * ppm2fb -- display a binary/ASCII PPM using the vt(4) framebuffer (one-shot).
 *
 * Simple test of the fb.c library: clear to blue, wait, load and centre the
 * image on red, wait, exit. See fbshow.c for a persistent, VT-grabbing viewer.
 *
 * Build:   make
 * Run:     ./ppm2fb lena.ppm        (as root, on a vt(4) console)
 */

#include "fb.h"
#include "ppm.h"

#include <stdio.h>
#include <stdlib.h>
#include <sysexits.h>

int main(int argc, char* argv[]) {
	framebuffer_t* fb;
	img32_t*       img;

	if (argc == 1) {
		fprintf(stderr, "Usage: %s lena.ppm\n", argv[0]);
		return EX_USAGE;
	}

	fprintf(stderr, "Starting up...\n");
	if ((fb = fb_open(NULL, 1)) == NULL) {   /* default console, double buffered */
		perror("fb_open");
		return EX_UNAVAILABLE;
	}

	if (fb->bytes_pp != 4)
		fprintf(stderr, "warning: framebuffer is %d bpp; this demo assumes 32\n",
		        fb->info.fb_depth);

	cls(fb, 0x00007F);          /* blue */
	fb_flip(fb);
	getc(stdin);                /* pause */

	if ((img = read_ppm(argv[1])) == NULL) {
		fprintf(stderr, "failed to read %s\n", argv[1]);
		fb_close(fb);
		return EX_DATAERR;
	}

	cls(fb, 0x7F0000);          /* red */
	blit(fb, img,
	     fb->info.fb_width  / 2 - img->w / 2,
	     fb->info.fb_height / 2 - img->h / 2);
	fb_flip(fb);
	free(img);

	getc(stdin);                /* pause */

	fprintf(stderr, "Shutting down...\n");
	fb_close(fb);
	return EX_OK;
}

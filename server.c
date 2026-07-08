/*
 * server -- a seed of a framebuffer display server for FreeBSD/vt(4).
 *
 * It takes *exclusive* ownership of a dedicated VT (native vt(4) protocol, no
 * DRM/KMS/linuxkpi), puts the console into graphics mode, and runs a render
 * loop over LibFB. Cooperates with VT switching: while switched away it stops
 * drawing (so it never touches other terminals) and resumes on return.
 *
 * Layers:  fb.c (framebuffer)  +  vtcon.c (VT/console ownership)  +  ppm.c
 *
 * Usage: server image.ppm [vtnum]      (vtnum omitted/0 => first free VT)
 * Quit:  'q' / ESC, or SIGINT/SIGTERM. VT switching (Alt+Fn, Ctrl+Alt+Fn,
 * Alt+arrows) is refused while the server runs -- see vtcon.c.
 * Run as root on a vt(4) console (sysctl kern.vty == vt).
 */

#include "fb.h"
#include "ppm.h"
#include "vtcon.h"

#include <time.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sysexits.h>

int main(int argc, char* argv[]) {
	vtcon_t        con;
	framebuffer_t* fb  = NULL;
	img32_t*       img = NULL;
	char           dev[32];
	int            vtnum = 0;
	int            rc = EX_OK;

	if (argc < 2) {
		fprintf(stderr, "Usage: %s image.ppm [vtnum]\n", argv[0]);
		return EX_USAGE;
	}
	if (argc >= 3)
		vtnum = atoi(argv[2]);

	if ((img = read_ppm(argv[1])) == NULL) {
		fprintf(stderr, "failed to read %s\n", argv[1]);
		return EX_DATAERR;
	}

	/* take exclusive ownership of a VT */
	if (vtcon_acquire(&con, vtnum) != 0) {
		fprintf(stderr, "vtcon_acquire: %s\n", strerror(errno));
		free(img);
		return EX_OSERR;
	}

	/* map the framebuffer on the VT we now own (its own fd for mmap) */
	snprintf(dev, sizeof(dev), "/dev/ttyv%d", con.vtnum - 1);
	if ((fb = fb_open(dev, 1)) == NULL) {
		fprintf(stderr, "fb_open(%s): %s\n", dev, strerror(errno));
		rc = EX_UNAVAILABLE;
		goto out;
	}

	fprintf(stderr,
	        "server: own vt %d, %dx%d %dbpp. 'q'/ESC to quit; "
	        "VT switching is blocked while running.\n",
	        con.vtnum, fb->info.fb_width, fb->info.fb_height, fb->info.fb_depth);

	/* the render loop */
	while (!vtcon_quit_requested()) {
		struct timespec ts;

		vtcon_pump(&con);              /* service VT-switch handshake */

		/* quit on 'q'/'Q'/Escape; arrow keys & VT-switch combos are ignored */
		if (vtcon_quit_key(&con))
			goto out;

		if (con.active) {
			cls(fb, 0x101018);         /* server background: dark slate */
			blit(fb, img,
			     fb->info.fb_width  / 2 - img->w / 2,
			     fb->info.fb_height / 2 - img->h / 2);
			fb_flip(fb);
		}

		ts.tv_sec = 0; ts.tv_nsec = 16L * 1000 * 1000;   /* ~60 fps */
		nanosleep(&ts, NULL);
	}

out:
	if (fb)
		fb_close(fb);
	vtcon_release(&con);
	free(img);
	return rc;
}

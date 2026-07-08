/*
 * fbshow -- grab a virtual terminal under vt(4), switch to it, and hold a PPM
 * image on screen, redrawing continuously so the console can't leave it
 * overwritten. Companion to ppm2fb.
 *
 * Usage: fbshow image.ppm [vtnum]
 *   vtnum : 1-based VT to grab (7 == ttyv6). Omit or 0 == auto-pick the first
 *           unused VT via VT_OPENQRY (recommended: no getty steals keystrokes).
 *
 * While showing: press 'q' or ESC to quit. You may also switch away with
 * Ctrl+Alt+F1..; the image is drawn only while its VT is the active one, so it
 * never scribbles over other terminals. SIGINT/SIGTERM/SIGHUP also exit cleanly.
 *
 * On exit the VT is returned to text mode and the original VT restored.
 * Run as root on a vt(4) console (sysctl kern.vty == vt).
 *
 * Note: vt(4)'s KDSETMODE/KD_GRAPHICS is incomplete (it only sets a flag and
 * does not fully stop the text layer), so the continuous redraw below is what
 * actually keeps the image up; KD_GRAPHICS is best-effort on top of that.
 */

#include "fb.h"
#include "ppm.h"

#include <sys/ioctl.h>
#include <sys/consio.h>   /* VT_ACTIVATE, VT_WAITACTIVE, VT_GETACTIVE, VT_OPENQRY */
#include <sys/kbio.h>     /* KDSETMODE, KD_GRAPHICS, KD_TEXT */
#include <termios.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sysexits.h>

static volatile sig_atomic_t g_quit = 0;
static void on_signal(int sig) { (void)sig; g_quit = 1; }

int main(int argc, char* argv[]) {
	const char*    file;
	int            vtnum = 0;             /* 0 => auto-pick */
	char           devpath[32];
	int            ctl = -1, orig_vt = -1, cur = -1;
	framebuffer_t* fb  = NULL;
	img32_t*       img = NULL;
	struct termios told, traw;
	int            have_termios = 0, graphics = 0;
	int            rc = EX_OK;

	if (argc < 2) {
		fprintf(stderr, "Usage: %s image.ppm [vtnum]\n", argv[0]);
		return EX_USAGE;
	}
	file = argv[1];
	if (argc >= 3)
		vtnum = atoi(argv[2]);

	if ((img = read_ppm(file)) == NULL) {
		fprintf(stderr, "failed to read %s\n", file);
		return EX_DATAERR;
	}

	/* control fd on the current console, for VT queries / switching */
	if ((ctl = open("/dev/ttyv0", O_RDWR)) == -1) {
		perror("open /dev/ttyv0");
		rc = EX_OSFILE; goto out;
	}
	if (ioctl(ctl, VT_GETACTIVE, &orig_vt) == -1) {   /* pointer arg */
		perror("VT_GETACTIVE");
		rc = EX_OSERR; goto out;
	}

	/* pick a VT (auto = first unused; must query BEFORE we open it) */
	if (vtnum <= 0) {
		if (ioctl(ctl, VT_OPENQRY, &vtnum) == -1 || vtnum <= 0) {  /* ptr */
			perror("VT_OPENQRY");
			rc = EX_OSERR; goto out;
		}
	}
	snprintf(devpath, sizeof(devpath), "/dev/ttyv%d", vtnum - 1);

	/* open + mmap the framebuffer via the target VT, double buffered */
	if ((fb = fb_open(devpath, 1)) == NULL) {
		fprintf(stderr, "fb_open(%s): %s\n", devpath, strerror(errno));
		rc = EX_UNAVAILABLE; goto out;
	}

	/* switch the display to our VT (VT_ACTIVATE takes the number BY VALUE) */
	if (ioctl(fb->fd, VT_ACTIVATE, vtnum) == -1) {
		perror("VT_ACTIVATE");
		rc = EX_OSERR; goto out;
	}
	ioctl(fb->fd, VT_WAITACTIVE, vtnum);   /* by value; best effort */

	signal(SIGINT,  on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGHUP,  on_signal);

	/* raw, non-blocking keyboard on the VT so we can catch 'q' / ESC */
	if (tcgetattr(fb->fd, &told) == 0) {
		traw = told;
		cfmakeraw(&traw);
		if (tcsetattr(fb->fd, TCSANOW, &traw) == 0)
			have_termios = 1;
	}
	fcntl(fb->fd, F_SETFL, fcntl(fb->fd, F_GETFL, 0) | O_NONBLOCK);

	/* best-effort: ask the text console to stop drawing over us */
	if (ioctl(fb->fd, KDSETMODE, KD_GRAPHICS) == 0)
		graphics = 1;

	/* hide the text cursor (vt(4)'s KD_GRAPHICS may still blink it) */
	(void)write(fb->fd, "\033[?25l", 6);

	fprintf(stderr, "Showing on ttyv%d (vt %d). Press 'q' or ESC to quit.\n",
	        vtnum - 1, vtnum);

	while (!g_quit) {
		struct timespec ts;

		/*
		 * Drain keystrokes; quit on 'q' / 'Q' / a standalone ESC. Arrow keys
		 * and other special keys arrive as escape sequences beginning with ESC
		 * (e.g. Up = ESC '[' 'A'), so a plain "== 27 -> quit" test would exit
		 * the moment any arrow is pressed. Swallow ESC '[' / ESC 'O' sequences
		 * instead; a lone ESC (no continuation) still quits. See vtcon_quit_key.
		 */
		{
			static int    esc_pending = 0, in_seq = 0;
			unsigned char ch;
			int           got_any = 0;

			while (read(fb->fd, &ch, 1) == 1) {
				got_any = 1;
				if (in_seq) {
					if (ch >= 0x40 && ch <= 0x7E) in_seq = 0;
					continue;
				}
				if (esc_pending) {
					esc_pending = 0;
					if (ch == '[' || ch == 'O') { in_seq = 1; continue; }
					g_quit = 1;         /* the ESC was a real keypress */
				}
				if (ch == 0x1B) { esc_pending = 1; continue; }
				if (ch == 'q' || ch == 'Q') g_quit = 1;
			}
			if (esc_pending && !got_any) { esc_pending = 0; g_quit = 1; }
		}

		/* the framebuffer is shared by all VTs, so paint only while ours
		   is the visible one -- otherwise we'd scribble over other TTYs */
		if (ioctl(fb->fd, VT_GETACTIVE, &cur) == 0 && cur == vtnum) {
			cls(fb, 0x000000);
			blit(fb, img,
			     fb->info.fb_width  / 2 - img->w / 2,
			     fb->info.fb_height / 2 - img->h / 2);
			fb_flip(fb);
		}

		ts.tv_sec = 0; ts.tv_nsec = 50L * 1000 * 1000;   /* ~20 fps */
		nanosleep(&ts, NULL);
	}

out:
	if (fb) {
		(void)write(fb->fd, "\033[?25h", 6);   /* restore the cursor */
		if (graphics)     ioctl(fb->fd, KDSETMODE, KD_TEXT);
		if (have_termios) tcsetattr(fb->fd, TCSANOW, &told);
	}
	/* return to where we started, but only if we're still the active VT */
	if (ctl != -1 && orig_vt > 0) {
		if (ioctl(ctl, VT_GETACTIVE, &cur) == 0 && cur == vtnum) {
			ioctl(ctl, VT_ACTIVATE,   orig_vt);
			ioctl(ctl, VT_WAITACTIVE, orig_vt);
		}
	}
	if (fb)  fb_close(fb);
	if (ctl != -1) close(ctl);
	free(img);
	return rc;
}

/*
 * vtcon.c -- native FreeBSD VT / console ownership (see vtcon.h).
 *
 * The VT_PROCESS handshake in a nutshell:
 *   - We call VT_SETMODE(VT_PROCESS) with relsig/acqsig.
 *   - When the user tries to switch away, the kernel sends `relsig` and waits
 *     for us to answer with VT_RELDISP(VT_TRUE) (allow) or VT_FALSE (deny).
 *   - When we are switched back to, the kernel sends `acqsig`; we answer with
 *     VT_RELDISP(VT_ACKACQ) and re-assert graphics mode.
 * If our process dies, the kernel force-resets the VT to KD_TEXT + VT_AUTO, so
 * a crash can't permanently trap the console.
 *
 * By default (lock_switch = 1) we DENY switches: on `relsig` we answer
 * VT_RELDISP(VT_FALSE), so the user cannot leave our VT while the server runs.
 * This is the safe way to get a hard monopoly: because it rides on VT_PROCESS,
 * process death still triggers the kernel's KD_TEXT + VT_AUTO reset, so a crash
 * can't trap every console. VT_LOCKSWITCH would give the same monopoly but with
 * no such safety net (a hang locks the user out of all VTs), so we avoid it.
 * vtcon_set_switch_lock(c, 0) restores cooperative switching (answer VT_TRUE).
 */

#include "vtcon.h"

#include <sys/ioctl.h>
#include <sys/kbio.h>     /* KDSETMODE, KD_GRAPHICS, KD_TEXT */
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>

#define RELSIG SIGUSR1    /* kernel -> us: please release the VT */
#define ACQSIG SIGUSR2    /* kernel -> us: you have the VT again  */

/* A display server owns exactly one console, so a single set of flags set by
   the async signal handlers is sufficient. */
static volatile sig_atomic_t s_relreq = 0;
static volatile sig_atomic_t s_acqreq = 0;
static volatile sig_atomic_t s_quit   = 0;

static void on_rel(int sig)  { (void)sig; s_relreq = 1; }
static void on_acq(int sig)  { (void)sig; s_acqreq = 1; }
static void on_quit(int sig) { (void)sig; s_quit   = 1; }

int vtcon_quit_requested(void) { return s_quit; }

void vtcon_set_switch_lock(vtcon_t* c, int on) { c->lock_switch = on ? 1 : 0; }

int vtcon_acquire(vtcon_t* c, int vtnum) {
	struct vt_mode vtm;
	char           dev[32];

	memset(c, 0, sizeof(*c));
	c->fd = c->ctl_fd = -1;
	c->lock_switch = 1;    /* default: refuse VT switches while we own it */

	if ((c->ctl_fd = open("/dev/ttyv0", O_RDWR)) == -1)
		return -1;
	if (ioctl(c->ctl_fd, VT_GETACTIVE, &c->orig_vtnum) == -1)   /* ptr arg */
		goto fail;

	/* pick a VT -- query for a free one BEFORE we open (and thus mark) it */
	if (vtnum <= 0) {
		if (ioctl(c->ctl_fd, VT_OPENQRY, &vtnum) == -1 || vtnum <= 0)
			goto fail;
	}
	c->vtnum = vtnum;
	snprintf(dev, sizeof(dev), "/dev/ttyv%d", vtnum - 1);
	if ((c->fd = open(dev, O_RDWR)) == -1)
		goto fail;

	/* switch the display to our VT (VT_ACTIVATE takes the number BY VALUE) */
	if (ioctl(c->fd, VT_ACTIVATE, vtnum) == -1)
		goto fail;
	ioctl(c->fd, VT_WAITACTIVE, vtnum);

	/* install handlers BEFORE VT_SETMODE so no rel/acq signal is missed */
	signal(RELSIG,  on_rel);
	signal(ACQSIG,  on_acq);
	signal(SIGINT,  on_quit);
	signal(SIGTERM, on_quit);
	signal(SIGHUP,  on_quit);

	/* take cooperative exclusive ownership of the VT */
	if (ioctl(c->fd, VT_GETMODE, &c->saved_vtmode) == 0)
		c->have_saved_vtmode = 1;
	memset(&vtm, 0, sizeof(vtm));
	vtm.mode   = VT_PROCESS;
	vtm.waitv  = 0;
	vtm.relsig = RELSIG;
	vtm.acqsig = ACQSIG;
	vtm.frsig  = RELSIG;             /* must be non-zero */
	if (ioctl(c->fd, VT_SETMODE, &vtm) == -1)
		goto fail;

	/* raw, non-blocking keyboard for our own input */
	if (tcgetattr(c->fd, &c->saved_termios) == 0) {
		struct termios raw = c->saved_termios;
		cfmakeraw(&raw);
		if (tcsetattr(c->fd, TCSANOW, &raw) == 0)
			c->have_saved_termios = 1;
	}
	fcntl(c->fd, F_SETFL, fcntl(c->fd, F_GETFL, 0) | O_NONBLOCK);

	/* graphics mode: stop the text layer, hide the cursor */
	if (ioctl(c->fd, KDSETMODE, KD_GRAPHICS) == 0)
		c->graphics = 1;
	(void)write(c->fd, "\033[?25l", 6);

	c->active = 1;
	return 0;

fail:
	vtcon_release(c);
	return -1;
}

void vtcon_pump(vtcon_t* c) {
	if (s_relreq) {
		s_relreq = 0;
		if (c->lock_switch) {
			/* refuse: keep the VT and stay active. The kernel aborts
			   the switch; the user remains on our console until quit. */
			ioctl(c->fd, VT_RELDISP, VT_FALSE);   /* by value */
		} else {
			c->active = 0;
			/* allow the switch; the incoming VT sets up its own mode */
			ioctl(c->fd, VT_RELDISP, VT_TRUE);    /* by value */
		}
	}
	if (s_acqreq) {
		s_acqreq = 0;
		ioctl(c->fd, VT_RELDISP, VT_ACKACQ);      /* by value */
		if (ioctl(c->fd, KDSETMODE, KD_GRAPHICS) == 0)
			c->graphics = 1;
		(void)write(c->fd, "\033[?25l", 6);
		c->active = 1;
	}
}

int vtcon_getkey(vtcon_t* c) {
	unsigned char ch;
	if (c->fd < 0)
		return -1;
	if (read(c->fd, &ch, 1) == 1)
		return (int)ch;
	return -1;
}

/*
 * Interpret keyboard input as a "quit or not" decision, tolerant of escape
 * sequences. The keyboard is in K_XLATE, so arrow keys, Home/End, function
 * keys, etc. arrive as multi-byte sequences that all begin with ESC (0x1B):
 *
 *     Up = ESC '[' 'A'      F5 = ESC '[' '1' '5' '~'      etc.
 *
 * A naive "byte == 27 -> quit" test therefore quits the moment any arrow key
 * (or an Alt+arrow VT-switch attempt) is pressed. We instead run a tiny state
 * machine: on ESC we wait to see whether a '[' or 'O' follows (=> it's a
 * sequence: swallow it up to its final byte) or not (=> a real Escape press).
 *
 * A lone ESC is only reported as quit once no continuation has arrived, which
 * may take one extra frame if the ESC and its '[' are split across reads. That
 * one-frame latency is imperceptible and guarantees an arrow key is never
 * misread as Escape. State is file-static: a display server owns one console.
 */
int vtcon_quit_key(vtcon_t* c) {
	static int esc_pending = 0;   /* saw a bare ESC, awaiting continuation  */
	static int in_seq      = 0;   /* inside an ESC '[' / 'O' sequence body  */
	int ch;
	int quit    = 0;
	int got_any = 0;

	while ((ch = vtcon_getkey(c)) != -1) {
		got_any = 1;

		if (in_seq) {                    /* CSI/SS3 body: ends at 0x40..0x7E */
			if (ch >= 0x40 && ch <= 0x7E)
				in_seq = 0;
			continue;
		}
		if (esc_pending) {
			esc_pending = 0;
			if (ch == '[' || ch == 'O') {   /* it was a sequence, not Esc   */
				in_seq = 1;
				continue;
			}
			quit = 1;                        /* the ESC was a real keypress  */
			/* fall through to also classify this byte */
		}
		if (ch == 0x1B) { esc_pending = 1; continue; }
		if (ch == 'q' || ch == 'Q') quit = 1;
		/* any other byte is ignored */
	}

	/* A bare ESC with nothing following it (this drain produced no bytes yet
	   a previous one left ESC pending) is a genuine Escape press. */
	if (esc_pending && !got_any) {
		esc_pending = 0;
		quit = 1;
	}
	return quit;
}

void vtcon_release(vtcon_t* c) {
	if (c == NULL)
		return;

	if (c->fd != -1) {
		(void)write(c->fd, "\033[?25h", 6);        /* restore cursor */
		if (c->graphics)
			ioctl(c->fd, KDSETMODE, KD_TEXT);
		if (c->have_saved_vtmode) {
			ioctl(c->fd, VT_SETMODE, &c->saved_vtmode);
		} else {
			struct vt_mode vtm;
			memset(&vtm, 0, sizeof(vtm));
			vtm.mode = VT_AUTO;
			ioctl(c->fd, VT_SETMODE, &vtm);
		}
		if (c->have_saved_termios)
			tcsetattr(c->fd, TCSANOW, &c->saved_termios);
		c->graphics = 0;
	}

	/* return to the original VT, but only if we're still the visible one */
	if (c->ctl_fd != -1 && c->orig_vtnum > 0) {
		int cur = -1;
		if (ioctl(c->ctl_fd, VT_GETACTIVE, &cur) == 0 && cur == c->vtnum) {
			ioctl(c->ctl_fd, VT_ACTIVATE,   c->orig_vtnum);
			ioctl(c->ctl_fd, VT_WAITACTIVE, c->orig_vtnum);
		}
	}

	if (c->fd != -1)     { close(c->fd);     c->fd = -1; }
	if (c->ctl_fd != -1) { close(c->ctl_fd); c->ctl_fd = -1; }
	c->active = 0;
}

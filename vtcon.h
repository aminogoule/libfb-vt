#ifndef VTCON_H
#define VTCON_H

/*
 * vtcon -- native FreeBSD VT / console ownership for a framebuffer display
 * server. Pure vt(4) protocol via <sys/consio.h> + <sys/kbio.h>:
 *   - VT_OPENQRY / VT_ACTIVATE : grab and switch to a dedicated VT
 *   - KDSETMODE KD_GRAPHICS    : put the console into graphics mode
 *   - VT_SETMODE VT_PROCESS    : cooperative *exclusive* ownership -- the
 *                                kernel asks us (via signals) before switching
 *                                away, and tells us when we get the VT back
 *
 * No DRM, no KMS, no linuxkpi. This is the seed of a display server: it owns
 * the active VT exclusively.
 *
 * By default it REFUSES VT switches while active (lock_switch = 1): the kernel
 * asks permission to switch away and we answer VT_RELDISP(VT_FALSE), so the
 * user stays on our VT until we quit. This is deliberately done via VT_PROCESS
 * rather than VT_LOCKSWITCH: if our process dies, the kernel force-resets the
 * VT to KD_TEXT + VT_AUTO, so a crash can never permanently trap every console
 * (VT_LOCKSWITCH has no such safety net). Call vtcon_set_switch_lock(c, 0) to
 * restore the cooperative model (allow switches, c->active tracks visibility).
 */

#include <sys/consio.h>   /* struct vt_mode */
#include <termios.h>

typedef struct {
	int  fd;                 /* fd of the owned VT device                 */
	int  ctl_fd;             /* control fd on /dev/ttyv0 for queries      */
	int  vtnum;              /* 1-based VT number we own                  */
	int  orig_vtnum;         /* VT to return to on release               */
	int  active;             /* 1 while our VT is the visible one         */
	int  graphics;           /* 1 if KD_GRAPHICS is currently set         */
	int  lock_switch;        /* 1 => refuse VT switches while we own it   */

	/* saved state for a clean restore */
	struct vt_mode saved_vtmode;
	int            have_saved_vtmode;
	struct termios saved_termios;
	int            have_saved_termios;
} vtcon_t;

/*
 * Grab a VT and take exclusive ownership.
 *   vtnum <= 0  -> first free VT via VT_OPENQRY (recommended)
 *   vtnum  > 0  -> that specific 1-based VT (7 == ttyv6)
 * Switches to it, enters graphics mode, installs the VT_PROCESS handshake and
 * quit-signal handlers. Returns 0 on success, -1 on failure (errno set).
 */
int  vtcon_acquire(vtcon_t* c, int vtnum);

/* Service any pending VT-switch handshake. Call once per frame. With switch
   locking on (default) a switch request is refused and c->active stays 1; with
   it off, c->active tracks visibility (0 while switched away, 1 while held). */
void vtcon_pump(vtcon_t* c);

/* Enable (on != 0, the default) or disable refusing of VT switches while we own
   the console. Takes effect on the next switch request. */
void vtcon_set_switch_lock(vtcon_t* c, int on);

/* Non-blocking read of one input byte from the VT keyboard (K_XLATE).
   Returns the byte (0..255), or -1 if nothing is pending. */
int  vtcon_getkey(vtcon_t* c);

/* Drain all pending keyboard input and report whether the user asked to quit.
   Returns 1 for 'q'/'Q' or a standalone ESC keypress, 0 otherwise. Arrow keys
   and other terminal escape sequences (ESC '[' ... / ESC 'O' ...) are consumed
   and ignored, so a stray sequence never trips the ESC-quit. Call this once per
   frame instead of a raw vtcon_getkey() loop that tests for byte 27. */
int  vtcon_quit_key(vtcon_t* c);

/* True once SIGINT / SIGTERM / SIGHUP has been received. */
int  vtcon_quit_requested(void);

/* Restore text mode, VT_AUTO, termios, cursor and the original VT. Idempotent
   and safe on a partially-acquired console. */
void vtcon_release(vtcon_t* c);

#endif /* VTCON_H */

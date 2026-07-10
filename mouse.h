#ifndef MOUSE_H
#define MOUSE_H

/*
 * mouse -- relative-pointer reader with two interchangeable backends,
 * selected at compile time by CPU architecture (see mouse.c):
 *
 *   sysmouse  (x86 / amd64 default) -- moused(8) republishes whatever
 *       pointing device is attached (PS/2, USB, ...) as the "sysmouse"
 *       protocol on /dev/sysmouse, so every consumer shares one stream. We
 *       ask for MOUSE_SETLEVEL 1 (8-byte packets: dx, dy, buttons, dz),
 *       falling back to level 0 (5-byte, no wheel) if the driver refuses.
 *
 *   evdev     (arm64 default) -- arm64 boards expose the pointer through the
 *       evdev interface as a stream of struct input_event records on
 *       /dev/input/eventN (default /dev/input/event3). We decode EV_REL
 *       (motion + wheel), EV_KEY (buttons) and clamp on EV_SYN/SYN_REPORT.
 *
 * Either backend can be forced regardless of arch with -DMOUSE_BACKEND_EVDEV
 * or -DMOUSE_BACKEND_SYSMOUSE. Buttons 4-7 are not decoded on either path --
 * out of scope for this seed.
 *
 * The public interface below is identical for both backends, so consumers
 * (server.c) never learn which one is compiled in.
 */

#include <stdint.h>

#define MOUSE_BTN_LEFT   0x01
#define MOUSE_BTN_MIDDLE 0x02
#define MOUSE_BTN_RIGHT  0x04

typedef struct {
	int fd;

	/* sysmouse backend: in-progress packet reassembly */
	int level;             /* 0 or 1, whichever MOUSE_SETLEVEL accepted */
	int pktlen;             /* 5 or 8, matching level                    */
	int syncmask, syncval;  /* sync-byte check, matching level           */
	unsigned char pkt[8];
	int nbytes;             /* bytes of the in-progress packet collected */

	/* evdev backend: carry for a struct input_event record split across
	   two reads (kernel returns whole records, but a short non-blocking
	   read can still leave a tail); 32 >= sizeof(struct input_event). */
	unsigned char rawbuf[32];
	int rawlen;

	int buttons;            /* live bitmask, MOUSE_BTN_*                 */
	int x, y;                /* accumulated absolute position, clamped   */
	int max_x, max_y;        /* inclusive bounds set by mouse_set_bounds */
	int dz;                   /* wheel delta accumulated since the last   */
	                          /* mouse_poll() call (reset on entry to it) */
} mouse_t;

/* Open dev, non-blocking; NULL => the compiled-in backend's default device
   ("/dev/sysmouse" for sysmouse, "/dev/input/event3" for evdev). Returns
   0/-1 (errno set). Safe to treat failure as "no mouse available" and carry
   on without one. */
int mouse_open(mouse_t* m, const char* dev);

/* Screen bounds (inclusive) for absolute-position clamping. Call once you
   know the framebuffer size, and again if it ever changes. */
void mouse_set_bounds(mouse_t* m, int max_x, int max_y);

/*
 * Drain all pending bytes, updating m->x/y/buttons as full packets complete.
 * Returns 1 if position or button state changed, 0 if nothing new landed,
 * -1 on a fatal read error (device gone -- caller should mouse_close()).
 */
int mouse_poll(mouse_t* m);

void mouse_close(mouse_t* m);

/* Set (e.g. from $MOUSE_DEBUG) to trace every decoded packet's raw bytes to
   stderr, even ones apply_packet() judges "unchanged" -- needed to see what
   a wheel click actually sends when it doesn't move dz the way we expect
   (the sign/scale of dz is unverified on real hardware, see mouse.c). */
extern int mouse_debug;

#endif /* MOUSE_H */

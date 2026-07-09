#ifndef MOUSE_H
#define MOUSE_H

/*
 * mouse -- FreeBSD /dev/sysmouse reader.
 *
 * moused(8) translates whatever pointing device is attached (PS/2, USB, ...)
 * into the "sysmouse" protocol and republishes it on /dev/sysmouse, so every
 * consumer (X, the console, us) shares one relative-motion stream regardless
 * of the underlying hardware. We open that device directly: no /dev/psm0, no
 * /dev/mem.
 *
 * The kernel supports two sysmouse packet sizes, chosen with MOUSE_SETLEVEL:
 *   level 0 -- 5 bytes: dx, dy, 3 buttons        (MOUSE_MSC_PACKETSIZE)
 *   level 1 -- 8 bytes: dx, dy, 3 buttons, dz     (MOUSE_SYS_PACKETSIZE)
 * We ask for level 1 so wheel motion is available; if the driver refuses
 * (or moused isn't running that way) level 0 still works, we just don't get
 * dz. Buttons 4-7 (MOUSE_SYS_EXTBUTTONS) are not decoded -- out of scope for
 * this seed.
 */

#include <stdint.h>

#define MOUSE_BTN_LEFT   0x01
#define MOUSE_BTN_MIDDLE 0x02
#define MOUSE_BTN_RIGHT  0x04

typedef struct {
	int fd;
	int level;             /* 0 or 1, whichever MOUSE_SETLEVEL accepted */
	int pktlen;             /* 5 or 8, matching level                    */
	int syncmask, syncval;  /* sync-byte check, matching level           */
	unsigned char pkt[8];
	int nbytes;             /* bytes of the in-progress packet collected */

	int buttons;            /* live bitmask, MOUSE_BTN_*                 */
	int x, y;                /* accumulated absolute position, clamped   */
	int max_x, max_y;        /* inclusive bounds set by mouse_set_bounds */
	int dz;                   /* wheel delta accumulated since the last   */
	                          /* mouse_poll() call (reset on entry to it) */
} mouse_t;

/* Open dev (NULL => "/dev/sysmouse"), non-blocking. Returns 0/-1 (errno set).
   Safe to treat failure as "no mouse available" and carry on without one. */
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

#endif /* MOUSE_H */

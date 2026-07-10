/*
 * mouse.c -- /dev/sysmouse packet decoder (see mouse.h).
 *
 * The first 5 bytes of a sysmouse packet are the classic Mouse-Systems-style
 * frame, present at both protocol levels:
 *
 *   byte 0 : sync byte -- top 2 bits are MOUSE_SYS_SYNC (0x80,0x00); the low
 *            3 bits are the button state, INVERTED (1 == that button is up).
 *   byte 1 : dx, first half   (signed)
 *   byte 2 : dy, first half   (signed)
 *   byte 3 : dx, second half  (signed; added to byte 1)
 *   byte 4 : dy, second half  (signed; added to byte 2)
 *
 * Splitting each axis across two bytes lets a fast flick exceed +-127 in one
 * packet. Device dy is positive = pointer moved away from the user (up the
 * screen), so it is subtracted from our screen-space y (which grows
 * downward).
 *
 * At level 1 a 6th-8th byte is appended (MOUSE_SYS_PACKETSIZE == 8):
 *   byte 5, 6 : wheel. NOT a clean signed delta on this hardware/driver --
 *            confirmed via $MOUSE_DEBUG raw-packet traces: scroll up sends
 *            {byte5=0x7f, byte6=0x00} (byte5 saturates at INT8_MAX rather
 *            than a small delta), scroll down sends {byte5=0x00,
 *            byte6=0x01} (direction carried by byte6 instead). We take
 *            sign(byte5 - byte6) and treat every completed packet as one
 *            "click" (+-1) rather than trusting the magnitude.
 *   byte 7    : extended button state -- not decoded here (see mouse.h).
 */

#include "mouse.h"

#include <sys/ioctl.h>
#include <sys/mouse.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>

int mouse_debug = 0;

int mouse_open(mouse_t* m, const char* dev) {
	int level;

	memset(m, 0, sizeof(*m));
	m->fd = -1;
	m->max_x = INT_MAX;
	m->max_y = INT_MAX;

	if (dev == NULL)
		dev = "/dev/sysmouse";

	if ((m->fd = open(dev, O_RDONLY | O_NONBLOCK)) == -1)
		return -1;

	level = 1;
	if (ioctl(m->fd, MOUSE_SETLEVEL, &level) == -1) {
		level = 0;
		(void)ioctl(m->fd, MOUSE_SETLEVEL, &level);  /* best effort */
	}
	m->level  = level;
	if (level == 1) {
		m->pktlen   = MOUSE_SYS_PACKETSIZE;
		m->syncmask = MOUSE_SYS_SYNCMASK;
		m->syncval  = MOUSE_SYS_SYNC;
	} else {
		m->pktlen   = MOUSE_MSC_PACKETSIZE;
		m->syncmask = MOUSE_MSC_SYNCMASK;
		m->syncval  = MOUSE_MSC_SYNC;
	}
	return 0;
}

void mouse_set_bounds(mouse_t* m, int max_x, int max_y) {
	m->max_x = max_x;
	m->max_y = max_y;
	if (m->x > max_x) m->x = max_x;
	if (m->y > max_y) m->y = max_y;
	if (m->x < 0) m->x = 0;
	if (m->y < 0) m->y = 0;
}

/* Decode one complete packet (m->pktlen bytes in m->pkt) into m->x/y/buttons.
   Returns 1 if anything changed. */
static int apply_packet(mouse_t* m) {
	int dx, dy, dz = 0;
	int buttons;
	int changed;

	dx = (int8_t)m->pkt[1] + (int8_t)m->pkt[3];
	dy = (int8_t)m->pkt[2] + (int8_t)m->pkt[4];
	if (m->pktlen >= MOUSE_SYS_PACKETSIZE) {
		/* see the mouse.c header comment: this hardware doesn't give a
		   clean small signed delta, just saturates one of two bytes
		   depending on direction -- normalise to one "click" (+-1) */
		int zraw = (int8_t)m->pkt[5] - (int8_t)m->pkt[6];
		dz = (zraw > 0) - (zraw < 0);
	}
	m->dz += dz;

	/* MOUSE_SYS_BUTTON*UP and MOUSE_MSC_BUTTON*UP share the same bit values
	   (0x04/0x02/0x01); using the SYS names for both levels is deliberate,
	   not a level-0/1 mixup. [VERIFY] against <sys/mouse.h> on target. */
	buttons = 0;
	if (!(m->pkt[0] & MOUSE_SYS_BUTTON1UP)) buttons |= MOUSE_BTN_LEFT;
	if (!(m->pkt[0] & MOUSE_SYS_BUTTON2UP)) buttons |= MOUSE_BTN_MIDDLE;
	if (!(m->pkt[0] & MOUSE_SYS_BUTTON3UP)) buttons |= MOUSE_BTN_RIGHT;

	changed = (dx != 0 || dy != 0 || dz != 0 || buttons != m->buttons);

	m->x += dx;
	m->y -= dy;                       /* device up (+dy) -> screen up (-y) */
	if (m->x < 0) m->x = 0;
	if (m->y < 0) m->y = 0;
	if (m->x > m->max_x) m->x = m->max_x;
	if (m->y > m->max_y) m->y = m->max_y;
	m->buttons = buttons;

	return changed;
}

int mouse_poll(mouse_t* m) {
	unsigned char buf[64];
	ssize_t       r;
	int           i;
	int           changed = 0;

	if (m->fd < 0)
		return -1;

	m->dz = 0;
	for (;;) {
		r = read(m->fd, buf, sizeof(buf));
		if (r < 0) {
			if (errno == EAGAIN)
				break;
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (r == 0)
			break;

		for (i = 0; i < r; i++) {
			unsigned char b = buf[i];

			if (m->nbytes == 0) {
				/* resync: a new packet must start with the sync byte */
				if ((b & m->syncmask) != m->syncval)
					continue;
			}
			m->pkt[m->nbytes++] = b;
			if (m->nbytes >= m->pktlen) {
				if (mouse_debug) {
					int k;
					fprintf(stderr, "pkt:");
					for (k = 0; k < m->pktlen; k++)
						fprintf(stderr, " %02x", m->pkt[k]);
					fprintf(stderr, "\n");
				}
				if (apply_packet(m))
					changed = 1;
				m->nbytes = 0;
			}
		}
	}
	return changed;
}

void mouse_close(mouse_t* m) {
	if (m->fd >= 0) {
		close(m->fd);
		m->fd = -1;
	}
}

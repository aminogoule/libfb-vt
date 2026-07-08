#ifndef FBSVGA_H
#define FBSVGA_H

/*
 * fbsvga.h -- VMware SVGA II specific extensions on top of the fb.h API.
 *
 * fb_svga.c also implements the plain fb.h entry points (fb_open/fb_close/
 * fb_drawbuf/fb_pitch/fb_flip), so ppm.c / server.c / ppm2fb.c build against
 * it unchanged. fb_open() picks a default mode; use fb_svga_open() for control.
 *
 * These extras expose what a linear-FB-with-2D display server actually needs
 * on VMware SVGA II, with NO X, NO DRM/KMS, NO vmwgfx:
 *   - explicit mode set (width/height/bpp)
 *   - enable/disable the SVGA scanout (hand the screen back to vt(4) text)
 *   - hardware 2D via the FIFO (RECT_COPY / RECT_FILL where the device
 *     advertises the capability; software fallback otherwise)
 *   - explicit UPDATE (dirty-rect present) for partial redraws
 */

#include "fb.h"
#include <stdint.h>

/*
 * Open and mode-set the SVGA II device.
 *   width,height -- requested resolution (clamped to SVGA_REG_MAX_*).
 *   bpp          -- 32 recommended (also the only value ppm.c blits).
 *   db           -- non-zero => software back buffer (fb_flip memcpys + UPDATE);
 *                   zero => draw straight into VRAM (fb_flip just UPDATEs).
 * Returns NULL on error (errno set). Needs root, kern.securelevel <= 0.
 */
framebuffer_t* fb_svga_open(int width, int height, int bpp, int db);

/*
 * Toggle the SVGA scanout. on=0 returns the device to VGA emulation so the
 * vt(4) text console is visible again; on=1 re-asserts our linear mode.
 * Wire this into the VT-switch handshake: disable on release, enable on
 * acquire (see the server.c snippet in the notes).
 */
void fb_svga_enable(framebuffer_t* fb, int on);

/* Present a dirty rectangle (SVGA_CMD_UPDATE). fb_flip() calls this full-screen. */
int  fb_svga_update(framebuffer_t* fb, int x, int y, int w, int h);

/*
 * Hardware 2D. Both operate directly on VRAM and present the result.
 *   fb_svga_fill -- solid rect (HW SVGA_CMD_RECT_FILL if capable, else CPU).
 *   fb_svga_copy -- move a rect within VRAM (HW SVGA_CMD_RECT_COPY if capable,
 *                   else CPU memmove). Ideal for console scroll / window drag.
 * color is 0x00RRGGBB packed as the framebuffer's native 32bpp pixel.
 * Return 0 on success, -1 on error.
 */
int  fb_svga_fill(framebuffer_t* fb, int x, int y, int w, int h, uint32_t color);
int  fb_svga_copy(framebuffer_t* fb, int sx, int sy, int dx, int dy, int w, int h);

/* Non-zero if the device advertised the hardware RECT_COPY / RECT_FILL caps. */
int  fb_svga_have_accel(framebuffer_t* fb);

#endif /* FBSVGA_H */

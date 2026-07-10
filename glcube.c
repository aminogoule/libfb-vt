/*
 * glcube.c -- the same spinning shaded cube as cube.c, but driven through
 *             mgl (mgl.h/mgl.c): a real matrix stack (mgl_rotate/translate/
 *             perspective) and immediate-mode glBegin/glVertex3f/glColor3f
 *             calls feeding a depth-buffered software rasterizer, instead of
 *             cube.c's hand-rolled per-frame trig + painter's-algorithm sort.
 *
 * Proof-of-concept for the "programmatic OpenGL" milestone: a real hardware
 * SVGA3D-backed GL is a separate, later effort (see mgl.h).
 *
 * Usage: glcube [vtnum]      (vtnum omitted/0 => first free VT)
 * Quit:  'q' / ESC, or SIGINT/SIGTERM. VT switching is blocked while running.
 * Run as root on a vt(4) console. Link with -lm.
 */

#include "fb.h"
#include "vtcon.h"
#include "mgl.h"

#include <math.h>
#include <time.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sysexits.h>
#include <stdint.h>

#define WIN_W   320
#define WIN_H   200
#define FRAME     3
#define TITLE    16

#define BG_DESK   0x2E4055u
#define BG_CLIENT 0x0C0E14u
#define CHROME    0xC8CCD4u
#define TITLEBAR  0x2A5AB8u

static int      g_w, g_h;
static uint8_t* g_buf;
static size_t   g_pitch;

static void fill_rect(int x, int y, int w, int h, uint32_t c) {
	int yy, xx;
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > g_w) w = g_w - x;
	if (y + h > g_h) h = g_h - y;
	for (yy = 0; yy < h; yy++) {
		uint32_t* line = (uint32_t*)(g_buf + (size_t)(y + yy) * g_pitch) + x;
		for (xx = 0; xx < w; xx++)
			line[xx] = c;
	}
}

/* ------------------------------------------------------------------ *
 * Cube geometry: 6 faces, one flat colour each (mgl_color3f per       *
 * vertex -- flat by giving all 4 corners of a face the same colour). *
 * ------------------------------------------------------------------ */
static const float CUBE_V[8][3] = {
	{-1,-1,-1}, { 1,-1,-1}, { 1, 1,-1}, {-1, 1,-1},   /* z = -1 */
	{-1,-1, 1}, { 1,-1, 1}, { 1, 1, 1}, {-1, 1, 1},   /* z = +1 */
};
static const int CUBE_F[6][4] = {
	{0,1,2,3}, {4,5,6,7},           /* -z, +z */
	{0,1,5,4}, {2,3,7,6},           /* -y, +y */
	{1,2,6,5}, {0,3,7,4},           /* +x, -x */
};
static const float CUBE_COL[6][3] = {
	{0.88f,0.28f,0.28f}, {0.28f,0.88f,0.28f}, {0.28f,0.41f,0.88f},
	{0.88f,0.75f,0.25f}, {0.75f,0.31f,0.82f}, {0.25f,0.78f,0.75f},
};

static void gl_cube_render(mgl_ctx_t* ctx, int wx, int wy, double t) {
	int f;

	mgl_set_target(g_buf, g_pitch, g_w, g_h);
	mgl_viewport(wx, wy, WIN_W, WIN_H);

	/* only clear the client area's colour; depth buffer is shared with the
	   whole screen but that's fine since nothing else writes it */
	fill_rect(wx, wy, WIN_W, WIN_H, BG_CLIENT);
	mgl_clear(0, 1);   /* depth only -- colour already hand-cleared above */

	mgl_matrix_mode(MGL_PROJECTION);
	mgl_load_identity();
	mgl_perspective(42.0f, (float)WIN_W / (float)WIN_H, 1.0f, 20.0f);

	mgl_matrix_mode(MGL_MODELVIEW);
	mgl_load_identity();
	mgl_translate(0.0f, 0.0f, -4.2f);
	mgl_rotate((float)(t * 0.7 * 180.0 / M_PI), 0.0f, 1.0f, 0.0f);
	mgl_rotate((float)(t * 0.9 * 180.0 / M_PI), 1.0f, 0.0f, 0.0f);
	mgl_rotate((float)(t * 0.35 * 180.0 / M_PI), 0.0f, 0.0f, 1.0f);

	mgl_enable_depth_test(1);
	mgl_enable_cull_face(1);

	mgl_begin(MGL_QUADS);
	for (f = 0; f < 6; f++) {
		const int* F = CUBE_F[f];
		int j;
		mgl_color3f(CUBE_COL[f][0], CUBE_COL[f][1], CUBE_COL[f][2]);
		for (j = 0; j < 4; j++)
			mgl_vertex3f(CUBE_V[F[j]][0], CUBE_V[F[j]][1], CUBE_V[F[j]][2]);
	}
	mgl_end();
	(void)ctx;
}

static void paint_desktop(int wx, int wy) {
	int ox = wx - FRAME, oy = wy - FRAME - TITLE;
	int ow = WIN_W + 2*FRAME, oh = WIN_H + 2*FRAME + TITLE;
	int i;

	fill_rect(0, 0, g_w, g_h, BG_DESK);
	fill_rect(ox, oy, ow, oh, CHROME);
	fill_rect(ox, oy, ow, TITLE, TITLEBAR);
	for (i = 0; i < 3; i++) {
		static const uint32_t dot[3] = {0xE05050, 0xE0C040, 0x50C060};
		fill_rect(ox + 6 + i*12, oy + TITLE/2 - 3, 6, 6, dot[i]);
	}
	fill_rect(wx, wy, WIN_W, WIN_H, BG_CLIENT);
}

int main(int argc, char* argv[]) {
	vtcon_t        con;
	framebuffer_t* fb = NULL;
	mgl_ctx_t*     gl = NULL;
	char           dev[32];
	int            vtnum = 0, rc = EX_OK, wx, wy;
	struct timespec t0;

	if (argc >= 2)
		vtnum = atoi(argv[1]);

	if (vtcon_acquire(&con, vtnum) != 0) {
		fprintf(stderr, "vtcon_acquire: %s\n", strerror(errno));
		return EX_OSERR;
	}

	snprintf(dev, sizeof(dev), "/dev/ttyv%d", con.vtnum - 1);
	if ((fb = fb_open(dev, 1)) == NULL) {
		fprintf(stderr, "fb_open(%s): %s\n", dev, strerror(errno));
		rc = EX_UNAVAILABLE;
		goto out;
	}

	g_w = fb->info.fb_width;
	g_h = fb->info.fb_height;
	wx  = (g_w - WIN_W) / 2;
	wy  = (g_h - WIN_H) / 2;

	fprintf(stderr,
	        "glcube: own vt %d, %dx%d %dbpp, window %dx%d (mgl software GL). "
	        "'q'/ESC to quit.\n",
	        con.vtnum, g_w, g_h, fb->info.fb_depth, WIN_W, WIN_H);

	gl = mgl_create(g_w, g_h);
	if (gl == NULL) {
		fprintf(stderr, "mgl_create: out of memory\n");
		rc = EX_UNAVAILABLE;
		goto out;
	}
	mgl_make_current(gl);

	g_buf   = (uint8_t*)fb_drawbuf(fb);
	g_pitch = fb_pitch(fb);
	paint_desktop(wx, wy);

	clock_gettime(CLOCK_MONOTONIC, &t0);

	while (!vtcon_quit_requested()) {
		struct timespec now, ts;
		double          t;

		vtcon_pump(&con);

		if (vtcon_quit_key(&con))
			goto out;

		if (con.active) {
			clock_gettime(CLOCK_MONOTONIC, &now);
			t = (now.tv_sec - t0.tv_sec) +
			    (now.tv_nsec - t0.tv_nsec) / 1e9;

			g_buf   = (uint8_t*)fb_drawbuf(fb);
			g_pitch = fb_pitch(fb);
			gl_cube_render(gl, wx, wy, t);
			fb_flip(fb);
		}

		ts.tv_sec = 0; ts.tv_nsec = 16L * 1000 * 1000;
		nanosleep(&ts, NULL);
	}

out:
	if (gl)
		mgl_destroy(gl);
	if (fb)
		fb_close(fb);
	vtcon_release(&con);
	return rc;
}

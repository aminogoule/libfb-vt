/*
 * cube.c -- a spinning, solid-shaded 3D cube rendered into a 320x200 window
 *           by the display-server skeleton (vtcon + LibFB). Sibling of
 *           server.c: same exclusive-VT ownership and render loop, but the
 *           payload is a live software-rendered cube instead of a PPM.
 *
 * Renderer: rotate (X/Y/Z) -> perspective project -> back-face cull ->
 *           painter's sort by depth -> flat-shaded filled faces (+ edge lines).
 * Pure software, so it runs on any backend (fb.c / fb_vga.c / fb_svga.c); on
 * the SVGA II backend you get true 32bpp colour.
 *
 * The 320x200 is the CUBE VIEWPORT (the drawable "window"); a thin frame and
 * title bar are drawn just outside it, so the animation area is exactly 320x200.
 *
 * Usage: cube [vtnum]      (vtnum omitted/0 => first free VT)
 * Quit:  'q' / ESC, or SIGINT/SIGTERM. VT switching is blocked while running.
 * Run as root on a vt(4) console. Link with -lm.
 */

#include "fb.h"
#include "vtcon.h"

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
#define FRAME     3        /* chrome border thickness              */
#define TITLE    16        /* title-bar height                     */

#define BG_DESK  0x2E4055u /* desktop backdrop (slate blue)        */
#define BG_CLIENT 0x0C0E14u/* inside the window (near-black)       */
#define CHROME   0xC8CCD4u /* window frame (light grey)            */
#define TITLEBAR 0x2A5AB8u /* title bar (blue)                     */

/* ------------------------------------------------------------------ *
 * Tiny clipped 32bpp primitives (0x00RRGGBB), all bounds-checked.    *
 * ------------------------------------------------------------------ */
static int      g_w, g_h;               /* screen size               */
static uint8_t* g_buf;                  /* draw buffer               */
static size_t   g_pitch;                /* bytes per scanline        */

static inline void put_px(int x, int y, uint32_t c) {
	if ((unsigned)x < (unsigned)g_w && (unsigned)y < (unsigned)g_h)
		*(uint32_t*)(g_buf + (size_t)y * g_pitch + (size_t)x * 4) = c;
}

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

static inline int imin3(int a, int b, int c){ int m=a<b?a:b; return m<c?m:c; }
static inline int imax3(int a, int b, int c){ int m=a>b?a:b; return m>c?m:c; }

/* Filled triangle via edge functions, clipped to [clx0,cly0]..[clx1,cly1]
   (inclusive). Winding-agnostic: a pixel is inside when all three edge
   functions share a sign. */
static void fill_tri(int clx0, int cly0, int clx1, int cly1,
                     int ax, int ay, int bx, int by, int cx, int cy,
                     uint32_t col) {
	int minx = imin3(ax, bx, cx), maxx = imax3(ax, bx, cx);
	int miny = imin3(ay, by, cy), maxy = imax3(ay, by, cy);
	int x, y;

	if (minx < clx0) minx = clx0;
	if (miny < cly0) miny = cly0;
	if (maxx > clx1) maxx = clx1;
	if (maxy > cly1) maxy = cly1;

	for (y = miny; y <= maxy; y++) {
		for (x = minx; x <= maxx; x++) {
			int e0 = (bx - ax) * (y - ay) - (by - ay) * (x - ax);
			int e1 = (cx - bx) * (y - by) - (cy - by) * (x - bx);
			int e2 = (ax - cx) * (y - cy) - (ay - cy) * (x - cx);
			if ((e0 >= 0 && e1 >= 0 && e2 >= 0) ||
			    (e0 <= 0 && e1 <= 0 && e2 <= 0))
				put_px(x, y, col);
		}
	}
}

/* Bresenham line clipped to a rect (used for face outlines). */
static void draw_line(int clx0, int cly0, int clx1, int cly1,
                      int x0, int y0, int x1, int y1, uint32_t col) {
	int dx =  abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
	int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
	int err = dx + dy, e2;
	for (;;) {
		if (x0 >= clx0 && x0 <= clx1 && y0 >= cly0 && y0 <= cly1)
			put_px(x0, y0, col);
		if (x0 == x1 && y0 == y1) break;
		e2 = 2 * err;
		if (e2 >= dy) { err += dy; x0 += sx; }
		if (e2 <= dx) { err += dx; y0 += sy; }
	}
}

/* ------------------------------------------------------------------ *
 * Cube geometry.                                                     *
 * ------------------------------------------------------------------ */
static const double CUBE_V[8][3] = {
	{-1,-1,-1}, { 1,-1,-1}, { 1, 1,-1}, {-1, 1,-1},   /* z = -1 */
	{-1,-1, 1}, { 1,-1, 1}, { 1, 1, 1}, {-1, 1, 1},   /* z = +1 */
};
static const int CUBE_F[6][4] = {
	{0,1,2,3}, {4,5,6,7},           /* -z, +z */
	{0,1,5,4}, {2,3,7,6},           /* -y, +y */
	{1,2,6,5}, {0,3,7,4},           /* +x, -x */
};
static const uint32_t CUBE_COL[6] = {
	0xE04848, 0x48E048, 0x4868E0, 0xE0C040, 0xC050D0, 0x40C8C0,
};

/* projection tuning: fits a unit cube comfortably inside 320x200 */
#define CUBE_SCALE  0.95
#define CUBE_DIST   4.2
#define CUBE_FOCAL  220.0

static void vnorm(double v[3]) {
	double n = sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
	if (n > 1e-9) { v[0]/=n; v[1]/=n; v[2]/=n; }
}

/*
 * Render one frame of the cube into the window whose client area top-left is
 * (wx,wy) and size WIN_W x WIN_H, at animation time t (seconds).
 */
static void cube_render(int wx, int wy, double t) {
	const double ax = t * 0.9, ay = t * 0.7, az = t * 0.35;
	const double sx = sin(ax), cx = cos(ax);
	const double sy = sin(ay), cy = cos(ay);
	const double sz = sin(az), cz = cos(az);
	double  L[3] = { 0.40, 0.60, -0.70 };
	double  rot[8][3];                       /* rotated verts        */
	int     px[8], py[8];                    /* projected 2D         */
	double  order[6];                        /* face view-space z    */
	int     idx[6], vis[6], nvis = 0;
	int     ccx = wx + WIN_W / 2, ccy = wy + WIN_H / 2;
	int     clx0 = wx, cly0 = wy, clx1 = wx + WIN_W - 1, cly1 = wy + WIN_H - 1;
	int     i, j, f;

	vnorm(L);

	/* clear only the client area (chrome/desktop persist in the back buffer) */
	fill_rect(wx, wy, WIN_W, WIN_H, BG_CLIENT);

	/* transform + project every vertex */
	for (i = 0; i < 8; i++) {
		double x = CUBE_V[i][0] * CUBE_SCALE;
		double y = CUBE_V[i][1] * CUBE_SCALE;
		double z = CUBE_V[i][2] * CUBE_SCALE;
		double y1 = y * cx - z * sx,  z1 = y * sx + z * cx;           /* X */
		double x2 = x * cy + z1 * sy, z2 = -x * sy + z1 * cy;         /* Y */
		double x3 = x2 * cz - y1 * sz, y3 = x2 * sz + y1 * cz;        /* Z */
		double zc, inv;
		rot[i][0] = x3; rot[i][1] = y3; rot[i][2] = z2;
		zc  = z2 + CUBE_DIST;
		inv = CUBE_FOCAL / zc;
		px[i] = ccx + (int)lround(x3 * inv);
		py[i] = ccy - (int)lround(y3 * inv);
	}

	/* per face: outward normal, cull, shade, record depth */
	for (f = 0; f < 6; f++) {
		const int* F = CUBE_F[f];
		double a[3], b[3], N[3], fc[3], fcv[3];
		double dvis, ndl, inten;

		for (j = 0; j < 3; j++) {
			a[j]  = rot[F[1]][j] - rot[F[0]][j];
			b[j]  = rot[F[2]][j] - rot[F[0]][j];
			fc[j] = (rot[F[0]][j]+rot[F[1]][j]+rot[F[2]][j]+rot[F[3]][j]) * 0.25;
		}
		N[0] = a[1]*b[2] - a[2]*b[1];
		N[1] = a[2]*b[0] - a[0]*b[2];
		N[2] = a[0]*b[1] - a[1]*b[0];
		vnorm(N);
		/* force the normal to point outward (away from the cube centre) */
		if (N[0]*fc[0] + N[1]*fc[1] + N[2]*fc[2] < 0) {
			N[0] = -N[0]; N[1] = -N[1]; N[2] = -N[2];
		}
		fcv[0] = fc[0]; fcv[1] = fc[1]; fcv[2] = fc[2] + CUBE_DIST;
		dvis = N[0]*fcv[0] + N[1]*fcv[1] + N[2]*fcv[2];
		if (dvis >= 0)                       /* back-facing: skip     */
			continue;

		ndl   = N[0]*L[0] + N[1]*L[1] + N[2]*L[2];
		if (ndl < 0) ndl = 0;
		inten = 0.25 + 0.75 * ndl;           /* ambient + diffuse     */
		{
			uint32_t base = CUBE_COL[f];
			unsigned r = (unsigned)(((base >> 16) & 0xFF) * inten);
			unsigned g = (unsigned)(((base >>  8) & 0xFF) * inten);
			unsigned b8= (unsigned)(( base        & 0xFF) * inten);
			if (r>255)r=255; if (g>255)g=255; if (b8>255)b8=255;
			vis[nvis] = f;
			idx[nvis] = (int)((r << 16) | (g << 8) | b8);   /* shaded colour */
			order[nvis] = fcv[2];
			nvis++;
		}
	}

	/* painter's algorithm: far faces first (larger view z) */
	for (i = 1; i < nvis; i++) {
		int   vf = vis[i], vc = idx[i];
		double vz = order[i];
		j = i - 1;
		while (j >= 0 && order[j] < vz) {
			vis[j+1]=vis[j]; idx[j+1]=idx[j]; order[j+1]=order[j]; j--;
		}
		vis[j+1]=vf; idx[j+1]=vc; order[j+1]=vz;
	}

	/* fill each visible face as two triangles, then outline it */
	for (i = 0; i < nvis; i++) {
		const int* F = CUBE_F[vis[i]];
		uint32_t col = (uint32_t)idx[i];
		uint32_t edge = ((col >> 2) & 0x3F3F3F);           /* darker outline */
		fill_tri(clx0, cly0, clx1, cly1,
		         px[F[0]],py[F[0]], px[F[1]],py[F[1]], px[F[2]],py[F[2]], col);
		fill_tri(clx0, cly0, clx1, cly1,
		         px[F[0]],py[F[0]], px[F[2]],py[F[2]], px[F[3]],py[F[3]], col);
		for (j = 0; j < 4; j++) {
			int p0 = F[j], p1 = F[(j+1) & 3];
			draw_line(clx0, cly0, clx1, cly1,
			          px[p0],py[p0], px[p1],py[p1], edge);
		}
	}
}

/* Paint the desktop and the window chrome once (persists in the back buffer). */
static void paint_desktop(int wx, int wy) {
	int ox = wx - FRAME, oy = wy - FRAME - TITLE;
	int ow = WIN_W + 2*FRAME, oh = WIN_H + 2*FRAME + TITLE;
	int i;

	fill_rect(0, 0, g_w, g_h, BG_DESK);          /* backdrop            */
	fill_rect(ox, oy, ow, oh, CHROME);           /* window frame        */
	fill_rect(ox, oy, ow, TITLE, TITLEBAR);      /* title bar           */
	for (i = 0; i < 3; i++) {                     /* three title dots   */
		static const uint32_t dot[3] = {0xE05050, 0xE0C040, 0x50C060};
		fill_rect(ox + 6 + i*12, oy + TITLE/2 - 3, 6, 6, dot[i]);
	}
	fill_rect(wx, wy, WIN_W, WIN_H, BG_CLIENT);  /* client area         */
}

int main(int argc, char* argv[]) {
	vtcon_t        con;
	framebuffer_t* fb = NULL;
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
	        "cube: own vt %d, %dx%d %dbpp, window %dx%d. 'q'/ESC to quit.\n",
	        con.vtnum, g_w, g_h, fb->info.fb_depth, WIN_W, WIN_H);

	/* draw the static desktop/chrome once into the back buffer */
	g_buf   = (uint8_t*)fb_drawbuf(fb);
	g_pitch = fb_pitch(fb);
	paint_desktop(wx, wy);

	clock_gettime(CLOCK_MONOTONIC, &t0);

	while (!vtcon_quit_requested()) {
		struct timespec now, ts;
		double          t;

		vtcon_pump(&con);

		/* quit on 'q'/'Q'/Escape; arrow keys & VT-switch combos are ignored */
		if (vtcon_quit_key(&con))
			goto out;

		if (con.active) {
			clock_gettime(CLOCK_MONOTONIC, &now);
			t = (now.tv_sec - t0.tv_sec) +
			    (now.tv_nsec - t0.tv_nsec) / 1e9;

			g_buf   = (uint8_t*)fb_drawbuf(fb);
			g_pitch = fb_pitch(fb);
			cube_render(wx, wy, t);
			fb_flip(fb);
		}

		ts.tv_sec = 0; ts.tv_nsec = 16L * 1000 * 1000;   /* ~60 fps */
		nanosleep(&ts, NULL);
	}

out:
	if (fb)
		fb_close(fb);
	vtcon_release(&con);
	return rc;
}

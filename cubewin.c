/*
 * cubewin -- the mgl spinning cube (see glcube.c) as a compositor *client*,
 * not a standalone VT owner.
 *
 * cube.c/glcube.c each call vtcon_acquire() and grab an exclusive VT of
 * their own -- fine as standalone demos, but that's a second process fighting
 * the compositor (server.c) for console/VT ownership if both run at once.
 * cubewin instead behaves exactly like term.c: it is a plain socket client of
 * the compositor's AF_UNIX protocol (proto.h). It owns no VT and opens no
 * framebuffer; it asks the compositor for one surface (window), gets back a
 * shared-memory pixel buffer + a shm fd via SCM_RIGHTS, and from then on just
 * renders into that buffer with mgl and sends FBVT_COMMIT -- the same
 * "shared buffer, compositor does the compositing" relationship term.c's text
 * grid has with its window. No framebuffer backend to link against, no
 * per-backend build variant (see Makefile: built once, like term).
 *
 * Usage: cubewin [width] [height]   (defaults 320x200)
 * Quit:  'q'/'Q'/ESC (routed through the compositor like any other key),
 *        or the compositor closing our window (FBVT_CLOSE / system menu).
 */

#include "proto.h"
#include "mgl.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <poll.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <sysexits.h>

#define DEF_W 320
#define DEF_H 200

#define BG_CLIENT 0x0C0E14u

typedef struct {
	int       sock;
	uint32_t  surf_id;
	uint32_t* pix;
	size_t    map_size;
	int       pw, ph;
	size_t    stride;
} win_t;

static volatile sig_atomic_t g_quit;

static void on_signal(int sig) { (void)sig; g_quit = 1; }

/* ------------------------------------------------------------------ *
 * Cube geometry: same as glcube.c.                                    *
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

static void gl_cube_render(win_t* w, double t) {
	int f;

	mgl_set_target((uint8_t*)w->pix, w->stride, w->pw, w->ph);
	mgl_viewport(0, 0, w->pw, w->ph);
	mgl_clear_color((((BG_CLIENT >> 16) & 0xFF) / 255.0f),
	                (((BG_CLIENT >>  8) & 0xFF) / 255.0f),
	                (( BG_CLIENT        & 0xFF) / 255.0f), 1.0f);
	mgl_clear(1, 1);

	mgl_matrix_mode(MGL_PROJECTION);
	mgl_load_identity();
	mgl_perspective(42.0f, (float)w->pw / (float)w->ph, 1.0f, 20.0f);

	mgl_matrix_mode(MGL_MODELVIEW);
	mgl_load_identity();
	mgl_translate(0.0f, 0.0f, -4.2f);
	mgl_rotate((float)(t * 0.7 * 180.0 / M_PI), 0.0f, 1.0f, 0.0f);
	mgl_rotate((float)(t * 0.9 * 180.0 / M_PI), 1.0f, 0.0f, 0.0f);
	mgl_rotate((float)(t * 0.35 * 180.0 / M_PI), 0.0f, 0.0f, 1.0f);

	mgl_enable_depth_test(1);
	/* Double-sided: see glcube.c's gl_cube_render for why culling is off --
	   CUBE_F's winding isn't consistently outward-CCW for every face, so
	   culling drops the wrong faces and the far side bleeds through. The
	   depth buffer alone hides occluded faces correctly either way. */
	mgl_enable_cull_face(0);

	mgl_begin(MGL_QUADS);
	for (f = 0; f < 6; f++) {
		const int* F = CUBE_F[f];
		int j;
		mgl_color3f(CUBE_COL[f][0], CUBE_COL[f][1], CUBE_COL[f][2]);
		for (j = 0; j < 4; j++)
			mgl_vertex3f(CUBE_V[F[j]][0], CUBE_V[F[j]][1], CUBE_V[F[j]][2]);
	}
	mgl_end();
	mgl_swap_buffers();
}

/* ------------------------------------------------------------------ *
 * Protocol plumbing -- same shape as term.c's connect_server/handshake. *
 * ------------------------------------------------------------------ */
static int connect_server(void) {
	struct sockaddr_un sa;
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	strncpy(sa.sun_path, FBVT_SOCK_PATH, sizeof(sa.sun_path) - 1);
	if (connect(fd, (struct sockaddr*)&sa, sizeof(sa)) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static int handshake(win_t* w, int want_w, int want_h) {
	struct fbvt_msg m;
	int             passfd = -1;

	memset(&m, 0, sizeof(m));
	m.type = FBVT_HELLO; m.arg[0] = FBVT_PROTO_VERSION;
	if (fbvt_send(w->sock, &m, NULL) != 0)
		return -1;
	if (fbvt_recv(w->sock, &m, NULL, 0, NULL) != 1 || m.type != FBVT_HELLO_ACK)
		return -1;

	memset(&m, 0, sizeof(m));
	m.type = FBVT_CREATE_SURFACE;
	m.arg[0] = want_w;
	m.arg[1] = want_h;
	if (fbvt_send(w->sock, &m, NULL) != 0)
		return -1;

	if (fbvt_recv(w->sock, &m, NULL, 0, &passfd) != 1 ||
	    m.type != FBVT_SURFACE_CREATED || passfd < 0)
		return -1;

	w->surf_id  = m.id;
	w->pw       = m.arg[0];
	w->ph       = m.arg[1];
	w->stride   = (size_t)m.arg[2];
	w->map_size = w->stride * (size_t)w->ph;
	w->pix = mmap(NULL, w->map_size, PROT_READ | PROT_WRITE,
	              MAP_SHARED, passfd, 0);
	close(passfd);
	if (w->pix == MAP_FAILED)
		return -1;
	return 0;
}

static void set_title(win_t* w, const char* s) {
	struct fbvt_msg m;
	size_t n = strlen(s);
	memset(&m, 0, sizeof(m));
	m.type   = FBVT_SET_TITLE;
	m.id     = w->surf_id;
	m.paylen = (uint32_t)n;
	fbvt_send(w->sock, &m, s);
}

static void commit(win_t* w) {
	struct fbvt_msg m;
	memset(&m, 0, sizeof(m));
	m.type   = FBVT_COMMIT;
	m.id     = w->surf_id;
	m.arg[0] = 0; m.arg[1] = 0; m.arg[2] = w->pw; m.arg[3] = w->ph;
	fbvt_send(w->sock, &m, NULL);
}

/* True if this input byte means "quit" (q/Q/ESC), same convention as the
   standalone cube.c/glcube.c demos. */
static int is_quit_byte(unsigned char b) {
	return b == 'q' || b == 'Q' || b == 0x1B;
}

int main(int argc, char* argv[]) {
	win_t          w;
	mgl_ctx_t*     gl;
	int            want_w = DEF_W, want_h = DEF_H;
	int            rc = EX_OK;
	struct timespec t0;

	if (argc >= 2) want_w = atoi(argv[1]);
	if (argc >= 3) want_h = atoi(argv[2]);
	if (want_w < 16) want_w = DEF_W;
	if (want_h < 16) want_h = DEF_H;

	signal(SIGPIPE, SIG_IGN);
	signal(SIGINT,  on_signal);
	signal(SIGTERM, on_signal);

	memset(&w, 0, sizeof(w));
	w.sock = -1;

	if ((w.sock = connect_server()) < 0) {
		fprintf(stderr, "cubewin: cannot connect to %s: %s\n",
		        FBVT_SOCK_PATH, strerror(errno));
		return EX_UNAVAILABLE;
	}
	if (handshake(&w, want_w, want_h) != 0) {
		fprintf(stderr, "cubewin: handshake failed: %s\n", strerror(errno));
		rc = EX_PROTOCOL;
		goto out;
	}

	gl = mgl_create(w.pw, w.ph);
	if (gl == NULL) {
		fprintf(stderr, "cubewin: mgl_create: out of memory\n");
		rc = EX_OSERR;
		goto out;
	}
	mgl_make_current(gl);

	set_title(&w, "cubewin (mgl)");

	clock_gettime(CLOCK_MONOTONIC, &t0);

	while (!g_quit) {
		struct pollfd pfd;
		struct timespec now;
		double t;
		int n, want_quit = 0;

		pfd.fd = w.sock; pfd.events = POLLIN; pfd.revents = 0;
		n = poll(&pfd, 1, 16);
		if (n < 0) {
			if (errno == EINTR) continue;
			break;
		}

		if (pfd.revents & (POLLIN | POLLHUP | POLLERR)) {
			struct fbvt_msg m;
			unsigned char   seqbuf[8];
			int r = fbvt_recv(w.sock, &m, seqbuf, sizeof(seqbuf), NULL);
			if (r <= 0)
				break;                     /* compositor gone */
			if (m.type == FBVT_INPUT_KEY) {
				want_quit = is_quit_byte((unsigned char)m.arg[0]);
			} else if (m.type == FBVT_INPUT_KEYSEQ) {
				uint32_t i;
				for (i = 0; i < m.paylen && i < sizeof(seqbuf); i++)
					if (is_quit_byte(seqbuf[i]))
						want_quit = 1;
			} else if (m.type == FBVT_CLOSE) {
				want_quit = 1;
			}
		}
		if (want_quit)
			break;

		clock_gettime(CLOCK_MONOTONIC, &now);
		t = (now.tv_sec - t0.tv_sec) + (now.tv_nsec - t0.tv_nsec) / 1e9;

		gl_cube_render(&w, t);
		commit(&w);
	}

	mgl_destroy(gl);

	if (w.surf_id) {
		struct fbvt_msg m;
		memset(&m, 0, sizeof(m));
		m.type = FBVT_DESTROY_SURFACE;
		m.id   = w.surf_id;
		fbvt_send(w.sock, &m, NULL);
	}

out:
	if (w.pix && w.pix != MAP_FAILED) munmap(w.pix, w.map_size);
	if (w.sock >= 0) close(w.sock);
	return rc;
}

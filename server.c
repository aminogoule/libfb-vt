/*
 * server -- a seed framebuffer *compositor* for FreeBSD/vt(4).
 *
 * It takes exclusive ownership of a dedicated VT (native vt(4) protocol, no
 * DRM/KMS/linuxkpi), enters graphics mode, and then acts as a tiny display
 * server: it listens on an AF_UNIX socket (FBVT_SOCK_PATH), hands each client a
 * shared-memory pixel buffer for a "surface" (window), composites all surfaces
 * -- desktop backdrop, window chrome, title, client pixels -- into the
 * framebuffer, and forwards keyboard input to the focused (topmost) surface.
 *
 * This is the compositor half of the client/server split; the terminal lives in
 * term.c as a client. See proto.h for the wire protocol.
 *
 * Layers:  fb.c (framebuffer) + vtcon.c (VT ownership) + proto.c (IPC)
 *
 * Usage:  server [-e command] [-w wallpaper.ppm] [vtnum]
 *           -e command : after the VT is grabbed and the socket is up, spawn
 *                        `command` (via /bin/sh -c) as the first client. Handy
 *                        because a locked VT leaves no shell to launch one from,
 *                        e.g.  server -e ./term
 *           -w file    : PPM to use as the desktop wallpaper (stretched to fill
 *                        the screen). Without it the desktop is a solid colour.
 *           vtnum      : 1-based VT to own (omitted/0 => first free VT).
 *
 * Quit:  SIGINT/SIGTERM/SIGHUP, or automatically once the last client that ever
 *        connected has disconnected (e.g. the terminal's shell exited).
 * Run as root on a vt(4) console (sysctl kern.vty == vt).
 */

#include "fb.h"
#include "vtcon.h"
#include "proto.h"
#include "ppm.h"
#include "font8x16.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sysexits.h>

#define MAX_SURFACES 16
#define FRAME         2          /* window border thickness              */
#define TITLE        20          /* title-bar height                     */
#define GLYPH_W       8
#define GLYPH_H      16

#define BG_DESK   0x2E4055u      /* desktop backdrop (slate blue)        */
#define CHROME    0xC8CCD4u      /* window frame (light grey)            */
#define TITLEBAR  0x2A5AB8u      /* focused title bar (blue)             */
#define TITLEBAR2 0x54606Fu      /* unfocused title bar (grey)           */
#define TITLETX   0xFFFFFFu      /* title text                           */

/* ------------------------------------------------------------------ *
 * A surface == one client window. The array order IS the z-order:     *
 * index 0 is the bottom window, the last is the top (focused) one.    *
 * ------------------------------------------------------------------ */
typedef struct {
	int       fd;                /* client socket                        */
	int       hello;             /* HELLO received                       */
	uint32_t  id;                /* surface id (0 until CREATE_SURFACE)  */
	int       shm_fd;            /* our end of the shared pixel buffer   */
	uint32_t* pix;               /* mmap'd client buffer (NULL if none)  */
	size_t    map_size;
	int       w, h;              /* client pixel size                    */
	size_t    stride;            /* bytes per row (== w*4)               */
	int       x, y;              /* client-area top-left on screen       */
	char      title[FBVT_MAX_PAYLOAD + 1];
} surface_t;

static surface_t s_surf[MAX_SURFACES];
static int       s_nsurf   = 0;
static uint32_t  s_next_id = 1;
static int       s_had_client = 0;   /* at least one client has connected */
static img32_t*  s_wall    = NULL;   /* desktop wallpaper (NULL => solid)  */

/* screen draw state (mirrors cube.c's tiny primitive layer) */
static int      g_w, g_h;
static uint8_t* g_buf;
static size_t   g_pitch;

/* ------------------------------------------------------------------ *
 * Clipped 32bpp screen primitives (0x00RRGGBB).                       *
 * ------------------------------------------------------------------ */
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

/* Paint the desktop backdrop: a wallpaper PPM stretched (nearest-neighbour) to
   fill the whole screen, or a solid colour when no wallpaper is loaded. */
static void draw_desktop(void) {
	int x, y;

	if (s_wall == NULL || s_wall->w <= 0 || s_wall->h <= 0) {
		fill_rect(0, 0, g_w, g_h, BG_DESK);
		return;
	}
	for (y = 0; y < g_h; y++) {
		int sy = (int)((long)y * s_wall->h / g_h);
		const uint32_t* srow = &(&s_wall->pixel)[(size_t)sy * s_wall->w];
		uint32_t* drow = (uint32_t*)(g_buf + (size_t)y * g_pitch);
		for (x = 0; x < g_w; x++) {
			int sx = (int)((long)x * s_wall->w / g_w);
			drow[x] = srow[sx];
		}
	}
}

/* draw one 8x16 glyph; bit 0 of each row byte is the leftmost pixel */
static void draw_glyph(int x, int y, unsigned char ch, uint32_t col) {
	int row, bit;
	const unsigned char* g = font8x16_basic[ch];
	for (row = 0; row < GLYPH_H; row++)
		for (bit = 0; bit < GLYPH_W; bit++)
			if (g[row] & (1u << bit))
				put_px(x + bit, y + row, col);
}

static void draw_string(int x, int y, const char* s, uint32_t col) {
	for (; *s; s++, x += GLYPH_W) {
		if (*s == '\t' || *s == '\n' || *s == '\r')
			continue;
		draw_glyph(x, y, (unsigned char)*s, col);
	}
}

/* blit a client buffer (w*h at `stride` bytes/row, 0x00RRGGBB) to the screen at
   (dx,dy), cropped to the screen. */
static void blit_surface(const surface_t* s) {
	int row, cols;
	int sy0 = 0, sx0 = 0;
	int dx = s->x, dy = s->y;
	int w = s->w, h = s->h;

	if (s->pix == NULL)
		return;

	if (dx < 0) { sx0 = -dx; w += dx; dx = 0; }
	if (dy < 0) { sy0 = -dy; h += dy; dy = 0; }
	if (dx + w > g_w) w = g_w - dx;
	if (dy + h > g_h) h = g_h - dy;
	if (w <= 0 || h <= 0)
		return;

	cols = w;
	for (row = 0; row < h; row++) {
		const uint32_t* src =
			(const uint32_t*)((const uint8_t*)s->pix +
			                  (size_t)(sy0 + row) * s->stride) + sx0;
		uint32_t* dst =
			(uint32_t*)(g_buf + (size_t)(dy + row) * g_pitch) + dx;
		memcpy(dst, src, (size_t)cols * 4);
	}
}

/* Composite the whole scene: desktop, then every surface bottom-to-top with
   its chrome. The topmost surface (index s_nsurf-1) is the focused one. */
static void composite(void) {
	int i;

	draw_desktop();

	for (i = 0; i < s_nsurf; i++) {
		surface_t* s = &s_surf[i];
		int focused = (i == s_nsurf - 1);
		int ox, oy, ow, oh;

		if (s->pix == NULL)              /* surface not realised yet */
			continue;

		ox = s->x - FRAME;
		oy = s->y - FRAME - TITLE;
		ow = s->w + 2 * FRAME;
		oh = s->h + 2 * FRAME + TITLE;

		fill_rect(ox, oy, ow, oh, CHROME);                       /* frame    */
		fill_rect(ox, oy, ow, TITLE,
		          focused ? TITLEBAR : TITLEBAR2);               /* titlebar */
		if (s->title[0])
			draw_string(ox + 6, oy + (TITLE - GLYPH_H) / 2,
			            s->title, TITLETX);
		blit_surface(s);                                         /* pixels   */
	}
}

/* ------------------------------------------------------------------ *
 * Surface bookkeeping.                                                *
 * ------------------------------------------------------------------ */

/* Tear down surface at index i and compact the array (preserving z-order). */
static void surface_drop(int i) {
	surface_t* s = &s_surf[i];

	if (s->pix && s->pix != MAP_FAILED)
		munmap(s->pix, s->map_size);
	if (s->shm_fd >= 0)
		close(s->shm_fd);
	if (s->fd >= 0)
		close(s->fd);

	memmove(&s_surf[i], &s_surf[i + 1],
	        (size_t)(s_nsurf - i - 1) * sizeof(s_surf[0]));
	s_nsurf--;
}

/* Realise a CREATE_SURFACE: allocate the shared buffer, position the window,
   and reply with SURFACE_CREATED carrying the shm fd. Returns 0/-1. */
static int surface_realise(surface_t* s, int w, int h) {
	struct fbvt_msg r;
	int    maxw = g_w - 2 * FRAME;
	int    maxh = g_h - 2 * FRAME - TITLE;
	int    n;

	if (w < 1) w = 1;
	if (h < 1) h = 1;
	if (w > maxw) w = maxw;
	if (h > maxh) h = maxh;

	s->w      = w;
	s->h      = h;
	s->stride = (size_t)w * 4;
	s->map_size = s->stride * (size_t)h;

	s->shm_fd = shm_open(SHM_ANON, O_RDWR, 0600);
	if (s->shm_fd < 0)
		return -1;
	if (ftruncate(s->shm_fd, (off_t)s->map_size) != 0)
		return -1;
	s->pix = mmap(NULL, s->map_size, PROT_READ | PROT_WRITE,
	              MAP_SHARED, s->shm_fd, 0);
	if (s->pix == MAP_FAILED) {
		s->pix = NULL;
		return -1;
	}

	/* cascade windows from the top-left so several are visible at once */
	n = s_nsurf - 1;                     /* this surface's index (it's on top) */
	s->x = FRAME + 24 + (n % 6) * 28;
	s->y = FRAME + TITLE + 24 + (n % 6) * 28;
	if (s->x + s->w > g_w) s->x = (g_w - s->w) / 2;
	if (s->y + s->h > g_h) s->y = (g_h - s->h) / 2 + TITLE;
	if (s->x < FRAME) s->x = FRAME;
	if (s->y < FRAME + TITLE) s->y = FRAME + TITLE;

	s->id = s_next_id++;

	memset(&r, 0, sizeof(r));
	r.type   = FBVT_SURFACE_CREATED;
	r.id     = s->id;
	r.arg[0] = s->w;
	r.arg[1] = s->h;
	r.arg[2] = (int32_t)s->stride;
	if (fbvt_send_fd(s->fd, &r, s->shm_fd) != 0)
		return -1;
	return 0;
}

/* ------------------------------------------------------------------ *
 * Client message handling. Returns 0 to keep the client, -1 to drop.  *
 * ------------------------------------------------------------------ */
static int client_dispatch(int i, int* dirty) {
	surface_t*      s = &s_surf[i];
	struct fbvt_msg m;
	char            pay[FBVT_MAX_PAYLOAD + 1];
	int             passfd = -1;
	int             rc;

	rc = fbvt_recv(s->fd, &m, pay, FBVT_MAX_PAYLOAD, &passfd);
	if (passfd >= 0)                     /* clients never send us fds */
		close(passfd);
	if (rc <= 0)
		return -1;                       /* EOF or error => drop */

	switch (m.type) {
	case FBVT_HELLO: {
		struct fbvt_msg ack;
		s->hello = 1;
		memset(&ack, 0, sizeof(ack));
		ack.type   = FBVT_HELLO_ACK;
		ack.arg[0] = FBVT_PROTO_VERSION;
		if (fbvt_send(s->fd, &ack, NULL) != 0)
			return -1;
		break;
	}
	case FBVT_CREATE_SURFACE:
		if (s->pix != NULL)              /* one surface per client (seed) */
			break;
		if (surface_realise(s, m.arg[0], m.arg[1]) != 0)
			return -1;
		*dirty = 1;
		break;
	case FBVT_COMMIT:
		/* damage tracking is coarse: any commit re-composites everything */
		if (s->pix != NULL)
			*dirty = 1;
		break;
	case FBVT_SET_TITLE:
		if (m.paylen > FBVT_MAX_PAYLOAD)
			m.paylen = FBVT_MAX_PAYLOAD;
		memcpy(s->title, pay, m.paylen);
		s->title[m.paylen] = '\0';
		*dirty = 1;
		break;
	case FBVT_DESTROY_SURFACE:
		return -1;                       /* drop => teardown */
	default:
		/* unknown / server-only messages: ignore */
		break;
	}
	return 0;
}

/* Forward one keyboard byte to the focused (topmost) surface. */
static void route_key(int byte) {
	surface_t*      s;
	struct fbvt_msg m;

	if (s_nsurf == 0)
		return;
	s = &s_surf[s_nsurf - 1];            /* focused == topmost */
	if (s->id == 0)
		return;

	memset(&m, 0, sizeof(m));
	m.type   = FBVT_INPUT_KEY;
	m.id     = s->id;
	m.arg[0] = byte & 0xFF;
	(void)fbvt_send(s->fd, &m, NULL);    /* a dead client is reaped on recv */
}

/* ------------------------------------------------------------------ *
 * Socket setup.                                                       *
 * ------------------------------------------------------------------ */
static int listen_socket(void) {
	struct sockaddr_un sa;
	int fd;

	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
		return -1;

	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	strncpy(sa.sun_path, FBVT_SOCK_PATH, sizeof(sa.sun_path) - 1);

	unlink(FBVT_SOCK_PATH);              /* clear a stale socket */
	if (bind(fd, (struct sockaddr*)&sa, sizeof(sa)) != 0) {
		close(fd);
		return -1;
	}
	if (listen(fd, 8) != 0) {
		unlink(FBVT_SOCK_PATH);
		close(fd);
		return -1;
	}
	return fd;
}

/* Spawn `cmd` via /bin/sh -c as an initial client. */
static void spawn_client(const char* cmd) {
	pid_t pid = fork();
	if (pid == 0) {
		execl("/bin/sh", "sh", "-c", cmd, (char*)NULL);
		_exit(127);
	}
	/* parent: SIGCHLD is ignored, so the child is auto-reaped */
}

int main(int argc, char* argv[]) {
	vtcon_t        con;
	framebuffer_t* fb  = NULL;
	char           dev[32];
	const char*    spawn = NULL;
	const char*    wall  = NULL;
	int            vtnum = 0;
	int            lfd   = -1;
	int            rc    = EX_OK;
	int            argi;

	/* args: [-e command] [-w wallpaper.ppm] [vtnum] */
	for (argi = 1; argi < argc; argi++) {
		if (strcmp(argv[argi], "-e") == 0 && argi + 1 < argc)
			spawn = argv[++argi];
		else if (strcmp(argv[argi], "-w") == 0 && argi + 1 < argc)
			wall = argv[++argi];
		else
			vtnum = atoi(argv[argi]);
	}

	signal(SIGPIPE, SIG_IGN);            /* dead-client writes => EPIPE  */
	signal(SIGCHLD, SIG_IGN);            /* auto-reap spawned clients    */

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
	if ((lfd = listen_socket()) < 0) {
		fprintf(stderr, "listen_socket(%s): %s\n",
		        FBVT_SOCK_PATH, strerror(errno));
		rc = EX_OSERR;
		goto out;
	}

	g_w = fb->info.fb_width;
	g_h = fb->info.fb_height;

	fprintf(stderr,
	        "server: compositor on vt %d, %dx%d %dbpp, socket %s.\n",
	        con.vtnum, g_w, g_h, fb->info.fb_depth, FBVT_SOCK_PATH);

	if (wall) {
		s_wall = read_ppm(wall);
		if (s_wall == NULL)
			fprintf(stderr, "server: cannot read wallpaper %s: %s "
			        "(using solid backdrop)\n", wall, strerror(errno));
	}

	if (spawn)
		spawn_client(spawn);

	/* first paint: desktop (wallpaper or solid) */
	g_buf   = (uint8_t*)fb_drawbuf(fb);
	g_pitch = fb_pitch(fb);
	composite();
	fb_flip(fb);

	while (!vtcon_quit_requested()) {
		struct pollfd pfd[2 + MAX_SURFACES];
		int           npfd = 0;
		int           dirty = 0;
		int           key, i, n;

		vtcon_pump(&con);                /* service VT-switch handshake */

		/* build the poll set: keyboard, listener, clients */
		pfd[npfd].fd = con.fd;    pfd[npfd].events = POLLIN; npfd++;
		pfd[npfd].fd = lfd;       pfd[npfd].events = POLLIN; npfd++;
		for (i = 0; i < s_nsurf; i++) {
			pfd[npfd].fd     = s_surf[i].fd;
			pfd[npfd].events = POLLIN;
			npfd++;
		}

		n = poll(pfd, (nfds_t)npfd, 100);
		if (n < 0 && errno != EINTR) {
			fprintf(stderr, "poll: %s\n", strerror(errno));
			break;
		}

		/* keyboard -> focused client (drain all pending bytes) */
		if (pfd[0].revents & POLLIN) {
			while ((key = vtcon_getkey(&con)) != -1)
				route_key(key);
		}

		/* new client connections */
		if (pfd[1].revents & POLLIN) {
			int cfd = accept(lfd, NULL, NULL);
			if (cfd >= 0) {
				if (s_nsurf >= MAX_SURFACES) {
					close(cfd);
				} else {
					surface_t* s = &s_surf[s_nsurf++];
					memset(s, 0, sizeof(*s));
					s->fd = cfd;
					s->shm_fd = -1;
					s_had_client = 1;
				}
			}
		}

		/* client messages -- iterate the client pollfds by the fd they carry.
		   Dispatching may drop a client and compact s_surf, so we resolve each
		   pollfd's fd back to its current surface index every time rather than
		   trusting positional indices. */
		{
			int nclients = npfd - 2;
			int j;
			for (j = 0; j < nclients; j++) {
				int   cfd = pfd[2 + j].fd;
				short re  = pfd[2 + j].revents;
				if (!(re & (POLLIN | POLLHUP | POLLERR)))
					continue;
				for (i = 0; i < s_nsurf; i++)
					if (s_surf[i].fd == cfd)
						break;
				if (i == s_nsurf)        /* already dropped this round */
					continue;
				if (client_dispatch(i, &dirty) != 0) {
					surface_drop(i);
					dirty = 1;
				}
			}
		}

		/* exit once the last client that ever connected has gone */
		if (s_had_client && s_nsurf == 0)
			break;

		if (dirty && con.active) {
			g_buf   = (uint8_t*)fb_drawbuf(fb);
			g_pitch = fb_pitch(fb);
			composite();
			fb_flip(fb);
		}
	}

out:
	while (s_nsurf > 0)
		surface_drop(s_nsurf - 1);
	if (lfd >= 0) {
		close(lfd);
		unlink(FBVT_SOCK_PATH);
	}
	if (fb)
		fb_close(fb);
	vtcon_release(&con);
	free(s_wall);
	return rc;
}

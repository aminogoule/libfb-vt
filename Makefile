# libfb-vt -- console framebuffer graphics + display-server seed for FreeBSD 10+
#
# Three interchangeable framebuffer BACKENDS, same fb.h API:
#
#   fb.c       -- linear vt(4) framebuffer (vt_fb / scfb / KMS): FBIOGTYPE+mmap
#                 off the tty. No /dev/mem, no ports. Use on a vtfb0 console.
#   fb_vga.c   -- planar VGA (vt_vga / vtvga0): mode 0x12 640x480x16 via
#                 /dev/mem (0xA0000) + /dev/io (VGA ports). Slow fallback.
#   fb_svga.c  -- VMware SVGA II (PCI 15ad:0405): linear framebuffer + HW 2D
#                 via /dev/pci + /dev/io + /dev/mem (VRAM & FIFO BARs). No X,
#                 no DRM/KMS, no vmwgfx. (fbsvga.h adds modeset/enable/2D.)
#
# Programs (built once per backend, suffixed .fb / .vga / .svga):
#   ppm2fb  -- one-shot: draw an image once
#   fbshow  -- persistent viewer (grabs a free VT)
#   server  -- display-server seed (exclusive VT ownership, render loop)
#
# Binaries are compiled straight from source per variant so the three backends'
# identical fb_open()/... symbols never collide at link time.
#
# Build everything:      make            (== make all)
# Build one backend:     make svga | make vga | make fb
# Requirements: run as root on a vt(4) console (sysctl kern.vty == vt).
#   fb_vga / fb_svga additionally need kern.securelevel <= 0 (writable /dev/mem).

CC?=		cc
CFLAGS?=	-Wall -Wextra -O2
LIBM?=		-lm
LIBUTIL?=	-lutil

PROG_SRC_PPM2FB=	ppm2fb.c ppm.c
PROG_SRC_FBSHOW=	fbshow.c ppm.c
PROG_SRC_SERVER=	server.c vtcon.c mouse.c kbd.c proto.c ppm.c
PROG_SRC_CUBE=		cube.c vtcon.c
PROG_SRC_TERM=		term.c proto.c

HDRS=		fb.h ppm.h vtcon.h fbsvga.h mouse.h kbd.h proto.h fontspleen.h

# fb_svga3.c uses no port I/O, so it also builds on arm64 (the usual SVGA3 case).

# term is a display-server *client*: it links no framebuffer backend (it only
# talks the socket protocol + drives a pty), so it is one backend-independent
# binary, built alongside every backend group.
all: svga svga3 vga fb term

svga:  ppm2fb.svga  fbshow.svga  server.svga  cube.svga
svga3: ppm2fb.svga3 fbshow.svga3 server.svga3 cube.svga3
vga:   ppm2fb.vga   fbshow.vga    server.vga   cube.vga
fb:    ppm2fb.fb    fbshow.fb     server.fb    cube.fb

# ---- terminal client (backend-independent) --------------------------
term: $(PROG_SRC_TERM) proto.h fontspleen.h
	$(CC) $(CFLAGS) -o $@ $(PROG_SRC_TERM) $(LIBUTIL)

# ---- VMware SVGA II (linear + HW 2D) --------------------------------
ppm2fb.svga: $(PROG_SRC_PPM2FB) fb_svga.c $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(PROG_SRC_PPM2FB) fb_svga.c
fbshow.svga: $(PROG_SRC_FBSHOW) fb_svga.c $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(PROG_SRC_FBSHOW) fb_svga.c
server.svga: $(PROG_SRC_SERVER) fb_svga.c $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(PROG_SRC_SERVER) fb_svga.c
cube.svga: $(PROG_SRC_CUBE) fb_svga.c $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(PROG_SRC_CUBE) fb_svga.c $(LIBM)

# ---- VMware SVGA3 (15ad:0406, register MMIO, no port I/O) ------------
ppm2fb.svga3: $(PROG_SRC_PPM2FB) fb_svga3.c $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(PROG_SRC_PPM2FB) fb_svga3.c
fbshow.svga3: $(PROG_SRC_FBSHOW) fb_svga3.c $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(PROG_SRC_FBSHOW) fb_svga3.c
server.svga3: $(PROG_SRC_SERVER) fb_svga3.c $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(PROG_SRC_SERVER) fb_svga3.c
cube.svga3: $(PROG_SRC_CUBE) fb_svga3.c $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(PROG_SRC_CUBE) fb_svga3.c $(LIBM)

# ---- planar VGA (vtvga0) --------------------------------------------
ppm2fb.vga: $(PROG_SRC_PPM2FB) fb_vga.c $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(PROG_SRC_PPM2FB) fb_vga.c
fbshow.vga: $(PROG_SRC_FBSHOW) fb_vga.c $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(PROG_SRC_FBSHOW) fb_vga.c
server.vga: $(PROG_SRC_SERVER) fb_vga.c $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(PROG_SRC_SERVER) fb_vga.c
cube.vga: $(PROG_SRC_CUBE) fb_vga.c $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(PROG_SRC_CUBE) fb_vga.c $(LIBM)

# ---- linear vt_fb / scfb / KMS (original) ---------------------------
ppm2fb.fb: $(PROG_SRC_PPM2FB) fb.c $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(PROG_SRC_PPM2FB) fb.c
fbshow.fb: $(PROG_SRC_FBSHOW) fb.c $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(PROG_SRC_FBSHOW) fb.c
server.fb: $(PROG_SRC_SERVER) fb.c $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(PROG_SRC_SERVER) fb.c
cube.fb: $(PROG_SRC_CUBE) fb.c $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(PROG_SRC_CUBE) fb.c $(LIBM)

clean:
	rm -f ppm2fb.fb ppm2fb.vga ppm2fb.svga ppm2fb.svga3 \
	      fbshow.fb fbshow.vga fbshow.svga fbshow.svga3 \
	      server.fb server.vga server.svga server.svga3 \
	      cube.fb   cube.vga   cube.svga   cube.svga3 \
	      term \
	      *.o

.PHONY: all svga svga3 vga fb clean

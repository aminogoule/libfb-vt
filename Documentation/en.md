# libfb-vt (English summary)

> This is a shorter, less frequently updated mirror of the Russian documentation
> (see [architecture.md](architecture.md), [programs.md](programs.md),
> [backends.md](backends.md), [input.md](input.md),
> [troubleshooting.md](troubleshooting.md)), kept for reference. When in doubt,
> the Russian docs and the code are authoritative.

Console framebuffer graphics and a display-server seed for FreeBSD 10+ (vt(4)).
Pure native FreeBSD interfaces — no `/dev/mem`, no kvm, no DRM/KMS, no linuxkpi.
Only `<sys/fbio.h>`, `<sys/consio.h>`, `<sys/kbio.h>`.

## Layers

- `fb.c` / `fb.h` — framebuffer: open + `mmap` via `FBIOGTYPE`, software double buffer.
- `ppm.c` / `ppm.h` — PPM loader (P6/P3, comments, any maxval) + 32bpp blit/cls.
- `vtcon.c` / `vtcon.h` — native VT/console ownership: `VT_OPENQRY` grab, `KD_GRAPHICS`,
  and the `VT_SETMODE(VT_PROCESS)` switch handshake (the mechanism X uses).
- `mouse.c` / `mouse.h` — mouse pointer: two interchangeable backends selected at
  compile time by CPU architecture (single public interface, `server.c` doesn't know
  which one is compiled in):
  - **sysmouse** (x86/amd64 default) — reads `/dev/sysmouse` (needs `moused(8)` running),
    decodes the sysmouse protocol (level 1, 8-byte packets, falls back to level 0);
  - **evdev** (arm64 default, `__aarch64__`) — reads a stream of `struct input_event`
    records off `/dev/input/eventN`: `EV_REL` (motion + wheel), `EV_KEY` (buttons),
    clamps on `EV_SYN`/`SYN_REPORT`.

  Either backend can be forced regardless of arch with `-DMOUSE_BACKEND_EVDEV` or
  `-DMOUSE_BACKEND_SYSMOUSE`. Buttons 4-7 are not decoded on either path. Both keep
  an absolute, screen-clipped cursor position.

## Programs

- `ppm2fb image.ppm` — one-shot: draw an image once on the current console.
- `fbshow  image.ppm [vt]` — persistent viewer (polling; grabs a free VT).
- `server  image.ppm [vt]` — display-server seed: exclusive VT ownership + render loop.
- `cube    [vt]` — demo: a spinning solid-shaded 3D cube in a 320x200 window
  (same server skeleton; software renderer, works on every backend). Needs `-lm`.
- `glcube  [vt]` — same cube, driven through `mgl.c`/`mgl.h`, a small **software**
  OpenGL-1.x-style immediate-mode layer (`glBegin/glVertex3f/glColor3f`, a real
  MODELVIEW/PROJECTION matrix stack, a Z-buffered Gouraud rasterizer) that draws straight
  into the same `fb_drawbuf()` buffer any other client uses. Pure CPU, no GPU/DRM/KMS/vmwgfx
  — real SVGA3D hardware acceleration is a separate, larger effort planned for later. Needs `-lm`.

`[vt]` is a 1-based VT number (7 == ttyv6); omit or 0 to auto-pick the first free VT.

## Build

    make
    # or without make:
    cc -Wall -O2 -o server server.c vtcon.c ppm.c fb.c

## Run

Requires root, on a vt(4) console (`sysctl kern.vty` must report `vt`).

    sudo ./server ./test.ppm

Quit with `q` / ESC (or SIGINT/SIGTERM). `server` refuses VT switching
(`Alt+Fn`, `Ctrl+Alt+Fn`, `Alt+arrows`) while it runs; see below.

## Notes

- vt(4) owns the video mode; there is no userland VESA mode-setting. The console
  resolution is whatever the kernel/KMS set (`kern.vt.fb.default_mode`).
- vt(4)'s `KD_GRAPHICS` is incomplete; on a dedicated empty VT there is simply no
  text to draw, and the render loop repairs any damage.
- Pixel order assumed `0x00RRGGBB`. If colours look swapped, change the packing
  in `read_ppm()` (ppm.c).
- Quick test image:

      printf 'P6\n2 2\n255\n' > test.ppm && \
      printf '\xff\x00\x00\x00\xff\x00\x00\x00\xff\xff\xff\x00' >> test.ppm

## Backends (added): vt_fb / vt_vga / VMware SVGA II / SVGA3

The framebuffer layer now has four interchangeable backends behind the same
`fb.h` API. Pick one per binary; the Makefile builds all of them, suffixed:

| Backend    | Source       | Console / HW           | Path to pixels                          |
|------------|--------------|------------------------|-----------------------------------------|
| `.fb`      | `fb.c`       | vt_fb / scfb / KMS (vtfb0) | `FBIOGTYPE` + `mmap` off the tty     |
| `.vga`     | `fb_vga.c`   | vt_vga (vtvga0)        | `/dev/mem` 0xA0000 + `/dev/io` (planar 12h) |
| `.svga`    | `fb_svga.c`  | VMware SVGA II (15ad:0405) | `/dev/pci` + `/dev/io` + `/dev/mem` (linear + FIFO 2D) |
| `.svga3`   | `fb_svga3.c` | VMware SVGA3 (15ad:0406) | `/dev/pci` + `/dev/mem` (linear, MMIO registers) |

Which one you need:

    sysctl kern.vty                 # must be 'vt'
    dmesg | grep -E 'VT\(|vgapci'   # VT(vga)=vtvga0, VT(efifb)/fb=vtfb0
    pciconf -lv | grep -i vmware    # 15ad:0405 => SVGA II, 15ad:0406 => SVGA3

- **vtfb0** (linear console already present) -> `.fb`, simplest, no privileges
  beyond opening the tty.
- **vtvga0** (`VT(vga)`) -> `.vga`. Note: if the console is `VT(vga): text 80x25`
  the card is in VGA *text* mode; `fb_vga.c` assumes it has been put into 12h
  graphics. On VMware, prefer the SVGA path below instead.
- **VMware SVGA II** (`15ad:0405`) -> `.svga`: full linear 32bpp framebuffer at
  any resolution plus hardware 2D (`RECT_COPY`/`RECT_FILL` when the device
  advertises the caps, software fallback otherwise), no X, no DRM/KMS, no vmwgfx.
- **VMware SVGA3** (`15ad:0406`) -> `.svga3`: the register-MMIO generation (what
  VMware Fusion gives **arm64** / UEFI guests). Why it needs its own backend:
  - **no I/O ports** — registers live in BAR0 (index `i` = word at byte `i*4`), so
    no `/dev/io` and the backend is **architecture-independent** (builds on arm64);
  - **no legacy FIFO** — 2D and present go through **command buffers**: a 64-byte
    `SVGACBHeader` + command stream placed in the VRAM tail (physical address known,
    device DMAs it), context 0 started at open, header PA poked into
    `SVGA_REG_COMMAND_LOW/HIGH`. Present is `SVGA_CMD_UPDATE` (dirty-rect) and
    `fb_svga_copy` is hardware `SVGA_CMD_RECT_COPY` (gated on `SVGA_CAP_RECT_COPY`;
    ideal for scroll / window drag). SVGA has no HW solid-fill, so `fb_svga_fill` is
    a software fill + present. No `SVGA_CAP_COMMAND_BUFFERS` (or a failed submit) →
    fall back to a `SVGA_REG_SYNC` poke for present and software copy;
  - BAR0 = registers, **BAR2 = VRAM** (SVGA II used BAR1/BAR2); bases and lengths
    come from `PCIOCGETBAR` (64-bit-BAR safe).

### Build

    make            # all backends
    make svga       # just the VMware SVGA II binaries  (15ad:0405)
    make svga3      # just the VMware SVGA3 binaries     (15ad:0406)
    sudo ./server.svga3 ./test.ppm

### Requirements

All backends: root, on a `vt(4)` console. `.vga`, `.svga`, `.svga3` additionally
need `kern.securelevel <= 0` (writable `/dev/mem`). `.svga` uses `/dev/pci`,
`/dev/io`, `/dev/mem`; `.svga3` uses `/dev/pci` and `/dev/mem` (no `/dev/io`).

### VT switching while the server runs

By default `vtcon` now **refuses** VT switches while it owns the console
(`lock_switch = 1`): the kernel's switch request is answered with
`VT_RELDISP(VT_FALSE)`, so every switch hotkey (`Alt+Fn`, `Ctrl+Alt+Fn`,
`Alt+arrows`) and even a foreign `ioctl(VT_ACTIVATE)` is ignored, and the user
stays on our VT until the server quits (`q` / ESC / SIGINT). This is what you want
with the SVGA backend: leaving the VT while `SVGA_REG_ENABLE` is set left the device
owning the screen under another console and crashed/wedged the display; refusing
the switch removes that state entirely. Because the refusal rides on
`VT_PROCESS`, process death still triggers the kernel's `KD_TEXT` + `VT_AUTO`
reset -- unlike `VT_LOCKSWITCH`, a crash can't lock you out of every VT.

No `server.c` change is required; the default just works.

Note: the switch block is at the kernel switch routine (via `VT_PROCESS`), not at
a specific key, so it does not depend on keyboard mode. A separate, lower-level
way to stop switching is to take the keyboard out of `K_XLATE` into `K_RAW` /
`K_CODE` (`ioctl(fd, KDSKBMODE, K_CODE)`): raw scancodes then bypass the keymap's
switch actions entirely. That is the path for real raw keyboard input, but
beware: in raw/code mode the tty no longer cooks input, so `Ctrl+C` (SIGINT) and
the translated `q`/`ESC` quit stop working -- you must decode scancodes yourself
for quit, or you can get stuck (switching is also blocked). For now the library
stays in `K_XLATE` so `q`/`ESC` keep working; the `VT_PROCESS` deny already covers
every switch hotkey.

If you deliberately want cooperative switching back (allow `Ctrl+Alt+Fn`), call
`vtcon_set_switch_lock(&con, 0)` after `vtcon_acquire()`, and then, on the SVGA
backend, hand the screen back to the text console on `con.active` transitions:

    #include "fbsvga.h"
    vtcon_set_switch_lock(&con, 0);        /* re-enable cooperative switching */
    /* ... inside the render loop, after vtcon_pump(&con): */
    static int was_active = 1;
    if (con.active != was_active) {
        fb_svga_enable(fb, con.active);    /* 0 -> vt text; 1 -> our frame */
        was_active = con.active;
    }

`fb_close()` already drops `SVGA_REG_ENABLE`, so a clean exit restores the text
console regardless.

### SVGA II: bring-up order (recommended first test)

1. `fb_svga_open(1024,768,32,0)` then `fb_svga_fill(fb,0,0,1024,768,0x0000FF)`
   -> a solid blue screen validates PCI discovery, ports, both mmaps, mode set
   and FIFO+UPDATE in one shot.
2. `ppm2fb.svga image.ppm` (single/double buffered).
3. `server.svga` with the VT-cooperation patch above.

### Constants to verify against VMware's `svga_reg.h`

`fb_svga.c` marks with `[VERIFY]` the legacy 2D command numbers
(`SVGA_CMD_RECT_FILL`/`RECT_COPY`) and cap bits (`SVGA_CAP_RECT_FILL`/`COPY`).
Getting these wrong only disables hardware 2D (there is a software fallback); the
core path (ID handshake -> WIDTH/HEIGHT/BPP -> ENABLE -> draw to VRAM -> UPDATE)
uses only stable, well-known register values.

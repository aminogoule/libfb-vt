/*
 * term -- a graphical terminal emulator, as a client of the libfb-vt
 * compositor (server.c). No VT/framebuffer of its own: it talks the proto.h
 * protocol over the AF_UNIX socket, gets a shared-memory pixel buffer for its
 * window, and renders a text grid into it with the embedded 8x16 font.
 *
 *   compositor <--socket--> term
 *      owns VT              forks a shell in a pty (openpty/forkpty, -lutil)
 *      composites windows   emulates a VT100-ish terminal into the shm buffer
 *      routes keyboard  -->  writes those bytes to the pty master
 *
 * Emulation is a practical subset: printable text with wrap/scroll, the usual
 * C0 controls (CR/LF/BS/TAB/BEL), CSI cursor moves + erase (A B C D H f G d
 * J K), SGR colours (SGR 0/1/7/30-37/40-47/90-97/100-107/39/49), DEC ?25
 * cursor show/hide, and OSC 0/2 window titles. Enough to run an interactive
 * shell; not a full xterm.
 *
 * Usage: term [rows] [cols]      (defaults 25x80, clamped to the granted size)
 * Run wherever it can reach FBVT_SOCK_PATH (root, same host as the server).
 */

#include "proto.h"
#include "fontspleen.h"
#include "mouse.h"             /* MOUSE_BTN_* bit values (server<->client ABI) */

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <libutil.h>          /* forkpty() -- link with -lutil */
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sysexits.h>

#define GLYPH_W 8
#define GLYPH_H 16
#define DEF_ROWS 25
#define DEF_COLS 80
#define MAX_PARAMS 16

#define DEF_FG 0xC8CCD4u
#define DEF_BG 0x0C0E14u

/* 16-colour ANSI palette (0x00RRGGBB): 0-7 normal, 8-15 bright. */
static const uint32_t PAL[16] = {
	0x1B1D23, 0xCD3131, 0x2FA33B, 0xC7B12A,
	0x3465A4, 0xB048B0, 0x2AA1A8, 0xC8CCD4,
	0x545862, 0xF05050, 0x5FD75F, 0xF0E060,
	0x5F87D7, 0xE070E0, 0x5FD7D7, 0xFFFFFF,
};

typedef struct {
	uint32_t      cp;         /* Unicode code point in this cell */
	uint32_t      fg, bg;
} cell_t;

typedef struct {
	/* IPC / buffer */
	int       sock;
	uint32_t  surf_id;
	uint32_t* pix;            /* mmap'd shared window buffer */
	size_t    map_size;
	int       pw, ph;         /* pixel width/height of the buffer */
	size_t    stride;         /* bytes per row of the buffer */

	/* text grid */
	int       cols, rows;
	cell_t*   grid;           /* cols*rows cells */
	int       cx, cy;         /* cursor column/row */
	uint32_t  cur_fg, cur_bg;
	int       bold, reverse;
	int       cursor_vis;
	int       saved_cx, saved_cy;
	int       scroll_top, scroll_bot;  /* DECSTBM margins, 0-based inclusive */
	int       app_cursor_keys;         /* DECCKM: 1 => arrows report ESC O x */

	/* xterm-style mouse reporting (DECSET 1000/1002/1006) */
	int       mouse_mode;     /* 0 = off, 1000 = click, 1002 = click+drag */
	int       mouse_sgr;      /* 1 => SGR (1006) coordinate encoding      */
	int       mouse_buttons;  /* last reported button bitmask, MOUSE_BTN_* */

	/* pty */
	int       pty;            /* master fd */
	pid_t     child;

	/* keyboard -> pty escape rewriter (see handle_key_byte) */
	int           kin_state;   /* KIN_* */
	unsigned char kin_buf[8];  /* CSI intermediates/params buffered so far */
	int           kin_len;

	/* escape parser */
	int       state;          /* S_* */
	int       params[MAX_PARAMS];
	int       nparam;
	int       priv;           /* CSI '?' seen */
	char      osc[FBVT_MAX_PAYLOAD + 1];
	int       osclen;

	/* UTF-8 decoder (text path only) */
	int       u8_need;        /* continuation bytes still expected */
	uint32_t  u8_cp;          /* code point being assembled */
	uint32_t  u8_min;         /* smallest legal cp (overlong check) */

	int       dirty;
} term_t;

enum { S_NORM, S_ESC, S_CSI, S_OSC, S_OSC_ESC, S_CHARSET };
enum { KIN_NORM, KIN_ESC, KIN_CSI };

/* set from $TERM_DEBUG at startup; traces executed CSI sequences to stderr
   so scroll/margin issues in real apps (vim, mc) can be diagnosed without
   guessing. */
static int g_debug;

/* ------------------------------------------------------------------ *
 * Grid helpers.                                                       *
 * ------------------------------------------------------------------ */
static void cell_clear(term_t* t, cell_t* c) {
	c->cp = ' ';
	c->fg = t->cur_fg;
	c->bg = t->cur_bg;
}

static void grid_clear(term_t* t) {
	int i, n = t->cols * t->rows;
	for (i = 0; i < n; i++)
		cell_clear(t, &t->grid[i]);
}

/* erase cells [from,to) in the linear grid -- forward-declared here, defined
   below param(); needed by the scroll helpers. */
static void erase_span(term_t* t, int from, int to);

/* Scroll rows [top,bot] (0-based, inclusive) of the grid up/down by n lines,
   pulling in blank lines at the vacated edge. This is the primitive behind
   LF/RI at the scroll margins and the DECSTBM-aware CSI L/M/S/T sequences --
   vim and mc both rely on the terminal actually performing these (rather
   than silently ignoring them) to keep their cursor bookkeeping in sync with
   what's on screen. */
static void region_scroll_up(term_t* t, int top, int bot, int n) {
	int span = bot - top + 1;
	int i;
	if (n <= 0 || span <= 0) return;
	if (n > span) n = span;
	if (n < span)
		memmove(&t->grid[top * t->cols], &t->grid[(top + n) * t->cols],
		        (size_t)(span - n) * t->cols * sizeof(cell_t));
	for (i = bot - n + 1; i <= bot; i++)
		erase_span(t, i * t->cols, (i + 1) * t->cols);
}

static void region_scroll_down(term_t* t, int top, int bot, int n) {
	int span = bot - top + 1;
	int i;
	if (n <= 0 || span <= 0) return;
	if (n > span) n = span;
	if (n < span)
		memmove(&t->grid[(top + n) * t->cols], &t->grid[top * t->cols],
		        (size_t)(span - n) * t->cols * sizeof(cell_t));
	for (i = top; i < top + n; i++)
		erase_span(t, i * t->cols, (i + 1) * t->cols);
}

static void newline(term_t* t) {
	if (t->cy == t->scroll_bot)
		region_scroll_up(t, t->scroll_top, t->scroll_bot, 1);
	else if (t->cy < t->rows - 1)
		t->cy++;
}

static void put_char(term_t* t, uint32_t cp) {
	cell_t* c;
	if (t->cx >= t->cols) {          /* deferred wrap */
		t->cx = 0;
		newline(t);
	}
	c = &t->grid[t->cy * t->cols + t->cx];
	c->cp = cp;
	c->fg = t->reverse ? t->cur_bg : t->cur_fg;
	c->bg = t->reverse ? t->cur_fg : t->cur_bg;
	if (t->bold && !t->reverse)
		c->fg |= 0x808080;           /* cheap "brighten" for bold */
	t->cx++;
}

/* ------------------------------------------------------------------ *
 * CSI / SGR execution.                                                *
 * ------------------------------------------------------------------ */
static int param(term_t* t, int i, int dflt) {
	if (i >= t->nparam || t->params[i] == 0)   /* absent or 0 => default */
		return dflt;
	return t->params[i];
}

/* erase cells [from,to) in the linear grid */
static void erase_span(term_t* t, int from, int to) {
	int i;
	if (from < 0) from = 0;
	if (to > t->cols * t->rows) to = t->cols * t->rows;
	for (i = from; i < to; i++)
		cell_clear(t, &t->grid[i]);
}

static void sgr(term_t* t) {
	int i;
	if (t->nparam == 0) {                 /* bare CSI m == reset */
		t->cur_fg = DEF_FG; t->cur_bg = DEF_BG;
		t->bold = t->reverse = 0;
		return;
	}
	for (i = 0; i < t->nparam; i++) {
		int p = t->params[i];
		if (p == 0) { t->cur_fg = DEF_FG; t->cur_bg = DEF_BG;
		              t->bold = t->reverse = 0; }
		else if (p == 1)  t->bold = 1;
		else if (p == 7)  t->reverse = 1;
		else if (p == 22) t->bold = 0;
		else if (p == 27) t->reverse = 0;
		else if (p >= 30 && p <= 37)   t->cur_fg = PAL[p - 30];
		else if (p == 39)              t->cur_fg = DEF_FG;
		else if (p >= 40 && p <= 47)   t->cur_bg = PAL[p - 40];
		else if (p == 49)              t->cur_bg = DEF_BG;
		else if (p >= 90 && p <= 97)   t->cur_fg = PAL[8 + p - 90];
		else if (p >= 100 && p <= 107) t->cur_bg = PAL[8 + p - 100];
	}
}

/* DECSET (on=1) / DECRST (on=0) for the private modes we support: cursor
   visibility (25) and xterm mouse reporting (1000/1002 click modes, 1006 SGR
   coordinate encoding). A single CSI can carry several params (e.g.
   "\033[?1000;1006h"), so this walks all of them. */
static void dec_mode(term_t* t, int on) {
	int i, n = t->nparam ? t->nparam : 1;
	for (i = 0; i < n; i++) {
		int p = param(t, i, 0);
		switch (p) {
		case 1:    t->app_cursor_keys = on; break;   /* DECCKM */
		case 25:   t->cursor_vis = on; break;
		case 1000:
		case 1002: t->mouse_mode = on ? p : 0; break;
		case 1006: t->mouse_sgr  = on; break;
		default: break;
		}
	}
}

static void csi_exec(term_t* t, unsigned char final) {
	int p0 = param(t, 0, 1);
	int p1 = param(t, 1, 1);

	if (g_debug) {
		int i;
		fprintf(stderr, "csi %sfinal=%c params=[",
		        t->priv ? "? " : "", final);
		for (i = 0; i < t->nparam; i++)
			fprintf(stderr, "%s%d", i ? ";" : "", t->params[i]);
		fprintf(stderr, "] cy=%d cx=%d top=%d bot=%d\n",
		        t->cy, t->cx, t->scroll_top, t->scroll_bot);
	}

	switch (final) {
	case 'A': t->cy -= p0; if (t->cy < 0) t->cy = 0; break;
	case 'B': t->cy += p0; if (t->cy >= t->rows) t->cy = t->rows - 1; break;
	case 'C': t->cx += p0; if (t->cx >= t->cols) t->cx = t->cols - 1; break;
	case 'D': t->cx -= p0; if (t->cx < 0) t->cx = 0; break;
	case 'G': t->cx = p0 - 1; break;
	case 'd': t->cy = p0 - 1; break;
	case 'H':
	case 'f': t->cy = p0 - 1; t->cx = p1 - 1; break;
	case 'J':
		switch (param(t, 0, 0)) {
		case 0: erase_span(t, t->cy * t->cols + t->cx, t->cols * t->rows); break;
		case 1: erase_span(t, 0, t->cy * t->cols + t->cx + 1); break;
		case 2: erase_span(t, 0, t->cols * t->rows); break;
		}
		break;
	case 'K': {
		int row = t->cy * t->cols;
		switch (param(t, 0, 0)) {
		case 0: erase_span(t, row + t->cx, row + t->cols); break;
		case 1: erase_span(t, row, row + t->cx + 1); break;
		case 2: erase_span(t, row, row + t->cols); break;
		}
		break;
	}
	case 'h': if (t->priv) dec_mode(t, 1); break;
	case 'l': if (t->priv) dec_mode(t, 0); break;
	case 's': t->saved_cx = t->cx; t->saved_cy = t->cy; break;
	case 'u': t->cx = t->saved_cx; t->cy = t->saved_cy; break;
	case 'm': sgr(t); break;
	case 'r': {                           /* DECSTBM: set scroll margins */
		int top = param(t, 0, 1) - 1;
		int bot = param(t, 1, t->rows) - 1;
		if (top < 0) top = 0;
		if (bot >= t->rows) bot = t->rows - 1;
		if (top < bot) { t->scroll_top = top; t->scroll_bot = bot; }
		else            { t->scroll_top = 0;  t->scroll_bot = t->rows - 1; }
		t->cx = 0; t->cy = t->scroll_top;      /* DECSTBM homes the cursor */
		break;
	}
	case 'L':                             /* IL: insert Ps blank lines */
		if (t->cy >= t->scroll_top && t->cy <= t->scroll_bot)
			region_scroll_down(t, t->cy, t->scroll_bot, p0);
		break;
	case 'M':                             /* DL: delete Ps lines */
		if (t->cy >= t->scroll_top && t->cy <= t->scroll_bot)
			region_scroll_up(t, t->cy, t->scroll_bot, p0);
		break;
	case 'S': region_scroll_up(t, t->scroll_top, t->scroll_bot, p0); break;
	case 'T': region_scroll_down(t, t->scroll_top, t->scroll_bot, p0); break;
	default: break;                       /* unsupported: ignore */
	}
	if (t->cx < 0) t->cx = 0;
	if (t->cy < 0) t->cy = 0;
	if (t->cx >= t->cols) t->cx = t->cols - 1;
	if (t->cy >= t->rows) t->cy = t->rows - 1;
}

/* ------------------------------------------------------------------ *
 * Byte feed / escape state machine.                                   *
 * ------------------------------------------------------------------ */
static void set_title(term_t* t, const char* s) {
	struct fbvt_msg m;
	size_t n = strlen(s);
	if (n > FBVT_MAX_PAYLOAD) n = FBVT_MAX_PAYLOAD;
	memset(&m, 0, sizeof(m));
	m.type   = FBVT_SET_TITLE;
	m.id     = t->surf_id;
	m.paylen = (uint32_t)n;
	fbvt_send(t->sock, &m, s);
}

static void osc_finish(term_t* t) {
	/* OSC "0;title" or "2;title" set the window title */
	t->osc[t->osclen] = '\0';
	if ((t->osc[0] == '0' || t->osc[0] == '2') && t->osc[1] == ';')
		set_title(t, t->osc + 2);
}

static void feed(term_t* t, unsigned char ch) {
	/*
	 * UTF-8 assembly applies only to printable text (state S_NORM): escape and
	 * C0 control bytes are all < 0x80 and go straight to the state machine, so
	 * a multibyte sequence can only appear between them. Assemble lead +
	 * continuation bytes into one code point, then emit a single cell.
	 */
	if (t->state == S_NORM) {
		if (t->u8_need > 0) {
			if ((ch & 0xC0) == 0x80) {           /* valid continuation */
				t->u8_cp = (t->u8_cp << 6) | (ch & 0x3F);
				if (--t->u8_need == 0) {
					uint32_t cp = t->u8_cp;
					if (cp < t->u8_min || cp > 0x10FFFF ||
					    (cp >= 0xD800 && cp <= 0xDFFF))
						cp = 0xFFFD;             /* overlong/surrogate/OOR */
					put_char(t, cp);
					t->dirty = 1;
				}
				return;
			}
			t->u8_need = 0;                      /* bad seq: drop, reparse ch */
			put_char(t, 0xFFFD);
			t->dirty = 1;
		}
		if (ch >= 0x80) {                        /* a UTF-8 lead byte */
			if ((ch & 0xE0) == 0xC0) { t->u8_need = 1; t->u8_cp = ch & 0x1F;
			                           t->u8_min = 0x80; }
			else if ((ch & 0xF0) == 0xE0) { t->u8_need = 2; t->u8_cp = ch & 0x0F;
			                                t->u8_min = 0x800; }
			else if ((ch & 0xF8) == 0xF0) { t->u8_need = 3; t->u8_cp = ch & 0x07;
			                                t->u8_min = 0x10000; }
			else { put_char(t, 0xFFFD); t->dirty = 1; }   /* stray byte */
			return;
		}
		/* ch < 0x80: fall into the normal control/printable handling below */
	}

	switch (t->state) {
	case S_NORM:
		switch (ch) {
		case 0x1B: t->state = S_ESC; break;
		case '\r': t->cx = 0; break;
		case '\n': newline(t); break;
		case '\b': if (t->cx > 0) t->cx--; break;
		case '\t': t->cx = (t->cx + 8) & ~7;
		           if (t->cx >= t->cols) t->cx = t->cols - 1; break;
		case '\a': break;                 /* bell: ignore */
		case 0x0E: case 0x0F: break;      /* shift out/in: ignore */
		default:
			if (ch >= 0x20)
				put_char(t, ch);
			break;
		}
		break;

	case S_ESC:
		switch (ch) {
		case '[': t->state = S_CSI; t->nparam = 0; t->priv = 0;
		          memset(t->params, 0, sizeof(t->params)); break;
		case ']': t->state = S_OSC; t->osclen = 0; break;
		case '(': case ')': t->state = S_CHARSET; break;
		case 'M':                          /* reverse index: up + scroll */
			if (t->cy == t->scroll_top)
				region_scroll_down(t, t->scroll_top, t->scroll_bot, 1);
			else if (t->cy > 0)
				t->cy--;
			t->state = S_NORM; break;
		case 'c':                          /* RIS: full reset */
			t->cur_fg = DEF_FG; t->cur_bg = DEF_BG; t->bold = t->reverse = 0;
			t->cx = t->cy = 0; t->cursor_vis = 1; grid_clear(t);
			t->scroll_top = 0; t->scroll_bot = t->rows - 1;
			t->state = S_NORM; break;
		default: t->state = S_NORM; break;
		}
		break;

	case S_CSI:
		if (ch == '?') { t->priv = 1; }
		else if (ch >= '0' && ch <= '9') {
			if (t->nparam == 0) t->nparam = 1;
			if (t->nparam <= MAX_PARAMS)
				t->params[t->nparam - 1] =
					t->params[t->nparam - 1] * 10 + (ch - '0');
		}
		else if (ch == ';') {
			if (t->nparam == 0) t->nparam = 1;
			if (t->nparam < MAX_PARAMS) t->nparam++;
		}
		else if (ch >= 0x40 && ch <= 0x7E) {
			csi_exec(t, ch);
			t->state = S_NORM;
		}
		/* other intermediates (space, '!', etc.): stay until final */
		break;

	case S_OSC:
		if (ch == 0x07) { osc_finish(t); t->state = S_NORM; }
		else if (ch == 0x1B) t->state = S_OSC_ESC;
		else if (t->osclen < (int)sizeof(t->osc) - 1) t->osc[t->osclen++] = ch;
		break;

	case S_OSC_ESC:
		/* ST is ESC '\' ; anything else aborts the OSC */
		if (ch == '\\') osc_finish(t);
		t->state = S_NORM;
		break;

	case S_CHARSET:
		t->state = S_NORM;                 /* consume the charset designator */
		break;
	}
	t->dirty = 1;
}

/* ------------------------------------------------------------------ *
 * xterm-style mouse reporting -> pty.                                 *
 * ------------------------------------------------------------------ */

/* Encode and send one button/motion report. btn is the xterm button number
   (0=left,1=middle,2=right,4=wheel-up,5=wheel-down); motion adds the xterm
   "+32" convention for button-event-tracking drags. col/row are 0-based. */
static void report_button(term_t* t, int btn, int pressed, int motion,
                           int col, int row) {
	char buf[32];
	int  n, cb, cx, cy;

	cb = btn + (motion ? 32 : 0);
	cx = col + 1; if (cx > 223) cx = 223; if (cx < 1) cx = 1;
	cy = row + 1; if (cy > 223) cy = 223; if (cy < 1) cy = 1;

	if (t->mouse_sgr) {
		n = snprintf(buf, sizeof(buf), "\033[<%d;%d;%d%c",
		             cb, cx, cy, pressed ? 'M' : 'm');
		if (n > 0 && n < (int)sizeof(buf))
			(void)write(t->pty, buf, (size_t)n);
		return;
	}
	/* legacy X10 encoding: fixed 6 bytes, release is always reported as
	   button 3 (the protocol has no per-button release code). */
	buf[0] = 0x1B; buf[1] = '['; buf[2] = 'M';
	buf[3] = (char)(32 + (pressed ? cb : 3));
	buf[4] = (char)(32 + cx);
	buf[5] = (char)(32 + cy);
	(void)write(t->pty, buf, 6);
}

/* Translate a compositor FBVT_INPUT_MOUSE (pixel coords + button bitmask)
   into xterm mouse-reporting escape sequences on the pty, if the app running
   in this terminal has asked for them via DECSET 1000/1002. */
static void handle_mouse_msg(term_t* t, const struct fbvt_msg* m) {
	static const int BTN_BIT[3] = { MOUSE_BTN_LEFT, MOUSE_BTN_MIDDLE,
	                                 MOUSE_BTN_RIGHT };
	int buttons = m->arg[2];
	int dz      = m->arg[3];
	int col     = m->arg[0] / GLYPH_W;
	int row     = m->arg[1] / GLYPH_H;
	int changed = buttons ^ t->mouse_buttons;
	int i;

	if (t->mouse_mode == 0) {
		t->mouse_buttons = buttons;
		return;
	}
	if (col < 0) col = 0;
	if (row < 0) row = 0;

	if (dz > 0) report_button(t, 4, 1, 0, col, row);   /* wheel up   */
	if (dz < 0) report_button(t, 5, 1, 0, col, row);   /* wheel down */

	for (i = 0; i < 3; i++) {
		if (changed & BTN_BIT[i])
			report_button(t, i, (buttons & BTN_BIT[i]) != 0, 0, col, row);
	}
	if (!changed && buttons != 0 && t->mouse_mode == 1002) {
		for (i = 0; i < 3; i++) {           /* motion while dragging */
			if (buttons & BTN_BIT[i]) {
				report_button(t, i, 1, 1, col, row);
				break;
			}
		}
	}
	t->mouse_buttons = buttons;
}

/* ------------------------------------------------------------------ *
 * Keyboard -> pty: rewrite cursor-key escapes for DECCKM.             *
 *                                                                      *
 * The raw bytes forwarded by the compositor are whatever the vt(4)    *
 * console keymap emits, which is always the ANSI/normal form          *
 * (ESC [ A/B/C/D/H/F). But curses apps built against the "vt100"      *
 * terminfo entry define arrow keys as the SS3 application form        *
 * (ESC O A/B/C/D/H/F) and switch modes with DECCKM (CSI ?1h / ?1l).   *
 * Since we never actually retarget the keyboard hardware, we rewrite  *
 * the outgoing bytes here instead: bare (parameter-less) cursor-key   *
 * sequences get their '[' swapped for 'O' while app_cursor_keys is    *
 * set; everything else (Home/End with modifiers, PgUp/PgDn, function  *
 * keys, ordinary text) passes through untouched.                      *
 * ------------------------------------------------------------------ */
static void flush_raw(term_t* t, const unsigned char* buf, size_t n) {
	(void)write(t->pty, buf, n);
}

/* Flush whatever is currently buffered in the keyboard parser as-is (used
   on a genuine standalone ESC, or when a sequence times out unterminated).
   Returns the parser to KIN_NORM. */
static void kin_flush_pending(term_t* t) {
	unsigned char esc = 0x1B;
	if (t->kin_state == KIN_NORM)
		return;
	flush_raw(t, &esc, 1);
	if (t->kin_state == KIN_CSI) {
		unsigned char lb = '[';
		flush_raw(t, &lb, 1);
		if (t->kin_len)
			flush_raw(t, t->kin_buf, (size_t)t->kin_len);
	}
	t->kin_state = KIN_NORM;
	t->kin_len   = 0;
}

static void handle_key_byte(term_t* t, unsigned char b) {
	switch (t->kin_state) {
	case KIN_NORM:
		if (b == 0x1B) { t->kin_state = KIN_ESC; return; }
		flush_raw(t, &b, 1);
		return;

	case KIN_ESC:
		if (b == '[') {
			t->kin_state = KIN_CSI;
			t->kin_len   = 0;
			return;
		}
		/* not a CSI intro (e.g. a real standalone Escape keypress
		   immediately followed by ordinary text): flush both bytes as-is */
		kin_flush_pending(t);
		flush_raw(t, &b, 1);
		return;

	case KIN_CSI:
		if (b >= 0x40 && b <= 0x7E) {        /* final byte: sequence done */
			if (t->app_cursor_keys && t->kin_len == 0 &&
			    (b == 'A' || b == 'B' || b == 'C' || b == 'D' ||
			     b == 'H' || b == 'F')) {
				unsigned char seq[3] = { 0x1B, 'O', b };
				flush_raw(t, seq, 3);
			} else {
				unsigned char pre[2] = { 0x1B, '[' };
				flush_raw(t, pre, 2);
				if (t->kin_len)
					flush_raw(t, t->kin_buf, (size_t)t->kin_len);
				flush_raw(t, &b, 1);
			}
			t->kin_state = KIN_NORM;
			t->kin_len   = 0;
			return;
		}
		if (t->kin_len < (int)sizeof(t->kin_buf))
			t->kin_buf[t->kin_len++] = b;
		return;
	}
}

/* ------------------------------------------------------------------ *
 * Rendering: grid -> shm buffer, then COMMIT.                         *
 * ------------------------------------------------------------------ */
static void render(term_t* t) {
	int row, col, gy, gx;

	for (row = 0; row < t->rows; row++) {
		for (col = 0; col < t->cols; col++) {
			cell_t*  c   = &t->grid[row * t->cols + col];
			int      cur = t->cursor_vis && row == t->cy && col == t->cx;
			uint32_t fg  = cur ? c->bg : c->fg;
			uint32_t bg  = cur ? c->fg : c->bg;
			const unsigned char* g = glyph_for(c->cp);
			int px0 = col * GLYPH_W, py0 = row * GLYPH_H;

			for (gy = 0; gy < GLYPH_H; gy++) {
				uint32_t* line = (uint32_t*)((uint8_t*)t->pix +
				                 (size_t)(py0 + gy) * t->stride) + px0;
				for (gx = 0; gx < GLYPH_W; gx++)
					line[gx] = (g[gy] & (1u << gx)) ? fg : bg;
			}
		}
	}
}

static void commit(term_t* t) {
	struct fbvt_msg m;
	memset(&m, 0, sizeof(m));
	m.type   = FBVT_COMMIT;
	m.id     = t->surf_id;
	m.arg[0] = 0; m.arg[1] = 0; m.arg[2] = t->pw; m.arg[3] = t->ph;
	fbvt_send(t->sock, &m, NULL);
}

/* ------------------------------------------------------------------ *
 * Setup.                                                              *
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

/* HELLO handshake + CREATE_SURFACE; blocks for SURFACE_CREATED and maps the
   shared buffer. Returns 0/-1. */
static int handshake(term_t* t, int want_cols, int want_rows) {
	struct fbvt_msg m;
	int             passfd = -1;

	memset(&m, 0, sizeof(m));
	m.type = FBVT_HELLO; m.arg[0] = FBVT_PROTO_VERSION;
	if (fbvt_send(t->sock, &m, NULL) != 0)
		return -1;
	if (fbvt_recv(t->sock, &m, NULL, 0, NULL) != 1 || m.type != FBVT_HELLO_ACK)
		return -1;

	memset(&m, 0, sizeof(m));
	m.type = FBVT_CREATE_SURFACE;
	m.arg[0] = want_cols * GLYPH_W;
	m.arg[1] = want_rows * GLYPH_H;
	if (fbvt_send(t->sock, &m, NULL) != 0)
		return -1;

	if (fbvt_recv(t->sock, &m, NULL, 0, &passfd) != 1 ||
	    m.type != FBVT_SURFACE_CREATED || passfd < 0)
		return -1;

	t->surf_id = m.id;
	t->pw      = m.arg[0];
	t->ph      = m.arg[1];
	t->stride  = (size_t)m.arg[2];
	t->map_size = t->stride * (size_t)t->ph;
	t->pix = mmap(NULL, t->map_size, PROT_READ | PROT_WRITE,
	              MAP_SHARED, passfd, 0);
	close(passfd);
	if (t->pix == MAP_FAILED)
		return -1;

	t->cols = t->pw / GLYPH_W;
	t->rows = t->ph / GLYPH_H;
	return 0;
}

static int spawn_shell(term_t* t) {
	struct winsize ws;
	struct termios tio;
	int            master;
	pid_t          pid;

	memset(&ws, 0, sizeof(ws));
	ws.ws_col = (unsigned short)t->cols;
	ws.ws_row = (unsigned short)t->rows;
	ws.ws_xpixel = (unsigned short)t->pw;
	ws.ws_ypixel = (unsigned short)t->ph;

	pid = forkpty(&master, NULL, NULL, &ws);
	if (pid < 0)
		return -1;
	if (pid == 0) {                       /* child: the shell */
		const char* sh = getenv("SHELL");
		if (sh == NULL || *sh == '\0') sh = "/bin/sh";
		setenv("TERM", "vt100", 1);
		unsetenv("LINES");
		unsetenv("COLUMNS");
		execl(sh, sh, "-i", (char*)NULL);
		_exit(127);
	}

	/* parent */
	if (tcgetattr(master, &tio) == 0) {   /* be tolerant of odd defaults */
		tio.c_iflag |= ICRNL;
		tcsetattr(master, TCSANOW, &tio);
	}
	fcntl(master, F_SETFL, fcntl(master, F_GETFL, 0) | O_NONBLOCK);
	t->pty   = master;
	t->child = pid;
	return 0;
}

int main(int argc, char* argv[]) {
	term_t t;
	int    want_rows = DEF_ROWS, want_cols = DEF_COLS;
	int    rc = EX_OK;

	if (argc >= 2) want_rows = atoi(argv[1]);
	if (argc >= 3) want_cols = atoi(argv[2]);
	if (want_rows < 1) want_rows = DEF_ROWS;
	if (want_cols < 1) want_cols = DEF_COLS;

	signal(SIGPIPE, SIG_IGN);
	signal(SIGCHLD, SIG_IGN);              /* auto-reap the shell */

#ifdef TERM_FORCE_DEBUG
	g_debug = 1;                       /* baked in by `make term-debug` */
#else
	g_debug = (getenv("TERM_DEBUG") != NULL);
#endif

	memset(&t, 0, sizeof(t));
	t.sock = t.pty = -1;
	t.cur_fg = DEF_FG; t.cur_bg = DEF_BG;
	t.cursor_vis = 1;
	t.state = S_NORM;

	if ((t.sock = connect_server()) < 0) {
		fprintf(stderr, "term: cannot connect to %s: %s\n",
		        FBVT_SOCK_PATH, strerror(errno));
		return EX_UNAVAILABLE;
	}
	if (handshake(&t, want_cols, want_rows) != 0) {
		fprintf(stderr, "term: handshake failed: %s\n", strerror(errno));
		rc = EX_PROTOCOL;
		goto out;
	}

	t.grid = calloc((size_t)t.cols * t.rows, sizeof(cell_t));
	if (t.grid == NULL) { rc = EX_OSERR; goto out; }
	t.scroll_top = 0;
	t.scroll_bot = t.rows - 1;
	grid_clear(&t);

	if (spawn_shell(&t) != 0) {
		fprintf(stderr, "term: forkpty: %s\n", strerror(errno));
		rc = EX_OSERR;
		goto out;
	}

	set_title(&t, "term");
	render(&t);
	commit(&t);

	for (;;) {
		struct pollfd pfd[2];
		int           n;

		pfd[0].fd = t.sock; pfd[0].events = POLLIN; pfd[0].revents = 0;
		pfd[1].fd = t.pty;  pfd[1].events = POLLIN; pfd[1].revents = 0;

		n = poll(pfd, 2, 16);
		if (n < 0 && errno != EINTR)
			break;
		if (n == 0)                       /* idle tick: don't leave a real */
			kin_flush_pending(&t);        /* Escape/truncated CSI hanging  */

		/* input keys from the compositor -> the pty */
		if (pfd[0].revents & (POLLIN | POLLHUP | POLLERR)) {
			struct fbvt_msg m;
			int             r = fbvt_recv(t.sock, &m, NULL, 0, NULL);
			if (r <= 0)
				break;                     /* compositor gone */
			if (m.type == FBVT_INPUT_KEY) {
				unsigned char b = (unsigned char)m.arg[0];
				handle_key_byte(&t, b);
			} else if (m.type == FBVT_INPUT_MOUSE) {
				handle_mouse_msg(&t, &m);
			} else if (m.type == FBVT_CLOSE) {
				break;
			}
		}

		/* shell output -> the emulator */
		if (pfd[1].revents & (POLLIN | POLLHUP | POLLERR)) {
			unsigned char buf[4096];
			ssize_t       r = read(t.pty, buf, sizeof(buf));
			if (r > 0) {
				ssize_t i;
				for (i = 0; i < r; i++)
					feed(&t, buf[i]);
			} else if (r == 0 || (r < 0 && errno != EAGAIN &&
			                      errno != EINTR)) {
				break;                     /* shell exited */
			}
		}

		if (t.dirty) {
			render(&t);
			commit(&t);
			t.dirty = 0;
		}
	}

	/* tell the compositor our surface is going away */
	if (t.surf_id) {
		struct fbvt_msg m;
		memset(&m, 0, sizeof(m));
		m.type = FBVT_DESTROY_SURFACE;
		m.id   = t.surf_id;
		fbvt_send(t.sock, &m, NULL);
	}

out:
	if (t.pty >= 0)  close(t.pty);
	if (t.pix && t.pix != MAP_FAILED) munmap(t.pix, t.map_size);
	if (t.sock >= 0) close(t.sock);
	free(t.grid);
	return rc;
}

#ifndef KBD_H
#define KBD_H

/*
 * kbd -- decodes FreeBSD vt(4) K_CODE raw scancodes (see
 * vtcon_set_keyboard_raw()) into terminal input bytes, tracking real
 * Shift/Ctrl/Alt modifier state that K_XLATE's pre-baked keymap translation
 * throws away. That state is what lets modifier combos K_XLATE can't
 * express -- Shift+Arrow (xterm "ESC[1;2A"), Ctrl+Arrow, etc. -- reach an
 * application at all.
 *
 * Letters/digits/punctuation/Enter/Backspace/Esc/Tab/F1-F12 are the standard
 * AT/PS2 "set 1" layout, US QWERTY -- confirmed against KBD_DEBUG=1 logs on
 * real hardware. Cursor/nav keys (Home/Up/PgUp/Left/Right/End/Down/PgDn/
 * Ins/Del) are also confirmed, but NOT via the classic 0xE0-prefixed
 * extended-key scheme every x86 keyboard controller has used since the
 * original IBM PC (as the set-1 docs would suggest) -- this console's
 * K_CODE instead hands them back as a flat, direct run of codes 0x5E-0x67,
 * no prefix at all. The 0xE0-prefixed path is kept in kbd.c as a defensive
 * fallback (in case some other keyboard/driver combo does use it) but has
 * not been observed to fire here.
 */

typedef struct {
	int shift, ctrl, alt;   /* live modifier state (either side held)   */
	int ext;                /* 1 => the previous byte was the 0xE0 prefix */
} kbd_t;

void kbd_init(kbd_t* k);

/*
 * Feed one raw scancode byte (from vtcon_get_scancode()). If it produces
 * terminal input, the bytes are written into buf (caller-supplied, at
 * least 8 bytes) and the count returned (1-8). Returns 0 if this byte
 * produced no output on its own: a key release, a bare modifier
 * press/release, the 0xE0 prefix byte of a two-byte sequence, or an
 * unmapped scancode.
 */
int kbd_feed(kbd_t* k, int code, unsigned char* buf);

#endif /* KBD_H */

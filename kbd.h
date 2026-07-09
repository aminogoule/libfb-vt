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
 * The scancode table is the standard AT/PS2 "set 1" layout, US QWERTY, with
 * 0xE0 as the two-byte extended-key prefix (arrows, Home/End, Ins/Del,
 * PgUp/PgDn, right Ctrl/Alt, keypad Enter/divide) -- the same code space
 * every x86 keyboard controller has used since the original IBM PC, and
 * what K_CODE is documented to hand back. [VERIFY] on real hardware via
 * KBD_DEBUG=1 (see server.c) before relying on it -- this has been built
 * from the documented layout, not confirmed against this specific console.
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

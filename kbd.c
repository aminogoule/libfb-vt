/*
 * kbd.c -- AT/PS2 scancode set 1 decoder (see kbd.h).
 */

#include "kbd.h"

#include <stdio.h>
#include <string.h>

/* Unshifted / shifted base characters by scancode, standard US QWERTY.
   0 = no simple character at this code (modifier, function key, unmapped,
   or handled specially in kbd_feed). Sized to cover the highest code we
   map (0x58 = F12). */
static const char UNSHIFT[0x59] = {
	[0x02]='1', [0x03]='2', [0x04]='3', [0x05]='4', [0x06]='5',
	[0x07]='6', [0x08]='7', [0x09]='8', [0x0A]='9', [0x0B]='0',
	[0x0C]='-', [0x0D]='=',
	[0x10]='q', [0x11]='w', [0x12]='e', [0x13]='r', [0x14]='t',
	[0x15]='y', [0x16]='u', [0x17]='i', [0x18]='o', [0x19]='p',
	[0x1A]='[', [0x1B]=']',
	[0x1E]='a', [0x1F]='s', [0x20]='d', [0x21]='f', [0x22]='g',
	[0x23]='h', [0x24]='j', [0x25]='k', [0x26]='l',
	[0x27]=';', [0x28]='\'', [0x29]='`',
	[0x2B]='\\', [0x2C]='z', [0x2D]='x', [0x2E]='c', [0x2F]='v',
	[0x30]='b', [0x31]='n', [0x32]='m',
	[0x33]=',', [0x34]='.', [0x35]='/',
	[0x39]=' ',
};

static const char SHIFT[0x59] = {
	[0x02]='!', [0x03]='@', [0x04]='#', [0x05]='$', [0x06]='%',
	[0x07]='^', [0x08]='&', [0x09]='*', [0x0A]='(', [0x0B]=')',
	[0x0C]='_', [0x0D]='+',
	[0x10]='Q', [0x11]='W', [0x12]='E', [0x13]='R', [0x14]='T',
	[0x15]='Y', [0x16]='U', [0x17]='I', [0x18]='O', [0x19]='P',
	[0x1A]='{', [0x1B]='}',
	[0x1E]='A', [0x1F]='S', [0x20]='D', [0x21]='F', [0x22]='G',
	[0x23]='H', [0x24]='J', [0x25]='K', [0x26]='L',
	[0x27]=':', [0x28]='"', [0x29]='~',
	[0x2B]='|', [0x2C]='Z', [0x2D]='X', [0x2E]='C', [0x2F]='V',
	[0x30]='B', [0x31]='N', [0x32]='M',
	[0x33]='<', [0x34]='>', [0x35]='?',
	[0x39]=' ',
};

void kbd_init(kbd_t* k) {
	memset(k, 0, sizeof(*k));
}

/* xterm modifier parameter: 1 + Shift(1) + Alt(2) + Ctrl(4). 1 means "no
   modifier" -- omitted from the encoded sequence entirely. */
static int xmod(const kbd_t* k) {
	return 1 + (k->shift ? 1 : 0) + (k->alt ? 2 : 0) + (k->ctrl ? 4 : 0);
}

static int send_cursor(const kbd_t* k, char letter, unsigned char* buf) {
	int mod = xmod(k);
	int n = (mod == 1)
	    ? snprintf((char*)buf, 8, "\033[%c", letter)
	    : snprintf((char*)buf, 8, "\033[1;%d%c", mod, letter);
	return (n > 0 && n < 8) ? n : 0;
}

static int send_tilde(const kbd_t* k, int code, unsigned char* buf) {
	int mod = xmod(k);
	int n = (mod == 1)
	    ? snprintf((char*)buf, 8, "\033[%d~", code)
	    : snprintf((char*)buf, 8, "\033[%d;%d~", code, mod);
	return (n > 0 && n < 8) ? n : 0;
}

static int send_fkey(const kbd_t* k, int n, unsigned char* buf) {
	static const int tilde_code[12] = { 11, 12, 13, 14, 15, 17,
	                                     18, 19, 20, 21, 23, 24 };
	int mod = xmod(k);
	int len;

	if (n < 1 || n > 12)
		return 0;
	if (n <= 4) {
		char letter = (char)('P' + (n - 1));
		len = (mod == 1)
		    ? snprintf((char*)buf, 8, "\033O%c", letter)
		    : snprintf((char*)buf, 8, "\033[1;%d%c", mod, letter);
		return (len > 0 && len < 8) ? len : 0;
	}
	len = (mod == 1)
	    ? snprintf((char*)buf, 8, "\033[%d~", tilde_code[n - 1])
	    : snprintf((char*)buf, 8, "\033[%d;%d~", tilde_code[n - 1], mod);
	return (len > 0 && len < 8) ? len : 0;
}

int kbd_feed(kbd_t* k, int code, unsigned char* buf) {
	int is_break, sc, ext;

	if (code == 0xE0) { k->ext = 1; return 0; }

	is_break = code & 0x80;
	sc       = code & 0x7F;
	ext      = k->ext;
	k->ext   = 0;

	/* modifiers: track state, never produce output themselves */
	if (sc == 0x2A || sc == 0x36) { k->shift = !is_break; return 0; }
	if (sc == 0x1D)               { k->ctrl  = !is_break; return 0; }
	if (sc == 0x38)               { k->alt   = !is_break; return 0; }
	if (sc == 0x3A || sc == 0x45 || sc == 0x46)
		return 0;                 /* CapsLock / NumLock / ScrollLock: ignored */

	if (is_break)
		return 0;                 /* only key-down produces output */

	/*
	 * Extended (cursor/nav) keys. Confirmed via KBD_DEBUG=1 on real
	 * hardware: this console's K_CODE does NOT use the classic AT 0xE0
	 * prefix at all -- Home/Up/PgUp/Left/Right/End/Down/PgDn/Ins/Del
	 * arrive as a flat, direct run of codes 0x5E-0x67. The `ext`
	 * (0xE0-prefixed) branch below is kept as a defensive fallback for
	 * keyboards/drivers that *do* use the standard AT encoding, but has
	 * not been observed to fire on the hardware this was tested on.
	 */
	if (!ext) {
		switch (sc) {
		case 0x5E: return send_cursor(k, 'H', buf);   /* Home    */
		case 0x5F: return send_cursor(k, 'A', buf);   /* Up      */
		case 0x60: return send_tilde(k, 5, buf);      /* PgUp    */
		case 0x61: return send_cursor(k, 'D', buf);   /* Left    */
		case 0x62: return send_cursor(k, 'C', buf);   /* Right   */
		case 0x63: return send_cursor(k, 'F', buf);   /* End     */
		case 0x64: return send_cursor(k, 'B', buf);   /* Down    */
		case 0x65: return send_tilde(k, 6, buf);      /* PgDn    */
		case 0x66: return send_tilde(k, 2, buf);      /* Insert  */
		case 0x67: return send_tilde(k, 3, buf);      /* Delete  */
		default: break;
		}
	}

	if (ext) {
		switch (sc) {
		case 0x48: return send_cursor(k, 'A', buf);   /* Up    */
		case 0x50: return send_cursor(k, 'B', buf);   /* Down  */
		case 0x4D: return send_cursor(k, 'C', buf);   /* Right */
		case 0x4B: return send_cursor(k, 'D', buf);   /* Left  */
		case 0x47: return send_cursor(k, 'H', buf);   /* Home  */
		case 0x4F: return send_cursor(k, 'F', buf);   /* End   */
		case 0x49: return send_tilde(k, 5, buf);      /* PgUp  */
		case 0x51: return send_tilde(k, 6, buf);      /* PgDn  */
		case 0x52: return send_tilde(k, 2, buf);      /* Ins   */
		case 0x53: return send_tilde(k, 3, buf);      /* Del   */
		case 0x1C: buf[0] = '\r'; return 1;           /* KP Enter */
		case 0x35: buf[0] = '/';  return 1;           /* KP /     */
		default: return 0;
		}
	}

	if (sc == 0x01) { buf[0] = 0x1B; return 1; }      /* Esc       */
	if (sc == 0x0E) { buf[0] = 0x7F; return 1; }      /* Backspace */
	if (sc == 0x1C) { buf[0] = '\r'; return 1; }      /* Enter     */
	if (sc == 0x0F) {                                 /* Tab       */
		if (k->shift) { memcpy(buf, "\033[Z", 3); return 3; }
		buf[0] = '\t'; return 1;
	}
	if (sc >= 0x3B && sc <= 0x44) return send_fkey(k, sc - 0x3B + 1, buf);
	if (sc == 0x57) return send_fkey(k, 11, buf);
	if (sc == 0x58) return send_fkey(k, 12, buf);

	if (sc >= (int)(sizeof(UNSHIFT) / sizeof(UNSHIFT[0])))
		return 0;                     /* out of table range: unmapped */

	{
		char ch = k->shift ? SHIFT[sc] : UNSHIFT[sc];
		if (ch == 0)
			return 0;              /* unmapped scancode */
		if (k->ctrl) {
			if      (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 1);
			else if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 1);
			else if (ch == ' ')              ch = 0;
			else if (ch == '[')              ch = 0x1B;
			else if (ch == '\\')             ch = 0x1C;
			else if (ch == ']')              ch = 0x1D;
			/* other punctuation: no Ctrl form, send the plain char */
		}
		if (k->alt) {
			buf[0] = 0x1B;
			buf[1] = (unsigned char)ch;
			return 2;
		}
		buf[0] = (unsigned char)ch;
		return 1;
	}
}

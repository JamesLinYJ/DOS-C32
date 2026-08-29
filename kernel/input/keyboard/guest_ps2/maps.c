// SPDX-License-Identifier: GPL-2.0-only
/* Named logical-key to AT/PS2 scan-set map for the virtual keyboard. */
#include "private.h"

#define GUEST_PS2_SCAN_VALID 0x01u
#define GUEST_PS2_SCAN_E0 0x02u
#define GUEST_PS2_EXTENDED_CODE 0x100u

struct guest_ps2_scan_entry {
	uint8_t set1;
	uint8_t set2;
	uint8_t flags;
	uint8_t reserved;
};

#define PS2_SCAN(key, set1_byte, set2_byte) \
	[INPUT_KEY_CODE_##key] = { \
		(set1_byte), (set2_byte), GUEST_PS2_SCAN_VALID, 0u \
	}
#define PS2_SCAN_E0(key, set1_byte, set2_byte) \
	[INPUT_KEY_CODE_##key] = { \
		(set1_byte), (set2_byte), \
		GUEST_PS2_SCAN_VALID | GUEST_PS2_SCAN_E0, 0u \
	}

static const struct guest_ps2_scan_entry scan_entries[
	INPUT_KEY_CODE_COUNT] = {
	PS2_SCAN(ESC, 0x01u, 0x76u),
	PS2_SCAN(1, 0x02u, 0x16u),
	PS2_SCAN(2, 0x03u, 0x1eu),
	PS2_SCAN(3, 0x04u, 0x26u),
	PS2_SCAN(4, 0x05u, 0x25u),
	PS2_SCAN(5, 0x06u, 0x2eu),
	PS2_SCAN(6, 0x07u, 0x36u),
	PS2_SCAN(7, 0x08u, 0x3du),
	PS2_SCAN(8, 0x09u, 0x3eu),
	PS2_SCAN(9, 0x0au, 0x46u),
	PS2_SCAN(0, 0x0bu, 0x45u),
	PS2_SCAN(MINUS, 0x0cu, 0x4eu),
	PS2_SCAN(EQUAL, 0x0du, 0x55u),
	PS2_SCAN(BACKSPACE, 0x0eu, 0x66u),
	PS2_SCAN(TAB, 0x0fu, 0x0du),
	PS2_SCAN(Q, 0x10u, 0x15u),
	PS2_SCAN(W, 0x11u, 0x1du),
	PS2_SCAN(E, 0x12u, 0x24u),
	PS2_SCAN(R, 0x13u, 0x2du),
	PS2_SCAN(T, 0x14u, 0x2cu),
	PS2_SCAN(Y, 0x15u, 0x35u),
	PS2_SCAN(U, 0x16u, 0x3cu),
	PS2_SCAN(I, 0x17u, 0x43u),
	PS2_SCAN(O, 0x18u, 0x44u),
	PS2_SCAN(P, 0x19u, 0x4du),
	PS2_SCAN(LEFTBRACE, 0x1au, 0x54u),
	PS2_SCAN(RIGHTBRACE, 0x1bu, 0x5bu),
	PS2_SCAN(ENTER, 0x1cu, 0x5au),
	PS2_SCAN(LEFTCTRL, 0x1du, 0x14u),
	PS2_SCAN(A, 0x1eu, 0x1cu),
	PS2_SCAN(S, 0x1fu, 0x1bu),
	PS2_SCAN(D, 0x20u, 0x23u),
	PS2_SCAN(F, 0x21u, 0x2bu),
	PS2_SCAN(G, 0x22u, 0x34u),
	PS2_SCAN(H, 0x23u, 0x33u),
	PS2_SCAN(J, 0x24u, 0x3bu),
	PS2_SCAN(K, 0x25u, 0x42u),
	PS2_SCAN(L, 0x26u, 0x4bu),
	PS2_SCAN(SEMICOLON, 0x27u, 0x4cu),
	PS2_SCAN(APOSTROPHE, 0x28u, 0x52u),
	PS2_SCAN(GRAVE, 0x29u, 0x0eu),
	PS2_SCAN(LEFTSHIFT, 0x2au, 0x12u),
	PS2_SCAN(BACKSLASH, 0x2bu, 0x5du),
	PS2_SCAN(Z, 0x2cu, 0x1au),
	PS2_SCAN(X, 0x2du, 0x22u),
	PS2_SCAN(C, 0x2eu, 0x21u),
	PS2_SCAN(V, 0x2fu, 0x2au),
	PS2_SCAN(B, 0x30u, 0x32u),
	PS2_SCAN(N, 0x31u, 0x31u),
	PS2_SCAN(M, 0x32u, 0x3au),
	PS2_SCAN(COMMA, 0x33u, 0x41u),
	PS2_SCAN(DOT, 0x34u, 0x49u),
	PS2_SCAN(SLASH, 0x35u, 0x4au),
	PS2_SCAN(RIGHTSHIFT, 0x36u, 0x59u),
	PS2_SCAN(KPASTERISK, 0x37u, 0x7cu),
	PS2_SCAN(LEFTALT, 0x38u, 0x11u),
	PS2_SCAN(SPACE, 0x39u, 0x29u),
	PS2_SCAN(CAPSLOCK, 0x3au, 0x58u),
	PS2_SCAN(F1, 0x3bu, 0x05u),
	PS2_SCAN(F2, 0x3cu, 0x06u),
	PS2_SCAN(F3, 0x3du, 0x04u),
	PS2_SCAN(F4, 0x3eu, 0x0cu),
	PS2_SCAN(F5, 0x3fu, 0x03u),
	PS2_SCAN(F6, 0x40u, 0x0bu),
	PS2_SCAN(F7, 0x41u, 0x02u),
	PS2_SCAN(F8, 0x42u, 0x0au),
	PS2_SCAN(F9, 0x43u, 0x01u),
	PS2_SCAN(F10, 0x44u, 0x09u),
	PS2_SCAN(NUMLOCK, 0x45u, 0x77u),
	PS2_SCAN(SCROLLLOCK, 0x46u, 0x7eu),
	PS2_SCAN(KP7, 0x47u, 0x6cu),
	PS2_SCAN(KP8, 0x48u, 0x75u),
	PS2_SCAN(KP9, 0x49u, 0x7du),
	PS2_SCAN(KPMINUS, 0x4au, 0x7bu),
	PS2_SCAN(KP4, 0x4bu, 0x6bu),
	PS2_SCAN(KP5, 0x4cu, 0x73u),
	PS2_SCAN(KP6, 0x4du, 0x74u),
	PS2_SCAN(KPPLUS, 0x4eu, 0x79u),
	PS2_SCAN(KP1, 0x4fu, 0x69u),
	PS2_SCAN(KP2, 0x50u, 0x72u),
	PS2_SCAN(KP3, 0x51u, 0x7au),
	PS2_SCAN(KP0, 0x52u, 0x70u),
	PS2_SCAN(KPDOT, 0x53u, 0x71u),
	PS2_SCAN(SYSRQ, 0x54u, 0x7fu),
	PS2_SCAN(102ND, 0x56u, 0x61u),
	PS2_SCAN(F11, 0x57u, 0x78u),
	PS2_SCAN(F12, 0x58u, 0x07u),
	PS2_SCAN(KPEQUAL, 0x59u, 0x0fu),
	PS2_SCAN(KPJPCOMMA, 0x5cu, 0x27u),
	PS2_SCAN(F13, 0x5du, 0x08u),
	PS2_SCAN(F14, 0x5eu, 0x10u),
	PS2_SCAN(F15, 0x5fu, 0x18u),
	PS2_SCAN(F16, 0x67u, 0x20u),
	PS2_SCAN(F17, 0x68u, 0x28u),
	PS2_SCAN(F18, 0x69u, 0x30u),
	PS2_SCAN(F19, 0x6au, 0x38u),
	PS2_SCAN(F20, 0x6bu, 0x40u),
	PS2_SCAN(F21, 0x6cu, 0x48u),
	PS2_SCAN(F22, 0x6du, 0x50u),
	PS2_SCAN(F23, 0x6eu, 0x57u),
	PS2_SCAN(KATAKANA_HIRAGANA, 0x70u, 0x13u),
	PS2_SCAN(RO, 0x73u, 0x51u),
	PS2_SCAN(F24, 0x76u, 0x5fu),
	PS2_SCAN(HIRAGANA, 0x77u, 0x62u),
	PS2_SCAN(KATAKANA, 0x78u, 0x63u),
	PS2_SCAN(HENKAN, 0x79u, 0x64u),
	PS2_SCAN(MUHENKAN, 0x7bu, 0x67u),
	PS2_SCAN(YEN, 0x7du, 0x6au),
	PS2_SCAN(KPCOMMA, 0x7eu, 0x6du),

	PS2_SCAN_E0(PREVIOUSSONG, 0x10u, 0x15u),
	PS2_SCAN_E0(NEXTSONG, 0x19u, 0x4du),
	PS2_SCAN_E0(KPENTER, 0x1cu, 0x5au),
	PS2_SCAN_E0(RIGHTCTRL, 0x1du, 0x14u),
	PS2_SCAN_E0(MUTE, 0x20u, 0x23u),
	PS2_SCAN_E0(CALC, 0x21u, 0x2bu),
	PS2_SCAN_E0(PLAYPAUSE, 0x22u, 0x34u),
	PS2_SCAN_E0(STOPCD, 0x24u, 0x3bu),
	PS2_SCAN_E0(VOLUMEDOWN, 0x2eu, 0x21u),
	PS2_SCAN_E0(VOLUMEUP, 0x30u, 0x32u),
	PS2_SCAN_E0(HOMEPAGE, 0x32u, 0x3au),
	PS2_SCAN_E0(KPSLASH, 0x35u, 0x4au),
	PS2_SCAN_E0(RIGHTALT, 0x38u, 0x11u),
	PS2_SCAN_E0(PAUSE, 0x45u, 0x77u),
	PS2_SCAN_E0(HOME, 0x47u, 0x6cu),
	PS2_SCAN_E0(UP, 0x48u, 0x75u),
	PS2_SCAN_E0(PAGEUP, 0x49u, 0x7du),
	PS2_SCAN_E0(LEFT, 0x4bu, 0x6bu),
	PS2_SCAN_E0(MACRO, 0x4cu, 0x6fu),
	PS2_SCAN_E0(RIGHT, 0x4du, 0x74u),
	PS2_SCAN_E0(KPPLUSMINUS, 0x4eu, 0x79u),
	PS2_SCAN_E0(END, 0x4fu, 0x69u),
	PS2_SCAN_E0(DOWN, 0x50u, 0x72u),
	PS2_SCAN_E0(PAGEDOWN, 0x51u, 0x7au),
	PS2_SCAN_E0(INSERT, 0x52u, 0x70u),
	PS2_SCAN_E0(DELETE, 0x53u, 0x71u),
	PS2_SCAN_E0(LEFTMETA, 0x5bu, 0x1fu),
	PS2_SCAN_E0(RIGHTMETA, 0x5cu, 0x27u),
	PS2_SCAN_E0(COMPOSE, 0x5du, 0x2fu),
	PS2_SCAN_E0(POWER, 0x5eu, 0x37u),
	PS2_SCAN_E0(SLEEP, 0x5fu, 0x3fu),
	PS2_SCAN_E0(WAKEUP, 0x63u, 0x5eu),
	PS2_SCAN_E0(SEARCH, 0x65u, 0x10u),
	PS2_SCAN_E0(BOOKMARKS, 0x66u, 0x18u),
	PS2_SCAN_E0(REFRESH, 0x67u, 0x20u),
	PS2_SCAN_E0(STOP, 0x68u, 0x28u),
	PS2_SCAN_E0(FORWARD, 0x69u, 0x30u),
	PS2_SCAN_E0(BACK, 0x6au, 0x38u),
	PS2_SCAN_E0(COMPUTER, 0x6bu, 0x40u),
	PS2_SCAN_E0(MAIL, 0x6cu, 0x48u),
	PS2_SCAN_E0(MEDIA, 0x6du, 0x50u),
};

bool guest_ps2_internal_lookup(input_key_code_t keycode,
	uint16_t *set1_code, uint16_t *set2_code)
{
	const struct guest_ps2_scan_entry *entry;
	uint16_t prefix;

	if (set1_code == NULL || set2_code == NULL ||
	    keycode == INPUT_KEY_CODE_RESERVED ||
	    keycode >= INPUT_KEY_CODE_COUNT)
		return false;
	entry = &scan_entries[keycode];
	if ((entry->flags & GUEST_PS2_SCAN_VALID) == 0u)
		return false;
	prefix = (entry->flags & GUEST_PS2_SCAN_E0) != 0u
			 ? GUEST_PS2_EXTENDED_CODE
			 : 0u;
	*set1_code = prefix | entry->set1;
	*set2_code = prefix | entry->set2;
	return true;
}

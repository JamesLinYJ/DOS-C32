// SPDX-License-Identifier: GPL-2.0-only
/*
 * Native US console keymap
 *
 * Compatibility contract: capture the BIOS scan/ASCII pair selected by the modifier,
 *                 lock and enhanced-key state at key-make time
 * Safety changes: decoded key codes index only bounded private tables; no raw
 *                 controller byte, external input ABI or guest pointer is used
 */
#include "keymap.h"

#include "input_keycodes.h"

#define KEY_CHARACTER_VALID (1u << 0)
#define KEY_CONTROL_VALID (1u << 1)
#define KEY_CHARACTER_LETTER (1u << 2)

struct key_character_entry {
	uint8_t scan;
	uint8_t normal;
	uint8_t shifted;
	uint8_t control;
	uint8_t flags;
};

/* Enhanced keypad keys are selected by decoded identity, not scan position. */
static const struct key_character_entry character_map[] = {
	[INPUT_KEY_CODE_ESC] = {0x01u, 0x1bu, 0x1bu, 0x1bu,
		KEY_CHARACTER_VALID | KEY_CONTROL_VALID},
	[INPUT_KEY_CODE_1] = {0x02u, '1', '!', 0u, KEY_CHARACTER_VALID},
	[INPUT_KEY_CODE_2] = {0x03u, '2', '@', 0x00u,
		KEY_CHARACTER_VALID | KEY_CONTROL_VALID},
	[INPUT_KEY_CODE_3] = {0x04u, '3', '#', 0u, KEY_CHARACTER_VALID},
	[INPUT_KEY_CODE_4] = {0x05u, '4', '$', 0u, KEY_CHARACTER_VALID},
	[INPUT_KEY_CODE_5] = {0x06u, '5', '%', 0u, KEY_CHARACTER_VALID},
	[INPUT_KEY_CODE_6] = {0x07u, '6', '^', 0x1eu,
		KEY_CHARACTER_VALID | KEY_CONTROL_VALID},
	[INPUT_KEY_CODE_7] = {0x08u, '7', '&', 0u, KEY_CHARACTER_VALID},
	[INPUT_KEY_CODE_8] = {0x09u, '8', '*', 0u, KEY_CHARACTER_VALID},
	[INPUT_KEY_CODE_9] = {0x0au, '9', '(', 0u, KEY_CHARACTER_VALID},
	[INPUT_KEY_CODE_0] = {0x0bu, '0', ')', 0u, KEY_CHARACTER_VALID},
	[INPUT_KEY_CODE_MINUS] = {0x0cu, '-', '_', 0x1fu,
		KEY_CHARACTER_VALID | KEY_CONTROL_VALID},
	[INPUT_KEY_CODE_EQUAL] = {0x0du, '=', '+', 0u,
		KEY_CHARACTER_VALID},
	[INPUT_KEY_CODE_BACKSPACE] = {0x0eu, '\b', '\b', 0x7fu,
		KEY_CHARACTER_VALID | KEY_CONTROL_VALID},
	[INPUT_KEY_CODE_TAB] = {0x0fu, '\t', 0u, 0u, KEY_CHARACTER_VALID},
	[INPUT_KEY_CODE_Q] = {0x10u, 'q', 'Q', 17u,
		KEY_CHARACTER_VALID | KEY_CONTROL_VALID |
		KEY_CHARACTER_LETTER},
	[INPUT_KEY_CODE_W] = {0x11u, 'w', 'W', 23u,
		KEY_CHARACTER_VALID | KEY_CONTROL_VALID |
		KEY_CHARACTER_LETTER},
	[INPUT_KEY_CODE_E] = {0x12u, 'e', 'E', 5u,
		KEY_CHARACTER_VALID | KEY_CONTROL_VALID |
		KEY_CHARACTER_LETTER},
	[INPUT_KEY_CODE_R] = {0x13u, 'r', 'R', 18u,
		KEY_CHARACTER_VALID | KEY_CONTROL_VALID |
		KEY_CHARACTER_LETTER},
	[INPUT_KEY_CODE_T] = {0x14u, 't', 'T', 20u,
		KEY_CHARACTER_VALID | KEY_CONTROL_VALID |
		KEY_CHARACTER_LETTER},
	[INPUT_KEY_CODE_Y] = {0x15u, 'y', 'Y', 25u,
		KEY_CHARACTER_VALID | KEY_CONTROL_VALID |
		KEY_CHARACTER_LETTER},
	[INPUT_KEY_CODE_U] = {0x16u, 'u', 'U', 21u,
		KEY_CHARACTER_VALID | KEY_CONTROL_VALID |
		KEY_CHARACTER_LETTER},
	[INPUT_KEY_CODE_I] = {0x17u, 'i', 'I', 9u,
		KEY_CHARACTER_VALID | KEY_CONTROL_VALID |
		KEY_CHARACTER_LETTER},
	[INPUT_KEY_CODE_O] = {0x18u, 'o', 'O', 15u,
		KEY_CHARACTER_VALID | KEY_CONTROL_VALID |
		KEY_CHARACTER_LETTER},
	[INPUT_KEY_CODE_P] = {0x19u, 'p', 'P', 16u,
		KEY_CHARACTER_VALID | KEY_CONTROL_VALID |
		KEY_CHARACTER_LETTER},
	[INPUT_KEY_CODE_LEFTBRACE] = {0x1au, '[', '{', 0x1bu,
		KEY_CHARACTER_VALID | KEY_CONTROL_VALID},
	[INPUT_KEY_CODE_RIGHTBRACE] = {0x1bu, ']', '}', 0x1du,
		KEY_CHARACTER_VALID | KEY_CONTROL_VALID},
	[INPUT_KEY_CODE_ENTER] = {0x1cu, '\r', '\r', '\n',
		KEY_CHARACTER_VALID | KEY_CONTROL_VALID},
	[INPUT_KEY_CODE_A] = {0x1eu, 'a', 'A', 1u,
		KEY_CHARACTER_VALID | KEY_CONTROL_VALID |
		KEY_CHARACTER_LETTER},
	[INPUT_KEY_CODE_S] = {0x1fu, 's', 'S', 19u,
		KEY_CHARACTER_VALID | KEY_CONTROL_VALID |
		KEY_CHARACTER_LETTER},
	[INPUT_KEY_CODE_D] = {0x20u, 'd', 'D', 4u,
		KEY_CHARACTER_VALID | KEY_CONTROL_VALID |
		KEY_CHARACTER_LETTER},
	[INPUT_KEY_CODE_F] = {0x21u, 'f', 'F', 6u,
		KEY_CHARACTER_VALID | KEY_CONTROL_VALID |
		KEY_CHARACTER_LETTER},
	[INPUT_KEY_CODE_G] = {0x22u, 'g', 'G', 7u,
		KEY_CHARACTER_VALID | KEY_CONTROL_VALID |
		KEY_CHARACTER_LETTER},
	[INPUT_KEY_CODE_H] = {0x23u, 'h', 'H', 8u,
		KEY_CHARACTER_VALID | KEY_CONTROL_VALID |
		KEY_CHARACTER_LETTER},
	[INPUT_KEY_CODE_J] = {0x24u, 'j', 'J', 10u,
		KEY_CHARACTER_VALID | KEY_CONTROL_VALID |
		KEY_CHARACTER_LETTER},
	[INPUT_KEY_CODE_K] = {0x25u, 'k', 'K', 11u,
		KEY_CHARACTER_VALID | KEY_CONTROL_VALID |
		KEY_CHARACTER_LETTER},
	[INPUT_KEY_CODE_L] = {0x26u, 'l', 'L', 12u,
		KEY_CHARACTER_VALID | KEY_CONTROL_VALID |
		KEY_CHARACTER_LETTER},
	[INPUT_KEY_CODE_SEMICOLON] = {0x27u, ';', ':', 0u,
		KEY_CHARACTER_VALID},
	[INPUT_KEY_CODE_APOSTROPHE] = {0x28u, '\'', '"', 0u,
		KEY_CHARACTER_VALID},
	[INPUT_KEY_CODE_GRAVE] = {0x29u, '`', '~', 0u,
		KEY_CHARACTER_VALID},
	[INPUT_KEY_CODE_BACKSLASH] = {0x2bu, '\\', '|', 0x1cu,
		KEY_CHARACTER_VALID | KEY_CONTROL_VALID},
	[INPUT_KEY_CODE_Z] = {0x2cu, 'z', 'Z', 26u,
		KEY_CHARACTER_VALID | KEY_CONTROL_VALID |
		KEY_CHARACTER_LETTER},
	[INPUT_KEY_CODE_X] = {0x2du, 'x', 'X', 24u,
		KEY_CHARACTER_VALID | KEY_CONTROL_VALID |
		KEY_CHARACTER_LETTER},
	[INPUT_KEY_CODE_C] = {0x2eu, 'c', 'C', 3u,
		KEY_CHARACTER_VALID | KEY_CONTROL_VALID |
		KEY_CHARACTER_LETTER},
	[INPUT_KEY_CODE_V] = {0x2fu, 'v', 'V', 22u,
		KEY_CHARACTER_VALID | KEY_CONTROL_VALID |
		KEY_CHARACTER_LETTER},
	[INPUT_KEY_CODE_B] = {0x30u, 'b', 'B', 2u,
		KEY_CHARACTER_VALID | KEY_CONTROL_VALID |
		KEY_CHARACTER_LETTER},
	[INPUT_KEY_CODE_N] = {0x31u, 'n', 'N', 14u,
		KEY_CHARACTER_VALID | KEY_CONTROL_VALID |
		KEY_CHARACTER_LETTER},
	[INPUT_KEY_CODE_M] = {0x32u, 'm', 'M', 13u,
		KEY_CHARACTER_VALID | KEY_CONTROL_VALID |
		KEY_CHARACTER_LETTER},
	[INPUT_KEY_CODE_COMMA] = {0x33u, ',', '<', 0u,
		KEY_CHARACTER_VALID},
	[INPUT_KEY_CODE_DOT] = {0x34u, '.', '>', 0u, KEY_CHARACTER_VALID},
	[INPUT_KEY_CODE_SLASH] = {0x35u, '/', '?', 0u,
		KEY_CHARACTER_VALID},
	[INPUT_KEY_CODE_SPACE] = {0x39u, ' ', ' ', 0x00u,
		KEY_CHARACTER_VALID | KEY_CONTROL_VALID},
	[INPUT_KEY_CODE_102ND] = {0x56u, '\\', '|', 0x1cu,
		KEY_CHARACTER_VALID | KEY_CONTROL_VALID},
};

struct navigation_entry {
	input_key_code_t code;
	uint8_t base_scan;
	uint8_t control_scan;
	uint8_t alternate_scan;
};

static const struct navigation_entry navigation_map[] = {
	{INPUT_KEY_CODE_HOME, 0x47u, 0x77u, 0x97u},
	{INPUT_KEY_CODE_UP, 0x48u, 0x8du, 0x98u},
	{INPUT_KEY_CODE_PAGEUP, 0x49u, 0x84u, 0x99u},
	{INPUT_KEY_CODE_LEFT, 0x4bu, 0x73u, 0x9bu},
	{INPUT_KEY_CODE_RIGHT, 0x4du, 0x74u, 0x9du},
	{INPUT_KEY_CODE_END, 0x4fu, 0x75u, 0x9fu},
	{INPUT_KEY_CODE_DOWN, 0x50u, 0x91u, 0xa0u},
	{INPUT_KEY_CODE_PAGEDOWN, 0x51u, 0x76u, 0xa1u},
	{INPUT_KEY_CODE_INSERT, 0x52u, 0x92u, 0xa2u},
	{INPUT_KEY_CODE_DELETE, 0x53u, 0x93u, 0xa3u},
};

static bool modifier_shifted(const struct keyboard_console *console)
{
	return console->left_shift != 0u || console->right_shift != 0u;
}

static bool modifier_control(const struct keyboard_console *console)
{
	return console->left_ctrl != 0u || console->right_ctrl != 0u;
}

static bool modifier_alternate(const struct keyboard_console *console)
{
	return console->left_alt != 0u || console->right_alt != 0u;
}

static void set_bios_only(struct keyboard_key_translation *translation,
			  uint8_t scan)
{
	translation->bios_key = (uint16_t)scan << 8u;
	translation->character = 0u;
	translation->has_character = 0u;
}

static void set_character(struct keyboard_key_translation *translation,
			  uint8_t scan, uint8_t character)
{
	translation->bios_key = ((uint16_t)scan << 8u) | character;
	translation->character = character == (uint8_t)'\r'
				       ? (uint8_t)'\n'
				       : character;
	translation->has_character = 1u;
}

static bool translate_function(const struct keyboard_console *console,
	input_key_code_t code, struct keyboard_key_translation *translation)
{
	uint8_t scan;
	uint8_t index;

	if (code >= INPUT_KEY_CODE_F1 && code <= INPUT_KEY_CODE_F10) {
		index = (uint8_t)(code - INPUT_KEY_CODE_F1);
		if (modifier_alternate(console))
			scan = (uint8_t)(0x68u + index);
		else if (modifier_control(console))
			scan = (uint8_t)(0x5eu + index);
		else if (modifier_shifted(console))
			scan = (uint8_t)(0x54u + index);
		else
			scan = (uint8_t)(0x3bu + index);
		set_bios_only(translation, scan);
		return true;
	}
	if (code != INPUT_KEY_CODE_F11 && code != INPUT_KEY_CODE_F12)
		return false;
	index = (uint8_t)(code - INPUT_KEY_CODE_F11);
	if (modifier_alternate(console))
		scan = (uint8_t)(0x8bu + index);
	else if (modifier_control(console))
		scan = (uint8_t)(0x89u + index);
	else if (modifier_shifted(console))
		scan = (uint8_t)(0x87u + index);
	else
		scan = (uint8_t)(0x85u + index);
	set_bios_only(translation, scan);
	return true;
}

static bool translate_navigation(const struct keyboard_console *console,
	input_key_code_t code, struct keyboard_key_translation *translation)
{
	size_t index;
	uint8_t scan;

	for (index = 0u; index < ARRAY_SIZE(navigation_map); ++index) {
		if (navigation_map[index].code != code)
			continue;
		if (modifier_alternate(console))
			scan = navigation_map[index].alternate_scan;
		else if (modifier_control(console))
			scan = navigation_map[index].control_scan;
		else
			scan = navigation_map[index].base_scan;
		set_bios_only(translation, scan);
		return true;
	}
	return false;
}

static bool keypad_navigation_code(input_key_code_t code,
				    input_key_code_t *navigation)
{
	static const input_key_code_t keypad_navigation[] = {
		[INPUT_KEY_CODE_KP7 - INPUT_KEY_CODE_KP7] = INPUT_KEY_CODE_HOME,
		[INPUT_KEY_CODE_KP8 - INPUT_KEY_CODE_KP7] = INPUT_KEY_CODE_UP,
		[INPUT_KEY_CODE_KP9 - INPUT_KEY_CODE_KP7] = INPUT_KEY_CODE_PAGEUP,
		[INPUT_KEY_CODE_KP4 - INPUT_KEY_CODE_KP7] = INPUT_KEY_CODE_LEFT,
		[INPUT_KEY_CODE_KP5 - INPUT_KEY_CODE_KP7] = INPUT_KEY_CODE_RESERVED,
		[INPUT_KEY_CODE_KP6 - INPUT_KEY_CODE_KP7] = INPUT_KEY_CODE_RIGHT,
		[INPUT_KEY_CODE_KP1 - INPUT_KEY_CODE_KP7] = INPUT_KEY_CODE_END,
		[INPUT_KEY_CODE_KP2 - INPUT_KEY_CODE_KP7] = INPUT_KEY_CODE_DOWN,
		[INPUT_KEY_CODE_KP3 - INPUT_KEY_CODE_KP7] = INPUT_KEY_CODE_PAGEDOWN,
		[INPUT_KEY_CODE_KP0 - INPUT_KEY_CODE_KP7] = INPUT_KEY_CODE_INSERT,
		[INPUT_KEY_CODE_KPDOT - INPUT_KEY_CODE_KP7] = INPUT_KEY_CODE_DELETE,
	};
	uint16_t index;

	if (navigation == NULL || code < INPUT_KEY_CODE_KP7 ||
	    code > INPUT_KEY_CODE_KPDOT || code == INPUT_KEY_CODE_KPMINUS ||
	    code == INPUT_KEY_CODE_KPPLUS)
		return false;
	index = (uint16_t)(code - INPUT_KEY_CODE_KP7);
	if (index >= ARRAY_SIZE(keypad_navigation))
		return false;
	*navigation = keypad_navigation[index];
	return true;
}

static bool translate_keypad(const struct keyboard_console *console,
	input_key_code_t code, struct keyboard_key_translation *translation)
{
	struct keypad_character_entry {
		uint8_t scan;
		uint8_t character;
	};
	static const struct keypad_character_entry keypad_characters[] = {
		[INPUT_KEY_CODE_KP7 - INPUT_KEY_CODE_KP7] = {0x47u, '7'},
		[INPUT_KEY_CODE_KP8 - INPUT_KEY_CODE_KP7] = {0x48u, '8'},
		[INPUT_KEY_CODE_KP9 - INPUT_KEY_CODE_KP7] = {0x49u, '9'},
		[INPUT_KEY_CODE_KP4 - INPUT_KEY_CODE_KP7] = {0x4bu, '4'},
		[INPUT_KEY_CODE_KP5 - INPUT_KEY_CODE_KP7] = {0x4cu, '5'},
		[INPUT_KEY_CODE_KP6 - INPUT_KEY_CODE_KP7] = {0x4du, '6'},
		[INPUT_KEY_CODE_KP1 - INPUT_KEY_CODE_KP7] = {0x4fu, '1'},
		[INPUT_KEY_CODE_KP2 - INPUT_KEY_CODE_KP7] = {0x50u, '2'},
		[INPUT_KEY_CODE_KP3 - INPUT_KEY_CODE_KP7] = {0x51u, '3'},
		[INPUT_KEY_CODE_KP0 - INPUT_KEY_CODE_KP7] = {0x52u, '0'},
		[INPUT_KEY_CODE_KPDOT - INPUT_KEY_CODE_KP7] = {0x53u, '.'},
	};
	input_key_code_t navigation;
	uint16_t index;
	bool numeric;

	if (code == INPUT_KEY_CODE_KPASTERISK) {
		set_character(translation, 0x37u, '*');
		return true;
	}
	if (code == INPUT_KEY_CODE_KPMINUS) {
		set_character(translation, 0x4au, '-');
		return true;
	}
	if (code == INPUT_KEY_CODE_KPPLUS) {
		set_character(translation, 0x4eu, '+');
		return true;
	}
	if (code == INPUT_KEY_CODE_KPSLASH) {
		if (modifier_control(console))
			set_bios_only(translation, 0x95u);
		else if (modifier_alternate(console))
			set_bios_only(translation, 0xa4u);
		else
			set_character(translation, 0x35u, '/');
		return true;
	}
	if (code == INPUT_KEY_CODE_KPENTER) {
		if (modifier_alternate(console))
			set_bios_only(translation, 0xa6u);
		else
			set_character(translation, 0x1cu,
				      modifier_control(console) ? '\n' : '\r');
		return true;
	}
	if (!keypad_navigation_code(code, &navigation))
		return false;
	if (modifier_control(console) || modifier_alternate(console)) {
		if (navigation == INPUT_KEY_CODE_RESERVED) {
			set_bios_only(translation, 0x4cu);
			return true;
		}
		return translate_navigation(console, navigation, translation);
	}
	numeric = (console->num_lock != 0u) != modifier_shifted(console);
	if (!numeric) {
		if (navigation == INPUT_KEY_CODE_RESERVED) {
			set_bios_only(translation, 0x4cu);
			return true;
		}
		return translate_navigation(console, navigation, translation);
	}
	index = (uint16_t)(code - INPUT_KEY_CODE_KP7);
	if (index >= ARRAY_SIZE(keypad_characters) ||
	    keypad_characters[index].character == 0u)
		return false;
	set_character(translation, keypad_characters[index].scan,
		      keypad_characters[index].character);
	return true;
}

static bool translate_character(const struct keyboard_console *console,
	input_key_code_t code, struct keyboard_key_translation *translation)
{
	const struct key_character_entry *entry;
	uint8_t character;
	bool shifted;

	if (code >= ARRAY_SIZE(character_map))
		return false;
	entry = &character_map[code];
	if ((entry->flags & KEY_CHARACTER_VALID) == 0u)
		return false;
	if (modifier_alternate(console)) {
		if (code >= INPUT_KEY_CODE_1 && code <= INPUT_KEY_CODE_EQUAL)
			set_bios_only(translation,
				      (uint8_t)(0x78u + code -
						INPUT_KEY_CODE_1));
		else
			set_bios_only(translation, entry->scan);
		return true;
	}
	if (modifier_control(console)) {
		if (code == INPUT_KEY_CODE_TAB) {
			set_bios_only(translation, 0x94u);
			return true;
		}
		if ((entry->flags & KEY_CONTROL_VALID) == 0u)
			return false;
		set_character(translation, entry->scan, entry->control);
		return true;
	}
	shifted = modifier_shifted(console);
	if ((entry->flags & KEY_CHARACTER_LETTER) != 0u &&
	    console->caps_lock != 0u)
		shifted = !shifted;
	if (code == INPUT_KEY_CODE_TAB && shifted) {
		set_bios_only(translation, entry->scan);
		return true;
	}
	character = shifted ? entry->shifted : entry->normal;
	set_character(translation, entry->scan, character);
	return true;
}

bool keyboard_keymap_translate(const struct keyboard_console *console,
	input_key_code_t code, struct keyboard_key_translation *translation)
{
	if (console == NULL || translation == NULL ||
	    code > INPUT_KEY_CODE_NAMESPACE_MAX)
		return false;
	*translation = (struct keyboard_key_translation){0u, 0u, 0u};
	if (translate_function(console, code, translation) ||
	    translate_navigation(console, code, translation) ||
	    translate_keypad(console, code, translation) ||
	    translate_character(console, code, translation))
		return true;
	if (code == INPUT_KEY_CODE_PAUSE) {
		set_bios_only(translation, 0x45u);
		return true;
	}
	return false;
}

bool keyboard_keymap_toggles_insert(const struct keyboard_console *console,
	input_key_code_t code)
{
	bool numeric;

	if (console == NULL)
		return false;
	if (code == INPUT_KEY_CODE_INSERT)
		return true;
	if (code != INPUT_KEY_CODE_KP0 || modifier_control(console) ||
	    modifier_alternate(console))
		return false;
	numeric = (console->num_lock != 0u) != modifier_shifted(console);
	return !numeric;
}

bool keyboard_keymap_compatibility_bios_key(char character, uint16_t *key)
{
	uint16_t code;
	uint8_t byte = (uint8_t)character;

	if (key == NULL)
		return false;
	if (character == '\n' || character == '\r') {
		*key = 0x1c0du;
		return true;
	}
	for (code = 0u; code < ARRAY_SIZE(character_map); ++code) {
		const struct key_character_entry *entry = &character_map[code];

		if ((entry->flags & KEY_CHARACTER_VALID) == 0u)
			continue;
		if (entry->normal == byte || entry->shifted == byte ||
		    ((entry->flags & KEY_CONTROL_VALID) != 0u &&
		     entry->control == byte)) {
			*key = ((uint16_t)entry->scan << 8u) | byte;
			return true;
		}
	}
	return false;
}

/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DOSC32_INPUT_CONSOLE_KEYMAP_H
#define DOSC32_INPUT_CONSOLE_KEYMAP_H

#include "keyboard.h"

struct keyboard_key_translation {
	uint16_t bios_key;
	uint8_t character;
	uint8_t has_character;
};

bool keyboard_keymap_translate(
	const struct keyboard_console *console, input_key_code_t code,
	struct keyboard_key_translation *translation);
bool keyboard_keymap_toggles_insert(
	const struct keyboard_console *console, input_key_code_t code);
bool keyboard_keymap_compatibility_bios_key(char character, uint16_t *key);

#endif

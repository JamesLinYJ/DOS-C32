/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DOSC32_GUEST_PS2_KEYBOARD_PRIVATE_H
#define DOSC32_GUEST_PS2_KEYBOARD_PRIVATE_H

#include "guest_ps2_keyboard.h"
#include "input_keycodes.h"

#define GUEST_PS2_KEYBOARD_COOKIE 0x4750324bu
#define GUEST_PS2_KEYBOARD_GENERATION_MAX ((uint64_t)-2)

enum guest_ps2_encode_status {
	GUEST_PS2_ENCODE_OK = 0,
	GUEST_PS2_ENCODE_NO_OUTPUT,
	GUEST_PS2_ENCODE_UNSUPPORTED
};

struct guest_ps2_sequence {
	uint8_t values[GUEST_PS2_KEYBOARD_SEQUENCE_CAPACITY];
	uint8_t count;
	uint8_t reserved[7];
} __aligned(8);

bool guest_ps2_internal_lookup(input_key_code_t keycode,
	uint16_t *set1_code, uint16_t *set2_code) __must_check;

enum guest_ps2_encode_status guest_ps2_internal_encode(
	const struct input_event *event,
	const struct x86_i8042_keyboard_mode *mode,
	struct guest_ps2_sequence *sequence) __must_check;
uint64_t guest_ps2_internal_saturating_increment(uint64_t value);
void guest_ps2_internal_poison(struct guest_ps2_keyboard *keyboard);
enum input_focus_result guest_ps2_internal_focus_enter(
	struct input_handler *handler, kernel_object_handle_t context,
	kernel_object_handle_t core_identity,
	uint64_t focus_generation) __must_check;
void guest_ps2_internal_focus_leave(
	struct input_handler *handler, kernel_object_handle_t context,
	kernel_object_handle_t core_identity, uint64_t focus_generation);
enum input_handler_result guest_ps2_internal_receive(
	struct input_handler *handler, kernel_object_handle_t context,
	const struct input_event *event) __must_check;

#endif

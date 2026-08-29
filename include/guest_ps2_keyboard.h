/* SPDX-License-Identifier: GPL-2.0-only */
/* Internal decoded-key consumer for an isolated guest PS/2 keyboard. */
#ifndef DOSC32_GUEST_PS2_KEYBOARD_H
#define DOSC32_GUEST_PS2_KEYBOARD_H

#include "input.h"
#include "x86_i8042.h"

#define GUEST_PS2_KEYBOARD_SEQUENCE_CAPACITY 8u

enum guest_ps2_keyboard_status {
	GUEST_PS2_KEYBOARD_OK = 0,
	GUEST_PS2_KEYBOARD_RETRY,
	GUEST_PS2_KEYBOARD_INVALID_ARGUMENT,
	GUEST_PS2_KEYBOARD_INVALID_STATE,
	GUEST_PS2_KEYBOARD_CAPACITY_EXHAUSTED,
	GUEST_PS2_KEYBOARD_IDENTITY_MISMATCH,
	GUEST_PS2_KEYBOARD_STALE_REFERENCE,
	GUEST_PS2_KEYBOARD_BUSY,
	GUEST_PS2_KEYBOARD_POISONED
};

enum guest_ps2_keyboard_phase {
	GUEST_PS2_KEYBOARD_EMPTY = 0,
	GUEST_PS2_KEYBOARD_REGISTERED,
	GUEST_PS2_KEYBOARD_FOCUSED,
	GUEST_PS2_KEYBOARD_QUIESCED,
	GUEST_PS2_KEYBOARD_RETIRED,
	GUEST_PS2_KEYBOARD_POISONED_PHASE
};

struct guest_ps2_keyboard_config {
	kernel_object_handle_t identity;
	kernel_object_handle_t context_identity;
	kernel_object_handle_t input_core_identity;
	kernel_object_handle_t machine_identity;
	kernel_object_handle_t source_identity;
	struct input_core *input_core;
	uint8_t reserved[8];
} __aligned(8);

struct guest_ps2_keyboard_reference {
	kernel_object_handle_t identity;
	uint64_t generation;
	struct input_handler_binding handler_binding;
} __aligned(8);

struct guest_ps2_keyboard_snapshot {
	kernel_object_handle_t identity;
	uint64_t generation;
	uint64_t focus_generation;
	uint64_t focus_bind_count;
	uint64_t focus_unbind_count;
	uint64_t received_event_count;
	uint64_t injected_event_count;
	uint64_t injected_byte_count;
	uint64_t deferred_event_count;
	uint64_t unsupported_event_count;
	uint64_t stale_event_count;
	uint8_t phase;
	uint8_t downstream_bound;
	uint8_t poisoned;
	uint8_t reserved[5];
} __aligned(8);

/* Caller-owned; no native address is used as an identity or ABI handle. */
struct guest_ps2_keyboard {
	struct guest_ps2_keyboard_config config;
	struct input_handler input_handler;
	struct input_handler_binding handler_binding;
	struct x86_i8042_input_binding downstream_binding;
	uint64_t generation;
	uint64_t focus_generation;
	uint64_t focus_bind_count;
	uint64_t focus_unbind_count;
	uint64_t received_event_count;
	uint64_t injected_event_count;
	uint64_t injected_byte_count;
	uint64_t deferred_event_count;
	uint64_t unsupported_event_count;
	uint64_t stale_event_count;
	uint32_t lifecycle_cookie;
	uint8_t phase;
	uint8_t downstream_bound;
	uint8_t poisoned;
	uint8_t reserved[5];
} __aligned(8);

void guest_ps2_keyboard_construct(struct guest_ps2_keyboard *keyboard);
enum guest_ps2_keyboard_status guest_ps2_keyboard_register(
	struct guest_ps2_keyboard *keyboard,
	const struct guest_ps2_keyboard_config *config,
	struct guest_ps2_keyboard_reference *reference) __must_check;
enum guest_ps2_keyboard_status guest_ps2_keyboard_focus(
	struct guest_ps2_keyboard *keyboard,
	const struct guest_ps2_keyboard_reference *reference) __must_check;
enum guest_ps2_keyboard_status guest_ps2_keyboard_unfocus(
	struct guest_ps2_keyboard *keyboard,
	const struct guest_ps2_keyboard_reference *reference) __must_check;
enum guest_ps2_keyboard_status guest_ps2_keyboard_pump(
	struct guest_ps2_keyboard *keyboard,
	const struct guest_ps2_keyboard_reference *reference,
	size_t budget, size_t *processed) __must_check;
enum guest_ps2_keyboard_status guest_ps2_keyboard_quiesce(
	struct guest_ps2_keyboard *keyboard,
	const struct guest_ps2_keyboard_reference *reference) __must_check;
enum guest_ps2_keyboard_status guest_ps2_keyboard_unregister(
	struct guest_ps2_keyboard *keyboard,
	const struct guest_ps2_keyboard_reference *reference) __must_check;
enum guest_ps2_keyboard_status guest_ps2_keyboard_snapshot(
	const struct guest_ps2_keyboard *keyboard,
	const struct guest_ps2_keyboard_reference *reference,
	struct guest_ps2_keyboard_snapshot *snapshot) __must_check;

static_assert_expression(
	sizeof(struct guest_ps2_keyboard_reference) == 56u,
	"guest PS/2 keyboard reference layout changed");
static_assert_expression(
	__builtin_offsetof(struct guest_ps2_keyboard_reference, handler_binding) ==
		16u,
	"guest PS/2 handler binding offset changed");

#endif

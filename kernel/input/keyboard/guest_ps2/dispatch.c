// SPDX-License-Identifier: GPL-2.0-only
/* IRQ-safe decoded-event consumption and atomic guest sequence injection. */
#include "private.h"

#include "x86_guest_space.h"

static enum input_handler_result defer_event(
	struct guest_ps2_keyboard *keyboard)
{
	keyboard->deferred_event_count = guest_ps2_internal_saturating_increment(
		keyboard->deferred_event_count);
	return INPUT_HANDLER_DEFER;
}

static enum input_handler_result poison_event(
	struct guest_ps2_keyboard *keyboard, bool stale)
{
	if (stale)
		keyboard->stale_event_count =
			guest_ps2_internal_saturating_increment(
				keyboard->stale_event_count);
	guest_ps2_internal_poison(keyboard);
	return INPUT_HANDLER_BROKEN;
}

static bool status_is_zero_commit_retry(enum x86_guest_space_status status)
{
	return status == X86_GUEST_SPACE_INPUT_MODE_CHANGED ||
	       status == X86_GUEST_SPACE_CAPACITY_EXHAUSTED ||
	       status == X86_GUEST_SPACE_IO_DENIED ||
	       status == X86_GUEST_SPACE_INVALID_STATE;
}

static bool event_matches_focus(const struct guest_ps2_keyboard *keyboard,
	const struct input_event *event)
{
	return event->handler_identity == keyboard->config.identity &&
	       event->focus_generation == keyboard->focus_generation &&
	       event->device_generation != 0u && event->sequence != 0u;
}

enum input_handler_result guest_ps2_internal_receive(
	struct input_handler *handler, kernel_object_handle_t context,
	const struct input_event *event)
{
	struct guest_ps2_keyboard *keyboard = input_handler_context(handler);
	struct x86_i8042_keyboard_mode mode;
	struct guest_ps2_sequence sequence;
	enum guest_ps2_encode_status encode_status;
	enum x86_guest_space_status status;

	if (keyboard == NULL || event == NULL ||
	    handler != &keyboard->input_handler ||
	    keyboard->lifecycle_cookie != GUEST_PS2_KEYBOARD_COOKIE ||
	    context != keyboard->config.context_identity ||
	    keyboard->phase != GUEST_PS2_KEYBOARD_FOCUSED ||
	    keyboard->downstream_bound == 0u || keyboard->poisoned != 0u ||
	    !event_matches_focus(keyboard, event))
		return keyboard != NULL ? poison_event(keyboard, true)
					: INPUT_HANDLER_BROKEN;
	keyboard->received_event_count = guest_ps2_internal_saturating_increment(
		keyboard->received_event_count);
	status = x86_guest_space_i8042_input_keyboard_mode(
		&keyboard->downstream_binding, &mode);
	if (status != X86_GUEST_SPACE_OK) {
		if (status_is_zero_commit_retry(status))
			return defer_event(keyboard);
		return poison_event(
			keyboard, status == X86_GUEST_SPACE_STALE_BINDING);
	}
	if (mode.scanning_enabled == 0u || mode.interface_enabled == 0u)
		return defer_event(keyboard);
	encode_status = guest_ps2_internal_encode(event, &mode, &sequence);
	if (encode_status == GUEST_PS2_ENCODE_NO_OUTPUT)
		return INPUT_HANDLER_HANDLED;
	if (encode_status != GUEST_PS2_ENCODE_OK) {
		keyboard->unsupported_event_count =
			guest_ps2_internal_saturating_increment(
				keyboard->unsupported_event_count);
		return INPUT_HANDLER_REJECTED;
	}
	status = x86_guest_space_i8042_input_inject_keyboard_sequence(
		&keyboard->downstream_binding, &mode, sequence.values,
		ARRAY_SIZE(sequence.values), sequence.count);
	if (status == X86_GUEST_SPACE_OK ||
	    status == X86_GUEST_SPACE_INPUT_COMMITTED_DELIVERY_PENDING) {
		keyboard->injected_event_count =
			guest_ps2_internal_saturating_increment(
				keyboard->injected_event_count);
		if ((uint64_t)-1 - keyboard->injected_byte_count < sequence.count)
			keyboard->injected_byte_count = (uint64_t)-1;
		else
			keyboard->injected_byte_count += sequence.count;
		return INPUT_HANDLER_HANDLED;
	}
	if (status_is_zero_commit_retry(status))
		return defer_event(keyboard);
	return poison_event(keyboard,
			    status == X86_GUEST_SPACE_STALE_BINDING);
}

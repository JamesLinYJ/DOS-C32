// SPDX-License-Identifier: GPL-2.0-only
/* Caller-owned guest PS/2 input-handler and downstream binding lifecycle. */
#include "private.h"

#include "x86_guest_space.h"

static bool identity_is_valid(kernel_object_handle_t identity)
{
	return identity != 0u && identity != KERNEL_OBJECT_HANDLE_INVALID;
}

static bool bytes_are_zero(const uint8_t *bytes, size_t count)
{
	size_t index;

	for (index = 0u; index < count; ++index) {
		if (bytes[index] != 0u)
			return false;
	}
	return true;
}

static void clear_downstream_binding(struct guest_ps2_keyboard *keyboard)
{
	uint8_t *bytes = (uint8_t *)&keyboard->downstream_binding;
	size_t index;

	for (index = 0u; index < sizeof(keyboard->downstream_binding); ++index)
		bytes[index] = 0u;
}

static bool config_is_valid(const struct guest_ps2_keyboard_config *config)
{
	return config != NULL && identity_is_valid(config->identity) &&
	       identity_is_valid(config->context_identity) &&
	       identity_is_valid(config->input_core_identity) &&
	       identity_is_valid(config->machine_identity) &&
	       identity_is_valid(config->source_identity) &&
	       config->source_identity != config->machine_identity &&
	       config->input_core != NULL &&
	       bytes_are_zero(config->reserved, ARRAY_SIZE(config->reserved));
}

static bool handler_binding_equal(
	const struct input_handler_binding *left,
	const struct input_handler_binding *right)
{
	return left->core_identity == right->core_identity &&
	       left->core_generation == right->core_generation &&
	       left->handler_identity == right->handler_identity &&
	       left->handler_generation == right->handler_generation &&
	       left->slot == right->slot &&
	       bytes_are_zero(left->reserved, ARRAY_SIZE(left->reserved)) &&
	       bytes_are_zero(right->reserved, ARRAY_SIZE(right->reserved));
}

static enum guest_ps2_keyboard_status reference_status(
	const struct guest_ps2_keyboard *keyboard,
	const struct guest_ps2_keyboard_reference *reference)
{
	if (keyboard == NULL || reference == NULL ||
	    keyboard->lifecycle_cookie != GUEST_PS2_KEYBOARD_COOKIE ||
	    !identity_is_valid(reference->identity))
		return GUEST_PS2_KEYBOARD_INVALID_ARGUMENT;
	if (keyboard->phase == GUEST_PS2_KEYBOARD_EMPTY ||
	    keyboard->phase == GUEST_PS2_KEYBOARD_RETIRED)
		return GUEST_PS2_KEYBOARD_INVALID_STATE;
	if (reference->identity != keyboard->config.identity)
		return GUEST_PS2_KEYBOARD_IDENTITY_MISMATCH;
	if (reference->generation != keyboard->generation ||
	    !handler_binding_equal(&reference->handler_binding,
				   &keyboard->handler_binding))
		return GUEST_PS2_KEYBOARD_STALE_REFERENCE;
	if (keyboard->poisoned != 0u ||
	    keyboard->phase == GUEST_PS2_KEYBOARD_POISONED_PHASE)
		return GUEST_PS2_KEYBOARD_POISONED;
	return GUEST_PS2_KEYBOARD_OK;
}

static enum guest_ps2_keyboard_status input_status(enum input_status status)
{
	if (status == INPUT_OK)
		return GUEST_PS2_KEYBOARD_OK;
	if (status == INPUT_INVALID_ARGUMENT)
		return GUEST_PS2_KEYBOARD_INVALID_ARGUMENT;
	if (status == INPUT_IDENTITY_MISMATCH)
		return GUEST_PS2_KEYBOARD_IDENTITY_MISMATCH;
	if (status == INPUT_STALE_BINDING)
		return GUEST_PS2_KEYBOARD_STALE_REFERENCE;
	if (status == INPUT_CAPACITY_EXHAUSTED)
		return GUEST_PS2_KEYBOARD_CAPACITY_EXHAUSTED;
	if (status == INPUT_BUSY || status == INPUT_RETRY ||
	    status == INPUT_DEFERRED || status == INPUT_ACCESS_DENIED ||
	    status == INPUT_UNAVAILABLE)
		return GUEST_PS2_KEYBOARD_BUSY;
	if (status == INPUT_POISONED || status == INPUT_HANDLER_FAULT)
		return GUEST_PS2_KEYBOARD_POISONED;
	return GUEST_PS2_KEYBOARD_INVALID_STATE;
}

static enum guest_ps2_keyboard_status guest_status(
	enum x86_guest_space_status status)
{
	if (status == X86_GUEST_SPACE_OK)
		return GUEST_PS2_KEYBOARD_OK;
	if (status == X86_GUEST_SPACE_INVALID_ARGUMENT)
		return GUEST_PS2_KEYBOARD_INVALID_ARGUMENT;
	if (status == X86_GUEST_SPACE_MACHINE_MISMATCH ||
	    status == X86_GUEST_SPACE_INTERRUPT_SOURCE_MISMATCH)
		return GUEST_PS2_KEYBOARD_IDENTITY_MISMATCH;
	if (status == X86_GUEST_SPACE_STALE_BINDING)
		return GUEST_PS2_KEYBOARD_STALE_REFERENCE;
	if (status == X86_GUEST_SPACE_CAPACITY_EXHAUSTED)
		return GUEST_PS2_KEYBOARD_CAPACITY_EXHAUSTED;
	if (status == X86_GUEST_SPACE_DEVICE_EVENT_RETRY)
		return GUEST_PS2_KEYBOARD_RETRY;
	if (status == X86_GUEST_SPACE_IO_DENIED)
		return GUEST_PS2_KEYBOARD_BUSY;
	if (status == X86_GUEST_SPACE_DEVICE_FAULT)
		return GUEST_PS2_KEYBOARD_POISONED;
	return GUEST_PS2_KEYBOARD_INVALID_STATE;
}

uint64_t guest_ps2_internal_saturating_increment(uint64_t value)
{
	return value == (uint64_t)-1 ? value : value + 1u;
}

void guest_ps2_internal_poison(struct guest_ps2_keyboard *keyboard)
{
	if (keyboard == NULL)
		return;
	keyboard->poisoned = 1u;
	keyboard->phase = GUEST_PS2_KEYBOARD_POISONED_PHASE;
}

static bool focus_failure_is_recoverable(enum x86_guest_space_status status)
{
	return status == X86_GUEST_SPACE_INVALID_STATE ||
	       status == X86_GUEST_SPACE_IO_DENIED ||
	       status == X86_GUEST_SPACE_INPUT_MODE_CHANGED ||
	       status == X86_GUEST_SPACE_DEVICE_EVENT_RETRY;
}

enum input_focus_result guest_ps2_internal_focus_enter(
	struct input_handler *handler, kernel_object_handle_t context,
	kernel_object_handle_t core_identity, uint64_t focus_generation)
{
	struct guest_ps2_keyboard *keyboard = input_handler_context(handler);
	const struct x86_i8042_input_config input_config = {
		.capabilities = X86_I8042_INPUT_KEYBOARD,
		.reserved = {0u},
	};
	enum x86_guest_space_status status;

	if (keyboard == NULL || handler != &keyboard->input_handler ||
	    keyboard->lifecycle_cookie != GUEST_PS2_KEYBOARD_COOKIE ||
	    context != keyboard->config.context_identity ||
	    core_identity != keyboard->config.input_core_identity ||
	    focus_generation == 0u ||
	    keyboard->phase != GUEST_PS2_KEYBOARD_REGISTERED ||
	    keyboard->downstream_bound != 0u || keyboard->poisoned != 0u) {
		if (keyboard != NULL)
			guest_ps2_internal_poison(keyboard);
		return INPUT_FOCUS_BROKEN;
	}
	status = x86_guest_space_i8042_input_bind(
		keyboard->config.machine_identity, keyboard->config.source_identity,
		&input_config, &keyboard->downstream_binding);
	if (status != X86_GUEST_SPACE_OK) {
		if (focus_failure_is_recoverable(status))
			return INPUT_FOCUS_REJECTED;
		guest_ps2_internal_poison(keyboard);
		return INPUT_FOCUS_BROKEN;
	}
	keyboard->focus_generation = focus_generation;
	keyboard->focus_bind_count = guest_ps2_internal_saturating_increment(
		keyboard->focus_bind_count);
	keyboard->downstream_bound = 1u;
	keyboard->phase = GUEST_PS2_KEYBOARD_FOCUSED;
	return INPUT_FOCUS_OK;
}

void guest_ps2_internal_focus_leave(
	struct input_handler *handler, kernel_object_handle_t context,
	kernel_object_handle_t core_identity, uint64_t focus_generation)
{
	struct guest_ps2_keyboard *keyboard = input_handler_context(handler);
	enum x86_guest_space_status status;

	if (keyboard == NULL || handler != &keyboard->input_handler ||
	    keyboard->lifecycle_cookie != GUEST_PS2_KEYBOARD_COOKIE ||
	    context != keyboard->config.context_identity ||
	    core_identity != keyboard->config.input_core_identity ||
	    focus_generation != keyboard->focus_generation ||
	    keyboard->phase != GUEST_PS2_KEYBOARD_FOCUSED ||
	    keyboard->downstream_bound == 0u || keyboard->poisoned != 0u) {
		if (keyboard != NULL)
			guest_ps2_internal_poison(keyboard);
		return;
	}
	status = x86_guest_space_i8042_input_quiesce(
		keyboard->config.machine_identity, &keyboard->downstream_binding);
	if (status != X86_GUEST_SPACE_OK) {
		guest_ps2_internal_poison(keyboard);
		return;
	}
	status = x86_guest_space_i8042_input_unbind(
		keyboard->config.machine_identity, &keyboard->downstream_binding);
	if (status != X86_GUEST_SPACE_OK) {
		guest_ps2_internal_poison(keyboard);
		return;
	}
	clear_downstream_binding(keyboard);
	keyboard->focus_generation = 0u;
	keyboard->focus_unbind_count = guest_ps2_internal_saturating_increment(
		keyboard->focus_unbind_count);
	keyboard->downstream_bound = 0u;
	keyboard->phase = GUEST_PS2_KEYBOARD_REGISTERED;
}

void guest_ps2_keyboard_construct(struct guest_ps2_keyboard *keyboard)
{
	uint8_t *bytes = (uint8_t *)keyboard;
	size_t index;

	if (keyboard == NULL)
		return;
	for (index = 0u; index < sizeof(*keyboard); ++index)
		bytes[index] = 0u;
	keyboard->lifecycle_cookie = GUEST_PS2_KEYBOARD_COOKIE;
	keyboard->phase = GUEST_PS2_KEYBOARD_EMPTY;
	input_handler_construct(&keyboard->input_handler);
}

enum guest_ps2_keyboard_status guest_ps2_keyboard_register(
	struct guest_ps2_keyboard *keyboard,
	const struct guest_ps2_keyboard_config *config,
	struct guest_ps2_keyboard_reference *reference)
{
	struct input_handler_config handler_config;
	struct input_handler_binding binding;
	enum input_status status;
	uint64_t generation;

	if (keyboard == NULL || !config_is_valid(config) || reference == NULL ||
	    keyboard->lifecycle_cookie != GUEST_PS2_KEYBOARD_COOKIE)
		return GUEST_PS2_KEYBOARD_INVALID_ARGUMENT;
	if (keyboard->phase != GUEST_PS2_KEYBOARD_EMPTY &&
	    keyboard->phase != GUEST_PS2_KEYBOARD_RETIRED)
		return GUEST_PS2_KEYBOARD_INVALID_STATE;
	if (keyboard->generation >= GUEST_PS2_KEYBOARD_GENERATION_MAX)
		return GUEST_PS2_KEYBOARD_CAPACITY_EXHAUSTED;
	generation = keyboard->generation + 1u;
	handler_config = (struct input_handler_config){
		.identity = config->identity,
		.context = config->context_identity,
		.handler_context = keyboard,
		.capabilities = INPUT_CAPABILITY_KEY,
		.focus_enter = guest_ps2_internal_focus_enter,
		.focus_leave = guest_ps2_internal_focus_leave,
		.receive = guest_ps2_internal_receive,
		.reserved = {0u},
	};
	status = input_handler_register(config->input_core,
					&keyboard->input_handler,
					&handler_config, &binding);
	if (status != INPUT_OK)
		return input_status(status);
	keyboard->config = *config;
	keyboard->handler_binding = binding;
	keyboard->generation = generation;
	keyboard->focus_generation = 0u;
	keyboard->focus_bind_count = 0u;
	keyboard->focus_unbind_count = 0u;
	keyboard->received_event_count = 0u;
	keyboard->injected_event_count = 0u;
	keyboard->injected_byte_count = 0u;
	keyboard->deferred_event_count = 0u;
	keyboard->unsupported_event_count = 0u;
	keyboard->stale_event_count = 0u;
	keyboard->phase = GUEST_PS2_KEYBOARD_REGISTERED;
	keyboard->downstream_bound = 0u;
	keyboard->poisoned = 0u;
	clear_downstream_binding(keyboard);
	*reference = (struct guest_ps2_keyboard_reference){
		.identity = config->identity,
		.generation = generation,
		.handler_binding = binding,
	};
	return GUEST_PS2_KEYBOARD_OK;
}

enum guest_ps2_keyboard_status guest_ps2_keyboard_focus(
	struct guest_ps2_keyboard *keyboard,
	const struct guest_ps2_keyboard_reference *reference)
{
	enum guest_ps2_keyboard_status status =
		reference_status(keyboard, reference);
	enum input_status focus_status;

	if (status != GUEST_PS2_KEYBOARD_OK)
		return status;
	if (keyboard->phase != GUEST_PS2_KEYBOARD_REGISTERED ||
	    keyboard->downstream_bound != 0u)
		return GUEST_PS2_KEYBOARD_INVALID_STATE;
	focus_status = input_focus_set(keyboard->config.input_core,
				       &reference->handler_binding);
	if (focus_status != INPUT_OK)
		return input_status(focus_status);
	return keyboard->phase == GUEST_PS2_KEYBOARD_FOCUSED &&
		       keyboard->downstream_bound != 0u
		       ? GUEST_PS2_KEYBOARD_OK
		       : GUEST_PS2_KEYBOARD_POISONED;
}

enum guest_ps2_keyboard_status guest_ps2_keyboard_unfocus(
	struct guest_ps2_keyboard *keyboard,
	const struct guest_ps2_keyboard_reference *reference)
{
	enum guest_ps2_keyboard_status status =
		reference_status(keyboard, reference);
	enum input_status focus_status;

	if (status != GUEST_PS2_KEYBOARD_OK)
		return status;
	if (keyboard->phase != GUEST_PS2_KEYBOARD_FOCUSED ||
	    keyboard->downstream_bound == 0u)
		return GUEST_PS2_KEYBOARD_INVALID_STATE;
	focus_status = input_focus_clear(keyboard->config.input_core,
					 &reference->handler_binding);
	if (focus_status != INPUT_OK)
		return input_status(focus_status);
	return keyboard->poisoned == 0u &&
		       keyboard->phase == GUEST_PS2_KEYBOARD_REGISTERED &&
		       keyboard->downstream_bound == 0u
		       ? GUEST_PS2_KEYBOARD_OK
		       : GUEST_PS2_KEYBOARD_POISONED;
}

enum guest_ps2_keyboard_status guest_ps2_keyboard_pump(
	struct guest_ps2_keyboard *keyboard,
	const struct guest_ps2_keyboard_reference *reference,
	size_t budget, size_t *processed)
{
	enum guest_ps2_keyboard_status status =
		reference_status(keyboard, reference);

	if (status != GUEST_PS2_KEYBOARD_OK)
		return status;
	if (processed == NULL || budget == 0u)
		return GUEST_PS2_KEYBOARD_INVALID_ARGUMENT;
	if (keyboard->phase != GUEST_PS2_KEYBOARD_FOCUSED ||
	    keyboard->downstream_bound == 0u)
		return GUEST_PS2_KEYBOARD_INVALID_STATE;
	return guest_status(x86_guest_space_device_events_pump(
		keyboard->config.machine_identity, budget, processed));
}

enum guest_ps2_keyboard_status guest_ps2_keyboard_quiesce(
	struct guest_ps2_keyboard *keyboard,
	const struct guest_ps2_keyboard_reference *reference)
{
	enum guest_ps2_keyboard_status status =
		reference_status(keyboard, reference);
	enum input_status handler_status;

	if (status != GUEST_PS2_KEYBOARD_OK)
		return status;
	if (keyboard->phase != GUEST_PS2_KEYBOARD_REGISTERED ||
	    keyboard->downstream_bound != 0u)
		return GUEST_PS2_KEYBOARD_INVALID_STATE;
	handler_status = input_handler_quiesce(
		keyboard->config.input_core, &reference->handler_binding);
	if (handler_status != INPUT_OK)
		return input_status(handler_status);
	keyboard->phase = GUEST_PS2_KEYBOARD_QUIESCED;
	return GUEST_PS2_KEYBOARD_OK;
}

enum guest_ps2_keyboard_status guest_ps2_keyboard_unregister(
	struct guest_ps2_keyboard *keyboard,
	const struct guest_ps2_keyboard_reference *reference)
{
	enum guest_ps2_keyboard_status status =
		reference_status(keyboard, reference);
	enum input_status handler_status;

	if (status != GUEST_PS2_KEYBOARD_OK)
		return status;
	if (keyboard->phase != GUEST_PS2_KEYBOARD_QUIESCED ||
	    keyboard->downstream_bound != 0u)
		return GUEST_PS2_KEYBOARD_INVALID_STATE;
	handler_status = input_handler_unregister(
		keyboard->config.input_core, &reference->handler_binding);
	if (handler_status != INPUT_OK)
		return input_status(handler_status);
	keyboard->phase = GUEST_PS2_KEYBOARD_RETIRED;
	return GUEST_PS2_KEYBOARD_OK;
}

enum guest_ps2_keyboard_status guest_ps2_keyboard_snapshot(
	const struct guest_ps2_keyboard *keyboard,
	const struct guest_ps2_keyboard_reference *reference,
	struct guest_ps2_keyboard_snapshot *snapshot)
{
	enum guest_ps2_keyboard_status status;

	if (snapshot == NULL)
		return GUEST_PS2_KEYBOARD_INVALID_ARGUMENT;
	status = reference_status(keyboard, reference);
	if (status != GUEST_PS2_KEYBOARD_OK &&
	    status != GUEST_PS2_KEYBOARD_POISONED)
		return status;
	*snapshot = (struct guest_ps2_keyboard_snapshot){
		.identity = keyboard->config.identity,
		.generation = keyboard->generation,
		.focus_generation = keyboard->focus_generation,
		.focus_bind_count = keyboard->focus_bind_count,
		.focus_unbind_count = keyboard->focus_unbind_count,
		.received_event_count = keyboard->received_event_count,
		.injected_event_count = keyboard->injected_event_count,
		.injected_byte_count = keyboard->injected_byte_count,
		.deferred_event_count = keyboard->deferred_event_count,
		.unsupported_event_count = keyboard->unsupported_event_count,
		.stale_event_count = keyboard->stale_event_count,
		.phase = keyboard->phase,
		.downstream_bound = keyboard->downstream_bound,
		.poisoned = keyboard->poisoned,
		.reserved = {0u},
	};
	return status;
}

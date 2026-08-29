// SPDX-License-Identifier: GPL-2.0-only
/*
 * Native x86 interrupt descriptor/action dispatch.
 *
 * A small single-core contract provides explicit descriptor/action ownership
 * and a private interrupt ABI.
 */
#include "native_private.h"

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

static bool storage_is_valid(struct x86_native_irq_line_slot *lines,
	uint32_t line_capacity, struct x86_native_irq_action_slot *actions,
	uint32_t action_capacity)
{
	uintptr_t action_bytes;
	uintptr_t action_end;
	uintptr_t action_start = (uintptr_t)actions;
	uintptr_t line_bytes;
	uintptr_t line_end;
	uintptr_t line_start = (uintptr_t)lines;

	if (lines == NULL || line_capacity == 0u || actions == NULL ||
	    action_capacity == 0u ||
	    line_start % __alignof__(struct x86_native_irq_line_slot) != 0u ||
	    action_start % __alignof__(struct x86_native_irq_action_slot) !=
		    0u ||
	    check_mul_overflow((uintptr_t)line_capacity,
			       (uintptr_t)sizeof(*lines), &line_bytes) ||
	    check_add_overflow(line_start, line_bytes, &line_end) ||
	    check_mul_overflow((uintptr_t)action_capacity,
			       (uintptr_t)sizeof(*actions), &action_bytes) ||
	    check_add_overflow(action_start, action_bytes, &action_end))
		return false;
	return line_start >= action_end || action_start >= line_end;
}

static bool replacement_is_disjoint(
	const struct x86_native_irq_dispatch *dispatch,
	struct x86_native_irq_line_slot *lines, uint32_t line_capacity,
	struct x86_native_irq_action_slot *actions, uint32_t action_capacity)
{
	uintptr_t action_end;
	uintptr_t action_start = (uintptr_t)actions;
	uintptr_t line_end;
	uintptr_t line_start = (uintptr_t)lines;
	uintptr_t old_action_end;
	uintptr_t old_action_start = (uintptr_t)dispatch->actions;
	uintptr_t old_line_end;
	uintptr_t old_line_start = (uintptr_t)dispatch->lines;

	if (check_add_overflow(
		    old_line_start,
		    (uintptr_t)dispatch->line_capacity *
			    (uintptr_t)sizeof(*dispatch->lines),
		    &old_line_end) ||
	    check_add_overflow(old_action_start,
			       (uintptr_t)dispatch->action_capacity *
				       (uintptr_t)sizeof(*dispatch->actions),
			       &old_action_end) ||
	    check_add_overflow(line_start,
			       (uintptr_t)line_capacity *
				       (uintptr_t)sizeof(*lines),
			       &line_end) ||
	    check_add_overflow(action_start,
			       (uintptr_t)action_capacity *
				       (uintptr_t)sizeof(*actions),
			       &action_end))
		return false;
	return (line_start >= old_line_end || old_line_start >= line_end) &&
	       (line_start >= old_action_end || old_action_start >= line_end) &&
	       (action_start >= old_line_end || old_line_start >= action_end) &&
	       (action_start >= old_action_end ||
		old_action_start >= action_end);
}

static void clear_line(struct x86_native_irq_line_slot *line)
{
	*line = (struct x86_native_irq_line_slot){
		.first_action = X86_NATIVE_IRQ_INVALID_SLOT,
	};
}

static void clear_storage(struct x86_native_irq_line_slot *lines,
	uint32_t line_capacity, struct x86_native_irq_action_slot *actions,
	uint32_t action_capacity)
{
	uint32_t index;

	for (index = 0u; index < line_capacity; ++index)
		clear_line(&lines[index]);
	for (index = 0u; index < action_capacity; ++index) {
		actions[index] = (struct x86_native_irq_action_slot){
			.identity = KERNEL_OBJECT_HANDLE_INVALID,
			.context = KERNEL_OBJECT_HANDLE_INVALID,
			.next_action = X86_NATIVE_IRQ_INVALID_SLOT,
		};
	}
}

static void clear_lifetime(struct x86_native_irq_dispatch *dispatch)
{
	struct x86_native_irq_line_slot *lines = dispatch->lines;
	struct x86_native_irq_action_slot *actions = dispatch->actions;
	uint64_t generation = dispatch->generation;
	uint32_t line_capacity = dispatch->line_capacity;
	uint32_t action_capacity = dispatch->action_capacity;

	clear_storage(lines, line_capacity, actions, action_capacity);
	*dispatch = (struct x86_native_irq_dispatch){
		.identity = KERNEL_OBJECT_HANDLE_INVALID,
		.controller_identity = KERNEL_OBJECT_HANDLE_INVALID,
		.controller_context = KERNEL_OBJECT_HANDLE_INVALID,
		.generation = generation,
		.lines = lines,
		.actions = actions,
		.line_capacity = line_capacity,
		.action_capacity = action_capacity,
		.lifecycle_cookie = X86_NATIVE_IRQ_DISPATCH_COOKIE,
		.phase = X86_NATIVE_IRQ_DISPATCH_EMPTY,
		.initialized = 1u,
	};
}

static bool dispatch_config_is_valid(
	const struct x86_native_irq_dispatch_config *config)
{
	return config != NULL && identity_is_valid(config->identity) &&
	       identity_is_valid(config->controller_identity) &&
	       identity_is_valid(config->controller_context) &&
	       config->identity != config->controller_identity &&
	       config->identity != config->controller_context &&
	       config->controller.begin != NULL && config->controller.end != NULL &&
	       config->controller.quiesce != NULL &&
	       config->controller.resume != NULL;
}

static bool line_config_is_valid(
	const struct x86_native_irq_line_config *line)
{
	return line != NULL &&
	       (line->flags & ~X86_NATIVE_IRQ_LINE_FLAG_MASK) == 0u &&
	       bytes_are_zero(line->reserved, ARRAY_SIZE(line->reserved));
}

static bool line_configs_are_valid(
	const struct x86_native_irq_line_config *lines, uint32_t line_count)
{
	uint32_t first;
	uint32_t second;

	if (lines == NULL || line_count == 0u)
		return false;
	for (first = 0u; first < line_count; ++first) {
		if (!line_config_is_valid(&lines[first]))
			return false;
		for (second = 0u; second < first; ++second) {
			if (lines[first].vector == lines[second].vector ||
			    lines[first].hardware_irq ==
				    lines[second].hardware_irq)
				return false;
		}
	}
	return true;
}

static enum x86_native_irq_status dispatch_identity_status(
	const struct x86_native_irq_dispatch *dispatch,
	kernel_object_handle_t identity)
{
	if (dispatch == NULL || !identity_is_valid(identity))
		return X86_NATIVE_IRQ_INVALID_ARGUMENT;
	if (dispatch->lifecycle_cookie != X86_NATIVE_IRQ_DISPATCH_COOKIE ||
	    dispatch->initialized != 1u ||
	    dispatch->phase == X86_NATIVE_IRQ_DISPATCH_UNINITIALIZED ||
	    dispatch->phase == X86_NATIVE_IRQ_DISPATCH_EMPTY)
		return X86_NATIVE_IRQ_INVALID_STATE;
	if (dispatch->identity != identity)
		return X86_NATIVE_IRQ_IDENTITY_MISMATCH;
	if (dispatch->phase == X86_NATIVE_IRQ_DISPATCH_POISONED_PHASE)
		return X86_NATIVE_IRQ_POISONED;
	return X86_NATIVE_IRQ_OK;
}

static bool storage_encoding_is_valid(
	const struct x86_native_irq_dispatch *dispatch)
{
	uint32_t action_count = 0u;
	uint32_t line_count = 0u;
	uint32_t line_index;
	uint32_t linked_actions = 0u;

	for (line_index = 0u; line_index < dispatch->line_capacity;
	     ++line_index) {
		const struct x86_native_irq_line_slot *line =
			&dispatch->lines[line_index];
		uint32_t action = line->first_action;
		uint32_t line_actions = 0u;
		uint32_t steps = 0u;

		if (line->active == 0u)
			continue;
		line_count++;
		while (action != X86_NATIVE_IRQ_INVALID_SLOT) {
			const struct x86_native_irq_action_slot *slot;

			if (action >= dispatch->action_capacity ||
			    steps++ >= dispatch->action_capacity)
				return false;
			slot = &dispatch->actions[action];
			if (slot->active == 0u ||
			    slot->hardware_irq != line->hardware_irq)
				return false;
			line_actions++;
			action = slot->next_action;
		}
		if (line_actions != line->action_count)
			return false;
		linked_actions += line_actions;
	}
	for (line_index = 0u; line_index < dispatch->action_capacity;
	     ++line_index) {
		if (dispatch->actions[line_index].active != 0u)
			action_count++;
	}
	return line_count == dispatch->line_count &&
	       linked_actions == dispatch->action_count &&
	       action_count == dispatch->action_count;
}

void x86_native_irq_dispatch_construct(
	struct x86_native_irq_dispatch *dispatch)
{
	if (dispatch == NULL)
		return;
	*dispatch = (struct x86_native_irq_dispatch){
		.identity = KERNEL_OBJECT_HANDLE_INVALID,
		.controller_identity = KERNEL_OBJECT_HANDLE_INVALID,
		.controller_context = KERNEL_OBJECT_HANDLE_INVALID,
		.lifecycle_cookie = X86_NATIVE_IRQ_DISPATCH_COOKIE,
		.phase = X86_NATIVE_IRQ_DISPATCH_UNINITIALIZED,
	};
}

enum x86_native_irq_status x86_native_irq_dispatch_initialize(
	struct x86_native_irq_dispatch *dispatch,
	struct x86_native_irq_line_slot *lines, uint32_t line_capacity,
	struct x86_native_irq_action_slot *actions, uint32_t action_capacity)
{
	if (dispatch == NULL ||
	    !storage_is_valid(lines, line_capacity, actions, action_capacity))
		return X86_NATIVE_IRQ_INVALID_ARGUMENT;
	if (dispatch->lifecycle_cookie != X86_NATIVE_IRQ_DISPATCH_COOKIE ||
	    dispatch->initialized != 0u ||
	    dispatch->phase != X86_NATIVE_IRQ_DISPATCH_UNINITIALIZED)
		return X86_NATIVE_IRQ_INVALID_STATE;
	clear_storage(lines, line_capacity, actions, action_capacity);
	*dispatch = (struct x86_native_irq_dispatch){
		.identity = KERNEL_OBJECT_HANDLE_INVALID,
		.controller_identity = KERNEL_OBJECT_HANDLE_INVALID,
		.controller_context = KERNEL_OBJECT_HANDLE_INVALID,
		.lines = lines,
		.actions = actions,
		.line_capacity = line_capacity,
		.action_capacity = action_capacity,
		.lifecycle_cookie = X86_NATIVE_IRQ_DISPATCH_COOKIE,
		.phase = X86_NATIVE_IRQ_DISPATCH_EMPTY,
		.initialized = 1u,
	};
	return X86_NATIVE_IRQ_OK;
}

enum x86_native_irq_status x86_native_irq_dispatch_replace_storage(
	struct x86_native_irq_dispatch *dispatch,
	struct x86_native_irq_line_slot *lines, uint32_t line_capacity,
	struct x86_native_irq_action_slot *actions, uint32_t action_capacity)
{
	uint32_t index;

	if (dispatch == NULL ||
	    !storage_is_valid(lines, line_capacity, actions, action_capacity))
		return X86_NATIVE_IRQ_INVALID_ARGUMENT;
	if (dispatch->lifecycle_cookie != X86_NATIVE_IRQ_DISPATCH_COOKIE ||
	    dispatch->initialized != 1u ||
	    (dispatch->phase != X86_NATIVE_IRQ_DISPATCH_EMPTY &&
	     dispatch->phase != X86_NATIVE_IRQ_DISPATCH_PREPARED &&
	     dispatch->phase != X86_NATIVE_IRQ_DISPATCH_QUIESCED) ||
	    dispatch->dispatch_active != 0u)
		return X86_NATIVE_IRQ_INVALID_STATE;
	if (!replacement_is_disjoint(dispatch, lines, line_capacity, actions,
				     action_capacity))
		return X86_NATIVE_IRQ_INVALID_ARGUMENT;
	if (dispatch->phase != X86_NATIVE_IRQ_DISPATCH_EMPTY &&
	    (line_capacity < dispatch->line_capacity ||
	     action_capacity < dispatch->action_capacity))
		return X86_NATIVE_IRQ_CAPACITY_EXHAUSTED;
	if (!storage_encoding_is_valid(dispatch))
		return X86_NATIVE_IRQ_POISONED;
	clear_storage(lines, line_capacity, actions, action_capacity);
	if (dispatch->phase != X86_NATIVE_IRQ_DISPATCH_EMPTY) {
		for (index = 0u; index < dispatch->line_capacity; ++index)
			lines[index] = dispatch->lines[index];
		for (index = 0u; index < dispatch->action_capacity; ++index)
			actions[index] = dispatch->actions[index];
	}
	dispatch->lines = lines;
	dispatch->actions = actions;
	dispatch->line_capacity = line_capacity;
	dispatch->action_capacity = action_capacity;
	return X86_NATIVE_IRQ_OK;
}

enum x86_native_irq_status x86_native_irq_dispatch_prepare(
	struct x86_native_irq_dispatch *dispatch,
	const struct x86_native_irq_dispatch_config *config,
	const struct x86_native_irq_line_config *lines, uint32_t line_count)
{
	uint32_t index;

	if (dispatch == NULL || !dispatch_config_is_valid(config) ||
	    !line_configs_are_valid(lines, line_count))
		return X86_NATIVE_IRQ_INVALID_ARGUMENT;
	if (dispatch->lifecycle_cookie != X86_NATIVE_IRQ_DISPATCH_COOKIE ||
	    dispatch->initialized != 1u ||
	    dispatch->phase != X86_NATIVE_IRQ_DISPATCH_EMPTY ||
	    dispatch->action_count != 0u || dispatch->line_count != 0u)
		return X86_NATIVE_IRQ_INVALID_STATE;
	if (line_count > dispatch->line_capacity)
		return X86_NATIVE_IRQ_CAPACITY_EXHAUSTED;
	if (dispatch->generation >= X86_NATIVE_IRQ_GENERATION_MAX)
		return X86_NATIVE_IRQ_CAPACITY_EXHAUSTED;
	clear_storage(dispatch->lines, dispatch->line_capacity,
		      dispatch->actions, dispatch->action_capacity);
	for (index = 0u; index < line_count; ++index) {
		dispatch->lines[index] = (struct x86_native_irq_line_slot){
			.vector = lines[index].vector,
			.hardware_irq = lines[index].hardware_irq,
			.flags = lines[index].flags,
			.first_action = X86_NATIVE_IRQ_INVALID_SLOT,
			.action_count = 0u,
			.active = 1u,
			.reserved = {0u},
		};
	}
	dispatch->generation++;
	dispatch->identity = config->identity;
	dispatch->controller_identity = config->controller_identity;
	dispatch->controller_context = config->controller_context;
	dispatch->controller = config->controller;
	dispatch->next_sequence = 0u;
	dispatch->handled_count = 0u;
	dispatch->unhandled_count = 0u;
	dispatch->spurious_count = 0u;
	dispatch->fault_count = 0u;
	dispatch->line_count = line_count;
	dispatch->phase = X86_NATIVE_IRQ_DISPATCH_PREPARED;
	return X86_NATIVE_IRQ_OK;
}

enum x86_native_irq_status x86_native_irq_dispatch_publish(
	struct x86_native_irq_dispatch *dispatch)
{
	enum x86_native_irq_controller_result result;

	if (dispatch == NULL)
		return X86_NATIVE_IRQ_INVALID_ARGUMENT;
	if (dispatch->lifecycle_cookie != X86_NATIVE_IRQ_DISPATCH_COOKIE ||
	    dispatch->initialized != 1u ||
	    dispatch->phase != X86_NATIVE_IRQ_DISPATCH_PREPARED)
		return X86_NATIVE_IRQ_INVALID_STATE;
	result = dispatch->controller.resume(dispatch->controller_context,
					     dispatch->identity);
	if (result == X86_NATIVE_IRQ_CONTROLLER_RESULT_OK) {
		dispatch->phase = X86_NATIVE_IRQ_DISPATCH_ACTIVE;
		return X86_NATIVE_IRQ_OK;
	}
	if (result == X86_NATIVE_IRQ_CONTROLLER_RESULT_REJECTED)
		return X86_NATIVE_IRQ_CONTROLLER_REJECTED;
	dispatch->phase = X86_NATIVE_IRQ_DISPATCH_POISONED_PHASE;
	return X86_NATIVE_IRQ_POISONED;
}

enum x86_native_irq_status x86_native_irq_dispatch_abort(
	struct x86_native_irq_dispatch *dispatch)
{
	if (dispatch == NULL)
		return X86_NATIVE_IRQ_INVALID_ARGUMENT;
	if (dispatch->lifecycle_cookie != X86_NATIVE_IRQ_DISPATCH_COOKIE ||
	    dispatch->initialized != 1u ||
	    dispatch->phase != X86_NATIVE_IRQ_DISPATCH_PREPARED ||
	    dispatch->action_count != 0u)
		return X86_NATIVE_IRQ_INVALID_STATE;
	clear_lifetime(dispatch);
	return X86_NATIVE_IRQ_OK;
}

enum x86_native_irq_status x86_native_irq_dispatch_quiesce(
	struct x86_native_irq_dispatch *dispatch,
	kernel_object_handle_t identity)
{
	enum x86_native_irq_controller_result controller_result;
	enum x86_native_irq_status status =
		dispatch_identity_status(dispatch, identity);

	if (status != X86_NATIVE_IRQ_OK)
		return status;
	if (dispatch->phase != X86_NATIVE_IRQ_DISPATCH_ACTIVE)
		return X86_NATIVE_IRQ_INVALID_STATE;
	if (dispatch->dispatch_active != 0u)
		return X86_NATIVE_IRQ_BUSY;
	/* Close dispatch before masking and draining the controller source. */
	dispatch->phase = X86_NATIVE_IRQ_DISPATCH_QUIESCED;
	controller_result = dispatch->controller.quiesce(
		dispatch->controller_context, dispatch->identity);
	if (controller_result == X86_NATIVE_IRQ_CONTROLLER_RESULT_OK)
		return X86_NATIVE_IRQ_OK;
	dispatch->phase = X86_NATIVE_IRQ_DISPATCH_POISONED_PHASE;
	return X86_NATIVE_IRQ_POISONED;
}

enum x86_native_irq_status x86_native_irq_dispatch_resume(
	struct x86_native_irq_dispatch *dispatch,
	kernel_object_handle_t identity)
{
	enum x86_native_irq_controller_result controller_result;
	enum x86_native_irq_status status =
		dispatch_identity_status(dispatch, identity);

	if (status != X86_NATIVE_IRQ_OK)
		return status;
	if (dispatch->phase != X86_NATIVE_IRQ_DISPATCH_QUIESCED ||
	    dispatch->dispatch_active != 0u)
		return X86_NATIVE_IRQ_INVALID_STATE;
	controller_result = dispatch->controller.resume(
		dispatch->controller_context, dispatch->identity);
	if (controller_result == X86_NATIVE_IRQ_CONTROLLER_RESULT_OK) {
		dispatch->phase = X86_NATIVE_IRQ_DISPATCH_ACTIVE;
		return X86_NATIVE_IRQ_OK;
	}
	if (controller_result == X86_NATIVE_IRQ_CONTROLLER_RESULT_REJECTED)
		return X86_NATIVE_IRQ_CONTROLLER_REJECTED;
	dispatch->phase = X86_NATIVE_IRQ_DISPATCH_POISONED_PHASE;
	return X86_NATIVE_IRQ_POISONED;
}

enum x86_native_irq_status x86_native_irq_dispatch_retire(
	struct x86_native_irq_dispatch *dispatch,
	kernel_object_handle_t identity)
{
	enum x86_native_irq_status status =
		dispatch_identity_status(dispatch, identity);

	if (status != X86_NATIVE_IRQ_OK)
		return status;
	if (dispatch->phase != X86_NATIVE_IRQ_DISPATCH_QUIESCED ||
	    dispatch->action_count != 0u || dispatch->dispatch_active != 0u)
		return X86_NATIVE_IRQ_INVALID_STATE;
	clear_lifetime(dispatch);
	return X86_NATIVE_IRQ_OK;
}

enum x86_native_irq_status x86_native_irq_dispatch_poison(
	struct x86_native_irq_dispatch *dispatch,
	kernel_object_handle_t identity)
{
	enum x86_native_irq_status status =
		dispatch_identity_status(dispatch, identity);

	if (status != X86_NATIVE_IRQ_OK)
		return status;
	dispatch->phase = X86_NATIVE_IRQ_DISPATCH_POISONED_PHASE;
	return X86_NATIVE_IRQ_OK;
}

enum x86_native_irq_status x86_native_irq_dispatch_snapshot(
	const struct x86_native_irq_dispatch *dispatch,
	struct x86_native_irq_dispatch_snapshot *snapshot)
{
	if (dispatch == NULL || snapshot == NULL)
		return X86_NATIVE_IRQ_INVALID_ARGUMENT;
	if (dispatch->lifecycle_cookie != X86_NATIVE_IRQ_DISPATCH_COOKIE ||
	    dispatch->initialized != 1u)
		return X86_NATIVE_IRQ_INVALID_STATE;
	if (dispatch->phase == X86_NATIVE_IRQ_DISPATCH_ACTIVE ||
	    dispatch->dispatch_active != 0u)
		return X86_NATIVE_IRQ_BUSY;
	*snapshot = (struct x86_native_irq_dispatch_snapshot){
		.identity = dispatch->identity,
		.controller_identity = dispatch->controller_identity,
		.generation = dispatch->generation,
		.next_sequence = dispatch->next_sequence,
		.handled_count = dispatch->handled_count,
		.unhandled_count = dispatch->unhandled_count,
		.spurious_count = dispatch->spurious_count,
		.fault_count = dispatch->fault_count,
		.line_capacity = dispatch->line_capacity,
		.action_capacity = dispatch->action_capacity,
		.line_count = dispatch->line_count,
		.action_count = dispatch->action_count,
		.phase = dispatch->phase,
		.dispatch_active = dispatch->dispatch_active,
		.reserved = {0u},
	};
	return X86_NATIVE_IRQ_OK;
}

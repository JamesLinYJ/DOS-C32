// SPDX-License-Identifier: GPL-2.0-only
/* Native vector dispatch: controller begin, actions, then controller end. */
#include "native_private.h"

static bool bytes_are_zero(const uint8_t *bytes, size_t count)
{
	size_t index;

	for (index = 0u; index < count; ++index) {
		if (bytes[index] != 0u)
			return false;
	}
	return true;
}

static uint64_t saturating_increment(uint64_t value)
{
	return value == (uint64_t)-1 ? value : value + 1u;
}

static struct x86_native_irq_line_slot *find_vector(
	struct x86_native_irq_dispatch *dispatch, uint32_t vector)
{
	uint32_t index;

	for (index = 0u; index < dispatch->line_capacity; ++index) {
		if (dispatch->lines[index].active != 0u &&
		    dispatch->lines[index].vector == vector)
			return &dispatch->lines[index];
	}
	return NULL;
}

static bool observation_is_valid(
	const struct x86_native_irq_observation *observation)
{
	return observation != NULL &&
	       (observation->kind == X86_NATIVE_IRQ_OBSERVATION_DELIVER ||
		observation->kind == X86_NATIVE_IRQ_OBSERVATION_SPURIOUS) &&
	       bytes_are_zero(observation->reserved,
			      ARRAY_SIZE(observation->reserved));
}

static enum x86_native_irq_completion dispatch_actions(
	struct x86_native_irq_dispatch *dispatch,
	const struct x86_native_irq_line_slot *line,
	const struct x86_native_irq_event *event)
{
	uint32_t action = line->first_action;
	uint32_t steps = 0u;
	bool handled = false;
	bool faulted = false;

	while (action != X86_NATIVE_IRQ_INVALID_SLOT) {
		struct x86_native_irq_action_slot *slot;
		enum x86_native_irq_action_result result;

		if (action >= dispatch->action_capacity ||
		    steps++ >= dispatch->action_capacity) {
			faulted = true;
			break;
		}
		slot = &dispatch->actions[action];
		if (slot->active == 0u ||
		    slot->hardware_irq != line->hardware_irq) {
			faulted = true;
			break;
		}
		action = slot->next_action;
		if (slot->accepting == 0u)
			continue;
		slot->in_flight = 1u;
		result = slot->handler(slot->context, event);
		slot->in_flight = 0u;
		if (result == X86_NATIVE_IRQ_ACTION_HANDLED) {
			handled = true;
			slot->handled_count =
				saturating_increment(slot->handled_count);
		} else if (result == X86_NATIVE_IRQ_ACTION_UNHANDLED) {
			slot->unhandled_count =
				saturating_increment(slot->unhandled_count);
		} else {
			faulted = true;
			slot->fault_count =
				saturating_increment(slot->fault_count);
		}
	}
	if (faulted)
		return X86_NATIVE_IRQ_COMPLETE_HANDLER_FAULT;
	return handled ? X86_NATIVE_IRQ_COMPLETE_HANDLED
		       : X86_NATIVE_IRQ_COMPLETE_UNHANDLED;
}

static enum x86_native_irq_status finish_dispatch(
	struct x86_native_irq_dispatch *dispatch,
	const struct x86_native_irq_event *event,
	const struct x86_native_irq_observation *observation,
	enum x86_native_irq_completion completion)
{
	enum x86_native_irq_controller_result result;

	result = dispatch->controller.end(dispatch->controller_context, event,
					  observation, completion);
	dispatch->dispatch_active = 0u;
	if (result != X86_NATIVE_IRQ_CONTROLLER_RESULT_OK) {
		dispatch->phase = X86_NATIVE_IRQ_DISPATCH_POISONED_PHASE;
		return X86_NATIVE_IRQ_POISONED;
	}
	if (completion == X86_NATIVE_IRQ_COMPLETE_SPURIOUS) {
		dispatch->spurious_count =
			saturating_increment(dispatch->spurious_count);
		return X86_NATIVE_IRQ_SPURIOUS;
	}
	if (completion == X86_NATIVE_IRQ_COMPLETE_HANDLER_FAULT) {
		dispatch->fault_count =
			saturating_increment(dispatch->fault_count);
		return X86_NATIVE_IRQ_HANDLER_FAULT;
	}
	if (completion == X86_NATIVE_IRQ_COMPLETE_HANDLED) {
		dispatch->handled_count =
			saturating_increment(dispatch->handled_count);
		return X86_NATIVE_IRQ_OK;
	}
	dispatch->unhandled_count =
		saturating_increment(dispatch->unhandled_count);
	return X86_NATIVE_IRQ_UNHANDLED;
}

enum x86_native_irq_status x86_native_irq_dispatch_vector(
	struct x86_native_irq_dispatch *dispatch, uint32_t vector)
{
	struct x86_native_irq_observation observation = {0};
	struct x86_native_irq_line_slot *line;
	struct x86_native_irq_event event;
	enum x86_native_irq_controller_result begin_result;
	enum x86_native_irq_completion completion;

	if (dispatch == NULL)
		return X86_NATIVE_IRQ_INVALID_ARGUMENT;
	if (dispatch->lifecycle_cookie != X86_NATIVE_IRQ_DISPATCH_COOKIE ||
	    dispatch->initialized != 1u ||
	    dispatch->phase != X86_NATIVE_IRQ_DISPATCH_ACTIVE)
		return dispatch->phase == X86_NATIVE_IRQ_DISPATCH_POISONED_PHASE
			       ? X86_NATIVE_IRQ_POISONED
			       : X86_NATIVE_IRQ_INVALID_STATE;
	line = find_vector(dispatch, vector);
	if (line == NULL)
		return X86_NATIVE_IRQ_NOT_MAPPED;
	if (dispatch->dispatch_active != 0u)
		return X86_NATIVE_IRQ_BUSY;
	if (dispatch->next_sequence >= X86_NATIVE_IRQ_GENERATION_MAX)
		return X86_NATIVE_IRQ_CAPACITY_EXHAUSTED;
	dispatch->next_sequence++;
	event = (struct x86_native_irq_event){
		.controller_identity = dispatch->controller_identity,
		.controller_generation = dispatch->generation,
		.sequence = dispatch->next_sequence,
		.vector = line->vector,
		.hardware_irq = line->hardware_irq,
		.line_flags = line->flags,
		.reserved = {0u},
	};
	dispatch->dispatch_active = 1u;
	begin_result = dispatch->controller.begin(
		dispatch->controller_context, &event, &observation);
	if (begin_result == X86_NATIVE_IRQ_CONTROLLER_RESULT_REJECTED) {
		dispatch->dispatch_active = 0u;
		return X86_NATIVE_IRQ_CONTROLLER_REJECTED;
	}
	if (begin_result != X86_NATIVE_IRQ_CONTROLLER_RESULT_OK) {
		dispatch->dispatch_active = 0u;
		dispatch->phase = X86_NATIVE_IRQ_DISPATCH_POISONED_PHASE;
		return X86_NATIVE_IRQ_POISONED;
	}
	if (!observation_is_valid(&observation)) {
		observation.kind = X86_NATIVE_IRQ_OBSERVATION_DELIVER;
		completion = X86_NATIVE_IRQ_COMPLETE_HANDLER_FAULT;
		(void)dispatch->controller.end(dispatch->controller_context,
					       &event, &observation, completion);
		dispatch->dispatch_active = 0u;
		dispatch->phase = X86_NATIVE_IRQ_DISPATCH_POISONED_PHASE;
		return X86_NATIVE_IRQ_POISONED;
	}
	if (observation.kind == X86_NATIVE_IRQ_OBSERVATION_SPURIOUS)
		completion = X86_NATIVE_IRQ_COMPLETE_SPURIOUS;
	else
		completion = dispatch_actions(dispatch, line, &event);
	return finish_dispatch(dispatch, &event, &observation, completion);
}

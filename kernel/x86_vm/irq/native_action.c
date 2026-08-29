// SPDX-License-Identifier: GPL-2.0-only
/* Registered native IRQ action topology; mutation requires quiescence. */
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

static void clear_action(struct x86_native_irq_action_slot *action)
{
	uint64_t generation = action->generation;

	*action = (struct x86_native_irq_action_slot){
		.identity = KERNEL_OBJECT_HANDLE_INVALID,
		.context = KERNEL_OBJECT_HANDLE_INVALID,
		.generation = generation,
		.next_action = X86_NATIVE_IRQ_INVALID_SLOT,
	};
}

static struct x86_native_irq_line_slot *find_hardware_irq(
	struct x86_native_irq_dispatch *dispatch, uint32_t hardware_irq)
{
	uint32_t index;

	for (index = 0u; index < dispatch->line_capacity; ++index) {
		if (dispatch->lines[index].active != 0u &&
		    dispatch->lines[index].hardware_irq == hardware_irq)
			return &dispatch->lines[index];
	}
	return NULL;
}

static bool action_config_is_valid(
	const struct x86_native_irq_action_config *config)
{
	return config != NULL && identity_is_valid(config->identity) &&
	       identity_is_valid(config->context) && config->handler != NULL &&
	       config->shared <= 1u &&
	       bytes_are_zero(config->reserved, ARRAY_SIZE(config->reserved));
}

static enum x86_native_irq_status action_slot_status(
	struct x86_native_irq_dispatch *dispatch,
	const struct x86_native_irq_action_binding *binding,
	struct x86_native_irq_action_slot **result)
{
	struct x86_native_irq_action_slot *slot;

	if (dispatch == NULL || binding == NULL || result == NULL ||
	    !identity_is_valid(binding->dispatch_identity) ||
	    !identity_is_valid(binding->action_identity) ||
	    !bytes_are_zero(binding->reserved, ARRAY_SIZE(binding->reserved)) ||
	    binding->slot >= dispatch->action_capacity)
		return X86_NATIVE_IRQ_INVALID_ARGUMENT;
	if (dispatch->lifecycle_cookie != X86_NATIVE_IRQ_DISPATCH_COOKIE ||
	    dispatch->initialized != 1u ||
	    dispatch->phase == X86_NATIVE_IRQ_DISPATCH_EMPTY ||
	    dispatch->phase == X86_NATIVE_IRQ_DISPATCH_UNINITIALIZED)
		return X86_NATIVE_IRQ_INVALID_STATE;
	if (dispatch->phase == X86_NATIVE_IRQ_DISPATCH_POISONED_PHASE)
		return X86_NATIVE_IRQ_POISONED;
	if (binding->dispatch_identity != dispatch->identity)
		return X86_NATIVE_IRQ_IDENTITY_MISMATCH;
	if (binding->dispatch_generation != dispatch->generation)
		return X86_NATIVE_IRQ_STALE_BINDING;
	slot = &dispatch->actions[binding->slot];
	if (slot->active == 0u ||
	    slot->generation != binding->action_generation)
		return X86_NATIVE_IRQ_STALE_BINDING;
	if (slot->identity != binding->action_identity)
		return X86_NATIVE_IRQ_IDENTITY_MISMATCH;
	*result = slot;
	return X86_NATIVE_IRQ_OK;
}

static bool action_identity_is_registered(
	const struct x86_native_irq_dispatch *dispatch,
	kernel_object_handle_t identity)
{
	uint32_t index;

	for (index = 0u; index < dispatch->action_capacity; ++index) {
		if (dispatch->actions[index].active != 0u &&
		    dispatch->actions[index].identity == identity)
			return true;
	}
	return false;
}

static bool line_accepts_action(
	const struct x86_native_irq_dispatch *dispatch,
	const struct x86_native_irq_line_slot *line, uint8_t shared)
{
	uint32_t action = line->first_action;
	uint32_t steps = 0u;

	if (line->action_count == 0u)
		return true;
	if ((line->flags & X86_NATIVE_IRQ_LINE_SHAREABLE) == 0u || shared == 0u)
		return false;
	while (action != X86_NATIVE_IRQ_INVALID_SLOT) {
		if (action >= dispatch->action_capacity ||
		    steps++ >= dispatch->action_capacity ||
		    dispatch->actions[action].active == 0u ||
		    dispatch->actions[action].shared == 0u)
			return false;
		action = dispatch->actions[action].next_action;
	}
	return true;
}

static void append_action(struct x86_native_irq_dispatch *dispatch,
	struct x86_native_irq_line_slot *line, uint32_t action_index)
{
	uint32_t *link = &line->first_action;
	uint32_t steps = 0u;

	while (*link != X86_NATIVE_IRQ_INVALID_SLOT &&
	       steps++ < dispatch->action_capacity)
		link = &dispatch->actions[*link].next_action;
	*link = action_index;
	line->action_count++;
}

enum x86_native_irq_status x86_native_irq_action_register(
	struct x86_native_irq_dispatch *dispatch,
	const struct x86_native_irq_action_config *config,
	struct x86_native_irq_action_binding *binding)
{
	struct x86_native_irq_action_slot *slot = NULL;
	struct x86_native_irq_line_slot *line;
	uint32_t index;

	if (dispatch == NULL || binding == NULL ||
	    !action_config_is_valid(config))
		return X86_NATIVE_IRQ_INVALID_ARGUMENT;
	if (dispatch->lifecycle_cookie != X86_NATIVE_IRQ_DISPATCH_COOKIE ||
	    dispatch->initialized != 1u ||
	    (dispatch->phase != X86_NATIVE_IRQ_DISPATCH_PREPARED &&
	     dispatch->phase != X86_NATIVE_IRQ_DISPATCH_QUIESCED))
		return X86_NATIVE_IRQ_INVALID_STATE;
	if (config->identity == dispatch->identity ||
	    config->identity == dispatch->controller_identity)
		return X86_NATIVE_IRQ_INVALID_ARGUMENT;
	line = find_hardware_irq(dispatch, config->hardware_irq);
	if (line == NULL)
		return X86_NATIVE_IRQ_NOT_MAPPED;
	if (!line_accepts_action(dispatch, line, config->shared))
		return X86_NATIVE_IRQ_BUSY;
	if (action_identity_is_registered(dispatch, config->identity))
		return X86_NATIVE_IRQ_ALREADY_REGISTERED;
	for (index = 0u; index < dispatch->action_capacity; ++index) {
		if (dispatch->actions[index].active == 0u &&
		    dispatch->actions[index].generation <
			    X86_NATIVE_IRQ_GENERATION_MAX) {
			slot = &dispatch->actions[index];
			break;
		}
	}
	if (slot == NULL)
		return X86_NATIVE_IRQ_CAPACITY_EXHAUSTED;
	slot->generation++;
	slot->identity = config->identity;
	slot->context = config->context;
	slot->handled_count = 0u;
	slot->unhandled_count = 0u;
	slot->fault_count = 0u;
	slot->handler = config->handler;
	slot->hardware_irq = config->hardware_irq;
	slot->next_action = X86_NATIVE_IRQ_INVALID_SLOT;
	slot->shared = config->shared;
	slot->active = 1u;
	slot->accepting = 1u;
	slot->in_flight = 0u;
	index = (uint32_t)(slot - dispatch->actions);
	append_action(dispatch, line, index);
	dispatch->action_count++;
	*binding = (struct x86_native_irq_action_binding){
		.dispatch_identity = dispatch->identity,
		.dispatch_generation = dispatch->generation,
		.action_identity = slot->identity,
		.action_generation = slot->generation,
		.slot = index,
		.reserved = {0u},
	};
	return X86_NATIVE_IRQ_OK;
}

enum x86_native_irq_status x86_native_irq_action_quiesce(
	struct x86_native_irq_dispatch *dispatch,
	const struct x86_native_irq_action_binding *binding)
{
	struct x86_native_irq_action_slot *slot;
	enum x86_native_irq_status status =
		action_slot_status(dispatch, binding, &slot);

	if (status != X86_NATIVE_IRQ_OK)
		return status;
	if (dispatch->phase != X86_NATIVE_IRQ_DISPATCH_PREPARED &&
	    dispatch->phase != X86_NATIVE_IRQ_DISPATCH_QUIESCED)
		return X86_NATIVE_IRQ_INVALID_STATE;
	if (slot->accepting == 0u)
		return X86_NATIVE_IRQ_INVALID_STATE;
	if (slot->in_flight != 0u)
		return X86_NATIVE_IRQ_BUSY;
	slot->accepting = 0u;
	return X86_NATIVE_IRQ_OK;
}

static bool unlink_action(struct x86_native_irq_dispatch *dispatch,
	struct x86_native_irq_line_slot *line, uint32_t action_index)
{
	uint32_t *link = &line->first_action;
	uint32_t steps = 0u;

	while (*link != X86_NATIVE_IRQ_INVALID_SLOT &&
	       steps++ < dispatch->action_capacity) {
		if (*link == action_index) {
			*link = dispatch->actions[action_index].next_action;
			line->action_count--;
			return true;
		}
		link = &dispatch->actions[*link].next_action;
	}
	return false;
}

enum x86_native_irq_status x86_native_irq_action_unregister(
	struct x86_native_irq_dispatch *dispatch,
	const struct x86_native_irq_action_binding *binding)
{
	struct x86_native_irq_action_slot *slot;
	struct x86_native_irq_line_slot *line;
	enum x86_native_irq_status status =
		action_slot_status(dispatch, binding, &slot);

	if (status != X86_NATIVE_IRQ_OK)
		return status;
	if (dispatch->phase != X86_NATIVE_IRQ_DISPATCH_PREPARED &&
	    dispatch->phase != X86_NATIVE_IRQ_DISPATCH_QUIESCED)
		return X86_NATIVE_IRQ_INVALID_STATE;
	if (slot->accepting != 0u || slot->in_flight != 0u ||
	    dispatch->dispatch_active != 0u)
		return X86_NATIVE_IRQ_BUSY;
	line = find_hardware_irq(dispatch, slot->hardware_irq);
	if (line == NULL || !unlink_action(dispatch, line, binding->slot)) {
		dispatch->phase = X86_NATIVE_IRQ_DISPATCH_POISONED_PHASE;
		return X86_NATIVE_IRQ_POISONED;
	}
	clear_action(slot);
	dispatch->action_count--;
	return X86_NATIVE_IRQ_OK;
}

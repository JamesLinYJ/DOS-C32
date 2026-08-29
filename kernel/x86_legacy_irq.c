// SPDX-License-Identifier: GPL-2.0-only
/* Selected native PIC/PIT domain over the generic descriptor/action core. */
#include "x86_legacy_irq.h"

#include "../config/x86-legacy-irq.h"

#define LEGACY_IRQ_GENERATION_MAX ((uint64_t)-2)

struct x86_legacy_irq_owner {
	struct x86_legacy_irq_config config;
	struct x86_native_irq_dispatch dispatch;
	struct x86_native_irq_line_slot lines[X86_LEGACY_IRQ_COUNT];
	struct x86_native_irq_action_slot
		actions[CONFIG_X86_LEGACY_IRQ_ACTION_CAPACITY];
	uint64_t generation;
	uint16_t line_count;
	uint8_t phase;
	uint8_t dispatch_constructed;
	uint8_t reserved[4];
} __aligned(8);

static struct x86_legacy_irq_owner owner;

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

static bool config_is_valid(const struct x86_legacy_irq_config *config)
{
	return config != NULL && identity_is_valid(config->source_identity) &&
	       identity_is_valid(config->controller_identity) &&
	       identity_is_valid(config->dispatch_identity) &&
	       config->source_identity != config->controller_identity &&
	       config->source_identity != config->dispatch_identity &&
	       config->controller_identity != config->dispatch_identity &&
	       bytes_are_zero(config->reserved, ARRAY_SIZE(config->reserved));
}

static enum x86_legacy_irq_status map_native_irq(
	enum x86_native_irq_status status)
{
	switch (status) {
	case X86_NATIVE_IRQ_OK:
		return X86_LEGACY_IRQ_OK;
	case X86_NATIVE_IRQ_INVALID_ARGUMENT:
		return X86_LEGACY_IRQ_INVALID_ARGUMENT;
	case X86_NATIVE_IRQ_INVALID_STATE:
		return X86_LEGACY_IRQ_INVALID_STATE;
	case X86_NATIVE_IRQ_CAPACITY_EXHAUSTED:
		return X86_LEGACY_IRQ_CAPACITY_EXHAUSTED;
	case X86_NATIVE_IRQ_IDENTITY_MISMATCH:
		return X86_LEGACY_IRQ_IDENTITY_MISMATCH;
	case X86_NATIVE_IRQ_STALE_BINDING:
		return X86_LEGACY_IRQ_STALE_BINDING;
	case X86_NATIVE_IRQ_NOT_MAPPED:
		return X86_LEGACY_IRQ_NOT_MAPPED;
	case X86_NATIVE_IRQ_BUSY:
	case X86_NATIVE_IRQ_ALREADY_REGISTERED:
		return X86_LEGACY_IRQ_BUSY;
	case X86_NATIVE_IRQ_SPURIOUS:
		return X86_LEGACY_IRQ_SPURIOUS;
	case X86_NATIVE_IRQ_UNHANDLED:
		return X86_LEGACY_IRQ_UNHANDLED;
	case X86_NATIVE_IRQ_HANDLER_FAULT:
		return X86_LEGACY_IRQ_HANDLER_FAULT;
	case X86_NATIVE_IRQ_CONTROLLER_REJECTED:
		return X86_LEGACY_IRQ_CONTROLLER_REJECTED;
	case X86_NATIVE_IRQ_POISONED:
	default:
		return X86_LEGACY_IRQ_POISONED;
	}
}

static enum x86_legacy_irq_status map_pic(enum x86_legacy_pic_status status)
{
	switch (status) {
	case X86_LEGACY_PIC_OK:
		return X86_LEGACY_IRQ_OK;
	case X86_LEGACY_PIC_INVALID_ARGUMENT:
		return X86_LEGACY_IRQ_INVALID_ARGUMENT;
	case X86_LEGACY_PIC_INVALID_STATE:
		return X86_LEGACY_IRQ_INVALID_STATE;
	case X86_LEGACY_PIC_CAPACITY_EXHAUSTED:
		return X86_LEGACY_IRQ_CAPACITY_EXHAUSTED;
	case X86_LEGACY_PIC_IDENTITY_MISMATCH:
		return X86_LEGACY_IRQ_IDENTITY_MISMATCH;
	case X86_LEGACY_PIC_BUSY:
		return X86_LEGACY_IRQ_BUSY;
	case X86_LEGACY_PIC_UNAVAILABLE:
		return X86_LEGACY_IRQ_NOT_MAPPED;
	case X86_LEGACY_PIC_POISONED:
	default:
		return X86_LEGACY_IRQ_POISONED;
	}
}

static enum x86_legacy_irq_status owner_status(
	kernel_object_handle_t source_identity)
{
	if (!identity_is_valid(source_identity))
		return X86_LEGACY_IRQ_INVALID_ARGUMENT;
	if (owner.phase == X86_LEGACY_IRQ_EMPTY)
		return X86_LEGACY_IRQ_INVALID_STATE;
	if (owner.config.source_identity != source_identity)
		return X86_LEGACY_IRQ_IDENTITY_MISMATCH;
	return owner.phase == X86_LEGACY_IRQ_POISONED_PHASE
		       ? X86_LEGACY_IRQ_POISONED
		       : X86_LEGACY_IRQ_OK;
}

static void clear_lifetime(void)
{
	struct x86_native_irq_dispatch dispatch = owner.dispatch;
	uint64_t generation = owner.generation;
	uint8_t constructed = owner.dispatch_constructed;

	owner = (struct x86_legacy_irq_owner){
		.dispatch = dispatch,
		.generation = generation,
		.phase = X86_LEGACY_IRQ_EMPTY,
		.dispatch_constructed = constructed,
	};
}

static void poison_owner(void)
{
	enum x86_native_irq_status dispatch_status;
	enum x86_legacy_pic_status pic_status;

	if (owner.phase == X86_LEGACY_IRQ_EMPTY)
		return;
	pic_status = x86_legacy_pic_poison(owner.config.controller_identity);
	dispatch_status = x86_native_irq_dispatch_poison(
		&owner.dispatch, owner.config.dispatch_identity);
	(void)pic_status;
	(void)dispatch_status;
	owner.phase = X86_LEGACY_IRQ_POISONED_PHASE;
}

static bool enabled_actions_complete(void)
{
	uint32_t index;

	for (index = 0u; index < owner.dispatch.line_capacity; ++index) {
		const struct x86_native_irq_line_slot *line =
			&owner.dispatch.lines[index];

		if (line->active == 0u || line->hardware_irq == 2u ||
		    (owner.config.enabled_irq_mask &
		     (uint16_t)(1u << line->hardware_irq)) == 0u)
			continue;
		if (line->action_count == 0u)
			return false;
	}
	return (owner.config.enabled_irq_mask & (uint16_t)(1u << 2u)) == 0u;
}

enum x86_legacy_irq_status x86_legacy_irq_prepare(
	const struct x86_legacy_irq_config *config)
{
	struct x86_native_irq_line_config discovered[X86_LEGACY_IRQ_COUNT];
	struct x86_native_irq_dispatch_config dispatch_config;
	struct x86_legacy_pic_config pic_config;
	enum x86_native_irq_status dispatch_status;
	enum x86_legacy_pic_status pic_status;
	uint32_t line_count;

	if (!config_is_valid(config))
		return X86_LEGACY_IRQ_INVALID_ARGUMENT;
	if (owner.phase == X86_LEGACY_IRQ_POISONED_PHASE)
		return X86_LEGACY_IRQ_POISONED;
	if (owner.phase != X86_LEGACY_IRQ_EMPTY)
		return X86_LEGACY_IRQ_INVALID_STATE;
	if (owner.generation >= LEGACY_IRQ_GENERATION_MAX)
		return X86_LEGACY_IRQ_CAPACITY_EXHAUSTED;
	if (owner.dispatch_constructed == 0u) {
		x86_native_irq_dispatch_construct(&owner.dispatch);
		dispatch_status = x86_native_irq_dispatch_initialize(
			&owner.dispatch, owner.lines, ARRAY_SIZE(owner.lines),
			owner.actions, ARRAY_SIZE(owner.actions));
		if (dispatch_status != X86_NATIVE_IRQ_OK)
			return map_native_irq(dispatch_status);
		owner.dispatch_constructed = 1u;
	}
	pic_config = (struct x86_legacy_pic_config){
		.controller_identity = config->controller_identity,
		.dispatch_identity = config->dispatch_identity,
		.pit_input_quantum = config->pit_input_quantum,
		.vector_base = config->vector_base,
		.present_irq_mask = config->present_irq_mask,
		.enabled_irq_mask = config->enabled_irq_mask,
		.pit_reload = config->pit_reload,
		.pit_rate_calibrated = config->pit_rate_calibrated,
		.present = config->present,
		.presence_evidence = config->presence_evidence,
		.reserved = {0u},
	};
	pic_status = x86_legacy_pic_prepare(
		&pic_config, discovered, ARRAY_SIZE(discovered), &line_count);
	if (pic_status != X86_LEGACY_PIC_OK)
		return map_pic(pic_status);
	dispatch_config = (struct x86_native_irq_dispatch_config){
		.identity = config->dispatch_identity,
		.controller_identity = config->controller_identity,
		.controller_context = config->controller_identity,
		.controller = x86_legacy_pic_controller_ops(),
	};
	dispatch_status = x86_native_irq_dispatch_prepare(
		&owner.dispatch, &dispatch_config, discovered, line_count);
	if (dispatch_status != X86_NATIVE_IRQ_OK) {
		if (x86_legacy_pic_abort(config->controller_identity) !=
		    X86_LEGACY_PIC_OK) {
			owner.config = *config;
			owner.phase = X86_LEGACY_IRQ_POISONED_PHASE;
			poison_owner();
			return X86_LEGACY_IRQ_POISONED;
		}
		return map_native_irq(dispatch_status);
	}
	owner.config = *config;
	owner.generation++;
	owner.line_count = (uint16_t)line_count;
	owner.phase = X86_LEGACY_IRQ_PREPARED;
	return X86_LEGACY_IRQ_OK;
}

enum x86_legacy_irq_status x86_legacy_irq_abort(
	kernel_object_handle_t source_identity)
{
	enum x86_legacy_irq_status status = owner_status(source_identity);

	if (status != X86_LEGACY_IRQ_OK)
		return status;
	if (owner.phase != X86_LEGACY_IRQ_PREPARED)
		return X86_LEGACY_IRQ_INVALID_STATE;
	if (x86_native_irq_dispatch_abort(&owner.dispatch) !=
		    X86_NATIVE_IRQ_OK ||
	    x86_legacy_pic_abort(owner.config.controller_identity) !=
		    X86_LEGACY_PIC_OK) {
		poison_owner();
		return X86_LEGACY_IRQ_POISONED;
	}
	clear_lifetime();
	return X86_LEGACY_IRQ_OK;
}

enum x86_legacy_irq_status x86_legacy_irq_publish(
	kernel_object_handle_t source_identity)
{
	enum x86_native_irq_status native_status;
	enum x86_legacy_irq_status status = owner_status(source_identity);

	if (status != X86_LEGACY_IRQ_OK)
		return status;
	if (owner.phase != X86_LEGACY_IRQ_PREPARED ||
	    !enabled_actions_complete())
		return X86_LEGACY_IRQ_INVALID_STATE;
	native_status = x86_native_irq_dispatch_publish(&owner.dispatch);
	status = map_native_irq(native_status);
	if (status == X86_LEGACY_IRQ_OK)
		owner.phase = X86_LEGACY_IRQ_ACTIVE;
	else if (status == X86_LEGACY_IRQ_POISONED)
		owner.phase = X86_LEGACY_IRQ_POISONED_PHASE;
	return status;
}

enum x86_legacy_irq_status x86_legacy_irq_quiesce(
	kernel_object_handle_t source_identity)
{
	enum x86_legacy_irq_status status = owner_status(source_identity);

	if (status != X86_LEGACY_IRQ_OK)
		return status;
	if (owner.phase != X86_LEGACY_IRQ_ACTIVE)
		return X86_LEGACY_IRQ_INVALID_STATE;
	status = map_native_irq(x86_native_irq_dispatch_quiesce(
		&owner.dispatch, owner.config.dispatch_identity));
	if (status == X86_LEGACY_IRQ_OK)
		owner.phase = X86_LEGACY_IRQ_QUIESCED;
	else if (status == X86_LEGACY_IRQ_POISONED)
		owner.phase = X86_LEGACY_IRQ_POISONED_PHASE;
	return status;
}

enum x86_legacy_irq_status x86_legacy_irq_resume(
	kernel_object_handle_t source_identity)
{
	enum x86_legacy_irq_status status = owner_status(source_identity);

	if (status != X86_LEGACY_IRQ_OK)
		return status;
	if (owner.phase != X86_LEGACY_IRQ_QUIESCED)
		return X86_LEGACY_IRQ_INVALID_STATE;
	status = map_native_irq(x86_native_irq_dispatch_resume(
		&owner.dispatch, owner.config.dispatch_identity));
	if (status == X86_LEGACY_IRQ_OK)
		owner.phase = X86_LEGACY_IRQ_ACTIVE;
	else if (status == X86_LEGACY_IRQ_POISONED)
		owner.phase = X86_LEGACY_IRQ_POISONED_PHASE;
	return status;
}

enum x86_legacy_irq_status x86_legacy_irq_retire(
	kernel_object_handle_t source_identity)
{
	enum x86_legacy_irq_status status = owner_status(source_identity);

	if (status != X86_LEGACY_IRQ_OK)
		return status;
	if (owner.phase != X86_LEGACY_IRQ_QUIESCED)
		return X86_LEGACY_IRQ_INVALID_STATE;
	if (x86_native_irq_dispatch_retire(
		    &owner.dispatch, owner.config.dispatch_identity) !=
		    X86_NATIVE_IRQ_OK ||
	    x86_legacy_pic_retire(owner.config.controller_identity,
				  owner.config.dispatch_identity) !=
		    X86_LEGACY_PIC_OK) {
		poison_owner();
		return X86_LEGACY_IRQ_POISONED;
	}
	clear_lifetime();
	return X86_LEGACY_IRQ_OK;
}

enum x86_legacy_irq_status x86_legacy_irq_action_register(
	kernel_object_handle_t source_identity,
	const struct x86_native_irq_action_config *config,
	struct x86_native_irq_action_binding *binding)
{
	enum x86_legacy_irq_status status = owner_status(source_identity);

	if (status != X86_LEGACY_IRQ_OK)
		return status;
	if (owner.phase != X86_LEGACY_IRQ_PREPARED &&
	    owner.phase != X86_LEGACY_IRQ_QUIESCED)
		return X86_LEGACY_IRQ_INVALID_STATE;
	return map_native_irq(x86_native_irq_action_register(
		&owner.dispatch, config, binding));
}

enum x86_legacy_irq_status x86_legacy_irq_action_quiesce(
	kernel_object_handle_t source_identity,
	const struct x86_native_irq_action_binding *binding)
{
	enum x86_legacy_irq_status status = owner_status(source_identity);

	if (status != X86_LEGACY_IRQ_OK)
		return status;
	return map_native_irq(
		x86_native_irq_action_quiesce(&owner.dispatch, binding));
}

enum x86_legacy_irq_status x86_legacy_irq_action_unregister(
	kernel_object_handle_t source_identity,
	const struct x86_native_irq_action_binding *binding)
{
	enum x86_legacy_irq_status status = owner_status(source_identity);

	if (status != X86_LEGACY_IRQ_OK)
		return status;
	return map_native_irq(
		x86_native_irq_action_unregister(&owner.dispatch, binding));
}

enum x86_legacy_irq_status x86_legacy_irq_dispatch_vector(uint32_t vector)
{
	enum x86_legacy_irq_status status;

	if (owner.phase != X86_LEGACY_IRQ_ACTIVE)
		return owner.phase == X86_LEGACY_IRQ_POISONED_PHASE
			       ? X86_LEGACY_IRQ_POISONED
			       : X86_LEGACY_IRQ_INVALID_STATE;
	status = map_native_irq(
		x86_native_irq_dispatch_vector(&owner.dispatch, vector));
	if (status == X86_LEGACY_IRQ_POISONED)
		owner.phase = X86_LEGACY_IRQ_POISONED_PHASE;
	return status;
}

bool x86_legacy_irq_is_initialized(void)
{
	return owner.phase == X86_LEGACY_IRQ_ACTIVE;
}

enum x86_legacy_irq_status x86_legacy_irq_source_info(
	kernel_object_handle_t source_identity,
	struct x86_legacy_irq_source_info *info)
{
	struct x86_legacy_irq_source_info prepared;
	enum x86_legacy_irq_status status = owner_status(source_identity);

	if (info == NULL)
		return X86_LEGACY_IRQ_INVALID_ARGUMENT;
	if (status != X86_LEGACY_IRQ_OK)
		return status;
	prepared = (struct x86_legacy_irq_source_info){
		.source_identity = owner.config.source_identity,
		.generation = owner.generation,
		.pit_input_quantum = owner.config.pit_input_quantum,
		.capabilities = X86_LEGACY_IRQ_SOURCE_PIT_CLOCK,
		.pit_rate_calibrated = owner.config.pit_rate_calibrated,
		.phase = owner.phase,
		.reserved = {0u},
	};
	*info = prepared;
	return X86_LEGACY_IRQ_OK;
}

enum x86_legacy_irq_status x86_legacy_irq_snapshot(
	kernel_object_handle_t source_identity,
	struct x86_legacy_irq_snapshot *snapshot)
{
	struct x86_native_irq_dispatch_snapshot dispatch;
	enum x86_legacy_irq_status status = owner_status(source_identity);

	if (snapshot == NULL)
		return X86_LEGACY_IRQ_INVALID_ARGUMENT;
	if (status != X86_LEGACY_IRQ_OK)
		return status;
	status = map_native_irq(
		x86_native_irq_dispatch_snapshot(&owner.dispatch, &dispatch));
	if (status != X86_LEGACY_IRQ_OK)
		return status;
	*snapshot = (struct x86_legacy_irq_snapshot){
		.source_identity = owner.config.source_identity,
		.controller_identity = owner.config.controller_identity,
		.dispatch_identity = owner.config.dispatch_identity,
		.generation = owner.generation,
		.handled_count = dispatch.handled_count,
		.unhandled_count = dispatch.unhandled_count,
		.spurious_count = dispatch.spurious_count,
		.fault_count = dispatch.fault_count,
		.vector_base = owner.config.vector_base,
		.present_irq_mask = owner.config.present_irq_mask,
		.enabled_irq_mask = owner.config.enabled_irq_mask,
		.line_count = owner.line_count,
		.action_count = (uint16_t)dispatch.action_count,
		.phase = owner.phase,
		.reserved = {0u},
	};
	return X86_LEGACY_IRQ_OK;
}

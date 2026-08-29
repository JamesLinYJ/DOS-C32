// SPDX-License-Identifier: GPL-2.0-only
/* Registration and lifetime ownership for the internal input core. */
#include "private.h"

bool input_internal_identity_valid(kernel_object_handle_t identity)
{
	return identity != 0u && identity != KERNEL_OBJECT_HANDLE_INVALID;
}

bool input_internal_bytes_zero(const uint8_t *bytes, size_t count)
{
	size_t index;

	for (index = 0u; index < count; ++index) {
		if (bytes[index] != 0u)
			return false;
	}
	return true;
}

uint64_t input_internal_saturating_increment(uint64_t value)
{
	return value == (uint64_t)-1 ? value : value + 1u;
}

void input_internal_guard_enter(const struct input_core *core)
{
	if (core->irq_enter != NULL)
		core->irq_enter(core->guard_context);
}

void input_internal_guard_exit(const struct input_core *core)
{
	if (core->irq_exit != NULL)
		core->irq_exit(core->guard_context);
}

bool input_internal_core_is_usable(const struct input_core *core)
{
	return core != NULL && core->lifecycle_cookie == INPUT_CORE_COOKIE &&
	       input_internal_identity_valid(core->identity) &&
	       core->phase != INPUT_CORE_UNINITIALIZED &&
	       core->phase != INPUT_CORE_RETIRED &&
	       core->phase != INPUT_CORE_POISONED_PHASE;
}

enum input_status input_internal_core_identity_status(
	const struct input_core *core, kernel_object_handle_t identity)
{
	if (core == NULL || !input_internal_identity_valid(identity))
		return INPUT_INVALID_ARGUMENT;
	if (core->lifecycle_cookie != INPUT_CORE_COOKIE ||
	    core->phase == INPUT_CORE_UNINITIALIZED ||
	    core->phase == INPUT_CORE_RETIRED)
		return INPUT_INVALID_STATE;
	if (core->identity != identity)
		return INPUT_IDENTITY_MISMATCH;
	if (core->phase == INPUT_CORE_POISONED_PHASE)
		return INPUT_POISONED;
	return INPUT_OK;
}

static bool core_config_is_valid(const struct input_core_config *config)
{
	bool has_enter;
	bool has_exit;

	if (config == NULL ||
	    !input_internal_identity_valid(config->identity) ||
	    config->caller_serializes_irq > 1u ||
	    !input_internal_bytes_zero(config->reserved,
				       ARRAY_SIZE(config->reserved)))
		return false;
	has_enter = config->irq_enter != NULL;
	has_exit = config->irq_exit != NULL;
	if (has_enter != has_exit)
		return false;
	if (!has_enter && config->caller_serializes_irq == 0u)
		return false;
	return has_enter ? input_internal_identity_valid(config->guard_context)
			 : config->guard_context == KERNEL_OBJECT_HANDLE_INVALID ||
				   config->guard_context == 0u;
}

static bool device_config_is_valid(const struct input_device_config *config)
{
	return config != NULL &&
	       input_internal_identity_valid(config->identity) &&
	       config->capabilities != 0u &&
	       (config->capabilities & ~INPUT_CAPABILITY_MASK) == 0u &&
	       config->queue != NULL && config->queue_capacity != 0u &&
	       input_internal_bytes_zero(config->reserved,
					 ARRAY_SIZE(config->reserved));
}

static bool handler_config_is_valid(const struct input_handler_config *config)
{
	return config != NULL &&
	       input_internal_identity_valid(config->identity) &&
	       input_internal_identity_valid(config->context) &&
	       config->handler_context != NULL &&
	       config->capabilities != 0u &&
	       (config->capabilities & ~INPUT_CAPABILITY_MASK) == 0u &&
	       config->focus_enter != NULL && config->focus_leave != NULL &&
	       config->receive != NULL &&
	       input_internal_bytes_zero(config->reserved,
					 ARRAY_SIZE(config->reserved));
}

static void clear_device_preserving_generation(struct input_device *device)
{
	uint64_t generation = device->generation;
	uint32_t cookie = device->lifecycle_cookie;
	uint8_t *bytes = (uint8_t *)device;
	size_t index;

	for (index = 0u; index < sizeof(*device); ++index)
		bytes[index] = 0u;
	device->generation = generation;
	device->lifecycle_cookie = cookie;
	device->registry_slot = INPUT_SLOT_INVALID;
}

static void clear_handler_preserving_generation(struct input_handler *handler)
{
	uint64_t generation = handler->generation;
	uint32_t cookie = handler->lifecycle_cookie;
	uint8_t *bytes = (uint8_t *)handler;
	size_t index;

	for (index = 0u; index < sizeof(*handler); ++index)
		bytes[index] = 0u;
	handler->generation = generation;
	handler->lifecycle_cookie = cookie;
	handler->registry_slot = INPUT_SLOT_INVALID;
}

void input_core_construct(struct input_core *core)
{
	if (core == NULL)
		return;
	*core = (struct input_core){
		.identity = KERNEL_OBJECT_HANDLE_INVALID,
		.guard_context = KERNEL_OBJECT_HANDLE_INVALID,
		.lifecycle_cookie = INPUT_CORE_COOKIE,
		.phase = INPUT_CORE_UNINITIALIZED,
	};
}

void input_device_construct(struct input_device *device)
{
	uint8_t *bytes = (uint8_t *)device;
	size_t index;

	if (device == NULL)
		return;
	for (index = 0u; index < sizeof(*device); ++index)
		bytes[index] = 0u;
	device->lifecycle_cookie = INPUT_DEVICE_COOKIE;
	device->registry_slot = INPUT_SLOT_INVALID;
}

void input_handler_construct(struct input_handler *handler)
{
	uint8_t *bytes = (uint8_t *)handler;
	size_t index;

	if (handler == NULL)
		return;
	for (index = 0u; index < sizeof(*handler); ++index)
		bytes[index] = 0u;
	handler->lifecycle_cookie = INPUT_HANDLER_COOKIE;
	handler->registry_slot = INPUT_SLOT_INVALID;
}

void *input_handler_context(const struct input_handler *handler)
{
	if (handler == NULL || handler->lifecycle_cookie != INPUT_HANDLER_COOKIE ||
	    handler->phase == INPUT_HANDLER_EMPTY)
		return NULL;
	return handler->config.handler_context;
}

enum input_status input_core_initialize(
	struct input_core *core, const struct input_core_config *config,
	struct input_device **devices, uint16_t device_capacity,
	struct input_handler **handlers, uint16_t handler_capacity)
{
	uint64_t generation;
	uint16_t slot;

	if (core == NULL || !core_config_is_valid(config) || devices == NULL ||
	    handlers == NULL || device_capacity == 0u || handler_capacity == 0u)
		return INPUT_INVALID_ARGUMENT;
	if (core->lifecycle_cookie != INPUT_CORE_COOKIE ||
	    (core->phase != INPUT_CORE_UNINITIALIZED &&
	     core->phase != INPUT_CORE_RETIRED))
		return INPUT_INVALID_STATE;
	if (core->generation >= INPUT_GENERATION_MAX)
		return INPUT_CAPACITY_EXHAUSTED;
	for (slot = 0u; slot < device_capacity; ++slot)
		devices[slot] = NULL;
	for (slot = 0u; slot < handler_capacity; ++slot)
		handlers[slot] = NULL;
	generation = core->generation + 1u;
	*core = (struct input_core){
		.identity = config->identity,
		.guard_context = config->guard_context,
		.generation = generation,
		.devices = devices,
		.handlers = handlers,
		.irq_enter = config->irq_enter,
		.irq_exit = config->irq_exit,
		.lifecycle_cookie = INPUT_CORE_COOKIE,
		.device_capacity = device_capacity,
		.handler_capacity = handler_capacity,
		.phase = INPUT_CORE_PREPARED,
		.caller_serializes_irq = config->caller_serializes_irq,
	};
	return INPUT_OK;
}

static bool core_has_in_flight(const struct input_core *core)
{
	uint16_t slot;

	if (core->dispatch_active != 0u)
		return true;
	for (slot = 0u; slot < core->device_capacity; ++slot) {
		if (core->devices[slot] != NULL &&
		    core->devices[slot]->in_flight != 0u)
			return true;
	}
	for (slot = 0u; slot < core->handler_capacity; ++slot) {
		if (core->handlers[slot] != NULL &&
		    core->handlers[slot]->in_flight != 0u)
			return true;
	}
	return false;
}

enum input_status input_core_replace_storage(
	struct input_core *core, kernel_object_handle_t identity,
	struct input_device **devices, uint16_t device_capacity,
	struct input_handler **handlers, uint16_t handler_capacity)
{
	uint16_t slot;
	enum input_status status =
		input_internal_core_identity_status(core, identity);

	if (status != INPUT_OK)
		return status;
	if (devices == NULL || handlers == NULL || devices == core->devices ||
	    handlers == core->handlers)
		return INPUT_INVALID_ARGUMENT;
	if (core->phase != INPUT_CORE_PREPARED &&
	    core->phase != INPUT_CORE_QUIESCED)
		return INPUT_INVALID_STATE;
	if (device_capacity < core->device_capacity ||
	    handler_capacity < core->handler_capacity)
		return INPUT_CAPACITY_EXHAUSTED;
	input_internal_guard_enter(core);
	if (core_has_in_flight(core)) {
		input_internal_guard_exit(core);
		return INPUT_BUSY;
	}
	for (slot = 0u; slot < device_capacity; ++slot)
		devices[slot] = slot < core->device_capacity
				? core->devices[slot]
				: NULL;
	for (slot = 0u; slot < handler_capacity; ++slot)
		handlers[slot] = slot < core->handler_capacity
				 ? core->handlers[slot]
				 : NULL;
	core->devices = devices;
	core->handlers = handlers;
	core->device_capacity = device_capacity;
	core->handler_capacity = handler_capacity;
	input_internal_guard_exit(core);
	return INPUT_OK;
}

enum input_status input_core_publish(struct input_core *core,
				     kernel_object_handle_t identity)
{
	enum input_status status =
		input_internal_core_identity_status(core, identity);

	if (status != INPUT_OK)
		return status;
	if (core->phase != INPUT_CORE_PREPARED)
		return INPUT_INVALID_STATE;
	input_internal_guard_enter(core);
	core->phase = INPUT_CORE_ACTIVE;
	input_internal_guard_exit(core);
	return INPUT_OK;
}

enum input_status input_core_quiesce(struct input_core *core,
				     kernel_object_handle_t identity)
{
	enum input_status status =
		input_internal_core_identity_status(core, identity);

	if (status != INPUT_OK)
		return status;
	if (core->phase != INPUT_CORE_ACTIVE)
		return INPUT_INVALID_STATE;
	input_internal_guard_enter(core);
	if (core_has_in_flight(core)) {
		input_internal_guard_exit(core);
		return INPUT_BUSY;
	}
	core->phase = INPUT_CORE_QUIESCED;
	input_internal_guard_exit(core);
	return INPUT_OK;
}

enum input_status input_core_resume(struct input_core *core,
				    kernel_object_handle_t identity)
{
	enum input_status status =
		input_internal_core_identity_status(core, identity);

	if (status != INPUT_OK)
		return status;
	if (core->phase != INPUT_CORE_QUIESCED)
		return INPUT_INVALID_STATE;
	input_internal_guard_enter(core);
	core->phase = INPUT_CORE_ACTIVE;
	input_internal_guard_exit(core);
	return INPUT_OK;
}

enum input_status input_core_retire(struct input_core *core,
				    kernel_object_handle_t identity)
{
	enum input_status status =
		input_internal_core_identity_status(core, identity);

	if (status != INPUT_OK)
		return status;
	if (core->phase != INPUT_CORE_QUIESCED || core->focus != NULL ||
	    core->device_count != 0u || core->handler_count != 0u ||
	    core_has_in_flight(core))
		return INPUT_INVALID_STATE;
	core->identity = KERNEL_OBJECT_HANDLE_INVALID;
	core->guard_context = KERNEL_OBJECT_HANDLE_INVALID;
	core->irq_enter = NULL;
	core->irq_exit = NULL;
	core->phase = INPUT_CORE_RETIRED;
	return INPUT_OK;
}

enum input_status input_core_poison(struct input_core *core,
				    kernel_object_handle_t identity)
{
	enum input_status status =
		input_internal_core_identity_status(core, identity);

	if (status != INPUT_OK)
		return status;
	input_internal_guard_enter(core);
	core->phase = INPUT_CORE_POISONED_PHASE;
	input_internal_guard_exit(core);
	return INPUT_OK;
}

static bool identity_is_registered(const struct input_core *core,
				   kernel_object_handle_t identity)
{
	uint16_t slot;

	if (core->identity == identity)
		return true;
	for (slot = 0u; slot < core->device_capacity; ++slot) {
		if (core->devices[slot] != NULL &&
		    core->devices[slot]->config.identity == identity)
			return true;
	}
	for (slot = 0u; slot < core->handler_capacity; ++slot) {
		if (core->handlers[slot] != NULL &&
		    core->handlers[slot]->config.identity == identity)
			return true;
	}
	return false;
}

enum input_status input_device_register(
	struct input_core *core, struct input_device *device,
	const struct input_device_config *config,
	struct input_device_binding *binding)
{
	uint16_t slot;
	uint64_t generation;

	if (!input_internal_core_is_usable(core) || device == NULL ||
	    binding == NULL || !device_config_is_valid(config))
		return INPUT_INVALID_ARGUMENT;
	if (core->phase != INPUT_CORE_PREPARED &&
	    core->phase != INPUT_CORE_QUIESCED)
		return INPUT_INVALID_STATE;
	if (device->lifecycle_cookie != INPUT_DEVICE_COOKIE)
		return INPUT_INVALID_ARGUMENT;
	if (device->phase == INPUT_DEVICE_POISONED_PHASE)
		return INPUT_POISONED;
	if (device->phase != INPUT_DEVICE_EMPTY)
		return INPUT_ALREADY_REGISTERED;
	if (identity_is_registered(core, config->identity))
		return INPUT_ALREADY_REGISTERED;
	for (slot = 0u; slot < core->device_capacity; ++slot) {
		if (core->devices[slot] == NULL)
			break;
	}
	if (slot == core->device_capacity ||
	    device->generation >= INPUT_GENERATION_MAX)
		return INPUT_CAPACITY_EXHAUSTED;
	generation = device->generation + 1u;
	clear_device_preserving_generation(device);
	device->generation = generation;
	device->config = *config;
	device->core = core;
	device->registry_slot = slot;
	device->phase = INPUT_DEVICE_ACTIVE;
	core->devices[slot] = device;
	core->device_count++;
	*binding = (struct input_device_binding){
		.core_identity = core->identity,
		.core_generation = core->generation,
		.device_identity = config->identity,
		.device_generation = generation,
		.slot = slot,
		.reserved = {0u},
	};
	return INPUT_OK;
}

enum input_status input_internal_device_binding_status(
	struct input_core *core, const struct input_device_binding *binding,
	struct input_device **device)
{
	struct input_device *candidate;

	if (core == NULL || binding == NULL || device == NULL ||
	    !input_internal_identity_valid(binding->core_identity) ||
	    !input_internal_identity_valid(binding->device_identity) ||
	    !input_internal_bytes_zero(binding->reserved,
				       ARRAY_SIZE(binding->reserved)) ||
	    binding->slot >= core->device_capacity)
		return INPUT_INVALID_ARGUMENT;
	if (!input_internal_core_is_usable(core))
		return core->phase == INPUT_CORE_POISONED_PHASE ? INPUT_POISONED
							 : INPUT_INVALID_STATE;
	if (binding->core_identity != core->identity)
		return INPUT_IDENTITY_MISMATCH;
	if (binding->core_generation != core->generation)
		return INPUT_STALE_BINDING;
	candidate = core->devices[binding->slot];
	if (candidate == NULL ||
	    candidate->generation != binding->device_generation)
		return INPUT_STALE_BINDING;
	if (candidate->config.identity != binding->device_identity)
		return INPUT_IDENTITY_MISMATCH;
	*device = candidate;
	return INPUT_OK;
}

enum input_status input_device_quiesce(
	struct input_core *core, const struct input_device_binding *binding)
{
	struct input_device *device;
	enum input_status status =
		input_internal_device_binding_status(core, binding, &device);

	if (status != INPUT_OK)
		return status;
	if (core->phase != INPUT_CORE_PREPARED &&
	    core->phase != INPUT_CORE_QUIESCED)
		return INPUT_INVALID_STATE;
	if (device->phase != INPUT_DEVICE_ACTIVE || device->in_flight != 0u)
		return INPUT_BUSY;
	device->phase = INPUT_DEVICE_QUIESCED;
	return INPUT_OK;
}

enum input_status input_device_unregister(
	struct input_core *core, const struct input_device_binding *binding)
{
	struct input_device *device;
	enum input_status status =
		input_internal_device_binding_status(core, binding, &device);

	if (status != INPUT_OK)
		return status;
	if (core->phase != INPUT_CORE_PREPARED &&
	    core->phase != INPUT_CORE_QUIESCED)
		return INPUT_INVALID_STATE;
	if (device->phase != INPUT_DEVICE_QUIESCED ||
	    device->in_flight != 0u || device->queue_count != 0u)
		return INPUT_BUSY;
	core->devices[binding->slot] = NULL;
	core->device_count--;
	clear_device_preserving_generation(device);
	return INPUT_OK;
}

enum input_status input_handler_register(
	struct input_core *core, struct input_handler *handler,
	const struct input_handler_config *config,
	struct input_handler_binding *binding)
{
	uint16_t slot;
	uint64_t generation;

	if (!input_internal_core_is_usable(core) || handler == NULL ||
	    binding == NULL || !handler_config_is_valid(config))
		return INPUT_INVALID_ARGUMENT;
	if (core->phase != INPUT_CORE_PREPARED &&
	    core->phase != INPUT_CORE_QUIESCED)
		return INPUT_INVALID_STATE;
	if (handler->lifecycle_cookie != INPUT_HANDLER_COOKIE)
		return INPUT_INVALID_ARGUMENT;
	if (handler->phase == INPUT_HANDLER_POISONED_PHASE)
		return INPUT_POISONED;
	if (handler->phase != INPUT_HANDLER_EMPTY)
		return INPUT_ALREADY_REGISTERED;
	if (identity_is_registered(core, config->identity))
		return INPUT_ALREADY_REGISTERED;
	for (slot = 0u; slot < core->handler_capacity; ++slot) {
		if (core->handlers[slot] == NULL)
			break;
	}
	if (slot == core->handler_capacity ||
	    handler->generation >= INPUT_GENERATION_MAX)
		return INPUT_CAPACITY_EXHAUSTED;
	generation = handler->generation + 1u;
	clear_handler_preserving_generation(handler);
	handler->generation = generation;
	handler->config = *config;
	handler->core = core;
	handler->registry_slot = slot;
	handler->phase = INPUT_HANDLER_ACTIVE;
	core->handlers[slot] = handler;
	core->handler_count++;
	*binding = (struct input_handler_binding){
		.core_identity = core->identity,
		.core_generation = core->generation,
		.handler_identity = config->identity,
		.handler_generation = generation,
		.slot = slot,
		.reserved = {0u},
	};
	return INPUT_OK;
}

enum input_status input_internal_handler_binding_status(
	struct input_core *core, const struct input_handler_binding *binding,
	struct input_handler **handler)
{
	struct input_handler *candidate;

	if (core == NULL || binding == NULL || handler == NULL ||
	    !input_internal_identity_valid(binding->core_identity) ||
	    !input_internal_identity_valid(binding->handler_identity) ||
	    !input_internal_bytes_zero(binding->reserved,
				       ARRAY_SIZE(binding->reserved)) ||
	    binding->slot >= core->handler_capacity)
		return INPUT_INVALID_ARGUMENT;
	if (!input_internal_core_is_usable(core))
		return core->phase == INPUT_CORE_POISONED_PHASE ? INPUT_POISONED
							 : INPUT_INVALID_STATE;
	if (binding->core_identity != core->identity)
		return INPUT_IDENTITY_MISMATCH;
	if (binding->core_generation != core->generation)
		return INPUT_STALE_BINDING;
	candidate = core->handlers[binding->slot];
	if (candidate == NULL ||
	    candidate->generation != binding->handler_generation)
		return INPUT_STALE_BINDING;
	if (candidate->config.identity != binding->handler_identity)
		return INPUT_IDENTITY_MISMATCH;
	*handler = candidate;
	return INPUT_OK;
}

enum input_status input_handler_quiesce(
	struct input_core *core, const struct input_handler_binding *binding)
{
	struct input_handler *handler;
	enum input_status status =
		input_internal_handler_binding_status(core, binding, &handler);

	if (status != INPUT_OK)
		return status;
	if (core->phase != INPUT_CORE_PREPARED &&
	    core->phase != INPUT_CORE_QUIESCED)
		return INPUT_INVALID_STATE;
	if ((handler->phase != INPUT_HANDLER_ACTIVE &&
	     handler->phase != INPUT_HANDLER_POISONED_PHASE) ||
	    handler->in_flight != 0u || core->focus == handler)
		return INPUT_BUSY;
	handler->phase = INPUT_HANDLER_QUIESCED;
	return INPUT_OK;
}

enum input_status input_handler_unregister(
	struct input_core *core, const struct input_handler_binding *binding)
{
	struct input_handler *handler;
	enum input_status status =
		input_internal_handler_binding_status(core, binding, &handler);

	if (status != INPUT_OK)
		return status;
	if (core->phase != INPUT_CORE_PREPARED &&
	    core->phase != INPUT_CORE_QUIESCED)
		return INPUT_INVALID_STATE;
	if (handler->phase != INPUT_HANDLER_QUIESCED ||
	    handler->in_flight != 0u || core->focus == handler)
		return INPUT_BUSY;
	core->handlers[binding->slot] = NULL;
	core->handler_count--;
	clear_handler_preserving_generation(handler);
	return INPUT_OK;
}

// SPDX-License-Identifier: GPL-2.0-only
/* Generation-bound focus publication and decoded-event dispatch. */
#include "private.h"

static bool event_fields_are_valid(uint8_t type, input_key_code_t code,
				   uint8_t value, uint8_t flags)
{
	if ((flags & (uint8_t)~INPUT_EVENT_FLAG_MASK) != 0u)
		return false;
	if (type != (uint8_t)INPUT_EVENT_KEY || code == 0u)
		return false;
	return value == (uint8_t)INPUT_KEY_RELEASED ||
	       value == (uint8_t)INPUT_KEY_PRESSED ||
	       value == (uint8_t)INPUT_KEY_REPEATED;
}

static uint32_t event_capability(uint8_t type)
{
	return type == (uint8_t)INPUT_EVENT_KEY ? INPUT_CAPABILITY_KEY : 0u;
}

static enum input_status enqueue_event(struct input_device *device,
				       const struct input_event *event)
{
	uint16_t index;

	if (device->queue_count >= device->config.queue_capacity) {
		device->overflow_count = input_internal_saturating_increment(
			device->overflow_count);
		return INPUT_CAPACITY_EXHAUSTED;
	}
	index = (uint16_t)((device->queue_head + device->queue_count) %
			   device->config.queue_capacity);
	device->config.queue[index] = *event;
	device->queue_count++;
	return INPUT_OK;
}

static enum input_status publish_focus(struct input_core *core,
				       struct input_handler *new_focus)
{
	struct input_handler *old_focus;
	uint64_t old_generation;
	uint64_t new_generation;
	enum input_focus_result enter_result;

	if (core->focus == new_focus)
		return INPUT_OK;
	if (core->focus_generation >= INPUT_GENERATION_MAX)
		return INPUT_CAPACITY_EXHAUSTED;
	new_generation = core->focus_generation + 1u;
	enter_result = new_focus->config.focus_enter(
		new_focus, new_focus->config.context, core->identity,
		new_generation);
	if (enter_result == INPUT_FOCUS_REJECTED)
		return INPUT_ACCESS_DENIED;
	if (enter_result != INPUT_FOCUS_OK) {
		new_focus->phase = INPUT_HANDLER_POISONED_PHASE;
		new_focus->fault_count = input_internal_saturating_increment(
			new_focus->fault_count);
		return INPUT_HANDLER_FAULT;
	}

	input_internal_guard_enter(core);
	if ((core->phase != INPUT_CORE_PREPARED &&
	     core->phase != INPUT_CORE_ACTIVE &&
	     core->phase != INPUT_CORE_QUIESCED) ||
	    core->dispatch_active != 0u ||
	    new_focus->phase != INPUT_HANDLER_ACTIVE) {
		input_internal_guard_exit(core);
		new_focus->config.focus_leave(
			new_focus, new_focus->config.context, core->identity,
			new_generation);
		return INPUT_BUSY;
	}
	old_focus = core->focus;
	old_generation = core->focus_generation;
	core->focus = new_focus;
	core->focus_generation = new_generation;
	input_internal_guard_exit(core);
	if (old_focus != NULL)
		old_focus->config.focus_leave(
			old_focus, old_focus->config.context, core->identity,
			old_generation);
	return INPUT_OK;
}

enum input_status input_focus_set(
	struct input_core *core, const struct input_handler_binding *binding)
{
	struct input_handler *handler;
	enum input_status status =
		input_internal_handler_binding_status(core, binding, &handler);

	if (status != INPUT_OK)
		return status;
	if (core->phase != INPUT_CORE_PREPARED &&
	    core->phase != INPUT_CORE_ACTIVE &&
	    core->phase != INPUT_CORE_QUIESCED)
		return INPUT_INVALID_STATE;
	if (handler->phase != INPUT_HANDLER_ACTIVE)
		return handler->phase == INPUT_HANDLER_POISONED_PHASE
			       ? INPUT_POISONED
			       : INPUT_INVALID_STATE;
	return publish_focus(core, handler);
}

enum input_status input_focus_clear(
	struct input_core *core, const struct input_handler_binding *binding)
{
	struct input_handler *old_focus;
	struct input_handler *expected;
	uint64_t old_generation;
	enum input_status status =
		input_internal_handler_binding_status(core, binding, &expected);

	if (status != INPUT_OK)
		return status;
	if (core->phase != INPUT_CORE_PREPARED &&
	    core->phase != INPUT_CORE_ACTIVE &&
	    core->phase != INPUT_CORE_QUIESCED)
		return INPUT_INVALID_STATE;
	input_internal_guard_enter(core);
	old_focus = core->focus;
	if (old_focus == NULL) {
		input_internal_guard_exit(core);
		return INPUT_NOT_FOUND;
	}
	if (old_focus != expected) {
		input_internal_guard_exit(core);
		return INPUT_IDENTITY_MISMATCH;
	}
	if (core->dispatch_active != 0u || old_focus->in_flight != 0u) {
		input_internal_guard_exit(core);
		return INPUT_BUSY;
	}
	if (core->focus_generation >= INPUT_GENERATION_MAX) {
		input_internal_guard_exit(core);
		return INPUT_CAPACITY_EXHAUSTED;
	}
	old_generation = core->focus_generation;
	core->focus = NULL;
	core->focus_generation++;
	input_internal_guard_exit(core);
	old_focus->config.focus_leave(old_focus, old_focus->config.context,
				      core->identity, old_generation);
	return INPUT_OK;
}

static void finish_dispatch(struct input_core *core,
			    struct input_device *device,
			    struct input_handler *handler)
{
	handler->in_flight = 0u;
	device->in_flight = 0u;
	core->dispatch_active = 0u;
}

static enum input_status account_handler_result(
	struct input_core *core, struct input_device *device,
	struct input_handler *handler, const struct input_event *event,
	enum input_handler_result result, bool from_queue)
{
	enum input_status status;

	if (result == INPUT_HANDLER_HANDLED) {
		handler->handled_count = input_internal_saturating_increment(
			handler->handled_count);
		device->submitted_count = input_internal_saturating_increment(
			device->submitted_count);
		return INPUT_OK;
	}
	if (result == INPUT_HANDLER_REJECTED) {
		handler->rejected_count = input_internal_saturating_increment(
			handler->rejected_count);
		device->rejected_count = input_internal_saturating_increment(
			device->rejected_count);
		return INPUT_ACCESS_DENIED;
	}
	if (result == INPUT_HANDLER_DEFER) {
		handler->deferred_count = input_internal_saturating_increment(
			handler->deferred_count);
		if (from_queue)
			return INPUT_RETRY;
		device->deferred_count = input_internal_saturating_increment(
			device->deferred_count);
		status = enqueue_event(device, event);
		return status == INPUT_OK ? INPUT_DEFERRED : status;
	}
	handler->fault_count = input_internal_saturating_increment(
		handler->fault_count);
	handler->phase = INPUT_HANDLER_POISONED_PHASE;
	/* Keep the poisoned focus published until process-context recovery calls
	 * input_focus_clear(); that callback owns external resource teardown. */
	(void)core;
	return INPUT_HANDLER_FAULT;
}

enum input_status input_submit(
	struct input_core *core, const struct input_device_binding *binding,
	uint8_t type, input_key_code_t code, uint8_t value,
	uint32_t hardware_code, uint8_t flags)
{
	struct input_device *device;
	struct input_handler *handler;
	struct input_event event;
	enum input_handler_result result;
	enum input_status status;
	uint32_t capability;

	if (!event_fields_are_valid(type, code, value, flags))
		return INPUT_INVALID_ARGUMENT;
	status = input_internal_device_binding_status(core, binding, &device);
	if (status != INPUT_OK)
		return status;
	capability = event_capability(type);
	if ((device->config.capabilities & capability) == 0u)
		return INPUT_ACCESS_DENIED;
	input_internal_guard_enter(core);
	status = input_internal_device_binding_status(core, binding, &device);
	if (status != INPUT_OK || core->phase != INPUT_CORE_ACTIVE ||
	    device->phase != INPUT_DEVICE_ACTIVE) {
		input_internal_guard_exit(core);
		return status != INPUT_OK ? status : INPUT_INVALID_STATE;
	}
	handler = core->focus;
	if (handler == NULL) {
		core->unfocused_drop_count = input_internal_saturating_increment(
			core->unfocused_drop_count);
		input_internal_guard_exit(core);
		return INPUT_UNAVAILABLE;
	}
	if (handler->phase != INPUT_HANDLER_ACTIVE ||
	    (handler->config.capabilities & capability) == 0u) {
		input_internal_guard_exit(core);
		return INPUT_ACCESS_DENIED;
	}
	if (device->next_sequence >= INPUT_GENERATION_MAX) {
		device->overflow_count = input_internal_saturating_increment(
			device->overflow_count);
		input_internal_guard_exit(core);
		return INPUT_CAPACITY_EXHAUSTED;
	}
	event = (struct input_event){
		.device_identity = device->config.identity,
		.handler_identity = handler->config.identity,
		.device_generation = device->generation,
		.focus_generation = core->focus_generation,
		.sequence = device->next_sequence + 1u,
		.hardware_code = hardware_code,
		.code = code,
		.type = type,
		.value = value,
		.flags = flags,
		.reserved = {0u},
	};
	device->next_sequence = event.sequence;
	/* A non-empty queue owns ordering until its head is consumed or proven
	 * stale.  Never bypass it merely because the current handler can run. */
	if (device->queue_count != 0u) {
		device->deferred_count = input_internal_saturating_increment(
			device->deferred_count);
		status = enqueue_event(device, &event);
		input_internal_guard_exit(core);
		return status == INPUT_OK ? INPUT_DEFERRED : status;
	}
	if (core->dispatch_active != 0u || device->in_flight != 0u) {
		input_internal_guard_exit(core);
		return INPUT_BUSY;
	}
	core->dispatch_active = 1u;
	device->in_flight = 1u;
	handler->in_flight = 1u;
	input_internal_guard_exit(core);
	result = handler->config.receive(handler, handler->config.context, &event);
	input_internal_guard_enter(core);
	finish_dispatch(core, device, handler);
	status = account_handler_result(core, device, handler, &event, result,
					false);
	input_internal_guard_exit(core);
	return status;
}

static bool queued_event_is_current(const struct input_core *core,
				    const struct input_device *device,
				    const struct input_handler *handler,
				    const struct input_event *event)
{
	return event->device_identity == device->config.identity &&
	       event->device_generation == device->generation &&
	       handler != NULL && handler->phase == INPUT_HANDLER_ACTIVE &&
	       event->handler_identity == handler->config.identity &&
	       event->focus_generation == core->focus_generation;
}

enum input_status input_device_pump(
	struct input_core *core, const struct input_device_binding *binding,
	uint16_t budget, uint16_t *delivered)
{
	struct input_device *device;
	enum input_status status;
	uint16_t completed = 0u;

	if (delivered == NULL || budget == 0u)
		return INPUT_INVALID_ARGUMENT;
	*delivered = 0u;
	status = input_internal_device_binding_status(core, binding, &device);
	if (status != INPUT_OK)
		return status;
	while (completed < budget) {
		struct input_event event;
		struct input_handler *handler;
		enum input_handler_result result;

		input_internal_guard_enter(core);
		if (core->phase != INPUT_CORE_ACTIVE ||
		    device->phase != INPUT_DEVICE_ACTIVE) {
			input_internal_guard_exit(core);
			return INPUT_INVALID_STATE;
		}
		if (device->queue_count == 0u) {
			input_internal_guard_exit(core);
			*delivered = completed;
			return completed == 0u ? INPUT_EMPTY : INPUT_OK;
		}
		event = device->config.queue[device->queue_head];
		handler = core->focus;
		if (!queued_event_is_current(core, device, handler, &event)) {
			device->queue_head = (uint16_t)((device->queue_head + 1u) %
						device->config.queue_capacity);
			device->queue_count--;
			device->stale_focus_drop_count =
				input_internal_saturating_increment(
					device->stale_focus_drop_count);
			input_internal_guard_exit(core);
			continue;
		}
		if (core->dispatch_active != 0u || device->in_flight != 0u ||
		    handler->in_flight != 0u) {
			input_internal_guard_exit(core);
			*delivered = completed;
			return INPUT_BUSY;
		}
		core->dispatch_active = 1u;
		device->in_flight = 1u;
		handler->in_flight = 1u;
		input_internal_guard_exit(core);
		result = handler->config.receive(handler, handler->config.context,
					 &event);
		input_internal_guard_enter(core);
		finish_dispatch(core, device, handler);
		status = account_handler_result(core, device, handler, &event, result,
						true);
		if (result != INPUT_HANDLER_DEFER) {
			device->queue_head = (uint16_t)((device->queue_head + 1u) %
						device->config.queue_capacity);
			device->queue_count--;
		}
		input_internal_guard_exit(core);
		if (result == INPUT_HANDLER_DEFER) {
			*delivered = completed;
			return status;
		}
		completed++;
		if (status == INPUT_HANDLER_FAULT) {
			*delivered = completed;
			return status;
		}
	}
	*delivered = completed;
	return INPUT_OK;
}

enum input_status input_core_snapshot(const struct input_core *core,
				      struct input_core_snapshot *snapshot)
{
	if (core == NULL || snapshot == NULL)
		return INPUT_INVALID_ARGUMENT;
	if (core->lifecycle_cookie != INPUT_CORE_COOKIE ||
	    core->phase == INPUT_CORE_UNINITIALIZED ||
	    core->phase == INPUT_CORE_RETIRED)
		return INPUT_INVALID_STATE;
	input_internal_guard_enter(core);
	*snapshot = (struct input_core_snapshot){
		.identity = core->identity,
		.focus_identity = core->focus != NULL
				  ? core->focus->config.identity
				  : KERNEL_OBJECT_HANDLE_INVALID,
		.generation = core->generation,
		.focus_generation = core->focus_generation,
		.unfocused_drop_count = core->unfocused_drop_count,
		.device_capacity = core->device_capacity,
		.handler_capacity = core->handler_capacity,
		.device_count = core->device_count,
		.handler_count = core->handler_count,
		.phase = core->phase,
		.dispatch_active = core->dispatch_active,
		.reserved = {0u},
	};
	input_internal_guard_exit(core);
	return snapshot->phase == INPUT_CORE_POISONED_PHASE ? INPUT_POISONED
							    : INPUT_OK;
}

enum input_status input_device_snapshot(
	const struct input_device *device, struct input_device_snapshot *snapshot)
{
	const struct input_core *core;

	if (device == NULL || snapshot == NULL ||
	    device->lifecycle_cookie != INPUT_DEVICE_COOKIE ||
	    device->phase == INPUT_DEVICE_EMPTY || device->core == NULL)
		return INPUT_INVALID_ARGUMENT;
	core = device->core;
	input_internal_guard_enter(core);
	*snapshot = (struct input_device_snapshot){
		.identity = device->config.identity,
		.generation = device->generation,
		.next_sequence = device->next_sequence,
		.submitted_count = device->submitted_count,
		.deferred_count = device->deferred_count,
		.rejected_count = device->rejected_count,
		.overflow_count = device->overflow_count,
		.stale_focus_drop_count = device->stale_focus_drop_count,
		.capabilities = device->config.capabilities,
		.queue_capacity = device->config.queue_capacity,
		.queue_count = device->queue_count,
		.phase = device->phase,
		.in_flight = device->in_flight,
		.reserved = {0u},
	};
	input_internal_guard_exit(core);
	return snapshot->phase == INPUT_DEVICE_POISONED_PHASE ? INPUT_POISONED
							      : INPUT_OK;
}

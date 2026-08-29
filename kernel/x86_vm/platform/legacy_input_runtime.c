// SPDX-License-Identifier: GPL-2.0-only
/*
 * Native legacy input runtime owner.
 *
 * Acquire-before-publish topology, masked-IRQ controller setup, bounded
 * protocol negotiation and reverse unwind protect caller-owned storage,
 * explicit identities/generations, normalized focus, and the hardware commit
 * point.
 */
#include "x86_legacy_input_runtime.h"

#include "x86_i8042.h"

#include "../../../config/atkbd.h"
#include "../../../config/x86-native-i8042.h"
#include "../../../config/x86-native-input.h"

#define LEGACY_INPUT_COOKIE 0x4c495254u
#define LEGACY_INPUT_GENERATION_MAX ((uint64_t)-2)
#define LEGACY_INPUT_NEGOTIATION_STEP_MAX 1000000u

#define LEGACY_INPUT_SERIO_TYPE_8042 1u
#define LEGACY_INPUT_SERIO_PROTOCOL_PS2_KEYBOARD 1u

#define ACQUIRED_REGISTRY (1u << 0)
#define ACQUIRED_INPUT_CORE (1u << 1)
#define ACQUIRED_CONTROLLER_CONTROL (1u << 2)
#define ACQUIRED_ATKBD_DRIVER (1u << 3)
#define ACQUIRED_CONSOLE_HANDLER (1u << 4)
#define ACQUIRED_GUEST_HANDLER (1u << 5)
#define ACQUIRED_NATIVE_INPUT_PREPARED (1u << 6)
#define ACQUIRED_NATIVE_INPUT_PUBLISHED (1u << 7)
#define ACQUIRED_INPUT_CORE_PUBLISHED (1u << 8)
#define ACQUIRED_CONTROLLER_PUBLISHED (1u << 9)
#define ACQUIRED_CONSOLE_FOCUSED (1u << 10)
#define ACQUIRED_DEFAULT_CONSOLE (1u << 11)
#define ACQUIRED_IRQ_ACTION (1u << 12)

/* The selected physical i8042/PIC domain is a singleton today. */
static struct x86_legacy_input_runtime *irq_runtime;

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

static uint64_t saturating_increment(uint64_t value)
{
	return value == (uint64_t)-1 ? value : value + 1u;
}

static bool identities_are_unique(
	const struct x86_legacy_input_runtime_config *config)
{
	const kernel_object_handle_t identities[] = {
		config->identity,
		config->serio_registry_identity,
		config->input_core_identity,
		config->controller_identity,
		config->keyboard_port_identity,
		config->atkbd_driver_identity,
		config->input_device_identity,
		config->console_handler_identity,
		config->guest_handler_identity,
		config->guest_context_identity,
		config->guest_machine_identity,
		config->guest_source_identity,
		config->legacy_irq_source_identity,
		config->irq_action_identity,
	};
	size_t left;

	for (left = 0u; left < ARRAY_SIZE(identities); ++left) {
		size_t right;

		if (!identity_is_valid(identities[left]))
			return false;
		for (right = left + 1u; right < ARRAY_SIZE(identities); ++right) {
			if (identities[left] == identities[right])
				return false;
		}
	}
	return identity_is_valid(config->io_context_identity);
}

static bool storage_is_valid(const struct x86_legacy_input_storage *storage)
{
	return storage != NULL && storage->serio_ports != NULL &&
	       storage->serio_drivers != NULL && storage->input_devices != NULL &&
	       storage->input_handlers != NULL &&
	       storage->keyboard_bytes != NULL &&
	       storage->decoded_events != NULL && storage->console_keys != NULL &&
	       storage->serio_port_capacity >= 1u &&
	       storage->serio_driver_capacity >= 1u &&
	       storage->input_device_capacity >= 1u &&
	       storage->input_handler_capacity >= 2u &&
	       storage->keyboard_byte_capacity != 0u &&
	       storage->decoded_event_capacity != 0u &&
	       storage->console_key_capacity != 0u &&
	       bytes_are_zero(storage->reserved,
			      ARRAY_SIZE(storage->reserved));
}

static bool config_is_valid(
	const struct x86_legacy_input_runtime_config *config)
{
	if (config == NULL || !identities_are_unique(config) ||
	    config->control_read8 == NULL || config->control_write8 == NULL ||
	    config->input_read8 == NULL || config->input_write8 == NULL ||
	    config->console_wait == NULL || !storage_is_valid(&config->storage) ||
	    config->controller_poll_limit == 0u ||
	    config->controller_poll_limit >
		    CONFIG_X86_NATIVE_I8042_POLL_LIMIT_MAX ||
	    config->input_write_poll_limit == 0u ||
	    config->input_write_poll_limit >
		    CONFIG_X86_NATIVE_INPUT_WRITE_POLL_LIMIT_MAX ||
	    config->negotiation_step_limit == 0u ||
	    config->negotiation_step_limit >
		    LEGACY_INPUT_NEGOTIATION_STEP_MAX ||
	    config->controller_drain_limit == 0u ||
	    config->controller_drain_limit >
		    CONFIG_X86_NATIVE_I8042_DRAIN_LIMIT_MAX ||
	    config->controller_stability_attempts < 2u ||
	    config->controller_stability_attempts >
		    CONFIG_X86_NATIVE_I8042_STABILITY_ATTEMPTS_MAX ||
	    config->atkbd_command_write_limit == 0u ||
	    config->atkbd_command_write_limit >
		    CONFIG_ATKBD_COMMAND_WRITE_LIMIT_MAX ||
	    config->atkbd_command_nak_limit == 0u ||
	    config->atkbd_command_nak_limit >
		    CONFIG_ATKBD_COMMAND_NAK_LIMIT_MAX ||
	    config->controller_present > 1u || config->keyboard_present > 1u ||
	    config->presence_evidence >
		    X86_NATIVE_INPUT_EVIDENCE_CONTROLLER_OBSERVED ||
	    !bytes_are_zero(config->reserved, ARRAY_SIZE(config->reserved)))
		return false;
	if (config->controller_present == 0u || config->keyboard_present == 0u)
		return false;
	return config->presence_evidence != X86_NATIVE_INPUT_EVIDENCE_NONE;
}

static enum x86_legacy_input_status runtime_status(
	const struct x86_legacy_input_runtime *runtime,
	kernel_object_handle_t identity)
{
	if (runtime == NULL || runtime->lifecycle_cookie != LEGACY_INPUT_COOKIE ||
	    !identity_is_valid(identity))
		return X86_LEGACY_INPUT_INVALID_ARGUMENT;
	if (runtime->phase == X86_LEGACY_INPUT_UNINITIALIZED ||
	    runtime->phase == X86_LEGACY_INPUT_EMPTY ||
	    runtime->phase == X86_LEGACY_INPUT_RETIRED)
		return X86_LEGACY_INPUT_INVALID_STATE;
	if (runtime->config.identity != identity)
		return X86_LEGACY_INPUT_IDENTITY_MISMATCH;
	return runtime->phase == X86_LEGACY_INPUT_POISONED_PHASE ||
		       runtime->poisoned != 0u
		       ? X86_LEGACY_INPUT_POISONED
		       : X86_LEGACY_INPUT_OK;
}

static enum x86_legacy_input_status map_serio(enum serio_status status)
{
	switch (status) {
	case SERIO_OK:
	case SERIO_EMPTY:
		return X86_LEGACY_INPUT_OK;
	case SERIO_RETRY:
		return X86_LEGACY_INPUT_RETRY;
	case SERIO_STREAM_LOST:
	case SERIO_RECOVERY_PENDING:
		return X86_LEGACY_INPUT_RECOVERY_PENDING;
	case SERIO_STREAM_ISOLATED:
		return X86_LEGACY_INPUT_STREAM_ISOLATED;
	case SERIO_UNAVAILABLE:
	case SERIO_NOT_FOUND:
		return X86_LEGACY_INPUT_UNAVAILABLE;
	case SERIO_CAPACITY_EXHAUSTED:
		return X86_LEGACY_INPUT_CAPACITY_EXHAUSTED;
	case SERIO_IDENTITY_MISMATCH:
		return X86_LEGACY_INPUT_IDENTITY_MISMATCH;
	case SERIO_STALE_EVENT:
		return X86_LEGACY_INPUT_STALE_REFERENCE;
	case SERIO_PARENT_BUSY:
	case SERIO_DRIVER_BUSY:
		return X86_LEGACY_INPUT_BUSY;
	case SERIO_POISONED:
		return X86_LEGACY_INPUT_POISONED;
	case SERIO_INVALID_ARGUMENT:
		return X86_LEGACY_INPUT_INVALID_ARGUMENT;
	default:
		return X86_LEGACY_INPUT_INVALID_STATE;
	}
}

static enum x86_legacy_input_status map_input(enum input_status status)
{
	switch (status) {
	case INPUT_OK:
	case INPUT_EMPTY:
		return X86_LEGACY_INPUT_OK;
	case INPUT_RETRY:
	case INPUT_DEFERRED:
		return X86_LEGACY_INPUT_RETRY;
	case INPUT_UNAVAILABLE:
		return X86_LEGACY_INPUT_UNAVAILABLE;
	case INPUT_CAPACITY_EXHAUSTED:
		return X86_LEGACY_INPUT_CAPACITY_EXHAUSTED;
	case INPUT_IDENTITY_MISMATCH:
		return X86_LEGACY_INPUT_IDENTITY_MISMATCH;
	case INPUT_STALE_BINDING:
		return X86_LEGACY_INPUT_STALE_REFERENCE;
	case INPUT_BUSY:
		return X86_LEGACY_INPUT_BUSY;
	case INPUT_POISONED:
	case INPUT_HANDLER_FAULT:
		return X86_LEGACY_INPUT_POISONED;
	case INPUT_INVALID_ARGUMENT:
		return X86_LEGACY_INPUT_INVALID_ARGUMENT;
	default:
		return X86_LEGACY_INPUT_INVALID_STATE;
	}
}

static enum x86_legacy_input_status map_native_input(
	enum x86_native_input_status status)
{
	switch (status) {
	case X86_NATIVE_INPUT_OK:
	case X86_NATIVE_INPUT_EMPTY:
	case X86_NATIVE_INPUT_DROPPED:
		return X86_LEGACY_INPUT_OK;
	case X86_NATIVE_INPUT_RETRY:
	case X86_NATIVE_INPUT_SOURCE_BACKPRESSURE:
		return X86_LEGACY_INPUT_RETRY;
	case X86_NATIVE_INPUT_STREAM_LOST:
	case X86_NATIVE_INPUT_RECOVERY_PENDING:
		return X86_LEGACY_INPUT_RECOVERY_PENDING;
	case X86_NATIVE_INPUT_STREAM_ISOLATED:
		return X86_LEGACY_INPUT_STREAM_ISOLATED;
	case X86_NATIVE_INPUT_UNAVAILABLE:
		return X86_LEGACY_INPUT_UNAVAILABLE;
	case X86_NATIVE_INPUT_IO_ERROR:
		return X86_LEGACY_INPUT_IO_ERROR;
	case X86_NATIVE_INPUT_CAPACITY_EXHAUSTED:
		return X86_LEGACY_INPUT_CAPACITY_EXHAUSTED;
	case X86_NATIVE_INPUT_IDENTITY_MISMATCH:
		return X86_LEGACY_INPUT_IDENTITY_MISMATCH;
	case X86_NATIVE_INPUT_POISONED:
		return X86_LEGACY_INPUT_POISONED;
	case X86_NATIVE_INPUT_INVALID_ARGUMENT:
		return X86_LEGACY_INPUT_INVALID_ARGUMENT;
	default:
		return X86_LEGACY_INPUT_INVALID_STATE;
	}
}

static enum x86_legacy_input_status map_controller(
	enum x86_native_i8042_status status)
{
	switch (status) {
	case X86_NATIVE_I8042_OK:
		return X86_LEGACY_INPUT_OK;
	case X86_NATIVE_I8042_UNAVAILABLE:
		return X86_LEGACY_INPUT_UNAVAILABLE;
	case X86_NATIVE_I8042_TIMEOUT:
		return X86_LEGACY_INPUT_RETRY;
	case X86_NATIVE_I8042_IO_ERROR:
		return X86_LEGACY_INPUT_IO_ERROR;
	case X86_NATIVE_I8042_CAPACITY_EXHAUSTED:
		return X86_LEGACY_INPUT_CAPACITY_EXHAUSTED;
	case X86_NATIVE_I8042_IDENTITY_MISMATCH:
		return X86_LEGACY_INPUT_IDENTITY_MISMATCH;
	case X86_NATIVE_I8042_POISONED:
		return X86_LEGACY_INPUT_POISONED;
	case X86_NATIVE_I8042_INVALID_ARGUMENT:
		return X86_LEGACY_INPUT_INVALID_ARGUMENT;
	default:
		return X86_LEGACY_INPUT_INVALID_STATE;
	}
}

static enum x86_legacy_input_status map_atkbd(enum atkbd_status status)
{
	switch (status) {
	case ATKBD_OK:
	case ATKBD_EMPTY:
	case ATKBD_WAITING:
		return X86_LEGACY_INPUT_OK;
	case ATKBD_RETRY:
		return X86_LEGACY_INPUT_RETRY;
	case ATKBD_UNAVAILABLE:
		return X86_LEGACY_INPUT_UNAVAILABLE;
	case ATKBD_CAPACITY_EXHAUSTED:
		return X86_LEGACY_INPUT_CAPACITY_EXHAUSTED;
	case ATKBD_IDENTITY_MISMATCH:
		return X86_LEGACY_INPUT_IDENTITY_MISMATCH;
	case ATKBD_STALE_REFERENCE:
		return X86_LEGACY_INPUT_STALE_REFERENCE;
	case ATKBD_BUSY:
		return X86_LEGACY_INPUT_BUSY;
	case ATKBD_PROTOCOL_ERROR:
		return X86_LEGACY_INPUT_NEGOTIATION_FAILED;
	case ATKBD_WRITE_UNCERTAIN:
		return X86_LEGACY_INPUT_PROTOCOL_UNCERTAIN;
	case ATKBD_POISONED:
		return X86_LEGACY_INPUT_POISONED;
	case ATKBD_INVALID_ARGUMENT:
		return X86_LEGACY_INPUT_INVALID_ARGUMENT;
	default:
		return X86_LEGACY_INPUT_INVALID_STATE;
	}
}

static enum x86_legacy_input_status map_guest(
	enum guest_ps2_keyboard_status status)
{
	switch (status) {
	case GUEST_PS2_KEYBOARD_OK:
		return X86_LEGACY_INPUT_OK;
	case GUEST_PS2_KEYBOARD_RETRY:
		return X86_LEGACY_INPUT_RETRY;
	case GUEST_PS2_KEYBOARD_CAPACITY_EXHAUSTED:
		return X86_LEGACY_INPUT_CAPACITY_EXHAUSTED;
	case GUEST_PS2_KEYBOARD_IDENTITY_MISMATCH:
		return X86_LEGACY_INPUT_IDENTITY_MISMATCH;
	case GUEST_PS2_KEYBOARD_STALE_REFERENCE:
		return X86_LEGACY_INPUT_STALE_REFERENCE;
	case GUEST_PS2_KEYBOARD_BUSY:
		return X86_LEGACY_INPUT_BUSY;
	case GUEST_PS2_KEYBOARD_POISONED:
		return X86_LEGACY_INPUT_POISONED;
	case GUEST_PS2_KEYBOARD_INVALID_ARGUMENT:
		return X86_LEGACY_INPUT_INVALID_ARGUMENT;
	default:
		return X86_LEGACY_INPUT_INVALID_STATE;
	}
}

static enum x86_legacy_input_status map_irq(
	enum x86_legacy_irq_status status)
{
	switch (status) {
	case X86_LEGACY_IRQ_OK:
		return X86_LEGACY_INPUT_OK;
	case X86_LEGACY_IRQ_CAPACITY_EXHAUSTED:
		return X86_LEGACY_INPUT_CAPACITY_EXHAUSTED;
	case X86_LEGACY_IRQ_IDENTITY_MISMATCH:
		return X86_LEGACY_INPUT_IDENTITY_MISMATCH;
	case X86_LEGACY_IRQ_STALE_BINDING:
		return X86_LEGACY_INPUT_STALE_REFERENCE;
	case X86_LEGACY_IRQ_BUSY:
		return X86_LEGACY_INPUT_BUSY;
	case X86_LEGACY_IRQ_POISONED:
		return X86_LEGACY_INPUT_POISONED;
	case X86_LEGACY_IRQ_INVALID_ARGUMENT:
		return X86_LEGACY_INPUT_INVALID_ARGUMENT;
	default:
		return X86_LEGACY_INPUT_INVALID_STATE;
	}
}

void x86_legacy_input_runtime_construct(
	struct x86_legacy_input_runtime *runtime)
{
	if (runtime == NULL)
		return;
	*runtime = (struct x86_legacy_input_runtime){
		.lifecycle_cookie = LEGACY_INPUT_COOKIE,
		.phase = X86_LEGACY_INPUT_EMPTY,
	};
	serio_registry_construct(&runtime->serio_registry);
	input_core_construct(&runtime->input_core);
	x86_native_i8042_construct(&runtime->controller_control);
	x86_native_input_construct(&runtime->native_input);
	atkbd_driver_construct(&runtime->atkbd_driver);
	atkbd_endpoint_construct(&runtime->atkbd_endpoint);
	keyboard_console_construct(&runtime->console);
	guest_ps2_keyboard_construct(&runtime->guest_keyboard);
}

static bool retire_prepared_core(struct x86_legacy_input_runtime *runtime)
{
	enum input_status status;

	if ((runtime->acquired & ACQUIRED_INPUT_CORE) == 0u)
		return true;
	status = input_core_publish(&runtime->input_core,
				    runtime->config.input_core_identity);
	if (status != INPUT_OK)
		return false;
	status = input_core_quiesce(&runtime->input_core,
				    runtime->config.input_core_identity);
	if (status != INPUT_OK)
		return false;
	status = input_core_retire(&runtime->input_core,
				   runtime->config.input_core_identity);
	if (status != INPUT_OK)
		return false;
	runtime->acquired &= ~ACQUIRED_INPUT_CORE;
	return true;
}

static bool rollback_precommit(struct x86_legacy_input_runtime *runtime)
{
	bool complete = true;

	if (runtime->protocol_committed != 0u ||
	    runtime->protocol_write_uncertain != 0u)
		return false;
	if ((runtime->acquired & ACQUIRED_DEFAULT_CONSOLE) != 0u) {
		if (keyboard_default_unbind(
			    &runtime->console,
			    runtime->config.console_handler_identity,
			    runtime->console.generation) != INPUT_OK) {
			complete = false;
		} else {
			runtime->acquired &= ~ACQUIRED_DEFAULT_CONSOLE;
		}
	}
	if ((runtime->acquired & ACQUIRED_CONSOLE_FOCUSED) != 0u) {
		if (keyboard_console_unfocus(&runtime->console) != INPUT_OK) {
			complete = false;
		} else {
			runtime->acquired &= ~ACQUIRED_CONSOLE_FOCUSED;
			runtime->focus = X86_LEGACY_INPUT_FOCUS_NONE;
		}
	}
	if ((runtime->acquired & ACQUIRED_IRQ_ACTION) != 0u) {
		if (x86_legacy_irq_action_quiesce(
			    runtime->config.legacy_irq_source_identity,
			    &runtime->irq_binding) != X86_LEGACY_IRQ_OK ||
		    x86_legacy_irq_action_unregister(
			    runtime->config.legacy_irq_source_identity,
			    &runtime->irq_binding) != X86_LEGACY_IRQ_OK) {
			complete = false;
		} else {
			runtime->acquired &= ~ACQUIRED_IRQ_ACTION;
		}
	}
	if (irq_runtime == runtime)
		irq_runtime = NULL;
	if ((runtime->acquired & ACQUIRED_CONTROLLER_PUBLISHED) != 0u) {
		if (x86_native_i8042_quiesce(
			    &runtime->controller_control,
			    runtime->config.controller_identity) !=
		    X86_NATIVE_I8042_OK ||
		    x86_native_i8042_retire(
			    &runtime->controller_control,
			    runtime->config.controller_identity) !=
		    X86_NATIVE_I8042_OK) {
			complete = false;
		} else {
			runtime->acquired &= ~(ACQUIRED_CONTROLLER_PUBLISHED |
					       ACQUIRED_CONTROLLER_CONTROL);
		}
	}
	if ((runtime->acquired & ACQUIRED_INPUT_CORE_PUBLISHED) != 0u) {
		if (input_core_quiesce(
			    &runtime->input_core,
			    runtime->config.input_core_identity) != INPUT_OK) {
			complete = false;
		} else {
			runtime->acquired &= ~ACQUIRED_INPUT_CORE_PUBLISHED;
		}
	}
	if ((runtime->acquired & ACQUIRED_NATIVE_INPUT_PUBLISHED) != 0u) {
		if (x86_native_input_quiesce(
			    &runtime->native_input,
			    runtime->config.controller_identity) !=
		    X86_NATIVE_INPUT_OK) {
			complete = false;
		} else {
			runtime->acquired &= ~ACQUIRED_NATIVE_INPUT_PUBLISHED;
		}
	}
	if (complete &&
	    (runtime->acquired & ACQUIRED_NATIVE_INPUT_PREPARED) != 0u) {
		if (x86_native_input_retire(
			    &runtime->native_input,
			    runtime->config.controller_identity) !=
		    X86_NATIVE_INPUT_OK) {
			complete = false;
		} else {
			runtime->acquired &= ~ACQUIRED_NATIVE_INPUT_PREPARED;
		}
	}
	if ((runtime->acquired & ACQUIRED_GUEST_HANDLER) != 0u) {
		if (guest_ps2_keyboard_quiesce(
			    &runtime->guest_keyboard,
			    &runtime->guest_reference) != GUEST_PS2_KEYBOARD_OK ||
		    guest_ps2_keyboard_unregister(
			    &runtime->guest_keyboard,
			    &runtime->guest_reference) != GUEST_PS2_KEYBOARD_OK) {
			complete = false;
		} else {
			runtime->acquired &= ~ACQUIRED_GUEST_HANDLER;
		}
	}
	if ((runtime->acquired & ACQUIRED_CONSOLE_HANDLER) != 0u) {
		if (keyboard_console_unregister(&runtime->console) != INPUT_OK) {
			complete = false;
		} else {
			runtime->acquired &= ~ACQUIRED_CONSOLE_HANDLER;
		}
	}
	if ((runtime->acquired & ACQUIRED_ATKBD_DRIVER) != 0u) {
		if (atkbd_driver_unregister(
			    &runtime->atkbd_driver,
			    runtime->config.atkbd_driver_identity) != ATKBD_OK) {
			complete = false;
		} else {
			runtime->acquired &= ~ACQUIRED_ATKBD_DRIVER;
		}
	}
	if ((runtime->acquired & ACQUIRED_CONTROLLER_CONTROL) != 0u) {
		if (x86_native_i8042_abort(
			    &runtime->controller_control,
			    runtime->config.controller_identity) !=
		    X86_NATIVE_I8042_OK) {
			complete = false;
		} else {
			runtime->acquired &= ~ACQUIRED_CONTROLLER_CONTROL;
		}
	}
	if ((runtime->acquired & ACQUIRED_INPUT_CORE) != 0u) {
		struct input_core_snapshot snapshot;
		enum input_status status = input_core_snapshot(
			&runtime->input_core, &snapshot);

		if (status != INPUT_OK) {
			complete = false;
		} else if (snapshot.phase == INPUT_CORE_PREPARED) {
			if (!retire_prepared_core(runtime))
				complete = false;
		} else if (snapshot.phase == INPUT_CORE_QUIESCED) {
			if (input_core_retire(
				    &runtime->input_core,
				    runtime->config.input_core_identity) != INPUT_OK)
				complete = false;
			else
				runtime->acquired &= ~ACQUIRED_INPUT_CORE;
		} else {
			complete = false;
		}
	}
	return complete;
}

static enum x86_legacy_input_status prepare_failed(
	struct x86_legacy_input_runtime *runtime,
	enum x86_legacy_input_status failure)
{
	if (!rollback_precommit(runtime)) {
		runtime->poisoned = 1u;
		runtime->controller_quarantined =
			(runtime->acquired & ACQUIRED_CONTROLLER_CONTROL) != 0u;
		runtime->phase = X86_LEGACY_INPUT_POISONED_PHASE;
		return X86_LEGACY_INPUT_POISONED;
	}
	runtime->phase = X86_LEGACY_INPUT_RETIRED;
	return failure;
}

static enum x86_legacy_input_status initialize_registry_and_core(
	struct x86_legacy_input_runtime *runtime)
{
	const struct x86_legacy_input_storage *storage =
		&runtime->config.storage;
	const struct input_core_config core_config = {
		.identity = runtime->config.input_core_identity,
		.guard_context = KERNEL_OBJECT_HANDLE_INVALID,
		.irq_enter = NULL,
		.irq_exit = NULL,
		.caller_serializes_irq = 1u,
		.reserved = {0u},
	};
	enum serio_status serio_status;
	enum input_status input_status;

	serio_status = serio_registry_initialize(
		&runtime->serio_registry,
		runtime->config.serio_registry_identity, storage->serio_ports,
		storage->serio_port_capacity, storage->serio_drivers,
		storage->serio_driver_capacity);
	if (serio_status != SERIO_OK)
		return map_serio(serio_status);
	runtime->acquired |= ACQUIRED_REGISTRY;
	input_status = input_core_initialize(
		&runtime->input_core, &core_config, storage->input_devices,
		storage->input_device_capacity, storage->input_handlers,
		storage->input_handler_capacity);
	if (input_status != INPUT_OK)
		return map_input(input_status);
	runtime->acquired |= ACQUIRED_INPUT_CORE;
	return X86_LEGACY_INPUT_OK;
}

static enum x86_legacy_input_status prepare_controller_control(
	struct x86_legacy_input_runtime *runtime)
{
	const struct x86_native_i8042_config config = {
		.controller_identity = runtime->config.controller_identity,
		.callback_context = runtime->config.io_context_identity,
		.read8 = runtime->config.control_read8,
		.write8 = runtime->config.control_write8,
		.poll_limit = runtime->config.controller_poll_limit,
		.data_port = X86_I8042_DATA_PORT,
		.status_port = X86_I8042_STATUS_PORT,
		.command_port = X86_I8042_COMMAND_PORT,
		.drain_limit = runtime->config.controller_drain_limit,
		.stability_attempts =
			runtime->config.controller_stability_attempts,
		.reserved = {0u},
	};
	struct x86_native_i8042_snapshot snapshot;
	enum x86_native_i8042_status status;

	status = x86_native_i8042_prepare(&runtime->controller_control, &config);
	if (status != X86_NATIVE_I8042_OK) {
		if (status == X86_NATIVE_I8042_POISONED)
			runtime->acquired |= ACQUIRED_CONTROLLER_CONTROL;
		return map_controller(status);
	}
	runtime->acquired |= ACQUIRED_CONTROLLER_CONTROL;
	status = x86_native_i8042_snapshot(
		&runtime->controller_control, runtime->config.controller_identity,
		&snapshot);
	if (status != X86_NATIVE_I8042_OK)
		return map_controller(status);
	runtime->translation_enabled = snapshot.translation_enabled;
	runtime->scan_mode = snapshot.translation_enabled != 0u
				     ? ATKBD_SCAN_TRANSLATED_SET1
				     : ATKBD_SCAN_RAW_SET2;
	return X86_LEGACY_INPUT_OK;
}

static enum x86_legacy_input_status register_consumers_and_driver(
	struct x86_legacy_input_runtime *runtime)
{
	const struct atkbd_driver_config atkbd_config = {
		.identity = runtime->config.atkbd_driver_identity,
		.input_core_identity = runtime->config.input_core_identity,
		.input_core = &runtime->input_core,
		.endpoints = &runtime->atkbd_endpoint,
		.endpoint_configs = &runtime->atkbd_endpoint_config,
		.endpoint_count = 1u,
		.command_write_limit =
			runtime->config.atkbd_command_write_limit,
		.command_nak_limit = runtime->config.atkbd_command_nak_limit,
		.matches = &runtime->keyboard_match,
		.match_count = 1u,
		.reserved = {0u},
	};
	const struct keyboard_console_config console_config = {
		.identity = runtime->config.console_handler_identity,
		.core_identity = runtime->config.input_core_identity,
		.queue = runtime->config.storage.console_keys,
		.wait = runtime->config.console_wait,
		.wait_context = runtime->config.console_wait_context,
		.queue_capacity = runtime->config.storage.console_key_capacity,
		.reserved = {0u},
	};
	const struct guest_ps2_keyboard_config guest_config = {
		.identity = runtime->config.guest_handler_identity,
		.context_identity = runtime->config.guest_context_identity,
		.input_core_identity = runtime->config.input_core_identity,
		.machine_identity = runtime->config.guest_machine_identity,
		.source_identity = runtime->config.guest_source_identity,
		.input_core = &runtime->input_core,
		.reserved = {0u},
	};
	enum atkbd_status atkbd_status;
	enum input_status input_status;
	enum guest_ps2_keyboard_status guest_status;

	runtime->keyboard_match = (struct serio_device_id){
		.type = LEGACY_INPUT_SERIO_TYPE_8042,
		.protocol = LEGACY_INPUT_SERIO_PROTOCOL_PS2_KEYBOARD,
		.id = SERIO_MATCH_ANY,
		.extra = SERIO_MATCH_ANY,
	};
	runtime->atkbd_endpoint_config = (struct atkbd_endpoint_config){
		.port_identity = runtime->config.keyboard_port_identity,
		.input_device_identity = runtime->config.input_device_identity,
		.input_queue = runtime->config.storage.decoded_events,
		.input_queue_capacity =
			runtime->config.storage.decoded_event_capacity,
		.scan_mode = runtime->scan_mode,
		.start_enabled = 0u,
		.reserved = {0u},
	};
	atkbd_status = atkbd_driver_register(
		&runtime->atkbd_driver, &runtime->serio_registry, &atkbd_config);
	if (atkbd_status != ATKBD_OK)
		return map_atkbd(atkbd_status);
	runtime->acquired |= ACQUIRED_ATKBD_DRIVER;
	input_status = keyboard_console_register(
		&runtime->console, &runtime->input_core, &console_config);
	if (input_status != INPUT_OK)
		return map_input(input_status);
	runtime->acquired |= ACQUIRED_CONSOLE_HANDLER;
	guest_status = guest_ps2_keyboard_register(
		&runtime->guest_keyboard, &guest_config,
		&runtime->guest_reference);
	if (guest_status != GUEST_PS2_KEYBOARD_OK)
		return map_guest(guest_status);
	runtime->acquired |= ACQUIRED_GUEST_HANDLER;
	return X86_LEGACY_INPUT_OK;
}

static enum x86_legacy_input_status prepare_native_input(
	struct x86_legacy_input_runtime *runtime)
{
	const struct x86_native_input_config config = {
		.controller_identity = runtime->config.controller_identity,
		.callback_context = runtime->config.io_context_identity,
		.keyboard_port_identity = runtime->config.keyboard_port_identity,
		.auxiliary_port_identity = KERNEL_OBJECT_HANDLE_INVALID,
		.registry = &runtime->serio_registry,
		.keyboard_queue = runtime->config.storage.keyboard_bytes,
		.auxiliary_queue = NULL,
		.read8 = runtime->config.input_read8,
		.write8 = runtime->config.input_write8,
		.irq_enter = NULL,
		.irq_exit = NULL,
		.data_port = X86_I8042_DATA_PORT,
		.status_port = X86_I8042_STATUS_PORT,
		.command_port = X86_I8042_COMMAND_PORT,
		.keyboard_queue_capacity =
			runtime->config.storage.keyboard_byte_capacity,
		.auxiliary_queue_capacity = 0u,
		.write_poll_limit = runtime->config.input_write_poll_limit,
		.keyboard_id = {
			LEGACY_INPUT_SERIO_TYPE_8042,
			LEGACY_INPUT_SERIO_PROTOCOL_PS2_KEYBOARD,
			0u,
			0u,
		},
		.auxiliary_id = {0u},
		.present = runtime->config.controller_present,
		.keyboard_present = runtime->config.keyboard_present,
		.auxiliary_present = 0u,
		.presence_evidence = runtime->config.presence_evidence,
		.caller_serializes_irq = 1u,
		.writes_supported = 1u,
		.status_allowed_mask = 0xffu,
		.status_output_full_mask = X86_I8042_STATUS_OUTPUT_FULL,
		.status_input_full_mask = X86_I8042_STATUS_INPUT_FULL,
		.status_auxiliary_mask = X86_I8042_STATUS_AUXILIARY,
		.status_parity_mask = X86_I8042_STATUS_PARITY,
		.status_timeout_mask = X86_I8042_STATUS_TIMEOUT,
		.status_frame_mask = 0u,
		.reserved = {0u},
	};
	enum x86_native_input_status status;
	enum atkbd_status atkbd_status;

	status = x86_native_input_prepare(&runtime->native_input, &config);
	if (status != X86_NATIVE_INPUT_OK)
		return map_native_input(status);
	runtime->acquired |= ACQUIRED_NATIVE_INPUT_PREPARED;
	status = x86_native_input_publish(
		&runtime->native_input, runtime->config.controller_identity);
	if (status != X86_NATIVE_INPUT_OK)
		return map_native_input(status);
	runtime->acquired |= ACQUIRED_NATIVE_INPUT_PUBLISHED;
	atkbd_status = atkbd_endpoint_reference(
		&runtime->atkbd_driver, runtime->config.atkbd_driver_identity, 0u,
		&runtime->atkbd_reference);
	return map_atkbd(atkbd_status);
}

enum x86_legacy_input_status x86_legacy_input_runtime_prepare(
	struct x86_legacy_input_runtime *runtime,
	const struct x86_legacy_input_runtime_config *config)
{
	enum x86_legacy_input_status status;

	if (runtime == NULL || runtime->lifecycle_cookie != LEGACY_INPUT_COOKIE ||
	    !config_is_valid(config))
		return X86_LEGACY_INPUT_INVALID_ARGUMENT;
	if (runtime->phase == X86_LEGACY_INPUT_POISONED_PHASE)
		return X86_LEGACY_INPUT_POISONED;
	if (runtime->phase != X86_LEGACY_INPUT_EMPTY)
		return X86_LEGACY_INPUT_INVALID_STATE;
	if (runtime->generation >= LEGACY_INPUT_GENERATION_MAX)
		return X86_LEGACY_INPUT_CAPACITY_EXHAUSTED;
	runtime->config = *config;
	runtime->generation++;
	runtime->acquired = 0u;
	runtime->focus = X86_LEGACY_INPUT_FOCUS_NONE;
	runtime->protocol_committed = 0u;
	runtime->protocol_write_uncertain = 0u;
	runtime->controller_quarantined = 0u;
	runtime->source_backpressure_pending = 0u;
	runtime->stream_recovery_pending = 0u;
	runtime->stream_isolated = 0u;
	runtime->io_fault_pending = 0u;
	runtime->stream_loss_epoch = 0u;
	runtime->stream_recovery_epoch = 0u;
	runtime->poisoned = 0u;
	status = initialize_registry_and_core(runtime);
	if (status != X86_LEGACY_INPUT_OK)
		return prepare_failed(runtime, status);
	status = prepare_controller_control(runtime);
	if (status != X86_LEGACY_INPUT_OK)
		return prepare_failed(runtime, status);
	status = register_consumers_and_driver(runtime);
	if (status != X86_LEGACY_INPUT_OK)
		return prepare_failed(runtime, status);
	status = prepare_native_input(runtime);
	if (status != X86_LEGACY_INPUT_OK)
		return prepare_failed(runtime, status);
	runtime->phase = X86_LEGACY_INPUT_PREPARED;
	return X86_LEGACY_INPUT_OK;
}

static bool refresh_keyboard_stream_state(
	struct x86_legacy_input_runtime *runtime)
{
	struct x86_native_input_snapshot snapshot;
	enum x86_native_input_status status = x86_native_input_snapshot(
		&runtime->native_input, runtime->config.controller_identity,
		&snapshot);

	if (status != X86_NATIVE_INPUT_OK)
		return false;
	runtime->stream_loss_epoch = snapshot.keyboard_stream_loss_epoch;
	runtime->stream_recovery_epoch =
		snapshot.keyboard_stream_recovery_epoch;
	runtime->stream_recovery_pending =
		snapshot.keyboard_recovery_required != 0u &&
		snapshot.keyboard_recovery_abandoned == 0u;
	runtime->stream_isolated = snapshot.keyboard_recovery_abandoned;
	return true;
}

static enum x86_native_irq_action_result keyboard_irq_action(
	kernel_object_handle_t context, const struct x86_native_irq_event *event)
{
	struct x86_legacy_input_runtime *runtime = irq_runtime;
	enum x86_native_input_status result;

	if (runtime == NULL || event == NULL ||
	    runtime->lifecycle_cookie != LEGACY_INPUT_COOKIE ||
	    runtime->config.identity != context ||
	    runtime->phase != X86_LEGACY_INPUT_ACTIVE ||
	    runtime->poisoned != 0u ||
	    event->hardware_irq != X86_LEGACY_KEYBOARD_IRQ)
		return X86_NATIVE_IRQ_ACTION_FAULT;
	runtime->irq_count = saturating_increment(runtime->irq_count);
	result = x86_native_input_capture(&runtime->native_input,
					  runtime->config.controller_identity);
	if (result == X86_NATIVE_INPUT_OK ||
	    result == X86_NATIVE_INPUT_DROPPED ||
	    result == X86_NATIVE_INPUT_RETRY)
		return X86_NATIVE_IRQ_ACTION_HANDLED;
	if (result == X86_NATIVE_INPUT_SOURCE_BACKPRESSURE) {
		runtime->source_backpressure_pending = 1u;
		runtime->source_backpressure_count = saturating_increment(
			runtime->source_backpressure_count);
		return X86_NATIVE_IRQ_ACTION_HANDLED;
	}
	if (result == X86_NATIVE_INPUT_STREAM_LOST ||
	    result == X86_NATIVE_INPUT_RECOVERY_PENDING ||
	    result == X86_NATIVE_INPUT_STREAM_ISOLATED) {
		runtime->irq_fault_count =
			saturating_increment(runtime->irq_fault_count);
		if (!refresh_keyboard_stream_state(runtime)) {
			runtime->poisoned = 1u;
			return X86_NATIVE_IRQ_ACTION_FAULT;
		}
		/* A later byte may already be resident in 60h. Recovery must actively
		 * sample it because acknowledging this edge cannot guarantee another. */
		runtime->source_backpressure_pending = 1u;
		return X86_NATIVE_IRQ_ACTION_HANDLED;
	}
	if (result == X86_NATIVE_INPUT_EMPTY ||
	    result == X86_NATIVE_INPUT_UNAVAILABLE) {
		runtime->irq_empty_count =
			saturating_increment(runtime->irq_empty_count);
		return X86_NATIVE_IRQ_ACTION_UNHANDLED;
	}
	runtime->irq_fault_count =
		saturating_increment(runtime->irq_fault_count);
	if (result == X86_NATIVE_INPUT_IO_ERROR ||
	    result == X86_NATIVE_INPUT_CAPACITY_EXHAUSTED) {
		/* EOI normally; process context performs the bounded quarantine. */
		runtime->io_fault_pending = 1u;
		return X86_NATIVE_IRQ_ACTION_HANDLED;
	}
	/* Identity, lifecycle or poison damage is a dispatcher-level fault. */
	runtime->poisoned = 1u;
	return X86_NATIVE_IRQ_ACTION_FAULT;
}

static void quarantine_runtime(struct x86_legacy_input_runtime *runtime)
{
	bool action_released =
		(runtime->acquired & ACQUIRED_IRQ_ACTION) == 0u;
	enum input_status input_poison_status;
	enum serio_status serio_poison_status;

	if (!action_released &&
	    x86_legacy_irq_action_quiesce(
		    runtime->config.legacy_irq_source_identity,
		    &runtime->irq_binding) == X86_LEGACY_IRQ_OK &&
	    x86_legacy_irq_action_unregister(
		    runtime->config.legacy_irq_source_identity,
		    &runtime->irq_binding) == X86_LEGACY_IRQ_OK) {
		runtime->acquired &= ~ACQUIRED_IRQ_ACTION;
		action_released = true;
	}
	if (action_released && irq_runtime == runtime)
		irq_runtime = NULL;
	if ((runtime->acquired & ACQUIRED_CONTROLLER_PUBLISHED) != 0u &&
	    x86_native_i8042_quiesce(
		    &runtime->controller_control,
		    runtime->config.controller_identity) == X86_NATIVE_I8042_OK) {
		runtime->acquired &= ~ACQUIRED_CONTROLLER_PUBLISHED;
		runtime->controller_quarantined = 1u;
	}
	if ((runtime->acquired & ACQUIRED_CONTROLLER_CONTROL) != 0u &&
	    (runtime->acquired & ACQUIRED_CONTROLLER_PUBLISHED) == 0u &&
	    runtime->controller_control.phase == X86_NATIVE_I8042_PREPARED)
		runtime->controller_quarantined = 1u;
	if ((runtime->acquired & ACQUIRED_DEFAULT_CONSOLE) != 0u &&
	    keyboard_default_unbind(
		    &runtime->console,
		    runtime->config.console_handler_identity,
		    runtime->console.generation) == INPUT_OK)
		runtime->acquired &= ~ACQUIRED_DEFAULT_CONSOLE;
	if (runtime->focus == X86_LEGACY_INPUT_FOCUS_CONSOLE) {
		if (keyboard_console_unfocus(&runtime->console) == INPUT_OK) {
			runtime->acquired &= ~ACQUIRED_CONSOLE_FOCUSED;
			runtime->focus = X86_LEGACY_INPUT_FOCUS_NONE;
		}
	} else if (runtime->focus == X86_LEGACY_INPUT_FOCUS_GUEST) {
		if (guest_ps2_keyboard_unfocus(
			    &runtime->guest_keyboard,
			    &runtime->guest_reference) == GUEST_PS2_KEYBOARD_OK)
			runtime->focus = X86_LEGACY_INPUT_FOCUS_NONE;
	}
	if ((runtime->acquired & ACQUIRED_INPUT_CORE_PUBLISHED) != 0u &&
	    input_core_quiesce(&runtime->input_core,
			       runtime->config.input_core_identity) == INPUT_OK) {
		runtime->acquired &= ~ACQUIRED_INPUT_CORE_PUBLISHED;
		if ((runtime->acquired & ACQUIRED_NATIVE_INPUT_PUBLISHED) != 0u &&
		    x86_native_input_quiesce(
			    &runtime->native_input,
			    runtime->config.controller_identity) ==
			    X86_NATIVE_INPUT_OK)
			runtime->acquired &= ~ACQUIRED_NATIVE_INPUT_PUBLISHED;
	}
	if ((runtime->acquired & ACQUIRED_INPUT_CORE) != 0u) {
		input_poison_status = input_core_poison(
			&runtime->input_core,
			runtime->config.input_core_identity);
		(void)input_poison_status;
	}
	if ((runtime->acquired & ACQUIRED_REGISTRY) != 0u) {
		serio_poison_status = serio_registry_poison(
			&runtime->serio_registry,
			runtime->config.serio_registry_identity);
		(void)serio_poison_status;
	}
	runtime->source_backpressure_pending = 0u;
	runtime->stream_recovery_pending = 0u;
	runtime->io_fault_pending = 0u;
	runtime->poisoned = 1u;
	runtime->phase = X86_LEGACY_INPUT_POISONED_PHASE;
}

/* Process context, IF=0. Close only the exact discontinuous keyboard stream,
 * then remove its interrupt source so a byte left in 60h cannot storm IRQ1.
 * The input/serio ownership tree remains inspectable and is not mislabeled as
 * globally corrupt. */
static enum x86_legacy_input_status isolate_keyboard_stream(
	struct x86_legacy_input_runtime *runtime)
{
	enum x86_native_input_status input_status;
	enum x86_native_i8042_status control_status;
	enum x86_legacy_irq_status irq_status;

	if (runtime->stream_loss_epoch == 0u) {
		quarantine_runtime(runtime);
		return X86_LEGACY_INPUT_POISONED;
	}
	input_status = x86_native_input_isolate_stream(
		&runtime->native_input, runtime->config.controller_identity,
		X86_NATIVE_INPUT_KEYBOARD, runtime->stream_loss_epoch);
	if (input_status != X86_NATIVE_INPUT_OK &&
	    input_status != X86_NATIVE_INPUT_STREAM_ISOLATED) {
		quarantine_runtime(runtime);
		return X86_LEGACY_INPUT_POISONED;
	}
	if ((runtime->acquired & ACQUIRED_IRQ_ACTION) != 0u) {
		irq_status = x86_legacy_irq_action_quiesce(
			runtime->config.legacy_irq_source_identity,
			&runtime->irq_binding);
		if (irq_status != X86_LEGACY_IRQ_OK ||
		    x86_legacy_irq_action_unregister(
			    runtime->config.legacy_irq_source_identity,
			    &runtime->irq_binding) != X86_LEGACY_IRQ_OK) {
			quarantine_runtime(runtime);
			return X86_LEGACY_INPUT_POISONED;
		}
		runtime->acquired &= ~ACQUIRED_IRQ_ACTION;
	}
	if (irq_runtime == runtime)
		irq_runtime = NULL;
	if ((runtime->acquired & ACQUIRED_CONTROLLER_PUBLISHED) != 0u) {
		control_status = x86_native_i8042_quiesce(
			&runtime->controller_control,
			runtime->config.controller_identity);
		if (control_status != X86_NATIVE_I8042_OK) {
			quarantine_runtime(runtime);
			return X86_LEGACY_INPUT_POISONED;
		}
		runtime->acquired &= ~ACQUIRED_CONTROLLER_PUBLISHED;
	}
	if (runtime->controller_quarantined == 0u)
		runtime->stream_isolation_count = saturating_increment(
			runtime->stream_isolation_count);
	runtime->controller_quarantined = 1u;
	runtime->source_backpressure_pending = 0u;
	runtime->stream_recovery_pending = 0u;
	runtime->stream_isolated = 1u;
	return X86_LEGACY_INPUT_STREAM_ISOLATED;
}

static enum x86_legacy_input_status publish_failed(
	struct x86_legacy_input_runtime *runtime,
	enum x86_legacy_input_status failure)
{
	if (runtime->protocol_committed != 0u ||
	    runtime->protocol_write_uncertain != 0u) {
		quarantine_runtime(runtime);
		return X86_LEGACY_INPUT_POISONED;
	}
	if (!rollback_precommit(runtime)) {
		quarantine_runtime(runtime);
		return X86_LEGACY_INPUT_POISONED;
	}
	runtime->phase = X86_LEGACY_INPUT_RETIRED;
	return failure;
}

static bool legacy_irq_is_masked_and_ready(
	const struct x86_legacy_input_runtime *runtime)
{
	struct x86_legacy_irq_snapshot snapshot;
	enum x86_legacy_irq_status status = x86_legacy_irq_snapshot(
		runtime->config.legacy_irq_source_identity, &snapshot);

	if (status != X86_LEGACY_IRQ_OK)
		return false;
	return (snapshot.phase == X86_LEGACY_IRQ_PREPARED ||
		snapshot.phase == X86_LEGACY_IRQ_QUIESCED) &&
	       (snapshot.present_irq_mask &
		(uint16_t)(1u << X86_LEGACY_KEYBOARD_IRQ)) != 0u &&
	       (snapshot.enabled_irq_mask &
		(uint16_t)(1u << X86_LEGACY_KEYBOARD_IRQ)) != 0u;
}

static enum x86_legacy_input_status observe_protocol_write(
	struct x86_legacy_input_runtime *runtime,
	enum atkbd_status process_status)
{
	struct atkbd_endpoint_snapshot snapshot;
	enum atkbd_status snapshot_status = atkbd_endpoint_snapshot(
		&runtime->atkbd_driver, &runtime->atkbd_reference, &snapshot);

	if (snapshot_status != ATKBD_OK) {
		/* The write call ran, but its owner state cannot prove the commit
		 * boundary. Never turn that observation failure into a replay. */
		runtime->protocol_write_uncertain = 1u;
		return X86_LEGACY_INPUT_PROTOCOL_UNCERTAIN;
	}
	if (snapshot.protocol_committed != 0u)
		runtime->protocol_committed = 1u;
	if (snapshot.write_uncertain != 0u) {
		runtime->protocol_write_uncertain = 1u;
		return X86_LEGACY_INPUT_PROTOCOL_UNCERTAIN;
	}
	return map_atkbd(process_status);
}

static enum x86_legacy_input_status negotiate_keyboard(
	struct x86_legacy_input_runtime *runtime)
{
	uint32_t step;
	enum atkbd_status status = atkbd_command_begin(
		&runtime->atkbd_driver, &runtime->atkbd_reference,
		ATKBD_COMMAND_NEGOTIATE, 0u);

	if (status != ATKBD_OK)
		return map_atkbd(status);
	for (step = 0u; step < runtime->config.negotiation_step_limit; ++step) {
		struct atkbd_endpoint_snapshot snapshot;

		runtime->negotiation_step_count = saturating_increment(
			runtime->negotiation_step_count);
		status = atkbd_endpoint_snapshot(
			&runtime->atkbd_driver, &runtime->atkbd_reference,
			&snapshot);
		if (status != ATKBD_OK)
			return map_atkbd(status);
		if (snapshot.command_phase == ATKBD_COMMAND_COMPLETE)
			return snapshot.negotiated != 0u && snapshot.enabled != 0u
				       ? X86_LEGACY_INPUT_OK
				       : X86_LEGACY_INPUT_NEGOTIATION_FAILED;
		if (snapshot.command_phase == ATKBD_COMMAND_FAILED)
			return X86_LEGACY_INPUT_NEGOTIATION_FAILED;
		if (snapshot.command_phase == ATKBD_COMMAND_READY) {
			runtime->protocol_write_attempt_count =
				saturating_increment(
					runtime->protocol_write_attempt_count);
			status = atkbd_process(&runtime->atkbd_driver,
						&runtime->atkbd_reference);
			{
				enum x86_legacy_input_status write_status =
					observe_protocol_write(runtime, status);

				if (write_status != X86_LEGACY_INPUT_OK &&
				    write_status != X86_LEGACY_INPUT_RETRY)
					return write_status;
			}
			continue;
		}
		if (snapshot.command_phase == ATKBD_COMMAND_WAIT_ACK) {
			enum x86_native_input_status capture =
				x86_native_input_capture(
					&runtime->native_input,
					runtime->config.controller_identity);

			if (capture == X86_NATIVE_INPUT_OK ||
			    capture == X86_NATIVE_INPUT_EMPTY)
				continue;
			if (capture == X86_NATIVE_INPUT_RETRY) {
				uint16_t delivered;
				enum serio_status pump_status = serio_port_pump(
					&runtime->native_input.keyboard_port, 1u,
					&delivered);

				(void)delivered;
				if (pump_status == SERIO_OK ||
				    pump_status == SERIO_EMPTY ||
				    pump_status == SERIO_RETRY)
					continue;
				return map_serio(pump_status);
			}
			return map_native_input(capture);
		}
		return X86_LEGACY_INPUT_NEGOTIATION_FAILED;
	}
	return X86_LEGACY_INPUT_NEGOTIATION_FAILED;
}

enum x86_legacy_input_status x86_legacy_input_runtime_publish(
	struct x86_legacy_input_runtime *runtime,
	kernel_object_handle_t identity)
{
	const struct x86_native_irq_action_config action_config = {
		.identity = runtime != NULL ? runtime->config.irq_action_identity : 0u,
		.context = runtime != NULL ? runtime->config.identity : 0u,
		.hardware_irq = X86_LEGACY_KEYBOARD_IRQ,
		.shared = 0u,
		.reserved = {0u},
		.handler = keyboard_irq_action,
	};
	enum x86_legacy_input_status status = runtime_status(runtime, identity);
	enum input_status input_status;
	enum x86_native_i8042_status control_status;
	enum x86_legacy_irq_status irq_status;

	if (status != X86_LEGACY_INPUT_OK)
		return status;
	if (runtime->phase != X86_LEGACY_INPUT_PREPARED)
		return X86_LEGACY_INPUT_INVALID_STATE;
	if (irq_runtime != NULL || !legacy_irq_is_masked_and_ready(runtime))
		return X86_LEGACY_INPUT_BUSY;
	runtime->phase = X86_LEGACY_INPUT_PUBLISHING;
	input_status = input_core_publish(
		&runtime->input_core, runtime->config.input_core_identity);
	if (input_status != INPUT_OK)
		return publish_failed(runtime, map_input(input_status));
	runtime->acquired |= ACQUIRED_INPUT_CORE_PUBLISHED;
	control_status = x86_native_i8042_publish(
		&runtime->controller_control, runtime->config.controller_identity);
	if (control_status != X86_NATIVE_I8042_OK)
		return publish_failed(runtime, map_controller(control_status));
	runtime->acquired |= ACQUIRED_CONTROLLER_PUBLISHED;
	status = negotiate_keyboard(runtime);
	if (status != X86_LEGACY_INPUT_OK)
		return publish_failed(runtime, status);
	input_status = keyboard_console_focus(&runtime->console);
	if (input_status != INPUT_OK)
		return publish_failed(runtime, map_input(input_status));
	runtime->acquired |= ACQUIRED_CONSOLE_FOCUSED;
	runtime->focus = X86_LEGACY_INPUT_FOCUS_CONSOLE;
	input_status = keyboard_default_bind(
		&runtime->console, runtime->config.console_handler_identity,
		runtime->console.generation);
	if (input_status != INPUT_OK)
		return publish_failed(runtime, map_input(input_status));
	runtime->acquired |= ACQUIRED_DEFAULT_CONSOLE;
	irq_status = x86_legacy_irq_action_register(
		runtime->config.legacy_irq_source_identity, &action_config,
		&runtime->irq_binding);
	if (irq_status != X86_LEGACY_IRQ_OK)
		return publish_failed(runtime, map_irq(irq_status));
	runtime->acquired |= ACQUIRED_IRQ_ACTION;
	irq_runtime = runtime;
	runtime->phase = X86_LEGACY_INPUT_ACTIVE;
	return X86_LEGACY_INPUT_OK;
}

static enum x86_legacy_input_status restore_console_focus(
	struct x86_legacy_input_runtime *runtime)
{
	enum input_status status = keyboard_console_focus(&runtime->console);

	if (status != INPUT_OK)
		return map_input(status);
	status = keyboard_default_bind(
		&runtime->console, runtime->config.console_handler_identity,
		runtime->console.generation);
	if (status != INPUT_OK) {
		enum input_status unfocus_status =
			keyboard_console_unfocus(&runtime->console);

		if (unfocus_status != INPUT_OK) {
			quarantine_runtime(runtime);
			return X86_LEGACY_INPUT_POISONED;
		}
		return map_input(status);
	}
	runtime->acquired |= ACQUIRED_CONSOLE_FOCUSED |
			     ACQUIRED_DEFAULT_CONSOLE;
	runtime->focus = X86_LEGACY_INPUT_FOCUS_CONSOLE;
	return X86_LEGACY_INPUT_OK;
}

enum x86_legacy_input_status x86_legacy_input_runtime_focus_console(
	struct x86_legacy_input_runtime *runtime,
	kernel_object_handle_t identity)
{
	enum x86_legacy_input_status status = runtime_status(runtime, identity);
	struct guest_ps2_keyboard_snapshot guest_snapshot;
	enum guest_ps2_keyboard_status guest_status;
	enum input_status input_status;

	if (status == X86_LEGACY_INPUT_POISONED && runtime != NULL &&
	    runtime->phase == X86_LEGACY_INPUT_ACTIVE) {
		quarantine_runtime(runtime);
		return X86_LEGACY_INPUT_POISONED;
	}
	if (status != X86_LEGACY_INPUT_OK)
		return status;
	if (runtime->phase != X86_LEGACY_INPUT_ACTIVE)
		return X86_LEGACY_INPUT_INVALID_STATE;
	if (runtime->focus == X86_LEGACY_INPUT_FOCUS_CONSOLE) {
		if (keyboard_default_console() == &runtime->console)
			return X86_LEGACY_INPUT_OK;
		quarantine_runtime(runtime);
		return X86_LEGACY_INPUT_POISONED;
	}
	if (runtime->focus != X86_LEGACY_INPUT_FOCUS_GUEST)
		return X86_LEGACY_INPUT_INVALID_STATE;
	/* input_focus_set enters console, atomically swaps, then leaves guest. */
	input_status = keyboard_console_focus(&runtime->console);
	if (input_status != INPUT_OK)
		return map_input(input_status);
	guest_status = guest_ps2_keyboard_snapshot(
		&runtime->guest_keyboard, &runtime->guest_reference,
		&guest_snapshot);
	if (guest_status != GUEST_PS2_KEYBOARD_OK ||
	    guest_snapshot.phase != GUEST_PS2_KEYBOARD_REGISTERED ||
	    guest_snapshot.downstream_bound != 0u ||
	    guest_snapshot.poisoned != 0u) {
		quarantine_runtime(runtime);
		return X86_LEGACY_INPUT_POISONED;
	}
	runtime->acquired |= ACQUIRED_CONSOLE_FOCUSED;
	runtime->focus = X86_LEGACY_INPUT_FOCUS_CONSOLE;
	input_status = keyboard_default_bind(
		&runtime->console, runtime->config.console_handler_identity,
		runtime->console.generation);
	if (input_status == INPUT_OK) {
		runtime->acquired |= ACQUIRED_DEFAULT_CONSOLE;
		return X86_LEGACY_INPUT_OK;
	}
	guest_status = guest_ps2_keyboard_focus(
		&runtime->guest_keyboard, &runtime->guest_reference);
	if (guest_status == GUEST_PS2_KEYBOARD_OK) {
		runtime->acquired &= ~ACQUIRED_CONSOLE_FOCUSED;
		runtime->focus = X86_LEGACY_INPUT_FOCUS_GUEST;
		return map_input(input_status);
	}
	quarantine_runtime(runtime);
	return X86_LEGACY_INPUT_POISONED;
}

enum x86_legacy_input_status x86_legacy_input_runtime_focus_guest(
	struct x86_legacy_input_runtime *runtime,
	kernel_object_handle_t identity)
{
	enum x86_legacy_input_status status = runtime_status(runtime, identity);
	enum guest_ps2_keyboard_status guest_status;
	enum input_status input_status;

	if (status == X86_LEGACY_INPUT_POISONED && runtime != NULL &&
	    runtime->phase == X86_LEGACY_INPUT_ACTIVE) {
		quarantine_runtime(runtime);
		return X86_LEGACY_INPUT_POISONED;
	}
	if (status != X86_LEGACY_INPUT_OK)
		return status;
	if (runtime->phase != X86_LEGACY_INPUT_ACTIVE)
		return X86_LEGACY_INPUT_INVALID_STATE;
	if (runtime->focus == X86_LEGACY_INPUT_FOCUS_GUEST)
		return X86_LEGACY_INPUT_OK;
	if (runtime->focus != X86_LEGACY_INPUT_FOCUS_CONSOLE ||
	    (runtime->acquired & ACQUIRED_DEFAULT_CONSOLE) == 0u)
		return X86_LEGACY_INPUT_INVALID_STATE;
	/* input_focus_set enters guest, atomically swaps, then leaves console. */
	guest_status = guest_ps2_keyboard_focus(
		&runtime->guest_keyboard, &runtime->guest_reference);
	if (guest_status != GUEST_PS2_KEYBOARD_OK)
		return map_guest(guest_status);
	runtime->acquired &= ~ACQUIRED_CONSOLE_FOCUSED;
	runtime->focus = X86_LEGACY_INPUT_FOCUS_GUEST;
	input_status = keyboard_default_unbind(
		&runtime->console, runtime->config.console_handler_identity,
		runtime->console.generation);
	if (input_status == INPUT_OK) {
		runtime->acquired &= ~ACQUIRED_DEFAULT_CONSOLE;
		return X86_LEGACY_INPUT_OK;
	}
	status = restore_console_focus(runtime);
	if (status == X86_LEGACY_INPUT_OK)
		return map_input(input_status);
	quarantine_runtime(runtime);
	return X86_LEGACY_INPUT_POISONED;
}

static enum x86_legacy_input_status pump_decoded(
	struct x86_legacy_input_runtime *runtime, uint16_t budget,
	uint16_t *completed)
{
	enum input_status status;

	if (runtime->focus == X86_LEGACY_INPUT_FOCUS_CONSOLE)
		status = keyboard_console_pump(
			&runtime->console, &runtime->atkbd_endpoint.input_binding,
			budget, completed);
	else
		status = input_device_pump(
			&runtime->input_core,
			&runtime->atkbd_endpoint.input_binding, budget, completed);
	return map_input(status);
}

static enum x86_legacy_input_status account_pump_status(
	struct x86_legacy_input_runtime *runtime,
	enum x86_legacy_input_status status)
{
	if (status != X86_LEGACY_INPUT_OK &&
	    status != X86_LEGACY_INPUT_RETRY &&
	    status != X86_LEGACY_INPUT_BUSY) {
		quarantine_runtime(runtime);
		return X86_LEGACY_INPUT_POISONED;
	}
	return status;
}

static enum x86_legacy_input_status pump_protocol_maintenance(
	struct x86_legacy_input_runtime *runtime, bool *made_progress)
{
	struct atkbd_endpoint_snapshot snapshot;
	enum atkbd_status status = atkbd_endpoint_snapshot(
		&runtime->atkbd_driver, &runtime->atkbd_reference, &snapshot);

	if (status != ATKBD_OK)
		return map_atkbd(status);
	if (snapshot.command_phase == ATKBD_COMMAND_FAILED &&
	    snapshot.reconnect_required == 0u)
		return X86_LEGACY_INPUT_NEGOTIATION_FAILED;
	if (snapshot.negotiated == 0u && snapshot.reconnect_required == 0u &&
	    snapshot.resend_pending == 0u &&
	    (snapshot.command_phase == ATKBD_COMMAND_IDLE ||
	     snapshot.command_phase == ATKBD_COMMAND_COMPLETE)) {
		status = atkbd_command_begin(
			&runtime->atkbd_driver, &runtime->atkbd_reference,
			ATKBD_COMMAND_NEGOTIATE, 0u);
		if (status != ATKBD_OK)
			return map_atkbd(status);
		snapshot.command_phase = ATKBD_COMMAND_READY;
	}
	if (snapshot.reconnect_required == 0u &&
	    snapshot.resend_pending == 0u &&
	    snapshot.command_phase != ATKBD_COMMAND_READY)
		return X86_LEGACY_INPUT_OK;
	status = atkbd_process(&runtime->atkbd_driver,
			       &runtime->atkbd_reference);
	{
		enum x86_legacy_input_status write_status =
			observe_protocol_write(runtime, status);

		if (write_status != X86_LEGACY_INPUT_OK)
			return write_status;
	}
	if (status == ATKBD_OK || status == ATKBD_WAITING) {
		*made_progress = true;
		return X86_LEGACY_INPUT_OK;
	}
	return map_atkbd(status);
}

static enum x86_legacy_input_status recover_keyboard_stream(
	struct x86_legacy_input_runtime *runtime,
	struct x86_legacy_input_pump_result *result, bool *made_progress)
{
	enum x86_native_input_status status;

	if (runtime->stream_loss_epoch == 0u) {
		quarantine_runtime(runtime);
		return X86_LEGACY_INPUT_POISONED;
	}
	runtime->stream_recovery_attempt_count = saturating_increment(
		runtime->stream_recovery_attempt_count);
	status = x86_native_input_recover_stream(
		&runtime->native_input, runtime->config.controller_identity,
		X86_NATIVE_INPUT_KEYBOARD, runtime->stream_loss_epoch);
	if (status == X86_NATIVE_INPUT_OK) {
		if (!refresh_keyboard_stream_state(runtime) ||
		    runtime->stream_recovery_pending != 0u ||
		    runtime->stream_isolated != 0u ||
		    runtime->stream_recovery_epoch != runtime->stream_loss_epoch) {
			quarantine_runtime(runtime);
			return X86_LEGACY_INPUT_POISONED;
		}
		result->stream_recoveries = saturating_increment(
			result->stream_recoveries);
		runtime->source_backpressure_pending = 1u;
		*made_progress = true;
		return X86_LEGACY_INPUT_OK;
	}
	if (status == X86_NATIVE_INPUT_STREAM_LOST ||
	    status == X86_NATIVE_INPUT_RECOVERY_PENDING) {
		if (!refresh_keyboard_stream_state(runtime)) {
			quarantine_runtime(runtime);
			return X86_LEGACY_INPUT_POISONED;
		}
		return X86_LEGACY_INPUT_RECOVERY_PENDING;
	}
	if (status == X86_NATIVE_INPUT_STREAM_ISOLATED) {
		if (!refresh_keyboard_stream_state(runtime)) {
			quarantine_runtime(runtime);
			return X86_LEGACY_INPUT_POISONED;
		}
		return isolate_keyboard_stream(runtime);
	}
	quarantine_runtime(runtime);
	return X86_LEGACY_INPUT_POISONED;
}

static enum x86_legacy_input_status capture_backpressured_source(
	struct x86_legacy_input_runtime *runtime,
	struct x86_legacy_input_pump_result *result, bool *made_progress)
{
	enum x86_native_input_status status;

	runtime->active_capture_count = saturating_increment(
		runtime->active_capture_count);
	result->active_captures = saturating_increment(result->active_captures);
	status = x86_native_input_capture(&runtime->native_input,
					  runtime->config.controller_identity);
	if (status == X86_NATIVE_INPUT_EMPTY ||
	    status == X86_NATIVE_INPUT_UNAVAILABLE) {
		runtime->source_backpressure_pending = 0u;
		return X86_LEGACY_INPUT_OK;
	}
	if (status == X86_NATIVE_INPUT_OK ||
	    status == X86_NATIVE_INPUT_DROPPED) {
		/* Keep the bounded poll armed until one later sample observes OBF clear. */
		*made_progress = true;
		return X86_LEGACY_INPUT_OK;
	}
	if (status == X86_NATIVE_INPUT_RETRY) {
		/* The byte was consumed but remains represented by the raw FIFO. */
		*made_progress = true;
		return X86_LEGACY_INPUT_RETRY;
	}
	if (status == X86_NATIVE_INPUT_SOURCE_BACKPRESSURE)
		return X86_LEGACY_INPUT_RETRY;
	if (status == X86_NATIVE_INPUT_STREAM_LOST ||
	    status == X86_NATIVE_INPUT_RECOVERY_PENDING) {
		if (!refresh_keyboard_stream_state(runtime)) {
			quarantine_runtime(runtime);
			return X86_LEGACY_INPUT_POISONED;
		}
		return X86_LEGACY_INPUT_RECOVERY_PENDING;
	}
	if (status == X86_NATIVE_INPUT_STREAM_ISOLATED) {
		if (!refresh_keyboard_stream_state(runtime)) {
			quarantine_runtime(runtime);
			return X86_LEGACY_INPUT_POISONED;
		}
		return isolate_keyboard_stream(runtime);
	}
	quarantine_runtime(runtime);
	return X86_LEGACY_INPUT_POISONED;
}

enum x86_legacy_input_status x86_legacy_input_runtime_pump(
	struct x86_legacy_input_runtime *runtime,
	kernel_object_handle_t identity, uint16_t budget,
	struct x86_legacy_input_pump_result *result)
{
	enum x86_legacy_input_status status = X86_LEGACY_INPUT_OK;
	uint16_t remaining = budget;
	uint16_t completed = 0u;
	bool maintenance_progress = false;
	bool protocol_progress = false;

	if (runtime == NULL || result == NULL || budget == 0u)
		return X86_LEGACY_INPUT_INVALID_ARGUMENT;
	*result = (struct x86_legacy_input_pump_result){0};
	if (runtime->poisoned != 0u &&
	    runtime->phase == X86_LEGACY_INPUT_ACTIVE) {
		quarantine_runtime(runtime);
		return X86_LEGACY_INPUT_POISONED;
	}
	status = runtime_status(runtime, identity);
	if (status != X86_LEGACY_INPUT_OK)
		return status;
	if (runtime->phase != X86_LEGACY_INPUT_ACTIVE ||
	    runtime->focus == X86_LEGACY_INPUT_FOCUS_NONE)
		return X86_LEGACY_INPUT_INVALID_STATE;
	if (runtime->io_fault_pending != 0u) {
		quarantine_runtime(runtime);
		return X86_LEGACY_INPUT_POISONED;
	}
	if (runtime->stream_isolated != 0u)
		return isolate_keyboard_stream(runtime);
	runtime->pump_count = saturating_increment(runtime->pump_count);
	/* Decode consumers first. This creates room for both queued scan bytes and
	 * synthetic key releases emitted by exact-epoch reconnect. */
	if (runtime->atkbd_endpoint.input_device.queue_count != 0u) {
		status = pump_decoded(runtime, remaining, &completed);
		result->input_events += completed;
		remaining = (uint16_t)(remaining - completed);
		if (status != X86_LEGACY_INPUT_OK) {
			status = account_pump_status(runtime, status);
			if (runtime->stream_recovery_pending != 0u &&
			    status == X86_LEGACY_INPUT_RETRY)
				status = X86_LEGACY_INPUT_RECOVERY_PENDING;
			goto finish;
		}
	}
	if (remaining != 0u && runtime->stream_recovery_pending == 0u &&
	    runtime->native_input.keyboard_port.queue_count != 0u) {
		enum serio_status serio_status = serio_port_pump(
			&runtime->native_input.keyboard_port, remaining,
			&completed);

		result->serio_events += completed;
		remaining = (uint16_t)(remaining - completed);
		status = map_serio(serio_status);
		if (status == X86_LEGACY_INPUT_RECOVERY_PENDING ||
		    status == X86_LEGACY_INPUT_STREAM_ISOLATED) {
			if (!refresh_keyboard_stream_state(runtime)) {
				quarantine_runtime(runtime);
				status = X86_LEGACY_INPUT_POISONED;
				goto finish;
			}
			if (runtime->stream_isolated != 0u) {
				status = isolate_keyboard_stream(runtime);
				goto finish;
			}
		} else if (status != X86_LEGACY_INPUT_OK) {
			status = account_pump_status(runtime, status);
			goto finish;
		}
	}
	if (remaining != 0u &&
	    runtime->atkbd_endpoint.input_device.queue_count != 0u) {
		status = pump_decoded(runtime, remaining, &completed);
		result->input_events += completed;
		remaining = (uint16_t)(remaining - completed);
		if (status != X86_LEGACY_INPUT_OK) {
			status = account_pump_status(runtime, status);
			if (runtime->stream_recovery_pending != 0u &&
			    status == X86_LEGACY_INPUT_RETRY)
				status = X86_LEGACY_INPUT_RECOVERY_PENDING;
			goto finish;
		}
	}
	if (runtime->stream_recovery_pending != 0u) {
		if (remaining == 0u) {
			status = X86_LEGACY_INPUT_RECOVERY_PENDING;
			goto finish;
		}
		remaining--;
		status = recover_keyboard_stream(runtime, result,
						 &maintenance_progress);
		if (status != X86_LEGACY_INPUT_OK)
			goto finish;
		if (remaining != 0u &&
		    runtime->atkbd_endpoint.input_device.queue_count != 0u) {
			status = pump_decoded(runtime, remaining, &completed);
			result->input_events += completed;
			remaining = (uint16_t)(remaining - completed);
			if (status != X86_LEGACY_INPUT_OK) {
				status = account_pump_status(runtime, status);
				goto finish;
			}
		}
	}
	if (remaining != 0u) {
		status = pump_protocol_maintenance(runtime,
						   &protocol_progress);
		if (status != X86_LEGACY_INPUT_OK) {
			status = account_pump_status(runtime, status);
			goto finish;
		}
		if (protocol_progress) {
			remaining--;
			maintenance_progress = true;
		}
	}
	if (remaining != 0u &&
	    runtime->focus == X86_LEGACY_INPUT_FOCUS_GUEST) {
		size_t processed = 0u;
		enum guest_ps2_keyboard_status guest_status =
			guest_ps2_keyboard_pump(
				&runtime->guest_keyboard,
				&runtime->guest_reference, remaining, &processed);

		if (processed > remaining) {
			quarantine_runtime(runtime);
			status = X86_LEGACY_INPUT_POISONED;
			goto finish;
		}
		result->guest_device_events += processed;
		remaining = (uint16_t)(remaining - (uint16_t)processed);
		status = map_guest(guest_status);
		if (status != X86_LEGACY_INPUT_OK) {
			status = account_pump_status(runtime, status);
			goto finish;
		}
	}
	/* One active sample is sufficient to recover a held OBF byte and remains
	 * bounded even if the controller or producer is uncooperative. */
	if (runtime->source_backpressure_pending != 0u && remaining != 0u) {
		remaining--;
		status = capture_backpressured_source(
			runtime, result, &maintenance_progress);
		if (status != X86_LEGACY_INPUT_OK)
			goto finish;
	}
	if (runtime->stream_recovery_pending != 0u)
		status = X86_LEGACY_INPUT_RECOVERY_PENDING;
	else if (runtime->stream_isolated != 0u)
		status = X86_LEGACY_INPUT_STREAM_ISOLATED;
	else if (runtime->source_backpressure_pending != 0u)
		status = X86_LEGACY_INPUT_RETRY;

finish:
	(void)remaining;
	result->made_progress =
		(maintenance_progress || result->serio_events != 0u ||
		 result->input_events != 0u ||
		 result->guest_device_events != 0u ||
		 result->active_captures != 0u ||
		 result->stream_recoveries != 0u)
			? 1u
			: 0u;
	return status;
}

enum x86_legacy_input_status x86_legacy_input_runtime_rollback(
	struct x86_legacy_input_runtime *runtime,
	kernel_object_handle_t identity)
{
	enum x86_legacy_input_status status = runtime_status(runtime, identity);

	if (status != X86_LEGACY_INPUT_OK)
		return status;
	if (runtime->protocol_write_uncertain != 0u)
		return X86_LEGACY_INPUT_PROTOCOL_UNCERTAIN;
	if (runtime->protocol_committed != 0u)
		return X86_LEGACY_INPUT_PROTOCOL_COMMITTED;
	if (runtime->phase != X86_LEGACY_INPUT_PREPARED &&
	    runtime->phase != X86_LEGACY_INPUT_PUBLISHING)
		return X86_LEGACY_INPUT_INVALID_STATE;
	if (!rollback_precommit(runtime)) {
		quarantine_runtime(runtime);
		return X86_LEGACY_INPUT_POISONED;
	}
	runtime->phase = X86_LEGACY_INPUT_RETIRED;
	return X86_LEGACY_INPUT_OK;
}

enum x86_legacy_input_status x86_legacy_input_runtime_snapshot(
	const struct x86_legacy_input_runtime *runtime,
	kernel_object_handle_t identity,
	struct x86_legacy_input_snapshot *snapshot)
{
	struct keyboard_console_snapshot console_snapshot;
	struct input_core_snapshot core_snapshot;
	struct input_device_snapshot device_snapshot;
	enum x86_legacy_input_status status;

	if (runtime == NULL || snapshot == NULL)
		return X86_LEGACY_INPUT_INVALID_ARGUMENT;
	status = runtime_status(runtime, identity);
	if (status != X86_LEGACY_INPUT_OK &&
	    status != X86_LEGACY_INPUT_POISONED)
		return status;
	*snapshot = (struct x86_legacy_input_snapshot){
		.identity = runtime->config.identity,
		.focused_handler_identity = KERNEL_OBJECT_HANDLE_INVALID,
		.generation = runtime->generation,
		.irq_count = runtime->irq_count,
		.irq_empty_count = runtime->irq_empty_count,
		.irq_fault_count = runtime->irq_fault_count,
		.pump_count = runtime->pump_count,
		.negotiation_step_count = runtime->negotiation_step_count,
		.protocol_write_attempt_count =
			runtime->protocol_write_attempt_count,
		.source_backpressure_count = runtime->source_backpressure_count,
		.active_capture_count = runtime->active_capture_count,
		.stream_loss_epoch = runtime->stream_loss_epoch,
		.stream_recovery_epoch = runtime->stream_recovery_epoch,
		.stream_recovery_attempt_count =
			runtime->stream_recovery_attempt_count,
		.stream_isolation_count = runtime->stream_isolation_count,
		.raw_queue_count = runtime->native_input.keyboard_port.queue_count,
		.decoded_queue_count =
			runtime->atkbd_endpoint.input_device.queue_count,
		.phase = runtime->phase,
		.focus = runtime->focus,
		.scan_mode = runtime->scan_mode,
		.translation_enabled = runtime->translation_enabled,
		.protocol_committed = runtime->protocol_committed,
		.protocol_write_uncertain =
			runtime->protocol_write_uncertain,
		.irq_action_registered =
			(runtime->acquired & ACQUIRED_IRQ_ACTION) != 0u,
		.controller_quarantined = runtime->controller_quarantined,
		.source_backpressure_pending =
			runtime->source_backpressure_pending,
		.stream_recovery_pending = runtime->stream_recovery_pending,
		.stream_isolated = runtime->stream_isolated,
		.io_fault_pending = runtime->io_fault_pending,
		.poisoned = runtime->poisoned,
		.reserved = {0u},
	};
	if ((runtime->acquired & ACQUIRED_INPUT_CORE) != 0u &&
	    input_core_snapshot(&runtime->input_core, &core_snapshot) == INPUT_OK)
		snapshot->focused_handler_identity =
			core_snapshot.focus_identity;
	if ((runtime->acquired & ACQUIRED_CONSOLE_HANDLER) != 0u &&
	    keyboard_console_snapshot(&runtime->console, &console_snapshot) ==
		    INPUT_OK &&
	    console_snapshot.queued_keys <= (uint32_t)(uint16_t)-1)
		snapshot->console_queue_count =
			(uint16_t)console_snapshot.queued_keys;
	if ((runtime->acquired & ACQUIRED_NATIVE_INPUT_PREPARED) != 0u &&
	    input_device_snapshot(&runtime->atkbd_endpoint.input_device,
				  &device_snapshot) == INPUT_OK) {
		snapshot->decoded_queue_count = device_snapshot.queue_count;
	}
	return status;
}

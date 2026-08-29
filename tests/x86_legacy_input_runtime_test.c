// SPDX-License-Identifier: GPL-2.0-only
/* Host-safe lifecycle, negotiation, focus and IRQ tests. */
#include "console.h"
#include "test_entry.h"
#include "x86_guest_space.h"
#include "x86_legacy_input_runtime.h"

#define OUTPUT_CAPACITY 64u
#define INJECTED_CAPACITY 32u

struct runtime_fixture {
	struct x86_legacy_input_runtime runtime;
	struct serio_port *serio_ports[2];
	struct serio_driver *serio_drivers[2];
	struct input_device *input_devices[2];
	struct input_handler *input_handlers[4];
	struct serio_raw_event raw_queue[8];
	struct input_event decoded_queue[1];
	struct keyboard_key_record console_queue[1];
};

struct fake_i8042 {
	uint8_t output[OUTPUT_CAPACITY];
	uint16_t output_head;
	uint16_t output_count;
	uint8_t command_byte;
	uint8_t expect_command_byte;
	uint8_t keyboard_ack;
	uint8_t input_status_fault_once;
	uint8_t input_data_fault_once;
	uint8_t next_input_status_bits;
	uint8_t last_keyboard_write;
	uint32_t keyboard_write_count;
};

static struct fake_i8042 hardware;
static struct x86_native_irq_action_config registered_action;
static struct x86_native_irq_action_binding registered_binding;
static uint64_t action_generation;
static uint8_t action_registered;
static uint8_t guest_bound;
static uint8_t guest_quiesced;
static uint8_t guest_event_pending;
static uint8_t guest_event_retry_once;
static uint8_t injected[INJECTED_CAPACITY];
static size_t injected_count;

static void reset_hardware(bool acknowledge_keyboard)
{
	hardware = (struct fake_i8042){
		.command_byte = (uint8_t)(X86_I8042_COMMAND_BYTE_SYSTEM |
					 X86_I8042_COMMAND_BYTE_TRANSLATE |
					 X86_I8042_COMMAND_BYTE_IRQ1),
		.keyboard_ack = acknowledge_keyboard ? 1u : 0u,
	};
}

static bool enqueue_output(uint8_t value)
{
	uint16_t index;

	if (hardware.output_count >= ARRAY_SIZE(hardware.output))
		return false;
	index = (uint16_t)((hardware.output_head + hardware.output_count) %
			   ARRAY_SIZE(hardware.output));
	hardware.output[index] = value;
	hardware.output_count++;
	return true;
}

static bool read_port(uint16_t port, uint8_t *value)
{
	if (value == NULL)
		return false;
	if (port == X86_I8042_STATUS_PORT) {
		*value = (uint8_t)(X86_I8042_STATUS_SYSTEM |
			(hardware.output_count != 0u
				 ? X86_I8042_STATUS_OUTPUT_FULL
				 : 0u));
		return true;
	}
	if (port != X86_I8042_DATA_PORT || hardware.output_count == 0u)
		return false;
	*value = hardware.output[hardware.output_head];
	hardware.output_head = (uint16_t)((hardware.output_head + 1u) %
					  ARRAY_SIZE(hardware.output));
	hardware.output_count--;
	return true;
}

static bool write_port(uint16_t port, uint8_t value)
{
	if (port == X86_I8042_COMMAND_PORT) {
		if (value == 0x20u)
			return enqueue_output(hardware.command_byte);
		if (value == 0x60u) {
			hardware.expect_command_byte = 1u;
			return true;
		}
		return false;
	}
	if (port != X86_I8042_DATA_PORT)
		return false;
	if (hardware.expect_command_byte != 0u) {
		hardware.expect_command_byte = 0u;
		hardware.command_byte = value;
		return true;
	}
	hardware.keyboard_write_count++;
	hardware.last_keyboard_write = value;
	return hardware.keyboard_ack == 0u || enqueue_output(0xfau);
}

static enum x86_native_i8042_io_status control_read8(
	kernel_object_handle_t context, uint16_t port, uint8_t *value)
{
	if (context == 0u)
		return X86_NATIVE_I8042_IO_FAULT;
	return read_port(port, value) ? X86_NATIVE_I8042_IO_OK
				      : X86_NATIVE_I8042_IO_FAULT;
}

static enum x86_native_i8042_io_status control_write8(
	kernel_object_handle_t context, uint16_t port, uint8_t value)
{
	if (context == 0u)
		return X86_NATIVE_I8042_IO_FAULT;
	return write_port(port, value) ? X86_NATIVE_I8042_IO_OK
				       : X86_NATIVE_I8042_IO_FAULT;
}

static enum x86_native_input_status input_read8(
	kernel_object_handle_t context, uint16_t port, uint8_t *value)
{
	if (context == 0u)
		return X86_NATIVE_INPUT_IO_ERROR;
	if (port == X86_I8042_STATUS_PORT &&
	    hardware.input_status_fault_once != 0u) {
		hardware.input_status_fault_once = 0u;
		return X86_NATIVE_INPUT_IO_ERROR;
	}
	if (!read_port(port, value))
		return X86_NATIVE_INPUT_IO_ERROR;
	if (port == X86_I8042_STATUS_PORT) {
		*value |= hardware.next_input_status_bits;
		hardware.next_input_status_bits = 0u;
	}
	return X86_NATIVE_INPUT_OK;
}

static enum x86_native_input_status input_write8(
	kernel_object_handle_t context, uint16_t port, uint8_t value)
{
	if (context == 0u)
		return X86_NATIVE_INPUT_IO_ERROR;
	if (port == X86_I8042_DATA_PORT &&
	    hardware.input_data_fault_once != 0u) {
		hardware.input_data_fault_once = 0u;
		return X86_NATIVE_INPUT_IO_ERROR;
	}
	return write_port(port, value) ? X86_NATIVE_INPUT_OK
				       : X86_NATIVE_INPUT_IO_ERROR;
}

static void test_wait(void *context)
{
	(void)context;
}

void keyboard_console_x86_wait(void *context)
{
	(void)context;
}

void console_putc(char character)
{
	(void)character;
}

void console_backspace(void)
{
}

enum x86_legacy_irq_status x86_legacy_irq_snapshot(
	kernel_object_handle_t source_identity,
	struct x86_legacy_irq_snapshot *snapshot)
{
	if (source_identity == 0u || snapshot == NULL)
		return X86_LEGACY_IRQ_INVALID_ARGUMENT;
	*snapshot = (struct x86_legacy_irq_snapshot){
		.source_identity = source_identity,
		.present_irq_mask = (uint16_t)(1u << X86_LEGACY_KEYBOARD_IRQ),
		.enabled_irq_mask = (uint16_t)(1u << X86_LEGACY_KEYBOARD_IRQ),
		.phase = X86_LEGACY_IRQ_PREPARED,
		.reserved = {0u},
	};
	return X86_LEGACY_IRQ_OK;
}

enum x86_legacy_irq_status x86_legacy_irq_action_register(
	kernel_object_handle_t source_identity,
	const struct x86_native_irq_action_config *config,
	struct x86_native_irq_action_binding *binding)
{
	if (source_identity == 0u || config == NULL || binding == NULL ||
	    config->handler == NULL || action_registered != 0u)
		return X86_LEGACY_IRQ_INVALID_ARGUMENT;
	action_generation++;
	registered_action = *config;
	registered_binding = (struct x86_native_irq_action_binding){
		.dispatch_identity = source_identity + 1u,
		.dispatch_generation = 1u,
		.action_identity = config->identity,
		.action_generation = action_generation,
		.slot = 0u,
		.reserved = {0u},
	};
	*binding = registered_binding;
	action_registered = 1u;
	return X86_LEGACY_IRQ_OK;
}

enum x86_legacy_irq_status x86_legacy_irq_action_quiesce(
	kernel_object_handle_t source_identity,
	const struct x86_native_irq_action_binding *binding)
{
	if (source_identity == 0u || binding == NULL ||
	    action_registered == 0u ||
	    binding->action_generation != registered_binding.action_generation)
		return X86_LEGACY_IRQ_STALE_BINDING;
	return X86_LEGACY_IRQ_OK;
}

enum x86_legacy_irq_status x86_legacy_irq_action_unregister(
	kernel_object_handle_t source_identity,
	const struct x86_native_irq_action_binding *binding)
{
	enum x86_legacy_irq_status status =
		x86_legacy_irq_action_quiesce(source_identity, binding);

	if (status != X86_LEGACY_IRQ_OK)
		return status;
	action_registered = 0u;
	return X86_LEGACY_IRQ_OK;
}

enum x86_guest_space_status x86_guest_space_i8042_input_bind(
	kernel_object_handle_t machine_identity,
	kernel_object_handle_t source_identity,
	const struct x86_i8042_input_config *config,
	struct x86_i8042_input_binding *binding)
{
	if (machine_identity == 0u || source_identity == 0u || config == NULL ||
	    binding == NULL || guest_bound != 0u ||
	    config->capabilities != X86_I8042_INPUT_KEYBOARD)
		return X86_GUEST_SPACE_INVALID_ARGUMENT;
	*binding = (struct x86_i8042_input_binding){
		.context_identity = machine_identity,
		.owner_identity = machine_identity + 1u,
		.controller_generation = 1u,
		.source_identity = source_identity,
		.source_generation = 1u,
		.capabilities = X86_I8042_INPUT_KEYBOARD,
		.reserved = {0u},
	};
	guest_bound = 1u;
	guest_quiesced = 0u;
	return X86_GUEST_SPACE_OK;
}

enum x86_guest_space_status x86_guest_space_i8042_input_quiesce(
	kernel_object_handle_t machine_identity,
	const struct x86_i8042_input_binding *binding)
{
	if (binding == NULL || guest_bound == 0u ||
	    binding->context_identity != machine_identity)
		return X86_GUEST_SPACE_STALE_BINDING;
	guest_quiesced = 1u;
	return X86_GUEST_SPACE_OK;
}

enum x86_guest_space_status x86_guest_space_i8042_input_unbind(
	kernel_object_handle_t machine_identity,
	const struct x86_i8042_input_binding *binding)
{
	if (binding == NULL || guest_bound == 0u || guest_quiesced == 0u ||
	    binding->context_identity != machine_identity)
		return X86_GUEST_SPACE_STALE_BINDING;
	guest_bound = 0u;
	guest_quiesced = 0u;
	return X86_GUEST_SPACE_OK;
}

enum x86_guest_space_status x86_guest_space_i8042_input_keyboard_mode(
	const struct x86_i8042_input_binding *binding,
	struct x86_i8042_keyboard_mode *mode)
{
	if (binding == NULL || mode == NULL || guest_bound == 0u)
		return X86_GUEST_SPACE_STALE_BINDING;
	*mode = (struct x86_i8042_keyboard_mode){
		.source_identity = binding->source_identity,
		.controller_generation = binding->controller_generation,
		.source_generation = binding->source_generation,
		.mode_generation = 1u,
		.scan_set = 2u,
		.translation_enabled = 1u,
		.scanning_enabled = 1u,
		.interface_enabled = 1u,
		.reserved = {0u},
	};
	return X86_GUEST_SPACE_OK;
}

enum x86_guest_space_status
x86_guest_space_i8042_input_inject_keyboard_sequence(
	const struct x86_i8042_input_binding *binding,
	const struct x86_i8042_keyboard_mode *mode, const uint8_t *values,
	size_t values_capacity, size_t count)
{
	size_t index;

	if (binding == NULL || mode == NULL || values == NULL ||
	    guest_bound == 0u || count == 0u || count > values_capacity ||
	    count > ARRAY_SIZE(injected) - injected_count)
		return X86_GUEST_SPACE_INVALID_ARGUMENT;
	for (index = 0u; index < count; ++index)
		injected[injected_count++] = values[index];
	guest_event_pending = 1u;
	return X86_GUEST_SPACE_OK;
}

enum x86_guest_space_status x86_guest_space_device_events_pump(
	kernel_object_handle_t machine_identity, size_t budget,
	size_t *processed)
{
	if (machine_identity == 0u || budget == 0u || processed == NULL)
		return X86_GUEST_SPACE_INVALID_ARGUMENT;
	if (guest_event_retry_once != 0u) {
		guest_event_retry_once = 0u;
		*processed = 0u;
		return X86_GUEST_SPACE_DEVICE_EVENT_RETRY;
	}
	*processed = guest_event_pending != 0u ? 1u : 0u;
	guest_event_pending = 0u;
	return X86_GUEST_SPACE_OK;
}

static struct x86_legacy_input_runtime_config make_config(
	struct runtime_fixture *fixture, kernel_object_handle_t base,
	uint32_t negotiation_steps)
{
	return (struct x86_legacy_input_runtime_config){
		.identity = base + 1u,
		.serio_registry_identity = base + 2u,
		.input_core_identity = base + 3u,
		.controller_identity = base + 4u,
		.io_context_identity = base + 5u,
		.keyboard_port_identity = base + 6u,
		.atkbd_driver_identity = base + 7u,
		.input_device_identity = base + 8u,
		.console_handler_identity = base + 9u,
		.guest_handler_identity = base + 10u,
		.guest_context_identity = base + 11u,
		.guest_machine_identity = base + 12u,
		.guest_source_identity = base + 13u,
		.legacy_irq_source_identity = base + 14u,
		.irq_action_identity = base + 15u,
		.control_read8 = control_read8,
		.control_write8 = control_write8,
		.input_read8 = input_read8,
		.input_write8 = input_write8,
		.console_wait = test_wait,
		.console_wait_context = NULL,
		.storage = {
			.serio_ports = fixture->serio_ports,
			.serio_drivers = fixture->serio_drivers,
			.input_devices = fixture->input_devices,
			.input_handlers = fixture->input_handlers,
			.keyboard_bytes = fixture->raw_queue,
			.decoded_events = fixture->decoded_queue,
			.console_keys = fixture->console_queue,
			.serio_port_capacity = ARRAY_SIZE(fixture->serio_ports),
			.serio_driver_capacity =
				ARRAY_SIZE(fixture->serio_drivers),
			.input_device_capacity =
				ARRAY_SIZE(fixture->input_devices),
			.input_handler_capacity =
				ARRAY_SIZE(fixture->input_handlers),
			.keyboard_byte_capacity = ARRAY_SIZE(fixture->raw_queue),
			.decoded_event_capacity =
				ARRAY_SIZE(fixture->decoded_queue),
			.console_key_capacity = ARRAY_SIZE(fixture->console_queue),
			.reserved = {0u},
		},
		.controller_poll_limit = 32u,
		.input_write_poll_limit = 32u,
		.negotiation_step_limit = negotiation_steps,
		.controller_drain_limit = 8u,
		.controller_stability_attempts = 4u,
		.atkbd_command_write_limit = 24u,
		.atkbd_command_nak_limit = 3u,
		.controller_present = 1u,
		.keyboard_present = 1u,
		.presence_evidence =
			X86_NATIVE_INPUT_EVIDENCE_CONTROLLER_OBSERVED,
		.reserved = {0u},
	};
}

static enum x86_native_irq_action_result inject_native_byte(uint8_t value)
{
	const struct x86_native_irq_event event = {
		.controller_identity = 0x9000u,
		.controller_generation = 1u,
		.sequence = 1u,
		.vector = X86_LEGACY_KEYBOARD_VECTOR,
		.hardware_irq = X86_LEGACY_KEYBOARD_IRQ,
		.line_flags = 0u,
		.reserved = {0u},
	};

	if (!enqueue_output(value) || action_registered == 0u)
		return X86_NATIVE_IRQ_ACTION_FAULT;
	return registered_action.handler(registered_action.context, &event);
}

static enum x86_native_irq_action_result call_action_with_context(
	kernel_object_handle_t context)
{
	const struct x86_native_irq_event event = {
		.controller_identity = 0x9000u,
		.controller_generation = 1u,
		.sequence = 2u,
		.vector = X86_LEGACY_KEYBOARD_VECTOR,
		.hardware_irq = X86_LEGACY_KEYBOARD_IRQ,
		.line_flags = 0u,
		.reserved = {0u},
	};

	return registered_action.handler(context, &event);
}

static int test_precommit_rollback(void)
{
	static struct runtime_fixture fixture;
	struct x86_legacy_input_runtime_config config =
		make_config(&fixture, 0x1000u, 64u);
	uint8_t original;

	reset_hardware(true);
	original = hardware.command_byte;
	x86_legacy_input_runtime_construct(&fixture.runtime);
	if (x86_legacy_input_runtime_prepare(&fixture.runtime, &config) !=
		    X86_LEGACY_INPUT_OK ||
	    fixture.runtime.phase != X86_LEGACY_INPUT_PREPARED ||
	    fixture.runtime.protocol_committed != 0u ||
	    hardware.command_byte == original)
		return 1;
	if (x86_legacy_input_runtime_rollback(&fixture.runtime,
					      config.identity) !=
		    X86_LEGACY_INPUT_OK ||
	    fixture.runtime.phase != X86_LEGACY_INPUT_RETIRED ||
	    hardware.command_byte != original)
		return 2;
	return 0;
}

static int test_zero_commit_publish_rollback(void)
{
	static struct runtime_fixture fixture;
	struct x86_legacy_input_runtime_config config =
		make_config(&fixture, 0x1800u, 8u);
	enum x86_legacy_input_status status;
	uint8_t original;

	reset_hardware(true);
	original = hardware.command_byte;
	hardware.input_status_fault_once = 1u;
	x86_legacy_input_runtime_construct(&fixture.runtime);
	if (x86_legacy_input_runtime_prepare(&fixture.runtime, &config) !=
		    X86_LEGACY_INPUT_OK)
		return 1;
	status = x86_legacy_input_runtime_publish(&fixture.runtime,
						 config.identity);
	if (status == X86_LEGACY_INPUT_OK ||
	    status == X86_LEGACY_INPUT_POISONED ||
	    fixture.runtime.phase != X86_LEGACY_INPUT_RETIRED ||
	    fixture.runtime.protocol_committed != 0u ||
	    fixture.runtime.protocol_write_uncertain != 0u ||
	    fixture.runtime.protocol_write_attempt_count != 1u ||
	    fixture.runtime.controller_quarantined != 0u ||
	    hardware.keyboard_write_count != 0u ||
	    hardware.command_byte != original || action_registered != 0u)
		return 2;
	return 0;
}

static int test_postcommit_poison(void)
{
	static struct runtime_fixture fixture;
	struct x86_legacy_input_runtime_config config =
		make_config(&fixture, 0x2000u, 8u);
	struct x86_legacy_input_snapshot snapshot;

	reset_hardware(false);
	x86_legacy_input_runtime_construct(&fixture.runtime);
	if (x86_legacy_input_runtime_prepare(&fixture.runtime, &config) !=
		    X86_LEGACY_INPUT_OK ||
	    x86_legacy_input_runtime_publish(&fixture.runtime, config.identity) !=
		    X86_LEGACY_INPUT_POISONED ||
	    x86_legacy_input_runtime_snapshot(
		    &fixture.runtime, config.identity, &snapshot) !=
		    X86_LEGACY_INPUT_POISONED ||
	    snapshot.phase != X86_LEGACY_INPUT_POISONED_PHASE ||
	    snapshot.protocol_committed == 0u || snapshot.poisoned == 0u ||
	    snapshot.controller_quarantined == 0u || action_registered != 0u ||
	    (hardware.command_byte &
	     X86_I8042_COMMAND_BYTE_KEYBOARD_DISABLED) == 0u ||
	    (hardware.command_byte & X86_I8042_COMMAND_BYTE_IRQ1) != 0u)
		return 1;
	return 0;
}

static int test_uncertain_write_quarantine(void)
{
	static struct runtime_fixture fixture;
	struct x86_legacy_input_runtime_config config =
		make_config(&fixture, 0x2800u, 8u);
	struct x86_legacy_input_snapshot snapshot;

	reset_hardware(true);
	hardware.input_data_fault_once = 1u;
	x86_legacy_input_runtime_construct(&fixture.runtime);
	if (x86_legacy_input_runtime_prepare(&fixture.runtime, &config) !=
		    X86_LEGACY_INPUT_OK ||
	    x86_legacy_input_runtime_publish(&fixture.runtime, config.identity) !=
		    X86_LEGACY_INPUT_POISONED ||
	    x86_legacy_input_runtime_snapshot(
		    &fixture.runtime, config.identity, &snapshot) !=
		    X86_LEGACY_INPUT_POISONED ||
	    snapshot.phase != X86_LEGACY_INPUT_POISONED_PHASE ||
	    snapshot.protocol_committed != 0u ||
	    snapshot.protocol_write_uncertain == 0u ||
	    snapshot.protocol_write_attempt_count != 1u ||
	    snapshot.controller_quarantined == 0u || snapshot.poisoned == 0u ||
	    hardware.keyboard_write_count != 0u || action_registered != 0u ||
	    (hardware.command_byte &
	     X86_I8042_COMMAND_BYTE_KEYBOARD_DISABLED) == 0u ||
	    (hardware.command_byte & X86_I8042_COMMAND_BYTE_IRQ1) != 0u)
		return 1;
	return 0;
}

static int test_active_routing(void)
{
	static struct runtime_fixture fixture;
	struct x86_legacy_input_runtime_config config =
		make_config(&fixture, 0x3000u, 128u);
	struct x86_legacy_input_pump_result pump;
	struct x86_legacy_input_snapshot snapshot;
	char character;

	/* A one-record raw FIFO makes the zero-commit 60h boundary observable. */
	config.storage.keyboard_byte_capacity = 1u;
	reset_hardware(true);
	keyboard_init();
	x86_legacy_input_runtime_construct(&fixture.runtime);
	if (x86_legacy_input_runtime_prepare(&fixture.runtime, &config) !=
		    X86_LEGACY_INPUT_OK ||
	    x86_legacy_input_runtime_publish(&fixture.runtime, config.identity) !=
		    X86_LEGACY_INPUT_OK ||
	    action_registered == 0u ||
	    keyboard_default_console() != &fixture.runtime.console)
		return 1;
	/* console=1, decoded=1: the third byte must wait in the raw FIFO. */
	if (inject_native_byte(0x1eu) != X86_NATIVE_IRQ_ACTION_HANDLED ||
	    inject_native_byte(0x30u) != X86_NATIVE_IRQ_ACTION_HANDLED ||
	    inject_native_byte(0x2eu) != X86_NATIVE_IRQ_ACTION_HANDLED ||
	    !keyboard_peekchar(&character) || character != 'a' ||
	    keyboard_getchar() != 'a')
		return 2;
	if (x86_legacy_input_runtime_pump(
		    &fixture.runtime, config.identity, 1u, &pump) !=
		    X86_LEGACY_INPUT_OK ||
	    pump.input_events != 1u || pump.serio_events != 0u ||
	    keyboard_getchar() != 'b')
		return 3;
	if (x86_legacy_input_runtime_pump(
		    &fixture.runtime, config.identity, 2u, &pump) !=
		    X86_LEGACY_INPUT_OK ||
	    pump.serio_events != 1u || keyboard_getchar() != 'c')
		return 4;
	if (x86_legacy_input_runtime_focus_guest(
		    &fixture.runtime, config.identity) != X86_LEGACY_INPUT_OK ||
	    guest_bound == 0u || keyboard_default_console() != NULL ||
	    inject_native_byte(0x20u) != X86_NATIVE_IRQ_ACTION_HANDLED ||
	    injected_count == 0u)
		return 5;
	guest_event_retry_once = 1u;
	if (x86_legacy_input_runtime_pump(
		    &fixture.runtime, config.identity, 2u, &pump) !=
		    X86_LEGACY_INPUT_RETRY ||
	    pump.guest_device_events != 0u ||
	    x86_legacy_input_runtime_pump(
		    &fixture.runtime, config.identity, 2u, &pump) !=
		    X86_LEGACY_INPUT_OK ||
	    pump.guest_device_events != 1u)
		return 5;
	if (x86_legacy_input_runtime_focus_console(
		    &fixture.runtime, config.identity) != X86_LEGACY_INPUT_OK ||
	    guest_bound != 0u ||
	    keyboard_default_console() != &fixture.runtime.console ||
	    x86_legacy_input_runtime_snapshot(
		    &fixture.runtime, config.identity, &snapshot) !=
		    X86_LEGACY_INPUT_OK ||
	    snapshot.focus != X86_LEGACY_INPUT_FOCUS_CONSOLE ||
	    snapshot.irq_action_registered == 0u ||
	    snapshot.translation_enabled == 0u ||
	    snapshot.scan_mode != ATKBD_SCAN_TRANSLATED_SET1)
		return 6;
	/* q occupies the console, w the decoded FIFO, e the raw FIFO. The r byte
	 * must remain in 60h, survive normal EOI, and be actively captured only
	 * after process-context pumping has created room. */
	if (inject_native_byte(0x10u) != X86_NATIVE_IRQ_ACTION_HANDLED ||
	    inject_native_byte(0x11u) != X86_NATIVE_IRQ_ACTION_HANDLED ||
	    inject_native_byte(0x12u) != X86_NATIVE_IRQ_ACTION_HANDLED ||
	    inject_native_byte(0x13u) != X86_NATIVE_IRQ_ACTION_HANDLED ||
	    hardware.output_count != 1u ||
	    x86_legacy_input_runtime_snapshot(
		    &fixture.runtime, config.identity, &snapshot) !=
		    X86_LEGACY_INPUT_OK ||
	    snapshot.source_backpressure_pending == 0u ||
	    snapshot.stream_recovery_pending != 0u ||
	    snapshot.stream_loss_epoch != 0u ||
	    snapshot.raw_queue_count != 1u ||
	    snapshot.decoded_queue_count != 1u || keyboard_getchar() != 'q')
		return 7;
	if (x86_legacy_input_runtime_pump(
		    &fixture.runtime, config.identity, 2u, &pump) !=
		    X86_LEGACY_INPUT_RETRY ||
	    pump.input_events != 1u || pump.serio_events != 1u ||
	    pump.active_captures != 0u || keyboard_getchar() != 'w')
		return 8;
	if (x86_legacy_input_runtime_pump(
		    &fixture.runtime, config.identity, 2u, &pump) !=
		    X86_LEGACY_INPUT_RETRY ||
	    pump.input_events != 1u || pump.active_captures != 1u ||
	    hardware.output_count != 0u || keyboard_getchar() != 'e')
		return 9;
	if (x86_legacy_input_runtime_pump(
		    &fixture.runtime, config.identity, 2u, &pump) !=
		    X86_LEGACY_INPUT_OK ||
	    pump.input_events != 1u || pump.active_captures != 1u ||
	    keyboard_getchar() != 'r' ||
	    x86_legacy_input_runtime_snapshot(
		    &fixture.runtime, config.identity, &snapshot) !=
		    X86_LEGACY_INPUT_OK ||
	    snapshot.source_backpressure_pending != 0u ||
	    snapshot.source_backpressure_count != 1u ||
	    snapshot.active_capture_count != 2u ||
	    snapshot.stream_loss_epoch != 0u)
		return 10;
	hardware.next_input_status_bits = X86_I8042_STATUS_PARITY;
	if (inject_native_byte(0x55u) != X86_NATIVE_IRQ_ACTION_HANDLED ||
	    x86_legacy_input_runtime_pump(
		    &fixture.runtime, config.identity, 1u, &pump) !=
		    X86_LEGACY_INPUT_OK ||
	    pump.made_progress == 0u || hardware.last_keyboard_write != 0xfeu ||
	    call_action_with_context(config.identity) !=
		    X86_NATIVE_IRQ_ACTION_HANDLED)
		return 11;
	/* Identity damage is fail-closed; recoverable port I/O defers quarantine. */
	if (call_action_with_context(config.identity + 1u) !=
		    X86_NATIVE_IRQ_ACTION_FAULT)
		return 12;
	hardware.input_status_fault_once = 1u;
	if (inject_native_byte(0x12u) != X86_NATIVE_IRQ_ACTION_HANDLED ||
	    x86_legacy_input_runtime_snapshot(
		    &fixture.runtime, config.identity, &snapshot) !=
		    X86_LEGACY_INPUT_OK ||
	    snapshot.io_fault_pending == 0u || snapshot.poisoned != 0u ||
	    x86_legacy_input_runtime_pump(
		    &fixture.runtime, config.identity, 1u, &pump) !=
		    X86_LEGACY_INPUT_POISONED ||
	    fixture.runtime.phase != X86_LEGACY_INPUT_POISONED_PHASE ||
	    action_registered != 0u)
		return 13;
	return 0;
}

static int test_stream_loss_recovery_and_isolation(void)
{
	static struct runtime_fixture fixture;
	struct x86_legacy_input_runtime_config config =
		make_config(&fixture, 0x4000u, 128u);
	struct x86_legacy_input_pump_result pump;
	struct x86_legacy_input_snapshot snapshot;
	struct atkbd_endpoint_snapshot atkbd_snapshot;
	uint8_t step;

	config.storage.keyboard_byte_capacity = 1u;
	reset_hardware(true);
	keyboard_init();
	x86_legacy_input_runtime_construct(&fixture.runtime);
	if (action_registered != 0u ||
	    x86_legacy_input_runtime_prepare(&fixture.runtime, &config) !=
		    X86_LEGACY_INPUT_OK ||
	    x86_legacy_input_runtime_publish(&fixture.runtime, config.identity) !=
		    X86_LEGACY_INPUT_OK)
		return 1;
	/* Commit three modifiers, then fill console/decoded/raw with q/w/e. The
	 * direct serio call models the race where 60h was already consumed after
	 * preflight but the raw FIFO became full before representation. */
	if (inject_native_byte(0x2au) != X86_NATIVE_IRQ_ACTION_HANDLED ||
	    inject_native_byte(0x1du) != X86_NATIVE_IRQ_ACTION_HANDLED ||
	    inject_native_byte(0x38u) != X86_NATIVE_IRQ_ACTION_HANDLED ||
	    (keyboard_bios_shift_flags() & 0x000eu) != 0x000eu ||
	    inject_native_byte(0x10u) != X86_NATIVE_IRQ_ACTION_HANDLED ||
	    inject_native_byte(0x11u) != X86_NATIVE_IRQ_ACTION_HANDLED ||
	    inject_native_byte(0x12u) != X86_NATIVE_IRQ_ACTION_HANDLED ||
	    serio_interrupt(&fixture.runtime.native_input.keyboard_port,
			    0x13u, X86_I8042_STATUS_OUTPUT_FULL, 0u) !=
		    SERIO_STREAM_LOST ||
	    inject_native_byte(0x14u) != X86_NATIVE_IRQ_ACTION_HANDLED ||
	    hardware.output_count != 1u ||
	    x86_legacy_input_runtime_snapshot(
		    &fixture.runtime, config.identity, &snapshot) !=
		    X86_LEGACY_INPUT_OK ||
	    snapshot.stream_loss_epoch != 1u ||
	    snapshot.stream_recovery_pending == 0u ||
	    snapshot.stream_isolated != 0u || snapshot.poisoned != 0u ||
	    action_registered == 0u)
		return 2;
	keyboard_flush();
	if (x86_legacy_input_runtime_pump(
		    &fixture.runtime, config.identity, 1u, &pump) !=
		    X86_LEGACY_INPUT_RECOVERY_PENDING ||
	    pump.input_events != 1u || pump.stream_recoveries != 0u)
		return 3;
	keyboard_flush();
	if (x86_legacy_input_runtime_pump(
		    &fixture.runtime, config.identity, 1u, &pump) !=
		    X86_LEGACY_INPUT_RETRY ||
	    pump.stream_recoveries != 1u ||
	    (keyboard_bios_shift_flags() & 0x000fu) != 0u ||
	    x86_legacy_input_runtime_snapshot(
		    &fixture.runtime, config.identity, &snapshot) !=
		    X86_LEGACY_INPUT_OK ||
	    snapshot.stream_recovery_pending != 0u ||
	    snapshot.stream_recovery_epoch != 1u ||
	    snapshot.source_backpressure_pending == 0u ||
	    snapshot.stream_isolated != 0u || snapshot.poisoned != 0u)
		return 4;
	/* Reconnect resets protocol state. Bounded pump calls renegotiate while
	 * actively consuming the held post-loss byte and the resulting ACKs. */
	for (step = 0u; step < 24u; ++step) {
		enum x86_legacy_input_status status =
			x86_legacy_input_runtime_pump(
				&fixture.runtime, config.identity, 2u, &pump);

		if (status != X86_LEGACY_INPUT_OK &&
		    status != X86_LEGACY_INPUT_RETRY)
			return 5;
		keyboard_flush();
		if (x86_legacy_input_runtime_snapshot(
			    &fixture.runtime, config.identity, &snapshot) !=
			    X86_LEGACY_INPUT_OK)
			return 6;
		if (snapshot.source_backpressure_pending == 0u &&
		    hardware.output_count == 0u)
			break;
	}
	if (step == 24u || snapshot.stream_recovery_epoch != 1u ||
	    snapshot.active_capture_count == 0u || snapshot.poisoned != 0u ||
	    atkbd_endpoint_snapshot(
		    &fixture.runtime.atkbd_driver, &fixture.runtime.atkbd_reference,
		    &atkbd_snapshot) != ATKBD_OK ||
	    atkbd_snapshot.negotiated == 0u || atkbd_snapshot.enabled == 0u ||
	    atkbd_snapshot.reconnect_release_count < 5u)
		return 7;

	/* A second exact loss is deliberately abandoned. The IRQ still completes;
	 * process context then isolates epoch 2, removes IRQ1, drains held output,
	 * and disables the keyboard interface without poisoning unrelated owners. */
	keyboard_flush();
	if (inject_native_byte(0x10u) != X86_NATIVE_IRQ_ACTION_HANDLED ||
	    inject_native_byte(0x11u) != X86_NATIVE_IRQ_ACTION_HANDLED ||
	    inject_native_byte(0x12u) != X86_NATIVE_IRQ_ACTION_HANDLED ||
	    serio_interrupt(&fixture.runtime.native_input.keyboard_port,
			    0x13u, X86_I8042_STATUS_OUTPUT_FULL, 0u) !=
		    SERIO_STREAM_LOST ||
	    x86_native_input_isolate_stream(
		    &fixture.runtime.native_input, config.controller_identity,
		    X86_NATIVE_INPUT_KEYBOARD, 2u) != X86_NATIVE_INPUT_OK ||
	    inject_native_byte(0x14u) != X86_NATIVE_IRQ_ACTION_HANDLED ||
	    hardware.output_count != 1u ||
	    x86_legacy_input_runtime_snapshot(
		    &fixture.runtime, config.identity, &snapshot) !=
		    X86_LEGACY_INPUT_OK ||
	    snapshot.stream_loss_epoch != 2u || snapshot.stream_isolated == 0u)
		return 8;
	if (x86_legacy_input_runtime_pump(
		    &fixture.runtime, config.identity, 1u, &pump) !=
		    X86_LEGACY_INPUT_STREAM_ISOLATED ||
	    fixture.runtime.phase != X86_LEGACY_INPUT_ACTIVE ||
	    fixture.runtime.poisoned != 0u || action_registered != 0u ||
	    hardware.output_count != 0u ||
	    (hardware.command_byte &
	     X86_I8042_COMMAND_BYTE_KEYBOARD_DISABLED) == 0u ||
	    (hardware.command_byte & X86_I8042_COMMAND_BYTE_IRQ1) != 0u ||
	    x86_legacy_input_runtime_snapshot(
		    &fixture.runtime, config.identity, &snapshot) !=
		    X86_LEGACY_INPUT_OK ||
	    snapshot.controller_quarantined == 0u ||
	    snapshot.stream_isolation_count != 1u ||
	    snapshot.stream_isolated == 0u || snapshot.poisoned != 0u)
		return 9;
	return 0;
}

static int run_tests(void)
{
	int status = test_precommit_rollback();

	if (status != 0)
		return 10 + status;
	status = test_postcommit_poison();
	if (status != 0)
		return 20 + status;
	status = test_zero_commit_publish_rollback();
	if (status != 0)
		return 25 + status;
	status = test_uncertain_write_quarantine();
	if (status != 0)
		return 28 + status;
	status = test_active_routing();
	if (status != 0)
		return 30 + status;
	status = test_stream_loss_recovery_and_isolation();
	return status == 0 ? 0 : 50 + status;
}

DOSC32_TEST_ENTRY(run_tests)

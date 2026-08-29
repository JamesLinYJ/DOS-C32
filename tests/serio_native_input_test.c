// SPDX-License-Identifier: GPL-2.0-only
/* Host-safe serio lifecycle, binding and native i8042 single-read tests. */
#include "test_entry.h"
#include "x86_native_input.h"

#define REGISTRY_ID ((kernel_object_handle_t)0x534552494f524547ull)
#define CONTROLLER_ID ((kernel_object_handle_t)0x4e41544956454354ull)
#define KBD_PORT_ID ((kernel_object_handle_t)0x4e41544956454b42ull)
#define AUX_PORT_ID ((kernel_object_handle_t)0x4e41544956454158ull)
#define DRIVER_ID ((kernel_object_handle_t)0x534552494f445256ull)
#define ROOT_ID ((kernel_object_handle_t)0x534552494f524f4full)
#define CHILD_ID ((kernel_object_handle_t)0x534552494f434849ull)
#define LEAF_ID ((kernel_object_handle_t)0x534552494f4c4541ull)

static struct serio_registry registry;
static struct serio_port *small_ports[2];
static struct serio_driver *small_drivers[1];
static struct serio_port *ports[8];
static struct serio_driver *drivers[4];
static struct serio_port *larger_ports[9];
static struct serio_driver *larger_drivers[5];
static struct serio_driver driver;
static struct x86_native_input_controller controller;
static struct serio_raw_event keyboard_queue[2];
static struct serio_raw_event auxiliary_queue[2];
static uint16_t read_ports[8];
static uint8_t read_status;
static uint8_t read_data;
static uint8_t read_count;
static uint8_t read_fault_call;
static uint16_t write_ports[8];
static uint8_t write_values[8];
static uint8_t write_count;
static uint8_t write_fault_call;
static uint8_t defer_enabled;
static uint8_t received_count;
static struct serio_raw_event received[16];
static uint8_t reconnect_count;
static uint8_t reconnect_retry_once;
static uint8_t stop_count;
static kernel_object_handle_t stop_order[3];

static enum x86_native_input_status read8(
	kernel_object_handle_t context, uint16_t port, uint8_t *value)
{
	if (context != CONTROLLER_ID || value == NULL || read_count >= 8u)
		return X86_NATIVE_INPUT_IO_ERROR;
	read_ports[read_count++] = port;
	if (read_fault_call == read_count)
		return X86_NATIVE_INPUT_IO_ERROR;
	if (port == 0x64u)
		*value = read_status;
	else if (port == 0x60u)
		*value = read_data;
	else
		return X86_NATIVE_INPUT_IO_ERROR;
	return X86_NATIVE_INPUT_OK;
}

static enum x86_native_input_status write8(
	kernel_object_handle_t context, uint16_t port, uint8_t value)
{
	if (context != CONTROLLER_ID || write_count >= ARRAY_SIZE(write_ports))
		return X86_NATIVE_INPUT_IO_ERROR;
	write_ports[write_count] = port;
	write_values[write_count] = value;
	write_count++;
	if (write_fault_call == write_count)
		return X86_NATIVE_INPUT_IO_ERROR;
	return X86_NATIVE_INPUT_OK;
}

static bool write_result_is(struct serio_write_result result,
			    enum serio_status status,
			    enum serio_write_commit commit)
{
	return result.status == status && result.commit == commit;
}

static void reset_write_fixture(uint8_t status)
{
	read_status = status;
	read_count = 0u;
	read_fault_call = 0u;
	write_count = 0u;
	write_fault_call = 0u;
}

static enum serio_status connect_driver(struct serio_port *port,
	struct serio_driver *candidate, void **binding_context)
{
	if (port == NULL || candidate != &driver || binding_context == NULL)
		return SERIO_INVALID_ARGUMENT;
	*binding_context = candidate;
	return SERIO_OK;
}

static void disconnect_driver(struct serio_port *port,
	struct serio_driver *candidate, void *binding_context)
{
	(void)port;
	(void)candidate;
	(void)binding_context;
}

static enum serio_status reconnect_driver(struct serio_port *port,
	struct serio_driver *candidate, void *binding_context)
{
	if (port == NULL || candidate != &driver || binding_context != &driver)
		return SERIO_INVALID_STATE;
	reconnect_count++;
	if (reconnect_retry_once != 0u) {
		reconnect_retry_once = 0u;
		return SERIO_RETRY;
	}
	return SERIO_OK;
}

static enum serio_receive_result receive_byte(
	struct serio_port *port, struct serio_driver *candidate,
	void *binding_context, const struct serio_raw_event *event)
{
	if (port == NULL || candidate != &driver || binding_context != &driver ||
	    event == NULL || received_count >= ARRAY_SIZE(received))
		return SERIO_RECEIVE_REJECTED;
	received[received_count++] = *event;
	if (event->data == 0x20u && defer_enabled != 0u)
		return SERIO_RECEIVE_DEFER;
	if (event->data == 0x30u)
		return SERIO_RECEIVE_REJECTED;
	return SERIO_RECEIVE_HANDLED;
}

static enum serio_status open_port(struct serio_port *port,
	struct serio_driver *candidate, void *binding_context)
{
	return port != NULL && port->driver == candidate &&
		       binding_context == candidate
		       ? SERIO_OK
		       : SERIO_INVALID_STATE;
}

static void stop_port(struct serio_port *port)
{
	if (stop_count < ARRAY_SIZE(stop_order))
		stop_order[stop_count++] = port->config.identity;
}

static enum serio_status fail_start(struct serio_port *port)
{
	(void)port;
	return SERIO_INVALID_STATE;
}

static struct x86_native_input_config native_config(void)
{
	const struct x86_native_input_config config = {
		.controller_identity = CONTROLLER_ID,
		.callback_context = CONTROLLER_ID,
		.keyboard_port_identity = KBD_PORT_ID,
		.auxiliary_port_identity = AUX_PORT_ID,
		.registry = &registry,
		.keyboard_queue = keyboard_queue,
		.auxiliary_queue = auxiliary_queue,
		.read8 = read8,
		.write8 = write8,
		.irq_enter = NULL,
		.irq_exit = NULL,
		.data_port = 0x60u,
		.status_port = 0x64u,
		.command_port = 0x64u,
		.keyboard_queue_capacity = ARRAY_SIZE(keyboard_queue),
		.auxiliary_queue_capacity = ARRAY_SIZE(auxiliary_queue),
		.write_poll_limit = 3u,
		.keyboard_id = {1u, 1u, 0u, 0u},
		.auxiliary_id = {1u, 2u, 0u, 0u},
		.present = 1u,
		.keyboard_present = 1u,
		.auxiliary_present = 1u,
		.presence_evidence =
			X86_NATIVE_INPUT_EVIDENCE_PLATFORM_ASSIGNED,
		.caller_serializes_irq = 1u,
		.writes_supported = 1u,
		.status_allowed_mask = 0xe3u,
		.status_output_full_mask = 0x01u,
		.status_input_full_mask = 0x02u,
		.status_auxiliary_mask = 0x20u,
		.status_parity_mask = 0x80u,
		.status_timeout_mask = 0x40u,
		.status_frame_mask = 0u,
		.reserved = {0u},
	};

	return config;
}

static int test_construct_and_registry(void)
{
	struct serio_registry unconstructed = {0};
	struct serio_port unconstructed_port = {0};
	struct serio_driver unconstructed_driver = {0};
	struct x86_native_input_controller unconstructed_controller = {0};
	struct x86_native_input_snapshot native_snapshot;
	struct serio_port_config port_config = {0};
	struct serio_driver_config driver_config = {0};

	if (serio_registry_initialize(&unconstructed, REGISTRY_ID, small_ports,
				      ARRAY_SIZE(small_ports), small_drivers,
				      ARRAY_SIZE(small_drivers)) !=
	    SERIO_INVALID_ARGUMENT)
		return 1;
	if (x86_native_input_snapshot(&unconstructed_controller, CONTROLLER_ID,
				      &native_snapshot) !=
	    X86_NATIVE_INPUT_INVALID_ARGUMENT)
		return 2;
	serio_registry_construct(&registry);
	if (serio_registry_initialize(&registry, REGISTRY_ID, small_ports,
				      ARRAY_SIZE(small_ports), small_drivers,
				      ARRAY_SIZE(small_drivers)) != SERIO_OK ||
	    registry.generation != 1u ||
	    serio_registry_replace_storage(&registry, REGISTRY_ID, ports,
					   ARRAY_SIZE(ports), drivers,
					   ARRAY_SIZE(drivers)) != SERIO_OK)
		return 3;
	port_config.identity = ROOT_ID;
	port_config.parent_identity = KERNEL_OBJECT_HANDLE_INVALID;
	port_config.caller_serializes_irq = 1u;
	port_config.queue = keyboard_queue;
	port_config.queue_capacity = ARRAY_SIZE(keyboard_queue);
	if (serio_port_prepare(&registry, &unconstructed_port, &port_config) !=
	    SERIO_INVALID_ARGUMENT)
		return 4;
	driver_config.identity = DRIVER_ID;
	driver_config.matches = &port_config.device_id;
	driver_config.match_count = 1u;
	driver_config.connect = connect_driver;
	driver_config.disconnect = disconnect_driver;
	driver_config.interrupt = receive_byte;
	return serio_driver_register(&registry, &unconstructed_driver,
				     &driver_config) == SERIO_INVALID_ARGUMENT
		       ? 0
		       : 5;
}

static int test_native_write_commit_boundaries(void)
{
	struct serio_write_result result;

	/* Status-read failure and bounded IBF backpressure happen before the
	 * keyboard data port is touched, so the caller may safely retry. */
	reset_write_fixture(0u);
	read_fault_call = 1u;
	result = serio_write(&controller.keyboard_port, 0xe1u);
	if (!write_result_is(result, SERIO_INVALID_STATE,
			     SERIO_WRITE_ZERO_COMMIT) ||
	    read_count != 1u || write_count != 0u)
		return 1;
	reset_write_fixture(0x02u);
	result = serio_write(&controller.keyboard_port, 0xe2u);
	if (!write_result_is(result, SERIO_RETRY, SERIO_WRITE_ZERO_COMMIT) ||
	    read_count != 3u || write_count != 0u)
		return 2;

	/* Once the data callback is attempted, its error cannot prove whether the
	 * port observed the byte.  Retrying would risk sending it twice. */
	reset_write_fixture(0u);
	write_fault_call = 1u;
	result = serio_write(&controller.keyboard_port, 0xe3u);
	if (!write_result_is(result, SERIO_INVALID_STATE,
			     SERIO_WRITE_UNCERTAIN) ||
	    read_count != 1u || write_count != 1u || write_ports[0] != 0x60u ||
	    write_values[0] != 0xe3u)
		return 3;
	reset_write_fixture(0u);
	result = serio_write(&controller.keyboard_port, 0xe4u);
	if (!write_result_is(result, SERIO_OK, SERIO_WRITE_COMMITTED) ||
	    read_count != 1u || write_count != 1u || write_ports[0] != 0x60u ||
	    write_values[0] != 0xe4u)
		return 4;

	/* AUX has a two-stage transaction.  A failure before 0xd4 is retryable;
	 * after the 0xd4 callback is attempted, every exit is uncertain until the
	 * data byte has definitely committed. */
	reset_write_fixture(0u);
	read_fault_call = 1u;
	result = serio_write(&controller.auxiliary_port, 0xf1u);
	if (!write_result_is(result, SERIO_INVALID_STATE,
			     SERIO_WRITE_ZERO_COMMIT) ||
	    read_count != 1u || write_count != 0u)
		return 5;
	reset_write_fixture(0u);
	write_fault_call = 1u;
	result = serio_write(&controller.auxiliary_port, 0xf2u);
	if (!write_result_is(result, SERIO_INVALID_STATE,
			     SERIO_WRITE_UNCERTAIN) ||
	    read_count != 1u || write_count != 1u || write_ports[0] != 0x64u ||
	    write_values[0] != 0xd4u)
		return 6;
	reset_write_fixture(0u);
	read_fault_call = 2u;
	result = serio_write(&controller.auxiliary_port, 0xf3u);
	if (!write_result_is(result, SERIO_INVALID_STATE,
			     SERIO_WRITE_UNCERTAIN) ||
	    read_count != 2u || write_count != 1u || write_ports[0] != 0x64u ||
	    write_values[0] != 0xd4u)
		return 7;
	reset_write_fixture(0u);
	write_fault_call = 2u;
	result = serio_write(&controller.auxiliary_port, 0xf4u);
	if (!write_result_is(result, SERIO_INVALID_STATE,
			     SERIO_WRITE_UNCERTAIN) ||
	    read_count != 2u || write_count != 2u || write_ports[0] != 0x64u ||
	    write_values[0] != 0xd4u || write_ports[1] != 0x60u ||
	    write_values[1] != 0xf4u)
		return 8;
	reset_write_fixture(0u);
	result = serio_write(&controller.auxiliary_port, 0xf5u);
	if (!write_result_is(result, SERIO_OK, SERIO_WRITE_COMMITTED) ||
	    read_count != 2u || write_count != 2u || write_ports[0] != 0x64u ||
	    write_values[0] != 0xd4u || write_ports[1] != 0x60u ||
	    write_values[1] != 0xf5u)
		return 9;
	reset_write_fixture(0u);
	return 0;
}

static int test_native_delivery(void)
{
	const struct serio_device_id match = {1u, SERIO_MATCH_ANY, 0u, 0u};
	const struct serio_driver_config driver_config = {
		.identity = DRIVER_ID,
		.matches = &match,
		.match_count = 1u,
		.manual_bind = 0u,
		.reserved = {0u},
		.connect = connect_driver,
		.disconnect = disconnect_driver,
		.reconnect = reconnect_driver,
		.interrupt = receive_byte,
	};
	struct x86_native_input_config config = native_config();
	struct x86_native_input_snapshot native_snapshot;
	struct serio_port_snapshot snapshot;
	struct serio_raw_event queued_head;
	struct serio_raw_event queued_tail;
	uint64_t last_sequence;
	uint64_t overflow_before;
	uint8_t receive_before;
	uint16_t queued_tail_index;
	uint16_t delivered = 0u;
	uint16_t keyboard_slot;
	int write_boundary_status;

	serio_driver_construct(&driver);
	if (serio_driver_register(&registry, &driver, &driver_config) != SERIO_OK)
		return 1;
	x86_native_input_construct(&controller);
	if (x86_native_input_prepare(&controller, &config) !=
		    X86_NATIVE_INPUT_OK ||
	    x86_native_input_publish(&controller, CONTROLLER_ID) !=
		    X86_NATIVE_INPUT_OK ||
	    controller.keyboard_port.driver != &driver ||
	    controller.auxiliary_port.driver != &driver)
		return 2;
	/* Transient reconnect backpressure retains the generation-bound driver and
	 * its recovery cursor; a later process call completes the same binding. */
	reconnect_retry_once = 1u;
	if (serio_port_reconnect(&controller.keyboard_port) != SERIO_RETRY ||
	    controller.keyboard_port.driver != &driver ||
	    serio_port_reconnect(&controller.keyboard_port) != SERIO_OK ||
	    controller.keyboard_port.driver != &driver)
		return 3;
	reconnect_count = 0u;
	/* Registry arrays cannot move while published IRQ targets are active. */
	if (serio_registry_replace_storage(&registry, REGISTRY_ID, larger_ports,
					   ARRAY_SIZE(larger_ports),
					   larger_drivers,
					   ARRAY_SIZE(larger_drivers)) != SERIO_RETRY)
		return 4;

	read_count = 0u;
	read_status = 0u;
	if (x86_native_input_irq(&controller, CONTROLLER_ID) !=
		    X86_NATIVE_INPUT_IRQ_NONE ||
	    read_count != 1u || read_ports[0] != 0x64u)
		return 4;
	read_count = 0u;
	read_status = 0x01u;
	read_data = 0x10u;
	if (x86_native_input_irq(&controller, CONTROLLER_ID) !=
		    X86_NATIVE_INPUT_IRQ_HANDLED ||
	    read_count != 2u || read_ports[0] != 0x64u ||
	    read_ports[1] != 0x60u || received_count != 1u ||
	    received[0].port_identity != KBD_PORT_ID ||
	    received[0].driver_identity != DRIVER_ID)
		return 5;
	read_count = 0u;
	read_status = 0xe9u;
	read_data = 0x30u;
	if (x86_native_input_irq(&controller, CONTROLLER_ID) !=
		    X86_NATIVE_INPUT_IRQ_HANDLED ||
	    received_count != 2u || received[1].port_identity != AUX_PORT_ID ||
	    received[1].raw_status != 0xe9u ||
	    received[1].flags !=
		    (SERIO_RAW_PARITY_ERROR | SERIO_RAW_TIMEOUT_ERROR |
		     SERIO_RAW_STATUS_UNRECOGNIZED))
		return 6;
	/* Process-context writes are bounded; replies remain ordinary raw input. */
	read_status = 0u;
	read_count = 0u;
	write_count = 0u;
	if (!write_result_is(serio_write(&controller.keyboard_port, 0xedu),
			     SERIO_OK, SERIO_WRITE_COMMITTED) ||
	    write_count != 1u || write_ports[0] != 0x60u ||
	    write_values[0] != 0xedu ||
	    !write_result_is(serio_write(&controller.auxiliary_port, 0xf4u),
			     SERIO_OK, SERIO_WRITE_COMMITTED) ||
	    write_count != 3u || write_ports[1] != 0x64u ||
	    write_values[1] != 0xd4u || write_ports[2] != 0x60u ||
	    write_values[2] != 0xf4u)
		return 7;
	write_boundary_status = test_native_write_commit_boundaries();
	if (write_boundary_status != 0)
		return 40 + write_boundary_status;
	read_status = 0x01u;
	read_data = 0xfau;
	if (x86_native_input_capture(&controller, CONTROLLER_ID) !=
		    X86_NATIVE_INPUT_OK)
		return 8;
	read_data = 0xfeu;
	if (x86_native_input_capture(&controller, CONTROLLER_ID) !=
		    X86_NATIVE_INPUT_OK ||
	    received[2].data != 0xfau || received[3].data != 0xfeu)
		return 9;
	read_status = 0x02u;
	read_count = 0u;
	write_count = 0u;
	if (!write_result_is(serio_write(&controller.keyboard_port, 0xf4u),
			     SERIO_RETRY, SERIO_WRITE_ZERO_COMMIT) ||
	    read_count != 3u || write_count != 0u)
		return 10;

	/* A newer handleable byte must queue behind a deferred head without
	 * reaching the driver first. */
	defer_enabled = 1u;
	read_status = 0x01u;
	read_data = 0x20u;
	read_count = 0u;
	if (x86_native_input_capture(&controller, CONTROLLER_ID) !=
		    X86_NATIVE_INPUT_RETRY || received_count != 5u ||
	    controller.keyboard_port.queue_count != 1u)
		return 11;
	defer_enabled = 0u;
	read_data = 0x21u;
	if (x86_native_input_capture(&controller, CONTROLLER_ID) !=
		    X86_NATIVE_INPUT_RETRY || received_count != 5u ||
	    controller.keyboard_port.queue_count != 2u ||
	    keyboard_queue[controller.keyboard_port.queue_head].data != 0x20u ||
	    keyboard_queue[(controller.keyboard_port.queue_head + 1u) %
			   ARRAY_SIZE(keyboard_queue)].data != 0x21u)
		return 12;
	delivered = 0u;
	if (serio_port_pump(&controller.keyboard_port, 1u, &delivered) != SERIO_OK ||
	    delivered != 1u || received_count != 6u || received[5].data != 0x20u ||
	    controller.keyboard_port.queue_count != 1u)
		return 13;
	delivered = 0u;
	if (serio_port_pump(&controller.keyboard_port, 1u, &delivered) != SERIO_OK ||
	    delivered != 1u || received_count != 7u || received[6].data != 0x21u ||
	    controller.keyboard_port.queue_count != 0u)
		return 14;

	/* Rebinding makes the old head stale.  A current-generation byte appends
	 * behind it; pump discards only the stale head and then delivers the next. */
	defer_enabled = 1u;
	read_count = 0u;
	read_data = 0x20u;
	if (x86_native_input_capture(&controller, CONTROLLER_ID) !=
		    X86_NATIVE_INPUT_RETRY || received_count != 8u ||
	    controller.keyboard_port.queue_count != 1u)
		return 15;
	if (serio_port_unbind(&controller.keyboard_port) != SERIO_OK ||
	    controller.keyboard_port.binding_generation != 2u)
		return 16;
	defer_enabled = 0u;
	receive_before = received_count;
	read_data = 0x22u;
	if (x86_native_input_capture(&controller, CONTROLLER_ID) !=
		    X86_NATIVE_INPUT_RETRY || received_count != receive_before ||
	    controller.keyboard_port.queue_count != 2u)
		return 17;
	delivered = 0u;
	if (serio_port_pump(&controller.keyboard_port, 1u, &delivered) != SERIO_OK ||
	    delivered != 1u || received_count != (uint8_t)(receive_before + 1u) ||
	    received[receive_before].data != 0x22u ||
	    serio_port_snapshot(&controller.keyboard_port, &snapshot) != SERIO_OK ||
	    snapshot.queue_count != 0u || snapshot.stale_binding_drop_count != 1u)
		return 18;

	/* Once 60h is consumed, exhaustion is a discontinuity, not an ordinary
	 * retry. The exact queued prefix remains observable until explicit
	 * process-context recovery discards it and resets the driver. */
	defer_enabled = 1u;
	read_count = 0u;
	read_data = 0x20u;
	if (x86_native_input_capture(&controller, CONTROLLER_ID) !=
		    X86_NATIVE_INPUT_RETRY)
		return 19;
	defer_enabled = 0u;
	read_data = 0x23u;
	if (x86_native_input_capture(&controller, CONTROLLER_ID) !=
		    X86_NATIVE_INPUT_RETRY ||
	    controller.keyboard_port.queue_count != ARRAY_SIZE(keyboard_queue))
		return 20;
	queued_tail_index = (uint16_t)((controller.keyboard_port.queue_head + 1u) %
				       ARRAY_SIZE(keyboard_queue));
	queued_head = keyboard_queue[controller.keyboard_port.queue_head];
	queued_tail = keyboard_queue[queued_tail_index];
	receive_before = received_count;
	last_sequence = received[receive_before - 1u].sequence;
	overflow_before = controller.keyboard_port.overflow_count;
	read_count = 0u;
	read_data = 0x24u;
	if (x86_native_input_capture(&controller, CONTROLLER_ID) !=
		    X86_NATIVE_INPUT_SOURCE_BACKPRESSURE ||
	    read_count != 1u || read_ports[0] != 0x64u ||
	    controller.keyboard_port.overflow_count != overflow_before ||
	    serio_interrupt(&controller.keyboard_port, 0x24u, 0x01u, 0u) !=
		    SERIO_STREAM_LOST ||
	    received_count != receive_before ||
	    received[receive_before - 1u].sequence != last_sequence ||
	    controller.keyboard_port.queue_count != ARRAY_SIZE(keyboard_queue) ||
	    controller.keyboard_port.overflow_count != overflow_before + 1u ||
	    keyboard_queue[controller.keyboard_port.queue_head].sequence !=
		    queued_head.sequence ||
	    keyboard_queue[controller.keyboard_port.queue_head].data !=
		    queued_head.data ||
	    keyboard_queue[queued_tail_index].sequence != queued_tail.sequence ||
	    keyboard_queue[queued_tail_index].data != queued_tail.data)
		return 21;
	delivered = 0u;
	if (serio_port_pump(&controller.keyboard_port, 1u, &delivered) !=
		    SERIO_STREAM_LOST ||
	    delivered != 0u ||
	    serio_port_snapshot(&controller.keyboard_port, &snapshot) != SERIO_OK ||
	    snapshot.stream_loss_epoch != 1u ||
	    snapshot.stream_recovery_epoch != 0u ||
	    snapshot.lost_byte_count != 1u || snapshot.recovery_required != 1u ||
	    snapshot.stream_isolated != 1u || snapshot.recovery_abandoned != 0u ||
	    snapshot.accepting != 0u)
		return 22;
	/* IRQ completion remains HANDLED, but a pending byte is left in 60h: only
	 * status is sampled until the exact loss epoch is recovered. */
	read_count = 0u;
	read_data = 0x25u;
	if (x86_native_input_irq(&controller, CONTROLLER_ID) !=
		    X86_NATIVE_INPUT_IRQ_HANDLED ||
	    read_count != 1u || read_ports[0] != 0x64u ||
	    received_count != receive_before ||
	    serio_port_recover_stream(&controller.keyboard_port, 2u) !=
		    SERIO_STALE_EVENT ||
	    x86_native_input_recover_stream(
		    &controller, CONTROLLER_ID, X86_NATIVE_INPUT_KEYBOARD, 1u) !=
		    X86_NATIVE_INPUT_OK ||
	    reconnect_count != 1u || controller.keyboard_port.queue_count != 0u ||
	    serio_port_snapshot(&controller.keyboard_port, &snapshot) != SERIO_OK ||
	    snapshot.stream_recovery_epoch != 1u ||
	    snapshot.recovery_discard_count != 2u ||
	    snapshot.recovery_required != 0u || snapshot.stream_isolated != 0u ||
	    snapshot.accepting != 1u ||
	    x86_native_input_snapshot(&controller, CONTROLLER_ID,
				      &native_snapshot) != X86_NATIVE_INPUT_OK ||
	    native_snapshot.stream_loss_count != 1u ||
	    native_snapshot.stream_recovery_count != 1u ||
	    native_snapshot.keyboard_stream_loss_epoch != 1u ||
	    native_snapshot.keyboard_stream_recovery_epoch != 1u)
		return 23;
	/* The same hardware byte can now be consumed once against reset state. */
	read_count = 0u;
	if (x86_native_input_capture(&controller, CONTROLLER_ID) !=
		    X86_NATIVE_INPUT_OK ||
	    read_count != 2u || received_count != (uint8_t)(receive_before + 1u) ||
	    received[receive_before].data != 0x25u)
		return 24;
	/* A bounded owner may choose terminal per-stream isolation instead of
	 * pretending recovery succeeded. IRQ acknowledgement still remains
	 * HANDLED and later bytes stay in hardware. */
	defer_enabled = 1u;
	read_data = 0x20u;
	if (x86_native_input_capture(&controller, CONTROLLER_ID) !=
		    X86_NATIVE_INPUT_RETRY)
		return 25;
	defer_enabled = 0u;
	read_data = 0x26u;
	if (x86_native_input_capture(&controller, CONTROLLER_ID) !=
		    X86_NATIVE_INPUT_RETRY)
		return 26;
	read_data = 0x27u;
	if (x86_native_input_capture(&controller, CONTROLLER_ID) !=
		    X86_NATIVE_INPUT_SOURCE_BACKPRESSURE ||
	    serio_interrupt(&controller.keyboard_port, 0x27u, 0x01u, 0u) !=
		    SERIO_STREAM_LOST ||
	    x86_native_input_recover_stream(
		    &controller, CONTROLLER_ID, X86_NATIVE_INPUT_KEYBOARD, 0u) !=
		    X86_NATIVE_INPUT_INVALID_ARGUMENT ||
	    x86_native_input_isolate_stream(
		    &controller, CONTROLLER_ID, X86_NATIVE_INPUT_KEYBOARD, 0u) !=
		    X86_NATIVE_INPUT_INVALID_ARGUMENT)
		return 27;
	if (x86_native_input_isolate_stream(
		    &controller, CONTROLLER_ID, X86_NATIVE_INPUT_KEYBOARD, 2u) !=
		    X86_NATIVE_INPUT_OK)
		return 28;
	read_count = 0u;
	if (x86_native_input_capture(&controller, CONTROLLER_ID) !=
		    X86_NATIVE_INPUT_STREAM_ISOLATED)
		return 29;
	if (x86_native_input_snapshot(&controller, CONTROLLER_ID,
				      &native_snapshot) != X86_NATIVE_INPUT_OK)
		return 30;
	if (native_snapshot.stream_loss_count != 2u)
		return 31;
	if (native_snapshot.stream_isolation_count != 1u)
		return 32;
	if (native_snapshot.keyboard_recovery_required != 1u ||
	    native_snapshot.keyboard_stream_isolated != 1u ||
	    native_snapshot.keyboard_recovery_abandoned != 1u)
		return 33;
	if (x86_native_input_quiesce(&controller, CONTROLLER_ID) !=
		    X86_NATIVE_INPUT_OK ||
	    x86_native_input_retire(&controller, CONTROLLER_ID) !=
		    X86_NATIVE_INPUT_OK ||
	    controller.generation != 1u ||
	    controller.keyboard_port.generation != 1u)
		return 34;
	/* A second-endpoint publish failure rolls back the first endpoint. */
	if (x86_native_input_prepare(&controller, &config) !=
		    X86_NATIVE_INPUT_OK)
		return 35;
	controller.auxiliary_port.config.start = fail_start;
	if (x86_native_input_publish(&controller, CONTROLLER_ID) !=
		    X86_NATIVE_INPUT_INVALID_STATE ||
	    registry.port_count != 0u)
		return 36;
	/* Retire validates both endpoints before mutating either one. */
	if (x86_native_input_prepare(&controller, &config) !=
		    X86_NATIVE_INPUT_OK ||
	    x86_native_input_publish(&controller, CONTROLLER_ID) !=
		    X86_NATIVE_INPUT_OK ||
	    x86_native_input_quiesce(&controller, CONTROLLER_ID) !=
		    X86_NATIVE_INPUT_OK)
		return 37;
	keyboard_slot = controller.keyboard_port.registry_slot;
	registry.ports[keyboard_slot] = NULL;
	if (x86_native_input_retire(&controller, CONTROLLER_ID) !=
		    X86_NATIVE_INPUT_RETRY ||
	    registry.port_count != 2u ||
	    controller.auxiliary_port.phase != SERIO_PORT_QUIESCED)
		return 38;
	registry.ports[keyboard_slot] = &controller.keyboard_port;
	if (x86_native_input_retire(&controller, CONTROLLER_ID) !=
		    X86_NATIVE_INPUT_OK ||
	    controller.generation != 3u || registry.port_count != 0u)
		return 39;
	return 0;
}

static void fill_port_config(struct serio_port_config *config,
	kernel_object_handle_t identity, kernel_object_handle_t parent,
	struct serio_raw_event *queue)
{
	config->identity = identity;
	config->parent_identity = parent;
	config->device_id.type = 0u;
	config->device_id.protocol = 0u;
	config->device_id.id = 0u;
	config->device_id.extra = 0u;
	config->manual_bind = 1u;
	config->caller_serializes_irq = 1u;
	config->reserved[0] = 0u;
	config->reserved[1] = 0u;
	config->callback_context = identity;
	config->port_context = NULL;
	config->irq_enter = NULL;
	config->irq_exit = NULL;
	config->start = NULL;
	config->stop = stop_port;
	config->open = open_port;
	config->close = NULL;
	config->write = NULL;
	config->queue = queue;
	config->queue_capacity = 1u;
	config->reserved_capacity = 0u;
}

static int test_zero_match_and_subtree(void)
{
	static struct serio_port root;
	static struct serio_port child;
	static struct serio_port leaf;
	static struct serio_raw_event root_queue[1];
	static struct serio_raw_event child_queue[1];
	static struct serio_raw_event leaf_queue[1];
	struct serio_port_config root_config;
	struct serio_port_config child_config;
	struct serio_port_config leaf_config;
	const struct serio_device_id zero_match = {0u, 0u, 0u, 0u};
	const struct serio_driver_config zero_driver_config = {
		.identity = DRIVER_ID,
		.matches = &zero_match,
		.match_count = 1u,
		.manual_bind = 1u,
		.reserved = {0u},
		.connect = connect_driver,
		.disconnect = disconnect_driver,
		.reconnect = NULL,
		.interrupt = receive_byte,
	};

	/* Re-registering the same constructed driver advances its generation. */
	if (serio_driver_unregister(&driver) != SERIO_OK ||
	    serio_driver_register(&registry, &driver, &zero_driver_config) !=
		    SERIO_OK ||
	    driver.generation != 2u)
		return 1;
	serio_port_construct(&root);
	serio_port_construct(&child);
	serio_port_construct(&leaf);
	fill_port_config(&root_config, ROOT_ID, KERNEL_OBJECT_HANDLE_INVALID,
			 root_queue);
	fill_port_config(&child_config, CHILD_ID, ROOT_ID, child_queue);
	fill_port_config(&leaf_config, LEAF_ID, CHILD_ID, leaf_queue);
	if (serio_port_prepare(&registry, &root, &root_config) != SERIO_OK ||
	    serio_port_publish(&root) != SERIO_OK ||
	    serio_port_prepare(&registry, &child, &child_config) != SERIO_OK ||
	    serio_port_publish(&child) != SERIO_OK ||
	    serio_port_prepare(&registry, &leaf, &leaf_config) != SERIO_OK ||
	    serio_port_publish(&leaf) != SERIO_OK ||
	    child.parent_generation != root.generation ||
	    leaf.parent_generation != child.generation ||
	    serio_port_bind(&root, DRIVER_ID) != SERIO_OK ||
	    root.driver != &driver)
		return 2;
	if (!write_result_is(serio_write(&root, 0x55u), SERIO_UNAVAILABLE,
			     SERIO_WRITE_ZERO_COMMIT))
		return 3;
	if (serio_port_quiesce(&root) != SERIO_PARENT_BUSY)
		return 4;
	leaf.in_flight = 1u;
	if (serio_port_unregister_subtree(&root) != SERIO_RETRY ||
	    root.accepting != 1u || child.accepting != 1u ||
	    leaf.accepting != 1u)
		return 5;
	leaf.in_flight = 0u;
	stop_count = 0u;
	if (serio_port_unregister_subtree(&root) != SERIO_OK ||
	    stop_count != 3u || stop_order[0] != LEAF_ID ||
	    stop_order[1] != CHILD_ID || stop_order[2] != ROOT_ID ||
	    registry.port_count != 0u || root.generation != 1u ||
	    child.generation != 1u || leaf.generation != 1u)
		return 6;
	return serio_driver_unregister(&driver) == SERIO_OK ? 0 : 7;
}

static int run_tests(void)
{
	int status;

	status = test_construct_and_registry();
	if (status != 0)
		return 10 + status;
	status = test_native_delivery();
	if (status != 0)
		return 30 + status;
	status = test_zero_match_and_subtree();
	if (status != 0)
		return 60 + status;
	return 0;
}

DOSC32_TEST_ENTRY(run_tests)

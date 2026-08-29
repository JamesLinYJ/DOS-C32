// SPDX-License-Identifier: GPL-2.0-only
/* Host-safe AT keyboard protocol, command, lifetime and backpressure tests. */
#include "atkbd.h"
#include "input_keycodes.h"
#include "test_entry.h"

#define SERIO_REGISTRY_ID ((kernel_object_handle_t)0x4154534552494f52ull)
#define INPUT_CORE_ID ((kernel_object_handle_t)0x4154494e50555443ull)
#define DRIVER_ID ((kernel_object_handle_t)0x41544b4244445256ull)
#define TRANSLATED_PORT_ID ((kernel_object_handle_t)0x41544b42584c4154ull)
#define RAW_PORT_ID ((kernel_object_handle_t)0x41544b4252415732ull)
#define TRANSLATED_DEVICE_ID ((kernel_object_handle_t)0x4154494e5054584cull)
#define RAW_DEVICE_ID ((kernel_object_handle_t)0x4154494e50545257ull)
#define HANDLER_ID ((kernel_object_handle_t)0x4154494e48414e44ull)
#define HANDLER_CONTEXT ((kernel_object_handle_t)0x4154434f4e544558ull)

static struct serio_registry registry;
static struct serio_port translated_port;
static struct serio_port raw_port;
static struct serio_port *serio_ports[4];
static struct serio_driver *serio_drivers[2];
static struct serio_raw_event translated_serio_queue[8];
static struct serio_raw_event raw_serio_queue[8];

static struct input_core input_core;
static struct input_device *input_devices[4];
static struct input_handler *input_handlers[2];
static struct input_handler handler;
static struct input_handler_binding handler_binding;
static struct input_event translated_input_queue[8];
static struct input_event raw_input_queue[8];

static struct atkbd_driver keyboard_driver;
static struct atkbd_endpoint endpoints[2];
static struct atkbd_endpoint_reference translated_reference;
static struct atkbd_endpoint_reference raw_reference;

static const struct serio_device_id keyboard_match = {
	.type = 1u,
	.protocol = 1u,
	.id = SERIO_MATCH_ANY,
	.extra = SERIO_MATCH_ANY,
};
static const struct atkbd_endpoint_config keyboard_endpoint_configs[] = {
	{
		.port_identity = TRANSLATED_PORT_ID,
		.input_device_identity = TRANSLATED_DEVICE_ID,
		.input_queue = translated_input_queue,
		.input_queue_capacity = ARRAY_SIZE(translated_input_queue),
		.scan_mode = ATKBD_SCAN_TRANSLATED_SET1,
		.start_enabled = 1u,
		.reserved = {0u},
	},
	{
		.port_identity = RAW_PORT_ID,
		.input_device_identity = RAW_DEVICE_ID,
		.input_queue = raw_input_queue,
		.input_queue_capacity = ARRAY_SIZE(raw_input_queue),
		.scan_mode = ATKBD_SCAN_RAW_SET2,
		.start_enabled = 1u,
		.reserved = {0u},
	},
};

static struct input_event received[96];
static uint16_t received_count;
static uint8_t defer_next;
static uint8_t defer_all;
static uint8_t modifier_mask;
static uint8_t focus_enter_count;
static uint8_t focus_leave_count;
static uint8_t open_count;
static uint8_t close_count;
static uint8_t writes[64];
static uint8_t write_count;
static uint8_t write_callback_count;
static enum serio_status synchronous_interrupt_status;

enum fake_write_action {
	FAKE_WRITE_COMMIT = 0,
	FAKE_WRITE_ZERO_RETRY,
	FAKE_WRITE_ZERO_ERROR,
	FAKE_WRITE_UNCERTAIN,
	FAKE_WRITE_COMMIT_SYNCHRONOUS_ACK
};

static enum fake_write_action next_write_action;

static enum input_focus_result focus_enter(
	struct input_handler *input_handler, kernel_object_handle_t context,
	kernel_object_handle_t core_identity, uint64_t focus_generation)
{
	if (input_handler_context(input_handler) != &handler ||
	    context != HANDLER_CONTEXT || core_identity != INPUT_CORE_ID ||
	    focus_generation == 0u)
		return INPUT_FOCUS_BROKEN;
	focus_enter_count++;
	return INPUT_FOCUS_OK;
}

static void focus_leave(struct input_handler *input_handler,
	kernel_object_handle_t context, kernel_object_handle_t core_identity,
	uint64_t focus_generation)
{
	if (input_handler_context(input_handler) == &handler &&
	    context == HANDLER_CONTEXT && core_identity == INPUT_CORE_ID &&
	    focus_generation != 0u)
		focus_leave_count++;
}

static enum input_handler_result receive_event(
	struct input_handler *input_handler, kernel_object_handle_t context,
	const struct input_event *event)
{
	if (input_handler_context(input_handler) != &handler ||
	    context != HANDLER_CONTEXT || event == NULL)
		return INPUT_HANDLER_BROKEN;
	if (defer_all != 0u || defer_next != 0u) {
		defer_next = 0u;
		return INPUT_HANDLER_DEFER;
	}
	if (received_count >= ARRAY_SIZE(received))
		return INPUT_HANDLER_BROKEN;
	received[received_count++] = *event;
	{
		uint8_t mask = 0u;

		switch (event->code) {
		case INPUT_KEY_CODE_LEFTSHIFT:
			mask = 1u << 0;
			break;
		case INPUT_KEY_CODE_RIGHTSHIFT:
			mask = 1u << 1;
			break;
		case INPUT_KEY_CODE_LEFTCTRL:
			mask = 1u << 2;
			break;
		case INPUT_KEY_CODE_RIGHTCTRL:
			mask = 1u << 3;
			break;
		case INPUT_KEY_CODE_LEFTALT:
			mask = 1u << 4;
			break;
		case INPUT_KEY_CODE_RIGHTALT:
			mask = 1u << 5;
			break;
		default:
			break;
		}
		if (event->value == INPUT_KEY_RELEASED)
			modifier_mask &= (uint8_t)~mask;
		else
			modifier_mask |= mask;
	}
	return INPUT_HANDLER_HANDLED;
}

static enum serio_status open_port(struct serio_port *port,
	struct serio_driver *driver, void *binding_context)
{
	if (port == NULL || driver == NULL || binding_context == NULL ||
	    port->driver != driver)
		return SERIO_INVALID_STATE;
	open_count++;
	return SERIO_OK;
}

static void close_port(struct serio_port *port,
	struct serio_driver *driver, void *binding_context)
{
	if (port != NULL && driver != NULL && binding_context != NULL)
		close_count++;
}

static struct serio_write_result write_port(struct serio_port *port,
					    uint8_t data)
{
	enum fake_write_action action = next_write_action;

	next_write_action = FAKE_WRITE_COMMIT;
	write_callback_count++;
	if (port == NULL)
		return (struct serio_write_result){
			.status = SERIO_INVALID_ARGUMENT,
			.commit = SERIO_WRITE_ZERO_COMMIT,
		};
	if (action == FAKE_WRITE_ZERO_RETRY)
		return (struct serio_write_result){
			.status = SERIO_RETRY,
			.commit = SERIO_WRITE_ZERO_COMMIT,
		};
	if (action == FAKE_WRITE_ZERO_ERROR)
		return (struct serio_write_result){
			.status = SERIO_INVALID_STATE,
			.commit = SERIO_WRITE_ZERO_COMMIT,
		};
	if (action == FAKE_WRITE_UNCERTAIN)
		return (struct serio_write_result){
			.status = SERIO_INVALID_STATE,
			.commit = SERIO_WRITE_UNCERTAIN,
		};
	if (write_count >= ARRAY_SIZE(writes))
		return (struct serio_write_result){
			.status = SERIO_CAPACITY_EXHAUSTED,
			.commit = SERIO_WRITE_ZERO_COMMIT,
		};
	writes[write_count++] = data;
	if (action == FAKE_WRITE_COMMIT_SYNCHRONOUS_ACK)
		synchronous_interrupt_status =
			serio_interrupt(port, 0xfau, 0u, 0u);
	return (struct serio_write_result){
		.status = SERIO_OK,
		.commit = SERIO_WRITE_COMMITTED,
	};
}

static int setup_serio_port(struct serio_port *port,
	kernel_object_handle_t identity, struct serio_raw_event *queue,
	uint16_t queue_capacity, bool construct)
{
	const struct serio_port_config config = {
		.identity = identity,
		.parent_identity = KERNEL_OBJECT_HANDLE_INVALID,
		.device_id = {1u, 1u, 0u, 0u},
		.manual_bind = 0u,
		.caller_serializes_irq = 1u,
		.reserved = {0u},
		.callback_context = KERNEL_OBJECT_HANDLE_INVALID,
		.port_context = NULL,
		.irq_enter = NULL,
		.irq_exit = NULL,
		.start = NULL,
		.stop = NULL,
		.open = open_port,
		.close = close_port,
		.write = write_port,
		.queue = queue,
		.queue_capacity = queue_capacity,
		.reserved_capacity = 0u,
	};

	if (construct)
		serio_port_construct(port);
	if (serio_port_prepare(&registry, port, &config) != SERIO_OK)
		return 1;
	return serio_port_publish(port) == SERIO_OK ? 0 : 2;
}

static int setup(void)
{
	const struct input_core_config core_config = {
		.identity = INPUT_CORE_ID,
		.guard_context = KERNEL_OBJECT_HANDLE_INVALID,
		.irq_enter = NULL,
		.irq_exit = NULL,
		.caller_serializes_irq = 1u,
		.reserved = {0u},
	};
	const struct input_handler_config handler_config = {
		.identity = HANDLER_ID,
		.context = HANDLER_CONTEXT,
		.handler_context = &handler,
		.capabilities = INPUT_CAPABILITY_KEY,
		.focus_enter = focus_enter,
		.focus_leave = focus_leave,
		.receive = receive_event,
		.reserved = {0u},
	};
	const struct atkbd_driver_config driver_config = {
		.identity = DRIVER_ID,
		.input_core_identity = INPUT_CORE_ID,
		.input_core = &input_core,
		.endpoints = endpoints,
		.endpoint_configs = keyboard_endpoint_configs,
		.endpoint_count = ARRAY_SIZE(keyboard_endpoint_configs),
		.command_write_limit = 24u,
		.command_nak_limit = 2u,
		.matches = &keyboard_match,
		.match_count = 1u,
		.reserved = {0u},
	};
	uint16_t slot;

	serio_registry_construct(&registry);
	if (serio_registry_initialize(&registry, SERIO_REGISTRY_ID,
				      serio_ports, ARRAY_SIZE(serio_ports),
				      serio_drivers,
				      ARRAY_SIZE(serio_drivers)) != SERIO_OK)
		return 1;
	if (setup_serio_port(&translated_port, TRANSLATED_PORT_ID,
			     translated_serio_queue,
			     ARRAY_SIZE(translated_serio_queue), true) != 0 ||
	    setup_serio_port(&raw_port, RAW_PORT_ID, raw_serio_queue,
			     ARRAY_SIZE(raw_serio_queue), true) != 0)
		return 2;
	input_core_construct(&input_core);
	if (input_core_initialize(&input_core, &core_config, input_devices,
				  ARRAY_SIZE(input_devices), input_handlers,
				  ARRAY_SIZE(input_handlers)) != INPUT_OK)
		return 3;
	input_handler_construct(&handler);
	if (input_handler_register(&input_core, &handler, &handler_config,
				   &handler_binding) != INPUT_OK)
		return 4;
	atkbd_driver_construct(&keyboard_driver);
	for (slot = 0u; slot < ARRAY_SIZE(endpoints); ++slot)
		atkbd_endpoint_construct(&endpoints[slot]);
	if (atkbd_driver_register(&keyboard_driver, &registry,
				  &driver_config) != ATKBD_OK ||
	    translated_port.driver != &keyboard_driver.serio_driver ||
	    raw_port.driver != &keyboard_driver.serio_driver || open_count != 2u)
		return 5;
	if (input_core_publish(&input_core, INPUT_CORE_ID) != INPUT_OK ||
	    input_focus_set(&input_core, &handler_binding) != INPUT_OK ||
	    atkbd_endpoint_reference(&keyboard_driver, DRIVER_ID, 0u,
				     &translated_reference) != ATKBD_OK ||
	    atkbd_endpoint_reference(&keyboard_driver, DRIVER_ID, 1u,
				     &raw_reference) != ATKBD_OK)
		return 6;
	return 0;
}

static bool event_is(uint16_t index, kernel_object_handle_t device,
	input_key_code_t code, uint8_t value, uint8_t flags)
{
	return index < received_count &&
	       received[index].device_identity == device &&
	       received[index].code == code && received[index].value == value &&
	       received[index].flags == flags;
}

static int send_bytes(struct serio_port *port, const uint8_t *bytes,
	size_t count)
{
	size_t index;

	for (index = 0u; index < count; ++index) {
		if (serio_interrupt(port, bytes[index], 0u, 0u) != SERIO_OK)
			return 1;
	}
	return 0;
}

static int test_translated_decode(void)
{
	static const uint8_t bytes[] = {
		0x1eu, 0x1eu, 0x9eu,
		0x2au, 0xaau,
		0xe0u, 0x1du, 0xe0u, 0x9du,
		0xe0u, 0x2au, 0xe0u, 0x37u,
		0xe0u, 0xb7u, 0xe0u, 0xaau,
		0xe1u, 0x1du, 0x45u, 0xe1u, 0x9du, 0xc5u,
	};
	uint16_t first = received_count;

	if (send_bytes(&translated_port, bytes, ARRAY_SIZE(bytes)) != 0)
		return 1;
	if (!event_is(first + 0u, TRANSLATED_DEVICE_ID, INPUT_KEY_CODE_A,
		      INPUT_KEY_PRESSED, 0u) ||
	    !event_is(first + 1u, TRANSLATED_DEVICE_ID, INPUT_KEY_CODE_A,
		      INPUT_KEY_REPEATED, 0u) ||
	    !event_is(first + 2u, TRANSLATED_DEVICE_ID, INPUT_KEY_CODE_A,
		      INPUT_KEY_RELEASED, 0u) ||
	    !event_is(first + 3u, TRANSLATED_DEVICE_ID,
		      INPUT_KEY_CODE_LEFTSHIFT, INPUT_KEY_PRESSED, 0u) ||
	    !event_is(first + 4u, TRANSLATED_DEVICE_ID,
		      INPUT_KEY_CODE_LEFTSHIFT, INPUT_KEY_RELEASED, 0u) ||
	    !event_is(first + 5u, TRANSLATED_DEVICE_ID,
		      INPUT_KEY_CODE_RIGHTCTRL, INPUT_KEY_PRESSED,
		      INPUT_EVENT_EXTENDED) ||
	    !event_is(first + 6u, TRANSLATED_DEVICE_ID,
		      INPUT_KEY_CODE_RIGHTCTRL, INPUT_KEY_RELEASED,
		      INPUT_EVENT_EXTENDED) ||
	    !event_is(first + 7u, TRANSLATED_DEVICE_ID, INPUT_KEY_CODE_SYSRQ,
		      INPUT_KEY_PRESSED, INPUT_EVENT_EXTENDED) ||
	    !event_is(first + 8u, TRANSLATED_DEVICE_ID, INPUT_KEY_CODE_SYSRQ,
		      INPUT_KEY_RELEASED, INPUT_EVENT_EXTENDED) ||
	    !event_is(first + 9u, TRANSLATED_DEVICE_ID, INPUT_KEY_CODE_PAUSE,
		      INPUT_KEY_PRESSED, INPUT_EVENT_EXTENDED))
		return 2;
	return endpoints[0].bat_count == 0u ? 0 : 3;
}

static int test_raw_decode(void)
{
	static const uint8_t bytes[] = {
		0x1cu, 0x1cu, 0xf0u, 0x1cu,
		0xe0u, 0x14u, 0xe0u, 0xf0u, 0x14u,
		0xe0u, 0x12u, 0xe0u, 0x7cu,
		0xe0u, 0xf0u, 0x7cu, 0xe0u, 0xf0u, 0x12u,
		0xe1u, 0x14u, 0x77u, 0xe1u, 0xf0u, 0x14u, 0xf0u, 0x77u,
		0x83u, 0xf0u, 0x83u,
	};
	uint16_t first = received_count;

	if (send_bytes(&raw_port, bytes, ARRAY_SIZE(bytes)) != 0)
		return 1;
	if (!event_is(first + 0u, RAW_DEVICE_ID, INPUT_KEY_CODE_A,
		      INPUT_KEY_PRESSED, 0u) ||
	    !event_is(first + 1u, RAW_DEVICE_ID, INPUT_KEY_CODE_A,
		      INPUT_KEY_REPEATED, 0u) ||
	    !event_is(first + 2u, RAW_DEVICE_ID, INPUT_KEY_CODE_A,
		      INPUT_KEY_RELEASED, 0u) ||
	    !event_is(first + 3u, RAW_DEVICE_ID, INPUT_KEY_CODE_RIGHTCTRL,
		      INPUT_KEY_PRESSED, INPUT_EVENT_EXTENDED) ||
	    !event_is(first + 4u, RAW_DEVICE_ID, INPUT_KEY_CODE_RIGHTCTRL,
		      INPUT_KEY_RELEASED, INPUT_EVENT_EXTENDED) ||
	    !event_is(first + 5u, RAW_DEVICE_ID, INPUT_KEY_CODE_SYSRQ,
		      INPUT_KEY_PRESSED, INPUT_EVENT_EXTENDED) ||
	    !event_is(first + 6u, RAW_DEVICE_ID, INPUT_KEY_CODE_SYSRQ,
		      INPUT_KEY_RELEASED, INPUT_EVENT_EXTENDED) ||
	    !event_is(first + 7u, RAW_DEVICE_ID, INPUT_KEY_CODE_PAUSE,
		      INPUT_KEY_PRESSED, INPUT_EVENT_EXTENDED) ||
	    !event_is(first + 8u, RAW_DEVICE_ID, INPUT_KEY_CODE_F7,
		      INPUT_KEY_PRESSED, 0u) ||
	    !event_is(first + 9u, RAW_DEVICE_ID, INPUT_KEY_CODE_F7,
		      INPUT_KEY_RELEASED, 0u))
		return 2;
	return 0;
}

static int test_named_map_edges(void)
{
	static const uint8_t translated_bytes[] = {
		0x73u, 0xf3u,
		0xe0u, 0x65u, 0xe0u, 0xe5u,
		0x5cu, 0xdcu,
	};
	static const uint8_t raw_bytes[] = {
		0x51u, 0xf0u, 0x51u,
		0xe0u, 0x10u, 0xe0u, 0xf0u, 0x10u,
		0x27u, 0xf0u, 0x27u,
	};
	uint16_t first = received_count;

	if (send_bytes(&translated_port, translated_bytes,
		       ARRAY_SIZE(translated_bytes)) != 0 ||
	    send_bytes(&raw_port, raw_bytes, ARRAY_SIZE(raw_bytes)) != 0)
		return 1;
	if (!event_is(first + 0u, TRANSLATED_DEVICE_ID, INPUT_KEY_CODE_RO,
		      INPUT_KEY_PRESSED, 0u) ||
	    !event_is(first + 1u, TRANSLATED_DEVICE_ID, INPUT_KEY_CODE_RO,
		      INPUT_KEY_RELEASED, 0u) ||
	    !event_is(first + 2u, TRANSLATED_DEVICE_ID,
		      INPUT_KEY_CODE_SEARCH, INPUT_KEY_PRESSED,
		      INPUT_EVENT_EXTENDED) ||
	    !event_is(first + 3u, TRANSLATED_DEVICE_ID,
		      INPUT_KEY_CODE_SEARCH, INPUT_KEY_RELEASED,
		      INPUT_EVENT_EXTENDED) ||
	    !event_is(first + 4u, TRANSLATED_DEVICE_ID,
		      INPUT_KEY_CODE_KPJPCOMMA, INPUT_KEY_PRESSED, 0u) ||
	    !event_is(first + 5u, TRANSLATED_DEVICE_ID,
		      INPUT_KEY_CODE_KPJPCOMMA, INPUT_KEY_RELEASED, 0u) ||
	    !event_is(first + 6u, RAW_DEVICE_ID, INPUT_KEY_CODE_RO,
		      INPUT_KEY_PRESSED, 0u) ||
	    !event_is(first + 7u, RAW_DEVICE_ID, INPUT_KEY_CODE_RO,
		      INPUT_KEY_RELEASED, 0u) ||
	    !event_is(first + 8u, RAW_DEVICE_ID, INPUT_KEY_CODE_SEARCH,
		      INPUT_KEY_PRESSED, INPUT_EVENT_EXTENDED) ||
	    !event_is(first + 9u, RAW_DEVICE_ID, INPUT_KEY_CODE_SEARCH,
		      INPUT_KEY_RELEASED, INPUT_EVENT_EXTENDED) ||
	    !event_is(first + 10u, RAW_DEVICE_ID,
		      INPUT_KEY_CODE_KPJPCOMMA, INPUT_KEY_PRESSED, 0u) ||
	    !event_is(first + 11u, RAW_DEVICE_ID,
		      INPUT_KEY_CODE_KPJPCOMMA, INPUT_KEY_RELEASED, 0u))
		return 2;
	return 0;
}

static int test_backpressure(void)
{
	uint16_t delivered = 0u;
	uint16_t first = received_count;

	defer_next = 1u;
	if (serio_interrupt(&raw_port, 0x32u, 0u, 0u) != SERIO_OK ||
	    endpoints[1].input_device.queue_count != 1u ||
	    serio_interrupt(&raw_port, 0x21u, 0u, 0u) != SERIO_OK ||
	    raw_port.queue_count != 0u ||
	    endpoints[1].input_device.queue_count != 2u ||
	    input_device_pump(&input_core, &endpoints[1].input_binding, 2u,
			      &delivered) != INPUT_OK || delivered != 2u ||
	    !event_is(first, RAW_DEVICE_ID, INPUT_KEY_CODE_B,
		      INPUT_KEY_PRESSED, 0u) ||
	    !event_is(first + 1u, RAW_DEVICE_ID, INPUT_KEY_CODE_C,
		      INPUT_KEY_PRESSED, 0u))
		return 1;
	first = received_count;
	input_core.dispatch_active = 1u;
	if (serio_interrupt(&raw_port, 0xe0u, 0u, 0u) != SERIO_OK ||
	    serio_interrupt(&raw_port, 0x14u, 0u, 0u) != SERIO_RETRY ||
	    serio_interrupt(&raw_port, 0x1cu, 0u, 0u) != SERIO_RETRY ||
	    raw_port.queue_count != 2u)
		return 2;
	input_core.dispatch_active = 0u;
	delivered = 0u;
	if (serio_port_pump(&raw_port, 2u, &delivered) != SERIO_OK ||
	    delivered != 2u || raw_port.queue_count != 0u ||
	    !event_is(first, RAW_DEVICE_ID, INPUT_KEY_CODE_RIGHTCTRL,
		      INPUT_KEY_PRESSED, INPUT_EVENT_EXTENDED) ||
	    !event_is(first + 1u, RAW_DEVICE_ID, INPUT_KEY_CODE_A,
		      INPUT_KEY_PRESSED, 0u))
		return 3;
	return 0;
}

static bool synthetic_release_seen(uint16_t first, input_key_code_t code)
{
	uint16_t index;

	for (index = first; index < received_count; ++index) {
		if (received[index].code == code &&
		    received[index].value == INPUT_KEY_RELEASED &&
		    (received[index].flags & INPUT_EVENT_SYNTHETIC) != 0u)
			return true;
	}
	return false;
}

static int test_stream_loss_recovery(void)
{
	static const uint8_t fill_input[] = {
		0x12u, 0x14u, 0x11u, 0x1cu,
		0x32u, 0x21u, 0x23u, 0x24u,
	};
	struct serio_port_snapshot port_snapshot;
	struct atkbd_endpoint_snapshot endpoint_snapshot;
	uint16_t delivered = 0u;
	uint16_t release_first;
	uint8_t index;

	/* Keep decoded events pending until that FIFO is full. The decoder has
	 * committed those presses, then its zero-commit DEFER fills the raw FIFO.
	 */
	defer_all = 1u;
	if (send_bytes(&raw_port, fill_input, ARRAY_SIZE(fill_input)) != 0 ||
	    endpoints[1].input_device.queue_count !=
		    ARRAY_SIZE(raw_input_queue))
		return 1;
	for (index = 0u; index < ARRAY_SIZE(raw_serio_queue); ++index) {
		if (serio_interrupt(&raw_port, 0x1cu, 0u, 0u) != SERIO_RETRY)
			return 2;
	}
	if (serio_interrupt(&raw_port, 0x1cu, 0u, 0u) !=
		    SERIO_STREAM_LOST ||
	    serio_port_snapshot(&raw_port, &port_snapshot) != SERIO_OK ||
	    port_snapshot.queue_count != ARRAY_SIZE(raw_serio_queue) ||
	    port_snapshot.stream_loss_epoch != 1u ||
	    port_snapshot.recovery_required != 1u ||
	    port_snapshot.accepting != 0u)
		return 3;
	/* The first recovery pass atomically discards the unusable raw suffix,
	 * but cannot yet publish key releases into a full decoded FIFO. */
	if (serio_port_recover_stream(&raw_port, 1u) !=
		    SERIO_RECOVERY_PENDING ||
	    raw_port.queue_count != 0u || endpoints[1].reconnect_active == 0u ||
	    endpoints[1].reconnect_retry_count != 1u)
		return 4;
	defer_all = 0u;
	if (input_device_pump(&input_core, &endpoints[1].input_binding,
			      ARRAY_SIZE(raw_input_queue), &delivered) != INPUT_OK ||
	    delivered != ARRAY_SIZE(raw_input_queue) || modifier_mask == 0u)
		return 5;
	release_first = received_count;
	if (serio_port_recover_stream(&raw_port, 1u) != SERIO_OK ||
	    endpoints[1].reconnect_active != 0u ||
	    endpoints[1].reconnect_count != 1u || modifier_mask != 0u ||
	    !synthetic_release_seen(release_first, INPUT_KEY_CODE_LEFTSHIFT) ||
	    !synthetic_release_seen(release_first, INPUT_KEY_CODE_LEFTCTRL) ||
	    !synthetic_release_seen(release_first, INPUT_KEY_CODE_LEFTALT) ||
	    atkbd_endpoint_snapshot(&keyboard_driver, &raw_reference,
				    &endpoint_snapshot) != ATKBD_OK ||
	    endpoint_snapshot.stream_loss_epoch != 1u ||
	    endpoint_snapshot.stream_recovery_epoch != 1u ||
	    endpoint_snapshot.stream_recovery_required != 0u ||
	    endpoint_snapshot.stream_isolated != 0u ||
	    endpoint_snapshot.stream_recovery_abandoned != 0u ||
	    endpoint_snapshot.reconnect_release_count == 0u)
		return 6;
	/* Pressed state was reset, so the next make is not misreported as repeat. */
	release_first = received_count;
	if (serio_interrupt(&raw_port, 0x12u, 0u, 0u) != SERIO_OK ||
	    !event_is(release_first, RAW_DEVICE_ID, INPUT_KEY_CODE_LEFTSHIFT,
		      INPUT_KEY_PRESSED, 0u) ||
	    serio_interrupt(&raw_port, 0xf0u, 0u, 0u) != SERIO_OK ||
	    serio_interrupt(&raw_port, 0x12u, 0u, 0u) != SERIO_OK ||
	    modifier_mask != 0u)
		return 7;
	return 0;
}

static int ack_current_command(void)
{
	if (serio_interrupt(&raw_port, 0xfau, 0u, 0u) != SERIO_OK)
		return 1;
	return 0;
}

static int test_write_commit_boundaries(void)
{
	struct atkbd_endpoint_snapshot snapshot;
	uint16_t delivered = 0u;
	uint8_t callbacks_before = write_callback_count;
	uint8_t writes_before = write_count;

	/* A zero-commit busy result is the only callback result that may replay
	 * this byte. The retry consumes the transaction-wide write budget but
	 * does not publish a hardware write or protocol commitment. */
	if (atkbd_command_begin(&keyboard_driver, &raw_reference,
				ATKBD_COMMAND_DISABLE, 0u) != ATKBD_OK)
		return 1;
	next_write_action = FAKE_WRITE_ZERO_RETRY;
	if (atkbd_process(&keyboard_driver, &raw_reference) != ATKBD_RETRY ||
	    endpoints[1].command.phase != ATKBD_COMMAND_READY ||
	    endpoints[1].command.writes != 1u ||
	    write_callback_count != (uint8_t)(callbacks_before + 1u) ||
	    write_count != writes_before ||
	    atkbd_endpoint_snapshot(&keyboard_driver, &raw_reference,
				    &snapshot) != ATKBD_OK ||
	    snapshot.protocol_committed != 0u || snapshot.write_uncertain != 0u)
		return 2;

	/* The retry commits once. A response raised before the write callback
	 * returns is serialized in the serio FIFO while ATKBD owns the endpoint;
	 * pumping it advances COMPLETE without issuing the byte again. */
	synchronous_interrupt_status = SERIO_INVALID_STATE;
	next_write_action = FAKE_WRITE_COMMIT_SYNCHRONOUS_ACK;
	if (atkbd_process(&keyboard_driver, &raw_reference) != ATKBD_WAITING ||
	    synchronous_interrupt_status != SERIO_RETRY ||
	    endpoints[1].command.phase != ATKBD_COMMAND_WAIT_ACK ||
	    raw_port.queue_count != 1u || write_count != writes_before + 1u ||
	    atkbd_endpoint_snapshot(&keyboard_driver, &raw_reference,
				    &snapshot) != ATKBD_OK ||
	    snapshot.protocol_committed == 0u || snapshot.write_uncertain != 0u)
		return 3;
	if (atkbd_process(&keyboard_driver, &raw_reference) != ATKBD_WAITING ||
	    write_count != writes_before + 1u ||
	    serio_port_pump(&raw_port, 1u, &delivered) != SERIO_OK ||
	    delivered != 1u || raw_port.queue_count != 0u ||
	    endpoints[1].command.phase != ATKBD_COMMAND_COMPLETE ||
	    endpoints[1].enabled != 0u || write_count != writes_before + 1u)
		return 4;
	if (atkbd_command_begin(&keyboard_driver, &raw_reference,
				ATKBD_COMMAND_ENABLE, 0u) != ATKBD_OK ||
	    atkbd_process(&keyboard_driver, &raw_reference) != ATKBD_WAITING ||
	    ack_current_command() != 0 || endpoints[1].enabled == 0u)
		return 5;

	/* An exact pre-commit I/O failure is not retryable and remains distinct
	 * from an uncertain hardware boundary. */
	writes_before = write_count;
	if (atkbd_command_begin(&keyboard_driver, &translated_reference,
				ATKBD_COMMAND_DISABLE, 0u) != ATKBD_OK)
		return 6;
	next_write_action = FAKE_WRITE_ZERO_ERROR;
	if (atkbd_process(&keyboard_driver, &translated_reference) !=
		    ATKBD_INVALID_STATE ||
	    write_count != writes_before ||
	    atkbd_endpoint_snapshot(&keyboard_driver, &translated_reference,
				    &snapshot) != ATKBD_OK ||
	    snapshot.command_phase != ATKBD_COMMAND_FAILED ||
	    snapshot.protocol_committed != 0u || snapshot.write_uncertain != 0u)
		return 7;

	/* An uncertain write is sticky for this endpoint generation. Neither a
	 * command restart nor process maintenance may resend through it. */
	if (atkbd_command_begin(&keyboard_driver, &translated_reference,
				ATKBD_COMMAND_DISABLE, 0u) != ATKBD_OK)
		return 8;
	next_write_action = FAKE_WRITE_UNCERTAIN;
	callbacks_before = write_callback_count;
	if (atkbd_process(&keyboard_driver, &translated_reference) !=
		    ATKBD_WRITE_UNCERTAIN ||
	    write_callback_count != (uint8_t)(callbacks_before + 1u) ||
	    write_count != writes_before ||
	    atkbd_endpoint_snapshot(&keyboard_driver, &translated_reference,
				    &snapshot) != ATKBD_OK ||
	    snapshot.command_phase != ATKBD_COMMAND_FAILED ||
	    snapshot.protocol_committed != 0u || snapshot.write_uncertain == 0u ||
	    snapshot.reconnect_required == 0u || snapshot.enabled != 0u)
		return 9;
	callbacks_before = write_callback_count;
	if (atkbd_process(&keyboard_driver, &translated_reference) !=
		    ATKBD_WRITE_UNCERTAIN ||
	    atkbd_command_begin(&keyboard_driver, &translated_reference,
				ATKBD_COMMAND_ENABLE, 0u) !=
		    ATKBD_WRITE_UNCERTAIN ||
	    write_callback_count != callbacks_before)
		return 10;
	return 0;
}

static int test_commands_errors_and_reconnect(void)
{
	struct atkbd_endpoint_snapshot snapshot;
	uint64_t reconnect_before = endpoints[1].reconnect_count;
	uint8_t writes_before = write_count;
	uint8_t command_steps = 0u;
	uint16_t first;

	if (serio_interrupt(&raw_port, 0x1cu, 0u,
			    SERIO_RAW_PARITY_ERROR) != SERIO_OK ||
	    atkbd_process(&keyboard_driver, &raw_reference) != ATKBD_OK ||
	    writes[write_count - 1u] != 0xfeu)
		return 1;
	if (atkbd_command_begin(&keyboard_driver, &raw_reference,
				ATKBD_COMMAND_NEGOTIATE, 0u) != ATKBD_OK ||
	    atkbd_process(&keyboard_driver, &raw_reference) != ATKBD_WAITING ||
	    writes[write_count - 1u] != 0xf5u ||
	    serio_interrupt(&raw_port, 0xfeu, 0u, 0u) != SERIO_OK ||
	    atkbd_process(&keyboard_driver, &raw_reference) != ATKBD_WAITING ||
	    writes[write_count - 1u] != 0xf5u || ack_current_command() != 0)
		return 2;
	/* Each byte has its own NAK budget; writes remain transaction-wide. */
	if (atkbd_process(&keyboard_driver, &raw_reference) != ATKBD_WAITING ||
	    writes[write_count - 1u] != 0xf0u ||
	    serio_interrupt(&raw_port, 0xfeu, 0u, 0u) != SERIO_OK ||
	    endpoints[1].command.phase != ATKBD_COMMAND_READY ||
	    endpoints[1].command.byte_naks != 1u ||
	    atkbd_process(&keyboard_driver, &raw_reference) != ATKBD_WAITING ||
	    writes[write_count - 1u] != 0xf0u || ack_current_command() != 0 ||
	    endpoints[1].command.byte_naks != 0u)
		return 3;
	while (endpoints[1].command.phase != ATKBD_COMMAND_COMPLETE &&
	       command_steps++ < 16u) {
		enum atkbd_status status =
			atkbd_process(&keyboard_driver, &raw_reference);

		if (status != ATKBD_WAITING || ack_current_command() != 0)
			return 4;
	}
	if (endpoints[1].command.phase != ATKBD_COMMAND_COMPLETE ||
	    endpoints[1].enabled == 0u || endpoints[1].negotiated == 0u ||
	    endpoints[1].command.writes != 10u ||
	    write_count - writes_before != 11u)
		return 5;
	/* A second command fails after the configured two NAKs. */
	if (atkbd_command_begin(&keyboard_driver, &raw_reference,
				ATKBD_COMMAND_SET_LEDS, 3u) != ATKBD_OK ||
	    atkbd_process(&keyboard_driver, &raw_reference) != ATKBD_WAITING ||
	    serio_interrupt(&raw_port, 0xfeu, 0u, 0u) != SERIO_OK ||
	    atkbd_process(&keyboard_driver, &raw_reference) != ATKBD_WAITING ||
	    serio_interrupt(&raw_port, 0xfeu, 0u, 0u) != SERIO_OK ||
	    atkbd_process(&keyboard_driver, &raw_reference) !=
		    ATKBD_PROTOCOL_ERROR)
		return 6;
	if (atkbd_command_begin(&keyboard_driver, &raw_reference,
				ATKBD_COMMAND_DISABLE, 0u) != ATKBD_OK ||
	    atkbd_process(&keyboard_driver, &raw_reference) != ATKBD_WAITING ||
	    ack_current_command() != 0 || endpoints[1].enabled != 0u ||
	    atkbd_command_begin(&keyboard_driver, &raw_reference,
				ATKBD_COMMAND_ENABLE, 0u) != ATKBD_OK ||
	    atkbd_process(&keyboard_driver, &raw_reference) != ATKBD_WAITING ||
	    ack_current_command() != 0 || endpoints[1].enabled == 0u)
		return 7;
	/* BAT is not a host reset: it requests a bounded serio reconnect. */
	if (serio_interrupt(&raw_port, 0xaau, 0u, 0u) != SERIO_OK ||
	    endpoints[1].reconnect_required == 0u ||
	    atkbd_process(&keyboard_driver, &raw_reference) != ATKBD_OK ||
	    endpoints[1].reconnect_count != reconnect_before + 1u)
		return 8;
	/* Reconnect cleared pressed state, so the next A is a press. */
	first = received_count;
	if (serio_interrupt(&raw_port, 0x1cu, 0u, 0u) != SERIO_OK ||
	    !event_is(first, RAW_DEVICE_ID, INPUT_KEY_CODE_A,
		      INPUT_KEY_PRESSED, 0u) ||
	    atkbd_endpoint_snapshot(&keyboard_driver, &raw_reference,
				    &snapshot) != ATKBD_OK ||
	    snapshot.bad_frame_count != 1u || snapshot.ack_count < 7u ||
	    snapshot.nak_count != 4u || snapshot.bat_count != 1u)
		return 9;
	return 0;
}

static int test_generation_rebind(void)
{
	struct atkbd_endpoint_reference old_reference = raw_reference;
	struct atkbd_endpoint_snapshot snapshot;
	uint16_t first;

	if (input_focus_clear(&input_core, &handler_binding) != INPUT_OK ||
	    input_core_quiesce(&input_core, INPUT_CORE_ID) != INPUT_OK ||
	    serio_port_quiesce(&raw_port) != SERIO_OK ||
	    serio_port_unregister(&raw_port) != SERIO_OK)
		return 1;
	if (setup_serio_port(&raw_port, RAW_PORT_ID, raw_serio_queue,
			     ARRAY_SIZE(raw_serio_queue), false) != 0 ||
	    atkbd_endpoint_reference(&keyboard_driver, DRIVER_ID, 1u,
				     &raw_reference) != ATKBD_OK)
		return 2;
	if (raw_reference.endpoint_generation ==
		    old_reference.endpoint_generation ||
	    raw_reference.port_generation == old_reference.port_generation ||
	    atkbd_endpoint_snapshot(&keyboard_driver, &old_reference,
				    &snapshot) != ATKBD_STALE_REFERENCE)
		return 3;
	if (input_core_resume(&input_core, INPUT_CORE_ID) != INPUT_OK ||
	    input_focus_set(&input_core, &handler_binding) != INPUT_OK)
		return 4;
	first = received_count;
	if (serio_interrupt(&raw_port, 0x1cu, 0u, 0u) != SERIO_OK ||
	    !event_is(first, RAW_DEVICE_ID, INPUT_KEY_CODE_A,
		      INPUT_KEY_PRESSED, 0u))
		return 5;
	return 0;
}

static int teardown(void)
{
	if (input_focus_clear(&input_core, &handler_binding) != INPUT_OK ||
	    input_core_quiesce(&input_core, INPUT_CORE_ID) != INPUT_OK)
		return 1;
	if (serio_port_quiesce(&raw_port) != SERIO_OK ||
	    serio_port_quiesce(&translated_port) != SERIO_OK || close_count != 3u)
		return 2;
	if (atkbd_driver_unregister(&keyboard_driver, DRIVER_ID) != ATKBD_OK)
		return 3;
	if (input_handler_quiesce(&input_core, &handler_binding) != INPUT_OK ||
	    input_handler_unregister(&input_core, &handler_binding) != INPUT_OK ||
	    input_core_retire(&input_core, INPUT_CORE_ID) != INPUT_OK)
		return 4;
	return focus_enter_count == 2u && focus_leave_count == 2u ? 0 : 5;
}

static int run_tests(void)
{
	int result;

	result = setup();
	if (result != 0)
		return 10 + result;
	result = test_translated_decode();
	if (result != 0)
		return 30 + result;
	result = test_raw_decode();
	if (result != 0)
		return 50 + result;
	result = test_named_map_edges();
	if (result != 0)
		return 60 + result;
	result = test_backpressure();
	if (result != 0)
		return 70 + result;
	result = test_stream_loss_recovery();
	if (result != 0)
		return 90 + result;
	result = test_write_commit_boundaries();
	if (result != 0)
		return 100 + result;
	result = test_commands_errors_and_reconnect();
	if (result != 0)
		return 110 + result;
	result = test_generation_rebind();
	if (result != 0)
		return 130 + result;
	result = teardown();
	return result == 0 ? 0 : 150 + result;
}

DOSC32_TEST_ENTRY(run_tests)

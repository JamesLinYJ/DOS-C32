// SPDX-License-Identifier: GPL-2.0-only
/* Generation-bound input focus and deferred-delivery tests. */
#include "input.h"
#include "test_entry.h"

#define CORE_ID ((kernel_object_handle_t)0x494e505554434f52ull)
#define GUARD_ID ((kernel_object_handle_t)0x494e505554475244ull)
#define DEVICE_ID ((kernel_object_handle_t)0x494e505554444556ull)
#define HANDLER_A_ID ((kernel_object_handle_t)0x494e505554484e41ull)
#define HANDLER_B_ID ((kernel_object_handle_t)0x494e505554484e42ull)
#define CONTEXT_A ((kernel_object_handle_t)0x494e505554435441ull)
#define CONTEXT_B ((kernel_object_handle_t)0x494e505554435442ull)

enum fake_mode {
	FAKE_HANDLE = 0,
	FAKE_DEFER,
	FAKE_REJECT,
	FAKE_BREAK
};

struct fake_handler_state {
	struct input_event last;
	uint64_t enter_count;
	uint64_t leave_count;
	uint64_t receive_count;
	uint64_t last_focus_generation;
	uint8_t mode;
	uint8_t reject_enter;
	uint8_t reserved[6];
};

static struct fake_handler_state handler_a_state;
static struct fake_handler_state handler_b_state;
static uint32_t guard_depth;
static uint32_t guard_fault;

static struct fake_handler_state *state_for_context(
	kernel_object_handle_t context)
{
	if (context == CONTEXT_A)
		return &handler_a_state;
	if (context == CONTEXT_B)
		return &handler_b_state;
	return NULL;
}

static void guard_enter(kernel_object_handle_t context)
{
	if (context != GUARD_ID || guard_depth != 0u)
		guard_fault = 1u;
	guard_depth++;
}

static void guard_exit(kernel_object_handle_t context)
{
	if (context != GUARD_ID || guard_depth != 1u)
		guard_fault = 1u;
	if (guard_depth != 0u)
		guard_depth--;
}

static enum input_focus_result focus_enter(
	struct input_handler *handler, kernel_object_handle_t context,
	kernel_object_handle_t core_identity, uint64_t focus_generation)
{
	struct fake_handler_state *state = state_for_context(context);

	if (state == NULL || input_handler_context(handler) != state ||
	    core_identity != CORE_ID || focus_generation == 0u ||
	    guard_depth != 0u)
		return INPUT_FOCUS_BROKEN;
	state->enter_count++;
	state->last_focus_generation = focus_generation;
	return state->reject_enter != 0u ? INPUT_FOCUS_REJECTED
					 : INPUT_FOCUS_OK;
}

static void focus_leave(struct input_handler *handler,
			kernel_object_handle_t context,
			kernel_object_handle_t core_identity,
			uint64_t focus_generation)
{
	struct fake_handler_state *state = state_for_context(context);

	if (state == NULL || input_handler_context(handler) != state ||
	    core_identity != CORE_ID || focus_generation == 0u ||
	    guard_depth != 0u) {
		guard_fault = 1u;
		return;
	}
	state->leave_count++;
	state->last_focus_generation = focus_generation;
}

static enum input_handler_result receive_event(
	struct input_handler *handler, kernel_object_handle_t context,
	const struct input_event *event)
{
	struct fake_handler_state *state = state_for_context(context);

	if (state == NULL || input_handler_context(handler) != state ||
	    event == NULL || guard_depth != 0u)
		return INPUT_HANDLER_BROKEN;
	state->last = *event;
	state->receive_count++;
	if (state->mode == FAKE_DEFER)
		return INPUT_HANDLER_DEFER;
	if (state->mode == FAKE_REJECT)
		return INPUT_HANDLER_REJECTED;
	if (state->mode == FAKE_BREAK)
		return INPUT_HANDLER_BROKEN;
	return INPUT_HANDLER_HANDLED;
}

static struct input_handler_config handler_config(
	kernel_object_handle_t identity, kernel_object_handle_t context)
{
	return (struct input_handler_config){
		.identity = identity,
		.context = context,
		.handler_context = state_for_context(context),
		.capabilities = INPUT_CAPABILITY_KEY,
		.focus_enter = focus_enter,
		.focus_leave = focus_leave,
		.receive = receive_event,
		.reserved = {0u},
	};
}

static int run_input_core_test(void)
{
	static struct input_core core;
	static struct input_device device;
	static struct input_handler handler_a;
	static struct input_handler handler_b;
	static struct input_device *devices[2];
	static struct input_handler *handlers[2];
	static struct input_device *larger_devices[4];
	static struct input_handler *larger_handlers[4];
	static struct input_event queue[2];
	struct input_core unconstructed = {0};
	struct input_device unconstructed_device = {0};
	struct input_core_config config = {
		.identity = CORE_ID,
		.guard_context = GUARD_ID,
		.irq_enter = guard_enter,
		.irq_exit = guard_exit,
		.caller_serializes_irq = 0u,
		.reserved = {0u},
	};
	struct input_device_config device_config = {
		.identity = DEVICE_ID,
		.capabilities = INPUT_CAPABILITY_KEY,
		.queue = queue,
		.queue_capacity = ARRAY_SIZE(queue),
		.reserved = {0u},
	};
	struct input_handler_config a_config =
		handler_config(HANDLER_A_ID, CONTEXT_A);
	struct input_handler_config b_config =
		handler_config(HANDLER_B_ID, CONTEXT_B);
	struct input_device_binding device_binding;
	struct input_handler_binding a_binding;
	struct input_handler_binding b_binding;
	struct input_core_snapshot core_snapshot;
	struct input_device_snapshot device_snapshot;
	struct input_event queued_head;
	struct input_event queued_tail;
	uint64_t last_sequence;
	uint64_t overflow_before;
	uint64_t receive_before;
	uint16_t queued_tail_index;
	uint16_t delivered;

	if (input_core_initialize(&unconstructed, &config, devices,
				  ARRAY_SIZE(devices), handlers,
				  ARRAY_SIZE(handlers)) != INPUT_INVALID_STATE ||
	    input_device_register(&unconstructed, &unconstructed_device,
				  &device_config, &device_binding) !=
		    INPUT_INVALID_ARGUMENT)
		return 1;
	input_core_construct(&core);
	input_device_construct(&device);
	input_handler_construct(&handler_a);
	input_handler_construct(&handler_b);
	if (input_core_initialize(&core, &config, devices, ARRAY_SIZE(devices),
				  handlers, ARRAY_SIZE(handlers)) != INPUT_OK ||
	    input_device_register(&core, &device, &device_config,
				  &device_binding) != INPUT_OK ||
	    input_handler_register(&core, &handler_a, &a_config, &a_binding) !=
		    INPUT_OK ||
	    input_handler_register(&core, &handler_b, &b_config, &b_binding) !=
		    INPUT_OK ||
	    input_focus_set(&core, &a_binding) != INPUT_OK ||
	    input_core_publish(&core, CORE_ID) != INPUT_OK)
		return 2;
	if (handler_a_state.enter_count != 1u ||
	    input_submit(&core, &device_binding, INPUT_EVENT_KEY, 30u,
			 (uint8_t)INPUT_KEY_PRESSED, 0x1eu, 0u) != INPUT_OK ||
	    handler_a_state.receive_count != 1u ||
	    handler_a_state.last.device_identity != DEVICE_ID ||
	    handler_a_state.last.handler_identity != HANDLER_A_ID ||
	    handler_a_state.last.code != 30u ||
	    handler_a_state.last.value != INPUT_KEY_PRESSED)
		return 3;

	/* Once the head defers, a later handleable event must append without
	 * calling the handler out of order. */
	handler_a_state.mode = FAKE_DEFER;
	if (input_submit(&core, &device_binding, INPUT_EVENT_KEY, 48u,
			 (uint8_t)INPUT_KEY_PRESSED, 0x30u, 0u) !=
		    INPUT_DEFERRED || handler_a_state.receive_count != 2u ||
	    input_device_snapshot(&device, &device_snapshot) != INPUT_OK ||
	    device_snapshot.queue_count != 1u)
		return 4;
	handler_a_state.mode = FAKE_HANDLE;
	if (input_submit(&core, &device_binding, INPUT_EVENT_KEY, 46u,
			 (uint8_t)INPUT_KEY_PRESSED, 0x2eu, 0u) !=
		    INPUT_DEFERRED || handler_a_state.receive_count != 2u ||
	    device.queue_count != 2u ||
	    queue[device.queue_head].code != 48u ||
	    queue[(device.queue_head + 1u) % ARRAY_SIZE(queue)].code != 46u)
		return 5;
	delivered = 0u;
	if (input_device_pump(&core, &device_binding, 1u, &delivered) != INPUT_OK ||
	    delivered != 1u || handler_a_state.receive_count != 3u ||
	    handler_a_state.last.code != 48u || device.queue_count != 1u)
		return 6;
	delivered = 0u;
	if (input_device_pump(&core, &device_binding, 1u, &delivered) != INPUT_OK ||
	    delivered != 1u || handler_a_state.receive_count != 4u ||
	    handler_a_state.last.code != 46u || device.queue_count != 0u)
		return 7;

	/* A focus-generation change invalidates only the old head.  A newly
	 * submitted current-generation item remains behind it and is delivered
	 * after the pump discards that stale head. */
	handler_a_state.mode = FAKE_DEFER;
	if (input_submit(&core, &device_binding, INPUT_EVENT_KEY, 32u,
			 (uint8_t)INPUT_KEY_PRESSED, 0x20u, 0u) != INPUT_DEFERRED ||
	    device.queue_count != 1u ||
	    input_focus_set(&core, &b_binding) != INPUT_OK ||
	    handler_a_state.leave_count != 1u ||
	    handler_b_state.enter_count != 1u)
		return 8;
	receive_before = handler_b_state.receive_count;
	if (input_submit(&core, &device_binding, INPUT_EVENT_KEY, 18u,
			 (uint8_t)INPUT_KEY_PRESSED, 0x12u, 0u) != INPUT_DEFERRED ||
	    handler_b_state.receive_count != receive_before ||
	    device.queue_count != 2u)
		return 9;
	delivered = 0u;
	if (input_device_pump(&core, &device_binding, 1u, &delivered) != INPUT_OK ||
	    delivered != 1u || handler_b_state.receive_count != receive_before + 1u ||
	    handler_b_state.last.code != 18u ||
	    input_device_snapshot(&device, &device_snapshot) != INPUT_OK ||
	    device_snapshot.queue_count != 0u ||
	    device_snapshot.stale_focus_drop_count != 1u)
		return 10;

	/* A full FIFO rejects only the new item: it neither calls downstream nor
	 * changes the existing head/tail or their eventual delivery order. */
	handler_b_state.mode = FAKE_DEFER;
	if (input_submit(&core, &device_binding, INPUT_EVENT_KEY, 33u,
			 (uint8_t)INPUT_KEY_PRESSED, 0x21u, 0u) !=
		    INPUT_DEFERRED)
		return 11;
	handler_b_state.mode = FAKE_HANDLE;
	if (input_submit(&core, &device_binding, INPUT_EVENT_KEY, 34u,
			 (uint8_t)INPUT_KEY_PRESSED, 0x22u, 0u) != INPUT_DEFERRED ||
	    device.queue_count != ARRAY_SIZE(queue))
		return 12;
	queued_tail_index = (uint16_t)((device.queue_head + 1u) % ARRAY_SIZE(queue));
	queued_head = queue[device.queue_head];
	queued_tail = queue[queued_tail_index];
	receive_before = handler_b_state.receive_count;
	last_sequence = handler_b_state.last.sequence;
	overflow_before = device.overflow_count;
	if (input_submit(&core, &device_binding, INPUT_EVENT_KEY, 35u,
			 (uint8_t)INPUT_KEY_PRESSED, 0x23u, 0u) !=
		    INPUT_CAPACITY_EXHAUSTED ||
	    handler_b_state.receive_count != receive_before ||
	    handler_b_state.last.sequence != last_sequence ||
	    device.queue_count != ARRAY_SIZE(queue) ||
	    device.overflow_count != overflow_before + 1u ||
	    queue[device.queue_head].sequence != queued_head.sequence ||
	    queue[device.queue_head].code != queued_head.code ||
	    queue[queued_tail_index].sequence != queued_tail.sequence ||
	    queue[queued_tail_index].code != queued_tail.code)
		return 13;
	delivered = 0u;
	if (input_device_pump(&core, &device_binding, 1u, &delivered) != INPUT_OK ||
	    delivered != 1u || handler_b_state.last.code != 33u ||
	    device.queue_count != 1u)
		return 14;
	delivered = 0u;
	if (input_device_pump(&core, &device_binding, 1u, &delivered) != INPUT_OK ||
	    delivered != 1u || handler_b_state.last.code != 34u ||
	    device.queue_count != 0u)
		return 15;

	handler_b_state.mode = FAKE_BREAK;
	if (input_submit(&core, &device_binding, INPUT_EVENT_KEY, 36u,
			 (uint8_t)INPUT_KEY_PRESSED, 0x24u, 0u) !=
		    INPUT_HANDLER_FAULT ||
	    input_core_snapshot(&core, &core_snapshot) != INPUT_OK ||
	    core_snapshot.focus_identity != HANDLER_B_ID ||
	    input_submit(&core, &device_binding, INPUT_EVENT_KEY, 36u,
			 (uint8_t)INPUT_KEY_RELEASED, 0x24u, 0u) !=
		    INPUT_ACCESS_DENIED)
		return 16;
	if (input_core_replace_storage(&core, CORE_ID, larger_devices,
				       ARRAY_SIZE(larger_devices), larger_handlers,
				       ARRAY_SIZE(larger_handlers)) !=
		    INPUT_INVALID_STATE ||
	    input_device_quiesce(&core, &device_binding) != INPUT_INVALID_STATE)
		return 17;

	if (input_core_quiesce(&core, CORE_ID) != INPUT_OK ||
	    input_core_replace_storage(&core, CORE_ID, larger_devices,
				       ARRAY_SIZE(larger_devices), larger_handlers,
				       ARRAY_SIZE(larger_handlers)) != INPUT_OK ||
	    input_focus_clear(&core, &b_binding) != INPUT_OK ||
	    handler_b_state.leave_count != 1u ||
	    input_device_quiesce(&core, &device_binding) != INPUT_OK ||
	    input_handler_quiesce(&core, &a_binding) != INPUT_OK ||
	    input_handler_quiesce(&core, &b_binding) != INPUT_OK ||
	    input_device_unregister(&core, &device_binding) != INPUT_OK ||
	    input_handler_unregister(&core, &a_binding) != INPUT_OK ||
	    input_handler_unregister(&core, &b_binding) != INPUT_OK ||
	    input_core_retire(&core, CORE_ID) != INPUT_OK)
		return 18;
	if (input_core_snapshot(&core, &core_snapshot) != INPUT_INVALID_STATE ||
	    guard_fault != 0u || guard_depth != 0u)
		return 19;
	return 0;
}

DOSC32_TEST_ENTRY(run_input_core_test)

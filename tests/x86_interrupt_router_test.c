// SPDX-License-Identifier: GPL-2.0-only
/* Host-safe native action dispatch and guest interrupt-domain tests. */
#include "test_entry.h"
#include "x86_guest_irq_router.h"
#include "x86_native_irq_dispatch.h"

#define ROUTER_ID ((kernel_object_handle_t)0x4755455354495251ull)
#define SINK_CONTEXT ((kernel_object_handle_t)0x475545535453494eull)
#define PIT_PRODUCER_ID ((kernel_object_handle_t)0x50495450524f4455ull)
#define INPUT_PRODUCER_ID ((kernel_object_handle_t)0x493830343250524full)
#define OTHER_PRODUCER_ID ((kernel_object_handle_t)0x4f5448455250524full)
#define DISPATCH_ID ((kernel_object_handle_t)0x4e41544956444953ull)
#define CONTROLLER_ID ((kernel_object_handle_t)0x4e41544956454354ull)
#define CONTROLLER_CONTEXT ((kernel_object_handle_t)0x4e41544956454358ull)
#define PIT_ACTION_ID ((kernel_object_handle_t)0x504954414354494full)
#define KEY_ACTION_ID ((kernel_object_handle_t)0x4b4559414354494full)
#define ACTION_CONTEXT ((kernel_object_handle_t)0x414354494f4e4358ull)

struct fake_sink_state {
	struct x86_legacy_chipset_source_config config;
	struct x86_legacy_chipset_source_event last_event;
	uint64_t bind_count;
	uint64_t submit_count;
	uint64_t quiesce_count;
	uint64_t resume_count;
	uint64_t unbind_count;
	uint8_t bound;
	uint8_t quiesced;
	uint8_t reject_next_submit;
	uint8_t reserved;
};

struct fake_controller_state {
	enum x86_native_irq_completion last_completion;
	uint64_t begin_count;
	uint64_t end_count;
	uint64_t quiesce_count;
	uint64_t resume_count;
	uint8_t active;
	uint8_t reserved[7];
};

static struct fake_sink_state sink_state;
static struct fake_controller_state controller_state;
static struct x86_guest_irq_router *action_router;
static struct x86_guest_irq_producer_binding action_pit_producer;
static uint64_t pit_action_count;
static uint64_t keyboard_action_count;

static enum x86_guest_irq_sink_result fake_sink_bind(
	kernel_object_handle_t context, kernel_object_handle_t router_identity,
	const struct x86_legacy_chipset_source_config *config)
{
	if (context != SINK_CONTEXT || router_identity != ROUTER_ID ||
	    config == NULL || sink_state.bound != 0u)
		return X86_GUEST_IRQ_SINK_POISONED;
	sink_state.config = *config;
	sink_state.bound = 1u;
	sink_state.quiesced = 0u;
	sink_state.bind_count++;
	return X86_GUEST_IRQ_SINK_OK;
}

static enum x86_guest_irq_sink_result fake_sink_submit(
	kernel_object_handle_t context, kernel_object_handle_t router_identity,
	const struct x86_legacy_chipset_source_event *event)
{
	if (context != SINK_CONTEXT || router_identity != ROUTER_ID ||
	    event == NULL || sink_state.bound == 0u ||
	    sink_state.quiesced != 0u)
		return X86_GUEST_IRQ_SINK_POISONED;
	if (sink_state.reject_next_submit != 0u) {
		sink_state.reject_next_submit = 0u;
		return X86_GUEST_IRQ_SINK_REJECTED;
	}
	sink_state.last_event = *event;
	sink_state.submit_count++;
	return X86_GUEST_IRQ_SINK_OK;
}

static enum x86_guest_irq_sink_result fake_sink_quiesce(
	kernel_object_handle_t context, kernel_object_handle_t router_identity)
{
	if (context != SINK_CONTEXT || router_identity != ROUTER_ID ||
	    sink_state.bound == 0u || sink_state.quiesced != 0u)
		return X86_GUEST_IRQ_SINK_POISONED;
	sink_state.quiesced = 1u;
	sink_state.quiesce_count++;
	return X86_GUEST_IRQ_SINK_OK;
}

static enum x86_guest_irq_sink_result fake_sink_resume(
	kernel_object_handle_t context, kernel_object_handle_t router_identity)
{
	if (context != SINK_CONTEXT || router_identity != ROUTER_ID ||
	    sink_state.bound == 0u || sink_state.quiesced == 0u)
		return X86_GUEST_IRQ_SINK_POISONED;
	sink_state.quiesced = 0u;
	sink_state.resume_count++;
	return X86_GUEST_IRQ_SINK_OK;
}

static enum x86_guest_irq_sink_result fake_sink_unbind(
	kernel_object_handle_t context, kernel_object_handle_t router_identity)
{
	if (context != SINK_CONTEXT || router_identity != ROUTER_ID ||
	    sink_state.bound == 0u || sink_state.quiesced == 0u)
		return X86_GUEST_IRQ_SINK_POISONED;
	sink_state.bound = 0u;
	sink_state.unbind_count++;
	return X86_GUEST_IRQ_SINK_OK;
}

static enum x86_native_irq_controller_result fake_controller_begin(
	kernel_object_handle_t context,
	const struct x86_native_irq_event *event,
	struct x86_native_irq_observation *observation)
{
	if (context != CONTROLLER_CONTEXT || event == NULL ||
	    observation == NULL || controller_state.active == 0u ||
	    event->controller_identity != CONTROLLER_ID)
		return X86_NATIVE_IRQ_CONTROLLER_RESULT_POISONED;
	*observation = (struct x86_native_irq_observation){
		.controller_cookie = event->sequence,
		.kind = (event->hardware_irq == 7u ||
			 event->hardware_irq == 15u)
				? X86_NATIVE_IRQ_OBSERVATION_SPURIOUS
				: X86_NATIVE_IRQ_OBSERVATION_DELIVER,
		.reserved = {0u},
	};
	controller_state.begin_count++;
	return X86_NATIVE_IRQ_CONTROLLER_RESULT_OK;
}

static enum x86_native_irq_controller_result fake_controller_end(
	kernel_object_handle_t context,
	const struct x86_native_irq_event *event,
	const struct x86_native_irq_observation *observation,
	enum x86_native_irq_completion completion)
{
	if (context != CONTROLLER_CONTEXT || event == NULL ||
	    observation == NULL || controller_state.active == 0u ||
	    observation->controller_cookie != event->sequence)
		return X86_NATIVE_IRQ_CONTROLLER_RESULT_POISONED;
	controller_state.last_completion = completion;
	controller_state.end_count++;
	return X86_NATIVE_IRQ_CONTROLLER_RESULT_OK;
}

static enum x86_native_irq_controller_result fake_controller_quiesce(
	kernel_object_handle_t context, kernel_object_handle_t dispatch_identity)
{
	if (context != CONTROLLER_CONTEXT || dispatch_identity != DISPATCH_ID ||
	    controller_state.active == 0u)
		return X86_NATIVE_IRQ_CONTROLLER_RESULT_POISONED;
	controller_state.active = 0u;
	controller_state.quiesce_count++;
	return X86_NATIVE_IRQ_CONTROLLER_RESULT_OK;
}

static enum x86_native_irq_controller_result fake_controller_resume(
	kernel_object_handle_t context, kernel_object_handle_t dispatch_identity)
{
	if (context != CONTROLLER_CONTEXT || dispatch_identity != DISPATCH_ID ||
	    controller_state.active != 0u)
		return X86_NATIVE_IRQ_CONTROLLER_RESULT_POISONED;
	controller_state.active = 1u;
	controller_state.resume_count++;
	return X86_NATIVE_IRQ_CONTROLLER_RESULT_OK;
}

static enum x86_native_irq_action_result pit_action(
	kernel_object_handle_t context,
	const struct x86_native_irq_event *event)
{
	struct x86_legacy_irq_event source_event = {
		.pit_input_ticks = 1193u,
		.source_identity = PIT_PRODUCER_ID,
		.kind = X86_LEGACY_IRQ_EVENT_PIT_CLOCK,
		.irq = 0u,
		.pit_rate_calibrated = 0u,
		.reserved = {0u},
	};

	if (context != ACTION_CONTEXT || event == NULL ||
	    event->hardware_irq != 0u || action_router == NULL)
		return X86_NATIVE_IRQ_ACTION_FAULT;
	if (x86_guest_irq_route_native(action_router, &action_pit_producer,
				       &source_event) != X86_GUEST_IRQ_ROUTER_OK)
		return X86_NATIVE_IRQ_ACTION_FAULT;
	pit_action_count++;
	return X86_NATIVE_IRQ_ACTION_HANDLED;
}

static enum x86_native_irq_action_result keyboard_action(
	kernel_object_handle_t context,
	const struct x86_native_irq_event *event)
{
	if (context != ACTION_CONTEXT || event == NULL ||
	    event->hardware_irq != 1u)
		return X86_NATIVE_IRQ_ACTION_FAULT;
	/* Physical IRQ1 is consumed here; serio/guest-i8042 submits separately. */
	keyboard_action_count++;
	return X86_NATIVE_IRQ_ACTION_HANDLED;
}

static int prepare_guest_router(
	struct x86_guest_irq_router *router,
	struct x86_guest_irq_producer_binding *pit,
	struct x86_guest_irq_producer_binding *input,
	struct x86_guest_irq_route_binding *pit_route)
{
	static struct x86_guest_irq_producer_slot small_producers[2];
	static struct x86_guest_irq_route_slot small_routes[1];
	static struct x86_guest_irq_producer_slot grown_producers[4];
	static struct x86_guest_irq_route_slot grown_routes[4];
	struct x86_guest_irq_router unconstructed = {0};
	const struct x86_guest_irq_router_config router_config = {
		.identity = ROUTER_ID,
		.sink_context_identity = SINK_CONTEXT,
		.sink = {
			.bind = fake_sink_bind,
			.submit = fake_sink_submit,
			.quiesce = fake_sink_quiesce,
			.resume = fake_sink_resume,
			.unbind = fake_sink_unbind,
		},
		.pit_input_quantum = 1193u,
		.capabilities = X86_GUEST_IRQ_PRODUCER_CAPABILITIES,
		.pit_rate_calibrated = 0u,
		.reserved = {0u},
	};
	const struct x86_guest_irq_producer_config pit_config = {
		.identity = PIT_PRODUCER_ID,
		.capabilities = X86_GUEST_IRQ_PRODUCER_PIT_CLOCK,
		.allowed_guest_irqs = 1u << 0,
		.reserved = {0u},
	};
	const struct x86_guest_irq_producer_config input_config = {
		.identity = INPUT_PRODUCER_ID,
		.capabilities = X86_GUEST_IRQ_PRODUCER_IRQ_EDGE,
		.allowed_guest_irqs = (1u << 1) | (1u << 12),
		.reserved = {0u},
	};
	const struct x86_guest_irq_native_route_config route_config = {
		.native_kind = X86_LEGACY_IRQ_EVENT_PIT_CLOCK,
		.native_irq = 0u,
		.guest_kind = X86_GUEST_IRQ_EVENT_PIT_CLOCK,
		.guest_irq = 0u,
		.reserved = {0u},
	};

	if (x86_guest_irq_router_initialize(
		    &unconstructed, small_producers, ARRAY_SIZE(small_producers),
		    small_routes, ARRAY_SIZE(small_routes)) !=
	    X86_GUEST_IRQ_ROUTER_INVALID_STATE)
		return 1;
	x86_guest_irq_router_construct(router);
	if (x86_guest_irq_router_initialize(
		    router, small_producers, ARRAY_SIZE(small_producers),
		    small_routes, ARRAY_SIZE(small_routes)) !=
		    X86_GUEST_IRQ_ROUTER_OK ||
	    x86_guest_irq_router_prepare(router, &router_config) !=
		    X86_GUEST_IRQ_ROUTER_OK ||
	    x86_guest_irq_producer_register(router, &pit_config, pit) !=
		    X86_GUEST_IRQ_ROUTER_OK ||
	    x86_guest_irq_producer_register(router, &input_config, input) !=
		    X86_GUEST_IRQ_ROUTER_OK ||
	    x86_guest_irq_native_route_install(router, pit, &route_config,
					       pit_route) !=
		    X86_GUEST_IRQ_ROUTER_OK)
		return 2;
	/* Prepared storage growth preserves slot and generation bindings. */
	if (x86_guest_irq_router_replace_storage(
		    router, grown_producers, ARRAY_SIZE(grown_producers),
		    grown_routes, ARRAY_SIZE(grown_routes)) !=
		    X86_GUEST_IRQ_ROUTER_OK ||
	    x86_guest_irq_router_publish(router) != X86_GUEST_IRQ_ROUTER_OK ||
	    sink_state.bind_count != 1u || sink_state.bound != 1u)
		return 3;
	return 0;
}

static int prepare_native_dispatch(
	struct x86_native_irq_dispatch *dispatch,
	struct x86_native_irq_action_binding *pit_binding,
	struct x86_native_irq_action_binding *key_binding)
{
	static struct x86_native_irq_line_slot small_lines[4];
	static struct x86_native_irq_action_slot small_actions[2];
	static struct x86_native_irq_line_slot grown_lines[8];
	static struct x86_native_irq_action_slot grown_actions[4];
	struct x86_native_irq_dispatch unconstructed = {0};
	const struct x86_native_irq_line_config line_config[] = {
		{.vector = 0x20u, .hardware_irq = 0u, .flags = 0u,
		 .reserved = {0u}},
		{.vector = 0x21u, .hardware_irq = 1u, .flags = 0u,
		 .reserved = {0u}},
		{.vector = 0x27u, .hardware_irq = 7u, .flags = 0u,
		 .reserved = {0u}},
		{.vector = 0x2fu, .hardware_irq = 15u, .flags = 0u,
		 .reserved = {0u}},
	};
	const struct x86_native_irq_dispatch_config dispatch_config = {
		.identity = DISPATCH_ID,
		.controller_identity = CONTROLLER_ID,
		.controller_context = CONTROLLER_CONTEXT,
		.controller = {
			.begin = fake_controller_begin,
			.end = fake_controller_end,
			.quiesce = fake_controller_quiesce,
			.resume = fake_controller_resume,
		},
	};
	const struct x86_native_irq_action_config pit_config = {
		.identity = PIT_ACTION_ID,
		.context = ACTION_CONTEXT,
		.hardware_irq = 0u,
		.shared = 0u,
		.reserved = {0u},
		.handler = pit_action,
	};
	const struct x86_native_irq_action_config key_config = {
		.identity = KEY_ACTION_ID,
		.context = ACTION_CONTEXT,
		.hardware_irq = 1u,
		.shared = 0u,
		.reserved = {0u},
		.handler = keyboard_action,
	};

	if (x86_native_irq_dispatch_initialize(
		    &unconstructed, small_lines, ARRAY_SIZE(small_lines),
		    small_actions, ARRAY_SIZE(small_actions)) !=
	    X86_NATIVE_IRQ_INVALID_STATE)
		return 1;
	x86_native_irq_dispatch_construct(dispatch);
	if (x86_native_irq_dispatch_initialize(
		    dispatch, small_lines, ARRAY_SIZE(small_lines), small_actions,
		    ARRAY_SIZE(small_actions)) != X86_NATIVE_IRQ_OK ||
	    x86_native_irq_dispatch_prepare(
		    dispatch, &dispatch_config, line_config,
		    ARRAY_SIZE(line_config)) != X86_NATIVE_IRQ_OK ||
	    x86_native_irq_action_register(dispatch, &pit_config,
					   pit_binding) != X86_NATIVE_IRQ_OK ||
	    x86_native_irq_action_register(dispatch, &key_config,
					   key_binding) != X86_NATIVE_IRQ_OK)
		return 2;
	if (x86_native_irq_dispatch_replace_storage(
		    dispatch, grown_lines, ARRAY_SIZE(grown_lines), grown_actions,
		    ARRAY_SIZE(grown_actions)) != X86_NATIVE_IRQ_OK ||
	    x86_native_irq_dispatch_publish(dispatch) != X86_NATIVE_IRQ_OK ||
	    controller_state.active != 1u ||
	    controller_state.resume_count != 1u)
		return 3;
	return 0;
}

static int test_routing_paths(
	struct x86_guest_irq_router *router,
	const struct x86_guest_irq_producer_binding *pit,
	const struct x86_guest_irq_producer_binding *input,
	struct x86_native_irq_dispatch *dispatch)
{
	const struct x86_guest_irq_producer_config forbidden_active_config = {
		.identity = OTHER_PRODUCER_ID,
		.capabilities = X86_GUEST_IRQ_PRODUCER_IRQ_EDGE,
		.allowed_guest_irqs = 1u << 3,
		.reserved = {0u},
	};
	struct x86_guest_irq_producer_binding ignored_binding;
	struct x86_legacy_irq_event native_irq1 = {
		.pit_input_ticks = 0u,
		.source_identity = PIT_PRODUCER_ID,
		.kind = X86_LEGACY_IRQ_EVENT_IRQ_EDGE,
		.irq = 1u,
		.pit_rate_calibrated = 0u,
		.reserved = {0u},
	};
	const struct x86_guest_irq_event guest_irq1 = {
		.pit_input_ticks = 0u,
		.kind = X86_GUEST_IRQ_EVENT_IRQ_EDGE,
		.irq = 1u,
		.reserved = {0u},
	};
	const struct x86_guest_irq_event forbidden_irq2 = {
		.pit_input_ticks = 0u,
		.kind = X86_GUEST_IRQ_EVENT_IRQ_EDGE,
		.irq = 2u,
		.reserved = {0u},
	};
	struct x86_guest_irq_router_snapshot guest_snapshot;
	struct x86_native_irq_dispatch_snapshot native_snapshot;
	uint64_t before;

	if (x86_guest_irq_producer_register(
		    router, &forbidden_active_config, &ignored_binding) !=
	    X86_GUEST_IRQ_ROUTER_INVALID_STATE)
		return 1;
	if (x86_guest_irq_router_snapshot(router, &guest_snapshot) !=
		    X86_GUEST_IRQ_ROUTER_BUSY ||
	    x86_native_irq_dispatch_snapshot(dispatch, &native_snapshot) !=
		    X86_NATIVE_IRQ_BUSY)
		return 8;
	before = sink_state.submit_count;
	if (x86_guest_irq_route_native(router, pit, &native_irq1) !=
		    X86_GUEST_IRQ_ROUTER_NOT_MAPPED ||
	    sink_state.submit_count != before)
		return 2;
	if (x86_native_irq_dispatch_vector(dispatch, 0x20u) !=
		    X86_NATIVE_IRQ_OK ||
	    pit_action_count != 1u || sink_state.submit_count != before + 1u ||
	    sink_state.last_event.kind !=
		    X86_LEGACY_CHIPSET_EVENT_PIT_CLOCK ||
	    sink_state.last_event.irq != 0u)
		return 3;
	before = sink_state.submit_count;
	if (x86_native_irq_dispatch_vector(dispatch, 0x21u) !=
		    X86_NATIVE_IRQ_OK ||
	    keyboard_action_count != 1u || sink_state.submit_count != before)
		return 4;
	/* Guest IRQ1 is emitted by the guest-i8042 producer, never native IRQ1. */
	if (x86_guest_irq_submit(router, input, &guest_irq1) !=
		    X86_GUEST_IRQ_ROUTER_OK ||
	    sink_state.submit_count != before + 1u ||
	    sink_state.last_event.kind != X86_LEGACY_CHIPSET_EVENT_IRQ_EDGE ||
	    sink_state.last_event.irq != 1u ||
	    x86_guest_irq_submit(router, input, &forbidden_irq2) !=
		    X86_GUEST_IRQ_ROUTER_ACCESS_DENIED)
		return 5;
	before = sink_state.submit_count;
	if (x86_native_irq_dispatch_vector(dispatch, 0x27u) !=
		    X86_NATIVE_IRQ_SPURIOUS ||
	    controller_state.last_completion !=
		    X86_NATIVE_IRQ_COMPLETE_SPURIOUS ||
	    x86_native_irq_dispatch_vector(dispatch, 0x2fu) !=
		    X86_NATIVE_IRQ_SPURIOUS ||
	    sink_state.submit_count != before ||
	    x86_native_irq_dispatch_vector(dispatch, 0x30u) !=
		    X86_NATIVE_IRQ_NOT_MAPPED)
		return 6;
	sink_state.reject_next_submit = 1u;
	if (x86_guest_irq_submit(router, input, &guest_irq1) !=
		    X86_GUEST_IRQ_ROUTER_SINK_REJECTED ||
	    x86_guest_irq_submit(router, input, &guest_irq1) !=
		    X86_GUEST_IRQ_ROUTER_OK)
		return 7;
	return 0;
}

static int test_quiesced_growth_and_stale_actions(
	struct x86_native_irq_dispatch *dispatch,
	const struct x86_native_irq_action_binding *pit_binding,
	struct x86_native_irq_action_binding *key_binding)
{
	static struct x86_native_irq_line_slot larger_lines[12];
	static struct x86_native_irq_action_slot larger_actions[8];
	const struct x86_native_irq_action_config key_config = {
		.identity = KEY_ACTION_ID,
		.context = ACTION_CONTEXT,
		.hardware_irq = 1u,
		.shared = 0u,
		.reserved = {0u},
		.handler = keyboard_action,
	};
	struct x86_native_irq_action_binding replacement;
	struct x86_native_irq_dispatch_snapshot snapshot;

	if (x86_native_irq_dispatch_replace_storage(
		    dispatch, larger_lines, ARRAY_SIZE(larger_lines),
		    larger_actions, ARRAY_SIZE(larger_actions)) !=
	    X86_NATIVE_IRQ_INVALID_STATE)
		return 1;
	if (x86_native_irq_action_quiesce(dispatch, key_binding) !=
		    X86_NATIVE_IRQ_INVALID_STATE ||
	    x86_native_irq_dispatch_quiesce(dispatch, DISPATCH_ID) !=
		    X86_NATIVE_IRQ_OK ||
	    controller_state.active != 0u)
		return 2;
	if (x86_native_irq_dispatch_snapshot(dispatch, &snapshot) !=
		    X86_NATIVE_IRQ_OK ||
	    snapshot.handled_count != 2u || snapshot.spurious_count != 2u ||
	    snapshot.unhandled_count != 0u || snapshot.fault_count != 0u)
		return 7;
	if (x86_native_irq_dispatch_replace_storage(
		    dispatch, larger_lines, ARRAY_SIZE(larger_lines),
		    larger_actions, ARRAY_SIZE(larger_actions)) != X86_NATIVE_IRQ_OK)
		return 3;
	if (x86_native_irq_action_quiesce(dispatch, key_binding) !=
		    X86_NATIVE_IRQ_OK ||
	    x86_native_irq_action_unregister(dispatch, key_binding) !=
		    X86_NATIVE_IRQ_OK ||
	    x86_native_irq_action_register(dispatch, &key_config,
					   &replacement) != X86_NATIVE_IRQ_OK ||
	    x86_native_irq_action_quiesce(dispatch, key_binding) !=
		    X86_NATIVE_IRQ_STALE_BINDING)
		return 4;
	*key_binding = replacement;
	if (x86_native_irq_dispatch_resume(dispatch, DISPATCH_ID) !=
		    X86_NATIVE_IRQ_OK ||
	    x86_native_irq_dispatch_vector(dispatch, 0x21u) !=
		    X86_NATIVE_IRQ_OK ||
	    keyboard_action_count != 2u ||
	    x86_native_irq_dispatch_quiesce(dispatch, DISPATCH_ID) !=
		    X86_NATIVE_IRQ_OK)
		return 5;
	if (x86_native_irq_action_quiesce(dispatch, pit_binding) !=
		    X86_NATIVE_IRQ_OK ||
	    x86_native_irq_action_unregister(dispatch, pit_binding) !=
		    X86_NATIVE_IRQ_OK ||
	    x86_native_irq_action_quiesce(dispatch, key_binding) !=
		    X86_NATIVE_IRQ_OK ||
	    x86_native_irq_action_unregister(dispatch, key_binding) !=
		    X86_NATIVE_IRQ_OK ||
	    x86_native_irq_dispatch_retire(dispatch, DISPATCH_ID) !=
		    X86_NATIVE_IRQ_OK)
		return 6;
	return 0;
}

static int teardown_guest_router(
	struct x86_guest_irq_router *router,
	struct x86_guest_irq_producer_binding *pit,
	const struct x86_guest_irq_producer_binding *input,
	const struct x86_guest_irq_route_binding *pit_route)
{
	static struct x86_guest_irq_producer_slot larger_producers[8];
	static struct x86_guest_irq_route_slot larger_routes[8];
	const struct x86_guest_irq_producer_config pit_config = {
		.identity = PIT_PRODUCER_ID,
		.capabilities = X86_GUEST_IRQ_PRODUCER_PIT_CLOCK,
		.allowed_guest_irqs = 1u << 0,
		.reserved = {0u},
	};
	struct x86_guest_irq_producer_binding replacement;
	struct x86_guest_irq_producer_binding stale = *pit;
	const struct x86_guest_irq_event pit_event = {
		.pit_input_ticks = 1193u,
		.kind = X86_GUEST_IRQ_EVENT_PIT_CLOCK,
		.irq = 0u,
		.reserved = {0u},
	};

	if (x86_guest_irq_router_replace_storage(
		    router, larger_producers, ARRAY_SIZE(larger_producers),
		    larger_routes, ARRAY_SIZE(larger_routes)) !=
	    X86_GUEST_IRQ_ROUTER_INVALID_STATE ||
	    x86_guest_irq_router_quiesce(router, ROUTER_ID) !=
		    X86_GUEST_IRQ_ROUTER_OK ||
	    sink_state.quiesced == 0u)
		return 1;
	if (x86_guest_irq_router_replace_storage(
		    router, larger_producers, ARRAY_SIZE(larger_producers),
		    larger_routes, ARRAY_SIZE(larger_routes)) !=
	    X86_GUEST_IRQ_ROUTER_OK)
		return 2;
	if (x86_guest_irq_router_resume(router, ROUTER_ID) !=
		    X86_GUEST_IRQ_ROUTER_OK ||
	    x86_guest_irq_submit(router, input,
			 &(const struct x86_guest_irq_event){
				 .kind = X86_GUEST_IRQ_EVENT_IRQ_EDGE,
				 .irq = 12u,
				 .reserved = {0u},
			 }) != X86_GUEST_IRQ_ROUTER_OK ||
	    x86_guest_irq_router_quiesce(router, ROUTER_ID) !=
		    X86_GUEST_IRQ_ROUTER_OK)
		return 3;
	if (x86_guest_irq_native_route_uninstall(router, pit, pit_route) !=
		    X86_GUEST_IRQ_ROUTER_OK ||
	    x86_guest_irq_native_route_uninstall(router, pit, pit_route) !=
		    X86_GUEST_IRQ_ROUTER_STALE_BINDING ||
	    x86_guest_irq_producer_quiesce(router, pit) !=
		    X86_GUEST_IRQ_ROUTER_OK ||
	    x86_guest_irq_producer_unregister(router, pit) !=
		    X86_GUEST_IRQ_ROUTER_OK ||
	    x86_guest_irq_producer_register(router, &pit_config, &replacement) !=
		    X86_GUEST_IRQ_ROUTER_OK ||
	    x86_guest_irq_submit(router, &stale, &pit_event) !=
		    X86_GUEST_IRQ_ROUTER_STALE_BINDING)
		return 4;
	*pit = replacement;
	if (x86_guest_irq_producer_quiesce(router, pit) !=
		    X86_GUEST_IRQ_ROUTER_OK ||
	    x86_guest_irq_producer_unregister(router, pit) !=
		    X86_GUEST_IRQ_ROUTER_OK ||
	    x86_guest_irq_producer_quiesce(router, input) !=
		    X86_GUEST_IRQ_ROUTER_OK ||
	    x86_guest_irq_producer_unregister(router, input) !=
		    X86_GUEST_IRQ_ROUTER_OK ||
	    x86_guest_irq_router_retire(router, ROUTER_ID) !=
		    X86_GUEST_IRQ_ROUTER_OK ||
	    sink_state.unbind_count != 1u || sink_state.bound != 0u)
		return 5;
	return 0;
}

static int run_tests(void)
{
	struct x86_guest_irq_router router;
	struct x86_guest_irq_producer_binding pit_producer;
	struct x86_guest_irq_producer_binding input_producer;
	struct x86_guest_irq_route_binding pit_route;
	struct x86_native_irq_dispatch dispatch;
	struct x86_native_irq_action_binding pit_binding;
	struct x86_native_irq_action_binding key_binding;
	struct x86_native_irq_dispatch_snapshot dispatch_snapshot;
	struct x86_guest_irq_router_snapshot router_snapshot;
	int status;

	status = prepare_guest_router(&router, &pit_producer, &input_producer,
				      &pit_route);
	if (status != 0)
		return 10 + status;
	action_router = &router;
	action_pit_producer = pit_producer;
	status = prepare_native_dispatch(&dispatch, &pit_binding, &key_binding);
	if (status != 0)
		return 20 + status;
	status = test_routing_paths(&router, &pit_producer, &input_producer,
				    &dispatch);
	if (status != 0)
		return 30 + status;
	status = test_quiesced_growth_and_stale_actions(
		&dispatch, &pit_binding, &key_binding);
	if (status != 0)
		return 40 + status;
	if (x86_native_irq_dispatch_snapshot(&dispatch, &dispatch_snapshot) !=
		    X86_NATIVE_IRQ_OK ||
	    dispatch_snapshot.phase != X86_NATIVE_IRQ_DISPATCH_EMPTY ||
	    dispatch_snapshot.spurious_count != 0u)
		return 50;
	status = teardown_guest_router(&router, &pit_producer,
				       &input_producer, &pit_route);
	if (status != 0)
		return 60 + status;
	if (x86_guest_irq_router_snapshot(&router, &router_snapshot) !=
		    X86_GUEST_IRQ_ROUTER_OK ||
	    router_snapshot.phase != X86_GUEST_IRQ_ROUTER_EMPTY)
		return 70;
	return 0;
}

DOSC32_TEST_ENTRY(run_tests)

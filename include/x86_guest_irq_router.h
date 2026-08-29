/* SPDX-License-Identifier: GPL-2.0-only */
/* Generation-bound many-producer routing into one guest interrupt sink. */
#ifndef DOSC32_X86_GUEST_IRQ_ROUTER_H
#define DOSC32_X86_GUEST_IRQ_ROUTER_H

#include "x86_legacy_chipset.h"
#include "x86_legacy_irq.h"

#define X86_GUEST_IRQ_PRODUCER_PIT_CLOCK (1u << 0)
#define X86_GUEST_IRQ_PRODUCER_IRQ_EDGE (1u << 1)
#define X86_GUEST_IRQ_PRODUCER_CAPABILITIES                         \
	(X86_GUEST_IRQ_PRODUCER_PIT_CLOCK | X86_GUEST_IRQ_PRODUCER_IRQ_EDGE)

enum x86_guest_irq_router_status {
	X86_GUEST_IRQ_ROUTER_OK = 0,
	X86_GUEST_IRQ_ROUTER_INVALID_ARGUMENT,
	X86_GUEST_IRQ_ROUTER_INVALID_STATE,
	X86_GUEST_IRQ_ROUTER_CAPACITY_EXHAUSTED,
	X86_GUEST_IRQ_ROUTER_IDENTITY_MISMATCH,
	X86_GUEST_IRQ_ROUTER_STALE_BINDING,
	X86_GUEST_IRQ_ROUTER_ALREADY_REGISTERED,
	X86_GUEST_IRQ_ROUTER_NOT_MAPPED,
	X86_GUEST_IRQ_ROUTER_ACCESS_DENIED,
	X86_GUEST_IRQ_ROUTER_BUSY,
	X86_GUEST_IRQ_ROUTER_SINK_REJECTED,
	X86_GUEST_IRQ_ROUTER_POISONED
};

enum x86_guest_irq_router_phase {
	X86_GUEST_IRQ_ROUTER_UNINITIALIZED = 0,
	X86_GUEST_IRQ_ROUTER_EMPTY,
	X86_GUEST_IRQ_ROUTER_PREPARED,
	X86_GUEST_IRQ_ROUTER_ACTIVE,
	X86_GUEST_IRQ_ROUTER_QUIESCED,
	X86_GUEST_IRQ_ROUTER_POISONED_PHASE
};

enum x86_guest_irq_event_kind {
	X86_GUEST_IRQ_EVENT_PIT_CLOCK = 1,
	X86_GUEST_IRQ_EVENT_IRQ_EDGE
};

enum x86_guest_irq_sink_result {
	X86_GUEST_IRQ_SINK_OK = 0,
	X86_GUEST_IRQ_SINK_REJECTED,
	X86_GUEST_IRQ_SINK_POISONED
};

struct x86_guest_irq_event {
	uint64_t pit_input_ticks;
	uint8_t kind;
	uint8_t irq;
	uint8_t reserved[6];
} __aligned(8);

typedef enum x86_guest_irq_sink_result (*x86_guest_irq_sink_bind_fn)(
	kernel_object_handle_t sink_context_identity,
	kernel_object_handle_t router_identity,
	const struct x86_legacy_chipset_source_config *config);
typedef enum x86_guest_irq_sink_result (*x86_guest_irq_sink_submit_fn)(
	kernel_object_handle_t sink_context_identity,
	kernel_object_handle_t router_identity,
	const struct x86_legacy_chipset_source_event *event);
typedef enum x86_guest_irq_sink_result (*x86_guest_irq_sink_lifecycle_fn)(
	kernel_object_handle_t sink_context_identity,
	kernel_object_handle_t router_identity);

struct x86_guest_irq_sink_ops {
	x86_guest_irq_sink_bind_fn bind;
	x86_guest_irq_sink_submit_fn submit;
	x86_guest_irq_sink_lifecycle_fn quiesce;
	x86_guest_irq_sink_lifecycle_fn resume;
	x86_guest_irq_sink_lifecycle_fn unbind;
};

struct x86_guest_irq_router_config {
	kernel_object_handle_t identity;
	kernel_object_handle_t sink_context_identity;
	struct x86_guest_irq_sink_ops sink;
	uint64_t pit_input_quantum;
	uint32_t capabilities;
	uint8_t pit_rate_calibrated;
	uint8_t reserved[3];
};

struct x86_guest_irq_producer_config {
	kernel_object_handle_t identity;
	uint32_t capabilities;
	uint16_t allowed_guest_irqs;
	uint8_t reserved[2];
} __aligned(8);

struct x86_guest_irq_producer_binding {
	kernel_object_handle_t router_identity;
	uint64_t router_generation;
	kernel_object_handle_t producer_identity;
	uint64_t producer_generation;
	uint32_t slot;
	uint8_t reserved[4];
} __aligned(8);

struct x86_guest_irq_native_route_config {
	uint8_t native_kind;
	uint8_t native_irq;
	uint8_t guest_kind;
	uint8_t guest_irq;
	uint8_t reserved[4];
};

struct x86_guest_irq_route_binding {
	kernel_object_handle_t router_identity;
	uint64_t router_generation;
	uint64_t route_generation;
	uint64_t producer_generation;
	uint32_t route_slot;
	uint32_t producer_slot;
} __aligned(8);

/* Caller-owned storage. It may be static, boot-allocated, or heap-backed. */
struct x86_guest_irq_producer_slot {
	kernel_object_handle_t identity;
	uint64_t generation;
	uint64_t submitted_count;
	uint64_t rejected_count;
	uint64_t sink_failure_count;
	uint16_t allowed_guest_irqs;
	uint8_t capabilities;
	uint8_t phase;
	uint8_t in_flight;
	uint8_t reserved[3];
} __aligned(8);

struct x86_guest_irq_route_slot {
	uint64_t generation;
	uint64_t producer_generation;
	uint32_t producer_slot;
	uint8_t native_kind;
	uint8_t native_irq;
	uint8_t guest_kind;
	uint8_t guest_irq;
	uint8_t active;
	uint8_t reserved[3];
} __aligned(8);

struct x86_guest_irq_router {
	kernel_object_handle_t identity;
	kernel_object_handle_t sink_context_identity;
	uint64_t generation;
	uint64_t submitted_count;
	uint64_t unmapped_count;
	uint64_t rejected_count;
	uint64_t sink_failure_count;
	struct x86_guest_irq_sink_ops sink;
	struct x86_guest_irq_producer_slot *producers;
	struct x86_guest_irq_route_slot *routes;
	uint64_t pit_input_quantum;
	uint32_t producer_capacity;
	uint32_t route_capacity;
	uint32_t producer_count;
	uint32_t route_count;
	uint32_t capabilities;
	uint32_t lifecycle_cookie;
	uint8_t pit_rate_calibrated;
	uint8_t phase;
	uint8_t dispatch_active;
	uint8_t initialized;
	uint8_t reserved[4];
} __aligned(8);

struct x86_guest_irq_router_snapshot {
	kernel_object_handle_t identity;
	kernel_object_handle_t sink_context_identity;
	uint64_t generation;
	uint64_t submitted_count;
	uint64_t unmapped_count;
	uint64_t rejected_count;
	uint64_t sink_failure_count;
	uint64_t pit_input_quantum;
	uint32_t producer_capacity;
	uint32_t route_capacity;
	uint32_t producer_count;
	uint32_t route_count;
	uint32_t capabilities;
	uint8_t pit_rate_calibrated;
	uint8_t phase;
	uint8_t dispatch_active;
	uint8_t reserved;
} __aligned(8);

/* Construct writes a valid empty C object without inspecting prior bytes. */
void x86_guest_irq_router_construct(struct x86_guest_irq_router *router);
/* Initialize only a constructed object; supplied storage may be heap-backed. */
enum x86_guest_irq_router_status x86_guest_irq_router_initialize(
	struct x86_guest_irq_router *router,
	struct x86_guest_irq_producer_slot *producers,
	uint32_t producer_capacity,
	struct x86_guest_irq_route_slot *routes,
	uint32_t route_capacity) __must_check;
enum x86_guest_irq_router_status x86_guest_irq_router_replace_storage(
	struct x86_guest_irq_router *router,
	struct x86_guest_irq_producer_slot *producers,
	uint32_t producer_capacity,
	struct x86_guest_irq_route_slot *routes,
	uint32_t route_capacity) __must_check;
enum x86_guest_irq_router_status x86_guest_irq_router_prepare(
	struct x86_guest_irq_router *router,
	const struct x86_guest_irq_router_config *config) __must_check;
enum x86_guest_irq_router_status x86_guest_irq_router_publish(
	struct x86_guest_irq_router *router) __must_check;
enum x86_guest_irq_router_status x86_guest_irq_router_abort(
	struct x86_guest_irq_router *router) __must_check;
enum x86_guest_irq_router_status x86_guest_irq_router_quiesce(
	struct x86_guest_irq_router *router,
	kernel_object_handle_t identity) __must_check;
enum x86_guest_irq_router_status x86_guest_irq_router_resume(
	struct x86_guest_irq_router *router,
	kernel_object_handle_t identity) __must_check;
enum x86_guest_irq_router_status x86_guest_irq_router_retire(
	struct x86_guest_irq_router *router,
	kernel_object_handle_t identity) __must_check;
enum x86_guest_irq_router_status x86_guest_irq_router_poison(
	struct x86_guest_irq_router *router,
	kernel_object_handle_t identity) __must_check;

enum x86_guest_irq_router_status x86_guest_irq_producer_register(
	struct x86_guest_irq_router *router,
	const struct x86_guest_irq_producer_config *config,
	struct x86_guest_irq_producer_binding *binding) __must_check;
enum x86_guest_irq_router_status x86_guest_irq_producer_quiesce(
	struct x86_guest_irq_router *router,
	const struct x86_guest_irq_producer_binding *binding) __must_check;
enum x86_guest_irq_router_status x86_guest_irq_producer_unregister(
	struct x86_guest_irq_router *router,
	const struct x86_guest_irq_producer_binding *binding) __must_check;

enum x86_guest_irq_router_status x86_guest_irq_native_route_install(
	struct x86_guest_irq_router *router,
	const struct x86_guest_irq_producer_binding *producer,
	const struct x86_guest_irq_native_route_config *config,
	struct x86_guest_irq_route_binding *route) __must_check;
enum x86_guest_irq_router_status x86_guest_irq_native_route_uninstall(
	struct x86_guest_irq_router *router,
	const struct x86_guest_irq_producer_binding *producer,
	const struct x86_guest_irq_route_binding *route) __must_check;

/* IRQ-safe synchronous paths: neither function allocates nor blocks. */
enum x86_guest_irq_router_status x86_guest_irq_submit(
	struct x86_guest_irq_router *router,
	const struct x86_guest_irq_producer_binding *producer,
	const struct x86_guest_irq_event *event) __must_check;
enum x86_guest_irq_router_status x86_guest_irq_route_native(
	struct x86_guest_irq_router *router,
	const struct x86_guest_irq_producer_binding *producer,
	const struct x86_legacy_irq_event *event) __must_check;

enum x86_guest_irq_router_status x86_guest_irq_router_snapshot(
	const struct x86_guest_irq_router *router,
	struct x86_guest_irq_router_snapshot *snapshot) __must_check;

static_assert_expression(sizeof(struct x86_guest_irq_event) == 16u,
			 "guest IRQ event layout changed");
static_assert_expression(sizeof(struct x86_guest_irq_producer_config) == 16u,
			 "guest IRQ producer config layout changed");
static_assert_expression(sizeof(struct x86_guest_irq_producer_binding) == 40u,
			 "guest IRQ producer binding layout changed");
static_assert_expression(sizeof(struct x86_guest_irq_native_route_config) == 8u,
			 "guest IRQ native route config layout changed");
static_assert_expression(sizeof(struct x86_guest_irq_route_binding) == 40u,
			 "guest IRQ route binding layout changed");

#endif

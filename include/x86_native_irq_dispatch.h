/* SPDX-License-Identifier: GPL-2.0-only */
/* Controller-neutral native x86 IRQ descriptors and registered actions. */
#ifndef DOSC32_X86_NATIVE_IRQ_DISPATCH_H
#define DOSC32_X86_NATIVE_IRQ_DISPATCH_H

#include "object_identity.h"

#define X86_NATIVE_IRQ_LINE_LEVEL (1u << 0)
#define X86_NATIVE_IRQ_LINE_SHAREABLE (1u << 1)
#define X86_NATIVE_IRQ_LINE_FLAG_MASK \
	(X86_NATIVE_IRQ_LINE_LEVEL | X86_NATIVE_IRQ_LINE_SHAREABLE)

enum x86_native_irq_status {
	X86_NATIVE_IRQ_OK = 0,
	X86_NATIVE_IRQ_INVALID_ARGUMENT,
	X86_NATIVE_IRQ_INVALID_STATE,
	X86_NATIVE_IRQ_CAPACITY_EXHAUSTED,
	X86_NATIVE_IRQ_IDENTITY_MISMATCH,
	X86_NATIVE_IRQ_STALE_BINDING,
	X86_NATIVE_IRQ_ALREADY_REGISTERED,
	X86_NATIVE_IRQ_NOT_MAPPED,
	X86_NATIVE_IRQ_BUSY,
	X86_NATIVE_IRQ_SPURIOUS,
	X86_NATIVE_IRQ_UNHANDLED,
	X86_NATIVE_IRQ_HANDLER_FAULT,
	X86_NATIVE_IRQ_CONTROLLER_REJECTED,
	X86_NATIVE_IRQ_POISONED
};

enum x86_native_irq_dispatch_phase {
	X86_NATIVE_IRQ_DISPATCH_UNINITIALIZED = 0,
	X86_NATIVE_IRQ_DISPATCH_EMPTY,
	X86_NATIVE_IRQ_DISPATCH_PREPARED,
	X86_NATIVE_IRQ_DISPATCH_ACTIVE,
	X86_NATIVE_IRQ_DISPATCH_QUIESCED,
	X86_NATIVE_IRQ_DISPATCH_POISONED_PHASE
};

enum x86_native_irq_controller_result {
	X86_NATIVE_IRQ_CONTROLLER_RESULT_OK = 0,
	X86_NATIVE_IRQ_CONTROLLER_RESULT_REJECTED,
	X86_NATIVE_IRQ_CONTROLLER_RESULT_POISONED
};

enum x86_native_irq_observation_kind {
	X86_NATIVE_IRQ_OBSERVATION_DELIVER = 1,
	X86_NATIVE_IRQ_OBSERVATION_SPURIOUS
};

enum x86_native_irq_action_result {
	X86_NATIVE_IRQ_ACTION_HANDLED = 0,
	X86_NATIVE_IRQ_ACTION_UNHANDLED,
	X86_NATIVE_IRQ_ACTION_FAULT
};

enum x86_native_irq_completion {
	X86_NATIVE_IRQ_COMPLETE_HANDLED = 1,
	X86_NATIVE_IRQ_COMPLETE_UNHANDLED,
	X86_NATIVE_IRQ_COMPLETE_HANDLER_FAULT,
	X86_NATIVE_IRQ_COMPLETE_SPURIOUS
};

struct x86_native_irq_observation {
	uint64_t controller_cookie;
	uint8_t kind;
	uint8_t reserved[7];
} __aligned(8);

struct x86_native_irq_event {
	kernel_object_handle_t controller_identity;
	uint64_t controller_generation;
	uint64_t sequence;
	uint32_t vector;
	uint32_t hardware_irq;
	uint32_t line_flags;
	uint8_t reserved[4];
} __aligned(8);

typedef enum x86_native_irq_controller_result
(*x86_native_irq_controller_begin_fn)(
	kernel_object_handle_t controller_context,
	const struct x86_native_irq_event *event,
	struct x86_native_irq_observation *observation);
typedef enum x86_native_irq_controller_result
(*x86_native_irq_controller_end_fn)(
	kernel_object_handle_t controller_context,
	const struct x86_native_irq_event *event,
	const struct x86_native_irq_observation *observation,
	enum x86_native_irq_completion completion);
typedef enum x86_native_irq_controller_result
(*x86_native_irq_controller_lifecycle_fn)(
	kernel_object_handle_t controller_context,
	kernel_object_handle_t dispatch_identity);
typedef enum x86_native_irq_action_result (*x86_native_irq_action_fn)(
	kernel_object_handle_t action_context,
	const struct x86_native_irq_event *event);

struct x86_native_irq_controller_ops {
	x86_native_irq_controller_begin_fn begin;
	x86_native_irq_controller_end_fn end;
	x86_native_irq_controller_lifecycle_fn quiesce;
	x86_native_irq_controller_lifecycle_fn resume;
};

struct x86_native_irq_line_config {
	uint32_t vector;
	uint32_t hardware_irq;
	uint32_t flags;
	uint8_t reserved[4];
} __aligned(8);

struct x86_native_irq_dispatch_config {
	kernel_object_handle_t identity;
	kernel_object_handle_t controller_identity;
	kernel_object_handle_t controller_context;
	struct x86_native_irq_controller_ops controller;
};

struct x86_native_irq_action_config {
	kernel_object_handle_t identity;
	kernel_object_handle_t context;
	uint32_t hardware_irq;
	uint8_t shared;
	uint8_t reserved[3];
	x86_native_irq_action_fn handler;
};

struct x86_native_irq_action_binding {
	kernel_object_handle_t dispatch_identity;
	uint64_t dispatch_generation;
	kernel_object_handle_t action_identity;
	uint64_t action_generation;
	uint32_t slot;
	uint8_t reserved[4];
} __aligned(8);

/* Caller-owned arrays may be obtained from the post-boot kernel heap. */
struct x86_native_irq_line_slot {
	uint32_t vector;
	uint32_t hardware_irq;
	uint32_t flags;
	uint32_t first_action;
	uint32_t action_count;
	uint8_t active;
	uint8_t reserved[3];
} __aligned(8);

struct x86_native_irq_action_slot {
	kernel_object_handle_t identity;
	kernel_object_handle_t context;
	uint64_t generation;
	uint64_t handled_count;
	uint64_t unhandled_count;
	uint64_t fault_count;
	x86_native_irq_action_fn handler;
	uint32_t hardware_irq;
	uint32_t next_action;
	uint8_t shared;
	uint8_t active;
	uint8_t accepting;
	uint8_t in_flight;
	uint8_t reserved[4];
} __aligned(8);

struct x86_native_irq_dispatch {
	kernel_object_handle_t identity;
	kernel_object_handle_t controller_identity;
	kernel_object_handle_t controller_context;
	uint64_t generation;
	uint64_t next_sequence;
	uint64_t handled_count;
	uint64_t unhandled_count;
	uint64_t spurious_count;
	uint64_t fault_count;
	struct x86_native_irq_controller_ops controller;
	struct x86_native_irq_line_slot *lines;
	struct x86_native_irq_action_slot *actions;
	uint32_t line_capacity;
	uint32_t action_capacity;
	uint32_t line_count;
	uint32_t action_count;
	uint32_t lifecycle_cookie;
	uint8_t phase;
	uint8_t dispatch_active;
	uint8_t initialized;
	uint8_t reserved[5];
} __aligned(8);

struct x86_native_irq_dispatch_snapshot {
	kernel_object_handle_t identity;
	kernel_object_handle_t controller_identity;
	uint64_t generation;
	uint64_t next_sequence;
	uint64_t handled_count;
	uint64_t unhandled_count;
	uint64_t spurious_count;
	uint64_t fault_count;
	uint32_t line_capacity;
	uint32_t action_capacity;
	uint32_t line_count;
	uint32_t action_count;
	uint8_t phase;
	uint8_t dispatch_active;
	uint8_t reserved[6];
} __aligned(8);

void x86_native_irq_dispatch_construct(
	struct x86_native_irq_dispatch *dispatch);
enum x86_native_irq_status x86_native_irq_dispatch_initialize(
	struct x86_native_irq_dispatch *dispatch,
	struct x86_native_irq_line_slot *lines, uint32_t line_capacity,
	struct x86_native_irq_action_slot *actions,
	uint32_t action_capacity) __must_check;
enum x86_native_irq_status x86_native_irq_dispatch_replace_storage(
	struct x86_native_irq_dispatch *dispatch,
	struct x86_native_irq_line_slot *lines, uint32_t line_capacity,
	struct x86_native_irq_action_slot *actions,
	uint32_t action_capacity) __must_check;
enum x86_native_irq_status x86_native_irq_dispatch_prepare(
	struct x86_native_irq_dispatch *dispatch,
	const struct x86_native_irq_dispatch_config *config,
	const struct x86_native_irq_line_config *lines,
	uint32_t line_count) __must_check;
enum x86_native_irq_status x86_native_irq_dispatch_publish(
	struct x86_native_irq_dispatch *dispatch) __must_check;
enum x86_native_irq_status x86_native_irq_dispatch_abort(
	struct x86_native_irq_dispatch *dispatch) __must_check;
enum x86_native_irq_status x86_native_irq_dispatch_quiesce(
	struct x86_native_irq_dispatch *dispatch,
	kernel_object_handle_t identity) __must_check;
enum x86_native_irq_status x86_native_irq_dispatch_resume(
	struct x86_native_irq_dispatch *dispatch,
	kernel_object_handle_t identity) __must_check;
enum x86_native_irq_status x86_native_irq_dispatch_retire(
	struct x86_native_irq_dispatch *dispatch,
	kernel_object_handle_t identity) __must_check;
enum x86_native_irq_status x86_native_irq_dispatch_poison(
	struct x86_native_irq_dispatch *dispatch,
	kernel_object_handle_t identity) __must_check;

enum x86_native_irq_status x86_native_irq_action_register(
	struct x86_native_irq_dispatch *dispatch,
	const struct x86_native_irq_action_config *config,
	struct x86_native_irq_action_binding *binding) __must_check;
enum x86_native_irq_status x86_native_irq_action_quiesce(
	struct x86_native_irq_dispatch *dispatch,
	const struct x86_native_irq_action_binding *binding) __must_check;
enum x86_native_irq_status x86_native_irq_action_unregister(
	struct x86_native_irq_dispatch *dispatch,
	const struct x86_native_irq_action_binding *binding) __must_check;

/* IRQ-safe: controller begin, registered actions, then controller end/EOI. */
enum x86_native_irq_status x86_native_irq_dispatch_vector(
	struct x86_native_irq_dispatch *dispatch, uint32_t vector) __must_check;
enum x86_native_irq_status x86_native_irq_dispatch_snapshot(
	const struct x86_native_irq_dispatch *dispatch,
	struct x86_native_irq_dispatch_snapshot *snapshot) __must_check;

static_assert_expression(sizeof(struct x86_native_irq_observation) == 16u,
			 "native IRQ observation layout changed");
static_assert_expression(sizeof(struct x86_native_irq_event) == 40u,
			 "native IRQ event layout changed");
static_assert_expression(sizeof(struct x86_native_irq_line_config) == 16u,
			 "native IRQ line config layout changed");
static_assert_expression(sizeof(struct x86_native_irq_action_binding) == 40u,
			 "native IRQ action binding layout changed");

#endif

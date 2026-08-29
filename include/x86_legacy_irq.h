/* SPDX-License-Identifier: GPL-2.0-only */
/* Runtime ownership of a selected native legacy PIC/PIT interrupt domain. */
#ifndef DOSC32_X86_LEGACY_IRQ_H
#define DOSC32_X86_LEGACY_IRQ_H

#include "x86_legacy_pic.h"

#define X86_LEGACY_IRQ_VECTOR_BASE 0x20u
#define X86_LEGACY_IRQ_COUNT X86_LEGACY_PIC_IRQ_COUNT
#define X86_LEGACY_TIMER_IRQ 0u
#define X86_LEGACY_KEYBOARD_IRQ 1u
#define X86_LEGACY_TIMER_VECTOR \
	(X86_LEGACY_IRQ_VECTOR_BASE + X86_LEGACY_TIMER_IRQ)
#define X86_LEGACY_KEYBOARD_VECTOR \
	(X86_LEGACY_IRQ_VECTOR_BASE + X86_LEGACY_KEYBOARD_IRQ)

#define X86_LEGACY_IRQ_SOURCE_PIT_CLOCK (1u << 0)

enum x86_legacy_irq_status {
	X86_LEGACY_IRQ_OK = 0,
	X86_LEGACY_IRQ_INVALID_ARGUMENT,
	X86_LEGACY_IRQ_INVALID_STATE,
	X86_LEGACY_IRQ_IDENTITY_MISMATCH,
	X86_LEGACY_IRQ_CAPACITY_EXHAUSTED,
	X86_LEGACY_IRQ_STALE_BINDING,
	X86_LEGACY_IRQ_NOT_MAPPED,
	X86_LEGACY_IRQ_BUSY,
	X86_LEGACY_IRQ_SPURIOUS,
	X86_LEGACY_IRQ_UNHANDLED,
	X86_LEGACY_IRQ_HANDLER_FAULT,
	X86_LEGACY_IRQ_CONTROLLER_REJECTED,
	X86_LEGACY_IRQ_POISONED
};

enum x86_legacy_irq_phase {
	X86_LEGACY_IRQ_EMPTY = 0,
	X86_LEGACY_IRQ_PREPARED,
	X86_LEGACY_IRQ_ACTIVE,
	X86_LEGACY_IRQ_QUIESCED,
	X86_LEGACY_IRQ_POISONED_PHASE
};

/* These are native producer event types, never an implicit vector mapping. */
enum x86_legacy_irq_event_kind {
	X86_LEGACY_IRQ_EVENT_PIT_CLOCK = 1,
	X86_LEGACY_IRQ_EVENT_IRQ_EDGE
};

struct x86_legacy_irq_config {
	kernel_object_handle_t source_identity;
	kernel_object_handle_t controller_identity;
	kernel_object_handle_t dispatch_identity;
	uint64_t pit_input_quantum;
	uint32_t vector_base;
	uint16_t present_irq_mask;
	uint16_t enabled_irq_mask;
	uint16_t pit_reload;
	uint8_t pit_rate_calibrated;
	uint8_t present;
	uint8_t presence_evidence;
	uint8_t reserved[5];
} __aligned(8);

struct x86_legacy_irq_source_info {
	kernel_object_handle_t source_identity;
	uint64_t generation;
	uint64_t pit_input_quantum;
	uint32_t capabilities;
	uint8_t pit_rate_calibrated;
	uint8_t phase;
	uint8_t reserved[2];
} __aligned(8);

struct x86_legacy_irq_event {
	uint64_t pit_input_ticks;
	kernel_object_handle_t source_identity;
	uint8_t kind;
	uint8_t irq;
	uint8_t pit_rate_calibrated;
	uint8_t reserved[5];
} __aligned(8);

struct x86_legacy_irq_snapshot {
	kernel_object_handle_t source_identity;
	kernel_object_handle_t controller_identity;
	kernel_object_handle_t dispatch_identity;
	uint64_t generation;
	uint64_t handled_count;
	uint64_t unhandled_count;
	uint64_t spurious_count;
	uint64_t fault_count;
	uint32_t vector_base;
	uint16_t present_irq_mask;
	uint16_t enabled_irq_mask;
	uint16_t line_count;
	uint16_t action_count;
	uint8_t phase;
	uint8_t reserved[7];
} __aligned(8);

/* Prepare programs a selected controller with every line still masked. */
enum x86_legacy_irq_status x86_legacy_irq_prepare(
	const struct x86_legacy_irq_config *config) __must_check;
enum x86_legacy_irq_status x86_legacy_irq_abort(
	kernel_object_handle_t source_identity) __must_check;
enum x86_legacy_irq_status x86_legacy_irq_publish(
	kernel_object_handle_t source_identity) __must_check;
enum x86_legacy_irq_status x86_legacy_irq_quiesce(
	kernel_object_handle_t source_identity) __must_check;
enum x86_legacy_irq_status x86_legacy_irq_resume(
	kernel_object_handle_t source_identity) __must_check;
enum x86_legacy_irq_status x86_legacy_irq_retire(
	kernel_object_handle_t source_identity) __must_check;

enum x86_legacy_irq_status x86_legacy_irq_action_register(
	kernel_object_handle_t source_identity,
	const struct x86_native_irq_action_config *config,
	struct x86_native_irq_action_binding *binding) __must_check;
enum x86_legacy_irq_status x86_legacy_irq_action_quiesce(
	kernel_object_handle_t source_identity,
	const struct x86_native_irq_action_binding *binding) __must_check;
enum x86_legacy_irq_status x86_legacy_irq_action_unregister(
	kernel_object_handle_t source_identity,
	const struct x86_native_irq_action_binding *binding) __must_check;

/* Hard-IRQ safe: controller begin, registered actions, controller end/EOI. */
enum x86_legacy_irq_status x86_legacy_irq_dispatch_vector(
	uint32_t vector) __must_check;
bool x86_legacy_irq_is_initialized(void) __must_check;
enum x86_legacy_irq_status x86_legacy_irq_source_info(
	kernel_object_handle_t source_identity,
	struct x86_legacy_irq_source_info *info) __must_check;
/* Active 64-bit counters are not sampled on i386; quiesce before snapshot. */
enum x86_legacy_irq_status x86_legacy_irq_snapshot(
	kernel_object_handle_t source_identity,
	struct x86_legacy_irq_snapshot *snapshot) __must_check;

static_assert_expression(sizeof(struct x86_legacy_irq_config) == 56u,
			 "native legacy IRQ config layout changed");
static_assert_expression(sizeof(struct x86_legacy_irq_source_info) == 32u,
			 "native IRQ source info layout changed");
static_assert_expression(sizeof(struct x86_legacy_irq_event) == 24u,
			 "native IRQ event layout changed");
static_assert_expression(sizeof(struct x86_legacy_irq_snapshot) == 88u,
			 "native legacy IRQ snapshot layout changed");

#endif

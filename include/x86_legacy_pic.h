/* SPDX-License-Identifier: GPL-2.0-only */
/* Native dual-8259/PIT platform backend for the generic IRQ dispatcher. */
#ifndef DOSC32_X86_LEGACY_PIC_H
#define DOSC32_X86_LEGACY_PIC_H

#include "x86_native_irq_dispatch.h"

#define X86_LEGACY_PIC_IRQ_COUNT 16u
#define X86_LEGACY_PIC_IRQ_MASK_ALL 0xffffu

enum x86_legacy_pic_status {
	X86_LEGACY_PIC_OK = 0,
	X86_LEGACY_PIC_INVALID_ARGUMENT,
	X86_LEGACY_PIC_INVALID_STATE,
	X86_LEGACY_PIC_CAPACITY_EXHAUSTED,
	X86_LEGACY_PIC_IDENTITY_MISMATCH,
	X86_LEGACY_PIC_UNAVAILABLE,
	X86_LEGACY_PIC_BUSY,
	X86_LEGACY_PIC_POISONED
};

enum x86_legacy_pic_phase {
	X86_LEGACY_PIC_EMPTY = 0,
	X86_LEGACY_PIC_PREPARED,
	X86_LEGACY_PIC_ACTIVE,
	X86_LEGACY_PIC_QUIESCED,
	X86_LEGACY_PIC_POISONED_PHASE
};

enum x86_legacy_pic_evidence {
	X86_LEGACY_PIC_EVIDENCE_NONE = 0,
	X86_LEGACY_PIC_EVIDENCE_PLATFORM_ASSIGNED,
	X86_LEGACY_PIC_EVIDENCE_FIRMWARE_REPORTED
};

struct x86_legacy_pic_config {
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

struct x86_legacy_pic_snapshot {
	kernel_object_handle_t controller_identity;
	kernel_object_handle_t dispatch_identity;
	uint64_t generation;
	uint64_t pit_input_quantum;
	uint32_t vector_base;
	uint16_t present_irq_mask;
	uint16_t enabled_irq_mask;
	uint16_t pit_reload;
	uint8_t pit_rate_calibrated;
	uint8_t presence_evidence;
	uint8_t phase;
	uint8_t reserved[3];
} __aligned(8);

/* Prepare initializes the selected controller with every line masked. */
enum x86_legacy_pic_status x86_legacy_pic_prepare(
	const struct x86_legacy_pic_config *config,
	struct x86_native_irq_line_config *lines,
	uint32_t line_capacity, uint32_t *line_count) __must_check;
enum x86_legacy_pic_status x86_legacy_pic_abort(
	kernel_object_handle_t controller_identity) __must_check;
enum x86_legacy_pic_status x86_legacy_pic_retire(
	kernel_object_handle_t controller_identity,
	kernel_object_handle_t dispatch_identity) __must_check;
enum x86_legacy_pic_status x86_legacy_pic_set_enabled_irqs(
	kernel_object_handle_t controller_identity,
	kernel_object_handle_t dispatch_identity,
	uint16_t enabled_irq_mask) __must_check;
enum x86_legacy_pic_status x86_legacy_pic_poison(
	kernel_object_handle_t controller_identity) __must_check;

/* Returned callbacks validate both controller and dispatcher identities. */
struct x86_native_irq_controller_ops
x86_legacy_pic_controller_ops(void) __must_check;
enum x86_legacy_pic_status x86_legacy_pic_snapshot(
	kernel_object_handle_t controller_identity,
	struct x86_legacy_pic_snapshot *snapshot) __must_check;

static_assert_expression(sizeof(struct x86_legacy_pic_config) == 48u,
			 "legacy PIC config layout changed");
static_assert_expression(sizeof(struct x86_legacy_pic_snapshot) == 48u,
			 "legacy PIC snapshot layout changed");

#endif

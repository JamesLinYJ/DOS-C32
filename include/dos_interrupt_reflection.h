/* SPDX-License-Identifier: GPL-2.0-only */
/* Transactional delivery of a real-mode interrupt through the guest IVT. */
#ifndef DOSC32_DOS_INTERRUPT_REFLECTION_H
#define DOSC32_DOS_INTERRUPT_REFLECTION_H

#include "compiler.h"
#include "dos_machine.h"
#include "types.h"

enum dos_interrupt_reflection_status {
	DOS_INTERRUPT_REFLECTION_OK = 0,
	DOS_INTERRUPT_REFLECTION_INVALID_ARGUMENT,
	DOS_INTERRUPT_REFLECTION_VECTOR_FAULT,
	DOS_INTERRUPT_REFLECTION_STACK_FAULT,
	DOS_INTERRUPT_REFLECTION_ROLLBACK_FAILED
};

#define DOS_INTERRUPT_REFLECTION_FRAME_BYTES 6u

/* Exact pre-reflection bytes and their 16-bit stack address. */
struct dos_interrupt_reflection_receipt {
	uint8_t original[DOS_INTERRUPT_REFLECTION_FRAME_BYTES];
	uint16_t stack_segment;
	uint16_t stack_offset;
	uint8_t valid;
	uint8_t reserved[5];
} __aligned(8);

static_assert_expression(
	sizeof(struct dos_interrupt_reflection_receipt) == 16u,
	"interrupt reflection receipt must stay fixed width");

/*
 * A value-only result keeps a failed memory transaction from publishing a
 * partially prepared CPU state.  machine_status retains the precise boundary
 * failure for the execution owner; it is DOS_MACHINE_OK only on success.
 */
struct dos_interrupt_reflection_result {
	struct dos_cpu_state state;
	struct dos_interrupt_reflection_receipt receipt;
	uint32_t status;
	uint32_t machine_status;
} __aligned(8);

static_assert_expression(sizeof(struct dos_interrupt_reflection_result) == 80u,
			 "interrupt reflection result must stay fixed width");

struct dos_interrupt_reflection_result dos_interrupt_reflect(
	const struct dos_machine *machine, uint8_t vector,
	const struct dos_cpu_state *post_interrupt_state) __must_check;
enum dos_machine_status dos_interrupt_reflection_rollback(
	const struct dos_machine *machine,
	const struct dos_interrupt_reflection_receipt *receipt) __must_check;

#endif

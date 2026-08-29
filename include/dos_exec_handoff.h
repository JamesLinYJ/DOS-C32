/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Fixed-width EXEC0 entry handoff.
 *
 * MS-DOS first switches to the child's SS:SP, pushes the entry CS and IP,
 * establishes DS=ES=PSP and AX=the late-bound drive result, then enters
 * through a far return.
 *
 * The C rewrite records the state visible at the first program instruction
 * and the four stack bytes left by that PUSH/PUSH/RETF sequence.  A backend
 * therefore consumes one pointer-free value whether it uses VM86 or a checked
 * interpreter; it never receives a native pointer disguised as a guest one.
 */
#ifndef DOSC32_DOS_EXEC_HANDOFF_H
#define DOSC32_DOS_EXEC_HANDOFF_H

#include "compiler.h"
#include "dos_exec_journal.h"
#include "dos_process.h"
#include "types.h"

#define DOS_EXEC_HANDOFF_STACK_BYTES 4u
#define DOS_EXEC_HANDOFF_STACK_WORDS 2u

struct dos_exec_handoff_stack_image {
	uint16_t segment;
	uint16_t offset;
	uint8_t bytes[DOS_EXEC_HANDOFF_STACK_BYTES];
};

/* Persistent/backend-facing value: no size_t, pointer, C enum or bool. */
struct dos_exec_handoff_plan {
	struct dos_cpu_state entry_state;
	struct dos_exec_handoff_stack_image stack_image;
	uint16_t child_psp;
	uint8_t format;
	uint8_t stack_word_count;
	uint8_t reserved[4];
} __aligned(8);

enum dos_exec_handoff_status {
	DOS_EXEC_HANDOFF_OK = 0,
	DOS_EXEC_HANDOFF_INVALID_ARGUMENT,
	DOS_EXEC_HANDOFF_INVALID_PROCESS_PLAN,
	DOS_EXEC_HANDOFF_INVALID_STATE,
	DOS_EXEC_HANDOFF_JOURNAL_FULL,
	DOS_EXEC_HANDOFF_MACHINE_FAULT,
	DOS_EXEC_HANDOFF_POISONED
};

static_assert_expression(sizeof(struct dos_exec_handoff_stack_image) == 8,
			 "EXEC handoff stack image must be fixed width");
static_assert_expression(sizeof(struct dos_exec_handoff_plan) == 72,
			 "EXEC handoff plan must be fixed width");
static_assert_expression(__alignof__(struct dos_exec_handoff_plan) == 8,
			 "EXEC handoff plan alignment changed");
static_assert_expression(
	__builtin_offsetof(struct dos_exec_handoff_plan, stack_image) == 56,
	"EXEC handoff stack image offset changed");
static_assert_expression(
	__builtin_offsetof(struct dos_exec_handoff_plan, child_psp) == 64,
	"EXEC handoff child PSP offset changed");

bool dos_exec_handoff_plan_has_valid_encoding(
    const struct dos_exec_handoff_plan *plan) __must_check;
bool dos_exec_handoff_plans_equal(
    const struct dos_exec_handoff_plan *left,
    const struct dos_exec_handoff_plan *right) __must_check;

/* Pure value preparation; result is unchanged on failure. */
enum dos_exec_handoff_status dos_exec_handoff_prepare_com(
    const struct dos_com_process_plan *process_plan,
    struct dos_exec_handoff_plan *result) __must_check;
enum dos_exec_handoff_status dos_exec_handoff_prepare_mz(
    const struct dos_mz_process_plan *process_plan,
    struct dos_exec_handoff_plan *result) __must_check;

/*
 * Stage the final four-byte stack footprint as one journal operation.  Guest
 * execution must remain excluded.  Atomic staging removes the unsafe interrupt
 * window that separate stack-word writes would create while preserving every
 * byte observable once the child begins.
 */
enum dos_exec_handoff_status dos_exec_handoff_stage_stack(
    const struct dos_exec_handoff_plan *plan,
    struct dos_exec_journal *journal,
    kernel_object_handle_t machine_identity,
    const struct dos_machine *machine) __must_check;

#endif

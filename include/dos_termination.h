/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * DOS process-termination capture
 *
 * Compatibility contract: restore the child vectors, release its arena blocks, close
 *                 every JFT entry from the last entry to the first, and
 *                 restore the parent process
 * Safety changes: validate the complete guest JFT before teardown and stream
 *                 it through bounded fixed storage instead of a native heap
 */
#ifndef DOSC32_DOS_TERMINATION_H
#define DOSC32_DOS_TERMINATION_H

#include "compiler.h"
#include "dos_abi.h"
#include "dos_machine.h"
#include "dos_memory.h"
#include "dos_process_runtime.h"
#include "dos_sft_batch.h"
#include "types.h"

enum dos_termination_status {
	DOS_TERMINATION_OK = 0,
	DOS_TERMINATION_INVALID_ARGUMENT,
	DOS_TERMINATION_STALE_PLAN,
	DOS_TERMINATION_MACHINE_FAULT,
	DOS_TERMINATION_MACHINE_POISONED,
	/* Retained as an ABI value; a 16-bit JFT is no longer size-limited. */
	DOS_TERMINATION_JFT_TOO_LARGE,
	DOS_TERMINATION_SFT_POISONED,
	DOS_TERMINATION_MEMORY_FAULT,
	DOS_TERMINATION_RUNTIME_FAULT,
	DOS_TERMINATION_POISONED
};

struct dos_termination_services {
	struct dos_process_runtime *runtime;
	struct dos_memory_arena *memory_arena;
	const struct dos_machine *machine;
	const struct dos_sft_batch_ops *sft_ops;
	kernel_object_handle_t machine_identity;
	kernel_object_handle_t sft_context;
};

struct dos_termination_plan {
	kernel_object_handle_t machine_identity;
	kernel_object_handle_t machine_context;
	uint64_t machine_address_limit;
	struct dos_far_pointer16 terminate_vector;
	struct dos_far_pointer16 control_c_vector;
	struct dos_far_pointer16 critical_error_vector;
	struct dos_far_pointer16 parent_user_stack;
	struct dos_far_pointer16 jft_pointer;
	uint16_t process_psp;
	uint16_t parent_psp;
	uint16_t jft_length;
	uint8_t a20_enabled;
	uint8_t captured;
	uint8_t reserved[4];
} __aligned(8);

struct dos_termination_result {
	struct dos_termination_plan plan;
	struct dos_process_runtime_snapshot child_runtime;
	uint32_t vector_status;
	uint32_t memory_status;
	uint32_t sft_status;
	uint32_t runtime_status;
} __aligned(8);

/*
 * Reads the PSP prefix, validates the full possibly relocated JFT range, and
 * proves every byte currently readable before publishing the bounded plan.
 * The terminating guest must remain quiesced until close_handles completes.
 */
enum dos_termination_status dos_termination_capture(
	const struct dos_machine *machine,
	kernel_object_handle_t machine_identity, uint16_t process_psp,
	struct dos_termination_plan *plan) __must_check;

/* Restores contiguous INT 22h/23h/24h IVT bytes as one rollback unit. */
enum dos_termination_status dos_termination_restore_vectors(
	const struct dos_machine *machine,
	kernel_object_handle_t machine_identity,
	const struct dos_termination_plan *plan) __must_check;

/*
 * Revalidates and prereads the complete JFT before the first close, then reads
 * it in fixed-size chunks and closes entries in the exact reverse order used
 * during process abort. Invalid/unused SFNs are ignored. All remaining entries are
 * attempted after a callback failure; partial close is irreversible and is
 * therefore reported as poisoned.
 */
enum dos_termination_status dos_termination_close_handles(
	const struct dos_machine *machine,
	kernel_object_handle_t machine_identity,
	const struct dos_termination_plan *plan,
	const struct dos_sft_batch_ops *ops,
	kernel_object_handle_t sft_context) __must_check;

/*
 * One MS-DOS-ordered normal-exit coordinator. parent_runtime must be the
 * snapshot taken immediately before EXEC published process_psp.  The child
 * PSP is captured before any teardown write; a mismatch is rejected without
 * mutation.  Once vector restoration succeeds, any uncertain later failure
 * poisons the arena and runtime rather than exposing a partly dead process.
 */
enum dos_termination_status dos_termination_execute(
	const struct dos_termination_services *services,
	const struct dos_process_runtime_snapshot *parent_runtime,
	uint16_t process_psp, struct dos_termination_result *result) __must_check;

static_assert_expression(sizeof(struct dos_termination_plan) == 56u,
			 "termination plans must stay fixed width");
static_assert_expression(sizeof(struct dos_termination_result) == 96u,
			 "termination results must stay fixed width");

#endif

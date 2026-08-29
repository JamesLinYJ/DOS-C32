/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Ordered access to the MS-DOS EXEC0/EXEC1 parameter block.
 *
 * EXEC reads the environment word after OPEN succeeds, but does not
 * consume the two FCB pointers and command-tail pointer until the image and
 * PSP preparation stages.  Separate bounded calls preserve that ordering.
 * No packed guest structure is cast onto native memory.
 */
#ifndef DOSC32_DOS_EXEC_PARAMETER_H
#define DOSC32_DOS_EXEC_PARAMETER_H

#include "compiler.h"
#include "dos_exec_journal.h"
#include "dos_exec_overlay.h"
#include "dos_machine.h"
#include "dos_process.h"
#include "types.h"

enum dos_exec_parameter_status {
	DOS_EXEC_PARAMETER_OK = 0,
	DOS_EXEC_PARAMETER_INVALID_ARGUMENT,
	DOS_EXEC_PARAMETER_MACHINE_FAULT
};

/*
 * The plan records why an environment segment was (or was not) selected. The
 * field stored in dos_exec_environment_source_plan is an
 * explicitly sized byte; this enum only names its valid encodings.
 */
enum dos_exec_environment_source_kind {
	DOS_EXEC_ENVIRONMENT_SOURCE_NONE = 0,
	DOS_EXEC_ENVIRONMENT_SOURCE_PARAMETER = 1,
	DOS_EXEC_ENVIRONMENT_SOURCE_PARENT = 2,
	DOS_EXEC_ENVIRONMENT_SOURCE_OVERLAY_SKIPPED = 3
};

/*
 * Persistent, data-model-independent result of the ordered environment reads.
 * source is always segment:0000, matching MOV ES,AX / XOR DI,DI.  parent_psp
 * is zero unless the parent PDB environment word was actually consulted.
 * No native pointer or compiler-sized value crosses this boundary.
 */
struct dos_exec_environment_source_plan {
	struct dos_far_pointer16 source;
	uint16_t parent_psp;
	uint8_t subfunction;
	uint8_t kind;
};

static inline bool dos_exec_environment_source_plan_has_valid_encoding(
    const struct dos_exec_environment_source_plan *plan)
{
	if (plan == NULL || plan->source.offset != 0u ||
	    !dos_exec_subfunction_is_valid(plan->subfunction))
		return false;
	switch (plan->kind) {
	case DOS_EXEC_ENVIRONMENT_SOURCE_NONE:
		return plan->subfunction != DOS_EXEC_OVERLAY &&
		       plan->source.segment == 0u;
	case DOS_EXEC_ENVIRONMENT_SOURCE_PARAMETER:
		return plan->subfunction != DOS_EXEC_OVERLAY &&
		       plan->source.segment != 0u && plan->parent_psp == 0u;
	case DOS_EXEC_ENVIRONMENT_SOURCE_PARENT:
		return plan->subfunction != DOS_EXEC_OVERLAY &&
		       plan->source.segment != 0u;
	case DOS_EXEC_ENVIRONMENT_SOURCE_OVERLAY_SKIPPED:
		return plan->subfunction == DOS_EXEC_OVERLAY &&
		       plan->source.segment == 0u && plan->parent_psp == 0u;
	default:
		return false;
	}
}

/*
 * Preserve MS-DOS ordering after OPEN and device probing:
 *
 *   AL=3: skip both guest reads;
 *   AL=0/1: read parameter-block word zero first; if it is zero, then read
 *           CurrentPDB:002ch; a second zero means no environment.
 *
 * parent_psp must come from the caller's pinned process-runtime snapshot.
 * The function does not retain machine or parameter_block.  plan is unchanged
 * on every error.  OVERLAY_SKIPPED and NONE must not be passed to the
 * environment scanner/builder.
 */
enum dos_exec_parameter_status dos_exec_parameter_decode_environment_source(
    const struct dos_machine *machine, uint8_t subfunction,
    struct dos_process_far_address parameter_block, uint16_t parent_psp,
    struct dos_exec_environment_source_plan *plan) __must_check;

/* parameter_block is a simulated DOS address; 0000:0000 remains valid. */
enum dos_exec_parameter_status dos_exec_parameter_read_environment(
    const struct dos_machine *machine,
    struct dos_process_far_address parameter_block,
    uint16_t *environment_segment) __must_check;

enum dos_exec_parameter_status dos_exec_parameter_read_first_fcb(
    const struct dos_machine *machine,
    struct dos_process_far_address parameter_block,
    struct dos_process_far_address *first_fcb) __must_check;

enum dos_exec_parameter_status dos_exec_parameter_read_second_fcb(
    const struct dos_machine *machine,
    struct dos_process_far_address parameter_block,
    struct dos_process_far_address *second_fcb) __must_check;

enum dos_exec_parameter_status dos_exec_parameter_read_command_tail(
    const struct dos_machine *machine,
    struct dos_process_far_address parameter_block,
    struct dos_process_far_address *command_tail) __must_check;

/*
 * EXEC3 does not read an environment or Exec0 pointers.  After image
 * classification, COM consumes only the first load-address word; MZ then
 * consumes the relocation-factor word.  Keeping two entry points prevents a
 * COM overlay from reading beyond its one-word parameter extent.
 */
enum dos_exec_parameter_status dos_exec_parameter_read_com_overlay_segment(
    const struct dos_machine *machine,
    struct dos_process_far_address parameter_block,
    uint16_t *load_segment) __must_check;

enum dos_exec_parameter_status dos_exec_parameter_read_mz_overlay_target(
    const struct dos_machine *machine,
    struct dos_process_far_address parameter_block,
    struct dos_exec_mz_overlay_target *target) __must_check;

/* Decoded value written to Exec1_SP..Exec1_CS at parameter-block offset 14. */
struct dos_exec_load_result_value {
	uint16_t initial_sp;
	uint16_t initial_ss;
	uint16_t initial_ip;
	uint16_t initial_cs;
};

/*
 * Stage the load-only return tuple in the caller's live undo journal.  The
 * coordinator prepares the default-AX stack word inside the unpublished load
 * lease before sealing this journal.  Encoding is explicit little endian;
 * this function never overlays dos_exec_load_result40 on guest memory.
 */
enum dos_exec_journal_status dos_exec_parameter_stage_load_result(
    struct dos_exec_journal *journal, kernel_object_handle_t machine_identity,
    const struct dos_machine *machine,
    struct dos_process_far_address parameter_block,
    const struct dos_exec_load_result_value *result) __must_check;

static_assert_expression(sizeof(struct dos_process_far_address) == 4,
			 "decoded DOS far addresses must remain four bytes");
static_assert_expression(__builtin_offsetof(struct dos_process_far_address,
					    offset) == 2,
			 "decoded DOS far-address field order changed");
static_assert_expression(sizeof(struct dos_exec_load_result_value) == 8,
			 "decoded EXEC1 result must remain eight bytes");
static_assert_expression(__builtin_offsetof(struct dos_exec_load_result_value,
					    initial_cs) == 6,
			 "decoded EXEC1 CS offset changed");
static_assert_expression(sizeof(struct dos_exec_environment_source_plan) == 8,
			 "environment selection plan must remain eight bytes");
static_assert_expression(
    __builtin_offsetof(struct dos_exec_environment_source_plan, parent_psp) ==
	4,
    "environment source parent PSP offset changed");
static_assert_expression(
    __builtin_offsetof(struct dos_exec_environment_source_plan, kind) == 7,
    "environment source kind offset changed");
#endif

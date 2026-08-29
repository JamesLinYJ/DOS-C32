// SPDX-License-Identifier: GPL-2.0-only
/* Value-only EXEC backend entry contract. */
#include "dos_exec_handoff.h"

static bool bytes_are_zero(const uint8_t *bytes, size_t count)
{
	size_t index;

	for (index = 0u; index < count; ++index) {
		if (bytes[index] != 0u)
			return false;
	}
	return true;
}

static bool bytes_equal(const uint8_t *left, const uint8_t *right,
			size_t count)
{
	size_t index;

	for (index = 0u; index < count; ++index) {
		if (left[index] != right[index])
			return false;
	}
	return true;
}

static bool image_format_is_process(uint8_t format)
{
	return format == (uint8_t)DOS_IMAGE_COM ||
	       format == (uint8_t)DOS_IMAGE_MZ;
}

static void store_word_le(uint8_t destination[2], uint16_t value)
{
	destination[0] = (uint8_t)(value & 0xffu);
	destination[1] = (uint8_t)(value >> 8);
}

static bool word_le_equals(const uint8_t source[2], uint16_t value)
{
	return source[0] == (uint8_t)(value & 0xffu) &&
	       source[1] == (uint8_t)(value >> 8);
}

static bool entry_state_has_exec_go_shape(const struct dos_cpu_state *state,
					   uint16_t child_psp)
{
	uint16_t sp;
	uint16_t ip;

	if (state->mode != (uint32_t)DOS_CPU_REAL16 ||
	    state->ds != child_psp || state->es != child_psp ||
	    dos_register_low16(state->edx) != child_psp ||
	    state->eax != state->ebx)
		return false;
	sp = dos_register_low16(state->esp);
	ip = dos_register_low16(state->eip);
	return dos_register_low16(state->edi) == sp &&
	       dos_register_low16(state->esi) == ip;
}

bool dos_exec_handoff_plan_has_valid_encoding(
    const struct dos_exec_handoff_plan *plan)
{
	uint16_t sp;
	uint16_t ip;

	if (plan == NULL || !image_format_is_process(plan->format) ||
	    plan->stack_word_count != DOS_EXEC_HANDOFF_STACK_WORDS ||
	    !bytes_are_zero(plan->reserved, ARRAY_SIZE(plan->reserved)) ||
	    !entry_state_has_exec_go_shape(&plan->entry_state,
					   plan->child_psp))
		return false;
	sp = dos_register_low16(plan->entry_state.esp);
	ip = dos_register_low16(plan->entry_state.eip);
	return plan->stack_image.segment == plan->entry_state.ss &&
	       plan->stack_image.offset ==
		   (uint16_t)(sp - DOS_EXEC_HANDOFF_STACK_BYTES) &&
	       word_le_equals(&plan->stack_image.bytes[0], ip) &&
	       word_le_equals(&plan->stack_image.bytes[2], plan->entry_state.cs);
}

static bool cpu_states_equal(const struct dos_cpu_state *left,
			     const struct dos_cpu_state *right)
{
	return left->eax == right->eax && left->ebx == right->ebx &&
	       left->ecx == right->ecx && left->edx == right->edx &&
	       left->esi == right->esi && left->edi == right->edi &&
	       left->ebp == right->ebp && left->esp == right->esp &&
	       left->eip == right->eip && left->eflags == right->eflags &&
	       left->cs == right->cs && left->ss == right->ss &&
	       left->ds == right->ds && left->es == right->es &&
	       left->fs == right->fs && left->gs == right->gs &&
	       left->mode == right->mode;
}

bool dos_exec_handoff_plans_equal(const struct dos_exec_handoff_plan *left,
				  const struct dos_exec_handoff_plan *right)
{
	return dos_exec_handoff_plan_has_valid_encoding(left) &&
	       dos_exec_handoff_plan_has_valid_encoding(right) &&
	       cpu_states_equal(&left->entry_state, &right->entry_state) &&
	       left->stack_image.segment == right->stack_image.segment &&
	       left->stack_image.offset == right->stack_image.offset &&
	       bytes_equal(left->stack_image.bytes, right->stack_image.bytes,
			   ARRAY_SIZE(left->stack_image.bytes)) &&
	       left->child_psp == right->child_psp &&
	       left->format == right->format &&
	       left->stack_word_count == right->stack_word_count;
}

static enum dos_exec_handoff_status prepare(
    const struct dos_cpu_state *state, uint16_t child_psp, uint8_t format,
    struct dos_exec_handoff_plan *result)
{
	struct dos_exec_handoff_plan prepared = {0};
	uint16_t ip;
	uint16_t sp;

	if (state == NULL || result == NULL)
		return DOS_EXEC_HANDOFF_INVALID_ARGUMENT;
	if (!image_format_is_process(format) ||
	    !entry_state_has_exec_go_shape(state, child_psp))
		return DOS_EXEC_HANDOFF_INVALID_PROCESS_PLAN;
	prepared.entry_state = *state;
	sp = dos_register_low16(state->esp);
	ip = dos_register_low16(state->eip);
	prepared.stack_image.segment = state->ss;
	prepared.stack_image.offset =
	    (uint16_t)(sp - DOS_EXEC_HANDOFF_STACK_BYTES);
	store_word_le(&prepared.stack_image.bytes[0], ip);
	store_word_le(&prepared.stack_image.bytes[2], state->cs);
	prepared.child_psp = child_psp;
	prepared.format = format;
	prepared.stack_word_count = DOS_EXEC_HANDOFF_STACK_WORDS;
	if (!dos_exec_handoff_plan_has_valid_encoding(&prepared))
		return DOS_EXEC_HANDOFF_INVALID_PROCESS_PLAN;
	*result = prepared;
	return DOS_EXEC_HANDOFF_OK;
}

enum dos_exec_handoff_status dos_exec_handoff_prepare_com(
    const struct dos_com_process_plan *process_plan,
    struct dos_exec_handoff_plan *result)
{
	if (process_plan == NULL || result == NULL)
		return DOS_EXEC_HANDOFF_INVALID_ARGUMENT;
	if (!dos_com_process_plan_has_valid_encoding(process_plan) ||
	    process_plan->launch_mode !=
		(uint8_t)DOS_PROCESS_LAUNCH_EXECUTE)
		return DOS_EXEC_HANDOFF_INVALID_PROCESS_PLAN;
	return prepare(&process_plan->initial_state, process_plan->psp_segment,
		       (uint8_t)DOS_IMAGE_COM, result);
}

enum dos_exec_handoff_status dos_exec_handoff_prepare_mz(
    const struct dos_mz_process_plan *process_plan,
    struct dos_exec_handoff_plan *result)
{
	if (process_plan == NULL || result == NULL)
		return DOS_EXEC_HANDOFF_INVALID_ARGUMENT;
	if (!dos_mz_process_plan_has_valid_encoding(process_plan) ||
	    process_plan->launch_mode !=
		(uint8_t)DOS_PROCESS_LAUNCH_EXECUTE)
		return DOS_EXEC_HANDOFF_INVALID_PROCESS_PLAN;
	return prepare(&process_plan->initial_state, process_plan->psp_segment,
		       (uint8_t)DOS_IMAGE_MZ, result);
}

static enum dos_exec_handoff_status
map_journal_status(enum dos_exec_journal_status status)
{
	switch (status) {
	case DOS_EXEC_JOURNAL_OK:
		return DOS_EXEC_HANDOFF_OK;
	case DOS_EXEC_JOURNAL_FULL:
		return DOS_EXEC_HANDOFF_JOURNAL_FULL;
	case DOS_EXEC_JOURNAL_MACHINE_FAULT:
		return DOS_EXEC_HANDOFF_MACHINE_FAULT;
	case DOS_EXEC_JOURNAL_POISONED:
		return DOS_EXEC_HANDOFF_POISONED;
	case DOS_EXEC_JOURNAL_INVALID_STATE:
		return DOS_EXEC_HANDOFF_INVALID_STATE;
	default:
		return DOS_EXEC_HANDOFF_INVALID_ARGUMENT;
	}
}

enum dos_exec_handoff_status dos_exec_handoff_stage_stack(
    const struct dos_exec_handoff_plan *plan,
    struct dos_exec_journal *journal,
    kernel_object_handle_t machine_identity,
    const struct dos_machine *machine)
{
	enum dos_exec_journal_status status;

	if (!dos_exec_handoff_plan_has_valid_encoding(plan))
		return DOS_EXEC_HANDOFF_INVALID_ARGUMENT;
	status = dos_exec_journal_stage_replace_far_span(
	    journal, machine_identity, machine, plan->stack_image.segment,
	    plan->stack_image.offset, plan->stack_image.bytes,
	    sizeof(plan->stack_image.bytes), sizeof(plan->stack_image.bytes));
	return map_journal_status(status);
}

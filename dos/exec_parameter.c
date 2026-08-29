// SPDX-License-Identifier: GPL-2.0-only
/* Ordered, byte-decoded DOS EXEC parameter-block access. */
#include "dos_exec_parameter.h"

#define EXEC_ENVIRONMENT_OFFSET 0u
#define EXEC_COMMAND_TAIL_OFFSET 2u
#define EXEC_FIRST_FCB_OFFSET 6u
#define EXEC_SECOND_FCB_OFFSET 10u
#define EXEC_LOAD_RESULT_OFFSET 14u
#define EXEC_OVERLAY_LOAD_SEGMENT_OFFSET 0u
#define EXEC_OVERLAY_RELOCATION_FACTOR_OFFSET 2u
#define EXEC_PARENT_ENVIRONMENT_OFFSET 0x2cu

static uint16_t read_le16(const uint8_t bytes[2])
{
	return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static enum dos_exec_parameter_status
read_word(const struct dos_machine *machine,
	  struct dos_process_far_address parameter_block, uint16_t displacement,
	  uint16_t *value)
{
	uint8_t bytes[2];
	uint16_t offset;

	if (machine == NULL || value == NULL)
		return DOS_EXEC_PARAMETER_INVALID_ARGUMENT;
	offset = (uint16_t)(parameter_block.offset + displacement);
	if (dos_machine_read_far(machine, parameter_block.segment, offset,
				 bytes, sizeof(bytes),
				 sizeof(bytes)) != DOS_MACHINE_OK)
		return DOS_EXEC_PARAMETER_MACHINE_FAULT;
	*value = read_le16(bytes);
	return DOS_EXEC_PARAMETER_OK;
}

static enum dos_exec_parameter_status
read_far_address(const struct dos_machine *machine,
		 struct dos_process_far_address parameter_block,
		 uint16_t displacement, struct dos_process_far_address *address)
{
	struct dos_process_far_address staging;
	uint8_t bytes[4];
	uint16_t offset;

	if (machine == NULL || address == NULL)
		return DOS_EXEC_PARAMETER_INVALID_ARGUMENT;
	offset = (uint16_t)(parameter_block.offset + displacement);
	if (dos_machine_read_far(machine, parameter_block.segment, offset,
				 bytes, sizeof(bytes),
				 sizeof(bytes)) != DOS_MACHINE_OK)
		return DOS_EXEC_PARAMETER_MACHINE_FAULT;
	staging.offset = read_le16(bytes);
	staging.segment = read_le16(bytes + 2u);
	*address = staging;
	return DOS_EXEC_PARAMETER_OK;
}

enum dos_exec_parameter_status dos_exec_parameter_read_environment(
    const struct dos_machine *machine,
    struct dos_process_far_address parameter_block,
    uint16_t *environment_segment)
{
	return read_word(machine, parameter_block, EXEC_ENVIRONMENT_OFFSET,
			 environment_segment);
}

static void publish_environment_source(
    struct dos_exec_environment_source_plan *destination,
    const struct dos_exec_environment_source_plan *source)
{
	destination->source.offset = source->source.offset;
	destination->source.segment = source->source.segment;
	destination->parent_psp = source->parent_psp;
	destination->subfunction = source->subfunction;
	destination->kind = source->kind;
}

enum dos_exec_parameter_status dos_exec_parameter_decode_environment_source(
    const struct dos_machine *machine, uint8_t subfunction,
    struct dos_process_far_address parameter_block, uint16_t parent_psp,
    struct dos_exec_environment_source_plan *plan)
{
	struct dos_exec_environment_source_plan staging = {
	    .source = {.offset = 0u, .segment = 0u},
	    .parent_psp = 0u,
	    .subfunction = subfunction,
	    .kind = DOS_EXEC_ENVIRONMENT_SOURCE_NONE,
	};
	struct dos_process_far_address parent_environment = {
	    .segment = parent_psp,
	    .offset = 0u,
	};
	enum dos_exec_parameter_status status;
	uint16_t environment_segment;

	if (plan == NULL || !dos_exec_subfunction_is_valid(subfunction))
		return DOS_EXEC_PARAMETER_INVALID_ARGUMENT;
	if (subfunction == DOS_EXEC_OVERLAY) {
		staging.kind = DOS_EXEC_ENVIRONMENT_SOURCE_OVERLAY_SKIPPED;
		publish_environment_source(plan, &staging);
		return DOS_EXEC_PARAMETER_OK;
	}

	status = read_word(machine, parameter_block, EXEC_ENVIRONMENT_OFFSET,
			   &environment_segment);
	if (status != DOS_EXEC_PARAMETER_OK)
		return status;
	if (environment_segment != 0u) {
		staging.source.segment = environment_segment;
		staging.kind = DOS_EXEC_ENVIRONMENT_SOURCE_PARAMETER;
		publish_environment_source(plan, &staging);
		return DOS_EXEC_PARAMETER_OK;
	}

	status = read_word(machine, parent_environment,
			   EXEC_PARENT_ENVIRONMENT_OFFSET,
			   &environment_segment);
	if (status != DOS_EXEC_PARAMETER_OK)
		return status;
	staging.parent_psp = parent_psp;
	staging.source.segment = environment_segment;
	if (environment_segment != 0u)
		staging.kind = DOS_EXEC_ENVIRONMENT_SOURCE_PARENT;
	publish_environment_source(plan, &staging);
	return DOS_EXEC_PARAMETER_OK;
}

enum dos_exec_parameter_status dos_exec_parameter_read_first_fcb(
    const struct dos_machine *machine,
    struct dos_process_far_address parameter_block,
    struct dos_process_far_address *first_fcb)
{
	return read_far_address(machine, parameter_block, EXEC_FIRST_FCB_OFFSET,
				first_fcb);
}

enum dos_exec_parameter_status dos_exec_parameter_read_second_fcb(
    const struct dos_machine *machine,
    struct dos_process_far_address parameter_block,
    struct dos_process_far_address *second_fcb)
{
	return read_far_address(machine, parameter_block,
				EXEC_SECOND_FCB_OFFSET, second_fcb);
}

enum dos_exec_parameter_status dos_exec_parameter_read_command_tail(
    const struct dos_machine *machine,
    struct dos_process_far_address parameter_block,
    struct dos_process_far_address *command_tail)
{
	return read_far_address(machine, parameter_block,
				EXEC_COMMAND_TAIL_OFFSET, command_tail);
}

enum dos_exec_parameter_status dos_exec_parameter_read_com_overlay_segment(
    const struct dos_machine *machine,
    struct dos_process_far_address parameter_block, uint16_t *load_segment)
{
	return read_word(machine, parameter_block,
			 EXEC_OVERLAY_LOAD_SEGMENT_OFFSET, load_segment);
}

enum dos_exec_parameter_status dos_exec_parameter_read_mz_overlay_target(
    const struct dos_machine *machine,
    struct dos_process_far_address parameter_block,
    struct dos_exec_mz_overlay_target *target)
{
	struct dos_exec_mz_overlay_target staging;
	enum dos_exec_parameter_status status;

	if (target == NULL)
		return DOS_EXEC_PARAMETER_INVALID_ARGUMENT;
	status =
	    read_word(machine, parameter_block,
		      EXEC_OVERLAY_LOAD_SEGMENT_OFFSET, &staging.load_segment);
	if (status != DOS_EXEC_PARAMETER_OK)
		return status;
	status = read_word(machine, parameter_block,
			   EXEC_OVERLAY_RELOCATION_FACTOR_OFFSET,
			   &staging.relocation_factor);
	if (status != DOS_EXEC_PARAMETER_OK)
		return status;
	*target = staging;
	return DOS_EXEC_PARAMETER_OK;
}

static void write_le16(uint8_t bytes[2], uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
}

enum dos_exec_journal_status dos_exec_parameter_stage_load_result(
    struct dos_exec_journal *journal, kernel_object_handle_t machine_identity,
    const struct dos_machine *machine,
    struct dos_process_far_address parameter_block,
    const struct dos_exec_load_result_value *result)
{
	uint8_t bytes[sizeof(struct dos_exec_load_result_value)];
	uint16_t offset;

	if (result == NULL)
		return DOS_EXEC_JOURNAL_INVALID_ARGUMENT;
	write_le16(bytes, result->initial_sp);
	write_le16(bytes + 2u, result->initial_ss);
	write_le16(bytes + 4u, result->initial_ip);
	write_le16(bytes + 6u, result->initial_cs);
	offset = (uint16_t)(parameter_block.offset + EXEC_LOAD_RESULT_OFFSET);
	return dos_exec_journal_stage_replace_far(
	    journal, machine_identity, machine, parameter_block.segment, offset,
	    bytes, sizeof(bytes), sizeof(bytes));
}

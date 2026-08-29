// SPDX-License-Identifier: GPL-2.0-only
/* Safe native construction of COMMAND's MS-DOS EXEC0 parameter block. */
#include "dos_exec_native.h"

#include "dos_memory.h"
#include "dos_process.h"
#include "string.h"

#define NATIVE_EXEC_NAME_OFFSET 0u
#define NATIVE_EXEC_NAME_CAPACITY DOS_EXEC_PATH_CAPACITY
#define NATIVE_EXEC_PARAGRAPH_BYTES 16u
#define NATIVE_EXEC_PARAMETER_OFFSET \
	(NATIVE_EXEC_NAME_OFFSET + NATIVE_EXEC_NAME_CAPACITY)
#define NATIVE_EXEC_PARAMETER_BYTES 14u
#define NATIVE_EXEC_FIRST_FCB_OFFSET \
	(NATIVE_EXEC_PARAMETER_OFFSET + NATIVE_EXEC_PARAMETER_BYTES)
#define NATIVE_EXEC_SECOND_FCB_OFFSET \
	(NATIVE_EXEC_FIRST_FCB_OFFSET + DOS_PROCESS_FCB_PREFIX_BYTES)
#define NATIVE_EXEC_COMMAND_TAIL_OFFSET \
	(NATIVE_EXEC_SECOND_FCB_OFFSET + DOS_PROCESS_FCB_PREFIX_BYTES)
#define NATIVE_EXEC_SCRATCH_BYTES \
	(NATIVE_EXEC_COMMAND_TAIL_OFFSET + sizeof(struct dos_command_tail40))
#define NATIVE_EXEC_SCRATCH_PARAGRAPHS \
	((NATIVE_EXEC_SCRATCH_BYTES + NATIVE_EXEC_PARAGRAPH_BYTES - 1u) / \
	 NATIVE_EXEC_PARAGRAPH_BYTES)

#define EXEC_PARAMETER_COMMAND_TAIL_OFFSET 2u
#define EXEC_PARAMETER_FIRST_FCB_OFFSET 6u
#define EXEC_PARAMETER_SECOND_FCB_OFFSET 10u

static void write_le16(uint8_t *destination, uint16_t value)
{
	destination[0] = (uint8_t)value;
	destination[1] = (uint8_t)(value >> 8);
}

static void write_far(uint8_t *destination, uint16_t segment, uint16_t offset)
{
	write_le16(destination, offset);
	write_le16(destination + 2u, segment);
}

static bool native_request_is_valid(
	const struct dos_exec_transaction_services *services,
	const struct dos_exec_native_request *request)
{
	size_t index;

	if (services == NULL || services->machine == NULL ||
	    services->memory_arena == NULL || services->runtime == NULL ||
	    request == NULL || request->executable_name == NULL ||
	    request->executable_name_length == 0u ||
	    request->executable_name_length >= NATIVE_EXEC_NAME_CAPACITY ||
	    request->command_tail_length > request->command_tail_capacity ||
	    request->command_tail_length > DOS_COMMAND_TAIL_BYTES - 1u ||
	    (request->command_tail == NULL && request->command_tail_length != 0u))
		return false;
	for (index = 0u; index < request->executable_name_length; ++index) {
		if (request->executable_name[index] == 0u)
			return false;
	}
	return true;
}

static void prepare_exec_parameter_block(uint8_t *scratch, uint16_t segment)
{
	uint8_t *parameters = scratch + NATIVE_EXEC_PARAMETER_OFFSET;

	/* Environment word zero means inherit the parent's PSP environment. */
	write_le16(parameters, 0u);
	write_far(parameters + EXEC_PARAMETER_COMMAND_TAIL_OFFSET, segment,
		  NATIVE_EXEC_COMMAND_TAIL_OFFSET);
	write_far(parameters + EXEC_PARAMETER_FIRST_FCB_OFFSET, segment,
		  NATIVE_EXEC_FIRST_FCB_OFFSET);
	write_far(parameters + EXEC_PARAMETER_SECOND_FCB_OFFSET, segment,
		  NATIVE_EXEC_SECOND_FCB_OFFSET);
}

enum dos_exec_native_status dos_exec_native_execute(
	struct dos_exec_transaction_table *transactions,
	const struct dos_exec_transaction_services *services,
	const struct dos_exec_native_request *request,
	struct dos_process_far_address terminate_vector,
	struct dos_exec_native_result *result)
{
	uint8_t scratch[NATIVE_EXEC_SCRATCH_PARAGRAPHS *
			NATIVE_EXEC_PARAGRAPH_BYTES] =
		{0u};
	struct dos_command_tail40 encoded_tail;
	struct dos_memory_allocation_result allocation = {0u, 0u};
	struct dos_exec_transaction_request guest_request;
	struct dos_exec_native_result prepared = {0};
	enum dos_exec_native_status native_status;
	enum dos_exec_executor_status executor_status;
	enum dos_memory_status memory_status;
	enum dos_machine_status machine_status;
	uint16_t scratch_owner;
	size_t index;

	if (transactions == NULL || result == NULL ||
	    !native_request_is_valid(services, request))
		return DOS_EXEC_NATIVE_INVALID_ARGUMENT;
	if (dos_process_encode_command_tail(
		request->command_tail, request->command_tail_capacity,
		request->command_tail_length, &encoded_tail) != DOS_PROCESS_OK)
		return DOS_EXEC_NATIVE_INVALID_ARGUMENT;

	scratch_owner = services->runtime->current_psp;
	memory_status = dos_memory_allocate_checked(
		services->memory_arena, services->machine,
		scratch_owner, NATIVE_EXEC_SCRATCH_PARAGRAPHS,
		&allocation);
	prepared.allocation_status = (uint32_t)memory_status;
	prepared.maximum_available = allocation.maximum_available;
	if (memory_status != DOS_MEMORY_OK) {
		*result = prepared;
		return DOS_EXEC_NATIVE_ALLOCATION_FAILED;
	}
	prepared.scratch_segment = allocation.block_segment;
	for (index = 0u; index < request->executable_name_length; ++index)
		scratch[NATIVE_EXEC_NAME_OFFSET + index] =
			request->executable_name[index];
	scratch[NATIVE_EXEC_NAME_OFFSET + request->executable_name_length] = 0u;
	prepare_exec_parameter_block(scratch, allocation.block_segment);
	if (memcpy_s(scratch + NATIVE_EXEC_COMMAND_TAIL_OFFSET,
		     sizeof(scratch) - NATIVE_EXEC_COMMAND_TAIL_OFFSET,
		     &encoded_tail, sizeof(encoded_tail),
		     sizeof(encoded_tail)) != MEMORY_OK) {
		prepared.staging_status = (uint32_t)DOS_MACHINE_INVALID_ARGUMENT;
		native_status = DOS_EXEC_NATIVE_STAGING_FAILED;
		goto cleanup;
	}
	machine_status = dos_machine_write_far(
		services->machine, allocation.block_segment, 0u, scratch,
		sizeof(scratch), sizeof(scratch));
	prepared.staging_status = (uint32_t)machine_status;
	if (machine_status != DOS_MACHINE_OK) {
		native_status = DOS_EXEC_NATIVE_STAGING_FAILED;
		goto cleanup;
	}

	guest_request = (struct dos_exec_transaction_request){
		.executable_name = {
			.offset = NATIVE_EXEC_NAME_OFFSET,
			.segment = allocation.block_segment,
		},
		.parameter_block = {
			.offset = NATIVE_EXEC_PARAMETER_OFFSET,
			.segment = allocation.block_segment,
		},
		.subfunction = DOS_EXEC_LOAD_AND_EXECUTE,
		.reserved = {0u},
	};
	executor_status = dos_exec_executor_execute(
		transactions, services, &guest_request, terminate_vector,
		&prepared.executor);
	native_status = executor_status == DOS_EXEC_EXECUTOR_OK
			? DOS_EXEC_NATIVE_OK
			: DOS_EXEC_NATIVE_EXEC_FAILED;

cleanup:
	memory_status = dos_memory_free_owned_checked(
		services->memory_arena, services->machine,
		allocation.block_segment, scratch_owner);
	prepared.cleanup_status = (uint32_t)memory_status;
	*result = prepared;
	return memory_status == DOS_MEMORY_OK ? native_status
					     : DOS_EXEC_NATIVE_CLEANUP_FAILED;
}

static_assert_expression(NATIVE_EXEC_SCRATCH_BYTES <=
			 NATIVE_EXEC_SCRATCH_PARAGRAPHS *
				 NATIVE_EXEC_PARAGRAPH_BYTES,
			 "native EXEC scratch extent overflowed");
static_assert_expression(NATIVE_EXEC_NAME_CAPACITY == DOS_EXEC_PATH_CAPACITY,
			 "native and DOS EXEC path capacities diverged");

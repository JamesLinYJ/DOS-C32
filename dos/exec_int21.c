// SPDX-License-Identifier: GPL-2.0-only
/* MS-DOS-compatible AH=4Bh register decode and typed error boundary. */
#include "dos_exec_int21.h"

#define INT21_EXEC_FUNCTION 0x4bu

static bool transaction_failure_is_dos_visible(
	enum dos_exec_transaction_status status)
{
	return status == DOS_EXEC_TRANSACTION_NAME_FAULT ||
	       status == DOS_EXEC_TRANSACTION_OPEN_FAILED ||
	       status == DOS_EXEC_TRANSACTION_IS_DEVICE ||
	       status == DOS_EXEC_TRANSACTION_BAD_ENVIRONMENT ||
	       status == DOS_EXEC_TRANSACTION_NOT_ENOUGH_MEMORY ||
	       status == DOS_EXEC_TRANSACTION_BAD_IMAGE ||
	       status == DOS_EXEC_TRANSACTION_IMAGE_TOO_LARGE ||
	       status == DOS_EXEC_TRANSACTION_SFT_UNAVAILABLE;
}

static enum dos_error transaction_failure_to_dos_error(
	enum dos_exec_transaction_status status)
{
	switch (status) {
	case DOS_EXEC_TRANSACTION_NAME_FAULT:
	case DOS_EXEC_TRANSACTION_OPEN_FAILED:
		return DOS_ERROR_FILE_NOT_FOUND;
	case DOS_EXEC_TRANSACTION_IS_DEVICE:
		return DOS_ERROR_ACCESS_DENIED;
	case DOS_EXEC_TRANSACTION_BAD_ENVIRONMENT:
		return DOS_ERROR_BAD_ENVIRONMENT;
	case DOS_EXEC_TRANSACTION_NOT_ENOUGH_MEMORY:
		return DOS_ERROR_NOT_ENOUGH_MEMORY;
	case DOS_EXEC_TRANSACTION_BAD_IMAGE:
	case DOS_EXEC_TRANSACTION_IMAGE_TOO_LARGE:
		return DOS_ERROR_BAD_FORMAT;
	case DOS_EXEC_TRANSACTION_SFT_UNAVAILABLE:
		return DOS_ERROR_TOO_MANY_OPEN_FILES;
	default:
		return DOS_ERROR_GENERAL_FAILURE;
	}
}

static void set_dos_error(struct dos_cpu_state *state, enum dos_error error)
{
	dos_register_set_low16(&state->eax, (uint16_t)error);
	state->eflags |= DOS_EFLAGS_CF;
}

enum dos_exec_int21_status dos_exec_int21_execute(
	struct dos_exec_transaction_table *transactions,
	const struct dos_exec_transaction_services *services,
	const struct dos_cpu_state *state, struct dos_exec_int21_result *result)
{
	struct dos_exec_int21_result prepared = {0};
	struct dos_exec_transaction_request request;
	struct dos_process_far_address terminate_vector;
	enum dos_exec_executor_status executor_status;
	enum dos_exec_transaction_status primary;
	enum dos_error error;
	uint8_t subfunction;

	if (transactions == NULL || services == NULL || state == NULL ||
	    result == NULL)
		return DOS_EXEC_INT21_INVALID_ARGUMENT;
	prepared.resume_state = *state;
	if (dos_register_high8(state->eax) != INT21_EXEC_FUNCTION) {
		prepared.status = (uint32_t)DOS_EXEC_INT21_NOT_EXEC_CALL;
		*result = prepared;
		return DOS_EXEC_INT21_NOT_EXEC_CALL;
	}
	subfunction = dos_register_low8(state->eax);
	if (!dos_exec_subfunction_is_valid(subfunction)) {
		error = DOS_ERROR_INVALID_FUNCTION;
		set_dos_error(&prepared.resume_state, error);
		prepared.status = (uint32_t)DOS_EXEC_INT21_DOS_ERROR;
		prepared.dos_error = (uint16_t)error;
		*result = prepared;
		return DOS_EXEC_INT21_DOS_ERROR;
	}
	if (subfunction != DOS_EXEC_LOAD_AND_EXECUTE) {
		prepared.status = (uint32_t)DOS_EXEC_INT21_UNIMPLEMENTED;
		*result = prepared;
		return DOS_EXEC_INT21_UNIMPLEMENTED;
	}

	request = (struct dos_exec_transaction_request){
		.executable_name = {
			.offset = dos_register_low16(state->edx),
			.segment = state->ds,
		},
		.parameter_block = {
			.offset = dos_register_low16(state->ebx),
			.segment = state->es,
		},
		.subfunction = subfunction,
		.reserved = {0u},
	};
	/* The precise software-interrupt event has already advanced IP. */
	terminate_vector = (struct dos_process_far_address){
		.offset = (uint16_t)state->eip,
		.segment = state->cs,
	};
	executor_status = dos_exec_executor_execute(
		transactions, services, &request, terminate_vector,
		&prepared.executor);
	if (executor_status == DOS_EXEC_EXECUTOR_OK) {
		prepared.resume_state.eflags &= ~DOS_EFLAGS_CF;
		prepared.status = (uint32_t)DOS_EXEC_INT21_CHILD_READY;
		*result = prepared;
		return DOS_EXEC_INT21_CHILD_READY;
	}
	primary = (enum dos_exec_transaction_status)
		prepared.executor.primary_status;
	if (executor_status == DOS_EXEC_EXECUTOR_TRANSACTION_FAILED &&
	    prepared.executor.cleanup_status == DOS_EXEC_TRANSACTION_OK &&
	    transaction_failure_is_dos_visible(primary)) {
		error = transaction_failure_to_dos_error(primary);
		set_dos_error(&prepared.resume_state, error);
		prepared.status = (uint32_t)DOS_EXEC_INT21_DOS_ERROR;
		prepared.dos_error = (uint16_t)error;
		*result = prepared;
		return DOS_EXEC_INT21_DOS_ERROR;
	}
	prepared.resume_state = *state;
	prepared.status = (uint32_t)DOS_EXEC_INT21_MACHINE_FAULT;
	*result = prepared;
	return DOS_EXEC_INT21_MACHINE_FAULT;
}

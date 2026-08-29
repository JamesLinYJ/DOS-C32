// SPDX-License-Identifier: GPL-2.0-only
/* MS-DOS-ordered, backend-neutral EXEC0 driver. */
#include "dos_exec_executor.h"

static enum dos_exec_executor_status cleanup_failed_transaction(
	struct dos_exec_transaction_table *transactions,
	const struct dos_exec_transaction_services *services,
	struct dos_exec_transaction_handle handle,
	enum dos_exec_transaction_status primary, uint32_t failure_detail,
	struct dos_exec_executor_result *result)
{
	struct dos_exec_executor_result prepared = {
		.session = {.value = 0u},
		.primary_status = (uint32_t)primary,
		.cleanup_status = (uint32_t)DOS_EXEC_TRANSACTION_OK,
		.failure_detail = failure_detail,
		.has_session = 0u,
		.reserved = {0u},
	};
	enum dos_exec_transaction_status cleanup;

	cleanup = dos_exec_transaction_abort(transactions, handle, services);
	if (cleanup == DOS_EXEC_TRANSACTION_OK)
		cleanup = dos_exec_transaction_retire(
			transactions, services->coordinator_identity, handle);
	prepared.cleanup_status = (uint32_t)cleanup;
	*result = prepared;
	return cleanup == DOS_EXEC_TRANSACTION_OK
		       ? DOS_EXEC_EXECUTOR_TRANSACTION_FAILED
		       : DOS_EXEC_EXECUTOR_CLEANUP_FAILED;
}

enum dos_exec_executor_status dos_exec_executor_execute(
	struct dos_exec_transaction_table *transactions,
	const struct dos_exec_transaction_services *services,
	const struct dos_exec_transaction_request *request,
	struct dos_process_far_address terminate_vector,
	struct dos_exec_executor_result *result)
{
	uint8_t executable_name[DOS_EXEC_PATH_CAPACITY];
	struct dos_exec_transaction_handle handle = {.value = 0u};
	struct dos_exec_environment_source_plan environment_source;
	struct dos_exec_transaction_environment environment;
	struct dos_load_plan image;
	struct dos_exec_transaction_target target;
	struct dos_exec_transaction_resident resident;
	struct dos_exec_transaction_relocation relocation;
	struct dos_exec_transaction_parent parent;
	struct dos_exec_transaction_inheritance inheritance;
	struct dos_exec_transaction_psp psp;
	struct dos_exec_handoff_plan handoff;
	struct dos_exec_backend_session_handle session = {.value = 0u};
	struct dos_exec_executor_result prepared;
	enum dos_exec_transaction_status status;
	uint32_t failure_detail = 0u;
	uint8_t is_device = 0u;

	if (transactions == NULL || services == NULL || request == NULL ||
	    result == NULL || request->subfunction != DOS_EXEC_LOAD_AND_EXECUTE)
		return DOS_EXEC_EXECUTOR_INVALID_ARGUMENT;
	status = dos_exec_transaction_begin(transactions, services, request,
					    &handle);
	if (status != DOS_EXEC_TRANSACTION_OK) {
		prepared = (struct dos_exec_executor_result){
			.session = {.value = 0u},
			.primary_status = (uint32_t)status,
			.cleanup_status = (uint32_t)DOS_EXEC_TRANSACTION_OK,
			.failure_detail = 0u,
			.has_session = 0u,
			.reserved = {0u},
		};
		*result = prepared;
		return DOS_EXEC_EXECUTOR_TRANSACTION_FAILED;
	}

	status = dos_exec_transaction_open(
		transactions, handle, services, executable_name,
		sizeof(executable_name), &failure_detail);
	if (status != DOS_EXEC_TRANSACTION_OK)
		goto failed;
	status = dos_exec_transaction_probe(transactions, handle, services,
					    &is_device, &failure_detail);
	if (status != DOS_EXEC_TRANSACTION_OK)
		goto failed;
	status = dos_exec_transaction_select_environment(
		transactions, handle, services, &environment_source);
	if (status != DOS_EXEC_TRANSACTION_OK)
		goto failed;
	status = dos_exec_transaction_prepare_environment(
		transactions, handle, services, &environment);
	if (status != DOS_EXEC_TRANSACTION_OK)
		goto failed;
	status = dos_exec_transaction_inspect_image(transactions, handle, services,
					    &image);
	if (status != DOS_EXEC_TRANSACTION_OK)
		goto failed;
	status = dos_exec_transaction_prepare_target(transactions, handle, services,
					     &target);
	if (status != DOS_EXEC_TRANSACTION_OK)
		goto failed;
	status = dos_exec_transaction_load_resident(transactions, handle, services,
					    &resident);
	if (status != DOS_EXEC_TRANSACTION_OK)
		goto failed;
	status = dos_exec_transaction_relocate_resident(
		transactions, handle, services, &relocation);
	if (status != DOS_EXEC_TRANSACTION_OK)
		goto failed;
	status = dos_exec_transaction_close(transactions, handle, services);
	if (status != DOS_EXEC_TRANSACTION_OK)
		goto failed;
	status = dos_exec_transaction_capture_parent(transactions, handle, services,
					     &parent);
	if (status != DOS_EXEC_TRANSACTION_OK)
		goto failed;
	status = dos_exec_transaction_prepare_inheritance(
		transactions, handle, services, &inheritance);
	if (status != DOS_EXEC_TRANSACTION_OK)
		goto failed;
	status = dos_exec_transaction_prepare_psp(
		transactions, handle, services, terminate_vector, &psp);
	if (status != DOS_EXEC_TRANSACTION_OK)
		goto failed;
	status = dos_exec_transaction_finalize_initial_state(
		transactions, handle, services, &resident);
	if (status != DOS_EXEC_TRANSACTION_OK)
		goto failed;
	status = dos_exec_transaction_stage_process_memory(transactions, handle,
						    services);
	if (status != DOS_EXEC_TRANSACTION_OK)
		goto failed;
	status = dos_exec_transaction_stage_global_memory(transactions, handle,
						   services);
	if (status != DOS_EXEC_TRANSACTION_OK)
		goto failed;
	status = dos_exec_transaction_prepare_handoff(transactions, handle, services,
					      &handoff);
	if (status != DOS_EXEC_TRANSACTION_OK)
		goto failed;
	status = dos_exec_transaction_prepare_backend(
		transactions, handle, services, &failure_detail);
	if (status != DOS_EXEC_TRANSACTION_OK)
		goto failed;
	status = dos_exec_transaction_seal_execute(transactions, handle, services,
					   &session);
	if (status != DOS_EXEC_TRANSACTION_OK)
		goto failed;

	prepared = (struct dos_exec_executor_result){
		.session = session,
		.primary_status = (uint32_t)DOS_EXEC_TRANSACTION_OK,
		.cleanup_status = (uint32_t)DOS_EXEC_TRANSACTION_OK,
		.failure_detail = 0u,
		.has_session = 1u,
		.reserved = {0u},
	};
	status = dos_exec_transaction_retire_published(transactions, handle,
						       services);
	prepared.cleanup_status = (uint32_t)status;
	*result = prepared;
	return status == DOS_EXEC_TRANSACTION_OK ? DOS_EXEC_EXECUTOR_OK
						 : DOS_EXEC_EXECUTOR_CLEANUP_FAILED;

failed:
	return cleanup_failed_transaction(transactions, services, handle, status,
					  failure_detail, result);
}

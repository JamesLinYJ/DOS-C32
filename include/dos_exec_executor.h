/* SPDX-License-Identifier: GPL-2.0-only */
/* One serialized driver for the complete EXEC0 transaction pipeline. */
#ifndef DOSC32_DOS_EXEC_EXECUTOR_H
#define DOSC32_DOS_EXEC_EXECUTOR_H

#include "compiler.h"
#include "dos_exec_transaction.h"
#include "types.h"

enum dos_exec_executor_status {
	DOS_EXEC_EXECUTOR_OK = 0,
	DOS_EXEC_EXECUTOR_INVALID_ARGUMENT,
	DOS_EXEC_EXECUTOR_TRANSACTION_FAILED,
	DOS_EXEC_EXECUTOR_CLEANUP_FAILED
};

/* Fixed diagnostic receipt; transaction enums are stored as uint32 values. */
struct dos_exec_executor_result {
	struct dos_exec_backend_session_handle session;
	uint32_t primary_status;
	uint32_t cleanup_status;
	uint32_t failure_detail;
	uint8_t has_session;
	uint8_t reserved[3];
} __aligned(8);

/*
 * Drives the exact common transaction from BEGIN through a runnable EXEC0
 * backend.  Every pre-publication failure is aborted and retired here, so
 * native COMMAND and INT 21h/AH=4Bh cannot grow different cleanup paths.
 * The function starts no guest instruction.
 */
enum dos_exec_executor_status dos_exec_executor_execute(
	struct dos_exec_transaction_table *transactions,
	const struct dos_exec_transaction_services *services,
	const struct dos_exec_transaction_request *request,
	struct dos_process_far_address terminate_vector,
	struct dos_exec_executor_result *result) __must_check;

static_assert_expression(sizeof(struct dos_exec_executor_result) == 24u,
			 "EXEC executor results must stay fixed width");

#endif

/* SPDX-License-Identifier: GPL-2.0-only */
/* Native COMMAND to standard MS-DOS EXEC0 request boundary. */
#ifndef DOSC32_DOS_EXEC_NATIVE_H
#define DOSC32_DOS_EXEC_NATIVE_H

#include "compiler.h"
#include "dos_exec_executor.h"
#include "types.h"

/* The caller supplies the exact bytes exposed at child PSP:81h. */
struct dos_exec_native_request {
	const uint8_t *executable_name;
	size_t executable_name_length;
	const uint8_t *command_tail;
	size_t command_tail_capacity;
	size_t command_tail_length;
};

enum dos_exec_native_status {
	DOS_EXEC_NATIVE_OK = 0,
	DOS_EXEC_NATIVE_INVALID_ARGUMENT,
	DOS_EXEC_NATIVE_ALLOCATION_FAILED,
	DOS_EXEC_NATIVE_STAGING_FAILED,
	DOS_EXEC_NATIVE_EXEC_FAILED,
	DOS_EXEC_NATIVE_CLEANUP_FAILED
};

struct dos_exec_native_result {
	struct dos_exec_executor_result executor;
	uint32_t allocation_status;
	uint32_t staging_status;
	uint32_t cleanup_status;
	uint16_t scratch_segment;
	uint16_t maximum_available;
} __aligned(8);

/*
 * This is an ABI adapter, not a second loader.  It constructs the same Exec0
 * block used by INT 21h/AH=4Bh in a temporary parent-owned MCB, invokes the
 * common executor, then releases only that exact MCB.
 */
enum dos_exec_native_status dos_exec_native_execute(
	struct dos_exec_transaction_table *transactions,
	const struct dos_exec_transaction_services *services,
	const struct dos_exec_native_request *request,
	struct dos_process_far_address terminate_vector,
	struct dos_exec_native_result *result) __must_check;

static_assert_expression(sizeof(struct dos_exec_native_result) == 40u,
			 "native EXEC results must stay fixed width");

#endif

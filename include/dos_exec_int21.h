/* SPDX-License-Identifier: GPL-2.0-only */
/* INT 21h/AH=4Bh decoder for the common DOS EXEC transaction. */
#ifndef DOSC32_DOS_EXEC_INT21_H
#define DOSC32_DOS_EXEC_INT21_H

#include "compiler.h"
#include "dos_error.h"
#include "dos_exec_executor.h"
#include "types.h"

enum dos_exec_int21_status {
	DOS_EXEC_INT21_NOT_EXEC_CALL = 0,
	DOS_EXEC_INT21_CHILD_READY,
	DOS_EXEC_INT21_DOS_ERROR,
	DOS_EXEC_INT21_UNIMPLEMENTED,
	DOS_EXEC_INT21_MACHINE_FAULT,
	DOS_EXEC_INT21_INVALID_ARGUMENT
};

struct dos_exec_int21_result {
	struct dos_cpu_state resume_state;
	struct dos_exec_executor_result executor;
	uint32_t status;
	uint16_t dos_error;
	uint8_t reserved[2];
} __aligned(8);

/*
 * A successful EXEC0 publishes a runnable child session and a dormant parent
 * resume state.  It does not execute either session.  EXEC1/EXEC3 remain
 * explicitly untranslated and leave the supplied register state unchanged.
 */
enum dos_exec_int21_status dos_exec_int21_execute(
	struct dos_exec_transaction_table *transactions,
	const struct dos_exec_transaction_services *services,
	const struct dos_cpu_state *state,
	struct dos_exec_int21_result *result) __must_check;

static_assert_expression(sizeof(struct dos_exec_int21_result) == 88u,
			 "INT 21h EXEC results must stay fixed width");

#endif

/* SPDX-License-Identifier: GPL-2.0-only */
/* One bounded guest-backend -> DOS service -> resume transition. */
#ifndef DOSC32_DOS_EXECUTION_LOOP_H
#define DOSC32_DOS_EXECUTION_LOOP_H

#include "compiler.h"
#include "dos_exec_backend_session.h"
#include "dos_exec_int21.h"
#include "dos_personality.h"

enum dos_execution_step_status {
	DOS_EXECUTION_STEP_SERVICE_RESUMED = 0,
	DOS_EXECUTION_STEP_PORT_RESUMED,
	/* CPU state is resumable only after an interrupt wakes backend HLT. */
	DOS_EXECUTION_STEP_HALTED,
	DOS_EXECUTION_STEP_EVENT_PENDING,
	DOS_EXECUTION_STEP_CHAIN_RESUMED,
	DOS_EXECUTION_STEP_BLOCKED,
	DOS_EXECUTION_STEP_PROCESS_EXITED,
	DOS_EXECUTION_STEP_MACHINE_FAULT,
	DOS_EXECUTION_STEP_SESSION_ERROR,
	DOS_EXECUTION_STEP_INVALID_ARGUMENT,
	/* Parent state is saved; owner must schedule child_session next. */
	DOS_EXECUTION_STEP_CHILD_STARTED,
	/* A typed platform handoff consumed the old service frame. */
	DOS_EXECUTION_STEP_EXECUTION_TRANSFERRED
};

/* Borrowed only for one bounded step; no binding is retained by a session. */
struct dos_execution_exec_binding {
	struct dos_exec_transaction_table *transactions;
	const struct dos_exec_transaction_services *services;
	enum dos_exec_int21_status (*execute)(
		struct dos_exec_transaction_table *transactions,
		const struct dos_exec_transaction_services *services,
		const struct dos_cpu_state *state,
		struct dos_exec_int21_result *result);
};

/*
 * Value-only result of one bounded step.  EVENT_PENDING deliberately returns
 * to the owner instead of hiding an unbounded run loop in the DOS dispatcher.
 */
struct dos_execution_step_result {
	struct dos_cpu_state state;
	struct dos_execution_event event;
	uint32_t status;
	uint32_t session_status;
	struct dos_interrupt_result interrupt;
	struct dos_exec_backend_session_handle child_session;
} __aligned(8);

static_assert_expression(sizeof(struct dos_execution_step_result) == 96,
			 "execution step results must be data-model independent");

struct dos_execution_step_result dos_execution_step(
	struct dos_exec_backend_session_table *table,
	struct dos_exec_backend_session_handle session,
	const struct dos_exec_backend_ops *ops,
	kernel_object_handle_t adapter_context,
	kernel_object_handle_t machine_identity,
	const struct dos_machine *machine,
	struct dos_personality *personality) __must_check;

struct dos_execution_step_result dos_execution_step_with_exec(
	struct dos_exec_backend_session_table *table,
	struct dos_exec_backend_session_handle session,
	const struct dos_exec_backend_ops *ops,
	kernel_object_handle_t adapter_context,
	kernel_object_handle_t machine_identity,
	const struct dos_machine *machine,
	struct dos_personality *personality,
	const struct dos_execution_exec_binding *exec_binding) __must_check;

#endif

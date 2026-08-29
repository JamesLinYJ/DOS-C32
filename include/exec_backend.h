/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * DOS instruction-execution adapter boundary.
 *
 * The adapter selects VM86, interpretation, or a protected guest mechanism;
 * it does not implement DOS services.  Preparation must leave the backend
 * dormant and must not execute even one guest instruction.
 */
#ifndef DOSC32_EXEC_BACKEND_H
#define DOSC32_EXEC_BACKEND_H

#include "compiler.h"
#include "dos_exec_handoff.h"
#include "dos_machine.h"
#include "types.h"

enum dos_exec_capability {
	DOS_EXEC_CAP_VM86 = 1u << 0,
	DOS_EXEC_CAP_INSTRUCTION_EMULATION = 1u << 1,
	DOS_EXEC_CAP_PROTECTED_MODE_GUEST = 1u << 2,
	DOS_EXEC_CAP_GUEST_VIRTUAL_MACHINES = 1u << 3
};

#define DOS_EXEC_CAPABILITY_MASK                                               \
	((uint32_t)(DOS_EXEC_CAP_VM86 | DOS_EXEC_CAP_INSTRUCTION_EMULATION |    \
		    DOS_EXEC_CAP_PROTECTED_MODE_GUEST |                        \
		    DOS_EXEC_CAP_GUEST_VIRTUAL_MACHINES))

enum dos_execution_event_kind {
	DOS_EXEC_EVENT_SOFTWARE_INTERRUPT = 0,
	DOS_EXEC_EVENT_EXCEPTION,
	DOS_EXEC_EVENT_PORT_IO,
	DOS_EXEC_EVENT_MODE_CHANGE,
	DOS_EXEC_EVENT_HALTED,
	DOS_EXEC_EVENT_EXITED,
	DOS_EXEC_EVENT_FAULT
};

/* Fixed-width result of one precise execution stop. */
struct dos_execution_event {
	uint32_t kind;
	uint32_t value;
	uint16_t port;
	uint8_t vector;
	uint8_t io_width;
	uint8_t io_write;
	uint8_t reserved[3];
} __aligned(8);

enum dos_exec_backend_prepare_status {
	DOS_EXEC_BACKEND_PREPARED = 0,
	/* Certainly rejected before ownership or guest execution. */
	DOS_EXEC_BACKEND_REJECTED,
	/* Ownership cannot be proved absent or present. */
	DOS_EXEC_BACKEND_PREPARE_UNCERTAIN
};

enum dos_exec_backend_release_status {
	DOS_EXEC_BACKEND_RELEASED = 0,
	DOS_EXEC_BACKEND_RETAINED,
	DOS_EXEC_BACKEND_RELEASE_UNCERTAIN
};

enum dos_exec_backend_run_status {
	/* CPU state and event describe one precise stop. */
	DOS_EXEC_BACKEND_EVENT = 0,
	/* Execution may have advanced without a precise recoverable state. */
	DOS_EXEC_BACKEND_RUN_UNCERTAIN
};

enum dos_exec_backend_state_commit_status {
	/* Backend-private precise-stop state accepted the replacement. */
	DOS_EXEC_BACKEND_STATE_COMMITTED = 0,
	/* Certainly rejected without changing backend-private state. */
	DOS_EXEC_BACKEND_STATE_REJECTED,
	/* Backend-private state may have changed and must be quarantined. */
	DOS_EXEC_BACKEND_STATE_COMMIT_UNCERTAIN
};

struct dos_exec_backend_prepare_result {
	kernel_object_handle_t backend_context;
	uint32_t failure_detail;
	uint8_t reserved[4];
} __aligned(8);

/*
 * Every pointer is borrowed for one call.  backend_context is an opaque,
 * generation-pinned integer capability.  PREPARED guarantees a dormant
 * backend.  REJECTED guarantees no acquired backend and may carry an opaque
 * failure detail.  Any malformed or unknown result is ownership-uncertain.
 *
 * run_until_event is not used during EXEC publication.  It starts only after
 * the session manager makes a dormant session runnable.  Precise faults are
 * returned as an EVENT of kind FAULT; a non-precise failure returns
 * RUN_UNCERTAIN and causes fail-closed quarantine.
 */
struct dos_exec_backend_ops {
	kernel_object_handle_t identity;
	uint32_t capabilities;
	enum dos_exec_backend_prepare_status (*prepare)(
	    kernel_object_handle_t context, const struct dos_machine *machine,
	    kernel_object_handle_t machine_identity,
	    const struct dos_exec_handoff_plan *handoff,
	    struct dos_exec_backend_prepare_result *result);
	enum dos_exec_backend_release_status (*release)(
	    kernel_object_handle_t context,
	    kernel_object_handle_t backend_context);
	enum dos_exec_backend_run_status (*run_until_event)(
	    kernel_object_handle_t context,
	    kernel_object_handle_t backend_context,
	    kernel_object_handle_t machine_identity,
	    const struct dos_machine *machine, struct dos_cpu_state *state,
	    struct dos_execution_event *event);
	/*
	 * Optional precise-stop commit hook.  A backend with no private state tied
	 * to service completion leaves this NULL.  The session calls it only after
	 * validating the expected state and before publishing the replacement.
	 */
	enum dos_exec_backend_state_commit_status (*commit_state_replacement)(
	    kernel_object_handle_t context,
	    kernel_object_handle_t backend_context,
	    const struct dos_cpu_state *expected_state,
	    const struct dos_cpu_state *replacement_state);
};

static_assert_expression(sizeof(struct dos_execution_event) == 16,
			 "execution events must be fixed width");
static_assert_expression(sizeof(struct dos_exec_backend_prepare_result) == 16,
			 "backend prepare results must be fixed width");

#endif

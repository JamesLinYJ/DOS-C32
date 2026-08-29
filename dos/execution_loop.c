// SPDX-License-Identifier: GPL-2.0-only
/*
 * Bounded backend-independent DOS execution step.
 *
 * The backend executes instructions; the personality owns MS-DOS semantics.
 */
#include "dos_execution_loop.h"

#include "dos_interrupt_reflection.h"

static struct dos_execution_step_result empty_result(
	enum dos_execution_step_status status)
{
	struct dos_execution_step_result result = {0};

	result.status = (uint32_t)status;
	result.session_status =
		(uint32_t)DOS_EXEC_BACKEND_SESSION_INVALID_ARGUMENT;
	result.interrupt.disposition = DOS_INTERRUPT_MACHINE_FAULT;
	result.interrupt.machine_status = DOS_MACHINE_INVALID_ARGUMENT;
	return result;
}

static enum dos_machine_status service_port_event(
	const struct dos_machine *machine, struct dos_execution_event *event,
	struct dos_cpu_state *state)
{
	enum dos_io_width width = (enum dos_io_width)event->io_width;
	uint32_t value = 0u;
	enum dos_machine_status status;

	if (event->io_write != 0u)
		return dos_machine_write_port(machine, event->port, width,
					      event->value);
	status = dos_machine_read_port(machine, event->port, width, &value);
	if (status != DOS_MACHINE_OK)
		return status;
	if (width == DOS_IO_WIDTH_8)
		dos_register_set_low8(&state->eax, (uint8_t)value);
	else if (width == DOS_IO_WIDTH_16)
		dos_register_set_low16(&state->eax, (uint16_t)value);
	else
		state->eax = value;
	event->value = value;
	return DOS_MACHINE_OK;
}

struct dos_execution_step_result dos_execution_step_with_exec(
	struct dos_exec_backend_session_table *table,
	struct dos_exec_backend_session_handle session,
	const struct dos_exec_backend_ops *ops,
	kernel_object_handle_t adapter_context,
	kernel_object_handle_t machine_identity,
	const struct dos_machine *machine, struct dos_personality *personality,
	const struct dos_execution_exec_binding *exec_binding)
{
	struct dos_execution_step_result result =
		empty_result(DOS_EXECUTION_STEP_INVALID_ARGUMENT);
	struct dos_cpu_state serviced_state;
	struct dos_interrupt_reflection_result reflection;
	struct dos_exec_int21_result exec_result;
	enum dos_exec_int21_status exec_status;
	enum dos_exec_backend_session_status session_status;

	if (table == NULL || ops == NULL || machine == NULL ||
	    personality == NULL)
		return result;
	session_status = dos_exec_backend_session_run_until_event(
		table, session, ops, adapter_context, machine_identity, machine,
		&result.state, &result.event);
	result.session_status = (uint32_t)session_status;
	if (session_status != DOS_EXEC_BACKEND_SESSION_OK) {
		result.status = (uint32_t)DOS_EXECUTION_STEP_SESSION_ERROR;
		return result;
	}
	if (result.event.kind == (uint32_t)DOS_EXEC_EVENT_PORT_IO) {
		serviced_state = result.state;
		result.interrupt.machine_status = service_port_event(
			machine, &result.event, &serviced_state);
		if (result.interrupt.machine_status == DOS_MACHINE_IO_DENIED) {
			result.interrupt.disposition = DOS_INTERRUPT_BLOCKED;
			result.status = (uint32_t)DOS_EXECUTION_STEP_BLOCKED;
			return result;
		}
		if (result.interrupt.machine_status != DOS_MACHINE_OK) {
			result.interrupt.disposition =
				DOS_INTERRUPT_MACHINE_FAULT;
			result.status =
				(uint32_t)DOS_EXECUTION_STEP_MACHINE_FAULT;
			return result;
		}
		session_status = dos_exec_backend_session_replace_state(
			table, session, ops, adapter_context, machine_identity,
			machine, &result.state, &serviced_state);
		result.session_status = (uint32_t)session_status;
		if (session_status != DOS_EXEC_BACKEND_SESSION_OK) {
			result.status =
				(uint32_t)DOS_EXECUTION_STEP_SESSION_ERROR;
			return result;
		}
		result.state = serviced_state;
		result.interrupt.disposition = DOS_INTERRUPT_HANDLED;
		result.status = (uint32_t)DOS_EXECUTION_STEP_PORT_RESUMED;
		return result;
	}
	if (result.event.kind == (uint32_t)DOS_EXEC_EVENT_HALTED) {
		result.status = (uint32_t)DOS_EXECUTION_STEP_HALTED;
		return result;
	}
	if (result.event.kind != (uint32_t)DOS_EXEC_EVENT_SOFTWARE_INTERRUPT) {
		result.status = (uint32_t)DOS_EXECUTION_STEP_EVENT_PENDING;
		return result;
	}
	if (result.event.vector == 0x21u && exec_binding != NULL &&
	    exec_binding->transactions != NULL && exec_binding->services != NULL &&
	    exec_binding->execute != NULL &&
	    dos_register_high8(result.state.eax) == 0x4bu) {
		exec_status = exec_binding->execute(
			exec_binding->transactions, exec_binding->services,
			&result.state, &exec_result);
		if (exec_status == DOS_EXEC_INT21_CHILD_READY ||
		    exec_status == DOS_EXEC_INT21_DOS_ERROR) {
			session_status = dos_exec_backend_session_replace_state(
				table, session, ops, adapter_context,
				machine_identity, machine, &result.state,
				&exec_result.resume_state);
			result.session_status = (uint32_t)session_status;
			if (session_status != DOS_EXEC_BACKEND_SESSION_OK) {
				result.status =
					(uint32_t)DOS_EXECUTION_STEP_SESSION_ERROR;
				return result;
			}
			result.state = exec_result.resume_state;
			result.interrupt.disposition = DOS_INTERRUPT_HANDLED;
			result.interrupt.machine_status = DOS_MACHINE_OK;
			if (exec_status == DOS_EXEC_INT21_CHILD_READY) {
				result.child_session =
					exec_result.executor.session;
				result.status = (uint32_t)
					DOS_EXECUTION_STEP_CHILD_STARTED;
			} else {
				result.status = (uint32_t)
					DOS_EXECUTION_STEP_SERVICE_RESUMED;
			}
			return result;
		}
		if (exec_status == DOS_EXEC_INT21_MACHINE_FAULT ||
		    exec_status == DOS_EXEC_INT21_INVALID_ARGUMENT) {
			result.interrupt.disposition =
				DOS_INTERRUPT_MACHINE_FAULT;
			result.interrupt.machine_status = DOS_MACHINE_IO_FAULT;
			result.status =
				(uint32_t)DOS_EXECUTION_STEP_MACHINE_FAULT;
			return result;
		}
		/* EXEC1/EXEC3 are real DOS functions; the ordinary personality
		 * keeps them blocked until their common executor modes exist. */
	}

	serviced_state = result.state;
	result.interrupt = dos_personality_interrupt(
		personality, machine, machine_identity, result.event.vector,
		&serviced_state);
	switch (result.interrupt.disposition) {
	case DOS_INTERRUPT_HANDLED:
		session_status = dos_exec_backend_session_replace_state(
			table, session, ops, adapter_context, machine_identity,
			machine, &result.state, &serviced_state);
		result.session_status = (uint32_t)session_status;
		if (session_status != DOS_EXEC_BACKEND_SESSION_OK) {
			result.status =
				(uint32_t)DOS_EXECUTION_STEP_SESSION_ERROR;
			return result;
		}
		result.state = serviced_state;
		result.status =
			(uint32_t)DOS_EXECUTION_STEP_SERVICE_RESUMED;
		return result;
	case DOS_INTERRUPT_CHAIN:
		reflection = dos_interrupt_reflect(machine, result.event.vector,
						   &result.state);
		if (reflection.status !=
		    (uint32_t)DOS_INTERRUPT_REFLECTION_OK) {
			result.interrupt.disposition =
				DOS_INTERRUPT_MACHINE_FAULT;
			result.interrupt.machine_status =
				(enum dos_machine_status)reflection.machine_status;
			result.status =
				(uint32_t)DOS_EXECUTION_STEP_MACHINE_FAULT;
			return result;
		}
		session_status = dos_exec_backend_session_replace_state(
			table, session, ops, adapter_context, machine_identity,
			machine, &result.state, &reflection.state);
		result.session_status = (uint32_t)session_status;
		if (session_status != DOS_EXEC_BACKEND_SESSION_OK) {
			result.status =
				(uint32_t)DOS_EXECUTION_STEP_SESSION_ERROR;
			return result;
		}
		result.state = reflection.state;
		result.status = (uint32_t)DOS_EXECUTION_STEP_CHAIN_RESUMED;
		return result;
	case DOS_INTERRUPT_BLOCKED:
		result.status = (uint32_t)DOS_EXECUTION_STEP_BLOCKED;
		return result;
	case DOS_INTERRUPT_PROCESS_EXITED:
		result.status =
			(uint32_t)DOS_EXECUTION_STEP_PROCESS_EXITED;
		return result;
	case DOS_INTERRUPT_MACHINE_FAULT:
		result.status = (uint32_t)DOS_EXECUTION_STEP_MACHINE_FAULT;
		return result;
	case DOS_INTERRUPT_EXECUTION_TRANSFERRED:
		/* The old register frame is no longer a resumable session state. */
		result.state = serviced_state;
		result.status =
			(uint32_t)DOS_EXECUTION_STEP_EXECUTION_TRANSFERRED;
		return result;
	}
	result.status = (uint32_t)DOS_EXECUTION_STEP_MACHINE_FAULT;
	result.interrupt.machine_status = DOS_MACHINE_INVALID_ARGUMENT;
	return result;
}

struct dos_execution_step_result dos_execution_step(
	struct dos_exec_backend_session_table *table,
	struct dos_exec_backend_session_handle session,
	const struct dos_exec_backend_ops *ops,
	kernel_object_handle_t adapter_context,
	kernel_object_handle_t machine_identity,
	const struct dos_machine *machine, struct dos_personality *personality)
{
	return dos_execution_step_with_exec(
		table, session, ops, adapter_context, machine_identity, machine,
		personality, NULL);
}

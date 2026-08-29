// SPDX-License-Identifier: GPL-2.0-only
/* Test-image-only direct VM86 -> DOS personality integration probe. */
#include "x86_vm86.h"

#include "console.h"
#include "dos_execution_loop.h"
#include "dos_memory.h"
#include "dos_personality.h"
#include "x86_guest_space.h"

#define SELFTEST_SEGMENT 0x3000u
#define SELFTEST_STACK_SEGMENT 0x3800u
#define SELFTEST_STACK_POINTER 0xfff0u
#define SELFTEST_ARENA_HEAD 0x3900u
#define SELFTEST_ARENA_END 0x3920u
#define SELFTEST_STEP_LIMIT 32u
#define SELFTEST_TIMER_CHAIN_VECTOR 0x1cu

static const struct dos_int21_drive_config selftest_drive_config = {
	.available_drive_mask = (uint32_t)1u << 2u,
	.current_drive = 2u,
	.boot_drive = 3u,
	.last_drive = 3u,
	.reserved = 0u,
};

static void selftest_serial_write_u32(uint32_t value)
{
	char digits[10];
	size_t count = 0u;

	if (value == 0u) {
		console_serial_write("0", 1u);
		return;
	}
	while (value != 0u) {
		digits[count++] = (char)('0' + value % 10u);
		value /= 10u;
	}
	while (count != 0u)
		console_serial_write(&digits[--count], 1u);
}

static void selftest_report_failure(
	uint32_t stage, uint32_t failure_detail,
	const struct dos_execution_step_result *first,
	const struct dos_execution_step_result *second,
	const struct dos_execution_step_result *third)
{
	console_serial_write("[vm86-selftest] stage=",
			     sizeof("[vm86-selftest] stage=") - 1u);
	selftest_serial_write_u32(stage);
	console_serial_write(" detail=", sizeof(" detail=") - 1u);
	selftest_serial_write_u32(failure_detail);
	console_serial_write(" steps=", sizeof(" steps=") - 1u);
	selftest_serial_write_u32(first->status);
	console_serial_write(",", 1u);
	selftest_serial_write_u32(second->status);
	console_serial_write(",", 1u);
	selftest_serial_write_u32(third->status);
	console_serial_write(" sessions=", sizeof(" sessions=") - 1u);
	selftest_serial_write_u32(first->session_status);
	console_serial_write(",", 1u);
	selftest_serial_write_u32(second->session_status);
	console_serial_write(",", 1u);
	selftest_serial_write_u32(third->session_status);
	console_serial_write(" kinds=", sizeof(" kinds=") - 1u);
	selftest_serial_write_u32(first->event.kind);
	console_serial_write(",", 1u);
	selftest_serial_write_u32(second->event.kind);
	console_serial_write(",", 1u);
	selftest_serial_write_u32(third->event.kind);
	console_serial_write(" vectors=", sizeof(" vectors=") - 1u);
	selftest_serial_write_u32(first->event.vector);
	console_serial_write(",", 1u);
	selftest_serial_write_u32(second->event.vector);
	console_serial_write(",", 1u);
	selftest_serial_write_u32(third->event.vector);
	console_serial_write(" port=", sizeof(" port=") - 1u);
	selftest_serial_write_u32(third->event.port);
	console_serial_write(" write=", sizeof(" write=") - 1u);
	selftest_serial_write_u32(third->event.io_write);
	console_serial_write(" width=", sizeof(" width=") - 1u);
	selftest_serial_write_u32(third->event.io_width);
	console_serial_write(" value=", sizeof(" value=") - 1u);
	selftest_serial_write_u32(third->event.value);
	console_serial_write(" ips=", sizeof(" ips=") - 1u);
	selftest_serial_write_u32(first->state.eip);
	console_serial_write(",", 1u);
	selftest_serial_write_u32(second->state.eip);
	console_serial_write(",", 1u);
	selftest_serial_write_u32(third->state.eip);
	console_serial_write("\n", 1u);
}

bool x86_vm86_boot_self_test(
	kernel_object_handle_t session_table_identity,
	kernel_object_handle_t personality_identity,
	kernel_object_handle_t runtime_identity,
	kernel_object_handle_t memory_arena_identity)
{
	static const uint8_t program[] = {
		0xb8u, 0x00u, 0x30u, /* MOV AX,3000h: get DOS version */
		0xcdu, 0x21u,       /* INT 21h */
		0xcdu, 0x60u,       /* reflected private interrupt */
		0xfbu,              /* STI: set guest-logical IF */
		0xfau,              /* CLI: clear guest-logical IF */
		0x9cu, 0x59u,       /* PUSHF; POP CX: observe IF clear */
		0xfbu,              /* STI */
		0x9cu, 0x5au,       /* PUSHF; POP DX: observe IF set */
		0x9cu, 0x9du,       /* PUSHF; POPF */
		0x9cu, 0x0eu,       /* PUSHF; PUSH CS */
		0x68u, 0x16u, 0x00u, /* PUSH 0016h: IRET target */
		0xcfu,              /* IRET */
		0xbbu, 0x78u, 0x56u, /* MOV BX,5678h: proves resume */
		0xe4u, 0x80u,       /* IN AL,80h: absent bus returns FFh */
		0x46u, 0xcfu,       /* INT 60h handler: INC SI; IRET */
	};
	static const uint8_t reflected_vector[] = {
		0x1bu, 0x00u, 0x00u, 0x30u, /* 3000:001b */
	};
	const struct dos_machine *machine = x86_guest_space_machine();
	const struct dos_exec_backend_ops *ops = x86_vm86_backend_ops();
	kernel_object_handle_t adapter_context = x86_vm86_backend_context();
	kernel_object_handle_t machine_identity =
		x86_guest_space_machine_identity();
	struct dos_exec_backend_session_table sessions =
		DOS_EXEC_BACKEND_SESSION_TABLE_INITIALIZER;
	struct dos_exec_backend_session_handle session = {0};
	struct dos_memory_arena arena =
		DOS_MEMORY_ARENA_INITIALIZER(memory_arena_identity);
	struct dos_personality personality = {0};
	struct dos_cpu_state state = {
		.edx = SELFTEST_SEGMENT,
		.esi = 0u,
		.edi = SELFTEST_STACK_POINTER,
		.esp = SELFTEST_STACK_POINTER,
		.eip = 0u,
		.eflags = DOS_EFLAGS_RESERVED_ONE,
		.cs = SELFTEST_SEGMENT,
		.ss = SELFTEST_STACK_SEGMENT,
		.ds = SELFTEST_SEGMENT,
		.es = SELFTEST_SEGMENT,
		.mode = (uint32_t)DOS_CPU_REAL16,
	};
	struct dos_exec_handoff_plan handoff = {
		.entry_state = state,
		.stack_image = {
			.segment = SELFTEST_STACK_SEGMENT,
			.offset = (uint16_t)(SELFTEST_STACK_POINTER -
					    DOS_EXEC_HANDOFF_STACK_BYTES),
			.bytes = {0x00u, 0x00u, 0x00u, 0x30u},
		},
		.child_psp = SELFTEST_SEGMENT,
		.format = (uint8_t)DOS_IMAGE_COM,
		.stack_word_count = DOS_EXEC_HANDOFF_STACK_WORDS,
		.reserved = {0u},
	};
	struct dos_execution_step_result service_step = {0};
	struct dos_execution_step_result reflected_step = {0};
	struct dos_execution_step_result port_step = {0};
	struct dos_execution_step_result step = {0};
	uint8_t original[sizeof(program)];
	uint8_t original_arena[DOS_MEMORY_MCB_BYTES];
	uint8_t original_stack[6u];
	uint8_t original_vector[sizeof(reflected_vector)];
	dos_linear_address_t linear =
		dos_far_to_linear(SELFTEST_SEGMENT, 0u, false);
	dos_linear_address_t arena_linear =
		dos_far_to_linear(SELFTEST_ARENA_HEAD, 0u, false);
	dos_linear_address_t vector_linear = 0x60u * 4u;
	uint32_t failure_detail = 0u;
	uint32_t stage = 1u;
	uint32_t step_count = 0u;
	bool session_acquired = false;
	bool service_observed = false;
	bool reflected_observed = false;
	bool port_observed = false;
	bool unexpected_event = false;
	bool passed = false;

	if (machine == NULL || ops == NULL ||
	    adapter_context == KERNEL_OBJECT_HANDLE_INVALID ||
	    machine_identity == KERNEL_OBJECT_HANDLE_INVALID ||
	    !dos_exec_handoff_plan_has_valid_encoding(&handoff) ||
	    dos_machine_read(machine, linear, original, sizeof(original),
			     sizeof(original)) != DOS_MACHINE_OK ||
	    dos_machine_read(machine, arena_linear, original_arena,
			     sizeof(original_arena), sizeof(original_arena)) !=
		DOS_MACHINE_OK ||
	    dos_machine_read_far(
		machine, SELFTEST_STACK_SEGMENT,
		(uint16_t)(SELFTEST_STACK_POINTER - sizeof(original_stack)),
		original_stack, sizeof(original_stack), sizeof(original_stack)) !=
		DOS_MACHINE_OK ||
	    dos_machine_read(machine, vector_linear, original_vector,
			     sizeof(original_vector), sizeof(original_vector)) !=
		DOS_MACHINE_OK ||
	    dos_machine_write(machine, vector_linear, reflected_vector,
			      sizeof(reflected_vector),
			      sizeof(reflected_vector)) != DOS_MACHINE_OK ||
	    dos_machine_write(machine, linear, program, sizeof(program),
			      sizeof(program)) != DOS_MACHINE_OK)
		return false;

	if (dos_memory_arena_initialize(&arena, machine, SELFTEST_ARENA_HEAD,
					SELFTEST_ARENA_END) == DOS_SUCCESS &&
	    dos_personality_initialize(
		&personality, personality_identity, machine_identity, machine,
		&arena, runtime_identity, SELFTEST_SEGMENT,
		&selftest_drive_config) ==
		DOS_PERSONALITY_READY &&
	    dos_exec_backend_session_table_construct(&sessions) ==
		DOS_EXEC_BACKEND_SESSION_OK &&
	    dos_exec_backend_session_table_initialize(
		&sessions, session_table_identity) ==
		DOS_EXEC_BACKEND_SESSION_OK &&
	    dos_exec_backend_session_prepare(
		&sessions, ops, adapter_context, machine_identity, machine,
		&handoff, &session, &failure_detail) ==
		DOS_EXEC_BACKEND_SESSION_OK) {
		session_acquired = true;
		stage = 2u;
		if (dos_exec_backend_session_publish(
			&sessions, session, ops, adapter_context,
			machine_identity, machine, &handoff) ==
		    DOS_EXEC_BACKEND_SESSION_OK) {
			stage = 3u;
			while (step_count < SELFTEST_STEP_LIMIT &&
			       !port_observed && !unexpected_event) {
				step = dos_execution_step(
					&sessions, session, ops, adapter_context,
					machine_identity, machine, &personality);
				++step_count;
				if (step.status == (uint32_t)
						   DOS_EXECUTION_STEP_SERVICE_RESUMED &&
				    step.event.vector == 0x21u &&
				    !service_observed) {
					service_step = step;
					service_observed = true;
					continue;
				}
				if (step.status == (uint32_t)
						   DOS_EXECUTION_STEP_CHAIN_RESUMED &&
				    step.event.vector ==
					    SELFTEST_TIMER_CHAIN_VECTOR)
					continue;
				if (step.status == (uint32_t)
						   DOS_EXECUTION_STEP_CHAIN_RESUMED &&
				    step.event.vector == 0x60u &&
				    service_observed && !reflected_observed) {
					reflected_step = step;
					reflected_observed = true;
					continue;
				}
				/* A hardware IRQ0 is acknowledged by the supervisor before
				 * reflection. The BIOS handler still emits its guest-visible
				 * master-PIC EOI; the machine model consumes it without touching
				 * host hardware. The test program contains no OUT instruction. */
				if (step.status == (uint32_t)
						   DOS_EXECUTION_STEP_PORT_RESUMED &&
				    step.event.port == 0x20u &&
				    step.event.io_write == 1u &&
				    step.event.io_width == (uint8_t)DOS_IO_WIDTH_8 &&
				    step.event.value == 0x20u)
					continue;
				if (step.status == (uint32_t)
						   DOS_EXECUTION_STEP_PORT_RESUMED &&
				    step.event.kind ==
					    (uint32_t)DOS_EXEC_EVENT_PORT_IO &&
				    step.event.port == 0x80u &&
				    reflected_observed) {
					port_step = step;
					port_observed = true;
					continue;
				}
				port_step = step;
				unexpected_event = true;
			}
			stage = 6u;
			passed = !unexpected_event && service_observed &&
				 reflected_observed && port_observed &&
				 service_step.status ==
					 (uint32_t)
						 DOS_EXECUTION_STEP_SERVICE_RESUMED &&
				 service_step.event.vector == 0x21u &&
				 service_step.state.eip == 5u &&
				 dos_register_low16(service_step.state.eax) ==
					 0x1706u &&
				 reflected_step.status ==
					 (uint32_t)
						 DOS_EXECUTION_STEP_CHAIN_RESUMED &&
				 reflected_step.event.vector == 0x60u &&
				 reflected_step.state.cs == SELFTEST_SEGMENT &&
				 reflected_step.state.eip == 27u &&
				 reflected_step.state.esp ==
					 SELFTEST_STACK_POINTER - 6u &&
				 port_step.status ==
					 (uint32_t)DOS_EXECUTION_STEP_PORT_RESUMED &&
				 port_step.event.kind ==
					 (uint32_t)DOS_EXEC_EVENT_PORT_IO &&
				 port_step.event.port == 0x80u &&
				 port_step.event.io_width ==
					 (uint8_t)DOS_IO_WIDTH_8 &&
				 port_step.interrupt.machine_status ==
					 DOS_MACHINE_OK &&
				 port_step.event.value == 0xffu &&
				 dos_register_low8(port_step.state.eax) == 0xffu &&
				 port_step.state.eip == 27u &&
				 dos_register_low16(port_step.state.esi) == 1u &&
				 (dos_register_low16(port_step.state.ecx) &
				  DOS_EFLAGS_IF) == 0u &&
				 (dos_register_low16(port_step.state.edx) &
				  DOS_EFLAGS_IF) != 0u &&
				 dos_register_low16(port_step.state.ebx) ==
					 0x5678u;
			if (passed)
				stage = 7u;
		}
	}
	if (session_acquired) {
		passed = dos_exec_backend_session_stop(
				 &sessions, session, ops, adapter_context) ==
			     DOS_EXEC_BACKEND_SESSION_OK &&
			 dos_exec_backend_session_retire(&sessions, session) ==
			     DOS_EXEC_BACKEND_SESSION_OK &&
			 passed;
	}

	if (dos_machine_write(machine, arena_linear, original_arena,
			      sizeof(original_arena), sizeof(original_arena)) !=
		DOS_MACHINE_OK ||
	    dos_machine_write(machine, vector_linear, original_vector,
			      sizeof(original_vector), sizeof(original_vector)) !=
		DOS_MACHINE_OK ||
	    dos_machine_write_far(
		machine, SELFTEST_STACK_SEGMENT,
		(uint16_t)(SELFTEST_STACK_POINTER - sizeof(original_stack)),
		original_stack, sizeof(original_stack), sizeof(original_stack)) !=
		DOS_MACHINE_OK ||
	    dos_machine_write(machine, linear, original, sizeof(original),
			      sizeof(original)) != DOS_MACHINE_OK)
		return false;
	if (!passed)
		selftest_report_failure(stage, failure_detail, &service_step,
					&reflected_step, &port_step);
	return passed;
}

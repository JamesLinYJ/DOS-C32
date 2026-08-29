// SPDX-License-Identifier: GPL-2.0-only
/* Freestanding logical-FLAGS, stack and failure tests. */
#include "dos_control_instruction.h"
#include "test_entry.h"

#define MEMORY_BYTES DOS_A20_WRAP_ADDRESS
#define TEST_CONTEXT ((kernel_object_handle_t)0x434f4e54524f4c31ull)
#define TEST_POP16_WRITABLE                                                  \
	(DOS_EFLAGS_CF | DOS_EFLAGS_PF | DOS_EFLAGS_AF | DOS_EFLAGS_ZF |   \
	 DOS_EFLAGS_SF | DOS_EFLAGS_TF | DOS_EFLAGS_IF | DOS_EFLAGS_DF |   \
	 DOS_EFLAGS_OF | DOS_EFLAGS_IOPL | DOS_EFLAGS_NT)

static uint8_t guest_memory[MEMORY_BYTES];
static uint32_t write_calls;
static uint32_t fail_write_call;
static bool fail_all_writes;
static bool fail_reads;

static enum dos_machine_status
read_memory(kernel_object_handle_t context, dos_linear_address_t address,
	    void *destination, size_t destination_capacity, size_t count)
{
	uint8_t *output = (uint8_t *)destination;
	size_t index;

	if (context != TEST_CONTEXT || destination == NULL ||
	    count > destination_capacity || address > MEMORY_BYTES ||
	    count > MEMORY_BYTES - (size_t)address)
		return DOS_MACHINE_ADDRESS_FAULT;
	if (fail_reads)
		return DOS_MACHINE_IO_FAULT;
	for (index = 0u; index < count; ++index)
		output[index] = guest_memory[(size_t)address + index];
	return DOS_MACHINE_OK;
}

static enum dos_machine_status
write_memory(kernel_object_handle_t context, dos_linear_address_t address,
	     const void *source, size_t source_capacity, size_t count)
{
	const uint8_t *input = (const uint8_t *)source;
	size_t index;

	++write_calls;
	if (context != TEST_CONTEXT || source == NULL ||
	    count > source_capacity || address > MEMORY_BYTES ||
	    count > MEMORY_BYTES - (size_t)address)
		return DOS_MACHINE_ADDRESS_FAULT;
	if (fail_all_writes || write_calls == fail_write_call) {
		if (count != 0u)
			guest_memory[address] = 0xeeu;
		return DOS_MACHINE_IO_FAULT;
	}
	for (index = 0u; index < count; ++index)
		guest_memory[(size_t)address + index] = input[index];
	return DOS_MACHINE_OK;
}

static const struct dos_machine_ops machine_ops = {
	.read_memory = read_memory,
	.write_memory = write_memory,
	.read_port = NULL,
	.write_port = NULL,
	.set_a20 = NULL,
};

static struct dos_cpu_state make_state(void)
{
	return (struct dos_cpu_state){
		.eax = 0x11111111u,
		.ebx = 0x22222222u,
		.ecx = 0x33333333u,
		.edx = 0x44444444u,
		.esi = 0x55555555u,
		.edi = 0x66666666u,
		.ebp = 0x77777777u,
		.esp = 0x0200u,
		.eip = 0xffffu,
		.eflags = DOS_EFLAGS_VM | DOS_EFLAGS_IF |
			  DOS_EFLAGS_CF | DOS_EFLAGS_RESERVED_ONE,
		.cs = 0x1234u,
		.ss = 0x1000u,
		.ds = 0x2000u,
		.es = 0x3000u,
		.fs = 0x4000u,
		.gs = 0x5000u,
		.mode = (uint32_t)DOS_CPU_VM86,
	};
}

static void reset_failures(void)
{
	write_calls = 0u;
	fail_write_call = 0u;
	fail_all_writes = false;
	fail_reads = false;
}

static int test_cli_sti_and_unknown(struct dos_machine *machine)
{
	struct dos_cpu_state state = make_state();
	struct dos_control_instruction_result result;

	result = dos_control_instruction_emulate(machine, 0xfau, false, &state);
	if (result.status != (uint32_t)DOS_CONTROL_INSTRUCTION_EMULATED ||
	    (result.state.eflags & DOS_EFLAGS_IF) != 0u ||
	    result.state.eip != 0u || state.eip != 0xffffu ||
	    (state.eflags & DOS_EFLAGS_IF) == 0u)
		return 1;
	result = dos_control_instruction_emulate(machine, 0xfbu, false,
						 &result.state);
	if (result.status != (uint32_t)DOS_CONTROL_INSTRUCTION_EMULATED ||
	    (result.state.eflags & DOS_EFLAGS_IF) == 0u ||
	    result.state.eip != 1u)
		return 2;
	result = dos_control_instruction_emulate(machine, 0x90u, false, &state);
	if (result.status != (uint32_t)DOS_CONTROL_INSTRUCTION_NOT_HANDLED ||
	    result.machine_status != (uint32_t)DOS_MACHINE_OK)
		return 3;
	return 0;
}

static int test_hlt_commits_next_ip(struct dos_machine *machine)
{
	struct dos_cpu_state state = make_state();
	struct dos_control_instruction_result result;

	state.eip = 0x1234u;
	result = dos_control_instruction_emulate(machine, 0xf4u, false, &state);
	if (result.status != (uint32_t)DOS_CONTROL_INSTRUCTION_HALTED ||
	    result.machine_status != (uint32_t)DOS_MACHINE_OK ||
	    result.state.eip != 0x1235u || state.eip != 0x1234u ||
	    result.state.eflags != state.eflags)
		return 1;
	state.eip = 0xffffu;
	result = dos_control_instruction_emulate(machine, 0xf4u, true, &state);
	if (result.status != (uint32_t)DOS_CONTROL_INSTRUCTION_HALTED ||
	    result.state.eip != 1u || state.eip != 0xffffu)
		return 2;
	return 0;
}

static int test_pushf_popf_iret(struct dos_machine *machine)
{
	struct dos_cpu_state state = make_state();
	struct dos_control_instruction_result result;
	size_t stack = 0x1000u * 16u + 0x01feu;

	state.eip = 0x0100u;
	reset_failures();
	guest_memory[stack] = 0xa5u;
	guest_memory[stack + 1u] = 0x5au;
	result = dos_control_instruction_emulate(machine, 0x9cu, false, &state);
	if (result.status != (uint32_t)DOS_CONTROL_INSTRUCTION_EMULATED ||
	    result.state.esp != 0x01feu || result.state.eip != 0x0101u ||
	    guest_memory[stack] != 0x03u || guest_memory[stack + 1u] != 0x02u)
		return 1;

	guest_memory[stack] = 0x55u;
	guest_memory[stack + 1u] = 0x7fu;
	state.esp = 0x01feu;
	result = dos_control_instruction_emulate(machine, 0x9du, false, &state);
	if (result.status != (uint32_t)DOS_CONTROL_INSTRUCTION_EMULATED ||
	    result.state.esp != 0x0200u || result.state.eip != 0x0101u ||
	    (result.state.eflags & TEST_POP16_WRITABLE) !=
		(0x7f55u & TEST_POP16_WRITABLE) ||
	    (result.state.eflags & DOS_EFLAGS_VM) == 0u ||
	    (result.state.eflags & DOS_EFLAGS_RESERVED_ONE) == 0u)
		return 2;

	guest_memory[stack] = 0x34u;
	guest_memory[stack + 1u] = 0x12u;
	guest_memory[stack + 2u] = 0x78u;
	guest_memory[stack + 3u] = 0x56u;
	guest_memory[stack + 4u] = 0x02u;
	guest_memory[stack + 5u] = 0x02u;
	state.esp = 0x01feu;
	result = dos_control_instruction_emulate(machine, 0xcfu, false, &state);
	if (result.status != (uint32_t)DOS_CONTROL_INSTRUCTION_EMULATED ||
	    result.state.eip != 0x1234u || result.state.cs != 0x5678u ||
	    result.state.esp != 0x0204u ||
	    (result.state.eflags & DOS_EFLAGS_IF) == 0u)
		return 3;
	return 0;
}

static int test_transaction_failures(struct dos_machine *machine)
{
	struct dos_cpu_state state = make_state();
	struct dos_control_instruction_result result;
	size_t stack = 0x1000u * 16u + 0x01feu;

	state.eip = 0x0100u;
	guest_memory[stack] = 0xa5u;
	guest_memory[stack + 1u] = 0x5au;
	reset_failures();
	fail_write_call = 1u;
	result = dos_control_instruction_emulate(machine, 0x9cu, false, &state);
	if (result.status !=
		(uint32_t)DOS_CONTROL_INSTRUCTION_MACHINE_FAULT ||
	    result.machine_status != (uint32_t)DOS_MACHINE_IO_FAULT ||
	    guest_memory[stack] != 0xa5u || guest_memory[stack + 1u] != 0x5au)
		return 1;

	reset_failures();
	fail_all_writes = true;
	result = dos_control_instruction_emulate(machine, 0x9cu, false, &state);
	if (result.status !=
		(uint32_t)DOS_CONTROL_INSTRUCTION_ROLLBACK_FAILED ||
	    result.machine_status != (uint32_t)DOS_MACHINE_ROLLBACK_FAILED)
		return 2;

	reset_failures();
	fail_reads = true;
	result = dos_control_instruction_emulate(machine, 0x9du, false, &state);
	if (result.status !=
		(uint32_t)DOS_CONTROL_INSTRUCTION_MACHINE_FAULT ||
	    result.machine_status != (uint32_t)DOS_MACHINE_IO_FAULT)
		return 3;
	state.esp = 0x10000u;
	if (dos_control_instruction_emulate(machine, 0xfau, false, &state).status !=
		(uint32_t)DOS_CONTROL_INSTRUCTION_INVALID_ARGUMENT)
		return 4;
	return 0;
}

static int run_tests(void)
{
	struct dos_machine machine;
	int status;

	if (dos_machine_configure(&machine, &machine_ops, TEST_CONTEXT,
				  MEMORY_BYTES, false) != DOS_MACHINE_OK)
		return 1;
	status = test_cli_sti_and_unknown(&machine);
	if (status != 0)
		return 10 + status;
	status = test_hlt_commits_next_ip(&machine);
	if (status != 0)
		return 20 + status;
	status = test_pushf_popf_iret(&machine);
	if (status != 0)
		return 30 + status;
	status = test_transaction_failures(&machine);
	if (status != 0)
		return 40 + status;
	return 0;
}

DOSC32_TEST_ENTRY(run_tests)

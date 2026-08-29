// SPDX-License-Identifier: GPL-2.0-only
/* Freestanding scalar IN/OUT decode tests. */
#include "dos_port_instruction.h"
#include "test_entry.h"

#define MEMORY_BYTES 0x10000u
#define TEST_CONTEXT ((kernel_object_handle_t)0x504f525444454331ull)

static uint8_t guest_memory[MEMORY_BYTES];

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
	for (index = 0u; index < count; ++index)
		output[index] = guest_memory[(size_t)address + index];
	return DOS_MACHINE_OK;
}

static enum dos_machine_status
write_memory(kernel_object_handle_t context, dos_linear_address_t address,
	     const void *source, size_t source_capacity, size_t count)
{
	(void)context;
	(void)address;
	(void)source;
	(void)source_capacity;
	(void)count;
	return DOS_MACHINE_IO_DENIED;
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
		.eax = 0x89abcdefu,
		.edx = 0x1234u,
		.eip = 0x0100u,
		.eflags = DOS_EFLAGS_RESERVED_ONE,
		.cs = 0u,
		.ss = 0u,
		.mode = (uint32_t)DOS_CPU_VM86,
	};
}

static int expect_decode(const struct dos_machine *machine,
			 struct dos_cpu_state state, const uint8_t *bytes,
			 size_t count, uint16_t port, enum dos_io_width width,
			 bool write, uint32_t value)
{
	struct dos_port_instruction_result result;
	size_t index;

	for (index = 0u; index < count; ++index)
		guest_memory[(uint16_t)(state.eip + index)] = bytes[index];
	result = dos_port_instruction_decode(machine, &state);
	return result.status == (uint32_t)DOS_PORT_INSTRUCTION_DECODED &&
	       result.machine_status == (uint32_t)DOS_MACHINE_OK &&
	       result.state.eip == (uint16_t)(state.eip + count) &&
	       result.event.kind == (uint32_t)DOS_EXEC_EVENT_PORT_IO &&
	       result.event.port == port &&
	       result.event.io_width == (uint8_t)width &&
	       result.event.io_write == (write ? 1u : 0u) &&
	       result.event.value == value
		       ? 0
		       : 1;
}

static int run_tests(void)
{
	static const uint8_t in_al_immediate[] = {0xe4u, 0x80u};
	static const uint8_t in_eax_immediate[] = {0x66u, 0xe5u, 0x42u};
	static const uint8_t in_ax_dx[] = {0xedu};
	static const uint8_t out_immediate_al[] = {0xe6u, 0x81u};
	static const uint8_t out_dx_ax[] = {0xefu};
	struct dos_machine machine;
	struct dos_cpu_state state = make_state();
	struct dos_port_instruction_result result;

	if (dos_machine_configure(&machine, &machine_ops, TEST_CONTEXT,
				  MEMORY_BYTES, false) != DOS_MACHINE_OK)
		return 1;
	if (expect_decode(&machine, state, in_al_immediate,
			  sizeof(in_al_immediate), 0x80u, DOS_IO_WIDTH_8,
			  false, 0u) != 0)
		return 2;
	if (expect_decode(&machine, state, in_eax_immediate,
			  sizeof(in_eax_immediate), 0x42u, DOS_IO_WIDTH_32,
			  false, 0u) != 0)
		return 3;
	if (expect_decode(&machine, state, in_ax_dx, sizeof(in_ax_dx),
			  0x1234u, DOS_IO_WIDTH_16, false, 0u) != 0)
		return 4;
	if (expect_decode(&machine, state, out_immediate_al,
			  sizeof(out_immediate_al), 0x81u, DOS_IO_WIDTH_8,
			  true, 0xefu) != 0)
		return 5;
	if (expect_decode(&machine, state, out_dx_ax, sizeof(out_dx_ax),
			  0x1234u, DOS_IO_WIDTH_16, true, 0xcdefu) != 0)
		return 6;

	state.eip = 0xffffu;
	guest_memory[0xffffu] = 0x66u;
	guest_memory[0u] = 0xe5u;
	guest_memory[1u] = 0x77u;
	result = dos_port_instruction_decode(&machine, &state);
	if (result.status != (uint32_t)DOS_PORT_INSTRUCTION_DECODED ||
	    result.state.eip != 2u || result.event.port != 0x77u ||
	    result.event.io_width != (uint8_t)DOS_IO_WIDTH_32)
		return 7;
	guest_memory[0xffffu] = 0x90u;
	result = dos_port_instruction_decode(&machine, &state);
	if (result.status != (uint32_t)DOS_PORT_INSTRUCTION_NOT_HANDLED)
		return 8;
	machine.address_limit = 1u;
	state.eip = 2u;
	result = dos_port_instruction_decode(&machine, &state);
	if (result.status != (uint32_t)DOS_PORT_INSTRUCTION_FETCH_FAULT ||
	    result.machine_status != (uint32_t)DOS_MACHINE_ADDRESS_FAULT)
		return 9;
	return 0;
}

DOSC32_TEST_ENTRY(run_tests)

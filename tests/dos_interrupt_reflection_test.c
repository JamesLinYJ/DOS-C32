// SPDX-License-Identifier: GPL-2.0-only
/* Freestanding IVT, IRET-frame, wrap and transactional-failure tests. */
#include "dos_interrupt_reflection.h"
#include "test_entry.h"

#define MEMORY_BYTES DOS_A20_WRAP_ADDRESS
#define TEST_CONTEXT ((kernel_object_handle_t)0x4952545245464c31ull)
#define TEST_VECTOR 0x30u

static uint8_t guest_memory[MEMORY_BYTES];
static uint32_t write_calls;
static uint32_t fail_write_call;
static bool fail_all_writes;
static dos_linear_address_t fail_read_address = MEMORY_BYTES;

static bool range_contains(dos_linear_address_t start, size_t count,
			   dos_linear_address_t address)
{
	return address >= start && (uint64_t)address - (uint64_t)start < count;
}

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
	if (range_contains(address, count, fail_read_address))
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

static void clear_fixture(void)
{
	size_t index;

	for (index = 0u; index < MEMORY_BYTES; ++index)
		guest_memory[index] = 0u;
	write_calls = 0u;
	fail_write_call = 0u;
	fail_all_writes = false;
	fail_read_address = MEMORY_BYTES;
}

static void install_vector(uint8_t vector, uint16_t segment, uint16_t offset)
{
	size_t address = (size_t)vector * 4u;

	guest_memory[address] = (uint8_t)offset;
	guest_memory[address + 1u] = (uint8_t)(offset >> 8u);
	guest_memory[address + 2u] = (uint8_t)segment;
	guest_memory[address + 3u] = (uint8_t)(segment >> 8u);
}

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
		.eip = 0x3456u,
		.eflags = DOS_EFLAGS_VM | DOS_EFLAGS_IF | DOS_EFLAGS_TF | 3u,
		.cs = 0x1234u,
		.ss = 0x1000u,
		.ds = 0x2000u,
		.es = 0x3000u,
		.fs = 0x4000u,
		.gs = 0x5000u,
		.mode = (uint32_t)DOS_CPU_VM86,
	};
}

static bool states_equal(const struct dos_cpu_state *left,
			 const struct dos_cpu_state *right)
{
	return left->eax == right->eax && left->ebx == right->ebx &&
	       left->ecx == right->ecx && left->edx == right->edx &&
	       left->esi == right->esi && left->edi == right->edi &&
	       left->ebp == right->ebp && left->esp == right->esp &&
	       left->eip == right->eip && left->eflags == right->eflags &&
	       left->cs == right->cs && left->ss == right->ss &&
	       left->ds == right->ds && left->es == right->es &&
	       left->fs == right->fs && left->gs == right->gs &&
	       left->mode == right->mode;
}

static int test_normal_reflection(struct dos_machine *machine)
{
	struct dos_cpu_state state = make_state();
	struct dos_cpu_state original = state;
	struct dos_interrupt_reflection_result result;
	size_t stack = 0x1000u * 16u + 0x01fau;
	size_t index;

	clear_fixture();
	install_vector(TEST_VECTOR, 0xabcdu, 0x789au);
	for (index = 0u; index < DOS_INTERRUPT_REFLECTION_FRAME_BYTES; ++index)
		guest_memory[stack + index] = 0x5au;
	result = dos_interrupt_reflect(machine, TEST_VECTOR, &state);
	if (result.status != (uint32_t)DOS_INTERRUPT_REFLECTION_OK ||
	    result.machine_status != (uint32_t)DOS_MACHINE_OK ||
	    !states_equal(&state, &original) || result.state.esp != 0x01fau ||
	    result.state.eip != 0x789au || result.state.cs != 0xabcdu ||
	    (result.state.eflags & (DOS_EFLAGS_IF | DOS_EFLAGS_TF)) != 0u ||
	    (result.state.eflags & (DOS_EFLAGS_VM | DOS_EFLAGS_CF)) !=
		(DOS_EFLAGS_VM | DOS_EFLAGS_CF) || result.receipt.valid != 1u ||
	    result.receipt.stack_segment != state.ss ||
	    result.receipt.stack_offset != 0x01fau)
		return 1;
	for (index = 0u; index < ARRAY_SIZE(result.receipt.original); ++index) {
		if (result.receipt.original[index] != 0x5au)
			return 2;
	}
	if (guest_memory[stack] != 0x56u ||
	    guest_memory[stack + 1u] != 0x34u ||
	    guest_memory[stack + 2u] != 0x34u ||
	    guest_memory[stack + 3u] != 0x12u ||
	    guest_memory[stack + 4u] != 0x03u ||
	    guest_memory[stack + 5u] != 0x03u)
		return 3;
	if (dos_interrupt_reflection_rollback(machine, &result.receipt) !=
	    DOS_MACHINE_OK)
		return 4;
	for (index = 0u; index < DOS_INTERRUPT_REFLECTION_FRAME_BYTES; ++index) {
		if (guest_memory[stack + index] != 0x5au)
			return 5;
	}
	return 0;
}

static int test_offset_and_a20_wrap(struct dos_machine *machine)
{
	struct dos_cpu_state state = make_state();
	struct dos_interrupt_reflection_result result;

	clear_fixture();
	install_vector(TEST_VECTOR, 0x1111u, 0x2222u);
	state.ss = 0x2000u;
	state.esp = 0x0004u;
	result = dos_interrupt_reflect(machine, TEST_VECTOR, &state);
	if (result.status != (uint32_t)DOS_INTERRUPT_REFLECTION_OK ||
	    result.state.esp != 0xfffeu || guest_memory[0x2fffeu] != 0x56u ||
	    guest_memory[0x2ffffu] != 0x34u ||
	    guest_memory[0x20000u] != 0x34u ||
	    guest_memory[0x20001u] != 0x12u ||
	    guest_memory[0x20002u] != 0x03u ||
	    guest_memory[0x20003u] != 0x03u)
		return 1;

	clear_fixture();
	install_vector(TEST_VECTOR, 0x1111u, 0x2222u);
	state.ss = 0xffffu;
	state.esp = 0x0014u;
	result = dos_interrupt_reflect(machine, TEST_VECTOR, &state);
	if (result.status != (uint32_t)DOS_INTERRUPT_REFLECTION_OK ||
	    result.state.esp != 0x000eu || guest_memory[0xffffeu] != 0x56u ||
	    guest_memory[0xfffffu] != 0x34u || guest_memory[0u] != 0x34u ||
	    guest_memory[1u] != 0x12u || guest_memory[2u] != 0x03u ||
	    guest_memory[3u] != 0x03u)
		return 2;
	return 0;
}

static int test_failures(struct dos_machine *machine)
{
	struct dos_cpu_state state = make_state();
	struct dos_cpu_state original = state;
	struct dos_interrupt_reflection_result result;
	size_t vector_address = TEST_VECTOR * 4u;
	size_t stack = 0x1000u * 16u + 0x01fau;
	size_t index;

	clear_fixture();
	install_vector(TEST_VECTOR, 0x1111u, 0x2222u);
	fail_read_address = vector_address;
	result = dos_interrupt_reflect(machine, TEST_VECTOR, &state);
	if (result.status !=
		(uint32_t)DOS_INTERRUPT_REFLECTION_VECTOR_FAULT ||
	    result.machine_status != (uint32_t)DOS_MACHINE_IO_FAULT ||
	    !states_equal(&state, &original))
		return 1;

	clear_fixture();
	install_vector(TEST_VECTOR, 0x1111u, 0x2222u);
	fail_read_address = stack;
	result = dos_interrupt_reflect(machine, TEST_VECTOR, &state);
	if (result.status !=
		(uint32_t)DOS_INTERRUPT_REFLECTION_STACK_FAULT ||
	    result.machine_status != (uint32_t)DOS_MACHINE_IO_FAULT)
		return 2;

	clear_fixture();
	install_vector(TEST_VECTOR, 0x1111u, 0x2222u);
	for (index = 0u; index < 6u; ++index)
		guest_memory[stack + index] = 0x5au;
	fail_write_call = 1u;
	result = dos_interrupt_reflect(machine, TEST_VECTOR, &state);
	if (result.status !=
		(uint32_t)DOS_INTERRUPT_REFLECTION_STACK_FAULT ||
	    result.machine_status != (uint32_t)DOS_MACHINE_IO_FAULT)
		return 3;
	for (index = 0u; index < 6u; ++index)
		if (guest_memory[stack + index] != 0x5au)
			return 4;

	clear_fixture();
	install_vector(TEST_VECTOR, 0x1111u, 0x2222u);
	fail_all_writes = true;
	result = dos_interrupt_reflect(machine, TEST_VECTOR, &state);
	if (result.status !=
		(uint32_t)DOS_INTERRUPT_REFLECTION_ROLLBACK_FAILED ||
	    result.machine_status != (uint32_t)DOS_MACHINE_ROLLBACK_FAILED)
		return 5;

	state.eip = 0x10000u;
	result = dos_interrupt_reflect(machine, TEST_VECTOR, &state);
	if (result.status !=
		(uint32_t)DOS_INTERRUPT_REFLECTION_INVALID_ARGUMENT ||
	    dos_interrupt_reflect(NULL, TEST_VECTOR, &original).status !=
		(uint32_t)DOS_INTERRUPT_REFLECTION_INVALID_ARGUMENT)
		return 6;
	return 0;
}

static int run_tests(void)
{
	struct dos_machine machine;
	int status;

	if (dos_machine_configure(&machine, &machine_ops, TEST_CONTEXT,
				  MEMORY_BYTES, false) != DOS_MACHINE_OK)
		return 1;
	status = test_normal_reflection(&machine);
	if (status != 0)
		return 10 + status;
	status = test_offset_and_a20_wrap(&machine);
	if (status != 0)
		return 20 + status;
	status = test_failures(&machine);
	if (status != 0)
		return 30 + status;
	return 0;
}

DOSC32_TEST_ENTRY(run_tests)

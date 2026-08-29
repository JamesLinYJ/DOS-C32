// SPDX-License-Identifier: GPL-2.0-only
/* Freestanding tests for the DOS-visible, fixed-width EXEC0 handoff. */
#include "dos_exec_handoff.h"
#include "test_entry.h"

#define GUEST_CAPACITY 0x110000u
#define MACHINE_IDENTITY ((kernel_object_handle_t)0x48414e444f464631ull)
#define MACHINE_CONTEXT ((kernel_object_handle_t)0x48414e444d454d31ull)

static uint8_t guest_memory[GUEST_CAPACITY];
static uint32_t read_calls;
static uint32_t write_calls;

static void fill_bytes(uint8_t *bytes, size_t count, uint8_t value)
{
	size_t index;

	for (index = 0u; index < count; ++index)
		bytes[index] = value;
}

static bool bytes_equal(const uint8_t *left, const uint8_t *right,
			size_t count)
{
	size_t index;

	for (index = 0u; index < count; ++index) {
		if (left[index] != right[index])
			return false;
	}
	return true;
}

static enum dos_machine_status backend_read(kernel_object_handle_t context,
					    dos_linear_address_t address,
					    void *destination,
					    size_t destination_capacity,
					    size_t count)
{
	uint8_t *output = (uint8_t *)destination;
	size_t index;

	++read_calls;
	if (context != MACHINE_CONTEXT || destination == NULL ||
	    count > destination_capacity || address > GUEST_CAPACITY ||
	    count > GUEST_CAPACITY - (size_t)address)
		return DOS_MACHINE_IO_FAULT;
	for (index = 0u; index < count; ++index)
		output[index] = guest_memory[(size_t)address + index];
	return DOS_MACHINE_OK;
}

static enum dos_machine_status backend_write(kernel_object_handle_t context,
					     dos_linear_address_t address,
					     const void *source,
					     size_t source_capacity,
					     size_t count)
{
	const uint8_t *input = (const uint8_t *)source;
	size_t index;

	++write_calls;
	if (context != MACHINE_CONTEXT || source == NULL ||
	    count > source_capacity || address > GUEST_CAPACITY ||
	    count > GUEST_CAPACITY - (size_t)address)
		return DOS_MACHINE_IO_FAULT;
	for (index = 0u; index < count; ++index)
		guest_memory[(size_t)address + index] = input[index];
	return DOS_MACHINE_OK;
}

static const struct dos_machine_ops machine_ops = {
	.read_memory = backend_read,
	.write_memory = backend_write,
	.read_port = NULL,
	.write_port = NULL,
	.set_a20 = NULL,
};

static struct dos_cpu_state make_state(uint16_t psp, uint16_t cs, uint16_t ip,
				       uint16_t ss, uint16_t sp,
				       uint16_t initial_ax)
{
	struct dos_cpu_state state = {0};

	state.eax = initial_ax;
	state.ebx = initial_ax;
	state.edx = psp;
	state.esi = ip;
	state.edi = sp;
	state.esp = sp;
	state.eip = ip;
	state.eflags = DOS_EFLAGS_IF | 2u;
	state.cs = cs;
	state.ss = ss;
	state.ds = psp;
	state.es = psp;
	state.mode = (uint32_t)DOS_CPU_REAL16;
	return state;
}

static int test_com_and_mz_value_plans(void)
{
	struct dos_com_process_plan com = {0};
	struct dos_mz_process_plan mz = {0};
	struct dos_exec_handoff_plan result;
	struct dos_exec_handoff_plan sentinel;

	fill_bytes((uint8_t *)&result, sizeof(result), 0xa5u);
	sentinel = result;
	com.psp_segment = 0x2000u;
	com.launch_mode = (uint8_t)DOS_PROCESS_LAUNCH_EXECUTE;
	com.initial_state =
	    make_state(0x2000u, 0x2000u, 0x0100u, 0x2000u, 0xfffeu,
		       0xff00u);
	if (dos_exec_handoff_prepare_com(&com, &result) !=
		DOS_EXEC_HANDOFF_OK ||
	    !dos_exec_handoff_plan_has_valid_encoding(&result) ||
	    result.format != DOS_IMAGE_COM || result.child_psp != 0x2000u ||
	    result.stack_image.segment != 0x2000u ||
	    result.stack_image.offset != 0xfffau ||
	    result.stack_image.bytes[0] != 0x00u ||
	    result.stack_image.bytes[1] != 0x01u ||
	    result.stack_image.bytes[2] != 0x00u ||
	    result.stack_image.bytes[3] != 0x20u ||
	    result.entry_state.esp != 0xfffeu)
		return 1;

	fill_bytes((uint8_t *)&result, sizeof(result), 0xa5u);
	sentinel = result;
	com.launch_mode = (uint8_t)DOS_PROCESS_LAUNCH_LOAD_ONLY;
	if (dos_exec_handoff_prepare_com(&com, &result) !=
		DOS_EXEC_HANDOFF_INVALID_PROCESS_PLAN ||
	    !bytes_equal((const uint8_t *)&result,
			 (const uint8_t *)&sentinel, sizeof(result)))
		return 2;

	mz.psp_segment = 0x3456u;
	mz.launch_mode = (uint8_t)DOS_PROCESS_LAUNCH_EXECUTE;
	mz.initial_state =
	    make_state(0x3456u, 0x4012u, 0xab34u, 0x4123u, 0x0102u,
		       0x55aau);
	if (dos_exec_handoff_prepare_mz(&mz, &result) !=
		DOS_EXEC_HANDOFF_OK ||
	    result.format != DOS_IMAGE_MZ ||
	    result.stack_image.segment != 0x4123u ||
	    result.stack_image.offset != 0x00feu ||
	    result.stack_image.bytes[0] != 0x34u ||
	    result.stack_image.bytes[1] != 0xabu ||
	    result.stack_image.bytes[2] != 0x12u ||
	    result.stack_image.bytes[3] != 0x40u)
		return 3;
	result.entry_state.ds = 0x9999u;
	if (dos_exec_handoff_plan_has_valid_encoding(&result))
		return 4;
	return 0;
}

static int test_atomic_stack_stage_and_abort(void)
{
	struct dos_mz_process_plan mz = {0};
	struct dos_exec_handoff_plan plan;
	struct dos_exec_journal journal = DOS_EXEC_JOURNAL_INITIALIZER;
	struct dos_machine machine;
	dos_linear_address_t linear;
	uint8_t original[4] = {0x11u, 0x22u, 0x33u, 0x44u};

	read_calls = 0u;
	write_calls = 0u;
	mz.psp_segment = 0x2345u;
	mz.launch_mode = (uint8_t)DOS_PROCESS_LAUNCH_EXECUTE;
	mz.initial_state =
	    make_state(0x2345u, 0x3456u, 0x789au, 0x4567u, 0x1000u,
		       0x00ffu);
	if (dos_exec_handoff_prepare_mz(&mz, &plan) != DOS_EXEC_HANDOFF_OK ||
	    dos_machine_configure(&machine, &machine_ops, MACHINE_CONTEXT,
				  GUEST_CAPACITY, true) != DOS_MACHINE_OK ||
	    dos_exec_journal_construct(&journal) != DOS_EXEC_JOURNAL_OK ||
	    dos_exec_journal_initialize(&journal, MACHINE_IDENTITY, &machine) !=
		DOS_EXEC_JOURNAL_OK)
		return 1;
	linear = dos_far_to_linear(plan.stack_image.segment,
				   plan.stack_image.offset, true);
	guest_memory[linear] = original[0];
	guest_memory[linear + 1u] = original[1];
	guest_memory[linear + 2u] = original[2];
	guest_memory[linear + 3u] = original[3];
	if (dos_exec_handoff_stage_stack(&plan, &journal, MACHINE_IDENTITY,
					 &machine) != DOS_EXEC_HANDOFF_OK ||
	    journal.record_count != 1u || read_calls != 1u || write_calls != 1u ||
	    !bytes_equal(&guest_memory[linear], plan.stack_image.bytes, 4u))
		return 2;
	if (dos_exec_journal_abort(&journal, MACHINE_IDENTITY, &machine) !=
		DOS_EXEC_JOURNAL_OK ||
	    !bytes_equal(&guest_memory[linear], original, 4u))
		return 3;
	return 0;
}

static int run_tests(void)
{
	int status = test_com_and_mz_value_plans();

	if (status != 0)
		return 10 + status;
	status = test_atomic_stack_stage_and_abort();
	if (status != 0)
		return 20 + status;
	return 0;
}

DOSC32_TEST_ENTRY(run_tests)

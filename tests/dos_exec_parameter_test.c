// SPDX-License-Identifier: GPL-2.0-only
/* Freestanding tests for ordered DOS EXEC parameter-block decoding. */
#include "dos_exec_parameter.h"
#include "test_entry.h"

#define TEST_MEMORY_SIZE DOS_A20_WRAP_ADDRESS
#define TEST_MACHINE_IDENTITY ((kernel_object_handle_t)0x4558454350415241ull)

static uint8_t guest_memory[TEST_MEMORY_SIZE];
static uint32_t read_calls;
static uint32_t write_calls;
static bool fail_read;
static uint32_t fail_read_call;

static enum dos_machine_status
test_read(kernel_object_handle_t context, dos_linear_address_t address,
	  void *destination, size_t destination_capacity, size_t count)
{
	uint8_t *bytes = (uint8_t *)destination;
	size_t index;

	++read_calls;
	if (fail_read ||
	    (fail_read_call != 0u && read_calls == fail_read_call)) {
		fail_read = false;
		fail_read_call = 0u;
		return DOS_MACHINE_ADDRESS_FAULT;
	}
	if (context != 1u || count > destination_capacity ||
	    (uint64_t)address >= TEST_MEMORY_SIZE ||
	    (uint64_t)count > TEST_MEMORY_SIZE - (uint64_t)address)
		return DOS_MACHINE_ADDRESS_FAULT;
	for (index = 0u; index < count; ++index)
		bytes[index] = guest_memory[address + index];
	return DOS_MACHINE_OK;
}

static enum dos_machine_status test_write(kernel_object_handle_t context,
					  dos_linear_address_t address,
					  const void *source,
					  size_t source_capacity, size_t count)
{
	const uint8_t *bytes = (const uint8_t *)source;
	size_t index;

	++write_calls;
	if (context != 1u || source == NULL || count > source_capacity ||
	    (uint64_t)address >= TEST_MEMORY_SIZE ||
	    (uint64_t)count > TEST_MEMORY_SIZE - (uint64_t)address)
		return DOS_MACHINE_ADDRESS_FAULT;
	for (index = 0u; index < count; ++index)
		guest_memory[address + index] = bytes[index];
	return DOS_MACHINE_OK;
}

static void put_le16(dos_linear_address_t address, uint16_t value)
{
	guest_memory[address] = (uint8_t)value;
	guest_memory[address + 1u] = (uint8_t)(value >> 8);
}

static void set_source_plan_sentinel(
    struct dos_exec_environment_source_plan *plan)
{
	plan->source.offset = 0x1111u;
	plan->source.segment = 0x2222u;
	plan->parent_psp = 0x3333u;
	plan->subfunction = 0x44u;
	plan->kind = 0x55u;
}

static bool source_plan_is_sentinel(
    const struct dos_exec_environment_source_plan *plan)
{
	return plan->source.offset == 0x1111u &&
	       plan->source.segment == 0x2222u &&
	       plan->parent_psp == 0x3333u && plan->subfunction == 0x44u &&
	       plan->kind == 0x55u;
}

static int test_environment_source_decode(const struct dos_machine *machine)
{
	struct dos_process_far_address block = {
	    .segment = 0x1000u,
	    .offset = 0xfffeu,
	};
	struct dos_exec_environment_source_plan plan;
	uint32_t calls_before;

	put_le16(0x1fffeu, 0x3456u);
	put_le16(0x2202cu, 0x7777u);
	calls_before = read_calls;
	if (dos_exec_parameter_decode_environment_source(
		machine, DOS_EXEC_LOAD_AND_EXECUTE, block, 0x2200u,
		&plan) != DOS_EXEC_PARAMETER_OK ||
	    read_calls != calls_before + 1u || plan.source.offset != 0u ||
	    plan.source.segment != 0x3456u || plan.parent_psp != 0u ||
	    plan.subfunction != DOS_EXEC_LOAD_AND_EXECUTE ||
	    plan.kind != DOS_EXEC_ENVIRONMENT_SOURCE_PARAMETER ||
	    !dos_exec_environment_source_plan_has_valid_encoding(&plan))
		return 1;

	/* AL=1 shares the same selection path. */
	calls_before = read_calls;
	if (dos_exec_parameter_decode_environment_source(
		machine, DOS_EXEC_LOAD_ONLY, block, 0x2200u, &plan) !=
		DOS_EXEC_PARAMETER_OK ||
	    read_calls != calls_before + 1u ||
	    plan.subfunction != DOS_EXEC_LOAD_ONLY ||
	    plan.kind != DOS_EXEC_ENVIRONMENT_SOURCE_PARAMETER)
		return 2;

	/* A zero parameter word causes the later CurrentPDB:002ch read. */
	put_le16(0x1fffeu, 0u);
	calls_before = read_calls;
	if (dos_exec_parameter_decode_environment_source(
		machine, DOS_EXEC_LOAD_ONLY, block, 0x2200u, &plan) !=
		DOS_EXEC_PARAMETER_OK ||
	    read_calls != calls_before + 2u || plan.source.offset != 0u ||
	    plan.source.segment != 0x7777u || plan.parent_psp != 0x2200u ||
	    plan.kind != DOS_EXEC_ENVIRONMENT_SOURCE_PARENT ||
	    !dos_exec_environment_source_plan_has_valid_encoding(&plan))
		return 3;

	/* PSP segment zero remains a guest value, never a native NULL. */
	put_le16(0x002cu, 0x8888u);
	calls_before = read_calls;
	if (dos_exec_parameter_decode_environment_source(
		machine, DOS_EXEC_LOAD_AND_EXECUTE, block, 0u, &plan) !=
		DOS_EXEC_PARAMETER_OK ||
	    read_calls != calls_before + 2u || plan.source.segment != 0x8888u ||
	    plan.parent_psp != 0u ||
	    plan.kind != DOS_EXEC_ENVIRONMENT_SOURCE_PARENT ||
	    !dos_exec_environment_source_plan_has_valid_encoding(&plan))
		return 4;

	/* A second zero is a valid no-environment result, not a bad source. */
	put_le16(0x2202cu, 0u);
	if (dos_exec_parameter_decode_environment_source(
		machine, DOS_EXEC_LOAD_AND_EXECUTE, block, 0x2200u,
		&plan) != DOS_EXEC_PARAMETER_OK ||
	    plan.source.offset != 0u || plan.source.segment != 0u ||
	    plan.parent_psp != 0x2200u ||
	    plan.kind != DOS_EXEC_ENVIRONMENT_SOURCE_NONE ||
	    !dos_exec_environment_source_plan_has_valid_encoding(&plan))
		return 5;

	/* EXEC3 jumps before both reads; even machine == NULL is sufficient. */
	calls_before = read_calls;
	if (dos_exec_parameter_decode_environment_source(
		NULL, DOS_EXEC_OVERLAY, block, 0xffffu, &plan) !=
		DOS_EXEC_PARAMETER_OK ||
	    read_calls != calls_before || plan.source.offset != 0u ||
	    plan.source.segment != 0u || plan.parent_psp != 0u ||
	    plan.subfunction != DOS_EXEC_OVERLAY ||
	    plan.kind != DOS_EXEC_ENVIRONMENT_SOURCE_OVERLAY_SKIPPED ||
	    !dos_exec_environment_source_plan_has_valid_encoding(&plan))
		return 6;
	plan.source.offset = 1u;
	if (dos_exec_environment_source_plan_has_valid_encoding(&plan) ||
	    dos_exec_environment_source_plan_has_valid_encoding(NULL))
		return 7;

	set_source_plan_sentinel(&plan);
	if (dos_exec_parameter_decode_environment_source(
		NULL, 2u, block, 0x2200u, &plan) !=
		DOS_EXEC_PARAMETER_INVALID_ARGUMENT ||
	    read_calls != calls_before || !source_plan_is_sentinel(&plan))
		return 8;
	if (dos_exec_parameter_decode_environment_source(
		NULL, DOS_EXEC_LOAD_ONLY, block, 0x2200u, &plan) !=
		DOS_EXEC_PARAMETER_INVALID_ARGUMENT ||
	    !source_plan_is_sentinel(&plan))
		return 9;
	if (dos_exec_parameter_decode_environment_source(
		machine, DOS_EXEC_LOAD_ONLY, block, 0x2200u, NULL) !=
		DOS_EXEC_PARAMETER_INVALID_ARGUMENT ||
	    read_calls != calls_before)
		return 10;

	/* Either ordered guest read may fail; neither publishes a partial plan. */
	set_source_plan_sentinel(&plan);
	fail_read_call = read_calls + 1u;
	if (dos_exec_parameter_decode_environment_source(
		machine, DOS_EXEC_LOAD_ONLY, block, 0x2200u, &plan) !=
		DOS_EXEC_PARAMETER_MACHINE_FAULT ||
	    !source_plan_is_sentinel(&plan))
		return 11;
	put_le16(0x2202cu, 0x7777u);
	set_source_plan_sentinel(&plan);
	fail_read_call = read_calls + 2u;
	if (dos_exec_parameter_decode_environment_source(
		machine, DOS_EXEC_LOAD_ONLY, block, 0x2200u, &plan) !=
		DOS_EXEC_PARAMETER_MACHINE_FAULT ||
	    !source_plan_is_sentinel(&plan))
		return 12;

	put_le16(0x1fffeu, 0x3456u);
	return 0;
}

static int run_tests(void)
{
	static const struct dos_machine_ops ops = {
	    .read_memory = test_read,
	    .write_memory = test_write,
	};
	struct dos_machine machine;
	struct dos_exec_journal journal;
	struct dos_process_far_address block = {
	    .segment = 0x1000u,
	    .offset = 0xfffeu,
	};
	struct dos_process_far_address address = {
	    .segment = 0xaaaau,
	    .offset = 0xbbbbu,
	};
	uint16_t environment = 0xcccdu;
	struct dos_exec_load_result_value result = {
	    .initial_sp = 0x1111u,
	    .initial_ss = 0x2222u,
	    .initial_ip = 0x3333u,
	    .initial_cs = 0x4444u,
	};
	struct dos_exec_mz_overlay_target overlay = {
	    .load_segment = 0xaaaau,
	    .relocation_factor = 0xbbbbu,
	};
	static const uint8_t encoded_result[8] = {
	    0x11u, 0x11u, 0x22u, 0x22u, 0x33u, 0x33u, 0x44u, 0x44u,
	};
	uint8_t old_result[8];
	uint32_t calls_before;
	size_t index;

	if (dos_machine_configure(&machine, &ops, 1u, TEST_MEMORY_SIZE,
				  false) != DOS_MACHINE_OK)
		return 1;
	if (test_environment_source_decode(&machine) != 0)
		return 2;
	/* ES:DI displacements use 16-bit effective-address wrap. */
	put_le16(0x1fffeu, 0x3456u);
	put_le16(0x10000u, 0x1111u);
	put_le16(0x10002u, 0x2222u);
	put_le16(0x10004u, 0x3333u);
	put_le16(0x10006u, 0x4444u);
	put_le16(0x10008u, 0x5555u);
	put_le16(0x1000au, 0x6666u);
	if (dos_exec_parameter_read_environment(
		&machine, block, &environment) != DOS_EXEC_PARAMETER_OK ||
	    environment != 0x3456u ||
	    dos_exec_parameter_read_first_fcb(&machine, block, &address) !=
		DOS_EXEC_PARAMETER_OK ||
	    address.offset != 0x3333u || address.segment != 0x4444u ||
	    dos_exec_parameter_read_second_fcb(&machine, block, &address) !=
		DOS_EXEC_PARAMETER_OK ||
	    address.offset != 0x5555u || address.segment != 0x6666u ||
	    dos_exec_parameter_read_command_tail(&machine, block, &address) !=
		DOS_EXEC_PARAMETER_OK ||
	    address.offset != 0x1111u || address.segment != 0x2222u)
		return 3;
	/* EXEC3 COM reads only word zero; MZ reads word zero and then word two.
	 */
	if (dos_exec_parameter_read_com_overlay_segment(
		&machine, block, &environment) != DOS_EXEC_PARAMETER_OK ||
	    environment != 0x3456u ||
	    dos_exec_parameter_read_mz_overlay_target(
		&machine, block, &overlay) != DOS_EXEC_PARAMETER_OK ||
	    overlay.load_segment != 0x3456u ||
	    overlay.relocation_factor != 0x1111u)
		return 4;
	/* A second-word fault observes the ordered two-word read but publishes no pair.
	 */
	overlay.load_segment = 0xaaaau;
	overlay.relocation_factor = 0xbbbbu;
	fail_read_call = read_calls + 2u;
	if (dos_exec_parameter_read_mz_overlay_target(&machine, block,
						      &overlay) !=
		DOS_EXEC_PARAMETER_MACHINE_FAULT ||
	    overlay.load_segment != 0xaaaau ||
	    overlay.relocation_factor != 0xbbbbu)
		return 5;

	/* The environment word may independently cross the A20 boundary. */
	block.segment = 0xffffu;
	block.offset = 0x000fu;
	guest_memory[0x000fffffu] = 0x78u;
	guest_memory[0] = 0x56u;
	if (dos_exec_parameter_read_environment(
		&machine, block, &environment) != DOS_EXEC_PARAMETER_OK ||
	    environment != 0x5678u)
		return 6;

	/* Segment zero is data, while native NULL arguments are rejected before
	 * any guest callback. */
	block.segment = 0u;
	block.offset = 0u;
	put_le16(0u, 0x9876u);
	if (dos_exec_parameter_read_environment(
		&machine, block, &environment) != DOS_EXEC_PARAMETER_OK ||
	    environment != 0x9876u)
		return 7;
	calls_before = read_calls;
	if (dos_exec_parameter_read_environment(NULL, block, &environment) !=
		DOS_EXEC_PARAMETER_INVALID_ARGUMENT ||
	    dos_exec_parameter_read_environment(&machine, block, NULL) !=
		DOS_EXEC_PARAMETER_INVALID_ARGUMENT ||
	    dos_exec_parameter_read_first_fcb(&machine, block, NULL) !=
		DOS_EXEC_PARAMETER_INVALID_ARGUMENT ||
	    dos_exec_parameter_read_com_overlay_segment(
		&machine, block, NULL) != DOS_EXEC_PARAMETER_INVALID_ARGUMENT ||
	    dos_exec_parameter_read_mz_overlay_target(&machine, block, NULL) !=
		DOS_EXEC_PARAMETER_INVALID_ARGUMENT ||
	    read_calls != calls_before)
		return 8;

	fail_read = true;
	environment = 0xa5a5u;
	if (dos_exec_parameter_read_environment(&machine, block,
						&environment) !=
		DOS_EXEC_PARAMETER_MACHINE_FAULT ||
	    environment != 0xa5a5u)
		return 9;
	fail_read = true;
	address.segment = 0xaaaau;
	address.offset = 0xbbbbu;
	if (dos_exec_parameter_read_command_tail(&machine, block, &address) !=
		DOS_EXEC_PARAMETER_MACHINE_FAULT ||
	    address.segment != 0xaaaau || address.offset != 0xbbbbu)
		return 10;

	/* Exec1_SP begins at block+14 with 16-bit effective-offset wrap.  The
	 * complete tuple is one journal record and abort restores all eight old
	 * bytes. */
	block.segment = 0x1000u;
	block.offset = 0xfff8u;
	for (index = 0u; index < sizeof(old_result); ++index) {
		old_result[index] = (uint8_t)(0xa0u + index);
		guest_memory[0x10006u + index] = old_result[index];
	}
	if (dos_exec_journal_construct(&journal) != DOS_EXEC_JOURNAL_OK ||
	    dos_exec_journal_initialize(&journal, TEST_MACHINE_IDENTITY,
					&machine) != DOS_EXEC_JOURNAL_OK)
		return 11;
	calls_before = read_calls;
	if (dos_exec_parameter_stage_load_result(
		&journal, TEST_MACHINE_IDENTITY, &machine, block, NULL) !=
		DOS_EXEC_JOURNAL_INVALID_ARGUMENT ||
	    read_calls != calls_before || journal.record_count != 0u)
		return 12;
	if (dos_exec_parameter_stage_load_result(
		&journal, TEST_MACHINE_IDENTITY, &machine, block, &result) !=
		DOS_EXEC_JOURNAL_OK ||
	    journal.record_count != 1u ||
	    journal.records[0].segment != 0x1000u ||
	    journal.records[0].offset != 0x0006u ||
	    journal.records[0].count != sizeof(encoded_result))
		return 13;
	for (index = 0u; index < sizeof(encoded_result); ++index) {
		if (guest_memory[0x10006u + index] != encoded_result[index])
			return 14;
	}
	if (dos_exec_journal_abort(&journal, TEST_MACHINE_IDENTITY, &machine) !=
	    DOS_EXEC_JOURNAL_OK)
		return 15;
	for (index = 0u; index < sizeof(old_result); ++index) {
		if (guest_memory[0x10006u + index] != old_result[index])
			return 16;
	}
	if (write_calls == 0u)
		return 17;
	return 0;
}

DOSC32_TEST_ENTRY(run_tests)

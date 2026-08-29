// SPDX-License-Identifier: GPL-2.0-only
/* Freestanding DOS EXEC environment layout, bounds and poison tests. */
#include "dos_environment.h"
#include "test_entry.h"

#define GUEST_MEMORY_BYTES 0x100000u
#define TEST_CONTEXT ((kernel_object_handle_t)0x454e5649524f4e4dull)

static uint8_t guest_memory[GUEST_MEMORY_BYTES];
static uint32_t read_calls;
static uint32_t write_calls;
static uint32_t fail_read_call;
static uint32_t fail_write_call;
static bool fail_all_writes;
static dos_linear_address_t watched_name_linear;
static uint32_t writes_before_first_name_read;
static bool watched_name_was_read;

static enum dos_machine_status
read_memory(kernel_object_handle_t context, dos_linear_address_t linear_address,
	    void *destination, size_t destination_capacity, size_t count)
{
	uint8_t *output = (uint8_t *)destination;
	size_t index;

	++read_calls;
	if (context != TEST_CONTEXT || destination == NULL ||
	    count > destination_capacity ||
	    linear_address > GUEST_MEMORY_BYTES ||
	    count > GUEST_MEMORY_BYTES - (size_t)linear_address)
		return DOS_MACHINE_ADDRESS_FAULT;
	if (fail_read_call != 0u && read_calls == fail_read_call)
		return DOS_MACHINE_IO_FAULT;
	if (!watched_name_was_read && linear_address == watched_name_linear) {
		watched_name_was_read = true;
		writes_before_first_name_read = write_calls;
	}
	for (index = 0u; index < count; ++index)
		output[index] = guest_memory[(size_t)linear_address + index];
	return DOS_MACHINE_OK;
}

static enum dos_machine_status write_memory(kernel_object_handle_t context,
					    dos_linear_address_t linear_address,
					    const void *source,
					    size_t source_capacity,
					    size_t count)
{
	const uint8_t *input = (const uint8_t *)source;
	size_t index;

	++write_calls;
	if (context != TEST_CONTEXT || source == NULL ||
	    count > source_capacity || linear_address > GUEST_MEMORY_BYTES ||
	    count > GUEST_MEMORY_BYTES - (size_t)linear_address)
		return DOS_MACHINE_ADDRESS_FAULT;
	if (fail_all_writes ||
	    (fail_write_call != 0u && write_calls == fail_write_call)) {
		/* A backend may have modified memory before reporting failure.
		 */
		if (count != 0u)
			guest_memory[linear_address] = 0xeeu;
		return DOS_MACHINE_IO_FAULT;
	}
	for (index = 0u; index < count; ++index)
		guest_memory[(size_t)linear_address + index] = input[index];
	return DOS_MACHINE_OK;
}

static const struct dos_machine_ops machine_ops = {
    .read_memory = read_memory,
    .write_memory = write_memory,
    .read_port = NULL,
    .write_port = NULL,
    .set_a20 = NULL,
};

static void fill_memory(uint8_t value)
{
	size_t index;

	for (index = 0u; index < sizeof(guest_memory); ++index)
		guest_memory[index] = value;
	watched_name_linear = GUEST_MEMORY_BYTES;
	writes_before_first_name_read = 0u;
	watched_name_was_read = false;
}

static void copy_bytes(uint32_t destination, const uint8_t *source,
		       size_t count)
{
	size_t index;

	for (index = 0u; index < count; ++index)
		guest_memory[(size_t)destination + index] = source[index];
}

static void set_name_plan(struct dos_exec_name_plan *plan,
			  struct dos_far_pointer16 source, uint16_t bytes)
{
	plan->source.offset = source.offset;
	plan->source.segment = source.segment;
	plan->bytes_including_nul = bytes;
	plan->reserved = 0u;
}

static bool bytes_equal(uint32_t address, const uint8_t *expected, size_t count)
{
	size_t index;

	for (index = 0u; index < count; ++index) {
		if (guest_memory[(size_t)address + index] != expected[index])
			return false;
	}
	return true;
}

static bool plan_is_sentinel(const struct dos_environment_plan *plan)
{
	return plan->source.offset == 0x1111u &&
	       plan->source.segment == 0x2222u &&
	       plan->environment_bytes == 0x33333333u &&
	       plan->executable_name.source.offset == 0x4444u &&
	       plan->executable_name.source.segment == 0x5555u &&
	       plan->executable_name.bytes_including_nul == 0x6666u &&
	       plan->executable_name.reserved == 0x7777u &&
	       plan->payload_bytes == 0x88888888u &&
	       plan->allocation_bytes == 0x99999999u &&
	       plan->paragraphs == 0xaaaau && plan->reserved == 0xbbbbu;
}

static void set_sentinel(struct dos_environment_plan *plan)
{
	plan->source.offset = 0x1111u;
	plan->source.segment = 0x2222u;
	plan->environment_bytes = 0x33333333u;
	plan->executable_name.source.offset = 0x4444u;
	plan->executable_name.source.segment = 0x5555u;
	plan->executable_name.bytes_including_nul = 0x6666u;
	plan->executable_name.reserved = 0x7777u;
	plan->payload_bytes = 0x88888888u;
	plan->allocation_bytes = 0x99999999u;
	plan->paragraphs = 0xaaaau;
	plan->reserved = 0xbbbbu;
}

static int test_exact_layout(struct dos_machine *machine)
{
	static const uint8_t environment[] = {
	    'A', '=', '1', 0u, 'B', '=', '2', 0u, 0u,
	};
	static const uint8_t executable_name[] = "C:\\BIN\\APP.EXE";
	struct dos_far_pointer16 source = {.offset = 0u, .segment = 0x2000u};
	struct dos_far_pointer16 name_source = {
	    .offset = 0x0100u,
	    .segment = 0x1000u,
	};
	struct dos_far_pointer16 destination = {
	    .offset = 0u,
	    .segment = 0x3000u,
	};
	struct dos_exec_name_plan name_plan;
	struct dos_environment_plan plan;
	uint32_t source_linear = 0x20000u;
	uint32_t name_linear = 0x10100u;
	uint32_t target_linear = 0x30000u;
	uint32_t expected_payload = (uint32_t)sizeof(environment) + 2u +
				    (uint32_t)sizeof(executable_name);
	size_t index;

	fill_memory(0xccu);
	copy_bytes(source_linear, environment, sizeof(environment));
	copy_bytes(name_linear, executable_name, sizeof(executable_name));
	set_name_plan(&name_plan, name_source,
		      (uint16_t)sizeof(executable_name));
	read_calls = 0u;
	write_calls = 0u;
	fail_read_call = 0u;
	fail_write_call = 0u;
	fail_all_writes = false;
	watched_name_linear = name_linear;
	if (dos_environment_plan_create(machine, source, &name_plan, &plan) !=
	    DOS_ENVIRONMENT_OK)
		return 1;
	if (watched_name_was_read)
		return 2;
	if (plan.source.offset != source.offset ||
	    plan.source.segment != source.segment ||
	    plan.environment_bytes != sizeof(environment) ||
	    plan.executable_name.source.offset != name_source.offset ||
	    plan.executable_name.source.segment != name_source.segment ||
	    plan.executable_name.bytes_including_nul != sizeof(executable_name) ||
	    plan.executable_name.reserved != 0u ||
	    plan.payload_bytes != expected_payload ||
	    plan.paragraphs != (uint16_t)((expected_payload + 15u) >> 4) ||
	    plan.allocation_bytes != (uint32_t)plan.paragraphs * 16u ||
	    plan.reserved != 0u)
		return 3;
	watched_name_was_read = false;
	if (dos_environment_build(machine, &plan, destination) !=
	    DOS_ENVIRONMENT_OK)
		return 4;
	if (!watched_name_was_read || writes_before_first_name_read != 2u)
		return 5;
	if (!bytes_equal(target_linear, environment, sizeof(environment)) ||
	    guest_memory[target_linear + sizeof(environment)] != 1u ||
	    guest_memory[target_linear + sizeof(environment) + 1u] != 0u ||
	    !bytes_equal(target_linear + sizeof(environment) + 2u,
			 executable_name, sizeof(executable_name)))
		return 6;
	for (index = plan.payload_bytes; index < plan.allocation_bytes;
	     ++index) {
		if (guest_memory[target_linear + index] != 0xccu)
			return 7;
	}
	return 0;
}

static int test_output_unchanged_and_limit(struct dos_machine *machine)
{
	struct dos_far_pointer16 source = {.offset = 0u, .segment = 0x4000u};
	struct dos_far_pointer16 name_source = {
	    .offset = 0u,
	    .segment = 0x8000u,
	};
	struct dos_exec_name_plan name_plan;
	struct dos_environment_plan plan;
	uint32_t source_linear = 0x40000u;
	uint32_t index;

	fill_memory(0x41u);
	set_name_plan(&name_plan, name_source, 6u);
	set_sentinel(&plan);
	if (dos_environment_plan_create(machine, source, &name_plan, &plan) !=
		DOS_ENVIRONMENT_BAD_SOURCE ||
	    !plan_is_sentinel(&plan))
		return 1;
	name_plan.bytes_including_nul = 0u;
	set_sentinel(&plan);
	if (dos_environment_plan_create(machine, source, &name_plan, &plan) !=
		DOS_ENVIRONMENT_INVALID_ARGUMENT ||
	    !plan_is_sentinel(&plan))
		return 2;
	set_name_plan(&name_plan, name_source, 6u);

	/* Both bytes of the terminator may occupy the last two scan slots. */
	for (index = 0u; index < DOS_ENVIRONMENT_SCAN_LIMIT; ++index)
		guest_memory[source_linear + index] = 0x41u;
	guest_memory[source_linear + DOS_ENVIRONMENT_SCAN_LIMIT - 2u] = 0u;
	guest_memory[source_linear + DOS_ENVIRONMENT_SCAN_LIMIT - 1u] = 0u;
	if (dos_environment_plan_create(machine, source, &name_plan, &plan) !=
		DOS_ENVIRONMENT_OK ||
	    plan.environment_bytes != DOS_ENVIRONMENT_SCAN_LIMIT)
		return 3;

	/* One NUL in the final slot is not a complete environment. */
	guest_memory[source_linear + DOS_ENVIRONMENT_SCAN_LIMIT - 2u] = 0x41u;
	if (dos_environment_plan_create(machine, source, &name_plan, &plan) !=
	    DOS_ENVIRONMENT_BAD_SOURCE)
		return 4;

	/* Checked wide sizing prevents a wrapping 16-bit length sum. */
	guest_memory[source_linear] = 0u;
	guest_memory[source_linear + 1u] = 0u;
	set_name_plan(&name_plan, name_source, 0xffffu);
	read_calls = 0u;
	set_sentinel(&plan);
	if (dos_environment_plan_create(machine, source, &name_plan, &plan) !=
		DOS_ENVIRONMENT_RANGE_OVERFLOW ||
	    !plan_is_sentinel(&plan) || read_calls != 2u)
		return 5;
	return 0;
}

static int test_far_wraps(struct dos_machine *machine)
{
	static const uint8_t name[] = "W.COM";
	struct dos_machine a20_machine;
	struct dos_exec_name_plan name_plan;
	struct dos_environment_plan plan;
	struct dos_far_pointer16 name_source = {
	    .offset = 0u,
	    .segment = 0x3000u,
	};
	struct dos_far_pointer16 offset_wrap = {
	    .offset = 0xfffeu,
	    .segment = 0x1000u,
	};
	struct dos_far_pointer16 a20_wrap = {
	    .offset = 0x000fu,
	    .segment = 0xffffu,
	};

	fill_memory(0x55u);
	copy_bytes(0x30000u, name, sizeof(name));
	set_name_plan(&name_plan, name_source, (uint16_t)sizeof(name));
	/* 1000:fffe, 1000:ffff, then 16-bit DI wraps to 1000:0000. */
	guest_memory[0x1fffeu] = 'X';
	guest_memory[0x1ffffu] = 0u;
	guest_memory[0x10000u] = 0u;
	if (dos_environment_plan_create(machine, offset_wrap, &name_plan,
					&plan) != DOS_ENVIRONMENT_OK ||
	    plan.environment_bytes != 3u)
		return 1;

	/* FFFF:000f is fffff; the following byte wraps through disabled A20. */
	guest_memory[0xfffffu] = 'Y';
	guest_memory[0u] = 0u;
	guest_memory[1u] = 0u;
	if (dos_environment_plan_create(machine, a20_wrap, &name_plan, &plan) !=
		DOS_ENVIRONMENT_OK ||
	    plan.environment_bytes != 3u)
		return 2;

	/* With A20 enabled the same second byte is above this machine's limit.
	 */
	if (dos_machine_configure(&a20_machine, &machine_ops, TEST_CONTEXT,
				  GUEST_MEMORY_BYTES, true) != DOS_MACHINE_OK)
		return 3;
	set_sentinel(&plan);
	if (dos_environment_plan_create(&a20_machine, a20_wrap, &name_plan,
					&plan) !=
		DOS_ENVIRONMENT_SOURCE_FAULT ||
	    !plan_is_sentinel(&plan))
		return 4;
	return 0;
}

static int test_fault_boundaries(struct dos_machine *machine)
{
	static const uint8_t environment[] = {'A', '=', 'B', 0u, 0u};
	static const uint8_t name[] = "F.COM";
	struct dos_far_pointer16 source = {.offset = 0u, .segment = 0x2000u};
	struct dos_far_pointer16 name_source = {
	    .offset = 0u,
	    .segment = 0x1000u,
	};
	struct dos_far_pointer16 destination = {
	    .offset = 0u,
	    .segment = 0x3000u,
	};
	struct dos_far_pointer16 invalid_destination = {
	    .offset = 0u,
	    .segment = 0x5000u,
	};
	struct dos_exec_name_plan name_plan;
	struct dos_environment_plan plan;
	struct dos_machine small_machine;

	fill_memory(0x5au);
	copy_bytes(0x20000u, environment, sizeof(environment));
	copy_bytes(0x10000u, name, sizeof(name));
	set_name_plan(&name_plan, name_source, (uint16_t)sizeof(name));
	if (dos_environment_plan_create(machine, source, &name_plan, &plan) !=
	    DOS_ENVIRONMENT_OK)
		return 1;
	if (dos_machine_configure(&small_machine, &machine_ops, TEST_CONTEXT,
				  0x40000u, false) != DOS_MACHINE_OK)
		return 2;
	write_calls = 0u;
	if (dos_environment_build(&small_machine, &plan, invalid_destination) !=
		DOS_ENVIRONMENT_TARGET_FAULT ||
	    write_calls != 0u)
		return 3;

	read_calls = 0u;
	write_calls = 0u;
	fail_read_call = 1u;
	if (dos_environment_build(machine, &plan, destination) !=
		DOS_ENVIRONMENT_SOURCE_FAULT ||
	    write_calls != 0u)
		return 4;
	fail_read_call = 0u;
	read_calls = 0u;
	write_calls = 0u;
	fail_write_call = 1u;
	if (dos_environment_build(machine, &plan, destination) !=
		DOS_ENVIRONMENT_TARGET_FAULT ||
	    write_calls != 2u || guest_memory[0x30000u] != 0x5au)
		return 5;
	fail_write_call = 0u;
	write_calls = 0u;
	fail_all_writes = true;
	if (dos_environment_build(machine, &plan, destination) !=
		DOS_ENVIRONMENT_TARGET_POISONED ||
	    write_calls != 2u || guest_memory[0x30000u] != 0xeeu)
		return 6;
	fail_all_writes = false;

	destination.offset = 1u;
	write_calls = 0u;
	if (dos_environment_build(machine, &plan, destination) !=
		DOS_ENVIRONMENT_INVALID_ARGUMENT ||
	    write_calls != 0u)
		return 7;
	return 0;
}

static int test_stale_and_late_source_fault(struct dos_machine *machine)
{
	static const uint8_t name[] = "L.COM";
	struct dos_far_pointer16 source = {.offset = 0u, .segment = 0x6000u};
	struct dos_far_pointer16 name_source = {
	    .offset = 0u,
	    .segment = 0x5000u,
	};
	struct dos_far_pointer16 destination = {
	    .offset = 0u,
	    .segment = 0x7000u,
	};
	struct dos_exec_name_plan name_plan;
	struct dos_environment_plan plan;
	uint32_t source_linear = 0x60000u;
	uint32_t index;

	fill_memory(0x6au);
	for (index = 0u; index < 200u; ++index)
		guest_memory[source_linear + index] = 'Q';
	guest_memory[source_linear + 200u] = 0u;
	guest_memory[source_linear + 201u] = 0u;
	copy_bytes(0x50000u, name, sizeof(name));
	set_name_plan(&name_plan, name_source, (uint16_t)sizeof(name));
	if (dos_environment_plan_create(machine, source, &name_plan, &plan) !=
	    DOS_ENVIRONMENT_OK)
		return 1;

	guest_memory[source_linear + 10u] = 0u;
	guest_memory[source_linear + 11u] = 0u;
	write_calls = 0u;
	if (dos_environment_build(machine, &plan, destination) !=
		DOS_ENVIRONMENT_STALE_PLAN ||
	    write_calls != 0u)
		return 2;
	guest_memory[source_linear + 10u] = 'Q';
	guest_memory[source_linear + 11u] = 'Q';

	/*
	 * 202 scan reads, one source read, one target-backup read and one
	 * write; then fail the second source-chunk read.
	 */
	read_calls = 0u;
	write_calls = 0u;
	fail_read_call = 205u;
	if (dos_environment_build(machine, &plan, destination) !=
		DOS_ENVIRONMENT_TARGET_FAULT ||
	    write_calls != 1u)
		return 3;
	fail_read_call = 0u;
	return 0;
}

static int test_guest_name_revalidation(struct dos_machine *machine)
{
	static const uint8_t environment[] = {0u, 0u};
	static const uint8_t name[] = "R.COM";
	struct dos_far_pointer16 source = {.offset = 0u, .segment = 0x2000u};
	struct dos_far_pointer16 name_source = {
	    .offset = 0u,
	    .segment = 0x1000u,
	};
	struct dos_far_pointer16 destination = {
	    .offset = 0u,
	    .segment = 0x3000u,
	};
	struct dos_exec_name_plan name_plan;
	struct dos_environment_plan plan;

	fill_memory(0x7au);
	copy_bytes(0x20000u, environment, sizeof(environment));
	copy_bytes(0x10000u, name, sizeof(name));
	set_name_plan(&name_plan, name_source, (uint16_t)sizeof(name));
	if (dos_environment_plan_create(machine, source, &name_plan, &plan) !=
	    DOS_ENVIRONMENT_OK)
		return 1;

	/* Stop on the newly early NUL; do not fetch any following guest byte. */
	guest_memory[0x10001u] = 0u;
	read_calls = 0u;
	write_calls = 0u;
	if (dos_environment_build(machine, &plan, destination) !=
		DOS_ENVIRONMENT_STALE_PLAN ||
	    read_calls != 7u || write_calls != 2u)
		return 2;

	copy_bytes(0x10000u, name, sizeof(name));
	guest_memory[0x10000u + sizeof(name) - 1u] = 'X';
	read_calls = 0u;
	write_calls = 0u;
	if (dos_environment_build(machine, &plan, destination) !=
		DOS_ENVIRONMENT_STALE_PLAN ||
	    read_calls != 11u || write_calls != 2u)
		return 3;

	/* The first name read is deliberately after environment and word 1. */
	copy_bytes(0x10000u, name, sizeof(name));
	read_calls = 0u;
	write_calls = 0u;
	fail_read_call = 6u;
	if (dos_environment_build(machine, &plan, destination) !=
		DOS_ENVIRONMENT_TARGET_FAULT ||
	    write_calls != 2u)
		return 4;
	fail_read_call = 0u;

	plan.executable_name.reserved = 1u;
	read_calls = 0u;
	write_calls = 0u;
	if (dos_environment_build(machine, &plan, destination) !=
		DOS_ENVIRONMENT_INVALID_ARGUMENT ||
	    read_calls != 0u || write_calls != 0u)
		return 5;
	return 0;
}

static int run_tests(void)
{
	struct dos_machine machine;
	int status;

	if (dos_machine_configure(&machine, &machine_ops, TEST_CONTEXT,
				  GUEST_MEMORY_BYTES, false) != DOS_MACHINE_OK)
		return 1;
	status = test_exact_layout(&machine);
	if (status != 0)
		return 10 + status;
	status = test_output_unchanged_and_limit(&machine);
	if (status != 0)
		return 20 + status;
	status = test_far_wraps(&machine);
	if (status != 0)
		return 30 + status;
	status = test_fault_boundaries(&machine);
	if (status != 0)
		return 40 + status;
	status = test_stale_and_late_source_fault(&machine);
	if (status != 0)
		return 50 + status;
	status = test_guest_name_revalidation(&machine);
	if (status != 0)
		return 60 + status;
	return 0;
}

DOSC32_TEST_ENTRY(run_tests)

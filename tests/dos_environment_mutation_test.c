// SPDX-License-Identifier: GPL-2.0-only
/* PSP environment mutation, publication and rollback fault tests. */
#include "dos_environment_mutation.h"
#include "test_entry.h"

#define TEST_MEMORY_BYTES 0x50000u
#define TEST_CONTEXT ((kernel_object_handle_t)0x454e564d55544131ull)
#define TEST_ARENA_ID ((kernel_object_handle_t)0x454e564152454e31ull)
#define TEST_ARENA_HEAD 0x0fffu
#define TEST_ARENA_END 0x3000u
#define TEST_PSP 0x1000u
#define TEST_PSP_PARAGRAPHS 16u
#define TEST_ENVIRONMENT_PARAGRAPHS 16u
#define TEST_EXACT_END 0x1021u

static const uint8_t initial_environment[] = {
	'P', 'A', 'T', 'H', '=', 'O', 'L', 'D', 0u,
	'C', 'o', 'm', 'S', 'p', 'e', 'c', '=', 'C', ':', 0x5cu,
	'C', 'O', 'M', 'M', 'A', 'N', 'D', '.', 'C', 'O', 'M', 0u,
	0u, 1u, 0u,
	'C', ':', 0x5cu, 'C', 'O', 'M', 'M', 'A', 'N', 'D', '.', 'C',
	'O', 'M', 0u,
};

static uint8_t guest_memory[TEST_MEMORY_BYTES];
static uint8_t old_environment_snapshot[
	TEST_ENVIRONMENT_PARAGRAPHS * 16u];
static dos_linear_address_t fail_write_address;
static uint32_t fail_write_remaining;
static bool fail_after_copy;

static bool range_contains(dos_linear_address_t start, size_t count,
			   dos_linear_address_t address)
{
	return address >= start &&
	       (uint64_t)address - (uint64_t)start < (uint64_t)count;
}

static enum dos_machine_status
read_memory(kernel_object_handle_t context, dos_linear_address_t address,
	    void *destination, size_t destination_capacity, size_t count)
{
	uint8_t *output = (uint8_t *)destination;
	size_t index;

	if (context != TEST_CONTEXT || destination == NULL ||
	    count > destination_capacity || address > TEST_MEMORY_BYTES ||
	    count > TEST_MEMORY_BYTES - (size_t)address)
		return DOS_MACHINE_ADDRESS_FAULT;
	for (index = 0u; index < count; ++index)
		output[index] = guest_memory[(size_t)address + index];
	return DOS_MACHINE_OK;
}

static enum dos_machine_status
write_memory(kernel_object_handle_t context, dos_linear_address_t address,
	     const void *source, size_t source_capacity, size_t count)
{
	const uint8_t *input = (const uint8_t *)source;
	bool fail;
	size_t index;

	if (context != TEST_CONTEXT || source == NULL || count > source_capacity ||
	    address > TEST_MEMORY_BYTES ||
	    count > TEST_MEMORY_BYTES - (size_t)address)
		return DOS_MACHINE_ADDRESS_FAULT;
	fail = fail_write_remaining != 0u &&
	       range_contains(address, count, fail_write_address);
	if (!fail || fail_after_copy) {
		for (index = 0u; index < count; ++index)
			guest_memory[(size_t)address + index] = input[index];
	}
	if (fail) {
		--fail_write_remaining;
		return DOS_MACHINE_IO_FAULT;
	}
	return DOS_MACHINE_OK;
}

static const struct dos_machine_ops machine_ops = {
	.read_memory = read_memory,
	.write_memory = write_memory,
};

static void clear_bytes(uint8_t *bytes, size_t count)
{
	size_t index;

	for (index = 0u; index < count; ++index)
		bytes[index] = 0u;
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

static uint16_t read_le16(size_t address)
{
	return (uint16_t)guest_memory[address] |
	       ((uint16_t)guest_memory[address + 1u] << 8u);
}

static void write_le16(size_t address, uint16_t value)
{
	guest_memory[address] = (uint8_t)value;
	guest_memory[address + 1u] = (uint8_t)(value >> 8u);
}

static uint16_t psp_environment_segment(void)
{
	return read_le16(((size_t)TEST_PSP << 4u) + 0x2cu);
}

static uint16_t mcb_owner(uint16_t block_segment)
{
	return read_le16(((size_t)(block_segment - 1u) << 4u) + 1u);
}

static void snapshot_environment(uint16_t segment)
{
	size_t linear = (size_t)segment << 4u;
	size_t index;

	for (index = 0u; index < sizeof(old_environment_snapshot); ++index)
		old_environment_snapshot[index] = guest_memory[linear + index];
}

static uint32_t count_psp_owned_blocks(void)
{
	uint16_t segment = TEST_ARENA_HEAD;
	uint32_t count = 0u;
	uint32_t steps;

	for (steps = 0u; steps < 64u; ++steps) {
		size_t linear = (size_t)segment << 4u;
		uint8_t signature = guest_memory[linear];
		uint16_t owner = read_le16(linear + 1u);
		uint16_t paragraphs = read_le16(linear + 3u);

		if (signature != (uint8_t)'M' && signature != (uint8_t)'Z')
			return 0xffffffffu;
		if (owner == TEST_PSP)
			++count;
		if (signature == (uint8_t)'Z')
			return count;
		segment = (uint16_t)((uint32_t)segment +
				     (uint32_t)paragraphs + 1u);
	}
	return 0xffffffffu;
}

static bool setup_runtime(struct dos_machine *machine,
			  struct dos_memory_arena *arena,
			  uint16_t arena_end, uint16_t *old_environment)
{
	struct dos_memory_allocation_result allocation;
	size_t environment_linear;
	size_t index;

	clear_bytes(guest_memory, sizeof(guest_memory));
	fail_write_address = 0u;
	fail_write_remaining = 0u;
	fail_after_copy = false;
	if (dos_machine_configure(machine, &machine_ops, TEST_CONTEXT,
				  sizeof(guest_memory), true) != DOS_MACHINE_OK)
		return false;
	*arena = (struct dos_memory_arena)
		DOS_MEMORY_ARENA_INITIALIZER(TEST_ARENA_ID);
	if (dos_memory_arena_initialize_checked(
			arena, machine, TEST_ARENA_HEAD, arena_end) !=
	    DOS_MEMORY_OK)
		return false;
	if (dos_memory_allocate_checked(arena, machine, TEST_PSP,
					TEST_PSP_PARAGRAPHS,
					&allocation) != DOS_MEMORY_OK ||
	    allocation.block_segment != TEST_PSP)
		return false;
	if (dos_memory_allocate_checked(
			arena, machine, TEST_PSP, TEST_ENVIRONMENT_PARAGRAPHS,
			&allocation) != DOS_MEMORY_OK)
		return false;
	*old_environment = allocation.block_segment;
	environment_linear = (size_t)*old_environment << 4u;
	for (index = 0u; index < sizeof(initial_environment); ++index)
		guest_memory[environment_linear + index] =
			initial_environment[index];
	write_le16(((size_t)TEST_PSP << 4u) + 0x2cu, *old_environment);
	snapshot_environment(*old_environment);
	return true;
}

static bool failure_preserved_state(const struct dos_machine *machine,
				    const struct dos_memory_arena *arena,
				    uint16_t old_environment)
{
	size_t environment_linear = (size_t)old_environment << 4u;

	return psp_environment_segment() == old_environment &&
	       mcb_owner(old_environment) == TEST_PSP &&
	       bytes_equal(guest_memory + environment_linear,
			   old_environment_snapshot,
			   sizeof(old_environment_snapshot)) &&
	       count_psp_owned_blocks() == 2u && arena->machine_poisoned == 0u &&
	       dos_memory_arena_validate_checked(arena, machine) == DOS_MEMORY_OK;
}

static bool environment_equals(uint16_t segment, const uint8_t *expected,
			       size_t expected_bytes)
{
	size_t linear = (size_t)segment << 4u;

	return bytes_equal(guest_memory + linear, expected, expected_bytes);
}

static int test_set_add_empty_delete(void)
{
	static const uint8_t after_replace[] = {
		'C', 'o', 'm', 'S', 'p', 'e', 'c', '=', 'C', ':', 0x5cu,
		'C', 'O', 'M', 'M', 'A', 'N', 'D', '.', 'C', 'O', 'M', 0u,
		'P', 'A', 'T', 'H', '=', 'C', ':', 0x5cu, 'B', 'I', 'N', 0u,
		0u, 1u, 0u,
		'C', ':', 0x5cu, 'C', 'O', 'M', 'M', 'A', 'N', 'D', '.', 'C',
		'O', 'M', 0u,
	};
	static const uint8_t after_add[] = {
		'C', 'o', 'm', 'S', 'p', 'e', 'c', '=', 'C', ':', 0x5cu,
		'C', 'O', 'M', 'M', 'A', 'N', 'D', '.', 'C', 'O', 'M', 0u,
		'P', 'A', 'T', 'H', '=', 'C', ':', 0x5cu, 'B', 'I', 'N', 0u,
		'T', 'E', 'M', 'P', '=', '4', '2', 0u,
		0u, 1u, 0u,
		'C', ':', 0x5cu, 'C', 'O', 'M', 'M', 'A', 'N', 'D', '.', 'C',
		'O', 'M', 0u,
	};
	static const uint8_t after_empty[] = {
		'C', 'o', 'm', 'S', 'p', 'e', 'c', '=', 'C', ':', 0x5cu,
		'C', 'O', 'M', 'M', 'A', 'N', 'D', '.', 'C', 'O', 'M', 0u,
		'P', 'A', 'T', 'H', '=', 'C', ':', 0x5cu, 'B', 'I', 'N', 0u,
		'T', 'E', 'M', 'P', '=', '4', '2', 0u,
		'E', 'M', 'P', 'T', 'Y', '=', 0u,
		0u, 1u, 0u,
		'C', ':', 0x5cu, 'C', 'O', 'M', 'M', 'A', 'N', 'D', '.', 'C',
		'O', 'M', 0u,
	};
	static const uint8_t after_delete[] = {
		'P', 'A', 'T', 'H', '=', 'C', ':', 0x5cu, 'B', 'I', 'N', 0u,
		'T', 'E', 'M', 'P', '=', '4', '2', 0u,
		'E', 'M', 'P', 'T', 'Y', '=', 0u,
		0u, 1u, 0u,
		'C', ':', 0x5cu, 'C', 'O', 'M', 'M', 'A', 'N', 'D', '.', 'C',
		'O', 'M', 0u,
	};
	struct dos_machine machine;
	struct dos_memory_arena arena;
	uint16_t old_environment;
	uint16_t current;

	if (!setup_runtime(&machine, &arena, TEST_ARENA_END,
			   &old_environment))
		return 1;
	if (dos_environment_mutate_psp(
			&arena, &machine, TEST_PSP, DOS_ENVIRONMENT_MUTATION_SET,
			(const uint8_t *)"path", 4u, 4u,
			(const uint8_t *)"C:\\BIN", 6u, 6u) !=
	    DOS_ENVIRONMENT_MUTATION_OK)
		return 2;
	current = psp_environment_segment();
	if (current == old_environment || mcb_owner(old_environment) != 0u ||
	    mcb_owner(current) != TEST_PSP ||
	    !environment_equals(current, after_replace, sizeof(after_replace)))
		return 3;
	if (dos_environment_mutate_psp(
			&arena, &machine, TEST_PSP, DOS_ENVIRONMENT_MUTATION_SET,
			(const uint8_t *)"temp", 4u, 4u,
			(const uint8_t *)"42", 2u, 2u) !=
	    DOS_ENVIRONMENT_MUTATION_OK)
		return 4;
	current = psp_environment_segment();
	if (!environment_equals(current, after_add, sizeof(after_add)))
		return 5;
	if (dos_environment_mutate_psp(
			&arena, &machine, TEST_PSP, DOS_ENVIRONMENT_MUTATION_SET,
			(const uint8_t *)"empty", 5u, 5u, NULL, 0u, 0u) !=
	    DOS_ENVIRONMENT_MUTATION_OK)
		return 6;
	current = psp_environment_segment();
	if (!environment_equals(current, after_empty, sizeof(after_empty)))
		return 7;
	if (dos_environment_mutate_psp(
			&arena, &machine, TEST_PSP,
			DOS_ENVIRONMENT_MUTATION_DELETE,
			(const uint8_t *)"COMSPEC", 7u, 7u, NULL, 0u, 0u) !=
	    DOS_ENVIRONMENT_MUTATION_OK)
		return 8;
	current = psp_environment_segment();
	if (!environment_equals(current, after_delete, sizeof(after_delete)) ||
	    count_psp_owned_blocks() != 2u ||
	    dos_memory_arena_validate_checked(&arena, &machine) != DOS_MEMORY_OK)
		return 9;
	if (dos_environment_mutate_psp(
			&arena, &machine, TEST_PSP,
			DOS_ENVIRONMENT_MUTATION_DELETE,
			(const uint8_t *)"MISSING", 7u, 7u, NULL, 0u, 0u) !=
		DOS_ENVIRONMENT_MUTATION_NOT_FOUND ||
	    psp_environment_segment() != current)
		return 10;
	return 0;
}

static int test_allocation_failure(void)
{
	struct dos_machine machine;
	struct dos_memory_arena arena;
	uint16_t old_environment;

	if (!setup_runtime(&machine, &arena, TEST_EXACT_END,
			   &old_environment))
		return 1;
	if (dos_environment_mutate_psp(
			&arena, &machine, TEST_PSP, DOS_ENVIRONMENT_MUTATION_SET,
			(const uint8_t *)"PATH", 4u, 4u,
			(const uint8_t *)"NEW", 3u, 3u) !=
	    DOS_ENVIRONMENT_MUTATION_NOT_ENOUGH_MEMORY)
		return 2;
	return failure_preserved_state(&machine, &arena, old_environment)
		       ? 0
		       : 3;
}

static int test_delete_last_entry(void)
{
	static const uint8_t only_environment[] = {
		'O', 'N', 'L', 'Y', '=', 'X', 0u, 0u, 1u, 0u,
		'C', ':', 0x5cu, 'C', 'O', 'M', 'M', 'A', 'N', 'D', '.', 'C',
		'O', 'M', 0u,
	};
	static const uint8_t empty_environment[] = {
		0u, 0u, 1u, 0u,
		'C', ':', 0x5cu, 'C', 'O', 'M', 'M', 'A', 'N', 'D', '.', 'C',
		'O', 'M', 0u,
	};
	struct dos_machine machine;
	struct dos_memory_arena arena;
	uint16_t old_environment;
	size_t linear;
	size_t index;

	if (!setup_runtime(&machine, &arena, TEST_ARENA_END,
			   &old_environment))
		return 1;
	linear = (size_t)old_environment << 4u;
	clear_bytes(guest_memory + linear, sizeof(old_environment_snapshot));
	for (index = 0u; index < sizeof(only_environment); ++index)
		guest_memory[linear + index] = only_environment[index];
	if (dos_environment_mutate_psp(
			&arena, &machine, TEST_PSP,
			DOS_ENVIRONMENT_MUTATION_DELETE,
			(const uint8_t *)"only", 4u, 4u, NULL, 0u, 0u) !=
	    DOS_ENVIRONMENT_MUTATION_OK)
		return 2;
	return environment_equals(psp_environment_segment(), empty_environment,
				  sizeof(empty_environment))
		       ? 0
		       : 3;
}

static int test_bad_layout_preserves_state(void)
{
	struct dos_machine machine;
	struct dos_memory_arena arena;
	uint16_t old_environment;
	size_t linear;
	size_t index;

	if (!setup_runtime(&machine, &arena, TEST_ARENA_END,
			   &old_environment))
		return 1;
	linear = (size_t)old_environment << 4u;
	for (index = 0u; index + 1u < sizeof(initial_environment); ++index) {
		if (guest_memory[linear + index] == 0u &&
		    guest_memory[linear + index + 1u] == 0u)
			break;
	}
	if (index + 3u >= sizeof(initial_environment))
		return 2;
	guest_memory[linear + index + 2u] = 2u;
	snapshot_environment(old_environment);
	if (dos_environment_mutate_psp(
			&arena, &machine, TEST_PSP, DOS_ENVIRONMENT_MUTATION_SET,
			(const uint8_t *)"PATH", 4u, 4u,
			(const uint8_t *)"NEW", 3u, 3u) !=
	    DOS_ENVIRONMENT_MUTATION_BAD_LAYOUT)
		return 3;
	return failure_preserved_state(&machine, &arena, old_environment)
		       ? 0
		       : 4;
}

static int test_destination_write_failure(void)
{
	struct dos_machine machine;
	struct dos_memory_arena arena;
	uint16_t old_environment;
	uint16_t expected_replacement;

	if (!setup_runtime(&machine, &arena, TEST_ARENA_END,
			   &old_environment))
		return 1;
	expected_replacement =
		(uint16_t)(old_environment + TEST_ENVIRONMENT_PARAGRAPHS + 1u);
	fail_write_address = (dos_linear_address_t)expected_replacement << 4u;
	fail_write_remaining = 1u;
	fail_after_copy = true;
	if (dos_environment_mutate_psp(
			&arena, &machine, TEST_PSP, DOS_ENVIRONMENT_MUTATION_SET,
			(const uint8_t *)"PATH", 4u, 4u,
			(const uint8_t *)"NEW", 3u, 3u) !=
	    DOS_ENVIRONMENT_MUTATION_MACHINE_FAULT)
		return 2;
	return failure_preserved_state(&machine, &arena, old_environment)
		       ? 0
		       : 3;
}

static int test_psp_publish_failure(void)
{
	struct dos_machine machine;
	struct dos_memory_arena arena;
	uint16_t old_environment;

	if (!setup_runtime(&machine, &arena, TEST_ARENA_END,
			   &old_environment))
		return 1;
	fail_write_address = ((dos_linear_address_t)TEST_PSP << 4u) + 0x2cu;
	fail_write_remaining = 1u;
	fail_after_copy = true;
	if (dos_environment_mutate_psp(
			&arena, &machine, TEST_PSP, DOS_ENVIRONMENT_MUTATION_SET,
			(const uint8_t *)"PATH", 4u, 4u,
			(const uint8_t *)"NEW", 3u, 3u) !=
	    DOS_ENVIRONMENT_MUTATION_MACHINE_FAULT)
		return 2;
	return failure_preserved_state(&machine, &arena, old_environment)
		       ? 0
		       : 3;
}

static int test_old_free_failure_unwinds(void)
{
	struct dos_machine machine;
	struct dos_memory_arena arena;
	uint16_t old_environment;

	if (!setup_runtime(&machine, &arena, TEST_ARENA_END,
			   &old_environment))
		return 1;
	fail_write_address =
		(dos_linear_address_t)(old_environment - 1u) << 4u;
	fail_write_remaining = 1u;
	fail_after_copy = true;
	if (dos_environment_mutate_psp(
			&arena, &machine, TEST_PSP, DOS_ENVIRONMENT_MUTATION_SET,
			(const uint8_t *)"PATH", 4u, 4u,
			(const uint8_t *)"NEW", 3u, 3u) !=
	    DOS_ENVIRONMENT_MUTATION_MACHINE_FAULT)
		return 2;
	return failure_preserved_state(&machine, &arena, old_environment)
		       ? 0
		       : 3;
}

static int test_uncertain_publish_poison(void)
{
	struct dos_machine machine;
	struct dos_memory_arena arena;
	uint16_t old_environment;

	if (!setup_runtime(&machine, &arena, TEST_ARENA_END,
			   &old_environment))
		return 1;
	fail_write_address = ((dos_linear_address_t)TEST_PSP << 4u) + 0x2cu;
	fail_write_remaining = 2u;
	fail_after_copy = false;
	if (dos_environment_mutate_psp(
			&arena, &machine, TEST_PSP, DOS_ENVIRONMENT_MUTATION_SET,
			(const uint8_t *)"PATH", 4u, 4u,
			(const uint8_t *)"NEW", 3u, 3u) !=
	    DOS_ENVIRONMENT_MUTATION_POISONED)
		return 2;
	return arena.machine_poisoned == 1u &&
		       psp_environment_segment() == old_environment &&
		       count_psp_owned_blocks() == 3u
		       ? 0
		       : 3;
}

static int run_tests(void)
{
	int result;

	result = test_set_add_empty_delete();
	if (result != 0)
		return 10 + result;
	result = test_allocation_failure();
	if (result != 0)
		return 30 + result;
	result = test_delete_last_entry();
	if (result != 0)
		return 40 + result;
	result = test_bad_layout_preserves_state();
	if (result != 0)
		return 45 + result;
	result = test_destination_write_failure();
	if (result != 0)
		return 50 + result;
	result = test_psp_publish_failure();
	if (result != 0)
		return 70 + result;
	result = test_old_free_failure_unwinds();
	if (result != 0)
		return 90 + result;
	result = test_uncertain_publish_poison();
	if (result != 0)
		return 110 + result;
	return 0;
}

DOSC32_TEST_ENTRY(run_tests)

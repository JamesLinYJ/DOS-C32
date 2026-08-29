// SPDX-License-Identifier: GPL-2.0-only
/* Freestanding regression tests for the DOS MCB manager. */
#include "dos_memory.h"
#include "test_entry.h"

#define TEST_MEMORY_BYTES 0x00110000u
#define TEST_CONTEXT 0x4d434230u
#define MCB_BYTES 16u
#define MCB_NORMAL 0x4du
#define MCB_END 0x5au
#define MCB_NAME_OFFSET 8u
#define TEST_ARENA_IDENTITY(segment)                                           \
	((kernel_object_handle_t)0x4d43420000000000ull | (uint16_t)(segment))

static uint8_t guest_memory[TEST_MEMORY_BYTES];
static bool fail_read_enabled;
static bool fail_write_enabled;
static dos_linear_address_t fail_read_address;
static dos_linear_address_t fail_write_address;
static uint32_t fail_read_count;
static uint32_t fail_write_count;
static uint32_t fail_write_partial_bytes;
static uint32_t read_call_count;
static uint32_t write_call_count;
static uint64_t fail_write_call_mask;

static uint16_t get_le16(const uint8_t *bytes)
{
	return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static void put_le16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
}

static uint8_t *mcb_bytes(uint16_t segment)
{
	return guest_memory + ((uint32_t)segment << 4);
}

static uint8_t mcb_signature_at(uint16_t segment)
{
	return mcb_bytes(segment)[0];
}

static uint16_t mcb_owner_at(uint16_t segment)
{
	return get_le16(mcb_bytes(segment) + 1u);
}

static uint16_t mcb_size_at(uint16_t segment)
{
	return get_le16(mcb_bytes(segment) + 3u);
}

static void put_mcb(uint16_t segment, uint8_t signature, uint16_t owner,
		    uint16_t paragraphs)
{
	uint8_t *bytes = mcb_bytes(segment);
	size_t index;

	for (index = 0u; index < MCB_BYTES; ++index)
		bytes[index] = 0u;
	bytes[0] = signature;
	put_le16(bytes + 1u, owner);
	put_le16(bytes + 3u, paragraphs);
}

static void fill_record(uint16_t segment, uint8_t seed)
{
	uint8_t *bytes = mcb_bytes(segment);
	size_t index;

	for (index = 0u; index < MCB_BYTES; ++index)
		bytes[index] = (uint8_t)(seed + (uint8_t)index);
}

static bool
mcb_name_matches(uint16_t segment,
		 const uint8_t expected[DOS_MEMORY_OWNER_NAME_BYTES])
{
	const uint8_t *bytes = mcb_bytes(segment);
	size_t index;

	for (index = 0u; index < DOS_MEMORY_OWNER_NAME_BYTES; ++index) {
		if (bytes[MCB_NAME_OFFSET + index] != expected[index])
			return false;
	}
	return true;
}

static void copy_record(uint16_t segment, uint8_t copy[MCB_BYTES])
{
	const uint8_t *bytes = mcb_bytes(segment);
	size_t index;

	for (index = 0u; index < MCB_BYTES; ++index)
		copy[index] = bytes[index];
}

static bool record_equals(uint16_t segment, const uint8_t copy[MCB_BYTES])
{
	const uint8_t *bytes = mcb_bytes(segment);
	size_t index;

	for (index = 0u; index < MCB_BYTES; ++index) {
		if (bytes[index] != copy[index])
			return false;
	}
	return true;
}

static bool rebind_values_equal(
    const struct dos_memory_owner_rebind_value *left,
    const struct dos_memory_owner_rebind_value *right)
{
	size_t index;

	if (left->header_segment != right->header_segment ||
	    left->expected_owner != right->expected_owner ||
	    left->new_owner != right->new_owner)
		return false;
	for (index = 0u; index < ARRAY_SIZE(left->reserved); ++index) {
		if (left->reserved[index] != right->reserved[index])
			return false;
	}
	for (index = 0u; index < ARRAY_SIZE(left->replacement_bytes); ++index) {
		if (left->replacement_bytes[index] !=
		    right->replacement_bytes[index])
			return false;
	}
	return true;
}

static bool tail_has_pattern(uint16_t segment, uint8_t seed)
{
	const uint8_t *bytes = mcb_bytes(segment);
	size_t index;

	for (index = 5u; index < MCB_BYTES; ++index) {
		if (bytes[index] != (uint8_t)(seed + (uint8_t)index))
			return false;
	}
	return true;
}

static enum dos_machine_status
test_read_memory(kernel_object_handle_t context, dos_linear_address_t address,
		 void *destination, size_t destination_capacity, size_t count)
{
	uint8_t *output = (uint8_t *)destination;
	size_t index;

	if (context != TEST_CONTEXT || count > destination_capacity ||
	    (uint64_t)address > TEST_MEMORY_BYTES ||
	    (uint64_t)count > TEST_MEMORY_BYTES - (uint64_t)address)
		return DOS_MACHINE_ADDRESS_FAULT;
	++read_call_count;
	if (fail_read_enabled && address == fail_read_address &&
	    fail_read_count != 0u) {
		--fail_read_count;
		return DOS_MACHINE_ADDRESS_FAULT;
	}
	for (index = 0u; index < count; ++index)
		output[index] = guest_memory[address + index];
	return DOS_MACHINE_OK;
}

static enum dos_machine_status
test_write_memory(kernel_object_handle_t context, dos_linear_address_t address,
		  const void *source, size_t source_capacity, size_t count)
{
	const uint8_t *input = (const uint8_t *)source;
	size_t index;
	bool injected_failure = false;
	size_t partial_count;

	if (context != TEST_CONTEXT || count > source_capacity ||
	    (uint64_t)address > TEST_MEMORY_BYTES ||
	    (uint64_t)count > TEST_MEMORY_BYTES - (uint64_t)address)
		return DOS_MACHINE_ADDRESS_FAULT;
	++write_call_count;
	if (write_call_count <= 64u &&
	    (fail_write_call_mask &
	     ((uint64_t)1u << (write_call_count - 1u))) != 0u)
		injected_failure = true;
	if (fail_write_enabled && address == fail_write_address &&
	    fail_write_count != 0u) {
		--fail_write_count;
		injected_failure = true;
	}
	if (injected_failure) {
		partial_count = fail_write_partial_bytes;
		if (partial_count > count)
			partial_count = count;
		for (index = 0u; index < partial_count; ++index)
			guest_memory[address + index] = input[index];
		return DOS_MACHINE_ADDRESS_FAULT;
	}
	for (index = 0u; index < count; ++index)
		guest_memory[address + index] = input[index];
	return DOS_MACHINE_OK;
}

static void clear_failures(void)
{
	fail_read_enabled = false;
	fail_write_enabled = false;
	fail_read_address = 0u;
	fail_write_address = 0u;
	fail_read_count = 0u;
	fail_write_count = 0u;
	fail_write_partial_bytes = 0u;
	read_call_count = 0u;
	write_call_count = 0u;
	fail_write_call_mask = 0u;
}

static bool reset_machine(struct dos_machine *machine)
{
	static const struct dos_machine_ops ops = {
	    .read_memory = test_read_memory,
	    .write_memory = test_write_memory,
	    .read_port = NULL,
	    .write_port = NULL,
	    .set_a20 = NULL,
	};
	size_t index;

	for (index = 0u; index < TEST_MEMORY_BYTES; ++index)
		guest_memory[index] = 0u;
	clear_failures();
	return dos_machine_configure(machine, &ops, TEST_CONTEXT,
				     TEST_MEMORY_BYTES, true) == DOS_MACHINE_OK;
}

static void build_strategy_chain(struct dos_memory_arena *arena)
{
	*arena = (struct dos_memory_arena)DOS_MEMORY_ARENA_INITIALIZER(
	    TEST_ARENA_IDENTITY(0x2000u));
	put_mcb(0x2000u, MCB_NORMAL, 0u, 10u);
	put_mcb(0x200bu, MCB_NORMAL, 0x1100u, 3u);
	put_mcb(0x200fu, MCB_NORMAL, 0u, 5u);
	put_mcb(0x2015u, MCB_NORMAL, 0x2200u, 2u);
	put_mcb(0x2018u, MCB_END, 0u, 8u);
	arena->head_segment = 0x2000u;
	arena->strategy = DOS_ALLOC_FIRST_FIT;
	arena->initialized = 1u;
	arena->machine_poisoned = 0u;
	arena->constructed = 1u;
	arena->generation = 1u;
	arena->identity = TEST_ARENA_IDENTITY(0x2000u);
}

static int test_allocation_strategies(struct dos_machine *machine)
{
	struct dos_memory_arena arena;
	uint16_t block;
	uint16_t maximum;
	uint16_t queried_maximum = 0xffffu;
	uint8_t strategy;

	if (!reset_machine(machine))
		return 1;
	build_strategy_chain(&arena);
	if (dos_memory_query_maximum_checked(
		&arena, machine, &queried_maximum) != DOS_MEMORY_OK ||
	    queried_maximum != 10u)
		return 2;
	fill_record(0x2005u, 0x40u);
	if (dos_memory_allocate(&arena, machine, 0x9001u, 4u, &block,
				&maximum) != DOS_SUCCESS ||
	    block != 0x2001u || maximum != 10u ||
	    mcb_owner_at(0x2000u) != 0x9001u || mcb_size_at(0x2000u) != 4u ||
	    mcb_size_at(0x2005u) != 5u || !tail_has_pattern(0x2005u, 0x40u))
		return 3;

	if (!reset_machine(machine))
		return 4;
	build_strategy_chain(&arena);
	fill_record(0x2014u, 0x50u);
	if (dos_memory_set_strategy(&arena, DOS_ALLOC_BEST_FIT) !=
		DOS_SUCCESS ||
	    dos_memory_get_strategy(&arena, &strategy) != DOS_SUCCESS ||
	    strategy != DOS_ALLOC_BEST_FIT)
		return 5;
	if (dos_memory_allocate(&arena, machine, 0x9002u, 4u, &block,
				&maximum) != DOS_SUCCESS ||
	    block != 0x2010u || maximum != 10u ||
	    mcb_owner_at(0x200fu) != 0x9002u || mcb_size_at(0x200fu) != 4u ||
	    mcb_size_at(0x2014u) != 0u || !tail_has_pattern(0x2014u, 0x50u))
		return 6;

	if (!reset_machine(machine))
		return 7;
	build_strategy_chain(&arena);
	fill_record(0x201cu, 0x60u);
	/* Every allocation strategy value greater than one selects last-fit. */
	if (dos_memory_set_strategy(&arena, 7u) != DOS_SUCCESS ||
	    dos_memory_get_strategy(&arena, &strategy) != DOS_SUCCESS ||
	    strategy != 7u)
		return 8;
	if (dos_memory_allocate(&arena, machine, 0x9003u, 4u, &block,
				&maximum) != DOS_SUCCESS ||
	    block != 0x201du || maximum != 10u ||
	    mcb_signature_at(0x2018u) != MCB_NORMAL ||
	    mcb_size_at(0x2018u) != 3u || mcb_owner_at(0x2018u) != 0u ||
	    mcb_signature_at(0x201cu) != MCB_END ||
	    mcb_owner_at(0x201cu) != 0x9003u || mcb_size_at(0x201cu) != 4u ||
	    !tail_has_pattern(0x201cu, 0x60u))
		return 9;
	return 0;
}

static int test_split_coalesce_and_free(struct dos_machine *machine)
{
	struct dos_memory_arena arena =
	    DOS_MEMORY_ARENA_INITIALIZER(TEST_ARENA_IDENTITY(0x3000u));
	uint16_t first;
	uint16_t second;
	uint16_t whole;
	uint16_t maximum;
	uint8_t fake_before;

	if (!reset_machine(machine))
		return 1;
	if (dos_memory_arena_initialize(&arena, machine, 0x3000u, 0x3100u) !=
	    DOS_SUCCESS)
		return 2;
	fill_record(0x300bu, 0x70u);
	if (dos_memory_allocate(&arena, machine, 0x1111u, 10u, &first,
				&maximum) != DOS_SUCCESS ||
	    first != 0x3001u || maximum != 255u ||
	    mcb_signature_at(0x3000u) != MCB_NORMAL ||
	    mcb_size_at(0x3000u) != 10u ||
	    mcb_signature_at(0x300bu) != MCB_END ||
	    mcb_size_at(0x300bu) != 244u || !tail_has_pattern(0x300bu, 0x70u))
		return 3;
	if (dos_memory_allocate(&arena, machine, 0x2222u, 20u, &second,
				&maximum) != DOS_SUCCESS ||
	    second != 0x300cu)
		return 4;
	/* The allocator stores CurrentPDB verbatim. With a zero CurrentPDB the
	 * call succeeds even though the resulting MCB still
	 * looks free to later arena scans. */
	if (dos_memory_allocate(&arena, machine, 0u, 1u, &whole, &maximum) !=
		DOS_SUCCESS ||
	    whole != 0x3021u || maximum != 223u ||
	    mcb_owner_at(0x3020u) != 0u || mcb_size_at(0x3020u) != 1u)
		return 5;
	if (dos_memory_free(&arena, machine, first) != DOS_SUCCESS ||
	    dos_memory_free(&arena, machine, second) != DOS_SUCCESS)
		return 6;
	if (dos_memory_allocate(&arena, machine, 0x3333u, 255u, &whole,
				&maximum) != DOS_SUCCESS ||
	    whole != 0x3001u || maximum != 255u ||
	    mcb_signature_at(0x3000u) != MCB_END ||
	    mcb_owner_at(0x3000u) != 0x3333u || mcb_size_at(0x3000u) != 255u)
		return 7;

	/* $DEALLOC checks ES-1 directly.  Kernel/EXEC owned release keeps the
	 * stricter canonical-chain and owner check at its separate boundary. */
	put_mcb(0x3200u, MCB_NORMAL, 0x7777u, 1u);
	fake_before = (uint8_t)mcb_owner_at(0x3200u);
	if (dos_memory_free_owned_checked(&arena, machine, 0x3201u, 0x7777u) !=
		DOS_MEMORY_INVALID_BLOCK ||
	    (uint8_t)mcb_owner_at(0x3200u) != fake_before ||
	    dos_memory_free(&arena, machine, 0x3201u) != DOS_SUCCESS ||
	    mcb_owner_at(0x3200u) != 0u)
		return 8;

	/* Segment zero is a value, not the allocation scan's not-found
	 * sentinel. */
	if (!reset_machine(machine))
		return 9;
	arena = (struct dos_memory_arena)DOS_MEMORY_ARENA_INITIALIZER(
	    TEST_ARENA_IDENTITY(0u));
	if (dos_memory_arena_initialize(&arena, machine, 0u, 0x20u) !=
		DOS_SUCCESS ||
	    dos_memory_allocate(&arena, machine, 0x4444u, 1u, &first,
				&maximum) != DOS_SUCCESS ||
	    first != 1u || maximum != 31u)
		return 10;
	return 0;
}

static int test_resize(struct dos_machine *machine)
{
	struct dos_memory_arena arena =
	    DOS_MEMORY_ARENA_INITIALIZER(TEST_ARENA_IDENTITY(0x4000u));
	uint16_t first;
	uint16_t second;
	uint16_t maximum;

	if (!reset_machine(machine))
		return 1;
	if (dos_memory_arena_initialize(&arena, machine, 0x4000u, 0x4100u) !=
		DOS_SUCCESS ||
	    dos_memory_allocate(&arena, machine, 0x1111u, 10u, &first,
				&maximum) != DOS_SUCCESS ||
	    dos_memory_allocate(&arena, machine, 0x2222u, 20u, &second,
				&maximum) != DOS_SUCCESS)
		return 2;
	if (dos_memory_resize(&arena, machine, first, 0x1111u, 5u, &maximum) !=
		DOS_SUCCESS ||
	    maximum != 10u || mcb_size_at(0x4000u) != 5u ||
	    mcb_owner_at(0x4000u) != 0x1111u || mcb_size_at(0x4006u) != 4u)
		return 3;
	if (dos_memory_resize(&arena, machine, first, 0x1111u, 8u, &maximum) !=
		DOS_SUCCESS ||
	    maximum != 10u || mcb_size_at(0x4000u) != 8u ||
	    mcb_size_at(0x4009u) != 1u)
		return 4;
	/* Failure returns the coalesced maximum and leaves that coalescing
	 * visible. */
	if (dos_memory_resize(&arena, machine, first, 0x1111u, 11u, &maximum) !=
		DOS_ERROR_NOT_ENOUGH_MEMORY ||
	    maximum != 10u || mcb_size_at(0x4000u) != 10u ||
	    mcb_owner_at(0x400bu) != 0x2222u)
		return 5;
	if (dos_memory_free(&arena, machine, second) != DOS_SUCCESS ||
	    dos_memory_resize(&arena, machine, first, 0x3333u, 30u, &maximum) !=
		DOS_SUCCESS ||
	    maximum != 255u || mcb_size_at(0x4000u) != 30u ||
	    mcb_owner_at(0x4000u) != 0x3333u ||
	    mcb_signature_at(0x401fu) != MCB_END ||
	    mcb_size_at(0x401fu) != 224u)
		return 6;
	/* $SETBLOCK also checks ES-1 directly and falls through
	 * alloc_set_owner, even for a valid MCB outside the canonical chain. */
	put_mcb(0x4300u, MCB_END, 0x9999u, 1u);
	if (dos_memory_resize(&arena, machine, 0x4301u, 0x1111u, 1u,
			      &maximum) != DOS_SUCCESS ||
	    maximum != 1u || mcb_owner_at(0x4300u) != 0x1111u)
		return 7;
	put_mcb(0x4400u, 0x51u, 0x9999u, 1u);
	if (dos_memory_resize(&arena, machine, 0x4401u, 0x1111u, 1u,
			      &maximum) != DOS_ERROR_ARENA_TRASHED)
		return 8;
	return 0;
}

static int test_free_process(struct dos_machine *machine)
{
	struct dos_memory_arena arena =
	    DOS_MEMORY_ARENA_INITIALIZER(TEST_ARENA_IDENTITY(0x5000u));
	uint16_t block;
	uint16_t maximum;

	if (!reset_machine(machine))
		return 1;
	if (dos_memory_arena_initialize(&arena, machine, 0x5000u, 0x5100u) !=
		DOS_SUCCESS ||
	    dos_memory_allocate(&arena, machine, 0x1111u, 5u, &block,
				&maximum) != DOS_SUCCESS ||
	    dos_memory_allocate(&arena, machine, 0x2222u, 6u, &block,
				&maximum) != DOS_SUCCESS ||
	    dos_memory_allocate(&arena, machine, 0x1111u, 7u, &block,
				&maximum) != DOS_SUCCESS)
		return 2;
	if (dos_memory_free_process(&arena, machine, 0x1111u) != DOS_SUCCESS ||
	    mcb_owner_at(0x5000u) != 0u || mcb_owner_at(0x5006u) != 0x2222u ||
	    mcb_owner_at(0x500du) != 0u ||
	    dos_memory_arena_validate(&arena, machine) != DOS_SUCCESS)
		return 3;

	/* Once a multi-MCB release has committed, a later backend fault leaves
	 * an externally observable partial update. Quarantine the arena instead
	 * of reporting a retryable machine fault. */
	if (!reset_machine(machine) ||
	    dos_memory_arena_initialize(&arena, machine, 0x5000u, 0x5100u) !=
		DOS_SUCCESS ||
	    dos_memory_allocate(&arena, machine, 0x1111u, 5u, &block,
				&maximum) != DOS_SUCCESS ||
	    dos_memory_allocate(&arena, machine, 0x2222u, 6u, &block,
				&maximum) != DOS_SUCCESS ||
	    dos_memory_allocate(&arena, machine, 0x1111u, 7u, &block,
				&maximum) != DOS_SUCCESS)
		return 4;
	fail_read_enabled = true;
	fail_read_address = 0x500d0u;
	fail_read_count = 1u;
	if (dos_memory_free_process_checked(&arena, machine, 0x1111u) !=
		DOS_MEMORY_MACHINE_POISONED ||
	    !arena.machine_poisoned || mcb_owner_at(0x5000u) != 0u ||
	    mcb_owner_at(0x5006u) != 0x2222u ||
	    mcb_owner_at(0x500du) != 0x1111u)
		return 5;
	clear_failures();
	if (dos_memory_arena_validate_checked(&arena, machine) !=
	    DOS_MEMORY_MACHINE_POISONED)
		return 6;
	return 0;
}

static int test_damage_and_boundaries(struct dos_machine *machine)
{
	struct dos_memory_arena arena =
	    DOS_MEMORY_ARENA_INITIALIZER(TEST_ARENA_IDENTITY(0x7000u));
	uint16_t block = 0xffffu;
	uint16_t maximum = 0xffffu;

	if (!reset_machine(machine))
		return 1;
	if (dos_memory_arena_initialize(&arena, machine, 0x7000u, 0x7010u) !=
	    DOS_SUCCESS)
		return 2;
	mcb_bytes(0x7000u)[0] = 0x51u;
	if (dos_memory_arena_validate(&arena, machine) !=
		DOS_ERROR_ARENA_TRASHED ||
	    dos_memory_allocate(&arena, machine, 0x1111u, 1u, &block,
				&maximum) != DOS_ERROR_ARENA_TRASHED ||
	    block != 0u || maximum != 0u ||
	    dos_memory_free(&arena, machine, 0x7001u) !=
		DOS_ERROR_INVALID_BLOCK)
		return 3;

	/* A terminal MCB cannot claim paragraphs beyond segment 0xffff. */
	put_mcb(0x1000u, MCB_END, 0u, 0xf000u);
	arena.head_segment = 0x1000u;
	arena.initialized = true;
	if (dos_memory_arena_validate(&arena, machine) !=
	    DOS_ERROR_ARENA_TRASHED)
		return 4;

	/* Reject the wraparound edge that could otherwise form a segment cycle.
	 */
	put_mcb(0xfffeu, MCB_NORMAL, 0x1111u, 1u);
	arena.head_segment = 0xfffeu;
	if (dos_memory_arena_validate(&arena, machine) !=
	    DOS_ERROR_ARENA_TRASHED)
		return 5;

	/* A simulated DOS segment zero is valid when 16-bit arithmetic wraps.
	 */
	if (!reset_machine(machine))
		return 6;
	put_mcb(0xfff0u, MCB_END, 0u, 15u);
	arena.head_segment = 0xfff0u;
	arena.strategy = DOS_ALLOC_LAST_FIT;
	arena.initialized = true;
	if (dos_memory_allocate(&arena, machine, 0xaaaau, 0u, &block,
				&maximum) != DOS_SUCCESS ||
	    block != 0u || maximum != 15u ||
	    mcb_signature_at(0xfff0u) != MCB_NORMAL ||
	    mcb_size_at(0xfff0u) != 14u ||
	    mcb_signature_at(0xffffu) != MCB_END ||
	    mcb_owner_at(0xffffu) != 0xaaaau ||
	    dos_memory_free(&arena, machine, 0u) != DOS_SUCCESS ||
	    mcb_owner_at(0xffffu) != 0u ||
	    dos_memory_arena_validate(&arena, machine) != DOS_SUCCESS)
		return 7;
	return 0;
}

static int test_machine_failures(struct dos_machine *machine)
{
	struct dos_memory_arena arena =
	    DOS_MEMORY_ARENA_INITIALIZER(TEST_ARENA_IDENTITY(0x6000u));
	uint8_t header_before[MCB_BYTES];
	uint8_t split_before[MCB_BYTES];
	uint16_t block;
	uint16_t maximum;

	if (!reset_machine(machine))
		return 1;
	fail_write_enabled = true;
	fail_write_address = 0x60000u;
	fail_write_count = 1u;
	if (dos_memory_arena_initialize(&arena, machine, 0x6000u, 0x6010u) !=
		DOS_ERROR_ARENA_TRASHED ||
	    arena.initialized)
		return 2;
	clear_failures();
	if (dos_memory_arena_initialize(&arena, machine, 0x6000u, 0x6010u) !=
	    DOS_SUCCESS)
		return 3;
	fail_read_enabled = true;
	fail_read_address = 0x60000u;
	fail_read_count = 1u;
	if (dos_memory_arena_validate(&arena, machine) !=
	    DOS_ERROR_ARENA_TRASHED)
		return 4;
	clear_failures();

	fill_record(0x6005u, 0x80u);
	copy_record(0x6000u, header_before);
	copy_record(0x6005u, split_before);
	fail_write_enabled = true;
	fail_write_address = 0x60000u;
	fail_write_count = 1u;
	if (dos_memory_allocate(&arena, machine, 0x1111u, 4u, &block,
				&maximum) != DOS_ERROR_ARENA_TRASHED ||
	    block != 0u || maximum != 15u ||
	    !record_equals(0x6000u, header_before) ||
	    !record_equals(0x6005u, split_before))
		return 5;
	clear_failures();
	fail_write_enabled = true;
	fail_write_address = 0x60050u;
	fail_write_count = 1u;
	if (dos_memory_allocate(&arena, machine, 0x1111u, 4u, &block,
				&maximum) != DOS_ERROR_ARENA_TRASHED ||
	    !record_equals(0x6000u, header_before) ||
	    !record_equals(0x6005u, split_before))
		return 6;
	clear_failures();
	if (dos_memory_allocate(&arena, machine, 0x1111u, 4u, &block,
				&maximum) != DOS_SUCCESS)
		return 7;
	fail_write_enabled = true;
	fail_write_address = 0x60000u;
	fail_write_count = 1u;
	if (dos_memory_free(&arena, machine, block) !=
		DOS_ERROR_ARENA_TRASHED ||
	    mcb_owner_at(0x6000u) != 0x1111u)
		return 8;
	clear_failures();

	/* A read fault later in a valid walk is an arena error, not OOM. */
	fail_read_enabled = true;
	fail_read_address = 0x60050u;
	fail_read_count = 1u;
	block = 0xffffu;
	maximum = 0xffffu;
	if (dos_memory_allocate(&arena, machine, 0x2222u, 20u, &block,
				&maximum) != DOS_ERROR_ARENA_TRASHED ||
	    block != 0u || maximum != 0u)
		return 9;
	return 0;
}

static int test_typed_transactions_and_poison(struct dos_machine *machine)
{
	struct dos_memory_arena arena;
	struct dos_memory_allocation_result result;
	uint8_t header_before[MCB_BYTES];
	uint8_t split_before[MCB_BYTES];
	uint16_t legacy_block;
	uint16_t legacy_maximum;
	uint8_t strategy;
	uint32_t reads_before;
	uint32_t writes_before;

	/* The explicit constructor establishes the host-side object lifetime.
	 */
	if (!reset_machine(machine))
		return 1;
	if (dos_memory_arena_construct(&arena, 0u) !=
		DOS_MEMORY_INVALID_ARGUMENT ||
	    dos_memory_arena_construct(&arena, KERNEL_OBJECT_HANDLE_INVALID) !=
		DOS_MEMORY_INVALID_ARGUMENT ||
	    dos_memory_arena_construct(&arena, TEST_ARENA_IDENTITY(0x8000u)) !=
		DOS_MEMORY_OK ||
	    dos_memory_arena_initialize_checked(&arena, machine, 0x8000u,
						0x8010u) != DOS_MEMORY_OK ||
	    !arena.initialized || arena.machine_poisoned ||
	    arena.generation != 1u)
		return 2;

	/* A partial MCB write with a successful repair is a machine fault. */
	fill_record(0x8005u, 0x90u);
	copy_record(0x8000u, header_before);
	copy_record(0x8005u, split_before);
	write_call_count = 0u;
	fail_write_enabled = true;
	fail_write_address = 0x80050u;
	fail_write_count = 1u;
	fail_write_partial_bytes = 7u;
	if (dos_memory_allocate_checked(&arena, machine, 0x1111u, 4u,
					&result) != DOS_MEMORY_MACHINE_FAULT ||
	    result.block_segment != 0u || result.maximum_available != 15u ||
	    arena.machine_poisoned || !record_equals(0x8000u, header_before) ||
	    !record_equals(0x8005u, split_before))
		return 3;
	clear_failures();
	if (dos_memory_arena_validate_checked(&arena, machine) != DOS_MEMORY_OK)
		return 4;

	/* A partial replacement whose rollback also fails is terminal poison.
	 */
	if (!reset_machine(machine) ||
	    dos_memory_arena_construct(&arena, TEST_ARENA_IDENTITY(0x8200u)) !=
		DOS_MEMORY_OK ||
	    dos_memory_arena_initialize_checked(&arena, machine, 0x8200u,
						0x8210u) != DOS_MEMORY_OK)
		return 5;
	write_call_count = 0u;
	fail_write_enabled = true;
	fail_write_address = 0x82000u;
	fail_write_count = 2u;
	fail_write_partial_bytes = 4u;
	if (dos_memory_allocate_checked(&arena, machine, 0x2222u, 15u,
					&result) !=
		DOS_MEMORY_MACHINE_POISONED ||
	    !arena.machine_poisoned)
		return 6;
	clear_failures();
	if (dos_memory_arena_validate_checked(&arena, machine) !=
		DOS_MEMORY_MACHINE_POISONED ||
	    dos_memory_get_strategy_checked(&arena, &strategy) !=
		DOS_MEMORY_MACHINE_POISONED)
		return 7;
	legacy_block = 0xffffu;
	legacy_maximum = 0xffffu;
	if (dos_memory_allocate(&arena, machine, 0x3333u, 1u, &legacy_block,
				&legacy_maximum) != DOS_ERROR_ARENA_TRASHED ||
	    legacy_block != 0u || legacy_maximum != 0u)
		return 8;

	/* If reverse split unwind cannot restore the hidden header, poison too.
	 */
	if (!reset_machine(machine) ||
	    dos_memory_arena_construct(&arena, TEST_ARENA_IDENTITY(0x8400u)) !=
		DOS_MEMORY_OK ||
	    dos_memory_arena_initialize_checked(&arena, machine, 0x8400u,
						0x8410u) != DOS_MEMORY_OK)
		return 9;
	fill_record(0x8405u, 0xa0u);
	copy_record(0x8400u, header_before);
	copy_record(0x8405u, split_before);
	write_call_count = 0u;
	fail_write_call_mask = ((uint64_t)1u << 1) | ((uint64_t)1u << 3);
	fail_write_partial_bytes = 5u;
	if (dos_memory_allocate_checked(&arena, machine, 0x4444u, 4u,
					&result) !=
		DOS_MEMORY_MACHINE_POISONED ||
	    !arena.machine_poisoned || !record_equals(0x8400u, header_before) ||
	    record_equals(0x8405u, split_before))
		return 10;
	clear_failures();
	if (dos_memory_free_checked(&arena, machine, 0x8401u) !=
	    DOS_MEMORY_MACHINE_POISONED)
		return 11;

	/* Generations never wrap.  Exhaustion is detected before touching the
	 * guest arena and is not itself poison. */
	if (!reset_machine(machine) ||
	    dos_memory_arena_construct(&arena, TEST_ARENA_IDENTITY(0x8600u)) !=
		DOS_MEMORY_OK)
		return 12;
	fill_record(0x8600u, 0xb0u);
	copy_record(0x8600u, header_before);
	arena.generation = DOS_MEMORY_GENERATION_MAX;
	if (dos_memory_arena_initialize_checked(&arena, machine, 0x8600u,
						0x8610u) !=
		DOS_MEMORY_GENERATION_EXHAUSTED ||
	    arena.generation != DOS_MEMORY_GENERATION_MAX ||
	    arena.initialized || arena.machine_poisoned ||
	    !record_equals(0x8600u, header_before))
		return 13;

	/* Strategy access obeys the same explicit construct/initialize
	 * lifecycle as the mutating paths. */
	if (dos_memory_arena_construct(&arena, TEST_ARENA_IDENTITY(0x8700u)) !=
		DOS_MEMORY_OK ||
	    dos_memory_get_strategy_checked(&arena, &strategy) !=
		DOS_MEMORY_INVALID_ARGUMENT ||
	    dos_memory_set_strategy_checked(&arena, DOS_ALLOC_LAST_FIT) !=
		DOS_MEMORY_INVALID_ARGUMENT)
		return 14;

	/* The outer EXEC coordinator can quarantine an otherwise healthy arena
	 * without reaching the guest backend. */
	if (!reset_machine(machine) ||
	    dos_memory_arena_construct(&arena, TEST_ARENA_IDENTITY(0x8800u)) !=
		DOS_MEMORY_OK ||
	    dos_memory_arena_initialize_checked(&arena, machine, 0x8800u,
						0x8810u) != DOS_MEMORY_OK)
		return 15;
	reads_before = read_call_count;
	writes_before = write_call_count;
	if (dos_memory_arena_poison(&arena) != DOS_MEMORY_OK ||
	    dos_memory_arena_poison(&arena) != DOS_MEMORY_OK ||
	    !arena.machine_poisoned || read_call_count != reads_before ||
	    write_call_count != writes_before ||
	    dos_memory_arena_validate_checked(&arena, machine) !=
		DOS_MEMORY_MACHINE_POISONED)
		return 16;

	/* Fixed-width lifetime flags are exact bytes, not C truth values.
	 * Corrupt 2 encodings and reserved bytes fail before guest I/O, while
	 * the caller's strategy output remains untouched. */
	if (!reset_machine(machine) ||
	    dos_memory_arena_construct(&arena, TEST_ARENA_IDENTITY(0x8a00u)) !=
		DOS_MEMORY_OK ||
	    dos_memory_arena_initialize_checked(&arena, machine, 0x8a00u,
						0x8a10u) != DOS_MEMORY_OK)
		return 17;
	reads_before = read_call_count;
	writes_before = write_call_count;
	strategy = 0xa5u;
	arena.initialized = 2u;
	if (dos_memory_arena_validate_checked(&arena, machine) !=
		DOS_MEMORY_INVALID_ARGUMENT ||
	    dos_memory_get_strategy_checked(&arena, &strategy) !=
		DOS_MEMORY_INVALID_ARGUMENT ||
	    dos_memory_arena_initialize_checked(&arena, machine, 0x8a00u,
						0x8a10u) !=
		DOS_MEMORY_INVALID_ARGUMENT ||
	    strategy != 0xa5u || read_call_count != reads_before ||
	    write_call_count != writes_before)
		return 18;
	arena.initialized = 1u;
	arena.machine_poisoned = 2u;
	if (dos_memory_get_strategy_checked(&arena, &strategy) !=
		DOS_MEMORY_INVALID_ARGUMENT ||
	    dos_memory_arena_poison(&arena) != DOS_MEMORY_INVALID_ARGUMENT)
		return 19;
	arena.machine_poisoned = 0u;
	arena.constructed = 2u;
	if (dos_memory_set_strategy_checked(&arena, 7u) !=
		DOS_MEMORY_INVALID_ARGUMENT)
		return 20;
	arena.constructed = 1u;
	arena.reserved[1] = 1u;
	if (dos_memory_arena_validate_checked(&arena, machine) !=
		DOS_MEMORY_INVALID_ARGUMENT ||
	    read_call_count != reads_before || write_call_count != writes_before)
		return 21;
	return 0;
}

static int test_owner_rebind_preparation(struct dos_machine *machine)
{
	static const struct dos_memory_owner_identity initial_owner = {
	    .psp_segment = 0x1111u,
	    .name = {'P', 'A', 'R', 'E', 'N', 'T', ' ', ' '},
	};
	static const struct dos_memory_owner_name_patch patch = {
	    .bytes = {'C', 0u, 0u, 0u, 0u, 0u, 0u, 0u},
	    .count = 2u,
	    .reserved = {0u},
	};
	struct dos_memory_owner_rebind_value value;
	struct dos_memory_owner_rebind_value unchanged;
	struct dos_memory_arena arena;
	struct dos_memory_allocation_result result;
	uint8_t record_before[MCB_BYTES];
	uint32_t writes_before;
	size_t index;

	if (!reset_machine(machine) ||
	    dos_memory_arena_construct(&arena, TEST_ARENA_IDENTITY(0x8700u)) !=
		DOS_MEMORY_OK ||
	    dos_memory_arena_initialize_checked(&arena, machine, 0x8700u,
						0x8710u) != DOS_MEMORY_OK ||
	    dos_memory_allocate_named_checked(&arena, machine, &initial_owner,
					      4u, &result) != DOS_MEMORY_OK)
		return 1;
	copy_record(0x8700u, record_before);
	writes_before = write_call_count;
	if (dos_memory_prepare_owner_rebind_checked(
		&arena, machine, result.block_segment,
		initial_owner.psp_segment, 0x2222u, &value) != DOS_MEMORY_OK ||
	    write_call_count != writes_before ||
	    !record_equals(0x8700u, record_before) ||
	    !dos_memory_owner_rebind_value_has_valid_encoding(&value) ||
	    value.header_segment != 0x8700u ||
	    value.expected_owner != initial_owner.psp_segment ||
	    value.new_owner != 0x2222u ||
	    get_le16(value.replacement_bytes + 1u) != 0x2222u)
		return 2;
	for (index = 0u; index < DOS_MEMORY_OWNER_NAME_BYTES; ++index) {
		if (value.replacement_bytes[MCB_NAME_OFFSET + index] !=
		    initial_owner.name[index])
			return 3;
	}

	if (dos_memory_prepare_owner_name_patch_rebind_checked(
		&arena, machine, result.block_segment,
		initial_owner.psp_segment, 0x3333u, &patch,
		&value) != DOS_MEMORY_OK ||
	    write_call_count != writes_before ||
	    value.replacement_bytes[MCB_NAME_OFFSET] != (uint8_t)'C' ||
	    value.replacement_bytes[MCB_NAME_OFFSET + 1u] != 0u ||
	    value.replacement_bytes[MCB_NAME_OFFSET + 2u] != (uint8_t)'R' ||
	    !record_equals(0x8700u, record_before))
		return 4;

	unchanged = value;
	if (dos_memory_prepare_owner_rebind_checked(
		&arena, machine, result.block_segment, 0x9999u, 0x4444u,
		&value) != DOS_MEMORY_OWNER_MISMATCH ||
	    !rebind_values_equal(&value, &unchanged) ||
	    write_call_count != writes_before)
		return 5;
	value.replacement_bytes[1] = 0u;
	value.replacement_bytes[2] = 0u;
	if (dos_memory_owner_rebind_value_has_valid_encoding(&value))
		return 6;
	value = unchanged;
	value.reserved[0] = 1u;
	if (dos_memory_owner_rebind_value_has_valid_encoding(&value))
		return 7;
	return 0;
}

static int test_strict_owner_transfer(struct dos_machine *machine)
{
	static const struct dos_memory_owner_identity initial_owner = {
	    .psp_segment = 0x1111u,
	    .name = {'P', 'A', 'R', 'E', 'N', 'T', ' ', ' '},
	};
	static const struct dos_memory_owner_identity named_owner = {
	    .psp_segment = 0x3333u,
	    .name = {'C', 'H', 'I', 'L', 'D', ' ', ' ', ' '},
	};
	static const struct dos_memory_owner_identity zero_owner = {
	    .psp_segment = 0u,
	    .name = {'Z', 'E', 'R', 'O', ' ', ' ', ' ', ' '},
	};
	static const struct dos_memory_owner_name_patch short_patch = {
	    .bytes = {'X', 0u, 0u, 0u, 0u, 0u, 0u, 0u},
	    .count = 1u,
	    .reserved = {0u},
	};
	static const uint8_t short_name[DOS_MEMORY_OWNER_NAME_BYTES] = {
	    'X', 'A', 'R', 'E', 'N', 'T', ' ', ' '};
	static const struct dos_memory_owner_name_patch full_patch = {
	    .bytes = {'F', 'U', 'L', 'L', 'N', 'A', 'M', 'E'},
	    .count = DOS_MEMORY_OWNER_NAME_BYTES,
	    .reserved = {0u},
	};
	static const struct dos_memory_owner_name_patch zero_count_patch = {
	    .bytes = {0u},
	    .count = 0u,
	    .reserved = {0u},
	};
	static const struct dos_memory_owner_name_patch large_count_patch = {
	    .bytes = {0u},
	    .count = DOS_MEMORY_OWNER_NAME_BYTES + 1u,
	    .reserved = {0u},
	};
	static const struct dos_memory_owner_name_patch reserved_patch = {
	    .bytes = {'R', 0u, 0u, 0u, 0u, 0u, 0u, 0u},
	    .count = 1u,
	    .reserved = {1u, 0u, 0u, 0u, 0u, 0u, 0u},
	};
	struct dos_memory_arena arena;
	struct dos_memory_allocation_result result;
	uint8_t record_before[MCB_BYTES];
	uint32_t reads_before;
	uint32_t writes_before;
	dos_linear_address_t header_address;

	if (!reset_machine(machine) ||
	    dos_memory_arena_construct(&arena, TEST_ARENA_IDENTITY(0x8800u)) !=
		DOS_MEMORY_OK ||
	    dos_memory_arena_initialize_checked(&arena, machine, 0x8800u,
						0x8810u) != DOS_MEMORY_OK ||
	    dos_memory_allocate_named_checked(&arena, machine, &initial_owner,
					      4u, &result) != DOS_MEMORY_OK ||
	    result.block_segment != 0x8801u ||
	    mcb_owner_at(0x8800u) != initial_owner.psp_segment ||
	    !mcb_name_matches(0x8800u, initial_owner.name))
		return 1;

	/* Owner-only transfer preserves all eight fixed-width MCB name bytes. */
	if (dos_memory_transfer_owner_checked(
		&arena, machine, result.block_segment,
		initial_owner.psp_segment, 0x2222u) != DOS_MEMORY_OK ||
	    mcb_owner_at(0x8800u) != 0x2222u ||
	    !mcb_name_matches(0x8800u, initial_owner.name))
		return 2;

	copy_record(0x8800u, record_before);
	writes_before = write_call_count;
	if (dos_memory_transfer_owner_checked(
		&arena, machine, result.block_segment,
		initial_owner.psp_segment,
		0x4444u) != DOS_MEMORY_OWNER_MISMATCH ||
	    write_call_count != writes_before ||
	    !record_equals(0x8800u, record_before))
		return 3;

	copy_record(0x8800u, record_before);
	reads_before = read_call_count;
	writes_before = write_call_count;
	if (dos_memory_transfer_owner_name_patch_checked(
		&arena, machine, result.block_segment, 0u, 0x2a2au,
		&short_patch) != DOS_MEMORY_INVALID_ARGUMENT ||
	    dos_memory_transfer_owner_name_patch_checked(
		&arena, machine, result.block_segment, 0x2222u, 0u,
		&short_patch) != DOS_MEMORY_INVALID_ARGUMENT ||
	    dos_memory_transfer_owner_name_patch_checked(
		&arena, machine, result.block_segment, 0x2222u, 0x2a2au,
		&zero_count_patch) != DOS_MEMORY_INVALID_ARGUMENT ||
	    dos_memory_transfer_owner_name_patch_checked(
		&arena, machine, result.block_segment, 0x2222u, 0x2a2au,
		&large_count_patch) != DOS_MEMORY_INVALID_ARGUMENT ||
	    dos_memory_transfer_owner_name_patch_checked(
		&arena, machine, result.block_segment, 0x2222u, 0x2a2au,
		&reserved_patch) != DOS_MEMORY_INVALID_ARGUMENT ||
	    dos_memory_transfer_owner_name_patch_checked(
		&arena, machine, result.block_segment, 0x2222u, 0x2a2au,
		NULL) != DOS_MEMORY_INVALID_ARGUMENT ||
	    read_call_count != reads_before ||
	    write_call_count != writes_before ||
	    !record_equals(0x8800u, record_before))
		return 4;

	/* A valid patch still requires the canonical MCB's exact old owner. */
	writes_before = write_call_count;
	if (dos_memory_transfer_owner_name_patch_checked(
		&arena, machine, result.block_segment, 0x1111u, 0x2a2au,
		&short_patch) != DOS_MEMORY_OWNER_MISMATCH ||
	    write_call_count != writes_before ||
	    !record_equals(0x8800u, record_before))
		return 5;

	/* Scan_Execname-style short updates preserve the unvisited tail bytes.
	 */
	if (dos_memory_transfer_owner_name_patch_checked(
		&arena, machine, result.block_segment, 0x2222u, 0x2a2au,
		&short_patch) != DOS_MEMORY_OK ||
	    mcb_owner_at(0x8800u) != 0x2a2au ||
	    !mcb_name_matches(0x8800u, short_name))
		return 6;
	if (dos_memory_transfer_owner_name_patch_checked(
		&arena, machine, result.block_segment, 0x2a2au, 0x2222u,
		&full_patch) != DOS_MEMORY_OK ||
	    mcb_owner_at(0x8800u) != 0x2222u ||
	    !mcb_name_matches(0x8800u, full_patch.bytes))
		return 7;

	if (dos_memory_transfer_named_owner_checked(
		&arena, machine, result.block_segment, 0x2222u, &named_owner) !=
		DOS_MEMORY_OK ||
	    mcb_owner_at(0x8800u) != named_owner.psp_segment ||
	    !mcb_name_matches(0x8800u, named_owner.name))
		return 8;

	copy_record(0x8800u, record_before);
	writes_before = write_call_count;
	if (dos_memory_transfer_owner_checked(
		&arena, machine, result.block_segment, 0u, 0x4444u) !=
		DOS_MEMORY_INVALID_ARGUMENT ||
	    dos_memory_transfer_owner_checked(
		&arena, machine, result.block_segment, named_owner.psp_segment,
		0u) != DOS_MEMORY_INVALID_ARGUMENT ||
	    dos_memory_transfer_named_owner_checked(
		&arena, machine, result.block_segment, named_owner.psp_segment,
		&zero_owner) != DOS_MEMORY_INVALID_ARGUMENT ||
	    dos_memory_transfer_named_owner_checked(
		&arena, machine, result.block_segment, named_owner.psp_segment,
		NULL) != DOS_MEMORY_INVALID_ARGUMENT ||
	    write_call_count != writes_before ||
	    !record_equals(0x8800u, record_before))
		return 9;

	/* A plausible MCB outside the canonical chain is never transferred. */
	put_mcb(0x8900u, MCB_END, named_owner.psp_segment, 1u);
	writes_before = write_call_count;
	if (dos_memory_transfer_owner_checked(
		&arena, machine, 0x8901u, named_owner.psp_segment, 0x4444u) !=
		DOS_MEMORY_INVALID_BLOCK ||
	    write_call_count != writes_before ||
	    mcb_owner_at(0x8900u) != named_owner.psp_segment)
		return 10;

	/* A failed write with successful repair leaves the canonical MCB exact.
	 */
	copy_record(0x8800u, record_before);
	clear_failures();
	header_address =
	    (dos_linear_address_t)((uint32_t)(result.block_segment - 1u) << 4);
	fail_write_enabled = true;
	fail_write_address = header_address;
	fail_write_count = 1u;
	fail_write_partial_bytes = 7u;
	if (dos_memory_transfer_owner_name_patch_checked(
		&arena, machine, result.block_segment, named_owner.psp_segment,
		0x4444u, &short_patch) != DOS_MEMORY_MACHINE_FAULT ||
	    arena.machine_poisoned || !record_equals(0x8800u, record_before))
		return 11;

	/* An uncertain repair is sticky poison, never a retryable owner change.
	 */
	clear_failures();
	fail_write_enabled = true;
	fail_write_address = header_address;
	fail_write_count = 2u;
	fail_write_partial_bytes = 4u;
	if (dos_memory_transfer_owner_name_patch_checked(
		&arena, machine, result.block_segment, named_owner.psp_segment,
		0x4444u, &short_patch) != DOS_MEMORY_MACHINE_POISONED ||
	    !arena.machine_poisoned ||
	    dos_memory_transfer_owner_name_patch_checked(
		&arena, machine, result.block_segment, named_owner.psp_segment,
		0x4444u, &short_patch) != DOS_MEMORY_MACHINE_POISONED)
		return 12;
	clear_failures();
	return 0;
}

static int run_tests(void)
{
	struct dos_machine machine;
	int status;

	status = test_allocation_strategies(&machine);
	if (status != 0)
		return 10 + status;
	status = test_split_coalesce_and_free(&machine);
	if (status != 0)
		return 30 + status;
	status = test_resize(&machine);
	if (status != 0)
		return 50 + status;
	status = test_free_process(&machine);
	if (status != 0)
		return 70 + status;
	status = test_damage_and_boundaries(&machine);
	if (status != 0)
		return 90 + status;
	status = test_machine_failures(&machine);
	if (status != 0)
		return 110 + status;
	status = test_typed_transactions_and_poison(&machine);
	if (status != 0)
		return 130 + status;
	status = test_owner_rebind_preparation(&machine);
	if (status != 0)
		return 150 + status;
	status = test_strict_owner_transfer(&machine);
	if (status != 0)
		return 170 + status;
	return 0;
}

DOSC32_TEST_ENTRY(run_tests)

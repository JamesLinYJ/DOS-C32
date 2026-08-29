// SPDX-License-Identifier: GPL-2.0-only
/* Freestanding regression tests for JFT resizing. */
#include "dos_abi.h"
#include "dos_jft.h"
#include "test_entry.h"

#define TEST_MEMORY_BYTES 0x00110000u
#define TEST_MACHINE_CONTEXT 0x4a465430u
#define TEST_ARENA_IDENTITY 0x4a46540000000001ull
#define TEST_ARENA_HEAD 0x1000u
#define TEST_ARENA_END 0x8000u
#define TEST_SMALL_ARENA_END 0x1100u
#define TEST_PSP 0x1001u
#define TEST_PSP_PARAGRAPHS 0x10u
#define PSP_JFT_OFFSET 0x18u
#define PSP_JFT_LENGTH_OFFSET 0x32u
#define PSP_JFT_POINTER_OFFSET 0x34u
#define TEST_FAILURE_RULES 4u

struct write_failure_rule {
	dos_linear_address_t address;
	uint32_t skip;
	uint32_t remaining;
	uint32_t partial;
};

struct test_fixture {
	struct dos_machine machine;
	struct dos_memory_arena arena;
};

static uint8_t guest_memory[TEST_MEMORY_BYTES];
static struct write_failure_rule failure_rules[TEST_FAILURE_RULES];

static uint16_t read_le16(const uint8_t *bytes)
{
	return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8u);
}

static void write_le16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8u);
}

static dos_linear_address_t far_linear(uint16_t segment, uint16_t offset)
{
	return ((dos_linear_address_t)segment << 4u) + offset;
}

static uint8_t *far_bytes(uint16_t segment, uint16_t offset)
{
	return guest_memory + far_linear(segment, offset);
}

static uint16_t mcb_owner(uint16_t block_segment)
{
	return read_le16(far_bytes((uint16_t)(block_segment - 1u), 1u));
}

static uint16_t psp_jft_length(void)
{
	return read_le16(far_bytes(TEST_PSP, PSP_JFT_LENGTH_OFFSET));
}

static struct dos_far_pointer16 psp_jft_pointer(void)
{
	struct dos_far_pointer16 pointer = {
		.offset = read_le16(far_bytes(TEST_PSP,
					    PSP_JFT_POINTER_OFFSET)),
		.segment = read_le16(far_bytes(
			TEST_PSP, PSP_JFT_POINTER_OFFSET + 2u)),
	};

	return pointer;
}

static void set_psp_jft(uint16_t length, struct dos_far_pointer16 pointer)
{
	write_le16(far_bytes(TEST_PSP, PSP_JFT_LENGTH_OFFSET), length);
	write_le16(far_bytes(TEST_PSP, PSP_JFT_POINTER_OFFSET), pointer.offset);
	write_le16(far_bytes(TEST_PSP, PSP_JFT_POINTER_OFFSET + 2u),
		   pointer.segment);
}

static void clear_failures(void)
{
	size_t index;

	for (index = 0u; index < ARRAY_SIZE(failure_rules); ++index)
		failure_rules[index] = (struct write_failure_rule){0};
}

static void fail_write(size_t slot, dos_linear_address_t address,
		       uint32_t skip, uint32_t count, uint32_t partial)
{
	if (slot >= ARRAY_SIZE(failure_rules))
		return;
	failure_rules[slot] = (struct write_failure_rule){
		.address = address,
		.skip = skip,
		.remaining = count,
		.partial = partial,
	};
}

static enum dos_machine_status
test_read_memory(kernel_object_handle_t context, dos_linear_address_t address,
		 void *destination, size_t destination_capacity, size_t count)
{
	uint8_t *output = (uint8_t *)destination;
	size_t index;

	if (context != TEST_MACHINE_CONTEXT || count > destination_capacity ||
	    (uint64_t)address > TEST_MEMORY_BYTES ||
	    (uint64_t)count > TEST_MEMORY_BYTES - (uint64_t)address)
		return DOS_MACHINE_ADDRESS_FAULT;
	for (index = 0u; index < count; ++index)
		output[index] = guest_memory[address + index];
	return DOS_MACHINE_OK;
}

static enum dos_machine_status
test_write_memory(kernel_object_handle_t context, dos_linear_address_t address,
		  const void *source, size_t source_capacity, size_t count)
{
	const uint8_t *input = (const uint8_t *)source;
	struct write_failure_rule *failure = NULL;
	size_t index;
	size_t amount;

	if (context != TEST_MACHINE_CONTEXT || count > source_capacity ||
	    (uint64_t)address > TEST_MEMORY_BYTES ||
	    (uint64_t)count > TEST_MEMORY_BYTES - (uint64_t)address)
		return DOS_MACHINE_ADDRESS_FAULT;
	for (index = 0u; index < ARRAY_SIZE(failure_rules); ++index) {
		struct write_failure_rule *candidate = &failure_rules[index];

		if (candidate->remaining == 0u || candidate->address != address)
			continue;
		if (candidate->skip != 0u) {
			--candidate->skip;
			continue;
		}
		failure = candidate;
		break;
	}
	if (failure == NULL) {
		for (index = 0u; index < count; ++index)
			guest_memory[address + index] = input[index];
		return DOS_MACHINE_OK;
	}
	--failure->remaining;
	amount = failure->partial < count ? failure->partial : count;
	for (index = 0u; index < amount; ++index)
		guest_memory[address + index] = input[index];
	return DOS_MACHINE_ADDRESS_FAULT;
}

static bool reset_fixture(struct test_fixture *fixture, uint16_t arena_end)
{
	static const struct dos_machine_ops machine_ops = {
		.read_memory = test_read_memory,
		.write_memory = test_write_memory,
		.read_port = NULL,
		.write_port = NULL,
		.set_a20 = NULL,
	};
	struct dos_memory_allocation_result allocation;
	struct dos_far_pointer16 inline_pointer = {
		.offset = PSP_JFT_OFFSET,
		.segment = TEST_PSP,
	};
	size_t index;

	for (index = 0u; index < ARRAY_SIZE(guest_memory); ++index)
		guest_memory[index] = 0u;
	clear_failures();
	if (dos_machine_configure(&fixture->machine, &machine_ops,
				  TEST_MACHINE_CONTEXT, TEST_MEMORY_BYTES,
				  true) != DOS_MACHINE_OK ||
	    dos_memory_arena_construct(&fixture->arena,
				       TEST_ARENA_IDENTITY) != DOS_MEMORY_OK ||
	    dos_memory_arena_initialize_checked(
		    &fixture->arena, &fixture->machine, TEST_ARENA_HEAD,
		    arena_end) != DOS_MEMORY_OK ||
	    dos_memory_allocate_checked(
		    &fixture->arena, &fixture->machine, TEST_PSP,
		    TEST_PSP_PARAGRAPHS, &allocation) != DOS_MEMORY_OK ||
	    allocation.block_segment != TEST_PSP)
		return false;
	for (index = 0u; index < DOS_JFT_MINIMUM_HANDLES; ++index)
		far_bytes(TEST_PSP, PSP_JFT_OFFSET)[index] = DOS_JFT_UNUSED;
	/* The initial process maps stdin, stdout, and stderr to SFT 0. */
	far_bytes(TEST_PSP, PSP_JFT_OFFSET)[0] = 0u;
	far_bytes(TEST_PSP, PSP_JFT_OFFSET)[1] = 0u;
	far_bytes(TEST_PSP, PSP_JFT_OFFSET)[2] = 0u;
	set_psp_jft(DOS_JFT_MINIMUM_HANDLES, inline_pointer);
	clear_failures();
	return true;
}

static bool pointer_equals(struct dos_far_pointer16 left,
			   struct dos_far_pointer16 right)
{
	return left.offset == right.offset && left.segment == right.segment;
}

static bool table_prefix_is_initial(struct dos_far_pointer16 pointer)
{
	const uint8_t *table = far_bytes(pointer.segment, pointer.offset);
	size_t index;

	if (table[0] != 0u || table[1] != 0u || table[2] != 0u)
		return false;
	for (index = 3u; index < DOS_JFT_MINIMUM_HANDLES; ++index) {
		if (table[index] != DOS_JFT_UNUSED)
			return false;
	}
	return true;
}

static int test_expand_equal_and_inline_shrink(void)
{
	struct test_fixture fixture;
	struct dos_far_pointer16 external;
	struct dos_far_pointer16 inline_pointer = {
		.offset = PSP_JFT_OFFSET,
		.segment = TEST_PSP,
	};
	size_t index;

	if (!reset_fixture(&fixture, TEST_ARENA_END))
		return 1;
	if (dos_jft_resize_checked(&fixture.arena, &fixture.machine, TEST_PSP,
				   40u) != DOS_JFT_OK)
		return 2;
	external = psp_jft_pointer();
	if (psp_jft_length() != 40u || external.offset != 0u ||
	    external.segment == TEST_PSP || mcb_owner(external.segment) != TEST_PSP ||
	    !table_prefix_is_initial(external))
		return 3;
	for (index = DOS_JFT_MINIMUM_HANDLES; index < 40u; ++index) {
		if (far_bytes(external.segment, 0u)[index] != DOS_JFT_UNUSED)
			return 4;
	}
	fail_write(0u, far_linear(TEST_PSP, PSP_JFT_LENGTH_OFFSET), 0u, 1u,
		   3u);
	if (dos_jft_resize_checked(&fixture.arena, &fixture.machine, TEST_PSP,
				   40u) != DOS_JFT_OK ||
	    failure_rules[0].remaining != 1u)
		return 5;
	far_bytes(external.segment, 0u)[25] = 7u;
	if (dos_jft_resize_checked(&fixture.arena, &fixture.machine, TEST_PSP,
				   0u) != DOS_JFT_TOO_MANY_OPEN_FILES ||
	    psp_jft_length() != 40u ||
	    !pointer_equals(psp_jft_pointer(), external))
		return 6;
	far_bytes(external.segment, 0u)[25] = DOS_JFT_UNUSED;
	clear_failures();
	if (dos_jft_resize_checked(&fixture.arena, &fixture.machine, TEST_PSP,
				   0u) != DOS_JFT_OK ||
	    psp_jft_length() != DOS_JFT_MINIMUM_HANDLES ||
	    !pointer_equals(psp_jft_pointer(), inline_pointer) ||
	    mcb_owner(external.segment) != 0u ||
	    !table_prefix_is_initial(inline_pointer))
		return 7;
	return 0;
}

static int test_external_shrink(void)
{
	struct test_fixture fixture;
	struct dos_far_pointer16 old;
	struct dos_far_pointer16 resized;
	uint8_t *old_table;
	uint8_t *resized_table;

	if (!reset_fixture(&fixture, TEST_ARENA_END))
		return 1;
	if (dos_jft_resize_checked(&fixture.arena, &fixture.machine, TEST_PSP,
				   64u) != DOS_JFT_OK)
		return 2;
	old = psp_jft_pointer();
	old_table = far_bytes(old.segment, old.offset);
	old_table[20] = 20u;
	old_table[31] = 31u;
	if (dos_jft_resize_checked(&fixture.arena, &fixture.machine, TEST_PSP,
				   32u) != DOS_JFT_OK)
		return 3;
	resized = psp_jft_pointer();
	resized_table = far_bytes(resized.segment, resized.offset);
	if (psp_jft_length() != 32u || resized.offset != 0u ||
	    resized.segment == old.segment || mcb_owner(old.segment) != 0u ||
	    mcb_owner(resized.segment) != TEST_PSP || resized_table[20] != 20u ||
	    resized_table[31] != 31u)
		return 4;
	return 0;
}

static int test_limits_and_maximum(void)
{
	struct test_fixture fixture;
	struct dos_far_pointer16 pointer;

	if (!reset_fixture(&fixture, TEST_ARENA_END))
		return 1;
	if (dos_jft_resize_checked(&fixture.arena, &fixture.machine, TEST_PSP,
				   0xffffu) != DOS_JFT_INVALID_FUNCTION ||
	    psp_jft_length() != DOS_JFT_MINIMUM_HANDLES)
		return 2;
	if (dos_jft_resize_checked(&fixture.arena, &fixture.machine, TEST_PSP,
				   DOS_JFT_MAXIMUM_HANDLES) != DOS_JFT_OK)
		return 3;
	pointer = psp_jft_pointer();
	if (psp_jft_length() != DOS_JFT_MAXIMUM_HANDLES ||
	    mcb_owner(pointer.segment) != TEST_PSP ||
	    far_bytes(pointer.segment, 0u)[DOS_JFT_MAXIMUM_HANDLES - 1u] !=
		DOS_JFT_UNUSED)
		return 4;
	if (dos_jft_resize_checked(&fixture.arena, &fixture.machine, TEST_PSP,
				   DOS_JFT_MINIMUM_HANDLES) != DOS_JFT_OK ||
	    mcb_owner(pointer.segment) != 0u)
		return 5;
	if (!reset_fixture(&fixture, TEST_SMALL_ARENA_END))
		return 6;
	if (dos_jft_resize_checked(&fixture.arena, &fixture.machine, TEST_PSP,
				   4096u) != DOS_JFT_NOT_ENOUGH_MEMORY ||
	    psp_jft_length() != DOS_JFT_MINIMUM_HANDLES)
		return 7;
	return 0;
}

static int test_unpublished_failure_cleanup(void)
{
	struct test_fixture fixture;
	uint16_t expected_block = (uint16_t)(TEST_PSP + TEST_PSP_PARAGRAPHS + 1u);
	dos_linear_address_t new_table = far_linear(expected_block, 0u);
	dos_linear_address_t new_header = far_linear(
		(uint16_t)(expected_block - 1u), 0u);

	if (!reset_fixture(&fixture, TEST_ARENA_END))
		return 1;
	fail_write(0u, new_table, 0u, 1u, 5u);
	if (dos_jft_resize_checked(&fixture.arena, &fixture.machine, TEST_PSP,
				   40u) != DOS_JFT_MACHINE_FAULT ||
	    psp_jft_length() != DOS_JFT_MINIMUM_HANDLES ||
	    mcb_owner(expected_block) != 0u || fixture.arena.machine_poisoned != 0u)
		return 2;
	if (!reset_fixture(&fixture, TEST_ARENA_END))
		return 3;
	fail_write(0u, new_table, 0u, 1u, 5u);
	/* Allocation creates this MCB once; fail the cleanup replacement. */
	fail_write(1u, new_header, 1u, 1u, 2u);
	if (dos_jft_resize_checked(&fixture.arena, &fixture.machine, TEST_PSP,
				   40u) != DOS_JFT_MACHINE_POISONED ||
	    fixture.arena.machine_poisoned != 1u)
		return 4;
	return 0;
}

static int test_publication_failure_and_poison(void)
{
	struct test_fixture fixture;
	uint16_t expected_block = (uint16_t)(TEST_PSP + TEST_PSP_PARAGRAPHS + 1u);
	dos_linear_address_t control =
		far_linear(TEST_PSP, PSP_JFT_LENGTH_OFFSET);

	if (!reset_fixture(&fixture, TEST_ARENA_END))
		return 1;
	fail_write(0u, control, 0u, 1u, 3u);
	if (dos_jft_resize_checked(&fixture.arena, &fixture.machine, TEST_PSP,
				   40u) != DOS_JFT_MACHINE_FAULT ||
	    psp_jft_length() != DOS_JFT_MINIMUM_HANDLES ||
	    mcb_owner(expected_block) != 0u || fixture.arena.machine_poisoned != 0u)
		return 2;
	if (!reset_fixture(&fixture, TEST_ARENA_END))
		return 3;
	fail_write(0u, control, 0u, 2u, 3u);
	if (dos_jft_resize_checked(&fixture.arena, &fixture.machine, TEST_PSP,
				   40u) != DOS_JFT_MACHINE_POISONED ||
	    fixture.arena.machine_poisoned != 1u)
		return 4;
	return 0;
}

static int test_old_release_rollback_and_owner_check(void)
{
	struct test_fixture fixture;
	struct dos_far_pointer16 old;
	uint16_t new_block;
	dos_linear_address_t old_header;

	if (!reset_fixture(&fixture, TEST_ARENA_END) ||
	    dos_jft_resize_checked(&fixture.arena, &fixture.machine, TEST_PSP,
				   40u) != DOS_JFT_OK)
		return 1;
	old = psp_jft_pointer();
	new_block = (uint16_t)(old.segment + 4u);
	old_header = far_linear((uint16_t)(old.segment - 1u), 0u);
	clear_failures();
	fail_write(0u, old_header, 0u, 1u, 2u);
	if (dos_jft_resize_checked(&fixture.arena, &fixture.machine, TEST_PSP,
				   64u) != DOS_JFT_MACHINE_FAULT ||
	    psp_jft_length() != 40u ||
	    !pointer_equals(psp_jft_pointer(), old) ||
	    mcb_owner(old.segment) != TEST_PSP || mcb_owner(new_block) != 0u ||
	    fixture.arena.machine_poisoned != 0u)
		return 2;
	clear_failures();
	write_le16(far_bytes((uint16_t)(old.segment - 1u), 1u), 0x2222u);
	if (dos_jft_resize_checked(&fixture.arena, &fixture.machine, TEST_PSP,
				   64u) != DOS_JFT_INVALID_STATE ||
	    psp_jft_length() != 40u ||
	    !pointer_equals(psp_jft_pointer(), old))
		return 3;
	return 0;
}

static int run_tests(void)
{
	int status;

	status = test_expand_equal_and_inline_shrink();
	if (status != 0)
		return 10 + status;
	status = test_external_shrink();
	if (status != 0)
		return 20 + status;
	status = test_limits_and_maximum();
	if (status != 0)
		return 30 + status;
	status = test_unpublished_failure_cleanup();
	if (status != 0)
		return 40 + status;
	status = test_publication_failure_and_poison();
	if (status != 0)
		return 50 + status;
	status = test_old_release_rollback_and_owner_check();
	if (status != 0)
		return 60 + status;
	return 0;
}

DOSC32_TEST_ENTRY(run_tests)

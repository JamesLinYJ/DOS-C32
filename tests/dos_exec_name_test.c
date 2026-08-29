// SPDX-License-Identifier: GPL-2.0-only
/* Freestanding DOS-contract tests for EXEC MCB name parsing. */
#include "dos_exec_name.h"
#include "test_entry.h"

#define OUTPUT_SENTINEL ((uint8_t)0xa5u)
#define TEST_MACHINE_CONTEXT ((kernel_object_handle_t)0x4e414d4554455354ull)
#define TEST_GUEST_CAPACITY DOS_REAL_MODE_ADDRESS_LIMIT

static uint8_t guest_memory[TEST_GUEST_CAPACITY];
static uint8_t guest_name_scratch[DOS_EXEC_NAME_SCAN_LIMIT];
static uint32_t guest_read_calls;
static uint32_t fail_guest_read_call;
static uint8_t guest_callback_valid;

static enum dos_machine_status
test_guest_read(kernel_object_handle_t context,
		dos_linear_address_t linear_address, void *destination,
		size_t destination_capacity, size_t count)
{
	uint8_t *output = (uint8_t *)destination;
	size_t index;

	++guest_read_calls;
	if (context != TEST_MACHINE_CONTEXT || destination == NULL ||
	    count > destination_capacity ||
	    (uint64_t)linear_address >= TEST_GUEST_CAPACITY ||
	    (uint64_t)count >
		TEST_GUEST_CAPACITY - (uint64_t)linear_address) {
		guest_callback_valid = 0u;
		return DOS_MACHINE_ADDRESS_FAULT;
	}
	if (fail_guest_read_call != 0u &&
	    guest_read_calls == fail_guest_read_call)
		return DOS_MACHINE_IO_FAULT;
	for (index = 0u; index < count; ++index)
		output[index] = guest_memory[(size_t)linear_address + index];
	return DOS_MACHINE_OK;
}

static enum dos_machine_status
test_guest_write(kernel_object_handle_t context,
		 dos_linear_address_t linear_address, const void *source,
		 size_t source_capacity, size_t count)
{
	(void)context;
	(void)linear_address;
	(void)source;
	(void)source_capacity;
	(void)count;
	return DOS_MACHINE_STOPPED;
}

static const struct dos_machine_ops guest_machine_ops = {
    .read_memory = test_guest_read,
    .write_memory = test_guest_write,
    .read_port = NULL,
    .write_port = NULL,
    .set_a20 = NULL,
};

static struct dos_machine make_guest_machine(bool a20_enabled)
{
	struct dos_machine machine = {
	    .ops = &guest_machine_ops,
	    .context = TEST_MACHINE_CONTEXT,
	    .address_limit = TEST_GUEST_CAPACITY,
	    .a20_enabled = a20_enabled,
	    .poisoned = 0u,
	};

	return machine;
}

static void reset_guest_adapter(void)
{
	guest_read_calls = 0u;
	fail_guest_read_call = 0u;
	guest_callback_valid = 1u;
}

static void put_guest_byte(struct dos_far_pointer16 source, uint32_t index,
			   bool a20_enabled, uint8_t byte)
{
	uint16_t offset =
	    (uint16_t)((uint32_t)source.offset + (uint16_t)index);
	dos_linear_address_t linear =
	    dos_far_to_linear(source.segment, offset, a20_enabled);

	guest_memory[(size_t)linear] = byte;
}

static void fill_plan(struct dos_exec_name_plan *plan, uint16_t value)
{
	plan->source.offset = value;
	plan->source.segment = value;
	plan->bytes_including_nul = value;
	plan->reserved = value;
}

static bool plan_is_filled(const struct dos_exec_name_plan *plan,
			   uint16_t value)
{
	return plan->source.offset == value && plan->source.segment == value &&
	       plan->bytes_including_nul == value && plan->reserved == value;
}

struct valid_name_case {
	const uint8_t *name;
	size_t capacity;
	size_t length;
	uint8_t expected[DOS_MEMORY_OWNER_NAME_BYTES];
	uint8_t count;
};

static const uint8_t normal_name[] = {
    'C', 'O', 'M', 'M', 'A', 'N', 'D', '.', 'C', 'O', 'M', 0u,
};
static const uint8_t multiple_separator_name[] = {
    'A',   ':', 'D', 'I', 'R', 0x5cu, 'S', 'U', 'B',
    0x5cu, 'R', 'U', 'N', '.', 'E',   'X', 'E', 0u,
};
static const uint8_t colon_name[] = {
    'X', ':', 'M', 'i', 'X', 'e', 'D', '.', 'B', 'I', 'N', 0u,
};
static const uint8_t backslash_name[] = {
    'D', 'I', 'R', 0x5cu, 'B', 'A', 'C', 'K', '.', 'C', 'O', 'M', 0u,
};
static const uint8_t forward_slash_name[] = {
    'C', ':', '/', 'B', 'I', 'N', '/', 'A', 'P', 'P', '.', 'E', 'X', 'E', 0u,
};
static const uint8_t short_name[] = {'A', '.', 'C', 'O', 'M', 0u};
static const uint8_t eight_byte_name[] = {
    '1', '2', '3', '4', '5', '6', '7', '8', '.', 'C', 'O', 'M', 0u,
};
static const uint8_t long_name[] = {
    '1', '2', '3', '4', '5', '6', '7', '8', '9', '.', 'C', 'O', 'M', 0u,
};
static const uint8_t dot_name[] = {'.', 'E', 'X', 'E', 0u};
static const uint8_t trailing_separator_name[] = {
    'C', ':', 0x5cu, 'D', 'I', 'R', 0x5cu, 0u,
};
static const uint8_t empty_name[] = {0u};
static const uint8_t spare_capacity_name[] = {'O', 'K', 0u, 0x7eu};

static const struct valid_name_case valid_cases[] = {
    {
	.name = normal_name,
	.capacity = ARRAY_SIZE(normal_name),
	.length = ARRAY_SIZE(normal_name),
	.expected = {'C', 'O', 'M', 'M', 'A', 'N', 'D', 0u},
	.count = 8u,
    },
    {
	.name = multiple_separator_name,
	.capacity = ARRAY_SIZE(multiple_separator_name),
	.length = ARRAY_SIZE(multiple_separator_name),
	.expected = {'R', 'U', 'N', 0u},
	.count = 4u,
    },
    {
	.name = colon_name,
	.capacity = ARRAY_SIZE(colon_name),
	.length = ARRAY_SIZE(colon_name),
	.expected = {'M', 'i', 'X', 'e', 'D', 0u},
	.count = 6u,
    },
    {
	.name = backslash_name,
	.capacity = ARRAY_SIZE(backslash_name),
	.length = ARRAY_SIZE(backslash_name),
	.expected = {'B', 'A', 'C', 'K', 0u},
	.count = 5u,
    },
    {
	.name = forward_slash_name,
	.capacity = ARRAY_SIZE(forward_slash_name),
	.length = ARRAY_SIZE(forward_slash_name),
	.expected = {'/', 'B', 'I', 'N', '/', 'A', 'P', 'P'},
	.count = 8u,
    },
    {
	.name = short_name,
	.capacity = ARRAY_SIZE(short_name),
	.length = ARRAY_SIZE(short_name),
	.expected = {'A', 0u},
	.count = 2u,
    },
    {
	.name = eight_byte_name,
	.capacity = ARRAY_SIZE(eight_byte_name),
	.length = ARRAY_SIZE(eight_byte_name),
	.expected = {'1', '2', '3', '4', '5', '6', '7', '8'},
	.count = 8u,
    },
    {
	.name = long_name,
	.capacity = ARRAY_SIZE(long_name),
	.length = ARRAY_SIZE(long_name),
	.expected = {'1', '2', '3', '4', '5', '6', '7', '8'},
	.count = 8u,
    },
    {
	.name = dot_name,
	.capacity = ARRAY_SIZE(dot_name),
	.length = ARRAY_SIZE(dot_name),
	.expected = {0u},
	.count = 1u,
    },
    {
	.name = trailing_separator_name,
	.capacity = ARRAY_SIZE(trailing_separator_name),
	.length = ARRAY_SIZE(trailing_separator_name),
	.expected = {0u},
	.count = 1u,
    },
    {
	.name = empty_name,
	.capacity = ARRAY_SIZE(empty_name),
	.length = ARRAY_SIZE(empty_name),
	.expected = {0u},
	.count = 1u,
    },
    {
	.name = spare_capacity_name,
	.capacity = ARRAY_SIZE(spare_capacity_name),
	.length = 3u,
	.expected = {'O', 'K', 0u},
	.count = 3u,
    },
};

static void fill_patch(struct dos_memory_owner_name_patch *patch, uint8_t value)
{
	size_t index;

	for (index = 0u; index < ARRAY_SIZE(patch->bytes); ++index)
		patch->bytes[index] = value;
	patch->count = value;
	for (index = 0u; index < ARRAY_SIZE(patch->reserved); ++index)
		patch->reserved[index] = value;
}

static bool patch_matches(const struct dos_memory_owner_name_patch *patch,
			  const uint8_t expected[DOS_MEMORY_OWNER_NAME_BYTES],
			  uint8_t count)
{
	size_t index;

	if (patch->count != count)
		return false;
	for (index = 0u; index < ARRAY_SIZE(patch->bytes); ++index) {
		if (patch->bytes[index] != expected[index])
			return false;
	}
	for (index = 0u; index < ARRAY_SIZE(patch->reserved); ++index) {
		if (patch->reserved[index] != 0u)
			return false;
	}
	return true;
}

static bool patch_is_filled(const struct dos_memory_owner_name_patch *patch,
			    uint8_t value)
{
	size_t index;

	if (patch->count != value)
		return false;
	for (index = 0u; index < ARRAY_SIZE(patch->bytes); ++index) {
		if (patch->bytes[index] != value)
			return false;
	}
	for (index = 0u; index < ARRAY_SIZE(patch->reserved); ++index) {
		if (patch->reserved[index] != value)
			return false;
	}
	return true;
}

static int test_valid_names(void)
{
	struct dos_memory_owner_name_patch patch;
	size_t index;

	for (index = 0u; index < ARRAY_SIZE(valid_cases); ++index) {
		fill_patch(&patch, OUTPUT_SENTINEL);
		if (dos_exec_name_build_owner_patch(
			valid_cases[index].name, valid_cases[index].capacity,
			valid_cases[index].length,
			&patch) != DOS_EXEC_NAME_OK ||
		    !patch_matches(&patch, valid_cases[index].expected,
				   valid_cases[index].count))
			return (int)index + 1;
	}
	return 0;
}

static int
expect_error_without_output_change(const uint8_t *name, size_t capacity,
				   size_t length,
				   enum dos_exec_name_status expected_status)
{
	struct dos_memory_owner_name_patch patch;

	fill_patch(&patch, OUTPUT_SENTINEL);
	if (dos_exec_name_build_owner_patch(name, capacity, length, &patch) !=
		expected_status ||
	    !patch_is_filled(&patch, OUTPUT_SENTINEL))
		return 1;
	return 0;
}

static int test_invalid_names_and_arguments(void)
{
	static const uint8_t embedded_nul[] = {'A', 0u, 'B', 0u};
	static const uint8_t missing_nul[] = {'A', 'B', 'C'};
	static const uint8_t valid[] = {'O', 'K', 0u};
	struct dos_memory_owner_name_patch patch;

	if (expect_error_without_output_change(
		embedded_nul, ARRAY_SIZE(embedded_nul),
		ARRAY_SIZE(embedded_nul), DOS_EXEC_NAME_INVALID_NAME) != 0)
		return 1;
	if (expect_error_without_output_change(
		missing_nul, ARRAY_SIZE(missing_nul), ARRAY_SIZE(missing_nul),
		DOS_EXEC_NAME_INVALID_NAME) != 0)
		return 2;
	if (expect_error_without_output_change(
		valid, ARRAY_SIZE(valid), 0u, DOS_EXEC_NAME_INVALID_ARGUMENT) !=
	    0)
		return 3;
	if (expect_error_without_output_change(
		valid, 2u, ARRAY_SIZE(valid), DOS_EXEC_NAME_INVALID_ARGUMENT) !=
	    0)
		return 4;
	if (expect_error_without_output_change(
		valid, 0u, 1u, DOS_EXEC_NAME_INVALID_ARGUMENT) != 0)
		return 5;
	if (expect_error_without_output_change(
		NULL, ARRAY_SIZE(valid), ARRAY_SIZE(valid),
		DOS_EXEC_NAME_INVALID_ARGUMENT) != 0)
		return 6;
	fill_patch(&patch, OUTPUT_SENTINEL);
	if (dos_exec_name_build_owner_patch(valid, ARRAY_SIZE(valid),
					    ARRAY_SIZE(valid), NULL) !=
		DOS_EXEC_NAME_INVALID_ARGUMENT ||
	    !patch_is_filled(&patch, OUTPUT_SENTINEL))
		return 7;
	return 0;
}

static int test_guest_scan_exact_fault_boundary(void)
{
	struct dos_machine machine = make_guest_machine(false);
	struct dos_far_pointer16 source = {
	    .offset = 0x0100u,
	    .segment = 0x1234u,
	};
	struct dos_exec_name_plan plan;
	uint8_t short_scratch[4] = {OUTPUT_SENTINEL, OUTPUT_SENTINEL,
				    OUTPUT_SENTINEL, OUTPUT_SENTINEL};

	put_guest_byte(source, 0u, false, (uint8_t)'A');
	put_guest_byte(source, 1u, false, 0u);
	put_guest_byte(source, 2u, false, (uint8_t)'X');
	reset_guest_adapter();
	fail_guest_read_call = 3u;
	fill_plan(&plan, 0xa5a5u);
	if (dos_exec_name_read_guest(&machine, source, short_scratch,
				     ARRAY_SIZE(short_scratch), &plan) !=
		DOS_EXEC_NAME_OK ||
	    guest_read_calls != 2u || guest_callback_valid == 0u ||
	    short_scratch[0] != (uint8_t)'A' || short_scratch[1] != 0u ||
	    plan.source.offset != source.offset ||
	    plan.source.segment != source.segment ||
	    plan.bytes_including_nul != 2u || plan.reserved != 0u ||
	    !dos_exec_name_plan_has_valid_encoding(&plan))
		return 1;

	put_guest_byte(source, 0u, false, 0u);
	reset_guest_adapter();
	fill_plan(&plan, 0xa5a5u);
	if (dos_exec_name_read_guest(&machine, source, short_scratch,
				     ARRAY_SIZE(short_scratch), &plan) !=
		DOS_EXEC_NAME_OK ||
	    guest_read_calls != 1u || plan.bytes_including_nul != 1u)
		return 2;

	put_guest_byte(source, 0u, false, (uint8_t)'A');
	put_guest_byte(source, 1u, false, (uint8_t)'B');
	put_guest_byte(source, 2u, false, 0u);
	reset_guest_adapter();
	fill_plan(&plan, 0xa5a5u);
	if (dos_exec_name_read_guest(&machine, source, short_scratch, 2u,
				     &plan) !=
		DOS_EXEC_NAME_BUFFER_TOO_SMALL ||
	    guest_read_calls != 2u || !plan_is_filled(&plan, 0xa5a5u) ||
	    short_scratch[0] != (uint8_t)'A' ||
	    short_scratch[1] != (uint8_t)'B')
		return 3;

	reset_guest_adapter();
	fail_guest_read_call = 2u;
	short_scratch[1] = OUTPUT_SENTINEL;
	fill_plan(&plan, 0xa5a5u);
	if (dos_exec_name_read_guest(&machine, source, short_scratch,
				     ARRAY_SIZE(short_scratch), &plan) !=
		DOS_EXEC_NAME_GUEST_FAULT ||
	    guest_read_calls != 2u || !plan_is_filled(&plan, 0xa5a5u) ||
	    short_scratch[0] != (uint8_t)'A' ||
	    short_scratch[1] != OUTPUT_SENTINEL)
		return 4;
	return 0;
}

static int test_guest_offset_wrap_and_a20(void)
{
	struct dos_machine wrapped_machine = make_guest_machine(false);
	struct dos_machine linear_machine = make_guest_machine(true);
	struct dos_far_pointer16 wrapping_source = {
	    .offset = 0xfffeu,
	    .segment = 0x2000u,
	};
	struct dos_far_pointer16 a20_source = {
	    .offset = 0x0010u,
	    .segment = 0xffffu,
	};
	struct dos_exec_name_plan plan;
	uint8_t scratch[4];

	put_guest_byte(wrapping_source, 0u, false, (uint8_t)'A');
	put_guest_byte(wrapping_source, 1u, false, (uint8_t)'B');
	put_guest_byte(wrapping_source, 2u, false, 0u);
	reset_guest_adapter();
	if (dos_exec_name_read_guest(&wrapped_machine, wrapping_source, scratch,
				     ARRAY_SIZE(scratch), &plan) !=
		DOS_EXEC_NAME_OK ||
	    guest_read_calls != 3u || scratch[0] != (uint8_t)'A' ||
	    scratch[1] != (uint8_t)'B' || scratch[2] != 0u ||
	    plan.bytes_including_nul != 3u)
		return 1;

	put_guest_byte(a20_source, 0u, false, 0u);
	put_guest_byte(a20_source, 0u, true, (uint8_t)'X');
	put_guest_byte(a20_source, 1u, true, 0u);
	reset_guest_adapter();
	if (dos_exec_name_read_guest(&wrapped_machine, a20_source, scratch,
				     ARRAY_SIZE(scratch), &plan) !=
		DOS_EXEC_NAME_OK ||
	    guest_read_calls != 1u || plan.bytes_including_nul != 1u)
		return 2;
	reset_guest_adapter();
	if (dos_exec_name_read_guest(&linear_machine, a20_source, scratch,
				     ARRAY_SIZE(scratch), &plan) !=
		DOS_EXEC_NAME_OK ||
	    guest_read_calls != 2u || scratch[0] != (uint8_t)'X' ||
	    scratch[1] != 0u || plan.bytes_including_nul != 2u)
		return 3;
	return 0;
}

static int test_guest_scan_limit_and_arguments(void)
{
	struct dos_machine machine = make_guest_machine(false);
	struct dos_far_pointer16 source = {0u, 0u};
	struct dos_exec_name_plan plan;
	size_t index;

	for (index = 0u; index < DOS_EXEC_NAME_SCAN_LIMIT; ++index)
		put_guest_byte(source, (uint32_t)index, false, (uint8_t)'Q');
	reset_guest_adapter();
	fill_plan(&plan, 0xa5a5u);
	if (dos_exec_name_read_guest(&machine, source, guest_name_scratch,
				     ARRAY_SIZE(guest_name_scratch), &plan) !=
		DOS_EXEC_NAME_INVALID_NAME ||
	    guest_read_calls != DOS_EXEC_NAME_SCAN_LIMIT ||
	    !plan_is_filled(&plan, 0xa5a5u))
		return 1;

	put_guest_byte(source, DOS_EXEC_NAME_SCAN_LIMIT - 1u, false, 0u);
	reset_guest_adapter();
	fill_plan(&plan, 0xa5a5u);
	if (dos_exec_name_read_guest(&machine, source, guest_name_scratch,
				     ARRAY_SIZE(guest_name_scratch), &plan) !=
		DOS_EXEC_NAME_OK ||
	    guest_read_calls != DOS_EXEC_NAME_SCAN_LIMIT ||
	    plan.bytes_including_nul != 0xffffu || plan.reserved != 0u)
		return 2;

	fill_plan(&plan, 0xa5a5u);
	if (dos_exec_name_read_guest(NULL, source, guest_name_scratch,
				     ARRAY_SIZE(guest_name_scratch), &plan) !=
		DOS_EXEC_NAME_INVALID_ARGUMENT ||
	    dos_exec_name_read_guest(&machine, source, NULL,
				     ARRAY_SIZE(guest_name_scratch), &plan) !=
		DOS_EXEC_NAME_INVALID_ARGUMENT ||
	    dos_exec_name_read_guest(&machine, source, guest_name_scratch, 0u,
				     &plan) != DOS_EXEC_NAME_INVALID_ARGUMENT ||
	    dos_exec_name_read_guest(&machine, source, guest_name_scratch,
				     ARRAY_SIZE(guest_name_scratch), NULL) !=
		DOS_EXEC_NAME_INVALID_ARGUMENT ||
	    dos_exec_name_read_guest(&machine, source,
				     (uint8_t *)(uintptr_t)-1, 2u, &plan) !=
		DOS_EXEC_NAME_INVALID_ARGUMENT ||
	    !plan_is_filled(&plan, 0xa5a5u))
		return 3;

	plan = (struct dos_exec_name_plan){
	    .source = {.offset = 0xffffu, .segment = 0xffffu},
	    .bytes_including_nul = 1u,
	    .reserved = 0u,
	};
	if (!dos_exec_name_plan_has_valid_encoding(&plan))
		return 4;
	plan.bytes_including_nul = 0u;
	if (dos_exec_name_plan_has_valid_encoding(&plan))
		return 5;
	plan.bytes_including_nul = 1u;
	plan.reserved = 1u;
	if (dos_exec_name_plan_has_valid_encoding(&plan) ||
	    dos_exec_name_plan_has_valid_encoding(NULL))
		return 6;
	return 0;
}

static int run_tests(void)
{
	int result = test_valid_names();

	if (result != 0)
		return result;
	result = test_invalid_names_and_arguments();
	if (result != 0)
		return 32 + result;
	result = test_guest_scan_exact_fault_boundary();
	if (result != 0)
		return 64 + result;
	result = test_guest_offset_wrap_and_a20();
	if (result != 0)
		return 80 + result;
	result = test_guest_scan_limit_and_arguments();
	if (result != 0)
		return 96 + result;
	return 0;
}

DOSC32_TEST_ENTRY(run_tests)

// SPDX-License-Identifier: GPL-2.0-only
/* Freestanding tests for bounded dynamic-JFT process termination. */
#include "dos_termination.h"
#include "test_entry.h"

#define TEST_MEMORY_BYTES 0x00040000u
#define TEST_PSP_SEGMENT 0x1000u
#define TEST_JFT_SEGMENT 0x2000u
#define TEST_JFT_OFFSET 0u
#define TEST_JFT_MAXIMUM 0xfffeu
#define TEST_EVENT_CAPACITY 32u
#define TEST_REFERENCE_BASE ((dos_sft_reference_handle_t)0x100000000ull)
#define TEST_MACHINE_CONTEXT \
	((kernel_object_handle_t)0x5445524d4d414348ull)
#define TEST_MACHINE_IDENTITY \
	((kernel_object_handle_t)0x5445524d4944454eull)
#define TEST_SFT_CONTEXT ((kernel_object_handle_t)0x534654434f4e5445ull)
#define TEST_SFT_IDENTITY ((kernel_object_handle_t)0x5346544944454e54ull)

static uint8_t guest_memory[TEST_MEMORY_BYTES];
static uint32_t read_calls;
static uint32_t jft_read_calls;
static uint32_t fail_read_call;
static size_t maximum_jft_read;
static uint8_t lookup_events[TEST_EVENT_CAPACITY];
static uint8_t release_events[TEST_EVENT_CAPACITY];
static uint8_t device_events[TEST_EVENT_CAPACITY];
static size_t lookup_count;
static size_t release_count;
static size_t device_count;
static uint8_t fail_release_sfn;

static dos_linear_address_t test_linear(uint16_t segment, uint16_t offset)
{
	return ((dos_linear_address_t)segment << 4) + offset;
}

static void clear_bytes(uint8_t *bytes, size_t count, uint8_t value)
{
	size_t index;

	for (index = 0u; index < count; ++index)
		bytes[index] = value;
}

static bool bytes_are_equal(const void *left, const void *right, size_t count)
{
	const uint8_t *left_bytes = (const uint8_t *)left;
	const uint8_t *right_bytes = (const uint8_t *)right;
	size_t index;

	for (index = 0u; index < count; ++index) {
		if (left_bytes[index] != right_bytes[index])
			return false;
	}
	return true;
}

static void put_le16(dos_linear_address_t address, uint16_t value)
{
	guest_memory[address] = (uint8_t)value;
	guest_memory[address + 1u] = (uint8_t)(value >> 8);
}

static void put_far(dos_linear_address_t address, uint16_t segment,
		    uint16_t offset)
{
	put_le16(address, offset);
	put_le16(address + 2u, segment);
}

static void reset_observation(void)
{
	read_calls = 0u;
	jft_read_calls = 0u;
	fail_read_call = 0u;
	maximum_jft_read = 0u;
	lookup_count = 0u;
	release_count = 0u;
	device_count = 0u;
	fail_release_sfn = DOS_JFT_ENTRY_UNUSED;
	clear_bytes(lookup_events, sizeof(lookup_events), 0u);
	clear_bytes(release_events, sizeof(release_events), 0u);
	clear_bytes(device_events, sizeof(device_events), 0u);
}

static void initialize_psp(uint16_t jft_length, uint16_t jft_segment,
			   uint16_t jft_offset)
{
	dos_linear_address_t psp = test_linear(TEST_PSP_SEGMENT, 0u);
	dos_linear_address_t jft = test_linear(jft_segment, jft_offset);

	clear_bytes(guest_memory, sizeof(guest_memory), 0u);
	if ((uint64_t)jft < TEST_MEMORY_BYTES &&
	    (uint64_t)jft_length <= TEST_MEMORY_BYTES - (uint64_t)jft)
		clear_bytes(guest_memory + jft, jft_length,
			    DOS_JFT_ENTRY_UNUSED);
	put_far(psp + __builtin_offsetof(struct dos_psp_prefix40, exit_vector),
		0x3000u, 0x0010u);
	put_far(psp + __builtin_offsetof(struct dos_psp_prefix40,
						 control_c_vector),
		0x3001u, 0x0020u);
	put_far(psp + __builtin_offsetof(struct dos_psp_prefix40,
						 fatal_abort_vector),
		0x3002u, 0x0030u);
	put_le16(psp + __builtin_offsetof(struct dos_psp_prefix40, parent_psp),
		 0x0800u);
	put_far(psp + __builtin_offsetof(struct dos_psp_prefix40, user_stack),
		0x0800u, 0xff00u);
	put_le16(psp + __builtin_offsetof(struct dos_psp_prefix40, jft_length),
		 jft_length);
	put_far(psp + __builtin_offsetof(struct dos_psp_prefix40, jft_pointer),
		jft_segment, jft_offset);
	reset_observation();
}

static enum dos_machine_status
read_memory(kernel_object_handle_t context, dos_linear_address_t address,
	    void *destination, size_t destination_capacity, size_t count)
{
	uint8_t *output = (uint8_t *)destination;
	dos_linear_address_t jft =
		test_linear(TEST_JFT_SEGMENT, TEST_JFT_OFFSET);
	size_t index;

	++read_calls;
	if (fail_read_call != 0u && read_calls == fail_read_call)
		return DOS_MACHINE_IO_FAULT;
	if (context != TEST_MACHINE_CONTEXT || destination == NULL ||
	    count > destination_capacity || address > TEST_MEMORY_BYTES ||
	    (uint64_t)count > TEST_MEMORY_BYTES - (uint64_t)address)
		return DOS_MACHINE_ADDRESS_FAULT;
	if (address >= jft && address < jft + TEST_JFT_MAXIMUM) {
		++jft_read_calls;
		if (count > maximum_jft_read)
			maximum_jft_read = count;
	}
	for (index = 0u; index < count; ++index)
		output[index] = guest_memory[address + index];
	return DOS_MACHINE_OK;
}

static enum dos_machine_status
write_memory(kernel_object_handle_t context, dos_linear_address_t address,
	     const void *source, size_t source_capacity, size_t count)
{
	const uint8_t *input = (const uint8_t *)source;
	size_t index;

	if (context != TEST_MACHINE_CONTEXT || source == NULL ||
	    count > source_capacity || address > TEST_MEMORY_BYTES ||
	    (uint64_t)count > TEST_MEMORY_BYTES - (uint64_t)address)
		return DOS_MACHINE_ADDRESS_FAULT;
	for (index = 0u; index < count; ++index)
		guest_memory[address + index] = input[index];
	return DOS_MACHINE_OK;
}

static enum dos_sft_adapter_status
lookup_sft(kernel_object_handle_t context, uint8_t sfn,
	   struct dos_sft_view *view)
{
	if (context != TEST_SFT_CONTEXT || view == NULL ||
	    lookup_count >= ARRAY_SIZE(lookup_events))
		return DOS_SFT_ADAPTER_FAULT;
	lookup_events[lookup_count++] = sfn;
	if (sfn == 0xeeu)
		return DOS_SFT_ADAPTER_INVALID_SFT;
	if (sfn == 0xefu)
		return DOS_SFT_ADAPTER_FAULT;
	view->reference_handle = TEST_REFERENCE_BASE + sfn;
	view->flags = sfn == 7u ? DOS_SFT_FLAG_IS_NETWORK : 0u;
	view->mode = 0u;
	return DOS_SFT_ADAPTER_OK;
}

static bool reference_to_sfn(dos_sft_reference_handle_t reference,
			      uint8_t *sfn)
{
	uint64_t value;

	if (sfn == NULL || reference < TEST_REFERENCE_BASE)
		return false;
	value = reference - TEST_REFERENCE_BASE;
	if (value >= DOS_JFT_ENTRY_UNUSED)
		return false;
	*sfn = (uint8_t)value;
	return true;
}

static enum dos_sft_adapter_status
release_reference(kernel_object_handle_t context,
		  dos_sft_reference_handle_t reference)
{
	uint8_t sfn;

	if (context != TEST_SFT_CONTEXT ||
	    release_count >= ARRAY_SIZE(release_events) ||
	    !reference_to_sfn(reference, &sfn))
		return DOS_SFT_ADAPTER_FAULT;
	release_events[release_count++] = sfn;
	return sfn == fail_release_sfn ? DOS_SFT_ADAPTER_FAULT
				       : DOS_SFT_ADAPTER_OK;
}

static enum dos_sft_adapter_status
close_device(kernel_object_handle_t context,
	     dos_sft_reference_handle_t reference)
{
	uint8_t sfn;

	if (context != TEST_SFT_CONTEXT ||
	    device_count >= ARRAY_SIZE(device_events) ||
	    !reference_to_sfn(reference, &sfn))
		return DOS_SFT_ADAPTER_FAULT;
	device_events[device_count++] = sfn;
	return DOS_SFT_ADAPTER_OK;
}

static const struct dos_machine_ops machine_ops = {
	.read_memory = read_memory,
	.write_memory = write_memory,
};

static const struct dos_sft_batch_ops sft_ops = {
	.identity = TEST_SFT_IDENTITY,
	.lookup = lookup_sft,
	.reference_release = release_reference,
	.device_close = close_device,
};

static bool event_sequence_matches(const uint8_t *actual,
				   size_t actual_count,
				   const uint8_t *expected,
				   size_t expected_count)
{
	return actual_count == expected_count &&
	       bytes_are_equal(actual, expected, expected_count);
}

static int test_maximum_jft_reverse(const struct dos_machine *machine)
{
	static const uint8_t expected[] = {8u, 7u, 6u, 5u,
					   4u, 3u, 2u, 1u};
	static const uint8_t expected_devices[] = {8u, 6u, 5u, 4u,
						   3u, 2u, 1u};
	struct dos_termination_plan plan;
	dos_linear_address_t jft =
		test_linear(TEST_JFT_SEGMENT, TEST_JFT_OFFSET);

	initialize_psp(TEST_JFT_MAXIMUM, TEST_JFT_SEGMENT, TEST_JFT_OFFSET);
	guest_memory[jft] = 1u;
	guest_memory[jft + 63u] = 2u;
	guest_memory[jft + 64u] = 3u;
	guest_memory[jft + 255u] = 4u;
	guest_memory[jft + 256u] = 5u;
	guest_memory[jft + 1024u] = 6u;
	guest_memory[jft + TEST_JFT_MAXIMUM - 2u] = 7u;
	guest_memory[jft + TEST_JFT_MAXIMUM - 1u] = 8u;
	if (dos_termination_capture(machine, TEST_MACHINE_IDENTITY,
				    TEST_PSP_SEGMENT, &plan) !=
		DOS_TERMINATION_OK ||
	    plan.jft_length != TEST_JFT_MAXIMUM ||
	    plan.jft_pointer.segment != TEST_JFT_SEGMENT ||
	    plan.jft_pointer.offset != TEST_JFT_OFFSET ||
	    maximum_jft_read > 64u || jft_read_calls <= 1u)
		return 1;

	reset_observation();
	if (dos_termination_close_handles(
		machine, TEST_MACHINE_IDENTITY, &plan, &sft_ops,
		TEST_SFT_CONTEXT) != DOS_TERMINATION_OK ||
	    maximum_jft_read > 64u || jft_read_calls <= 2u ||
	    !event_sequence_matches(lookup_events, lookup_count, expected,
				    ARRAY_SIZE(expected)) ||
	    !event_sequence_matches(release_events, release_count, expected,
				    ARRAY_SIZE(expected)) ||
	    !event_sequence_matches(device_events, device_count,
				    expected_devices,
				    ARRAY_SIZE(expected_devices)))
		return 2;
	return 0;
}

static int test_range_and_read_preflight(const struct dos_machine *machine)
{
	struct dos_termination_plan expected;
	struct dos_termination_plan plan;
	struct dos_machine short_machine = *machine;
	dos_linear_address_t jft =
		test_linear(TEST_JFT_SEGMENT, TEST_JFT_OFFSET);

	initialize_psp(128u, TEST_JFT_SEGMENT, TEST_JFT_OFFSET);
	clear_bytes((uint8_t *)(void *)&plan, sizeof(plan), 0xa5u);
	expected = plan;
	short_machine.address_limit = (uint64_t)jft + 127u;
	if (dos_termination_capture(&short_machine, TEST_MACHINE_IDENTITY,
				    TEST_PSP_SEGMENT, &plan) !=
		DOS_TERMINATION_MACHINE_FAULT ||
	    !bytes_are_equal(&plan, &expected, sizeof(plan)) ||
	    jft_read_calls != 0u || lookup_count != 0u)
		return 1;

	initialize_psp(200u, TEST_JFT_SEGMENT, TEST_JFT_OFFSET);
	guest_memory[jft + 199u] = 9u;
	if (dos_termination_capture(machine, TEST_MACHINE_IDENTITY,
				    TEST_PSP_SEGMENT, &plan) !=
		DOS_TERMINATION_OK)
		return 2;
	reset_observation();
	fail_read_call = 2u;
	if (dos_termination_close_handles(
		machine, TEST_MACHINE_IDENTITY, &plan, &sft_ops,
		TEST_SFT_CONTEXT) != DOS_TERMINATION_MACHINE_FAULT ||
	    lookup_count != 0u || release_count != 0u || device_count != 0u)
		return 3;
	return 0;
}

static int test_post_close_read_fault(const struct dos_machine *machine)
{
	struct dos_termination_plan plan;
	dos_linear_address_t jft =
		test_linear(TEST_JFT_SEGMENT, TEST_JFT_OFFSET);

	initialize_psp(200u, TEST_JFT_SEGMENT, TEST_JFT_OFFSET);
	guest_memory[jft + 199u] = 9u;
	guest_memory[jft + 100u] = 4u;
	if (dos_termination_capture(machine, TEST_MACHINE_IDENTITY,
				    TEST_PSP_SEGMENT, &plan) !=
		DOS_TERMINATION_OK)
		return 1;
	reset_observation();
	/* Four preflight reads, one successful reverse read, then failure. */
	fail_read_call = 6u;
	if (dos_termination_close_handles(
		machine, TEST_MACHINE_IDENTITY, &plan, &sft_ops,
		TEST_SFT_CONTEXT) != DOS_TERMINATION_SFT_POISONED ||
	    lookup_count != 1u || lookup_events[0] != 9u ||
	    release_count != 1u || release_events[0] != 9u ||
	    device_count != 1u || device_events[0] != 9u)
		return 2;
	return 0;
}

static int test_callback_failure_continues(const struct dos_machine *machine)
{
	static const uint8_t expected[] = {5u, 3u};
	static const uint8_t expected_device[] = {3u};
	struct dos_termination_plan plan;
	dos_linear_address_t jft =
		test_linear(TEST_JFT_SEGMENT, TEST_JFT_OFFSET);

	initialize_psp(70u, TEST_JFT_SEGMENT, TEST_JFT_OFFSET);
	guest_memory[jft] = 3u;
	guest_memory[jft + 69u] = 5u;
	if (dos_termination_capture(machine, TEST_MACHINE_IDENTITY,
				    TEST_PSP_SEGMENT, &plan) !=
		DOS_TERMINATION_OK)
		return 1;
	reset_observation();
	fail_release_sfn = 5u;
	if (dos_termination_close_handles(
		machine, TEST_MACHINE_IDENTITY, &plan, &sft_ops,
		TEST_SFT_CONTEXT) != DOS_TERMINATION_SFT_POISONED ||
	    !event_sequence_matches(lookup_events, lookup_count, expected,
				    ARRAY_SIZE(expected)) ||
	    !event_sequence_matches(release_events, release_count, expected,
				    ARRAY_SIZE(expected)) ||
	    !event_sequence_matches(device_events, device_count,
				    expected_device,
				    ARRAY_SIZE(expected_device)))
		return 2;
	return 0;
}

static int test_zero_and_stale_plan(const struct dos_machine *machine)
{
	struct dos_termination_plan plan;

	initialize_psp(0u, 0xffffu, 0xffffu);
	if (dos_termination_capture(machine, TEST_MACHINE_IDENTITY,
				    TEST_PSP_SEGMENT, &plan) !=
		DOS_TERMINATION_OK ||
	    dos_termination_close_handles(
		machine, TEST_MACHINE_IDENTITY, &plan, &sft_ops,
		TEST_SFT_CONTEXT) != DOS_TERMINATION_OK ||
	    jft_read_calls != 0u || lookup_count != 0u)
		return 1;
	plan.reserved[0] = 1u;
	reset_observation();
	if (dos_termination_close_handles(
		machine, TEST_MACHINE_IDENTITY, &plan, &sft_ops,
		TEST_SFT_CONTEXT) != DOS_TERMINATION_STALE_PLAN ||
	    read_calls != 0u || lookup_count != 0u)
		return 2;
	return 0;
}

static int run_tests(void)
{
	struct dos_machine machine;
	int status;

	if (dos_machine_configure(&machine, &machine_ops,
				  TEST_MACHINE_CONTEXT, TEST_MEMORY_BYTES,
				  false) != DOS_MACHINE_OK)
		return 1;
	status = test_maximum_jft_reverse(&machine);
	if (status != 0)
		return 10 + status;
	status = test_range_and_read_preflight(&machine);
	if (status != 0)
		return 20 + status;
	status = test_post_close_read_fault(&machine);
	if (status != 0)
		return 30 + status;
	status = test_callback_failure_continues(&machine);
	if (status != 0)
		return 40 + status;
	status = test_zero_and_stale_plan(&machine);
	if (status != 0)
		return 50 + status;
	return 0;
}

DOSC32_TEST_ENTRY(run_tests)

// SPDX-License-Identifier: GPL-2.0-only
/* Freestanding regression tests for PSP construction and COM/MZ plans. */
#include "dos_process.h"
#include "test_entry.h"

#define TEST_MEMORY_SIZE DOS_A20_WRAP_ADDRESS
#define TEST_PARENT_SEGMENT 0x1000u
#define TEST_JFT_SEGMENT 0x1100u
#define TEST_SOURCE_SEGMENT 0x1200u
#define TEST_CHILD_SEGMENT 0x2000u
#define TEST_PSP_JFT_OFFSET 0x18u
#define TEST_PSP_PARENT_OFFSET 0x16u
#define TEST_PSP_ENVIRONMENT_OFFSET 0x2cu
#define TEST_PSP_JFT_LENGTH_OFFSET 0x32u
#define TEST_PSP_JFT_POINTER_OFFSET 0x34u
#define TEST_PSP_NEXT_OFFSET 0x38u
#define TEST_PSP_SYSTEM_CALL_OFFSET 0x50u

static uint8_t guest_memory[TEST_MEMORY_SIZE];
static uint32_t read_calls;
static uint32_t write_calls;
static dos_linear_address_t last_write_address;
static size_t last_write_count;
static bool fail_next_read;
static uint32_t write_failures_remaining;
static size_t first_failure_partial_bytes;

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

/*
 * Independent model of REP MOVSB's read-then-write order.  Do not replace
 * this with a bulk-copy helper: destination-after-source overlap is the DOS
 * behavior under test.
 */
static bool model_forward_copy(uint8_t *bytes, size_t capacity,
			       size_t destination, size_t source, size_t count)
{
	size_t index;

	if (bytes == NULL || destination > capacity || source > capacity ||
	    count > capacity - destination || count > capacity - source)
		return false;
	for (index = 0u; index < count; ++index)
		bytes[destination + index] = bytes[source + index];
	return true;
}

static void put_le16(dos_linear_address_t address, uint16_t value)
{
	guest_memory[address] = (uint8_t)value;
	guest_memory[address + 1u] = (uint8_t)(value >> 8);
}

static uint16_t get_le16(const uint8_t *bytes, size_t offset)
{
	return (uint16_t)bytes[offset] | ((uint16_t)bytes[offset + 1u] << 8);
}

static int test_initial_psp(void)
{
	struct dos_process_initial_psp_request request = {
		.psp_segment = TEST_CHILD_SEGMENT,
		.block_end_segment = 0x2100u,
		.environment_segment = 0x3000u,
		.terminate_vector = {.segment = 0x1111u, .offset = 0x2222u},
		.control_c_vector = {.segment = 0x3333u, .offset = 0x4444u},
		.critical_error_vector = {
			.segment = 0x5555u,
			.offset = 0x6666u,
		},
	};
	struct dos_process_psp_image image;
	struct dos_process_psp_image unchanged;
	size_t index;

	clear_bytes((uint8_t *)(void *)&image, sizeof(image), 0xa5u);
	if (dos_process_prepare_initial_psp(&request, &image) != DOS_PROCESS_OK)
		return 1;
	/* The initial process maps stdin, stdout, and stderr to SFT 0. */
	if (image.segment != TEST_CHILD_SEGMENT ||
	    get_le16(image.bytes, 0u) != 0x20cdu ||
	    get_le16(image.bytes, 2u) != 0x2100u || image.bytes[5] != 0x9au ||
	    get_le16(image.bytes, 6u) != 0x0f00u ||
	    get_le16(image.bytes, 8u) != 0xff1cu ||
	    get_le16(image.bytes, 0x0au) != 0x2222u ||
	    get_le16(image.bytes, 0x0cu) != 0x1111u ||
	    get_le16(image.bytes, 0x0eu) != 0x4444u ||
	    get_le16(image.bytes, 0x10u) != 0x3333u ||
	    get_le16(image.bytes, 0x12u) != 0x6666u ||
	    get_le16(image.bytes, 0x14u) != 0x5555u ||
	    get_le16(image.bytes, TEST_PSP_PARENT_OFFSET) !=
		TEST_CHILD_SEGMENT ||
	    image.bytes[TEST_PSP_JFT_OFFSET] != 0u ||
	    image.bytes[TEST_PSP_JFT_OFFSET + 1u] != 0u ||
	    image.bytes[TEST_PSP_JFT_OFFSET + 2u] != 0u)
		return 2;
	for (index = 3u; index < DOS_PSP_DEFAULT_HANDLES; ++index) {
		if (image.bytes[TEST_PSP_JFT_OFFSET + index] !=
		    DOS_JFT_ENTRY_UNUSED)
			return 3;
	}
	if (get_le16(image.bytes, TEST_PSP_ENVIRONMENT_OFFSET) != 0x3000u ||
	    get_le16(image.bytes, TEST_PSP_JFT_LENGTH_OFFSET) !=
		DOS_PSP_DEFAULT_HANDLES ||
	    get_le16(image.bytes, TEST_PSP_JFT_POINTER_OFFSET) !=
		TEST_PSP_JFT_OFFSET ||
	    get_le16(image.bytes, TEST_PSP_JFT_POINTER_OFFSET + 2u) !=
		TEST_CHILD_SEGMENT ||
	    image.bytes[TEST_PSP_NEXT_OFFSET] != 0xffu ||
	    image.bytes[TEST_PSP_NEXT_OFFSET + 1u] != 0xffu ||
	    image.bytes[TEST_PSP_NEXT_OFFSET + 2u] != 0xffu ||
	    image.bytes[TEST_PSP_NEXT_OFFSET + 3u] != 0xffu ||
	    image.bytes[TEST_PSP_SYSTEM_CALL_OFFSET] != 0xcdu ||
	    image.bytes[TEST_PSP_SYSTEM_CALL_OFFSET + 1u] != 0x21u ||
	    image.bytes[TEST_PSP_SYSTEM_CALL_OFFSET + 2u] != 0xcbu ||
	    image.bytes[DOS_PSP_COMMAND_TAIL_OFFSET] != 0u ||
	    image.bytes[DOS_PSP_COMMAND_TAIL_OFFSET + 1u] != 0x0du)
		return 4;

	unchanged = image;
	request.block_end_segment =
		(uint16_t)(request.psp_segment + 0x0fu);
	if (dos_process_prepare_initial_psp(&request, &image) !=
		DOS_PROCESS_INVALID_PSP ||
	    !bytes_are_equal(&image, &unchanged, sizeof(image)) ||
	    dos_process_prepare_initial_psp(NULL, &image) !=
		DOS_PROCESS_INVALID_ARGUMENT)
		return 5;
	return 0;
}

static enum dos_machine_status
test_read_memory(kernel_object_handle_t context, dos_linear_address_t address,
		 void *destination, size_t destination_capacity, size_t count)
{
	uint8_t *output = (uint8_t *)destination;
	size_t index;

	++read_calls;
	if (fail_next_read) {
		fail_next_read = false;
		return DOS_MACHINE_ADDRESS_FAULT;
	}
	if (context != 1u || count > destination_capacity ||
	    (uint64_t)address >= TEST_MEMORY_SIZE ||
	    (uint64_t)count > TEST_MEMORY_SIZE - (uint64_t)address)
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
	size_t index;
	size_t partial_count;

	++write_calls;
	if (context != 1u || count > source_capacity ||
	    (uint64_t)address >= TEST_MEMORY_SIZE ||
	    (uint64_t)count > TEST_MEMORY_SIZE - (uint64_t)address)
		return DOS_MACHINE_ADDRESS_FAULT;
	if (write_failures_remaining != 0u) {
		--write_failures_remaining;
		partial_count = first_failure_partial_bytes < count
				    ? first_failure_partial_bytes
				    : count;
		first_failure_partial_bytes = 0u;
		for (index = 0u; index < partial_count; ++index)
			guest_memory[address + index] = input[index];
		return DOS_MACHINE_ADDRESS_FAULT;
	}
	for (index = 0u; index < count; ++index)
		guest_memory[address + index] = input[index];
	last_write_address = address;
	last_write_count = count;
	return DOS_MACHINE_OK;
}

static void initialize_parent_and_exec_sources(void)
{
	dos_linear_address_t parent = test_linear(TEST_PARENT_SEGMENT, 0u);
	dos_linear_address_t jft = test_linear(TEST_JFT_SEGMENT, 0x20u);
	dos_linear_address_t first = test_linear(TEST_SOURCE_SEGMENT, 0x100u);
	dos_linear_address_t second = test_linear(TEST_SOURCE_SEGMENT, 0x200u);
	dos_linear_address_t command = test_linear(TEST_SOURCE_SEGMENT, 0x300u);
	size_t index;

	clear_bytes(guest_memory, sizeof(guest_memory), 0u);
	clear_bytes(guest_memory + parent, DOS_PSP_SIZE, 0xccu);
	put_le16(parent + 0x32u, 5u);
	put_le16(parent + 0x34u, 0x20u);
	put_le16(parent + 0x36u, TEST_JFT_SEGMENT);
	guest_memory[jft] = 0u;
	guest_memory[jft + 1u] = 1u;
	guest_memory[jft + 2u] = 2u;
	guest_memory[jft + 3u] = 7u;
	guest_memory[jft + 4u] = 8u;
	for (index = 0u; index < DOS_PROCESS_FCB_PREFIX_BYTES; ++index) {
		guest_memory[first + index] = (uint8_t)(0x10u + index);
		guest_memory[second + index] = (uint8_t)(0x40u + index);
	}
	for (index = 0u; index < sizeof(struct dos_command_tail40); ++index)
		guest_memory[command + index] = (uint8_t)(0x80u ^ index);
	guest_memory[command] = 5u;
	guest_memory[command + 1u] = 'A';
	guest_memory[command + 2u] = 'B';
	guest_memory[command + 3u] = 0x1au;
	guest_memory[command + 4u] = 'X';
	guest_memory[command + 5u] = 'Y';
	guest_memory[command + 6u] = DOS_PROCESS_COMMAND_CR;
	read_calls = 0u;
	write_calls = 0u;
	last_write_address = 0u;
	last_write_count = 0u;
	fail_next_read = false;
	write_failures_remaining = 0u;
	first_failure_partial_bytes = 0u;
}

static struct dos_process_psp_request make_psp_request(void)
{
	struct dos_process_psp_request request = {
	    .psp_segment = TEST_CHILD_SEGMENT,
	    .block_end_segment = 0x2100u,
	    .parent_psp_segment = TEST_PARENT_SEGMENT,
	    .environment_segment = 0x3000u,
	    .terminate_vector = {.segment = 0x9000u, .offset = 0x1111u},
	    .control_c_vector = {.segment = 0x9001u, .offset = 0x2222u},
	    .critical_error_vector =
		{
		    .segment = 0x9002u,
		    .offset = 0x3333u,
		},
	    .command_tail_source =
		{
		    .segment = TEST_SOURCE_SEGMENT,
		    .offset = 0x300u,
		},
	    .first_fcb_source =
		{
		    .segment = TEST_SOURCE_SEGMENT,
		    .offset = 0x100u,
		},
	    .second_fcb_source =
		{
		    .segment = TEST_SOURCE_SEGMENT,
		    .offset = 0x200u,
		},
	    .inheritable_handle_mask = (1u << 0) | (1u << 2) | (1u << 4),
	};

	return request;
}

static bool psp_image_is_correct(const struct dos_process_psp_image *image)
{
	const uint8_t *bytes = image->bytes;
	size_t index;

	if (image->segment != TEST_CHILD_SEGMENT || bytes[0] != 0xcdu ||
	    bytes[1] != 0x20u || get_le16(bytes, 2u) != 0x2100u ||
	    bytes[4] != 0xccu || bytes[5] != 0x9au ||
	    get_le16(bytes, 6u) != 0x0f00u || get_le16(bytes, 8u) != 0xff1cu ||
	    get_le16(bytes, 0x0au) != 0x1111u ||
	    get_le16(bytes, 0x0cu) != 0x9000u ||
	    get_le16(bytes, 0x0eu) != 0x2222u ||
	    get_le16(bytes, 0x10u) != 0x9001u ||
	    get_le16(bytes, 0x12u) != 0x3333u ||
	    get_le16(bytes, 0x14u) != 0x9002u ||
	    get_le16(bytes, 0x16u) != TEST_PARENT_SEGMENT)
		return false;
	if (bytes[0x18u] != 0u || bytes[0x19u] != 0xffu || bytes[0x1au] != 2u ||
	    bytes[0x1bu] != 0xffu || bytes[0x1cu] != 8u)
		return false;
	for (index = 5u; index < DOS_PSP_DEFAULT_HANDLES; ++index) {
		if (bytes[0x18u + index] != 0xffu)
			return false;
	}
	if (get_le16(bytes, 0x2cu) != 0x3000u ||
	    get_le16(bytes, 0x32u) != DOS_PSP_DEFAULT_HANDLES ||
	    get_le16(bytes, 0x34u) != 0x18u ||
	    get_le16(bytes, 0x36u) != TEST_CHILD_SEGMENT ||
	    bytes[0x38u] != 0xffu || bytes[0x39u] != 0xffu ||
	    bytes[0x3au] != 0xffu || bytes[0x3bu] != 0xffu ||
	    bytes[0x50u] != 0xcdu || bytes[0x51u] != 0x21u ||
	    bytes[0x52u] != 0xcbu || bytes[0x53u] != 0xccu ||
	    bytes[0x54u] != 0xccu)
		return false;
	for (index = 0u; index < DOS_PROCESS_FCB_PREFIX_BYTES; ++index) {
		if (bytes[DOS_PSP_FIRST_FCB_OFFSET + index] !=
			(uint8_t)(0x10u + index) ||
		    bytes[DOS_PSP_SECOND_FCB_OFFSET + index] !=
			(uint8_t)(0x40u + index))
			return false;
	}
	for (index = 0u; index < 4u; ++index) {
		if (bytes[DOS_PSP_FIRST_FCB_OFFSET +
			  DOS_PROCESS_FCB_PREFIX_BYTES + index] != 0u ||
		    bytes[DOS_PSP_SECOND_FCB_OFFSET +
			  DOS_PROCESS_FCB_PREFIX_BYTES + index] != 0u)
			return false;
	}
	if (bytes[0x7cu] != 0xccu)
		return false;
	for (index = 0u; index < sizeof(struct dos_command_tail40); ++index) {
		if (bytes[DOS_PSP_COMMAND_TAIL_OFFSET + index] !=
		    guest_memory[test_linear(TEST_SOURCE_SEGMENT, 0x300u) +
				 index])
			return false;
	}
	return true;
}

static int test_command_tails(void)
{
	static const uint8_t line[] = {'A', 'B', 'C'};
	static const uint8_t control_z_line[] = {'D', 'E', 0x1au, 'X'};
	static const uint8_t malformed_line[] = {'A', '\r', 'B'};
	uint8_t maximum_line[126];
	uint8_t excessive_line[127];
	struct dos_command_tail40 tail;
	size_t index;

	if (dos_process_encode_command_tail(line, sizeof(line), sizeof(line),
					    &tail) != DOS_PROCESS_OK ||
	    tail.length != 3u || tail.data[0] != 'A' || tail.data[2] != 'C' ||
	    tail.data[3] != DOS_PROCESS_COMMAND_CR)
		return 1;
	/* ^Z is ordinary PSP data; COMMAND consumes batch EOF before EXEC. */
	if (dos_process_encode_command_tail(
		control_z_line, sizeof(control_z_line), sizeof(control_z_line),
		&tail) != DOS_PROCESS_OK ||
	    tail.length != 4u || tail.data[2] != 0x1au ||
	    tail.data[4] != DOS_PROCESS_COMMAND_CR)
		return 2;
	for (index = 0u; index < sizeof(maximum_line); ++index)
		maximum_line[index] = 'Q';
	if (dos_process_encode_command_tail(maximum_line, sizeof(maximum_line),
					    sizeof(maximum_line),
					    &tail) != DOS_PROCESS_OK ||
	    tail.length != 126u || tail.data[126] != DOS_PROCESS_COMMAND_CR)
		return 3;
	for (index = 0u; index < sizeof(excessive_line); ++index)
		excessive_line[index] = 'R';
	tail.length = 0xa5u;
	if (dos_process_encode_command_tail(
		excessive_line, sizeof(excessive_line), sizeof(excessive_line),
		&tail) != DOS_PROCESS_COMMAND_TAIL_TOO_LONG ||
	    tail.length != 0xa5u)
		return 4;
	if (dos_process_encode_command_tail(
		malformed_line, sizeof(malformed_line), sizeof(malformed_line),
		&tail) != DOS_PROCESS_BAD_COMMAND_TAIL ||
	    tail.length != 0xa5u)
		return 5;
	if (dos_process_encode_command_tail(NULL, 0u, 0u, &tail) !=
		DOS_PROCESS_OK ||
	    tail.length != 0u || tail.data[0] != DOS_PROCESS_COMMAND_CR)
		return 6;
	if (dos_process_encode_command_tail(line, 2u, sizeof(line), &tail) !=
	    DOS_PROCESS_INVALID_ARGUMENT)
		return 7;
	return 0;
}

static int test_psp(const struct dos_machine *machine)
{
	struct dos_machine a20_machine;
	struct dos_process_psp_request request = make_psp_request();
	struct dos_process_psp_image image;
	dos_linear_address_t child = test_linear(TEST_CHILD_SEGMENT, 0u);
	dos_linear_address_t parent = test_linear(TEST_PARENT_SEGMENT, 0u);
	size_t index;

	initialize_parent_and_exec_sources();
	if (dos_process_prepare_psp(machine, &request, &image) !=
		DOS_PROCESS_OK ||
	    write_calls != 0u || !psp_image_is_correct(&image))
		return 1;
	if (dos_process_commit_psp(machine, &image) != DOS_PROCESS_OK ||
	    write_calls != 1u || last_write_address != child ||
	    last_write_count != DOS_PSP_SIZE)
		return 2;
	for (index = 0u; index < DOS_PSP_SIZE; ++index) {
		if (guest_memory[child + index] != image.bytes[index])
			return 3;
	}
	/* The source parent is read-only throughout preparation and commit. */
	if (guest_memory[parent + 4u] != 0xccu ||
	    get_le16(guest_memory + parent, 0x32u) != 5u)
		return 4;

	/* EXEC accepts a raw 128-byte tail; only the helper above enforces
	 * COMMAND's canonical format. */
	initialize_parent_and_exec_sources();
	guest_memory[test_linear(TEST_SOURCE_SEGMENT, 0x300u)] = 127u;
	guest_memory[test_linear(TEST_SOURCE_SEGMENT, 0x300u) + 1u] = '\r';
	image.segment = 0xdeadu;
	image.bytes[0] = 0xa5u;
	if (dos_process_prepare_psp(machine, &request, &image) !=
		DOS_PROCESS_OK ||
	    image.segment != TEST_CHILD_SEGMENT || image.bytes[0x80u] != 127u ||
	    image.bytes[0x81u] != '\r' || write_calls != 0u)
		return 5;

	request.block_end_segment = request.psp_segment;
	if (dos_process_build_psp(machine, &request) !=
		DOS_PROCESS_INVALID_PSP ||
	    write_calls != 0u)
		return 6;
	request = make_psp_request();
	request.inheritable_handle_mask = 1u << DOS_PSP_DEFAULT_HANDLES;
	if (dos_process_build_psp(machine, &request) !=
		DOS_PROCESS_INVALID_ARGUMENT ||
	    write_calls != 0u)
		return 7;

	/* All fixed ranges are checked before the first guest read.  With A20
	 * disabled, EXEC's raw 128-byte copy wraps physical byte 100000h to 0.
	 */
	initialize_parent_and_exec_sources();
	request = make_psp_request();
	request.command_tail_source.segment = 0xfff0u;
	request.command_tail_source.offset = 0x0081u;
	for (index = 0u; index < 127u; ++index)
		guest_memory[0x000fff81u + index] =
		    (uint8_t)(0xa0u ^ (uint8_t)index);
	guest_memory[0] = (uint8_t)(0xa0u ^ 127u);
	if (dos_process_prepare_psp(machine, &request, &image) !=
		DOS_PROCESS_OK ||
	    write_calls != 0u)
		return 8;
	for (index = 0u; index < sizeof(struct dos_command_tail40); ++index) {
		if (image.bytes[DOS_PSP_COMMAND_TAIL_OFFSET + index] !=
		    (uint8_t)(0xa0u ^ (uint8_t)index))
			return 9;
	}

	/* The same range is outside this backend when A20 is enabled.  The
	 * complete fixed-range preflight fails before any guest callback. */
	initialize_parent_and_exec_sources();
	request = make_psp_request();
	request.command_tail_source.segment = 0xfff0u;
	request.command_tail_source.offset = 0x0081u;
	a20_machine = *machine;
	a20_machine.a20_enabled = true;
	image.segment = 0x5a5au;
	if (dos_process_prepare_psp(&a20_machine, &request, &image) !=
		DOS_PROCESS_MACHINE_FAULT ||
	    read_calls != 0u || write_calls != 0u || image.segment != 0x5a5au)
		return 10;

	initialize_parent_and_exec_sources();
	request = make_psp_request();
	image.segment = 0xbeefu;
	fail_next_read = true;
	if (dos_process_prepare_psp(machine, &request, &image) !=
		DOS_PROCESS_MACHINE_FAULT ||
	    image.segment != 0xbeefu || write_calls != 0u)
		return 11;

	/* The parent JFT pointer is decoded only after the parent PSP is read.
	 * Its 20-byte source may independently wrap at the A20 boundary.
	 */
	initialize_parent_and_exec_sources();
	request = make_psp_request();
	put_le16(parent + 0x32u, DOS_PSP_DEFAULT_HANDLES);
	put_le16(parent + 0x34u, 0x00f8u);
	put_le16(parent + 0x36u, 0xfff0u);
	request.inheritable_handle_mask = DOS_PROCESS_INHERITABLE_HANDLE_MASK;
	for (index = 0u; index < 8u; ++index)
		guest_memory[0x000ffff8u + index] = (uint8_t)(0x60u + index);
	for (index = 8u; index < DOS_PSP_DEFAULT_HANDLES; ++index)
		guest_memory[index - 8u] = (uint8_t)(0x60u + index);
	if (dos_process_prepare_psp(machine, &request, &image) !=
		DOS_PROCESS_OK ||
	    write_calls != 0u)
		return 12;
	for (index = 0u; index < DOS_PSP_DEFAULT_HANDLES; ++index) {
		if (image.bytes[0x18u + index] != (uint8_t)(0x60u + index))
			return 13;
	}

	/* A partial replacement is rolled back to the complete old PSP. */
	initialize_parent_and_exec_sources();
	request = make_psp_request();
	if (dos_process_prepare_psp(machine, &request, &image) !=
	    DOS_PROCESS_OK)
		return 14;
	write_failures_remaining = 1u;
	first_failure_partial_bytes = 37u;
	if (dos_process_commit_psp(machine, &image) !=
		DOS_PROCESS_MACHINE_FAULT ||
	    write_calls != 2u)
		return 15;
	for (index = 0u; index < DOS_PSP_SIZE; ++index) {
		if (guest_memory[child + index] != 0u)
			return 16;
	}

	/* A failed rollback is surfaced and never mistaken for a clean commit.
	 */
	initialize_parent_and_exec_sources();
	request = make_psp_request();
	if (dos_process_prepare_psp(machine, &request, &image) !=
	    DOS_PROCESS_OK)
		return 17;
	write_failures_remaining = 2u;
	first_failure_partial_bytes = 37u;
	if (dos_process_commit_psp(machine, &image) !=
		DOS_PROCESS_MACHINE_POISONED ||
	    write_calls != 2u || guest_memory[child] != image.bytes[0] ||
	    guest_memory[child + 37u] != 0u)
		return 18;

	initialize_parent_and_exec_sources();
	request = make_psp_request();
	for (index = 0u; index < sizeof(struct dos_command_tail40); ++index)
		guest_memory[test_linear(TEST_SOURCE_SEGMENT, 0x300u) + index] =
		    (uint8_t)(index ^ 0x5au);
	if (dos_process_prepare_psp(machine, &request, &image) !=
		DOS_PROCESS_OK ||
	    write_calls != 0u)
		return 19;
	for (index = 0u; index < sizeof(struct dos_command_tail40); ++index) {
		if (image.bytes[DOS_PSP_COMMAND_TAIL_OFFSET + index] !=
		    (uint8_t)(index ^ 0x5au))
			return 20;
	}

	/* A 16-bit REP wraps SI at 10000h while retaining the segment.  This is
	 * distinct from A20 and therefore succeeds in both A20 states. */
	initialize_parent_and_exec_sources();
	request = make_psp_request();
	request.command_tail_source.segment = 0x1300u;
	request.command_tail_source.offset = 0xfff0u;
	for (index = 0u; index < 16u; ++index)
		guest_memory[test_linear(0x1300u, 0xfff0u) + index] =
		    (uint8_t)(0x35u ^ (uint8_t)index);
	for (index = 16u; index < sizeof(struct dos_command_tail40); ++index)
		guest_memory[test_linear(0x1300u, 0u) + index - 16u] =
		    (uint8_t)(0x35u ^ (uint8_t)index);
	if (dos_process_prepare_psp(machine, &request, &image) !=
		DOS_PROCESS_OK ||
	    read_calls != 6u || write_calls != 0u)
		return 21;
	for (index = 0u; index < sizeof(struct dos_command_tail40); ++index) {
		if (image.bytes[DOS_PSP_COMMAND_TAIL_OFFSET + index] !=
		    (uint8_t)(0x35u ^ (uint8_t)index))
			return 22;
	}
	read_calls = 0u;
	write_calls = 0u;
	a20_machine = *machine;
	a20_machine.a20_enabled = true;
	if (dos_process_prepare_psp(&a20_machine, &request, &image) !=
		DOS_PROCESS_OK ||
	    read_calls != 6u || write_calls != 0u)
		return 23;

	/* DOS segments are values, not native pointers.  AH=50 can make a zero
	 * parent visible, and a staged PSP at segment zero remains addressable.
	 */
	initialize_parent_and_exec_sources();
	request = make_psp_request();
	request.parent_psp_segment = 0u;
	if (dos_process_prepare_psp(machine, &request, &image) !=
		DOS_PROCESS_OK ||
	    get_le16(image.bytes, 0x16u) != 0u)
		return 24;
	initialize_parent_and_exec_sources();
	request = make_psp_request();
	request.psp_segment = 0u;
	request.block_end_segment = 0x0100u;
	if (dos_process_prepare_psp(machine, &request, &image) !=
		DOS_PROCESS_OK ||
	    image.segment != 0u ||
	    dos_process_commit_psp(machine, &image) != DOS_PROCESS_OK ||
	    last_write_address != 0u || last_write_count != DOS_PSP_SIZE)
		return 25;
	return 0;
}

static bool psp_image_matches(const struct dos_process_psp_image *image,
			      uint16_t expected_segment,
			      const uint8_t expected[DOS_PSP_SIZE])
{
	size_t index;

	if (image->segment != expected_segment)
		return false;
	for (index = 0u; index < DOS_PSP_SIZE; ++index) {
		if (image->bytes[index] != expected[index])
			return false;
	}
	return true;
}

static int test_exact_child_jft(const struct dos_machine *machine)
{
	struct dos_process_psp_request request;
	struct dos_process_psp_image image;
	struct dos_sft_jft20 child_jft;
	uint8_t snapshot[DOS_PSP_SIZE];
	uint16_t snapshot_segment;
	uint32_t reads_after_prepare;
	size_t index;

	initialize_parent_and_exec_sources();
	request = make_psp_request();
	if (dos_process_prepare_psp(machine, &request, &image) !=
	    DOS_PROCESS_OK)
		return 1;
	for (index = 0u; index < DOS_SFT_BATCH_JFT_ENTRIES; ++index)
		child_jft.entries[index] = (uint8_t)(0x40u + index);
	reads_after_prepare = read_calls;
	if (dos_process_psp_set_jft20(&image, &child_jft) != DOS_PROCESS_OK ||
	    read_calls != reads_after_prepare || write_calls != 0u)
		return 2;
	for (index = 0u; index < DOS_SFT_BATCH_JFT_ENTRIES; ++index) {
		if (image.bytes[0x18u + index] != child_jft.entries[index])
			return 3;
	}
	if (dos_process_psp_set_jft20(NULL, &child_jft) !=
		DOS_PROCESS_INVALID_ARGUMENT ||
	    dos_process_psp_set_jft20(&image, NULL) !=
		DOS_PROCESS_INVALID_ARGUMENT ||
	    read_calls != reads_after_prepare || write_calls != 0u)
		return 4;

	/* Reject a stale or forged staging image before changing any JFT byte.
	 */
	image.bytes[0x32u] = DOS_PSP_DEFAULT_HANDLES - 1u;
	snapshot_segment = image.segment;
	for (index = 0u; index < DOS_PSP_SIZE; ++index)
		snapshot[index] = image.bytes[index];
	if (dos_process_psp_set_jft20(&image, &child_jft) !=
		DOS_PROCESS_INVALID_PSP ||
	    !psp_image_matches(&image, snapshot_segment, snapshot))
		return 5;

	initialize_parent_and_exec_sources();
	request = make_psp_request();
	if (dos_process_prepare_psp(machine, &request, &image) !=
	    DOS_PROCESS_OK)
		return 6;
	image.bytes[0x34u] = 0x19u;
	snapshot_segment = image.segment;
	for (index = 0u; index < DOS_PSP_SIZE; ++index)
		snapshot[index] = image.bytes[index];
	if (dos_process_psp_set_jft20(&image, &child_jft) !=
		DOS_PROCESS_INVALID_PSP ||
	    !psp_image_matches(&image, snapshot_segment, snapshot))
		return 7;

	initialize_parent_and_exec_sources();
	request = make_psp_request();
	if (dos_process_prepare_psp(machine, &request, &image) !=
	    DOS_PROCESS_OK)
		return 8;
	image.bytes[0x36u] ^= 1u;
	snapshot_segment = image.segment;
	for (index = 0u; index < DOS_PSP_SIZE; ++index)
		snapshot[index] = image.bytes[index];
	if (dos_process_psp_set_jft20(&image, &child_jft) !=
		DOS_PROCESS_INVALID_PSP ||
	    !psp_image_matches(&image, snapshot_segment, snapshot))
		return 9;

	/* Segment zero remains an ordinary DOS value, including in JFT far
	 * pointers; it is never interpreted as native NULL. */
	initialize_parent_and_exec_sources();
	request = make_psp_request();
	request.psp_segment = 0u;
	request.block_end_segment = 0x0100u;
	if (dos_process_prepare_psp(machine, &request, &image) !=
		DOS_PROCESS_OK ||
	    dos_process_psp_set_jft20(&image, &child_jft) != DOS_PROCESS_OK)
		return 10;
	for (index = 0u; index < DOS_SFT_BATCH_JFT_ENTRIES; ++index) {
		if (image.bytes[0x18u + index] != child_jft.entries[index])
			return 11;
	}
	return 0;
}

static int test_parent_snapshot(const struct dos_machine *machine)
{
	const kernel_object_handle_t machine_identity = 0x123456789abcdef0ull;
	struct dos_process_parent_snapshot snapshot;
	struct dos_process_parent_snapshot expected_snapshot;
	struct dos_process_psp_request request;
	struct dos_process_psp_image image;
	struct dos_process_psp_image expected_image;
	struct dos_sft_jft20 child_jft;
	struct dos_machine other_machine;
	dos_linear_address_t parent = test_linear(TEST_PARENT_SEGMENT, 0u);
	dos_linear_address_t parent_jft = test_linear(TEST_JFT_SEGMENT, 0x20u);
	uint32_t reads_after_capture;
	size_t index;

	initialize_parent_and_exec_sources();
	clear_bytes((uint8_t *)(void *)&snapshot, sizeof(snapshot), 0xa5u);
	if (dos_process_capture_parent_snapshot(machine, machine_identity,
						TEST_PARENT_SEGMENT,
						&snapshot) != DOS_PROCESS_OK ||
	    read_calls != 2u || write_calls != 0u ||
	    snapshot.machine_identity != machine_identity ||
	    snapshot.machine_context != machine->context ||
	    snapshot.machine_address_limit != machine->address_limit ||
	    snapshot.parent_psp_segment != TEST_PARENT_SEGMENT ||
	    snapshot.a20_enabled != 0u || snapshot.captured != 1u ||
	    snapshot.parent_psp[4] != 0xccu)
		return 1;
	for (index = 0u; index < DOS_PSP_DEFAULT_HANDLES; ++index) {
		uint8_t expected = index < 5u ? guest_memory[parent_jft + index]
					      : DOS_JFT_ENTRY_UNUSED;

		if (snapshot.parent_jft.entries[index] != expected)
			return 2;
	}

	/* The exact SFT result, not a mask-derived approximation, is installed.
	 */
	for (index = 0u; index < DOS_SFT_BATCH_JFT_ENTRIES; ++index)
		child_jft.entries[index] = (uint8_t)(0x40u + index);
	request = make_psp_request();
	request.inheritable_handle_mask = 0u;
	guest_memory[parent + 4u] = 0x11u;
	guest_memory[parent_jft] = 0xeeu;
	reads_after_capture = read_calls;
	if (dos_process_prepare_psp_from_snapshot(
		machine, machine_identity, &snapshot, &request, &child_jft,
		&image) != DOS_PROCESS_OK ||
	    read_calls != reads_after_capture + 3u || write_calls != 0u ||
	    image.bytes[4] != 0xccu)
		return 3;
	for (index = 0u; index < DOS_SFT_BATCH_JFT_ENTRIES; ++index) {
		if (image.bytes[0x18u + index] != child_jft.entries[index])
			return 4;
	}

	/* Binding errors are pure: no guest read and no partial image update.
	 */
	expected_image = image;
	if (dos_process_prepare_psp_from_snapshot(
		machine, machine_identity + 1u, &snapshot, &request, &child_jft,
		&image) != DOS_PROCESS_STALE_SNAPSHOT ||
	    read_calls != reads_after_capture + 3u ||
	    !bytes_are_equal(&image, &expected_image, sizeof(image)))
		return 5;
	request.parent_psp_segment = (uint16_t)(TEST_PARENT_SEGMENT + 1u);
	if (dos_process_prepare_psp_from_snapshot(
		machine, machine_identity, &snapshot, &request, &child_jft,
		&image) != DOS_PROCESS_STALE_SNAPSHOT ||
	    read_calls != reads_after_capture + 3u ||
	    !bytes_are_equal(&image, &expected_image, sizeof(image)))
		return 6;
	request.parent_psp_segment = TEST_PARENT_SEGMENT;
	other_machine = *machine;
	other_machine.a20_enabled = true;
	if (dos_process_prepare_psp_from_snapshot(
		&other_machine, machine_identity, &snapshot, &request,
		&child_jft, &image) != DOS_PROCESS_STALE_SNAPSHOT ||
	    read_calls != reads_after_capture + 3u ||
	    !bytes_are_equal(&image, &expected_image, sizeof(image)))
		return 7;
	snapshot.reserved[0] = 1u;
	if (dos_process_prepare_psp_from_snapshot(
		machine, machine_identity, &snapshot, &request, &child_jft,
		&image) != DOS_PROCESS_STALE_SNAPSHOT ||
	    read_calls != reads_after_capture + 3u ||
	    !bytes_are_equal(&image, &expected_image, sizeof(image)))
		return 8;
	snapshot.reserved[0] = 0u;

	/* A data-dependent JFT range fault leaves the caller's snapshot intact.
	 */
	initialize_parent_and_exec_sources();
	put_le16(parent + 0x32u, DOS_PSP_DEFAULT_HANDLES);
	put_le16(parent + 0x34u, 0x0100u);
	put_le16(parent + 0x36u, 0xfff0u);
	clear_bytes((uint8_t *)(void *)&snapshot, sizeof(snapshot), 0x5au);
	expected_snapshot = snapshot;
	other_machine = *machine;
	other_machine.a20_enabled = true;
	if (dos_process_capture_parent_snapshot(
		&other_machine, machine_identity, TEST_PARENT_SEGMENT,
		&snapshot) != DOS_PROCESS_MACHINE_FAULT ||
	    read_calls != 1u || write_calls != 0u ||
	    !bytes_are_equal(&snapshot, &expected_snapshot, sizeof(snapshot)))
		return 9;

	/* Invalid native identities fail before a guest callback. */
	initialize_parent_and_exec_sources();
	clear_bytes((uint8_t *)(void *)&snapshot, sizeof(snapshot), 0x3cu);
	expected_snapshot = snapshot;
	if (dos_process_capture_parent_snapshot(
		machine, KERNEL_OBJECT_HANDLE_INVALID, TEST_PARENT_SEGMENT,
		&snapshot) != DOS_PROCESS_INVALID_ARGUMENT ||
	    read_calls != 0u ||
	    !bytes_are_equal(&snapshot, &expected_snapshot, sizeof(snapshot)))
		return 10;

	/* A DOS parent at segment zero is a value, not native NULL. */
	if (dos_process_capture_parent_snapshot(machine, machine_identity, 0u,
						&snapshot) != DOS_PROCESS_OK ||
	    snapshot.parent_psp_segment != 0u || snapshot.captured != 1u ||
	    read_calls != 1u)
		return 11;
	return 0;
}

static int test_psp_source_aliasing(const struct dos_machine *machine)
{
	const kernel_object_handle_t machine_identity = 0x2468ace013579bdfull;
	struct dos_process_parent_snapshot snapshot;
	struct dos_process_psp_request request;
	struct dos_process_psp_image image;
	struct dos_process_psp_image expected_image;
	struct dos_sft_jft20 child_jft;
	uint8_t expected_psp[DOS_PSP_SIZE];
	dos_linear_address_t first_source;
	dos_linear_address_t second_source;
	uint32_t reads_after_capture;
	size_t index;

	/*
	 * The exact-JFT path ignores the compatibility wrapper's inheritance
	 * mask.  FCB1 directly names the staged child JFT, not stale guest RAM.
	 */
	initialize_parent_and_exec_sources();
	if (dos_process_capture_parent_snapshot(machine, machine_identity,
						TEST_PARENT_SEGMENT,
						&snapshot) != DOS_PROCESS_OK)
		return 1;
	for (index = 0u; index < DOS_SFT_BATCH_JFT_ENTRIES; ++index)
		child_jft.entries[index] = (uint8_t)(0x60u + index);
	request = make_psp_request();
	request.inheritable_handle_mask = 0xffffffffu;
	request.first_fcb_source.segment = TEST_CHILD_SEGMENT;
	request.first_fcb_source.offset = TEST_PSP_JFT_OFFSET;
	reads_after_capture = read_calls;
	if (dos_process_prepare_psp_from_snapshot(
		machine, machine_identity, &snapshot, &request, &child_jft,
		&image) != DOS_PROCESS_OK ||
	    read_calls != reads_after_capture + 2u || write_calls != 0u)
		return 2;
	for (index = 0u; index < DOS_SFT_BATCH_JFT_ENTRIES; ++index) {
		if (image.bytes[TEST_PSP_JFT_OFFSET + index] !=
		    child_jft.entries[index])
			return 3;
	}
	for (index = 0u; index < DOS_PROCESS_FCB_PREFIX_BYTES; ++index) {
		if (image.bytes[DOS_PSP_FIRST_FCB_OFFSET + index] !=
		    child_jft.entries[index])
			return 4;
	}

	/* EXEC writes and clears FCB1 before it resolves FCB2's source bytes.
	 */
	initialize_parent_and_exec_sources();
	if (dos_process_capture_parent_snapshot(machine, machine_identity,
						TEST_PARENT_SEGMENT,
						&snapshot) != DOS_PROCESS_OK)
		return 5;
	for (index = 0u; index < DOS_SFT_BATCH_JFT_ENTRIES; ++index)
		child_jft.entries[index] = (uint8_t)(0x70u + index);
	request = make_psp_request();
	request.second_fcb_source.segment = TEST_CHILD_SEGMENT;
	request.second_fcb_source.offset = DOS_PSP_FIRST_FCB_OFFSET;
	first_source = test_linear(TEST_SOURCE_SEGMENT, 0x100u);
	reads_after_capture = read_calls;
	if (dos_process_prepare_psp_from_snapshot(
		machine, machine_identity, &snapshot, &request, &child_jft,
		&image) != DOS_PROCESS_OK ||
	    read_calls != reads_after_capture + 2u || write_calls != 0u)
		return 6;
	for (index = 0u; index < DOS_PROCESS_FCB_PREFIX_BYTES; ++index) {
		uint8_t expected = guest_memory[first_source + index];

		if (image.bytes[DOS_PSP_FIRST_FCB_OFFSET + index] != expected ||
		    image.bytes[DOS_PSP_SECOND_FCB_OFFSET + index] != expected)
			return 7;
	}

	/*
	 * The tail starts 16 bytes after its source.  Seed only the bytes made
	 * visible by the preceding FCB2 copy/clear and parent PSP copy, then
	 * let the independent bytewise model generate all 128 destination
	 * bytes.
	 */
	initialize_parent_and_exec_sources();
	if (dos_process_capture_parent_snapshot(machine, machine_identity,
						TEST_PARENT_SEGMENT,
						&snapshot) != DOS_PROCESS_OK)
		return 8;
	for (index = 0u; index < DOS_SFT_BATCH_JFT_ENTRIES; ++index)
		child_jft.entries[index] = (uint8_t)(0x80u + index);
	request = make_psp_request();
	request.command_tail_source.segment = TEST_CHILD_SEGMENT;
	request.command_tail_source.offset = 0x70u;
	clear_bytes(expected_psp, sizeof(expected_psp), 0u);
	second_source = test_linear(TEST_SOURCE_SEGMENT, 0x200u);
	for (index = 0u; index < 8u; ++index)
		expected_psp[0x70u + index] =
		    guest_memory[second_source + 4u + index];
	for (index = 0u; index < 4u; ++index)
		expected_psp[0x7cu + index] =
		    snapshot.parent_psp[0x7cu + index];
	if (!model_forward_copy(expected_psp, sizeof(expected_psp),
				DOS_PSP_COMMAND_TAIL_OFFSET, 0x70u,
				sizeof(struct dos_command_tail40)))
		return 9;
	reads_after_capture = read_calls;
	if (dos_process_prepare_psp_from_snapshot(
		machine, machine_identity, &snapshot, &request, &child_jft,
		&image) != DOS_PROCESS_OK ||
	    read_calls != reads_after_capture + 2u || write_calls != 0u)
		return 10;
	if (!bytes_are_equal(image.bytes + 0x70u, expected_psp + 0x70u,
			     DOS_PSP_SIZE - 0x70u))
		return 11;

	/* ffff:0028 aliases the segment-zero child's JFT only when A20 is off.
	 */
	initialize_parent_and_exec_sources();
	if (dos_process_capture_parent_snapshot(machine, machine_identity,
						TEST_PARENT_SEGMENT,
						&snapshot) != DOS_PROCESS_OK)
		return 12;
	for (index = 0u; index < DOS_SFT_BATCH_JFT_ENTRIES; ++index)
		child_jft.entries[index] = (uint8_t)(0x90u + index);
	request = make_psp_request();
	request.psp_segment = 0u;
	request.block_end_segment = 0x0100u;
	request.first_fcb_source.segment = 0xffffu;
	request.first_fcb_source.offset = 0x0028u;
	reads_after_capture = read_calls;
	if (dos_process_prepare_psp_from_snapshot(
		machine, machine_identity, &snapshot, &request, &child_jft,
		&image) != DOS_PROCESS_OK ||
	    image.segment != 0u || read_calls != reads_after_capture + 2u ||
	    write_calls != 0u)
		return 13;
	for (index = 0u; index < DOS_PROCESS_FCB_PREFIX_BYTES; ++index) {
		if (image.bytes[DOS_PSP_FIRST_FCB_OFFSET + index] !=
		    child_jft.entries[index])
			return 14;
	}

	/*
	 * This tail starts eight bytes before the child PSP and then aliases
	 * it. Its first byte therefore performs a guest read after both FCBs
	 * have changed local staging.  Failure must publish neither image nor
	 * RAM.
	 */
	initialize_parent_and_exec_sources();
	if (dos_process_capture_parent_snapshot(machine, machine_identity,
						TEST_PARENT_SEGMENT,
						&snapshot) != DOS_PROCESS_OK)
		return 15;
	for (index = 0u; index < DOS_SFT_BATCH_JFT_ENTRIES; ++index)
		child_jft.entries[index] = (uint8_t)(0xa0u + index);
	request = make_psp_request();
	request.first_fcb_source.segment = TEST_CHILD_SEGMENT;
	request.first_fcb_source.offset = TEST_PSP_JFT_OFFSET;
	request.second_fcb_source.segment = TEST_CHILD_SEGMENT;
	request.second_fcb_source.offset = DOS_PSP_FIRST_FCB_OFFSET;
	request.command_tail_source.segment = 0x1fffu;
	request.command_tail_source.offset = 0x0008u;
	clear_bytes((uint8_t *)(void *)&image, sizeof(image), 0x5au);
	expected_image = image;
	reads_after_capture = read_calls;
	fail_next_read = true;
	if (dos_process_prepare_psp_from_snapshot(
		machine, machine_identity, &snapshot, &request, &child_jft,
		&image) != DOS_PROCESS_MACHINE_FAULT ||
	    read_calls != reads_after_capture + 1u || write_calls != 0u ||
	    !bytes_are_equal(&image, &expected_image, sizeof(image)))
		return 16;
	return 0;
}

static enum dos_process_status
plan_com_for_test(const struct dos_load_plan *image, uint16_t psp_segment,
		  uint16_t available_paragraphs, uint16_t initial_ax,
		  struct dos_com_process_plan *plan)
{
	struct dos_process_allocation_plan allocation;
	enum dos_process_status status;

	status = dos_process_select_allocation(image, available_paragraphs,
					       &allocation);
	if (status != DOS_PROCESS_OK)
		return status;
	return dos_process_plan_com(image, &allocation, psp_segment,
				    DOS_PROCESS_LAUNCH_EXECUTE, initial_ax,
				    plan);
}

static enum dos_process_status
plan_mz_for_test(const struct dos_load_plan *image, uint16_t psp_segment,
		 uint16_t available_paragraphs,
		 enum dos_process_launch_mode launch_mode, uint16_t initial_ax,
		 struct dos_mz_process_plan *plan)
{
	struct dos_process_allocation_plan allocation;
	enum dos_process_status status;

	status = dos_process_select_allocation(image, available_paragraphs,
					       &allocation);
	if (status != DOS_PROCESS_OK)
		return status;
	return dos_process_plan_mz(image, &allocation, psp_segment, launch_mode,
				   initial_ax, plan);
}

static int test_allocation_plans(void)
{
	struct dos_load_plan com = {
	    .format = DOS_IMAGE_COM,
	    .file_size = 1u,
	    .image_size = 1u,
	    .minimum_image_paragraphs = 1u,
	    .initial_ip = 0x100u,
	};
	struct dos_load_plan mz = {
	    .format = DOS_IMAGE_MZ,
	    .file_size = 0x64u,
	    .image_file_offset = 0x20u,
	    .image_size = 0x1e0u,
	    .minimum_image_paragraphs = 0x1eu,
	    .minimum_extra_paragraphs = 1u,
	    .maximum_extra_paragraphs = 0x20u,
	    .initial_sp = 0x0200u,
	};
	struct dos_process_allocation_plan allocation;
	struct dos_mz_process_plan mz_plan;
	struct dos_com_process_plan com_plan;

	if (dos_process_select_allocation(&com, 0x100u, &allocation) !=
		DOS_PROCESS_OK ||
	    !dos_process_allocation_plan_has_valid_encoding(&allocation) ||
	    allocation.format != DOS_IMAGE_COM ||
	    allocation.available_paragraphs != 0x100u ||
	    allocation.block_paragraphs != 0x100u || allocation.load_high)
		return 1;
	if (dos_process_plan_com(&com, &allocation, 0u,
				 DOS_PROCESS_LAUNCH_EXECUTE, 0u,
				 &com_plan) != DOS_PROCESS_OK ||
	    com_plan.psp_segment != 0u || com_plan.load_segment != 0x10u)
		return 2;
	allocation.reserved = 1u;
	com_plan.load_segment = 0x5a5au;
	if (dos_process_plan_com(&com, &allocation, 0u,
				 DOS_PROCESS_LAUNCH_EXECUTE, 0u,
				 &com_plan) != DOS_PROCESS_INVALID_ARGUMENT ||
	    com_plan.load_segment != 0x5a5au)
		return 10;
	allocation.reserved = 0u;
	com_plan.load_segment = 0x5a5au;
	if (dos_process_plan_com(&com, &allocation, 0xff80u,
				 DOS_PROCESS_LAUNCH_EXECUTE, 0u,
				 &com_plan) != DOS_PROCESS_RANGE_OVERFLOW ||
	    com_plan.load_segment != 0x5a5au)
		return 3;
	if (dos_process_plan_com(&com, &allocation, 0x2000u,
				 (enum dos_process_launch_mode)2u, 0u,
				 &com_plan) != DOS_PROCESS_INVALID_ARGUMENT)
		return 4;

	if (dos_process_select_allocation(&mz, 0x100u, &allocation) !=
		DOS_PROCESS_OK ||
	    !dos_process_allocation_plan_has_valid_encoding(&allocation) ||
	    allocation.block_paragraphs != 0x4eu || allocation.load_high)
		return 5;
	/* EXEC checks bare+min first, then independently requests bare+max;
	 * max below min is not a header-format rejection. */
	mz.minimum_extra_paragraphs = 0x20u;
	mz.maximum_extra_paragraphs = 1u;
	if (dos_process_select_allocation(&mz, 0x100u, &allocation) !=
		DOS_PROCESS_OK ||
	    allocation.block_paragraphs != 0x2fu)
		return 13;
	mz.minimum_extra_paragraphs = 1u;
	mz.maximum_extra_paragraphs = 0x20u;
	/* pages=1/header=ffe2h leaves 003eh resident paragraphs after the
	 * required 16-bit SUB, while the seek offset stays 000ffe20h. */
	mz.image_file_offset = 0xffe20u;
	mz.image_size = 0x3e0u;
	mz.minimum_image_paragraphs = 0x3eu;
	if (dos_process_select_allocation(&mz, 0x100u, &allocation) !=
		DOS_PROCESS_OK ||
	    allocation.block_paragraphs != 0x6eu)
		return 14;
	mz.image_file_offset = 0x20u;
	mz.image_size = 0x1e0u;
	mz.minimum_image_paragraphs = 0x1eu;
	/* max_extra=0 selects the largest block before EXEC checks min BSS. */
	mz.minimum_extra_paragraphs = 0xffffu;
	mz.maximum_extra_paragraphs = 0u;
	mz.load_high = true;
	if (dos_process_select_allocation(&mz, 0x2eu, &allocation) !=
		DOS_PROCESS_OK ||
	    allocation.block_paragraphs != 0x2eu || !allocation.load_high)
		return 6;
	/* A 16-bit bare+max carry follows EXEC's largest-block fallback. */
	mz.minimum_extra_paragraphs = 1u;
	mz.maximum_extra_paragraphs = 0xffffu;
	mz.load_high = false;
	if (dos_process_select_allocation(&mz, 0x100u, &allocation) !=
		DOS_PROCESS_OK ||
	    allocation.block_paragraphs != 0x100u || allocation.load_high)
		return 6;

	mz.maximum_extra_paragraphs = 0x20u;
	if (dos_process_select_allocation(&mz, 0x40u, &allocation) !=
		DOS_PROCESS_OK ||
	    allocation.block_paragraphs != 0x40u)
		return 7;
	allocation.block_paragraphs = 0x3fu;
	mz_plan.load_segment = 0x5a5au;
	if (dos_process_plan_mz(&mz, &allocation, 0x2000u,
				DOS_PROCESS_LAUNCH_EXECUTE, 0u,
				&mz_plan) != DOS_PROCESS_INVALID_ARGUMENT ||
	    mz_plan.load_segment != 0x5a5au)
		return 8;

	allocation.block_paragraphs = 0xa5a5u;
	if (dos_process_select_allocation(&mz, 0u, &allocation) !=
		DOS_PROCESS_NOT_ENOUGH_MEMORY ||
	    allocation.block_paragraphs != 0xa5a5u)
		return 9;
	com.target_kind = DOS_LOAD_TARGET_OVERLAY;
	if (dos_process_select_allocation(&com, 0x100u, &allocation) !=
		DOS_PROCESS_INVALID_ARGUMENT ||
	    allocation.block_paragraphs != 0xa5a5u)
		return 11;
	com.target_kind = DOS_LOAD_TARGET_PROCESS;
	com.reserved32 = 1u;
	if (dos_process_select_allocation(&com, 0x100u, &allocation) !=
		DOS_PROCESS_INVALID_ARGUMENT ||
	    allocation.block_paragraphs != 0xa5a5u)
		return 12;
	return 0;
}

static int test_com_plans(void)
{
	struct dos_process_allocation_plan allocation = {
	    .format = DOS_IMAGE_COM,
	    .available_paragraphs = 0x100u,
	    .block_paragraphs = 0x100u,
	};
	struct dos_load_plan image = {
	    .format = DOS_IMAGE_COM,
	    .file_size = 0x1000u,
	    .image_file_offset = 0u,
	    .image_size = 0x1000u,
	    .minimum_image_paragraphs = 0x100u,
	    .initial_ip = 0x100u,
	};
	struct dos_com_process_plan plan;
	uint16_t preserved_segment;

	if (plan_com_for_test(&image, 0x2000u, 0x1000u, 0x12ffu, &plan) !=
		DOS_PROCESS_OK ||
	    !dos_com_process_plan_has_valid_encoding(&plan) ||
	    plan.load_segment != 0x2010u || plan.load_offset != 0u ||
	    plan.load_linear_address != 0x20100u ||
	    plan.read_capacity != 0xff00u ||
	    plan.stack_sentinel_offset != 0xfffeu ||
	    plan.stack_sentinel_value != 0u ||
	    plan.load_only_stack_pointer != 0xfffcu ||
	    plan.load_only_stack_value != 0x12ffu ||
	    plan.launch_mode != DOS_PROCESS_LAUNCH_EXECUTE ||
	    plan.initial_state.eax != 0x12ffu ||
	    plan.initial_state.ebx != 0x12ffu ||
	    plan.initial_state.edx != 0x2000u ||
	    plan.initial_state.esi != 0x0100u ||
	    plan.initial_state.edi != 0xfffeu ||
	    plan.initial_state.cs != 0x2000u ||
	    plan.initial_state.ss != 0x2000u ||
	    plan.initial_state.ds != 0x2000u ||
	    plan.initial_state.es != 0x2000u ||
	    plan.initial_state.eip != 0x100u ||
	    plan.initial_state.esp != 0xfffeu ||
	    plan.initial_state.mode != DOS_CPU_REAL16)
		return 1;
	/* A COM exactly filling the bounded read is DOS's out-of-memory case.
	 */
	image.image_size = 0x0100u;
	image.file_size = 0x0100u;
	image.minimum_image_paragraphs = 0x10u;
	if (plan_com_for_test(&image, 0x2000u, 0x20u, 0u, &plan) !=
	    DOS_PROCESS_NOT_ENOUGH_MEMORY)
		return 2;
	image.image_size = 0x0100u;
	image.file_size = 0x0100u;
	if (plan_com_for_test(&image, 0x2000u, 0x100u, 0u, &plan) !=
		DOS_PROCESS_OK ||
	    plan.read_capacity != 0x0f00u ||
	    plan.stack_sentinel_offset != 0x0ffeu)
		return 3;
	image.format = DOS_IMAGE_MZ;
	if (dos_process_plan_com(&image, &allocation, 0x2000u,
				 DOS_PROCESS_LAUNCH_EXECUTE, 0u,
				 &plan) != DOS_PROCESS_WRONG_IMAGE_FORMAT)
		return 4;
	image.format = DOS_IMAGE_COM;
	image.initial_ip = 0u;
	preserved_segment = 0x5a5au;
	plan.load_segment = preserved_segment;
	if (plan_com_for_test(&image, 0x2000u, 0x100u, 0u, &plan) !=
		DOS_PROCESS_BAD_IMAGE_RANGE ||
	    plan.load_segment != preserved_segment)
		return 5;
	image.initial_ip = 0x100u;
	image.minimum_image_paragraphs = 0x11u;
	if (plan_com_for_test(&image, 0x2000u, 0x100u, 0u, &plan) !=
	    DOS_PROCESS_BAD_IMAGE_RANGE)
		return 6;
	image.minimum_image_paragraphs = 0x10u;
	image.image_size = 0xfeffu;
	image.file_size = 0xfeffu;
	image.minimum_image_paragraphs = 0xff0u;
	if (plan_com_for_test(&image, 0x2000u, 0x1000u, 0u, &plan) !=
		DOS_PROCESS_OK ||
	    plan.image_size != 0xfeffu)
		return 7;
	image.image_size = 0xff00u;
	image.file_size = 0xff00u;
	if (plan_com_for_test(&image, 0x2000u, 0x1000u, 0u, &plan) !=
	    DOS_PROCESS_BAD_IMAGE_RANGE)
		return 8;
	image.image_size = 1u;
	image.file_size = 1u;
	image.minimum_image_paragraphs = 1u;
	if (plan_com_for_test(&image, 0x2000u, 0x10u, 0u, &plan) !=
	    DOS_PROCESS_NOT_ENOUGH_MEMORY)
		return 9;
	return 0;
}

static int test_mz_plans(void)
{
	struct dos_process_allocation_plan allocation = {
	    .format = DOS_IMAGE_MZ,
	    .available_paragraphs = 0x200u,
	    .block_paragraphs = 0x200u,
	};
	struct dos_load_plan image = {
	    .format = DOS_IMAGE_MZ,
	    .file_size = 0x1274u,
	    .image_file_offset = 0x40u,
	    .image_size = 0x13c0u,
	    .minimum_image_paragraphs = 0x13cu,
	    .minimum_extra_paragraphs = 0x10u,
	    .maximum_extra_paragraphs = 0x20u,
	    .initial_cs = 2u,
	    .initial_ip = 0x100u,
	    .initial_ss = 0x20u,
	    .initial_sp = 0x0ff0u,
	    .relocation_count = 7u,
	    .relocation_table_offset = 0x1cu,
	    .load_high = false,
	};
	struct dos_load_plan short_image = {
	    .format = DOS_IMAGE_MZ,
	    .file_size = 0x64u,
	    .image_file_offset = 0x20u,
	    .image_size = 0x1e0u,
	    .minimum_image_paragraphs = 0x1eu,
	    .minimum_extra_paragraphs = 1u,
	    .maximum_extra_paragraphs = 0x20u,
	    .initial_cs = 0u,
	    .initial_ip = 0u,
	    .initial_ss = 0u,
	    .initial_sp = 0x0200u,
	    .relocation_count = 0u,
	    .relocation_table_offset = 0u,
	    .load_high = false,
	};
	struct dos_load_plan wrap_image;
	struct dos_mz_process_plan plan;
	uint16_t preserved_segment;

	if (plan_mz_for_test(&image, 0x2000u, 0x200u,
			     DOS_PROCESS_LAUNCH_EXECUTE, 0x34ffu,
			     &plan) != DOS_PROCESS_OK ||
	    !dos_mz_process_plan_has_valid_encoding(&plan) ||
	    plan.block_end_segment != 0x216cu || plan.load_segment != 0x2010u ||
	    plan.load_linear_address != 0x20100u ||
	    plan.image_file_offset != 0x40u || plan.image_size != 0x13c0u ||
	    plan.resident_paragraphs != 0x13cu ||
	    plan.relocation_factor != 0x2010u || plan.relocation_count != 7u ||
	    plan.relocation_table_offset != 0x1cu || plan.load_high ||
	    plan.load_only_stack_pointer != 0x0feeu ||
	    plan.load_only_stack_value != 0x34ffu ||
	    plan.launch_mode != DOS_PROCESS_LAUNCH_EXECUTE ||
	    plan.initial_state.eax != 0x34ffu ||
	    plan.initial_state.ebx != 0x34ffu ||
	    plan.initial_state.edx != 0x2000u ||
	    plan.initial_state.esi != 0x0100u ||
	    plan.initial_state.edi != 0x0ff0u ||
	    plan.initial_state.cs != 0x2012u ||
	    plan.initial_state.ss != 0x2030u ||
	    plan.initial_state.eip != 0x100u ||
	    plan.initial_state.esp != 0x0ff0u ||
	    plan.initial_state.ds != 0x2000u ||
	    plan.initial_state.es != 0x2000u)
		return 1;
	image.load_high = true;
	image.maximum_extra_paragraphs = 0u;
	if (plan_mz_for_test(&image, 0x2000u, 0x400u,
			     DOS_PROCESS_LAUNCH_EXECUTE, 0u,
			     &plan) != DOS_PROCESS_OK ||
	    !plan.load_high || plan.load_segment != 0x22c4u ||
	    plan.relocation_factor != 0x22c4u ||
	    plan.initial_state.cs != 0x22c6u ||
	    plan.initial_state.ss != 0x22e4u)
		return 2;
	image.load_high = false;
	image.maximum_extra_paragraphs = 0x20u;
	if (plan_mz_for_test(&image, 0x2000u, 0x100u,
			     DOS_PROCESS_LAUNCH_EXECUTE, 0u,
			     &plan) != DOS_PROCESS_NOT_ENOUGH_MEMORY)
		return 3;
	image.initial_cs = 0xf000u;
	/* The 16-bit CS relocation wraps, then the independent entry-range
	 * policy rejects the resulting address outside this allocation. */
	if (plan_mz_for_test(&image, 0x2000u, 0x200u,
			     DOS_PROCESS_LAUNCH_EXECUTE, 0u,
			     &plan) != DOS_PROCESS_BAD_IMAGE_RANGE)
		return 4;
	image.format = DOS_IMAGE_COM;
	if (dos_process_plan_mz(&image, &allocation, 0x2000u,
				DOS_PROCESS_LAUNCH_EXECUTE, 0u,
				&plan) != DOS_PROCESS_WRONG_IMAGE_FORMAT)
		return 5;

	/*
	 * A 100-byte MZ file with a 32-byte header has only 68 image bytes,
	 * but EXEC reserves the rest of its 512-byte page: 30 paragraphs.
	 */
	if (plan_mz_for_test(&short_image, 0x2000u, 0x40u,
			     DOS_PROCESS_LAUNCH_EXECUTE, 0u,
			     &plan) != DOS_PROCESS_OK ||
	    plan.image_size != 0x1e0u || plan.resident_paragraphs != 0x1eu ||
	    plan.load_segment != 0x2010u)
		return 6;
	if (plan_mz_for_test(&short_image, 0x2000u, 0x2eu,
			     DOS_PROCESS_LAUNCH_EXECUTE, 0u,
			     &plan) != DOS_PROCESS_NOT_ENOUGH_MEMORY)
		return 7;
	/* EXEC1 writes one word at SS:SP-2.  EXEC0 additionally needs
	 * SS:SP-4 for its PUSH/PUSH/RETF entry sequence. */
	short_image.initial_sp = 2u;
	if (plan_mz_for_test(&short_image, 0x2000u, 0x40u,
			     DOS_PROCESS_LAUNCH_EXECUTE, 0u,
			     &plan) != DOS_PROCESS_BAD_IMAGE_RANGE)
		return 8;
	if (plan_mz_for_test(&short_image, 0x2000u, 0x40u,
			     DOS_PROCESS_LAUNCH_LOAD_ONLY, 0x55u,
			     &plan) != DOS_PROCESS_OK ||
	    plan.launch_mode != DOS_PROCESS_LAUNCH_LOAD_ONLY ||
	    plan.load_only_stack_pointer != 0u ||
	    plan.load_only_stack_value != 0x55u)
		return 9;

	/* SP=0 is valid 16-bit state: EXEC's pushes target fffeH/fffcH. */
	short_image.initial_sp = 0u;
	short_image.maximum_extra_paragraphs = 0xffffu;
	if (plan_mz_for_test(&short_image, 0x1000u, 0x2000u,
			     DOS_PROCESS_LAUNCH_EXECUTE, 0x77u,
			     &plan) != DOS_PROCESS_OK ||
	    plan.initial_state.esp != 0u ||
	    plan.load_only_stack_pointer != 0xfffeu)
		return 10;
	short_image.initial_sp = 1u;
	if (plan_mz_for_test(&short_image, 0x1000u, 0x2000u,
			     DOS_PROCESS_LAUNCH_EXECUTE, 0u,
			     &plan) != DOS_PROCESS_BAD_IMAGE_RANGE)
		return 11;

	short_image.initial_sp = 0x0200u;
	short_image.maximum_extra_paragraphs = 0x20u;
	preserved_segment = 0x5a5au;
	plan.load_segment = preserved_segment;
	/* The loader has already read 26 bytes; EOF before the resident seek is
	 * still accepted later when the requested resident is under 512 bytes.
	 */
	short_image.file_size = 0x1au;
	if (plan_mz_for_test(&short_image, 0x2000u, 0x40u,
			     DOS_PROCESS_LAUNCH_EXECUTE, 0u,
			     &plan) != DOS_PROCESS_OK)
		return 12;
	short_image.file_size = 0x19u;
	plan.load_segment = preserved_segment;
	if (plan_mz_for_test(&short_image, 0x2000u, 0x40u,
			     DOS_PROCESS_LAUNCH_EXECUTE, 0u,
			     &plan) != DOS_PROCESS_BAD_IMAGE_RANGE ||
	    plan.load_segment != preserved_segment)
		return 13;
	short_image.file_size = 0x64u;
	short_image.minimum_image_paragraphs = 0x1du;
	if (plan_mz_for_test(&short_image, 0x2000u, 0x40u,
			     DOS_PROCESS_LAUNCH_EXECUTE, 0u,
			     &plan) != DOS_PROCESS_BAD_IMAGE_RANGE)
		return 14;
	short_image.minimum_image_paragraphs = 0x1eu;
	short_image.relocation_count = 1u;
	short_image.relocation_table_offset = 0x1fu;
	if (plan_mz_for_test(&short_image, 0x2000u, 0x40u,
			     DOS_PROCESS_LAUNCH_EXECUTE, 0u,
			     &plan) != DOS_PROCESS_OK)
		return 15;
	short_image.relocation_count = 0u;
	short_image.load_high = true;
	if (plan_mz_for_test(&short_image, 0x2000u, 0x40u,
			     DOS_PROCESS_LAUNCH_EXECUTE, 0u,
			     &plan) != DOS_PROCESS_BAD_IMAGE_RANGE)
		return 16;

	/*
	 * CS and SS relocation uses 16-bit additions. A block beginning at
	 * segment zero can safely contain both the high-loaded resident and the
	 * low wrapped entry/stack addresses, so range validation must not
	 * replace the guest-visible modulo arithmetic.
	 */
	wrap_image = (struct dos_load_plan){
	    .format = DOS_IMAGE_MZ,
	    .file_size = 0x64u,
	    .image_file_offset = 0x20u,
	    .image_size = 0x1e0u,
	    .minimum_image_paragraphs = 0x1eu,
	    .minimum_extra_paragraphs = 1u,
	    .maximum_extra_paragraphs = 0u,
	    .initial_cs = 0x30u,
	    .initial_ip = 0u,
	    .initial_ss = 0x20u,
	    .initial_sp = 0x0200u,
	    .load_high = true,
	};
	if (plan_mz_for_test(&wrap_image, 0u, 0xffffu,
			     DOS_PROCESS_LAUNCH_EXECUTE, 0x66u,
			     &plan) != DOS_PROCESS_OK ||
	    plan.block_end_segment != 0xffffu || plan.load_segment != 0xffe1u ||
	    plan.relocation_factor != 0xffe1u || !plan.load_high ||
	    plan.initial_state.cs != 0x0011u ||
	    plan.initial_state.ss != 0x0001u || plan.initial_state.ds != 0u ||
	    plan.initial_state.es != 0u ||
	    plan.load_only_stack_pointer != 0x01feu)
		return 17;
	return 0;
}

static int test_initial_ax_finalization(void)
{
	struct dos_load_plan com_image = {
	    .format = DOS_IMAGE_COM,
	    .file_size = 1u,
	    .image_size = 1u,
	    .minimum_image_paragraphs = 1u,
	    .initial_ip = 0x100u,
	};
	struct dos_load_plan mz_image = {
	    .format = DOS_IMAGE_MZ,
	    .file_size = 0x64u,
	    .image_file_offset = 0x20u,
	    .image_size = 0x1e0u,
	    .minimum_image_paragraphs = 0x1eu,
	    .minimum_extra_paragraphs = 1u,
	    .maximum_extra_paragraphs = 0x20u,
	    .initial_sp = 0x0200u,
	};
	struct dos_com_process_plan com_plan;
	struct dos_com_process_plan expected_com;
	struct dos_mz_process_plan mz_plan;
	struct dos_mz_process_plan expected_mz;

	if (plan_com_for_test(&com_image, 0x2000u, 0x100u, 0u, &com_plan) !=
	    DOS_PROCESS_OK)
		return 1;
	expected_com = com_plan;
	expected_com.load_only_stack_value = 0x12ffu;
	expected_com.initial_state.eax = 0x12ffu;
	expected_com.initial_state.ebx = 0x12ffu;
	if (dos_process_finalize_com_initial_ax(&com_plan, 0x12ffu) !=
		DOS_PROCESS_OK ||
	    !bytes_are_equal(&com_plan, &expected_com, sizeof(com_plan)))
		return 2;
	/* Rebinding is safe while the plan remains private and internally
	 * consistent. */
	expected_com.load_only_stack_value = 0xff00u;
	expected_com.initial_state.eax = 0xff00u;
	expected_com.initial_state.ebx = 0xff00u;
	if (dos_process_finalize_com_initial_ax(&com_plan, 0xff00u) !=
		DOS_PROCESS_OK ||
	    !bytes_are_equal(&com_plan, &expected_com, sizeof(com_plan)))
		return 3;

	if (plan_mz_for_test(&mz_image, 0x2000u, 0x40u,
			     DOS_PROCESS_LAUNCH_LOAD_ONLY, 0u,
			     &mz_plan) != DOS_PROCESS_OK)
		return 4;
	expected_mz = mz_plan;
	expected_mz.load_only_stack_value = 0x00ffu;
	expected_mz.initial_state.eax = 0x00ffu;
	expected_mz.initial_state.ebx = 0x00ffu;
	if (dos_process_finalize_mz_initial_ax(&mz_plan, 0x00ffu) !=
		DOS_PROCESS_OK ||
	    !bytes_are_equal(&mz_plan, &expected_mz, sizeof(mz_plan)))
		return 5;

	/* A stale/forged plan cannot be partially repaired by finalization. */
	com_plan.reserved8 = 1u;
	expected_com = com_plan;
	if (dos_process_finalize_com_initial_ax(&com_plan, 0x5555u) !=
		DOS_PROCESS_INVALID_ARGUMENT ||
	    !bytes_are_equal(&com_plan, &expected_com, sizeof(com_plan)))
		return 6;
	com_plan.reserved8 = 0u;
	com_plan.initial_state.ebx ^= 1u;
	expected_com = com_plan;
	if (dos_process_finalize_com_initial_ax(&com_plan, 0x5555u) !=
		DOS_PROCESS_INVALID_ARGUMENT ||
	    !bytes_are_equal(&com_plan, &expected_com, sizeof(com_plan)) ||
	    dos_process_finalize_com_initial_ax(NULL, 0u) !=
		DOS_PROCESS_INVALID_ARGUMENT ||
	    dos_process_finalize_mz_initial_ax(NULL, 0u) !=
		DOS_PROCESS_INVALID_ARGUMENT)
		return 7;
	return 0;
}

static int run_tests(void)
{
	static const struct dos_machine_ops ops = {
	    .read_memory = test_read_memory,
	    .write_memory = test_write_memory,
	};
	struct dos_machine machine;
	int status;

	if (dos_machine_configure(&machine, &ops, 1u, TEST_MEMORY_SIZE,
				  false) != DOS_MACHINE_OK)
		return 1;
	status = test_command_tails();
	if (status != 0)
		return 10 + status;
	status = test_initial_psp();
	if (status != 0)
		return 30 + status;
	status = test_psp(&machine);
	if (status != 0)
		return 20 + status;
	status = test_exact_child_jft(&machine);
	if (status != 0)
		return 50 + status;
	status = test_parent_snapshot(&machine);
	if (status != 0)
		return 70 + status;
	status = test_psp_source_aliasing(&machine);
	if (status != 0)
		return 170 + status;
	status = test_allocation_plans();
	if (status != 0)
		return 90 + status;
	status = test_com_plans();
	if (status != 0)
		return 110 + status;
	status = test_mz_plans();
	if (status != 0)
		return 130 + status;
	status = test_initial_ax_finalization();
	if (status != 0)
		return 150 + status;
	return 0;
}

DOSC32_TEST_ENTRY(run_tests)

// SPDX-License-Identifier: GPL-2.0-only
/* Freestanding MZ relocation tests. */
#include "dos_relocator.h"
#include "test_entry.h"

#define IMAGE_BYTES 512u
#define MEMORY_BYTES 0x40000u
#define TABLE_OFFSET 0x1cu
#define RESIDENT_FILE_OFFSET 0x80u
#define TABLE_AFTER_RESIDENT_OFFSET (RESIDENT_FILE_OFFSET + 0x80u)
#define RESIDENT_BASE 0x20000u
#define RESIDENT_SIZE 0x1000u
#define RELOCATION_FACTOR 0x2000u
#define READER_CONTEXT ((kernel_object_handle_t)0x52454c4f4346494cull)
#define MACHINE_CONTEXT ((kernel_object_handle_t)0x52454c4f434d454dull)

static uint8_t image_bytes[IMAGE_BYTES];
static uint8_t guest_memory[MEMORY_BYTES];
static uint32_t image_read_calls;
static uint32_t fail_image_read_call;
static uint32_t short_image_read_call;
static uint32_t mutate_image_read_call;
static size_t mutate_relocation_index;
static uint16_t mutate_relocation_offset;
static uint16_t mutate_relocation_segment;
static uint32_t machine_read_calls;
static uint32_t fail_machine_read_call;
static uint32_t machine_write_calls;
static uint32_t fail_machine_write_call;
static bool fail_all_machine_writes;

static void clear_bytes(uint8_t *bytes, size_t count)
{
	size_t index;

	for (index = 0u; index < count; ++index)
		bytes[index] = 0u;
}

static void write_le16_bytes(uint8_t *bytes, size_t offset, uint16_t value)
{
	bytes[offset] = (uint8_t)value;
	bytes[offset + 1u] = (uint8_t)(value >> 8);
}

static uint16_t read_le16_bytes(const uint8_t *bytes, size_t offset)
{
	return (uint16_t)bytes[offset] | ((uint16_t)bytes[offset + 1u] << 8);
}

static void set_relocation_at(size_t table_offset, size_t index,
			      uint16_t offset, uint16_t segment)
{
	size_t entry = table_offset + index * 4u;

	write_le16_bytes(image_bytes, entry, offset);
	write_le16_bytes(image_bytes, entry + 2u, segment);
}

static void set_relocation(size_t index, uint16_t offset, uint16_t segment)
{
	set_relocation_at(TABLE_OFFSET, index, offset, segment);
}

static void reset_fixture(void)
{
	clear_bytes(image_bytes, sizeof(image_bytes));
	clear_bytes(guest_memory, sizeof(guest_memory));
	image_read_calls = 0u;
	fail_image_read_call = 0u;
	short_image_read_call = 0u;
	mutate_image_read_call = 0u;
	mutate_relocation_index = 0u;
	mutate_relocation_offset = 0x20u;
	mutate_relocation_segment = 0x0100u;
	machine_read_calls = 0u;
	fail_machine_read_call = 0u;
	machine_write_calls = 0u;
	fail_machine_write_call = 0u;
	fail_all_machine_writes = false;
}

static enum dos_image_read_status image_read(kernel_object_handle_t context,
					     file_offset_t offset,
					     void *destination,
					     size_t destination_capacity,
					     size_t count, size_t *bytes_read)
{
	uint8_t *output = (uint8_t *)destination;
	size_t index;

	++image_read_calls;
	if (bytes_read == NULL)
		return DOS_IMAGE_READ_IO_ERROR;
	*bytes_read = 0u;
	if (context != READER_CONTEXT || destination == NULL ||
	    count > destination_capacity || offset > IMAGE_BYTES ||
	    count > IMAGE_BYTES - (size_t)offset ||
	    image_read_calls == fail_image_read_call)
		return DOS_IMAGE_READ_IO_ERROR;
	if (image_read_calls == mutate_image_read_call)
		set_relocation(mutate_relocation_index,
			       mutate_relocation_offset,
			       mutate_relocation_segment);
	for (index = 0u; index < count; ++index)
		output[index] = image_bytes[(size_t)offset + index];
	*bytes_read = count;
	if (image_read_calls == short_image_read_call && count != 0u)
		*bytes_read = count - 1u;
	return DOS_IMAGE_READ_OK;
}

static enum dos_machine_status machine_read(kernel_object_handle_t context,
					    dos_linear_address_t linear_address,
					    void *destination,
					    size_t destination_capacity,
					    size_t count)
{
	uint8_t *output = (uint8_t *)destination;
	size_t index;

	++machine_read_calls;
	if (context != MACHINE_CONTEXT || destination == NULL ||
	    count > destination_capacity || linear_address > MEMORY_BYTES ||
	    count > MEMORY_BYTES - (size_t)linear_address ||
	    machine_read_calls == fail_machine_read_call)
		return DOS_MACHINE_ADDRESS_FAULT;
	for (index = 0u; index < count; ++index)
		output[index] = guest_memory[(size_t)linear_address + index];
	return DOS_MACHINE_OK;
}

static enum dos_machine_status
machine_write(kernel_object_handle_t context,
	      dos_linear_address_t linear_address, const void *source,
	      size_t source_capacity, size_t count)
{
	const uint8_t *input = (const uint8_t *)source;
	size_t index;

	++machine_write_calls;
	if (context != MACHINE_CONTEXT || source == NULL ||
	    count > source_capacity || linear_address > MEMORY_BYTES ||
	    count > MEMORY_BYTES - (size_t)linear_address)
		return DOS_MACHINE_ADDRESS_FAULT;
	/* Dirty the first byte before failure to prove replace restores it. */
	if (fail_all_machine_writes ||
	    machine_write_calls == fail_machine_write_call) {
		guest_memory[linear_address] = 0xeeu;
		return DOS_MACHINE_IO_FAULT;
	}
	for (index = 0u; index < count; ++index)
		guest_memory[(size_t)linear_address + index] = input[index];
	return DOS_MACHINE_OK;
}

static const struct dos_machine_ops machine_ops = {
    .read_memory = machine_read,
    .write_memory = machine_write,
    .read_port = NULL,
    .write_port = NULL,
    .set_a20 = NULL,
};

static struct dos_image_reader make_reader(void)
{
	struct dos_image_reader reader = {
	    .context = READER_CONTEXT,
	    .size = IMAGE_BYTES,
	    .read = image_read,
	};

	return reader;
}

static struct dos_relocator_request make_request(uint16_t count)
{
	struct dos_relocator_request request = {
	    .relocation_table_offset = TABLE_OFFSET,
	    .resident_size = RESIDENT_SIZE,
	    .resident_linear_address = RESIDENT_BASE,
	    .relocation_count = count,
	    .relocation_factor = RELOCATION_FACTOR,
	};

	return request;
}

static bool result_is(const struct dos_relocator_result *result,
		      uint16_t validated, uint16_t applied)
{
	return result->validated_entries == validated &&
	       result->applied_entries == applied;
}

static bool registers_equal(const struct dos_cpu_state *left,
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

static int test_success_and_fixed_chunks(struct dos_machine *machine)
{
	struct dos_image_reader reader = make_reader();
	struct dos_relocator_request request = make_request(17u);
	struct dos_relocator_result result = {0xa5a5u, 0x5a5au};
	struct dos_cpu_state registers = {
	    .eax = 0x12345678u,
	    .ebx = 0x87654321u,
	    .ecx = 0x0badc0deu,
	    .eflags = DOS_EFLAGS_CF | DOS_EFLAGS_IF,
	    .cs = 0x1111u,
	    .ds = 0x2222u,
	    .mode = DOS_CPU_VM86,
	};
	struct dos_cpu_state expected_registers = registers;
	size_t index;

	reset_fixture();
	for (index = 0u; index < 17u; ++index) {
		set_relocation(index, (uint16_t)(0x20u + index * 2u), 0u);
		write_le16_bytes(guest_memory,
				 RESIDENT_BASE + 0x20u + index * 2u,
				 (uint16_t)(0x1000u + index));
	}
	/* Prove that the 16-bit target-word addition wraps exactly. */
	write_le16_bytes(guest_memory, RESIDENT_BASE + 0x20u, 0xf000u);
	if (dos_relocator_apply(&reader, machine, &request, &result) !=
		DOS_RELOCATOR_OK ||
	    !result_is(&result, 17u, 17u) || image_read_calls != 4u ||
	    machine_write_calls != 17u ||
	    read_le16_bytes(guest_memory, RESIDENT_BASE + 0x20u) != 0x1000u ||
	    !registers_equal(&registers, &expected_registers))
		return 1;
	for (index = 1u; index < 17u; ++index) {
		if (read_le16_bytes(guest_memory,
				    RESIDENT_BASE + 0x20u + index * 2u) !=
		    (uint16_t)(0x3000u + index))
			return 2;
	}
	return 0;
}

static int test_complete_validation_before_writes(struct dos_machine *machine)
{
	struct dos_image_reader reader = make_reader();
	struct dos_relocator_request request = make_request(2u);
	struct dos_relocator_result result = {0x1111u, 0x2222u};
	struct dos_machine a20_machine;

	reset_fixture();
	set_relocation(0u, 0x20u, 0u);
	set_relocation(1u, 0x20u, 0x0100u);
	write_le16_bytes(guest_memory, RESIDENT_BASE + 0x20u, 0x3456u);
	if (dos_relocator_apply(&reader, machine, &request, &result) !=
		DOS_RELOCATOR_TARGET_OUTSIDE_RESIDENT ||
	    !result_is(&result, 0x1111u, 0x2222u) ||
	    machine_write_calls != 0u ||
	    read_le16_bytes(guest_memory, RESIDENT_BASE + 0x20u) != 0x3456u)
		return 1;

	reset_fixture();
	request.relocation_count = 1u;
	set_relocation(0u, 0xffffu, 0u);
	if (dos_relocator_apply(&reader, machine, &request, &result) !=
		DOS_RELOCATOR_BAD_TARGET_OFFSET ||
	    machine_write_calls != 0u)
		return 2;

	reset_fixture();
	request.relocation_factor = 1u;
	request.resident_linear_address = 0u;
	request.resident_size = 0x100u;
	set_relocation(0u, 0x20u, 0xffffu);
	write_le16_bytes(guest_memory, 0x20u, 0xffffu);
	if (dos_relocator_apply(&reader, machine, &request, &result) !=
		DOS_RELOCATOR_OK ||
	    machine_write_calls != 1u ||
	    read_le16_bytes(guest_memory, 0x20u) != 0u)
		return 3;

	/* Segment wrapping and physical A20 wrapping are independent. */
	reset_fixture();
	request = make_request(1u);
	request.relocation_factor = 1u;
	request.resident_linear_address = 0u;
	request.resident_size = 2u;
	set_relocation(0u, 0x10u, 0xfffeu);
	write_le16_bytes(guest_memory, 0u, 0xffffu);
	if (dos_relocator_apply(&reader, machine, &request, &result) !=
		DOS_RELOCATOR_OK ||
	    read_le16_bytes(guest_memory, 0u) != 0u)
		return 4;
	a20_machine = *machine;
	a20_machine.a20_enabled = true;
	machine_write_calls = 0u;
	if (dos_relocator_apply(&reader, &a20_machine, &request, &result) !=
		DOS_RELOCATOR_TARGET_OUTSIDE_RESIDENT ||
	    machine_write_calls != 0u)
		return 5;

	reset_fixture();
	request = make_request(1u);
	request.resident_size = 0x100u;
	set_relocation(0u, 0xffu, 0u);
	if (dos_relocator_apply(&reader, machine, &request, &result) !=
		DOS_RELOCATOR_TARGET_OUTSIDE_RESIDENT ||
	    machine_write_calls != 0u)
		return 6;
	return 0;
}

static int test_file_and_resident_ranges(struct dos_machine *machine)
{
	struct dos_image_reader reader = make_reader();
	struct dos_relocator_request request = make_request(1u);
	struct dos_relocator_result result = {0xaaaau, 0xbbbbu};

	reset_fixture();
	request.relocation_table_offset = (file_offset_t)-2;
	reader.size = (file_offset_t)-1;
	if (dos_relocator_apply(&reader, machine, &request, &result) !=
		DOS_RELOCATOR_FILE_RANGE_OVERFLOW ||
	    !result_is(&result, 0xaaaau, 0xbbbbu))
		return 1;

	reader = make_reader();
	request = make_request(1u);
	request.relocation_table_offset = IMAGE_BYTES - 2u;
	if (dos_relocator_apply(&reader, machine, &request, &result) !=
		DOS_RELOCATOR_BAD_FILE_RANGE ||
	    !result_is(&result, 0xaaaau, 0xbbbbu) || image_read_calls != 0u ||
	    machine_write_calls != 0u)
		return 2;

	request = make_request(1u);
	request.resident_linear_address = MEMORY_BYTES - 1u;
	request.resident_size = 2u;
	if (dos_relocator_apply(&reader, machine, &request, &result) !=
	    DOS_RELOCATOR_BAD_RESIDENT_RANGE)
		return 3;

	/* The immutable file extent may equal the final entry's end exactly. */
	reset_fixture();
	request = make_request(1u);
	request.relocation_table_offset = IMAGE_BYTES - 4u;
	set_relocation_at(IMAGE_BYTES - 4u, 0u, 0x20u, 0u);
	write_le16_bytes(guest_memory, RESIDENT_BASE + 0x20u, 1u);
	if (dos_relocator_apply(&reader, machine, &request, &result) !=
		DOS_RELOCATOR_OK ||
	    read_le16_bytes(guest_memory, RESIDENT_BASE + 0x20u) != 0x2001u)
		return 4;
	return 0;
}

static int test_absolute_seek_offsets(struct dos_machine *machine)
{
	static const file_offset_t offsets[] = {
	    0u,
	    DOS_EXEC_PRIVATE_MZ_HEADER_BYTES - 1u,
	    DOS_EXEC_PRIVATE_MZ_HEADER_BYTES,
	    DOS_EXEC_PRIVATE_MZ_HEADER_BYTES + 1u,
	    TABLE_AFTER_RESIDENT_OFFSET,
	};
	struct dos_image_reader reader = make_reader();
	struct dos_relocator_request request = make_request(1u);
	struct dos_relocator_result result;
	size_t index;

	/*
	 * EXEC performs an absolute seek without comparing it with either the
	 * private-header end or the resident image's file position.
	 */
	for (index = 0u; index < sizeof(offsets) / sizeof(offsets[0]);
	     ++index) {
		reset_fixture();
		request.relocation_table_offset = offsets[index];
		result.validated_entries = 0xa5a5u;
		result.applied_entries = 0x5a5au;
		set_relocation_at((size_t)offsets[index], 0u, 0x20u, 0u);
		write_le16_bytes(guest_memory, RESIDENT_BASE + 0x20u, 1u);
		if (dos_relocator_apply(&reader, machine, &request, &result) !=
			DOS_RELOCATOR_OK ||
		    !result_is(&result, 1u, 1u) || image_read_calls != 2u ||
		    machine_write_calls != 1u ||
		    read_le16_bytes(guest_memory, RESIDENT_BASE + 0x20u) !=
			0x2001u)
			return (int)index + 1;
	}
	return 0;
}

static int test_reader_failures_and_revalidation(struct dos_machine *machine)
{
	struct dos_image_reader reader = make_reader();
	struct dos_relocator_request request = make_request(1u);
	struct dos_relocator_result result = {0x1357u, 0x2468u};

	reset_fixture();
	set_relocation(0u, 0x20u, 0u);
	fail_image_read_call = 1u;
	if (dos_relocator_apply(&reader, machine, &request, &result) !=
		DOS_RELOCATOR_IMAGE_IO_ERROR ||
	    !result_is(&result, 0x1357u, 0x2468u) || machine_write_calls != 0u)
		return 1;

	reset_fixture();
	set_relocation(0u, 0x20u, 0u);
	short_image_read_call = 1u;
	if (dos_relocator_apply(&reader, machine, &request, &result) !=
		DOS_RELOCATOR_IMAGE_SHORT_READ ||
	    machine_write_calls != 0u)
		return 2;

	reset_fixture();
	set_relocation(0u, 0x20u, 0u);
	fail_image_read_call = 2u;
	if (dos_relocator_apply(&reader, machine, &request, &result) !=
		DOS_RELOCATOR_IMAGE_IO_ERROR ||
	    machine_write_calls != 0u)
		return 3;

	reset_fixture();
	set_relocation(0u, 0x20u, 0u);
	short_image_read_call = 2u;
	if (dos_relocator_apply(&reader, machine, &request, &result) !=
		DOS_RELOCATOR_IMAGE_SHORT_READ ||
	    machine_write_calls != 0u)
		return 4;

	reset_fixture();
	set_relocation(0u, 0x20u, 0u);
	mutate_image_read_call = 2u;
	if (dos_relocator_apply(&reader, machine, &request, &result) !=
		DOS_RELOCATOR_TARGET_OUTSIDE_RESIDENT ||
	    machine_write_calls != 0u)
		return 5;

	/*
	 * Deliberately violate the immutable-reader contract after the first
	 * application chunk.  The second-pass range check stops the changed
	 * entry; the already modified words remain only in the isolated block,
	 * result is not published, and the EXEC layer must discard the block.
	 */
	reset_fixture();
	request = make_request(17u);
	for (mutate_relocation_index = 0u; mutate_relocation_index < 17u;
	     ++mutate_relocation_index) {
		set_relocation(mutate_relocation_index,
			       (uint16_t)(0x20u + mutate_relocation_index * 2u),
			       0u);
		write_le16_bytes(
		    guest_memory,
		    RESIDENT_BASE + 0x20u + mutate_relocation_index * 2u, 1u);
	}
	mutate_image_read_call = 4u;
	mutate_relocation_index = 16u;
	mutate_relocation_offset = 0x40u;
	mutate_relocation_segment = 0x0100u;
	if (dos_relocator_apply(&reader, machine, &request, &result) !=
		DOS_RELOCATOR_TARGET_OUTSIDE_RESIDENT ||
	    !result_is(&result, 0x1357u, 0x2468u) ||
	    machine_write_calls != 16u ||
	    read_le16_bytes(guest_memory, RESIDENT_BASE + 0x20u) != 0x2001u ||
	    read_le16_bytes(guest_memory, RESIDENT_BASE + 0x40u) != 1u)
		return 6;
	return 0;
}

static int test_transaction_faults(struct dos_machine *machine)
{
	struct dos_image_reader reader = make_reader();
	struct dos_relocator_request request = make_request(2u);
	struct dos_relocator_result result = {0xabcdu, 0xdcabu};

	reset_fixture();
	set_relocation(0u, 0x20u, 0u);
	set_relocation(1u, 0x22u, 0u);
	write_le16_bytes(guest_memory, RESIDENT_BASE + 0x20u, 0x1000u);
	write_le16_bytes(guest_memory, RESIDENT_BASE + 0x22u, 0x1100u);
	fail_machine_write_call = 2u;
	/* First entry is already relocated, but the result stays unpublished;
	 * the second entry's dirty failing write is rolled back. */
	if (dos_relocator_apply(&reader, machine, &request, &result) !=
		DOS_RELOCATOR_MACHINE_FAULT ||
	    !result_is(&result, 0xabcdu, 0xdcabu) ||
	    read_le16_bytes(guest_memory, RESIDENT_BASE + 0x20u) != 0x3000u ||
	    read_le16_bytes(guest_memory, RESIDENT_BASE + 0x22u) != 0x1100u ||
	    machine_write_calls != 3u)
		return 1;

	reset_fixture();
	request.relocation_count = 1u;
	set_relocation(0u, 0x20u, 0u);
	write_le16_bytes(guest_memory, RESIDENT_BASE + 0x20u, 0x1234u);
	fail_machine_read_call = 1u;
	if (dos_relocator_apply(&reader, machine, &request, &result) !=
		DOS_RELOCATOR_MACHINE_FAULT ||
	    machine_write_calls != 0u ||
	    read_le16_bytes(guest_memory, RESIDENT_BASE + 0x20u) != 0x1234u)
		return 2;

	reset_fixture();
	set_relocation(0u, 0x20u, 0u);
	write_le16_bytes(guest_memory, RESIDENT_BASE + 0x20u, 0x5678u);
	fail_all_machine_writes = true;
	if (dos_relocator_apply(&reader, machine, &request, &result) !=
		DOS_RELOCATOR_MACHINE_POISONED ||
	    !result_is(&result, 0xabcdu, 0xdcabu) ||
	    guest_memory[RESIDENT_BASE + 0x20u] != 0xeeu ||
	    machine_write_calls != 2u)
		return 3;
	return 0;
}

static int test_duplicate_and_empty_table(struct dos_machine *machine)
{
	struct dos_image_reader reader = make_reader();
	struct dos_relocator_request request = make_request(2u);
	struct dos_relocator_result result = {0xffffu, 0xffffu};

	reset_fixture();
	set_relocation(0u, 0x20u, 0u);
	set_relocation(1u, 0x20u, 0u);
	write_le16_bytes(guest_memory, RESIDENT_BASE + 0x20u, 1u);
	if (dos_relocator_apply(&reader, machine, &request, &result) !=
		DOS_RELOCATOR_OK ||
	    read_le16_bytes(guest_memory, RESIDENT_BASE + 0x20u) != 0x4001u ||
	    !result_is(&result, 2u, 2u))
		return 1;

	reset_fixture();
	request = make_request(0u);
	/* Local $LSEEK accepts this decoded 16-bit position past EOF.  With no
	 * entries it names no immutable-reader bytes and performs no callback. */
	request.relocation_table_offset = 0xffffu;
	request.resident_linear_address = MEMORY_BYTES;
	request.resident_size = 0u;
	if (dos_relocator_apply(&reader, machine, &request, &result) !=
		DOS_RELOCATOR_OK ||
	    !result_is(&result, 0u, 0u) || image_read_calls != 0u ||
	    machine_read_calls != 0u || machine_write_calls != 0u)
		return 2;

	request.relocation_table_offset = 0x10000u;
	result.validated_entries = 0x1357u;
	result.applied_entries = 0x2468u;
	if (dos_relocator_apply(&reader, machine, &request, &result) !=
		DOS_RELOCATOR_BAD_FILE_RANGE ||
	    !result_is(&result, 0x1357u, 0x2468u) || image_read_calls != 0u ||
	    machine_read_calls != 0u || machine_write_calls != 0u)
		return 3;
	return 0;
}

static int run_tests(void)
{
	struct dos_machine machine;
	int status;

	if (dos_machine_configure(&machine, &machine_ops, MACHINE_CONTEXT,
				  MEMORY_BYTES, false) != DOS_MACHINE_OK)
		return 1;
	status = test_success_and_fixed_chunks(&machine);
	if (status != 0)
		return 10 + status;
	status = test_complete_validation_before_writes(&machine);
	if (status != 0)
		return 20 + status;
	status = test_file_and_resident_ranges(&machine);
	if (status != 0)
		return 30 + status;
	status = test_absolute_seek_offsets(&machine);
	if (status != 0)
		return 40 + status;
	status = test_reader_failures_and_revalidation(&machine);
	if (status != 0)
		return 50 + status;
	status = test_transaction_faults(&machine);
	if (status != 0)
		return 60 + status;
	status = test_duplicate_and_empty_table(&machine);
	if (status != 0)
		return 70 + status;
	return 0;
}

DOSC32_TEST_ENTRY(run_tests)

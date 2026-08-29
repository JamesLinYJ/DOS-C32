// SPDX-License-Identifier: GPL-2.0-only
/* Freestanding resident-loader tests. */
#include "dos_image_load.h"
#include "test_entry.h"

#define IMAGE_CAPACITY 0x14000u
#define GUEST_CAPACITY 0x110000u
#define READER_CONTEXT ((kernel_object_handle_t)0x494d41474546494cull)
#define MACHINE_CONTEXT ((kernel_object_handle_t)0x494d4147454d454dull)
#define LEASE_HANDLE ((kernel_object_handle_t)0x0000000700000021ull)
#define ARENA_IDENTITY ((kernel_object_handle_t)0x4152454e41494431ull)
#define ARENA_GENERATION 9u
#define LEASE_OWNER 0x3456u
#define ORIGINAL_BYTE 0xa5u
#define MZ_HEADER_BYTES 0x20u

static uint8_t image_bytes[IMAGE_CAPACITY];
static uint8_t guest_memory[GUEST_CAPACITY];
static uint32_t image_read_calls;
static uint32_t fail_image_read_call;
static uint32_t short_image_read_call;
static uint32_t machine_read_calls;
static uint32_t machine_write_calls;
static uint32_t fail_machine_write_call;
static bool fail_all_machine_writes;

static void fill_bytes(uint8_t *bytes, size_t count, uint8_t value)
{
	size_t index;

	for (index = 0u; index < count; ++index)
		bytes[index] = value;
}

static void reset_fixture(void)
{
	size_t index;

	for (index = 0u; index < sizeof(image_bytes); ++index)
		image_bytes[index] = (uint8_t)(0x31u ^ (uint8_t)index);
	fill_bytes(guest_memory, sizeof(guest_memory), ORIGINAL_BYTE);
	image_read_calls = 0u;
	fail_image_read_call = 0u;
	short_image_read_call = 0u;
	machine_read_calls = 0u;
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
	    count > destination_capacity || offset > IMAGE_CAPACITY ||
	    count > IMAGE_CAPACITY - (size_t)offset ||
	    image_read_calls == fail_image_read_call)
		return DOS_IMAGE_READ_IO_ERROR;
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
	    count > destination_capacity || linear_address > GUEST_CAPACITY ||
	    count > GUEST_CAPACITY - (size_t)linear_address)
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
	    count > source_capacity || linear_address > GUEST_CAPACITY ||
	    count > GUEST_CAPACITY - (size_t)linear_address)
		return DOS_MACHINE_ADDRESS_FAULT;
	if ((fail_all_machine_writes ||
	     machine_write_calls == fail_machine_write_call) &&
	    count != 0u) {
		guest_memory[(size_t)linear_address] = 0xeeu;
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

static struct dos_machine make_machine(bool a20_enabled)
{
	struct dos_machine machine = {
	    .ops = &machine_ops,
	    .context = MACHINE_CONTEXT,
	    .address_limit = GUEST_CAPACITY,
	    .a20_enabled = a20_enabled,
	};

	return machine;
}

static struct dos_image_reader make_reader(file_offset_t size)
{
	struct dos_image_reader reader = {
	    .context = READER_CONTEXT,
	    .size = size,
	    .read = image_read,
	};

	return reader;
}

static void make_com_plans(uint16_t psp_segment, uint16_t block_paragraphs,
			   uint32_t image_size,
			   enum dos_process_launch_mode launch_mode,
			   struct dos_load_plan *image_plan,
			   struct dos_com_process_plan *process_plan,
			   struct dos_memory_lease_view *lease)
{
	uint32_t block_bytes = (uint32_t)block_paragraphs << 4;
	uint32_t capacity =
	    block_bytes >= 0x10000u ? 0xff00u : block_bytes - 0x100u;
	uint16_t stack = (uint16_t)(capacity + 0xfeu);

	*image_plan = (struct dos_load_plan){0};
	image_plan->format = DOS_IMAGE_COM;
	image_plan->file_size = image_size;
	image_plan->image_size = image_size;
	image_plan->minimum_image_paragraphs = (image_size + 15u) >> 4;
	image_plan->initial_ip = 0x100u;

	*process_plan = (struct dos_com_process_plan){0};
	process_plan->psp_segment = psp_segment;
	process_plan->block_end_segment =
	    (uint16_t)((uint32_t)psp_segment + block_paragraphs);
	process_plan->load_segment = (uint16_t)(psp_segment + 0x10u);
	process_plan->load_linear_address =
	    (dos_linear_address_t)((uint32_t)process_plan->load_segment << 4);
	process_plan->image_size = image_size;
	process_plan->read_capacity = capacity;
	process_plan->stack_sentinel_offset = stack;
	process_plan->stack_sentinel_value = 0u;
	process_plan->load_only_stack_pointer = (uint16_t)(stack - 2u);
	process_plan->load_only_stack_value = 0x34ffu;
	process_plan->launch_mode = launch_mode;
	process_plan->initial_state.ebx = 0x34ffu;
	process_plan->initial_state.esp = stack;
	process_plan->initial_state.ss = psp_segment;

	lease->handle.value = LEASE_HANDLE;
	lease->machine_context = MACHINE_CONTEXT;
	lease->arena_identity = ARENA_IDENTITY;
	lease->arena_generation = ARENA_GENERATION;
	lease->guest_segment = psp_segment;
	lease->paragraphs = block_paragraphs;
	lease->owner = LEASE_OWNER;
	lease->reserved = 0u;
}

static void make_mz_plans(uint16_t psp_segment, uint16_t block_paragraphs,
			  file_offset_t file_size, uint32_t image_size,
			  enum dos_process_launch_mode launch_mode,
			  struct dos_load_plan *image_plan,
			  struct dos_mz_process_plan *process_plan,
			  struct dos_memory_lease_view *lease)
{
	uint64_t source_span_end = MZ_HEADER_BYTES + (uint64_t)image_size;
	uint64_t page_span_end = (source_span_end + 511u) & ~511ull;
	uint32_t resident_bytes = (uint32_t)(page_span_end - MZ_HEADER_BYTES);
	uint32_t resident_paragraphs = resident_bytes >> 4;
	uint16_t load_segment = (uint16_t)(psp_segment + 0x10u);

	*image_plan = (struct dos_load_plan){0};
	image_plan->format = DOS_IMAGE_MZ;
	image_plan->file_size = file_size;
	image_plan->image_file_offset = MZ_HEADER_BYTES;
	image_plan->image_size = resident_bytes;
	image_plan->minimum_image_paragraphs = resident_paragraphs;
	image_plan->minimum_extra_paragraphs = 1u;
	image_plan->maximum_extra_paragraphs = 0x20u;
	image_plan->initial_cs = 0u;
	image_plan->initial_ip = 0u;
	image_plan->initial_ss = 0x10u;
	image_plan->initial_sp = 0x0200u;
	image_plan->relocation_count = 3u;
	image_plan->relocation_table_offset = 0x1cu;
	image_plan->load_high = false;

	*process_plan = (struct dos_mz_process_plan){0};
	process_plan->psp_segment = psp_segment;
	process_plan->block_end_segment =
	    (uint16_t)((uint32_t)psp_segment + block_paragraphs);
	process_plan->load_segment = load_segment;
	process_plan->load_linear_address =
	    (dos_linear_address_t)((uint32_t)load_segment << 4);
	process_plan->image_file_offset = MZ_HEADER_BYTES;
	process_plan->image_size = resident_bytes;
	process_plan->resident_paragraphs = resident_paragraphs;
	process_plan->relocation_factor = load_segment;
	process_plan->relocation_count = image_plan->relocation_count;
	process_plan->relocation_table_offset =
	    image_plan->relocation_table_offset;
	process_plan->load_only_stack_pointer = 0x01feu;
	process_plan->load_only_stack_value = 0xbeefu;
	process_plan->load_high = false;
	process_plan->launch_mode = launch_mode;
	process_plan->initial_state.ebx = 0xbeefu;
	process_plan->initial_state.esp = 0x0200u;
	process_plan->initial_state.eip = 0u;
	process_plan->initial_state.cs = load_segment;
	process_plan->initial_state.ss = (uint16_t)(load_segment + 0x10u);

	lease->handle.value = LEASE_HANDLE;
	lease->machine_context = MACHINE_CONTEXT;
	lease->arena_identity = ARENA_IDENTITY;
	lease->arena_generation = ARENA_GENERATION;
	lease->guest_segment = psp_segment;
	lease->paragraphs = block_paragraphs;
	lease->owner = LEASE_OWNER;
	lease->reserved = 0u;
}

static bool result_is(const struct dos_image_load_result *result,
		      kernel_object_handle_t handle, uint32_t written,
		      uint32_t resident, uint32_t untouched)
{
	return result->lease_handle == handle &&
	       result->file_bytes_written == written &&
	       result->resident_bytes == resident &&
	       result->untouched_bytes == untouched && result->reserved == 0u;
}

static bool result_is_unchanged(const struct dos_image_load_result *result,
				kernel_object_handle_t handle, uint32_t written,
				uint32_t resident, uint32_t untouched,
				uint32_t reserved)
{
	return result->lease_handle == handle &&
	       result->file_bytes_written == written &&
	       result->resident_bytes == resident &&
	       result->untouched_bytes == untouched &&
	       result->reserved == reserved;
}

static bool com_view_is_rejected_without_callbacks(
    const struct dos_image_reader *reader, const struct dos_machine *machine,
    const struct dos_load_plan *image_plan,
    const struct dos_com_process_plan *process_plan,
    const struct dos_memory_lease_view *lease_view,
    enum dos_image_load_status expected_status)
{
	struct dos_image_load_result result = {
	    .lease_handle = 0x11u,
	    .file_bytes_written = 0x22u,
	    .resident_bytes = 0x33u,
	    .untouched_bytes = 0x44u,
	    .reserved = 0x55u,
	};

	return dos_image_load_com_resident(reader, machine, image_plan,
					   process_plan, lease_view,
					   &result) == expected_status &&
	       image_read_calls == 0u && machine_read_calls == 0u &&
	       machine_write_calls == 0u &&
	       result_is_unchanged(&result, 0x11u, 0x22u, 0x33u, 0x44u, 0x55u);
}

static bool target_matches_file(size_t target, size_t file, size_t count)
{
	size_t index;

	for (index = 0u; index < count; ++index) {
		if (guest_memory[target + index] != image_bytes[file + index])
			return false;
	}
	return true;
}

static bool target_is_original(size_t target, size_t count)
{
	size_t index;

	for (index = 0u; index < count; ++index) {
		if (guest_memory[target + index] != ORIGINAL_BYTE)
			return false;
	}
	return true;
}

static int test_com_exact_file_write(void)
{
	struct dos_machine machine = make_machine(false);
	struct dos_load_plan image_plan;
	struct dos_com_process_plan process_plan;
	struct dos_memory_lease_view lease;
	struct dos_image_load_result result = {0};
	struct dos_image_reader reader;
	size_t target;

	reset_fixture();
	make_com_plans(0x3000u, 0x80u, 300u, DOS_PROCESS_LAUNCH_EXECUTE,
		       &image_plan, &process_plan, &lease);
	reader = make_reader(image_plan.file_size);
	target = process_plan.load_linear_address;
	if (dos_image_load_com_resident(&reader, &machine, &image_plan,
					&process_plan, &lease,
					&result) != DOS_IMAGE_LOAD_OK ||
	    !result_is(&result, LEASE_HANDLE, 300u, 0x700u, 0x700u - 300u) ||
	    image_read_calls != 2u || machine_write_calls != 2u ||
	    !target_matches_file(target, 0u, 300u) ||
	    !target_is_original(target + 300u, 0x700u - 300u))
		return 1;

	/* EXEC's equality case is out of memory; a forged plan never starts
	 * I/O. */
	reset_fixture();
	make_com_plans(0x3000u, 0x80u, 0x700u, DOS_PROCESS_LAUNCH_EXECUTE,
		       &image_plan, &process_plan, &lease);
	reader = make_reader(image_plan.file_size);
	result = (struct dos_image_load_result){
	    .lease_handle = 9u,
	    .file_bytes_written = 8u,
	    .resident_bytes = 7u,
	    .untouched_bytes = 6u,
	    .reserved = 5u,
	};
	if (dos_image_load_com_resident(&reader, &machine, &image_plan,
					&process_plan, &lease, &result) !=
		DOS_IMAGE_LOAD_BAD_RESIDENT_RANGE ||
	    image_read_calls != 0u || machine_write_calls != 0u ||
	    !result_is_unchanged(&result, 9u, 8u, 7u, 6u, 5u))
		return 2;

	reset_fixture();
	make_com_plans(0x3000u, 0x80u, 300u, DOS_PROCESS_LAUNCH_EXECUTE,
		       &image_plan, &process_plan, &lease);
	reader = make_reader(image_plan.file_size);
	fail_image_read_call = 2u;
	if (dos_image_load_com_resident(&reader, &machine, &image_plan,
					&process_plan, &lease, &result) !=
		DOS_IMAGE_LOAD_IMAGE_IO_ERROR ||
	    !target_matches_file(target, 0u, 256u) ||
	    !target_is_original(target + 256u, 44u))
		return 3;

	reset_fixture();
	make_com_plans(0x3000u, 0x80u, 300u, DOS_PROCESS_LAUNCH_EXECUTE,
		       &image_plan, &process_plan, &lease);
	reader = make_reader(image_plan.file_size);
	short_image_read_call = 2u;
	if (dos_image_load_com_resident(&reader, &machine, &image_plan,
					&process_plan, &lease, &result) !=
		DOS_IMAGE_LOAD_IMAGE_SHORT_READ ||
	    !target_matches_file(target, 0u, 256u) ||
	    !target_is_original(target + 256u, 44u))
		return 4;
	return 0;
}

static int test_mz_final_page_and_tail(void)
{
	struct dos_machine machine = make_machine(false);
	struct dos_load_plan image_plan;
	struct dos_mz_process_plan process_plan;
	struct dos_memory_lease_view lease;
	struct dos_image_load_result result = {0};
	struct dos_image_reader reader;
	size_t target;

	/* 100-byte physical file: 32-byte header, 68 bytes loaded, 412
	 * untouched. */
	reset_fixture();
	make_mz_plans(0x2000u, 0x80u, 100u, 68u, DOS_PROCESS_LAUNCH_EXECUTE,
		      &image_plan, &process_plan, &lease);
	reader = make_reader(image_plan.file_size);
	target = process_plan.load_linear_address;
	if (dos_image_load_mz_resident(&reader, &machine, &image_plan,
				       &process_plan, &lease,
				       &result) != DOS_IMAGE_LOAD_OK ||
	    !dos_mz_process_plan_has_valid_encoding(&process_plan) ||
	    !result_is(&result, LEASE_HANDLE, 68u, 480u, 412u) ||
	    !target_matches_file(target, MZ_HEADER_BYTES, 68u) ||
	    !target_is_original(target + 68u, 412u) ||
	    !target_is_original(target + 480u, 0x100u))
		return 1;

	/* EXEC reads real trailing bytes that fit before the resident page end.
	 */
	reset_fixture();
	make_mz_plans(0x2000u, 0x80u, 120u, 68u, DOS_PROCESS_LAUNCH_EXECUTE,
		      &image_plan, &process_plan, &lease);
	reader = make_reader(image_plan.file_size);
	if (dos_image_load_mz_resident(&reader, &machine, &image_plan,
				       &process_plan, &lease,
				       &result) != DOS_IMAGE_LOAD_OK ||
	    !result_is(&result, LEASE_HANDLE, 88u, 480u, 392u) ||
	    !target_matches_file(target, MZ_HEADER_BYTES, 88u) ||
	    !target_is_original(target + 88u, 392u))
		return 2;

	/* Using exact image paragraphs instead of EXEC's page footprint is
	 * stale. */
	reset_fixture();
	make_mz_plans(0x2000u, 0x80u, 100u, 68u, DOS_PROCESS_LAUNCH_EXECUTE,
		      &image_plan, &process_plan, &lease);
	reader = make_reader(image_plan.file_size);
	process_plan.resident_paragraphs = 5u;
	if (dos_image_load_mz_resident(&reader, &machine, &image_plan,
				       &process_plan, &lease, &result) !=
		DOS_IMAGE_LOAD_BAD_RESIDENT_RANGE ||
	    image_read_calls != 0u || machine_write_calls != 0u)
		return 3;
	return 0;
}

static int test_mz_wrapped_initial_segments(void)
{
	struct dos_machine machine = make_machine(false);
	struct dos_load_plan image_plan;
	struct dos_mz_process_plan process_plan;
	struct dos_memory_lease_view lease;
	struct dos_image_load_result result = {0};
	struct dos_image_reader reader;
	uint32_t prior_image_reads;
	uint32_t prior_machine_reads;
	uint32_t prior_machine_writes;
	size_t target;

	reset_fixture();
	make_mz_plans(0u, 0xffffu, 100u, 68u, DOS_PROCESS_LAUNCH_EXECUTE,
		      &image_plan, &process_plan, &lease);
	image_plan.maximum_extra_paragraphs = 0u;
	image_plan.initial_cs = 0x30u;
	image_plan.initial_ss = 0x20u;
	image_plan.load_high = true;
	process_plan.load_segment = 0xffe1u;
	process_plan.load_linear_address = 0x000ffe10u;
	process_plan.relocation_factor = 0xffe1u;
	process_plan.load_high = true;
	process_plan.initial_state.cs = 0x0011u;
	process_plan.initial_state.ss = 0x0001u;
	reader = make_reader(image_plan.file_size);
	target = process_plan.load_linear_address;

	if (dos_image_load_mz_resident(&reader, &machine, &image_plan,
				       &process_plan, &lease,
				       &result) != DOS_IMAGE_LOAD_OK ||
	    !result_is(&result, LEASE_HANDLE, 68u, 480u, 412u) ||
	    !target_matches_file(target, MZ_HEADER_BYTES, 68u) ||
	    !target_is_original(target + 68u, 412u) ||
	    dos_image_load_prepare_mz_stack(&machine, &process_plan, &lease) !=
		DOS_IMAGE_LOAD_OK)
		return 1;
	prior_image_reads = image_read_calls;
	prior_machine_reads = machine_read_calls;
	prior_machine_writes = machine_write_calls;
	process_plan.reserved1 = 1u;
	if (dos_image_load_mz_resident(&reader, &machine, &image_plan,
				       &process_plan, &lease, &result) !=
		DOS_IMAGE_LOAD_INVALID_ARGUMENT ||
	    image_read_calls != prior_image_reads ||
	    machine_read_calls != prior_machine_reads ||
	    machine_write_calls != prior_machine_writes)
		return 2;
	return 0;
}

static int test_reader_failures_and_discard(void)
{
	struct dos_machine machine = make_machine(false);
	struct dos_load_plan image_plan;
	struct dos_mz_process_plan process_plan;
	struct dos_memory_lease_view lease;
	struct dos_image_load_result result = {
	    .lease_handle = 0x11u,
	    .file_bytes_written = 0x22u,
	    .resident_bytes = 0x33u,
	    .untouched_bytes = 0x44u,
	    .reserved = 0x55u,
	};
	struct dos_image_reader reader;
	size_t target;

	/* Two-page footprint, 580 file bytes and a 412-byte untouched tail. */
	reset_fixture();
	make_mz_plans(0x2000u, 0x100u, 612u, 580u, DOS_PROCESS_LAUNCH_EXECUTE,
		      &image_plan, &process_plan, &lease);
	reader = make_reader(image_plan.file_size);
	target = process_plan.load_linear_address;
	fail_image_read_call = 2u;
	if (dos_image_load_mz_resident(&reader, &machine, &image_plan,
				       &process_plan, &lease, &result) !=
		DOS_IMAGE_LOAD_IMAGE_IO_ERROR ||
	    !result_is_unchanged(&result, 0x11u, 0x22u, 0x33u, 0x44u, 0x55u) ||
	    !target_matches_file(target, 0x20u, 256u) ||
	    !target_is_original(target + 256u, 256u))
		return 1;

	reset_fixture();
	make_mz_plans(0x2000u, 0x100u, 612u, 580u, DOS_PROCESS_LAUNCH_EXECUTE,
		      &image_plan, &process_plan, &lease);
	reader = make_reader(image_plan.file_size);
	short_image_read_call = 2u;
	if (dos_image_load_mz_resident(&reader, &machine, &image_plan,
				       &process_plan, &lease, &result) !=
		DOS_IMAGE_LOAD_IMAGE_SHORT_READ ||
	    !target_matches_file(target, 0x20u, 256u) ||
	    !target_is_original(target + 256u, 256u))
		return 2;

	/* Stale reader size is rejected during preflight, before any callback.
	 */
	reset_fixture();
	make_mz_plans(0x2000u, 0x100u, 612u, 580u, DOS_PROCESS_LAUNCH_EXECUTE,
		      &image_plan, &process_plan, &lease);
	reader = make_reader(611u);
	if (dos_image_load_mz_resident(&reader, &machine, &image_plan,
				       &process_plan, &lease,
				       &result) != DOS_IMAGE_LOAD_STALE_PLAN ||
	    image_read_calls != 0u || machine_write_calls != 0u)
		return 3;
	return 0;
}

static int test_machine_rollback_and_poison(void)
{
	struct dos_machine machine = make_machine(false);
	struct dos_load_plan image_plan;
	struct dos_mz_process_plan process_plan;
	struct dos_memory_lease_view lease;
	struct dos_image_load_result result = {0x11u, 0x22u, 0x33u, 0x44u,
					       0x55u};
	struct dos_image_reader reader;
	size_t target;

	reset_fixture();
	make_mz_plans(0x2000u, 0x100u, 612u, 580u, DOS_PROCESS_LAUNCH_EXECUTE,
		      &image_plan, &process_plan, &lease);
	reader = make_reader(image_plan.file_size);
	target = process_plan.load_linear_address;
	fail_machine_write_call = 1u;
	if (dos_image_load_mz_resident(&reader, &machine, &image_plan,
				       &process_plan, &lease, &result) !=
		DOS_IMAGE_LOAD_MACHINE_FAULT ||
	    machine_write_calls != 2u || !target_is_original(target, 256u) ||
	    !result_is_unchanged(&result, 0x11u, 0x22u, 0x33u, 0x44u, 0x55u))
		return 1;

	/* Chunk one remains prepared; rollback repairs only failing chunk two.
	 */
	reset_fixture();
	make_mz_plans(0x2000u, 0x100u, 612u, 580u, DOS_PROCESS_LAUNCH_EXECUTE,
		      &image_plan, &process_plan, &lease);
	reader = make_reader(image_plan.file_size);
	fail_machine_write_call = 2u;
	if (dos_image_load_mz_resident(&reader, &machine, &image_plan,
				       &process_plan, &lease, &result) !=
		DOS_IMAGE_LOAD_MACHINE_FAULT ||
	    machine_write_calls != 3u ||
	    !target_matches_file(target, 0x20u, 256u) ||
	    !target_is_original(target + 256u, 256u))
		return 2;

	reset_fixture();
	make_mz_plans(0x2000u, 0x100u, 612u, 580u, DOS_PROCESS_LAUNCH_EXECUTE,
		      &image_plan, &process_plan, &lease);
	reader = make_reader(image_plan.file_size);
	fail_all_machine_writes = true;
	if (dos_image_load_mz_resident(&reader, &machine, &image_plan,
				       &process_plan, &lease, &result) !=
		DOS_IMAGE_LOAD_MACHINE_POISONED ||
	    machine_write_calls != 2u || guest_memory[target] != 0xeeu)
		return 3;
	return 0;
}

static int test_stack_prepare_steps(void)
{
	struct dos_machine machine = make_machine(false);
	struct dos_load_plan image_plan;
	struct dos_com_process_plan com_plan;
	struct dos_mz_process_plan mz_plan;
	struct dos_memory_lease_view lease;
	size_t address;

	reset_fixture();
	make_com_plans(0x3000u, 0x1000u, 1u, DOS_PROCESS_LAUNCH_EXECUTE,
		       &image_plan, &com_plan, &lease);
	address = ((size_t)com_plan.psp_segment << 4) +
		  com_plan.stack_sentinel_offset;
	if (dos_image_load_prepare_com_stack(&machine, &com_plan, &lease) !=
		DOS_IMAGE_LOAD_OK ||
	    machine_write_calls != 1u || guest_memory[address] != 0u ||
	    guest_memory[address + 1u] != 0u ||
	    guest_memory[address - 1u] != ORIGINAL_BYTE)
		return 1;

	reset_fixture();
	make_com_plans(0x3000u, 0x1000u, 1u, DOS_PROCESS_LAUNCH_LOAD_ONLY,
		       &image_plan, &com_plan, &lease);
	if (dos_image_load_prepare_com_stack(&machine, &com_plan, &lease) !=
		DOS_IMAGE_LOAD_OK ||
	    machine_write_calls != 2u)
		return 2;
	address = ((size_t)com_plan.initial_state.ss << 4) +
		  com_plan.load_only_stack_pointer;
	if (guest_memory[address] != 0xffu ||
	    guest_memory[address + 1u] != 0x34u)
		return 3;

	/*
	 * At DOS's largest legal COM size, the zero sentinel intentionally
	 * replaces the final file byte at PSP:fffeH.
	 */
	reset_fixture();
	make_com_plans(0x3000u, 0x1000u, 0xfeffu, DOS_PROCESS_LAUNCH_EXECUTE,
		       &image_plan, &com_plan, &lease);
	{
		struct dos_image_reader reader =
		    make_reader(image_plan.file_size);
		struct dos_image_load_result result = {0};
		size_t last_file =
		    (size_t)com_plan.load_linear_address + 0xfefeu;

		if (dos_image_load_com_resident(&reader, &machine, &image_plan,
						&com_plan, &lease,
						&result) != DOS_IMAGE_LOAD_OK ||
		    guest_memory[last_file] != image_bytes[0xfefeu] ||
		    dos_image_load_prepare_com_stack(
			&machine, &com_plan, &lease) != DOS_IMAGE_LOAD_OK ||
		    guest_memory[last_file] != 0u ||
		    guest_memory[last_file + 1u] != 0u)
			return 4;
	}

	reset_fixture();
	make_mz_plans(0x1000u, 0x2000u, 100u, 68u, DOS_PROCESS_LAUNCH_EXECUTE,
		      &image_plan, &mz_plan, &lease);
	mz_plan.initial_state.esp = 0u;
	mz_plan.load_only_stack_pointer = 0xfffeu;
	if (dos_image_load_prepare_mz_stack(&machine, &mz_plan, &lease) !=
		DOS_IMAGE_LOAD_OK ||
	    machine_write_calls != 0u)
		return 5;

	reset_fixture();
	make_mz_plans(0x1000u, 0x2000u, 100u, 68u, DOS_PROCESS_LAUNCH_LOAD_ONLY,
		      &image_plan, &mz_plan, &lease);
	mz_plan.initial_state.esp = 0u;
	mz_plan.load_only_stack_pointer = 0xfffeu;
	address = ((size_t)mz_plan.initial_state.ss << 4) + 0xfffeu;
	if (dos_image_load_prepare_mz_stack(&machine, &mz_plan, &lease) !=
		DOS_IMAGE_LOAD_OK ||
	    machine_write_calls != 1u || guest_memory[address] != 0xefu ||
	    guest_memory[address + 1u] != 0xbeu)
		return 6;
	return 0;
}

static int test_far_and_segment_boundaries(void)
{
	struct dos_machine machine = make_machine(false);
	struct dos_load_plan image_plan;
	struct dos_com_process_plan process_plan;
	struct dos_memory_lease_view lease;
	struct dos_image_load_result result = {0};
	struct dos_image_reader reader;
	size_t target;

	/* Highest non-wrapping 1000h-paragraph COM block, A20 off then on. */
	reset_fixture();
	make_com_plans(0xefffu, 0x1000u, 1u, DOS_PROCESS_LAUNCH_EXECUTE,
		       &image_plan, &process_plan, &lease);
	reader = make_reader(1u);
	target = process_plan.load_linear_address;
	if (dos_image_load_com_resident(&reader, &machine, &image_plan,
					&process_plan, &lease,
					&result) != DOS_IMAGE_LOAD_OK ||
	    guest_memory[target] != image_bytes[0] ||
	    dos_image_load_prepare_com_stack(&machine, &process_plan, &lease) !=
		DOS_IMAGE_LOAD_OK)
		return 1;
	reset_fixture();
	machine.a20_enabled = true;
	if (dos_image_load_com_resident(&reader, &machine, &image_plan,
					&process_plan, &lease,
					&result) != DOS_IMAGE_LOAD_OK ||
	    guest_memory[target] != image_bytes[0] ||
	    dos_image_load_prepare_com_stack(&machine, &process_plan, &lease) !=
		DOS_IMAGE_LOAD_OK)
		return 2;

	/* A 16-bit block-end carry is rejected before reader or machine I/O. */
	reset_fixture();
	make_com_plans(0xf000u, 0x1000u, 1u, DOS_PROCESS_LAUNCH_EXECUTE,
		       &image_plan, &process_plan, &lease);
	reader = make_reader(1u);
	if (dos_image_load_com_resident(&reader, &machine, &image_plan,
					&process_plan, &lease,
					&result) != DOS_IMAGE_LOAD_BAD_LEASE ||
	    image_read_calls != 0u || machine_read_calls != 0u ||
	    machine_write_calls != 0u)
		return 3;

	/* Generation zero cannot name an acquired EXEC memory lease. */
	reset_fixture();
	make_com_plans(0x3000u, 0x80u, 1u, DOS_PROCESS_LAUNCH_EXECUTE,
		       &image_plan, &process_plan, &lease);
	reader = make_reader(1u);
	lease.handle.value = 0u;
	if (dos_image_load_com_resident(&reader, &machine, &image_plan,
					&process_plan, &lease, &result) !=
		DOS_IMAGE_LOAD_INVALID_ARGUMENT ||
	    image_read_calls != 0u || machine_read_calls != 0u ||
	    machine_write_calls != 0u)
		return 4;

	/* Fixed-layout reserved bytes are rejected before any callback. */
	reset_fixture();
	make_com_plans(0x3000u, 0x80u, 1u, DOS_PROCESS_LAUNCH_EXECUTE,
		       &image_plan, &process_plan, &lease);
	reader = make_reader(1u);
	process_plan.reserved16 = 1u;
	if (dos_image_load_com_resident(&reader, &machine, &image_plan,
					&process_plan, &lease, &result) !=
		DOS_IMAGE_LOAD_INVALID_ARGUMENT ||
	    image_read_calls != 0u || machine_read_calls != 0u ||
	    machine_write_calls != 0u)
		return 5;

	/* Target extent validation is callback-free and precedes file reads. */
	reset_fixture();
	make_com_plans(0x3000u, 0x80u, 1u, DOS_PROCESS_LAUNCH_EXECUTE,
		       &image_plan, &process_plan, &lease);
	reader = make_reader(1u);
	machine.address_limit = process_plan.load_linear_address + 0x100u;
	if (dos_image_load_com_resident(&reader, &machine, &image_plan,
					&process_plan, &lease, &result) !=
		DOS_IMAGE_LOAD_BAD_RESIDENT_RANGE ||
	    image_read_calls != 0u || machine_read_calls != 0u ||
	    machine_write_calls != 0u)
		return 6;
	return 0;
}

static int test_active_lease_view_validation(void)
{
	struct dos_machine machine = make_machine(false);
	struct dos_load_plan image_plan;
	struct dos_com_process_plan process_plan;
	struct dos_memory_lease_view lease;
	struct dos_image_reader reader;

	reset_fixture();
	make_com_plans(0x3000u, 0x80u, 1u, DOS_PROCESS_LAUNCH_EXECUTE,
		       &image_plan, &process_plan, &lease);
	reader = make_reader(1u);

	lease.machine_context = MACHINE_CONTEXT + 1u;
	if (!com_view_is_rejected_without_callbacks(
		&reader, &machine, &image_plan, &process_plan, &lease,
		DOS_IMAGE_LOAD_BAD_LEASE))
		return 1;
	lease.machine_context = 0u;
	if (!com_view_is_rejected_without_callbacks(
		&reader, &machine, &image_plan, &process_plan, &lease,
		DOS_IMAGE_LOAD_INVALID_ARGUMENT))
		return 2;
	lease.machine_context = KERNEL_OBJECT_HANDLE_INVALID;
	if (!com_view_is_rejected_without_callbacks(
		&reader, &machine, &image_plan, &process_plan, &lease,
		DOS_IMAGE_LOAD_INVALID_ARGUMENT))
		return 3;
	lease.machine_context = MACHINE_CONTEXT;

	lease.arena_identity = 0u;
	if (!com_view_is_rejected_without_callbacks(
		&reader, &machine, &image_plan, &process_plan, &lease,
		DOS_IMAGE_LOAD_INVALID_ARGUMENT))
		return 4;
	lease.arena_identity = KERNEL_OBJECT_HANDLE_INVALID;
	if (!com_view_is_rejected_without_callbacks(
		&reader, &machine, &image_plan, &process_plan, &lease,
		DOS_IMAGE_LOAD_INVALID_ARGUMENT))
		return 5;
	lease.arena_identity = ARENA_IDENTITY;

	lease.arena_generation = 0u;
	if (!com_view_is_rejected_without_callbacks(
		&reader, &machine, &image_plan, &process_plan, &lease,
		DOS_IMAGE_LOAD_INVALID_ARGUMENT))
		return 6;
	lease.arena_generation = ARENA_GENERATION;

	lease.owner = 0u;
	if (!com_view_is_rejected_without_callbacks(
		&reader, &machine, &image_plan, &process_plan, &lease,
		DOS_IMAGE_LOAD_INVALID_ARGUMENT))
		return 7;
	lease.owner = LEASE_OWNER;

	lease.reserved = 1u;
	if (!com_view_is_rejected_without_callbacks(
		&reader, &machine, &image_plan, &process_plan, &lease,
		DOS_IMAGE_LOAD_INVALID_ARGUMENT))
		return 8;
	lease.reserved = 0u;

	lease.paragraphs = 0u;
	if (!com_view_is_rejected_without_callbacks(
		&reader, &machine, &image_plan, &process_plan, &lease,
		DOS_IMAGE_LOAD_INVALID_ARGUMENT))
		return 9;
	lease.paragraphs = 0x80u;

	lease.guest_segment = 0x3001u;
	if (!com_view_is_rejected_without_callbacks(
		&reader, &machine, &image_plan, &process_plan, &lease,
		DOS_IMAGE_LOAD_BAD_LEASE))
		return 10;
	lease.guest_segment = 0x3000u;
	lease.paragraphs = 0x7fu;
	if (!com_view_is_rejected_without_callbacks(
		&reader, &machine, &image_plan, &process_plan, &lease,
		DOS_IMAGE_LOAD_BAD_LEASE))
		return 11;
	lease.paragraphs = 0x80u;

	lease.handle.value = KERNEL_OBJECT_HANDLE_INVALID;
	if (!com_view_is_rejected_without_callbacks(
		&reader, &machine, &image_plan, &process_plan, &lease,
		DOS_IMAGE_LOAD_INVALID_ARGUMENT))
		return 12;
	return 0;
}

static int test_large_mz_exec_chunk_progression(void)
{
	struct dos_machine machine = make_machine(true);
	struct dos_load_plan image_plan;
	struct dos_mz_process_plan process_plan;
	struct dos_memory_lease_view lease;
	struct dos_image_load_result result = {0};
	struct dos_image_reader reader;
	const uint32_t image_size = 0x11000u;
	const file_offset_t file_size = MZ_HEADER_BYTES + image_size;
	size_t target;

	reset_fixture();
	make_mz_plans(0x1000u, 0x1300u, file_size, image_size,
		      DOS_PROCESS_LAUNCH_EXECUTE, &image_plan, &process_plan,
		      &lease);
	reader = make_reader(file_size);
	target = process_plan.load_linear_address;
	if (dos_image_load_mz_resident(&reader, &machine, &image_plan,
				       &process_plan, &lease,
				       &result) != DOS_IMAGE_LOAD_OK ||
	    result.file_bytes_written != image_size ||
	    guest_memory[target] != image_bytes[MZ_HEADER_BYTES] ||
	    guest_memory[target + 0xfdffu] !=
		image_bytes[MZ_HEADER_BYTES + 0xfdffu] ||
	    guest_memory[target + 0xfe00u] !=
		image_bytes[MZ_HEADER_BYTES + 0xfe00u] ||
	    guest_memory[target + image_size - 1u] !=
		image_bytes[MZ_HEADER_BYTES + image_size - 1u])
		return 1;
	return 0;
}

static int test_mz_source_short_read_boundary(void)
{
	struct dos_machine machine = make_machine(true);
	struct dos_load_plan image_plan;
	struct dos_mz_process_plan process_plan;
	struct dos_memory_lease_view lease;
	struct dos_image_load_result result = {0};
	struct dos_image_reader reader;
	size_t target;

	/* A seek beyond EOF returns zero bytes. The 480-byte deficit is below
	 * the 512-byte rejection boundary. */
	reset_fixture();
	make_mz_plans(0x1000u, 0x80u, 26u, 68u, DOS_PROCESS_LAUNCH_EXECUTE,
		      &image_plan, &process_plan, &lease);
	reader = make_reader(image_plan.file_size);
	if (dos_image_load_mz_resident(&reader, &machine, &image_plan,
				       &process_plan, &lease,
				       &result) != DOS_IMAGE_LOAD_OK ||
	    !result_is(&result, LEASE_HANDLE, 0u, 480u, 480u) ||
	    image_read_calls != 0u || machine_write_calls != 0u)
		return 1;

	/* pages=0081h/header=2 yields 101eh resident paragraphs.  EXEC first
	 * requests FE00h bytes and then 03e0h; 511 missing bytes succeed. */
	reset_fixture();
	make_mz_plans(0x1000u, 0x1200u, 0x10001u, 0x101e0u,
		      DOS_PROCESS_LAUNCH_EXECUTE, &image_plan, &process_plan,
		      &lease);
	reader = make_reader(image_plan.file_size);
	target = process_plan.load_linear_address;
	if (dos_image_load_mz_resident(&reader, &machine, &image_plan,
				       &process_plan, &lease,
				       &result) != DOS_IMAGE_LOAD_OK ||
	    !result_is(&result, LEASE_HANDLE, 0xffe1u, 0x101e0u, 511u) ||
	    !target_matches_file(target, MZ_HEADER_BYTES, 0xffe1u) ||
	    !target_is_original(target + 0xffe1u, 511u))
		return 2;

	/* One fewer physical byte makes the final deficit exactly 512. */
	reset_fixture();
	make_mz_plans(0x1000u, 0x1200u, 0x10000u, 0x101e0u,
		      DOS_PROCESS_LAUNCH_EXECUTE, &image_plan, &process_plan,
		      &lease);
	reader = make_reader(image_plan.file_size);
	result = (struct dos_image_load_result){
	    .lease_handle = 1u,
	    .file_bytes_written = 2u,
	    .resident_bytes = 3u,
	    .untouched_bytes = 4u,
	    .reserved = 5u,
	};
	if (dos_image_load_mz_resident(&reader, &machine, &image_plan,
				       &process_plan, &lease, &result) !=
		DOS_IMAGE_LOAD_BAD_FILE_RANGE ||
	    image_read_calls != 0u || machine_write_calls != 0u ||
	    !result_is_unchanged(&result, 1u, 2u, 3u, 4u, 5u))
		return 3;
	return 0;
}

static int run_tests(void)
{
	int status;

	status = test_com_exact_file_write();
	if (status != 0)
		return 10 + status;
	status = test_mz_final_page_and_tail();
	if (status != 0)
		return 20 + status;
	status = test_mz_wrapped_initial_segments();
	if (status != 0)
		return 30 + status;
	status = test_reader_failures_and_discard();
	if (status != 0)
		return 40 + status;
	status = test_machine_rollback_and_poison();
	if (status != 0)
		return 50 + status;
	status = test_stack_prepare_steps();
	if (status != 0)
		return 60 + status;
	status = test_far_and_segment_boundaries();
	if (status != 0)
		return 70 + status;
	status = test_active_lease_view_validation();
	if (status != 0)
		return 80 + status;
	status = test_large_mz_exec_chunk_progression();
	if (status != 0)
		return 90 + status;
	status = test_mz_source_short_read_boundary();
	if (status != 0)
		return 100 + status;
	return 0;
}

DOSC32_TEST_ENTRY(run_tests)

// SPDX-License-Identifier: GPL-2.0-only
/* Freestanding tests for DOS-visible EXEC3 overlay target plans. */
#include "dos_exec_overlay.h"
#include "test_entry.h"

#define MZ_SIGNATURE 0x5a4du

static uint8_t image_header[DOS_EXEC_PRIVATE_MZ_HEADER_BYTES];

static void clear_header(void)
{
	size_t index;

	for (index = 0u; index < sizeof(image_header); ++index)
		image_header[index] = 0u;
}

static void write_le16(size_t offset, uint16_t value)
{
	image_header[offset] = (uint8_t)value;
	image_header[offset + 1u] = (uint8_t)(value >> 8);
}

static void make_mz_header(uint16_t pages, uint16_t header_paragraphs,
			   uint16_t minimum_extra, uint16_t maximum_extra)
{
	clear_header();
	write_le16(0u, MZ_SIGNATURE);
	write_le16(4u, pages);
	write_le16(6u, 7u);
	write_le16(8u, header_paragraphs);
	write_le16(10u, minimum_extra);
	write_le16(12u, maximum_extra);
	write_le16(14u, 0x1111u);
	write_le16(16u, 0x2222u);
	write_le16(20u, 0x3333u);
	write_le16(22u, 0x4444u);
	write_le16(24u, 0x1au);
}

static enum dos_image_read_status test_read(kernel_object_handle_t context,
					    file_offset_t offset,
					    void *destination,
					    size_t destination_capacity,
					    size_t count, size_t *bytes_read)
{
	uint8_t *output = (uint8_t *)destination;
	size_t index;

	if (bytes_read == NULL)
		return DOS_IMAGE_READ_IO_ERROR;
	*bytes_read = 0u;
	if (context != 1u || destination == NULL ||
	    count > destination_capacity || offset > sizeof(image_header) ||
	    count > sizeof(image_header) - (size_t)offset)
		return DOS_IMAGE_READ_IO_ERROR;
	for (index = 0u; index < count; ++index)
		output[index] = image_header[(size_t)offset + index];
	*bytes_read = count;
	return DOS_IMAGE_READ_OK;
}

static bool com_plans_equal(const struct dos_exec_com_overlay_plan *left,
			    const struct dos_exec_com_overlay_plan *right)
{
	return left->load_segment == right->load_segment &&
	       left->load_offset == right->load_offset &&
	       left->load_linear_address == right->load_linear_address &&
	       left->image_size == right->image_size &&
	       left->read_capacity == right->read_capacity &&
	       left->reserved == right->reserved;
}

static bool mz_plans_equal(const struct dos_exec_mz_overlay_plan *left,
			   const struct dos_exec_mz_overlay_plan *right)
{
	return left->load_segment == right->load_segment &&
	       left->load_offset == right->load_offset &&
	       left->load_linear_address == right->load_linear_address &&
	       left->image_file_offset == right->image_file_offset &&
	       left->image_size == right->image_size &&
	       left->resident_paragraphs == right->resident_paragraphs &&
	       left->relocation_factor == right->relocation_factor &&
	       left->relocation_count == right->relocation_count &&
	       left->relocation_table_offset ==
		   right->relocation_table_offset &&
	       left->reserved == right->reserved;
}

static int test_com_plan(void)
{
	struct dos_image_reader reader = {
	    .context = 1u,
	    .size = 0xfffeu,
	    .read = test_read,
	};
	struct dos_load_plan image_plan;
	struct dos_load_plan malformed;
	struct dos_exec_com_overlay_plan plan;
	const struct dos_exec_com_overlay_plan sentinel = {
	    .load_segment = 0xa5a5u,
	    .load_offset = 0x5a5au,
	    .load_linear_address = 0x12345678u,
	    .image_size = 0x87654321u,
	    .read_capacity = 0x13572468u,
	    .reserved = 0x55aa55aa55aa55aaull,
	};

	clear_header();
	image_header[0] = 0x90u;
	if (dos_loader_inspect_target(&reader, DOS_LOAD_TARGET_OVERLAY,
				      &image_plan) != DOS_LOADER_OK)
		return 1;
	plan = sentinel;
	if (dos_exec_overlay_plan_com(&image_plan, 0u, &plan) !=
		DOS_EXEC_OVERLAY_OK ||
	    !dos_exec_com_overlay_plan_has_valid_encoding(&plan) ||
	    plan.load_segment != 0u || plan.load_linear_address != 0u ||
	    plan.image_size != 0xfffeu ||
	    plan.read_capacity != DOS_EXEC_COM_OVERLAY_READ_CAPACITY)
		return 2;
	/* Segment ffffH is a DOS value; A20/range policy belongs to the later
	 * machine-bound lease, not this target plan. */
	if (dos_exec_overlay_plan_com(&image_plan, 0xffffu, &plan) !=
		DOS_EXEC_OVERLAY_OK ||
	    plan.load_segment != 0xffffu ||
	    plan.load_linear_address != 0xffff0u)
		return 3;

	plan = sentinel;
	if (dos_exec_overlay_plan_com(NULL, 0x1111u, &plan) !=
		DOS_EXEC_OVERLAY_INVALID_ARGUMENT ||
	    !com_plans_equal(&plan, &sentinel) ||
	    dos_exec_overlay_plan_com(&image_plan, 0x1111u, NULL) !=
		DOS_EXEC_OVERLAY_INVALID_ARGUMENT)
		return 4;
	malformed = image_plan;
	malformed.target_kind = DOS_LOAD_TARGET_PROCESS;
	if (dos_exec_overlay_plan_com(&malformed, 0x1111u, &plan) !=
		DOS_EXEC_OVERLAY_INVALID_ARGUMENT ||
	    !com_plans_equal(&plan, &sentinel))
		return 5;
	malformed = image_plan;
	malformed.reserved32 = 1u;
	if (dos_exec_overlay_plan_com(&malformed, 0x1111u, &plan) !=
		DOS_EXEC_OVERLAY_INVALID_ARGUMENT ||
	    !com_plans_equal(&plan, &sentinel))
		return 6;
	malformed = image_plan;
	malformed.image_size--;
	if (dos_exec_overlay_plan_com(&malformed, 0x1111u, &plan) !=
		DOS_EXEC_OVERLAY_STALE_IMAGE_PLAN ||
	    !com_plans_equal(&plan, &sentinel))
		return 7;
	malformed = image_plan;
	malformed.format = DOS_IMAGE_MZ;
	if (dos_exec_overlay_plan_com(&malformed, 0x1111u, &plan) !=
		DOS_EXEC_OVERLAY_WRONG_IMAGE_FORMAT ||
	    !com_plans_equal(&plan, &sentinel))
		return 8;
	return 0;
}

static int test_mz_plan(void)
{
	struct dos_image_reader reader = {
	    .context = 1u,
	    .size = 512u,
	    .read = test_read,
	};
	struct dos_load_plan image_plan;
	struct dos_load_plan malformed;
	struct dos_exec_mz_overlay_target target = {
	    .load_segment = 0x2345u,
	    .relocation_factor = 0xabcdu,
	};
	struct dos_exec_mz_overlay_plan plan;
	struct dos_exec_mz_overlay_plan malformed_plan;
	const struct dos_exec_mz_overlay_plan sentinel = {
	    .load_segment = 0xa5a5u,
	    .load_offset = 0x5a5au,
	    .load_linear_address = 0x12345678u,
	    .image_file_offset = 0x1122334455667788ull,
	    .image_size = 0x87654321u,
	    .resident_paragraphs = 0x13572468u,
	    .relocation_factor = 0x1111u,
	    .relocation_count = 0x2222u,
	    .relocation_table_offset = 0x3333u,
	    .reserved = 0x4444u,
	};

	/* max-BSS zero sets loader load_high, but EXEC3 never allocates or
	 * interprets that process policy. */
	make_mz_header(1u, 2u, 0x7777u, 0u);
	if (dos_loader_inspect_target(&reader, DOS_LOAD_TARGET_OVERLAY,
				      &image_plan) != DOS_LOADER_OK ||
	    !image_plan.load_high)
		return 1;
	plan = sentinel;
	if (dos_exec_overlay_plan_mz(&image_plan, &target, &plan) !=
		DOS_EXEC_OVERLAY_OK ||
	    !dos_exec_mz_overlay_plan_has_valid_encoding(&plan) ||
	    plan.load_segment != 0x2345u ||
	    plan.load_linear_address != 0x23450u ||
	    plan.relocation_factor != 0xabcdu ||
	    plan.image_file_offset != 32u || plan.image_size != 480u ||
	    plan.resident_paragraphs != 30u || plan.relocation_count != 7u ||
	    plan.relocation_table_offset != 0x1au)
		return 2;
	malformed_plan = plan;
	malformed_plan.image_file_offset = 17u;
	if (dos_exec_mz_overlay_plan_has_valid_encoding(&malformed_plan))
		return 3;
	malformed_plan = plan;
	malformed_plan.image_file_offset = (file_offset_t)-1;
	if (dos_exec_mz_overlay_plan_has_valid_encoding(&malformed_plan))
		return 4;
	malformed_plan = plan;
	malformed_plan.resident_paragraphs--;
	malformed_plan.image_size -= 16u;
	if (dos_exec_mz_overlay_plan_has_valid_encoding(&malformed_plan))
		return 5;
	/* The two Exec3 words are independent, including zero and ffffH. */
	target.load_segment = 0xffffu;
	target.relocation_factor = 0u;
	if (dos_exec_overlay_plan_mz(&image_plan, &target, &plan) !=
		DOS_EXEC_OVERLAY_OK ||
	    plan.load_segment != 0xffffu ||
	    plan.load_linear_address != 0xffff0u ||
	    plan.relocation_factor != 0u)
		return 6;
	target.load_segment = 0u;
	target.relocation_factor = 0xffffu;
	if (dos_exec_overlay_plan_mz(&image_plan, &target, &plan) !=
		DOS_EXEC_OVERLAY_OK ||
	    plan.load_segment != 0u || plan.load_linear_address != 0u ||
	    plan.relocation_factor != 0xffffu)
		return 7;

	/* Pages zero minus one header paragraph wraps to ffffH under 16-bit
	 * arithmetic; no process allocation bound is applied.
	 */
	make_mz_header(0u, 1u, 0xffffu, 1u);
	reader.size = DOS_EXEC_PRIVATE_MZ_HEADER_BYTES;
	if (dos_loader_inspect_target(&reader, DOS_LOAD_TARGET_OVERLAY,
				      &image_plan) != DOS_LOADER_OK ||
	    dos_exec_overlay_plan_mz(&image_plan, &target, &plan) !=
		DOS_EXEC_OVERLAY_OK ||
	    plan.resident_paragraphs != 0xffffu ||
	    plan.image_size != 0xffff0u || plan.image_file_offset != 16u)
		return 8;

	plan = sentinel;
	if (dos_exec_overlay_plan_mz(NULL, &target, &plan) !=
		DOS_EXEC_OVERLAY_INVALID_ARGUMENT ||
	    !mz_plans_equal(&plan, &sentinel) ||
	    dos_exec_overlay_plan_mz(&image_plan, NULL, &plan) !=
		DOS_EXEC_OVERLAY_INVALID_ARGUMENT ||
	    !mz_plans_equal(&plan, &sentinel) ||
	    dos_exec_overlay_plan_mz(&image_plan, &target, NULL) !=
		DOS_EXEC_OVERLAY_INVALID_ARGUMENT)
		return 9;
	malformed = image_plan;
	malformed.target_kind = DOS_LOAD_TARGET_PROCESS;
	if (dos_exec_overlay_plan_mz(&malformed, &target, &plan) !=
		DOS_EXEC_OVERLAY_INVALID_ARGUMENT ||
	    !mz_plans_equal(&plan, &sentinel))
		return 10;
	malformed = image_plan;
	malformed.minimum_image_paragraphs--;
	if (dos_exec_overlay_plan_mz(&malformed, &target, &plan) !=
		DOS_EXEC_OVERLAY_STALE_IMAGE_PLAN ||
	    !mz_plans_equal(&plan, &sentinel))
		return 11;
	malformed = image_plan;
	malformed.format = DOS_IMAGE_COM;
	if (dos_exec_overlay_plan_mz(&malformed, &target, &plan) !=
		DOS_EXEC_OVERLAY_WRONG_IMAGE_FORMAT ||
	    !mz_plans_equal(&plan, &sentinel))
		return 12;
	plan = sentinel;
	plan.resident_paragraphs = 0x10000u;
	plan.image_size = 0x100000u;
	if (dos_exec_mz_overlay_plan_has_valid_encoding(&plan))
		return 13;
	return 0;
}

static int run_tests(void)
{
	int status;

	status = test_com_plan();
	if (status != 0)
		return status;
	status = test_mz_plan();
	if (status != 0)
		return 100 + status;
	return 0;
}

DOSC32_TEST_ENTRY(run_tests)

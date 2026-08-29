// SPDX-License-Identifier: GPL-2.0-only
/* Freestanding regression tests for the DOS COM/MZ classifier. */
#include "dos_loader.h"
#include "test_entry.h"

#define TEST_IMAGE_BUFFER_BYTES 64u
#define TEST_READER_CONTEXT ((kernel_object_handle_t)1u)
#define TEST_COM_FILE_BYTES 3u
#define TEST_MZ_FILE_BYTES 512u
#define TEST_DOS_PARAGRAPH_BYTES 16u
#define TEST_MZ_PAGE_COUNT 1u
#define TEST_MZ_HEADER_PARAGRAPHS 2u
#define TEST_MZ_HEADER_BYTES \
	(TEST_MZ_HEADER_PARAGRAPHS * TEST_DOS_PARAGRAPH_BYTES)
#define TEST_MZ_RESIDENT_PARAGRAPHS 30u
#define TEST_MZ_SIGNATURE 0x5a4du
#define TEST_OLD_MZ_SIGNATURE 0x4d5au

/* Independent golden offsets: do not reuse the parser's constants. */
enum test_mz_header_offset {
	TEST_MZ_SIGNATURE_OFFSET = 0u,
	TEST_MZ_LAST_PAGE_BYTES_OFFSET = 2u,
	TEST_MZ_PAGE_COUNT_OFFSET = 4u,
	TEST_MZ_RELOCATION_COUNT_OFFSET = 6u,
	TEST_MZ_HEADER_PARAGRAPHS_OFFSET = 8u,
	TEST_MZ_MINIMUM_EXTRA_OFFSET = 10u,
	TEST_MZ_MAXIMUM_EXTRA_OFFSET = 12u,
	TEST_MZ_INITIAL_SS_OFFSET = 14u,
	TEST_MZ_INITIAL_SP_OFFSET = 16u,
	TEST_MZ_INITIAL_IP_OFFSET = 20u,
	TEST_MZ_INITIAL_CS_OFFSET = 22u,
	TEST_MZ_RELOCATION_TABLE_OFFSET = 24u
};

static uint8_t image_bytes[TEST_IMAGE_BUFFER_BYTES];
static size_t readable_size;

static void write_le16(size_t offset, uint16_t value)
{
	image_bytes[offset] = (uint8_t)value;
	image_bytes[offset + 1u] = (uint8_t)(value >> 8);
}

static void clear_header(void)
{
	size_t index;

	for (index = 0u; index < sizeof(image_bytes); ++index)
		image_bytes[index] = 0u;
	readable_size = sizeof(image_bytes);
}

static enum dos_image_read_status test_read(kernel_object_handle_t context,
					    file_offset_t offset,
					    void *destination,
					    size_t destination_capacity,
					    size_t count, size_t *bytes_read)
{
	uint8_t *output = (uint8_t *)destination;
	size_t index;

	*bytes_read = 0u;
	if (context != TEST_READER_CONTEXT || count > destination_capacity ||
	    offset > readable_size || count > readable_size - (size_t)offset)
		return DOS_IMAGE_READ_IO_ERROR;
	for (index = 0u; index < count; ++index)
		output[index] = image_bytes[(size_t)offset + index];
	*bytes_read = count;
	return DOS_IMAGE_READ_OK;
}

static void make_mz_header(uint16_t signature)
{
	clear_header();
	write_le16(TEST_MZ_SIGNATURE_OFFSET, signature);
	write_le16(TEST_MZ_LAST_PAGE_BYTES_OFFSET, 0u);
	write_le16(TEST_MZ_PAGE_COUNT_OFFSET, TEST_MZ_PAGE_COUNT);
	write_le16(TEST_MZ_RELOCATION_COUNT_OFFSET, 0u);
	write_le16(TEST_MZ_HEADER_PARAGRAPHS_OFFSET,
		   TEST_MZ_HEADER_PARAGRAPHS);
	write_le16(TEST_MZ_MINIMUM_EXTRA_OFFSET, 1u);
	write_le16(TEST_MZ_MAXIMUM_EXTRA_OFFSET, 0xffffu);
	write_le16(TEST_MZ_INITIAL_SS_OFFSET, 0x20u);
	write_le16(TEST_MZ_INITIAL_SP_OFFSET, 0xfffeu);
	write_le16(TEST_MZ_INITIAL_IP_OFFSET, 0x100u);
	write_le16(TEST_MZ_INITIAL_CS_OFFSET, 0x10u);
	write_le16(TEST_MZ_RELOCATION_TABLE_OFFSET, 0x1cu);
}

static bool mz_plan_matches(const struct dos_load_plan *plan,
			    enum dos_load_target_kind target,
			    file_offset_t image_offset,
			    uint16_t resident_paragraphs)
{
	return dos_load_plan_has_valid_encoding(plan) &&
	       dos_load_plan_has_inspected_encoding(plan) &&
	       plan->format == DOS_IMAGE_MZ && plan->target_kind == target &&
	       plan->image_file_offset == image_offset &&
	       plan->image_size ==
		   (uint64_t)resident_paragraphs * TEST_DOS_PARAGRAPH_BYTES &&
	       plan->minimum_image_paragraphs == resident_paragraphs;
}

static int run_tests(void)
{
	static const uint16_t ignored_length_values[] = {
	    0u, 1u, 100u, 511u, 512u, 0xffffu,
	};
	struct dos_image_reader reader = {
	    .context = TEST_READER_CONTEXT,
	    .size = TEST_COM_FILE_BYTES,
	    .read = test_read,
	};
	struct dos_load_plan plan;
	enum dos_loader_status status;
	size_t index;

	clear_header();
	image_bytes[0] = 0x90u;
	readable_size = TEST_COM_FILE_BYTES;
	status = dos_loader_inspect(&reader, &plan);
	if (status != DOS_LOADER_OK ||
	    !dos_load_plan_has_valid_encoding(&plan) ||
	    !dos_load_plan_has_inspected_encoding(&plan) ||
	    plan.format != DOS_IMAGE_COM ||
	    plan.image_size != TEST_COM_FILE_BYTES ||
	    plan.initial_ip != 0x100u ||
	    plan.target_kind != DOS_LOAD_TARGET_PROCESS)
		return 1;

	/* The private loader header is exactly 26 bytes. */
	image_bytes[0] = 'M';
	image_bytes[1] = 'Z';
	reader.size = DOS_EXEC_PRIVATE_MZ_HEADER_BYTES - 1u;
	readable_size = DOS_EXEC_PRIVATE_MZ_HEADER_BYTES - 1u;
	if (dos_loader_inspect(&reader, &plan) != DOS_LOADER_OK ||
	    plan.format != DOS_IMAGE_COM)
		return 2;
	make_mz_header(TEST_MZ_SIGNATURE);
	reader.size = DOS_EXEC_PRIVATE_MZ_HEADER_BYTES;
	readable_size = DOS_EXEC_PRIVATE_MZ_HEADER_BYTES;
	if (dos_loader_inspect(&reader, &plan) != DOS_LOADER_OK ||
	    !mz_plan_matches(&plan, DOS_LOAD_TARGET_PROCESS,
			     TEST_MZ_HEADER_BYTES,
			     TEST_MZ_RESIDENT_PARAGRAPHS))
		return 3;

	reader.size = 0u;
	if (dos_loader_inspect(&reader, &plan) != DOS_LOADER_EMPTY_IMAGE)
		return 4;

	make_mz_header(TEST_MZ_SIGNATURE);
	reader.size = TEST_MZ_FILE_BYTES;
	status = dos_loader_inspect(&reader, &plan);
	if (status != DOS_LOADER_OK ||
	    !mz_plan_matches(&plan, DOS_LOAD_TARGET_PROCESS,
			     TEST_MZ_HEADER_BYTES,
			     TEST_MZ_RESIDENT_PARAGRAPHS) ||
	    plan.initial_cs != 0x10u || plan.initial_ip != 0x100u ||
	    plan.old_mz_signature)
		return 5;

	make_mz_header(TEST_OLD_MZ_SIGNATURE);
	reader.size = TEST_MZ_FILE_BYTES;
	if (dos_loader_inspect(&reader, &plan) != DOS_LOADER_OK ||
	    !plan.old_mz_signature)
		return 6;

	/* exe_len_mod_512 is read into the private header but never consumed.
	 */
	for (index = 0u; index < sizeof(ignored_length_values) /
				     sizeof(ignored_length_values[0]);
	     ++index) {
		make_mz_header(TEST_MZ_SIGNATURE);
		write_le16(TEST_MZ_LAST_PAGE_BYTES_OFFSET,
			   ignored_length_values[index]);
		reader.size = 100u;
		if (dos_loader_inspect(&reader, &plan) != DOS_LOADER_OK ||
		    !mz_plan_matches(&plan, DOS_LOAD_TARGET_PROCESS,
				     TEST_MZ_HEADER_BYTES,
				     TEST_MZ_RESIDENT_PARAGRAPHS) ||
		    plan.file_size != 100u)
			return 7;
		if (dos_loader_inspect_target(&reader, DOS_LOAD_TARGET_OVERLAY,
					      &plan) != DOS_LOADER_OK ||
		    !mz_plan_matches(&plan, DOS_LOAD_TARGET_OVERLAY,
				     TEST_MZ_HEADER_BYTES,
				     TEST_MZ_RESIDENT_PARAGRAPHS))
			return 8;
	}

	/* A physical final-page deficit below 512 is checked by the load phase.
	 */
	make_mz_header(TEST_MZ_SIGNATURE);
	reader.size = 400u;
	if (dos_loader_inspect(&reader, &plan) != DOS_LOADER_OK ||
	    !mz_plan_matches(&plan, DOS_LOAD_TARGET_PROCESS,
			     TEST_MZ_HEADER_BYTES,
			     TEST_MZ_RESIDENT_PARAGRAPHS))
		return 9;

	/* EXEC seeks to the table offset; it does not require the table in
	 * header. */
	make_mz_header(TEST_MZ_SIGNATURE);
	reader.size = TEST_MZ_FILE_BYTES;
	write_le16(TEST_MZ_RELOCATION_COUNT_OFFSET, 2u);
	write_le16(TEST_MZ_RELOCATION_TABLE_OFFSET, 28u);
	if (dos_loader_inspect(&reader, &plan) != DOS_LOADER_OK ||
	    plan.relocation_count != 2u || plan.relocation_table_offset != 28u)
		return 10;

	/* SHL AX,5 discards the high page bits: pages differ by 0800h. */
	make_mz_header(TEST_MZ_SIGNATURE);
	write_le16(TEST_MZ_PAGE_COUNT_OFFSET, 0x0801u);
	if (dos_loader_inspect(&reader, &plan) != DOS_LOADER_OK ||
	    !mz_plan_matches(&plan, DOS_LOAD_TARGET_PROCESS,
			     TEST_MZ_HEADER_BYTES,
			     TEST_MZ_RESIDENT_PARAGRAPHS))
		return 11;

	/* Neither zero pages nor zero/one header paragraphs are pre-rejected.
	 */
	make_mz_header(TEST_MZ_SIGNATURE);
	write_le16(TEST_MZ_PAGE_COUNT_OFFSET, 0u);
	write_le16(TEST_MZ_HEADER_PARAGRAPHS_OFFSET, 0u);
	if (dos_loader_inspect(&reader, &plan) != DOS_LOADER_OK ||
	    !mz_plan_matches(&plan, DOS_LOAD_TARGET_PROCESS, 0u, 0u))
		return 12;
	make_mz_header(TEST_MZ_SIGNATURE);
	write_le16(TEST_MZ_HEADER_PARAGRAPHS_OFFSET, 0u);
	if (dos_loader_inspect(&reader, &plan) != DOS_LOADER_OK ||
	    !mz_plan_matches(&plan, DOS_LOAD_TARGET_PROCESS, 0u, 32u))
		return 13;
	make_mz_header(TEST_MZ_SIGNATURE);
	write_le16(TEST_MZ_HEADER_PARAGRAPHS_OFFSET, 1u);
	if (dos_loader_inspect(&reader, &plan) != DOS_LOADER_OK ||
	    !mz_plan_matches(&plan, DOS_LOAD_TARGET_PROCESS, 16u, 31u))
		return 14;

	/* SUB AX,header also wraps, while the seek offset remains full-width.
	 */
	make_mz_header(TEST_MZ_SIGNATURE);
	write_le16(TEST_MZ_HEADER_PARAGRAPHS_OFFSET, 0xffe2u);
	if (dos_loader_inspect(&reader, &plan) != DOS_LOADER_OK ||
	    !mz_plan_matches(&plan, DOS_LOAD_TARGET_PROCESS, 0xffe20u, 0x3eu))
		return 15;

	/* max BSS below min BSS is consumed by EXEC's later compatibility step. */
	make_mz_header(TEST_MZ_SIGNATURE);
	write_le16(TEST_MZ_MINIMUM_EXTRA_OFFSET, 0x20u);
	write_le16(TEST_MZ_MAXIMUM_EXTRA_OFFSET, 1u);
	if (dos_loader_inspect(&reader, &plan) != DOS_LOADER_OK ||
	    plan.minimum_extra_paragraphs != 0x20u ||
	    plan.maximum_extra_paragraphs != 1u)
		return 16;

	clear_header();
	reader.size = 0xff00u;
	if (dos_loader_inspect(&reader, &plan) != DOS_LOADER_IMAGE_TOO_LARGE)
		return 17;

	/* AL=3 requests ffffh bytes and succeeds only when the read is short.
	 */
	reader.size = 0xfffeu;
	if (dos_loader_inspect_target(&reader, DOS_LOAD_TARGET_OVERLAY,
				      &plan) != DOS_LOADER_OK ||
	    plan.format != DOS_IMAGE_COM ||
	    plan.target_kind != DOS_LOAD_TARGET_OVERLAY ||
	    plan.image_size != 0xfffeu)
		return 18;
	reader.size = 0xffffu;
	if (dos_loader_inspect_target(&reader, DOS_LOAD_TARGET_OVERLAY,
				      &plan) != DOS_LOADER_IMAGE_TOO_LARGE)
		return 19;
	reader.size = TEST_COM_FILE_BYTES;
	if (dos_loader_inspect_target(&reader, (enum dos_load_target_kind)2,
				      &plan) != DOS_LOADER_INVALID_ARGUMENT)
		return 20;

	/* Persistent transaction slots use the strict pure validator. */
	clear_header();
	reader.size = TEST_COM_FILE_BYTES;
	readable_size = TEST_COM_FILE_BYTES;
	if (dos_loader_inspect(&reader, &plan) != DOS_LOADER_OK)
		return 21;
	plan.initial_sp = 1u;
	if (dos_load_plan_has_inspected_encoding(&plan))
		return 22;
	make_mz_header(TEST_MZ_SIGNATURE);
	reader.size = TEST_MZ_FILE_BYTES;
	readable_size = sizeof(image_bytes);
	if (dos_loader_inspect(&reader, &plan) != DOS_LOADER_OK)
		return 23;
	plan.image_size += TEST_DOS_PARAGRAPH_BYTES;
	if (dos_load_plan_has_inspected_encoding(&plan))
		return 24;
	plan = (struct dos_load_plan){0};
	plan.format = (uint8_t)DOS_IMAGE_NATIVE32;
	plan.target_kind = (uint8_t)DOS_LOAD_TARGET_PROCESS;
	if (dos_load_plan_has_inspected_encoding(&plan))
		return 25;
	return 0;
}

DOSC32_TEST_ENTRY(run_tests)

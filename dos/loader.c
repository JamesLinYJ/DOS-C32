// SPDX-License-Identifier: GPL-2.0-only
/*
 * Safe COM/MZ image inspection
 *
 * Compatibility contract: reads the private 26-byte EXEC header; partial/non-MZ input
 *                 is COM, while MZ resident paragraphs use wrapped AX math
 * Safety changes: byte decoding and an independent checked 64-bit seek offset
 */
#include "dos_loader.h"

#include "overflow.h"

#define EXEC_HEADER_BYTES DOS_EXEC_PRIVATE_MZ_HEADER_BYTES
#define COM_LOAD_OFFSET 0x100u
#define COM_PROCESS_MAXIMUM_FILE_SIZE 0xfeffu
#define COM_OVERLAY_MAXIMUM_FILE_SIZE 0xfffeu
#define MZ_SIGNATURE 0x5a4du
#define MZ_OLD_SIGNATURE 0x4d5au
#define MZ_PAGE_TO_PARAGRAPH_SHIFT 5u

/* Field offsets within the private MZ header prefix. */
enum mz_private_header_offset {
	MZ_SIGNATURE_OFFSET = 0u,
	MZ_PAGE_COUNT_OFFSET = 4u,
	MZ_RELOCATION_COUNT_OFFSET = 6u,
	MZ_HEADER_PARAGRAPHS_OFFSET = 8u,
	MZ_MINIMUM_EXTRA_OFFSET = 10u,
	MZ_MAXIMUM_EXTRA_OFFSET = 12u,
	MZ_INITIAL_SS_OFFSET = 14u,
	MZ_INITIAL_SP_OFFSET = 16u,
	MZ_INITIAL_IP_OFFSET = 20u,
	MZ_INITIAL_CS_OFFSET = 22u,
	MZ_RELOCATION_TABLE_OFFSET = 24u
};

static uint16_t read_le16(const uint8_t *bytes)
{
	return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static enum dos_loader_status read_header(const struct dos_image_reader *reader,
					  uint8_t header[EXEC_HEADER_BYTES],
					  size_t count)
{
	size_t bytes_read = 0u;
	enum dos_image_read_status status;

	status = reader->read(reader->context, 0u, header, EXEC_HEADER_BYTES,
			      count, &bytes_read);
	if (status != DOS_IMAGE_READ_OK)
		return DOS_LOADER_IO_ERROR;
	return bytes_read == count ? DOS_LOADER_OK : DOS_LOADER_SHORT_READ;
}

static enum dos_loader_status
build_com_plan(const struct dos_image_reader *reader,
	       enum dos_load_target_kind target, struct dos_load_plan *plan)
{
	file_offset_t maximum =
	    target == DOS_LOAD_TARGET_OVERLAY
		? (file_offset_t)COM_OVERLAY_MAXIMUM_FILE_SIZE
		: (file_offset_t)COM_PROCESS_MAXIMUM_FILE_SIZE;

	if (reader->size > maximum)
		return DOS_LOADER_IMAGE_TOO_LARGE;
	plan->format = (uint8_t)DOS_IMAGE_COM;
	plan->target_kind = (uint8_t)target;
	plan->file_size = reader->size;
	plan->image_file_offset = 0u;
	plan->image_size = reader->size;
	plan->minimum_image_paragraphs =
	    (reader->size + DOS_EXEC_PARAGRAPH_BYTES - 1u) /
	    DOS_EXEC_PARAGRAPH_BYTES;
	plan->initial_ip = COM_LOAD_OFFSET;
	return DOS_LOADER_OK;
}

static enum dos_loader_status
build_mz_plan(const struct dos_image_reader *reader,
	      const uint8_t header[EXEC_HEADER_BYTES],
	      enum dos_load_target_kind target, struct dos_load_plan *plan)
{
	uint64_t header_size;
	uint16_t pages = read_le16(header + MZ_PAGE_COUNT_OFFSET);
	uint16_t relocation_count =
	    read_le16(header + MZ_RELOCATION_COUNT_OFFSET);
	uint16_t header_paragraphs =
	    read_le16(header + MZ_HEADER_PARAGRAPHS_OFFSET);
	uint16_t minimum_extra =
	    read_le16(header + MZ_MINIMUM_EXTRA_OFFSET);
	uint16_t maximum_extra =
	    read_le16(header + MZ_MAXIMUM_EXTRA_OFFSET);
	uint16_t relocation_offset =
	    read_le16(header + MZ_RELOCATION_TABLE_OFFSET);
	uint16_t page_paragraphs;
	uint16_t resident_paragraphs;

	if (check_mul_overflow((uint64_t)header_paragraphs,
			       (uint64_t)DOS_EXEC_PARAGRAPH_BYTES,
			       &header_size))
		return DOS_LOADER_RANGE_OVERFLOW;

	/*
	 * This calculation never references exe_len_mod_512. AX is a
	 * 16-bit register, so both the five-bit SHL and following SUB wrap.
	 * Widening pages before this calculation changes accepted DOS images.
	 */
	page_paragraphs =
	    (uint16_t)((uint32_t)pages << MZ_PAGE_TO_PARAGRAPH_SHIFT);
	resident_paragraphs = (uint16_t)(page_paragraphs - header_paragraphs);

	plan->format = (uint8_t)DOS_IMAGE_MZ;
	plan->target_kind = (uint8_t)target;
	plan->file_size = reader->size;
	plan->image_file_offset = header_size;
	plan->image_size =
	    (uint64_t)resident_paragraphs * DOS_EXEC_PARAGRAPH_BYTES;
	plan->minimum_image_paragraphs = resident_paragraphs;
	plan->minimum_extra_paragraphs = minimum_extra;
	plan->maximum_extra_paragraphs = maximum_extra;
	plan->initial_ss = read_le16(header + MZ_INITIAL_SS_OFFSET);
	plan->initial_sp = read_le16(header + MZ_INITIAL_SP_OFFSET);
	plan->initial_ip = read_le16(header + MZ_INITIAL_IP_OFFSET);
	plan->initial_cs = read_le16(header + MZ_INITIAL_CS_OFFSET);
	plan->relocation_count = relocation_count;
	plan->relocation_table_offset = relocation_offset;
	plan->old_mz_signature =
	    read_le16(header + MZ_SIGNATURE_OFFSET) == MZ_OLD_SIGNATURE ? 1u : 0u;
	plan->load_high = maximum_extra == 0u ? 1u : 0u;
	return DOS_LOADER_OK;
}

bool dos_load_plan_has_inspected_encoding(const struct dos_load_plan *plan)
{
	file_offset_t com_maximum;

	if (!dos_load_plan_has_valid_encoding(plan))
		return false;
	if (plan->format == (uint8_t)DOS_IMAGE_COM) {
		com_maximum =
		    plan->target_kind == (uint8_t)DOS_LOAD_TARGET_OVERLAY
			? (file_offset_t)COM_OVERLAY_MAXIMUM_FILE_SIZE
			: (file_offset_t)COM_PROCESS_MAXIMUM_FILE_SIZE;
		return plan->old_mz_signature == 0u && plan->load_high == 0u &&
		       plan->file_size != 0u && plan->file_size <= com_maximum &&
		       plan->image_file_offset == 0u &&
		       plan->image_size == plan->file_size &&
		       plan->minimum_image_paragraphs ==
			   (plan->file_size + DOS_EXEC_PARAGRAPH_BYTES - 1u) /
			       DOS_EXEC_PARAGRAPH_BYTES &&
		       plan->minimum_extra_paragraphs == 0u &&
		       plan->maximum_extra_paragraphs == 0u &&
		       plan->initial_cs == 0u &&
		       plan->initial_ip == COM_LOAD_OFFSET &&
		       plan->initial_ss == 0u && plan->initial_sp == 0u &&
		       plan->relocation_count == 0u &&
		       plan->relocation_table_offset == 0u;
	}
	if (plan->format != (uint8_t)DOS_IMAGE_MZ)
		return false;
	return dos_load_plan_has_wrapped_mz_geometry(plan);
}

enum dos_loader_status
dos_loader_inspect_target(const struct dos_image_reader *reader,
			  enum dos_load_target_kind target,
			  struct dos_load_plan *plan)
{
	uint8_t header[EXEC_HEADER_BYTES] = {0u};
	size_t header_count;
	uint16_t signature;
	enum dos_loader_status status;

	if (plan != NULL)
		*plan = (struct dos_load_plan){0};
	if (reader == NULL || reader->read == NULL || plan == NULL ||
	    (target != DOS_LOAD_TARGET_PROCESS &&
	     target != DOS_LOAD_TARGET_OVERLAY))
		return DOS_LOADER_INVALID_ARGUMENT;
	if (reader->size == 0u)
		return DOS_LOADER_EMPTY_IMAGE;
	header_count = reader->size < EXEC_HEADER_BYTES ? (size_t)reader->size
							: EXEC_HEADER_BYTES;
	status = read_header(reader, header, header_count);
	if (status != DOS_LOADER_OK)
		return status;
	if (header_count != EXEC_HEADER_BYTES)
		return build_com_plan(reader, target, plan);
	signature = read_le16(header + MZ_SIGNATURE_OFFSET);
	if (signature != MZ_SIGNATURE && signature != MZ_OLD_SIGNATURE)
		return build_com_plan(reader, target, plan);
	return build_mz_plan(reader, header, target, plan);
}

enum dos_loader_status dos_loader_inspect(const struct dos_image_reader *reader,
					  struct dos_load_plan *plan)
{
	return dos_loader_inspect_target(reader, DOS_LOAD_TARGET_PROCESS, plan);
}

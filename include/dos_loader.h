/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Safe MS-DOS COM/MZ inspection boundary.
 * Process and overlay targets remain distinct because their COM read counts
 * and equality-failure limits are observably different.
 */
#ifndef DOSC32_DOS_LOADER_H
#define DOSC32_DOS_LOADER_H

#include "address.h"
#include "compiler.h"
#include "dos_image.h"
#include "types.h"

enum dos_loader_status {
	DOS_LOADER_OK = 0,
	DOS_LOADER_INVALID_ARGUMENT,
	DOS_LOADER_EMPTY_IMAGE,
	DOS_LOADER_IO_ERROR,
	DOS_LOADER_SHORT_READ,
	DOS_LOADER_BAD_FORMAT,
	DOS_LOADER_IMAGE_TOO_LARGE,
	DOS_LOADER_RANGE_OVERFLOW
};

/*
 * EXEC0/EXEC1 reserve a PSP and require a COM read to finish below FEFFh.
 * EXEC3 requests FFFFh bytes at the caller's overlay address and therefore
 * accepts at most FFFEh.  This is loader geometry, not a host file mode.
 */
enum dos_load_target_kind {
	DOS_LOAD_TARGET_PROCESS = 0,
	DOS_LOAD_TARGET_OVERLAY = 1
};

enum dos_image_read_status { DOS_IMAGE_READ_OK = 0, DOS_IMAGE_READ_IO_ERROR };

/* The private read stops after exe_rle_table, before the complete MZ header. */
#define DOS_EXEC_PRIVATE_MZ_HEADER_BYTES 26u
#define DOS_EXEC_PARAGRAPH_BYTES 16u
#define DOS_EXEC_PARAGRAPH_ALIGNMENT_MASK (DOS_EXEC_PARAGRAPH_BYTES - 1u)
#define DOS_EXEC_MZ_PAGE_PARAGRAPHS 32u
#define DOS_EXEC_MZ_PAGE_PARAGRAPH_MASK \
	(DOS_EXEC_MZ_PAGE_PARAGRAPHS - 1u)
#define DOS_EXEC_MAX_16BIT_PARAGRAPH_BYTES \
	(0xffffu * DOS_EXEC_PARAGRAPH_BYTES)

struct dos_image_reader {
	kernel_object_handle_t context;
	file_offset_t size;
	enum dos_image_read_status (*read)(kernel_object_handle_t context,
					   file_offset_t offset,
					   void *destination,
					   size_t destination_capacity,
					   size_t count, size_t *bytes_read);
};

struct dos_load_plan {
	/* Persistent value fields use explicit widths, never C enum/bool size.
	 */
	uint8_t format;
	uint8_t old_mz_signature;
	uint8_t load_high;
	uint8_t target_kind;
	uint32_t reserved32;
	file_offset_t file_size;
	file_offset_t image_file_offset;
	/*
	 * COM: bytes present in the file. MZ: the requested resident
	 * byte count, exactly exec_res_len_para << 4; it is independent of
	 * exe_len_mod_512 and can exceed the bytes physically present.
	 */
	uint64_t image_size;
	/*
	 * COM: paragraphs covering image_size.  MZ: the exact wrapped
	 * 16-bit exec_res_len_para value.  The historical field name is kept
	 * to preserve the fixed value-object layout.
	 */
	uint64_t minimum_image_paragraphs;
	uint16_t minimum_extra_paragraphs;
	uint16_t maximum_extra_paragraphs;
	uint16_t initial_cs;
	uint16_t initial_ip;
	uint16_t initial_ss;
	uint16_t initial_sp;
	uint16_t relocation_count;
	uint16_t relocation_table_offset;
} __aligned(8);

static inline bool dos_load_target_value_is_valid(uint8_t target_kind)
{
	return target_kind == (uint8_t)DOS_LOAD_TARGET_PROCESS ||
	       target_kind == (uint8_t)DOS_LOAD_TARGET_OVERLAY;
}

static inline bool
dos_load_plan_has_valid_encoding(const struct dos_load_plan *plan)
{
	return plan != NULL && dos_image_format_value_is_valid(plan->format) &&
	       plan->old_mz_signature <= 1u && plan->load_high <= 1u &&
	       dos_load_target_value_is_valid(plan->target_kind) &&
	       plan->reserved32 == 0u;
}

/*
 * Validate the decoded MZ value geometry without reconstructing a
 * modern logical file length.  The low five zero bits are the invariant left
 * by wrapped `SHL AX,5` followed by `SUB AX,header_paragraphs` arithmetic.
 */
static inline bool
dos_load_plan_has_wrapped_mz_geometry(const struct dos_load_plan *plan)
{
	uint64_t header_paragraphs;
	uint64_t resident_paragraphs;
	uint16_t page_paragraphs;

	if (!dos_load_plan_has_valid_encoding(plan) ||
	    plan->format != (uint8_t)DOS_IMAGE_MZ ||
	    plan->file_size < DOS_EXEC_PRIVATE_MZ_HEADER_BYTES ||
	    (plan->image_file_offset & DOS_EXEC_PARAGRAPH_ALIGNMENT_MASK) != 0u ||
	    (plan->image_size & DOS_EXEC_PARAGRAPH_ALIGNMENT_MASK) != 0u ||
	    plan->image_file_offset > DOS_EXEC_MAX_16BIT_PARAGRAPH_BYTES ||
	    plan->image_size > DOS_EXEC_MAX_16BIT_PARAGRAPH_BYTES)
		return false;
	header_paragraphs = plan->image_file_offset / DOS_EXEC_PARAGRAPH_BYTES;
	resident_paragraphs = plan->image_size / DOS_EXEC_PARAGRAPH_BYTES;
	page_paragraphs = (uint16_t)((uint16_t)header_paragraphs +
				     (uint16_t)resident_paragraphs);
	return plan->minimum_image_paragraphs == resident_paragraphs &&
	       (page_paragraphs & DOS_EXEC_MZ_PAGE_PARAGRAPH_MASK) == 0u &&
	       plan->load_high == (plan->maximum_extra_paragraphs == 0u);
}

/*
 * Pure validator for a plan actually produced by the private EXEC header
 * inspection.  DOS_IMAGE_NATIVE32 is intentionally excluded: it is not a
 * MS-DOS classification result. This stricter form is suitable for a
 * persistent transaction slot; the lighter flag validator above remains
 * useful while a plan is being assembled.
 */
bool dos_load_plan_has_inspected_encoding(
    const struct dos_load_plan *plan) __must_check;

static_assert_expression(sizeof(struct dos_load_plan) == 56,
			 "load plan must be data-model independent");
static_assert_expression(__alignof__(struct dos_load_plan) == 8,
			 "load plan must remain explicitly 8-byte aligned");
static_assert_expression(__builtin_offsetof(struct dos_load_plan, format) == 0,
			 "load-plan format offset changed");
static_assert_expression(__builtin_offsetof(struct dos_load_plan, file_size) ==
			     8,
			 "load-plan file-size offset changed");
static_assert_expression(__builtin_offsetof(struct dos_load_plan, image_size) ==
			     24,
			 "load-plan image-size offset changed");
static_assert_expression(__builtin_offsetof(struct dos_load_plan,
					    relocation_table_offset) == 54,
			 "load-plan relocation offset changed");

enum dos_loader_status
dos_loader_inspect(const struct dos_image_reader *reader,
		   struct dos_load_plan *plan) __must_check;

/*
 * Explicit AL=3 geometry.  The ordinary wrapper above always selects a
 * process target and remains the API for EXEC0/EXEC1.  Output follows the
 * same all-or-zero convention as dos_loader_inspect().
 */
enum dos_loader_status
dos_loader_inspect_target(const struct dos_image_reader *reader,
			  enum dos_load_target_kind target,
			  struct dos_load_plan *plan) __must_check;

#endif

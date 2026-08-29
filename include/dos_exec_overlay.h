/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Pure EXEC3 overlay-target planning.
 *
 * EXEC3 does not construct a PSP, scan or allocate an environment, allocate
 * a process block, inherit a JFT, or publish CurrentPDB.  Its caller supplies
 * the load segment.  An MZ caller additionally supplies an independent
 * relocation factor, while a COM caller exposes only the first word of the
 * Exec3 parameter block. Separate entry points keep that access rule
 * visible in the C interface.
 */
#ifndef DOSC32_DOS_EXEC_OVERLAY_H
#define DOSC32_DOS_EXEC_OVERLAY_H

#include "compiler.h"
#include "dos_loader.h"
#include "types.h"

#define DOS_EXEC_COM_OVERLAY_READ_CAPACITY 0xffffu

enum dos_exec_overlay_status {
	DOS_EXEC_OVERLAY_OK = 0,
	DOS_EXEC_OVERLAY_INVALID_ARGUMENT,
	DOS_EXEC_OVERLAY_WRONG_IMAGE_FORMAT,
	DOS_EXEC_OVERLAY_STALE_IMAGE_PLAN
};

/* Decoded only after the loader has classified an EXEC3 image as MZ. */
struct dos_exec_mz_overlay_target {
	uint16_t load_segment;
	uint16_t relocation_factor;
};

/*
 * COM uses the caller segment at offset zero and asks ExecRead for ffffH
 * bytes.  DOS treats an exactly full read as insufficient memory, so the
 * inspected image is strictly smaller than read_capacity.
 */
struct dos_exec_com_overlay_plan {
	uint16_t load_segment;
	uint16_t load_offset;
	dos_linear_address_t load_linear_address;
	uint32_t image_size;
	uint32_t read_capacity;
	uint64_t reserved;
} __aligned(8);

/*
 * MZ resident geometry comes from dos_loader's MS-DOS-compatible wrapped
 * page/header calculation.  relocation_factor is deliberately not derived
 * from load_segment: Exec3 defines them as separate caller values.
 */
struct dos_exec_mz_overlay_plan {
	uint16_t load_segment;
	uint16_t load_offset;
	dos_linear_address_t load_linear_address;
	file_offset_t image_file_offset;
	uint32_t image_size;
	uint32_t resident_paragraphs;
	uint16_t relocation_factor;
	uint16_t relocation_count;
	uint16_t relocation_table_offset;
	uint16_t reserved;
} __aligned(8);

static inline bool dos_exec_com_overlay_plan_has_valid_encoding(
    const struct dos_exec_com_overlay_plan *plan)
{
	return plan != NULL && plan->load_offset == 0u &&
	       plan->load_linear_address ==
		   (dos_linear_address_t)((uint32_t)plan->load_segment << 4) &&
	       plan->image_size != 0u &&
	       plan->read_capacity == DOS_EXEC_COM_OVERLAY_READ_CAPACITY &&
	       plan->image_size < plan->read_capacity && plan->reserved == 0u;
}

static inline bool dos_exec_mz_overlay_plan_has_valid_encoding(
    const struct dos_exec_mz_overlay_plan *plan)
{
	uint16_t page_paragraphs;

	if (plan == NULL || plan->load_offset != 0u ||
	    plan->load_linear_address !=
		(dos_linear_address_t)((uint32_t)plan->load_segment << 4) ||
	    plan->image_file_offset > 0xffff0u ||
	    (plan->image_file_offset & 0x0fu) != 0u ||
	    plan->resident_paragraphs > 0xffffu ||
	    plan->image_size != plan->resident_paragraphs * 16u ||
	    plan->reserved != 0u)
		return false;
	page_paragraphs = (uint16_t)((uint16_t)(plan->image_file_offset >> 4) +
				     (uint16_t)plan->resident_paragraphs);
	return (page_paragraphs & 0x1fu) == 0u;
}

static_assert_expression(sizeof(struct dos_exec_mz_overlay_target) == 4,
			 "decoded EXEC3 MZ target must remain four bytes");
static_assert_expression(__builtin_offsetof(struct dos_exec_mz_overlay_target,
					    relocation_factor) == 2,
			 "decoded EXEC3 relocation-factor offset changed");
static_assert_expression(sizeof(struct dos_exec_com_overlay_plan) == 24,
			 "COM overlay plan must be data-model independent");
static_assert_expression(__alignof__(struct dos_exec_com_overlay_plan) == 8,
			 "COM overlay plan must remain explicitly aligned");
static_assert_expression(__builtin_offsetof(struct dos_exec_com_overlay_plan,
					    load_linear_address) == 4,
			 "COM overlay linear-address offset changed");
static_assert_expression(__builtin_offsetof(struct dos_exec_com_overlay_plan,
					    reserved) == 16,
			 "COM overlay reserved offset changed");
static_assert_expression(sizeof(struct dos_exec_mz_overlay_plan) == 32,
			 "MZ overlay plan must be data-model independent");
static_assert_expression(__alignof__(struct dos_exec_mz_overlay_plan) == 8,
			 "MZ overlay plan must remain explicitly aligned");
static_assert_expression(__builtin_offsetof(struct dos_exec_mz_overlay_plan,
					    image_file_offset) == 8,
			 "MZ overlay file-offset field moved");
static_assert_expression(__builtin_offsetof(struct dos_exec_mz_overlay_plan,
					    relocation_factor) == 24,
			 "MZ overlay relocation-factor field moved");

/* Segment zero is a DOS value, not a native NULL sentinel. */
enum dos_exec_overlay_status dos_exec_overlay_plan_com(
    const struct dos_load_plan *image_plan, uint16_t load_segment,
    struct dos_exec_com_overlay_plan *overlay_plan) __must_check;

enum dos_exec_overlay_status dos_exec_overlay_plan_mz(
    const struct dos_load_plan *image_plan,
    const struct dos_exec_mz_overlay_target *target,
    struct dos_exec_mz_overlay_plan *overlay_plan) __must_check;

#endif

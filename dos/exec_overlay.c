// SPDX-License-Identifier: GPL-2.0-only
/*
 * EXEC3 overlay-target planning.
 *
 * MS-DOS supplies the observable classification, parameter access, and
 * 16-bit values. The fixed-width all-or-unchanged value objects are private
 * safety infrastructure.
 */
#include "dos_exec_overlay.h"

#include "overflow.h"

#define DOS_EXEC_COM_INITIAL_IP 0x100u
#define DOS_EXEC_COM_MAXIMUM_IMAGE_SIZE 0xfffeu
#define DOS_PARAGRAPH_BYTES 16u

static enum dos_exec_overlay_status
validate_overlay_image_plan(const struct dos_load_plan *image_plan,
			    uint8_t expected_format)
{
	if (!dos_load_plan_has_valid_encoding(image_plan) ||
	    image_plan->target_kind != (uint8_t)DOS_LOAD_TARGET_OVERLAY)
		return DOS_EXEC_OVERLAY_INVALID_ARGUMENT;
	if (image_plan->format != expected_format)
		return DOS_EXEC_OVERLAY_WRONG_IMAGE_FORMAT;
	return DOS_EXEC_OVERLAY_OK;
}

static bool com_image_plan_is_source_exact(const struct dos_load_plan *plan)
{
	uint64_t rounded_size;

	if (check_add_overflow(plan->image_size,
			       (uint64_t)DOS_PARAGRAPH_BYTES - 1u,
			       &rounded_size))
		return false;
	return plan->old_mz_signature == 0u && plan->load_high == 0u &&
	       plan->file_size == plan->image_size &&
	       plan->image_file_offset == 0u && plan->image_size != 0u &&
	       plan->image_size <= DOS_EXEC_COM_MAXIMUM_IMAGE_SIZE &&
	       plan->minimum_image_paragraphs ==
		   rounded_size / DOS_PARAGRAPH_BYTES &&
	       plan->minimum_extra_paragraphs == 0u &&
	       plan->maximum_extra_paragraphs == 0u && plan->initial_cs == 0u &&
	       plan->initial_ip == DOS_EXEC_COM_INITIAL_IP &&
	       plan->initial_ss == 0u && plan->initial_sp == 0u &&
	       plan->relocation_count == 0u &&
	       plan->relocation_table_offset == 0u;
}

enum dos_exec_overlay_status
dos_exec_overlay_plan_com(const struct dos_load_plan *image_plan,
			  uint16_t load_segment,
			  struct dos_exec_com_overlay_plan *overlay_plan)
{
	struct dos_exec_com_overlay_plan staging = {0};
	enum dos_exec_overlay_status status;

	if (overlay_plan == NULL)
		return DOS_EXEC_OVERLAY_INVALID_ARGUMENT;
	status = validate_overlay_image_plan(image_plan, DOS_IMAGE_COM);
	if (status != DOS_EXEC_OVERLAY_OK)
		return status;
	if (!com_image_plan_is_source_exact(image_plan))
		return DOS_EXEC_OVERLAY_STALE_IMAGE_PLAN;

	staging.load_segment = load_segment;
	staging.load_linear_address =
	    (dos_linear_address_t)((uint32_t)load_segment << 4);
	staging.image_size = (uint32_t)image_plan->image_size;
	staging.read_capacity = DOS_EXEC_COM_OVERLAY_READ_CAPACITY;
	*overlay_plan = staging;
	return DOS_EXEC_OVERLAY_OK;
}

enum dos_exec_overlay_status
dos_exec_overlay_plan_mz(const struct dos_load_plan *image_plan,
			 const struct dos_exec_mz_overlay_target *target,
			 struct dos_exec_mz_overlay_plan *overlay_plan)
{
	struct dos_exec_mz_overlay_plan staging = {0};
	enum dos_exec_overlay_status status;

	if (target == NULL || overlay_plan == NULL)
		return DOS_EXEC_OVERLAY_INVALID_ARGUMENT;
	status = validate_overlay_image_plan(image_plan, DOS_IMAGE_MZ);
	if (status != DOS_EXEC_OVERLAY_OK)
		return status;
	if (!dos_load_plan_has_wrapped_mz_geometry(image_plan) ||
	    image_plan->image_size > (uint64_t)(uint32_t)-1 ||
	    image_plan->minimum_image_paragraphs > (uint64_t)(uint32_t)-1)
		return DOS_EXEC_OVERLAY_STALE_IMAGE_PLAN;

	staging.load_segment = target->load_segment;
	staging.load_linear_address =
	    (dos_linear_address_t)((uint32_t)target->load_segment << 4);
	staging.image_file_offset = image_plan->image_file_offset;
	staging.image_size = (uint32_t)image_plan->image_size;
	staging.resident_paragraphs =
	    (uint32_t)image_plan->minimum_image_paragraphs;
	staging.relocation_factor = target->relocation_factor;
	staging.relocation_count = image_plan->relocation_count;
	staging.relocation_table_offset = image_plan->relocation_table_offset;
	*overlay_plan = staging;
	return DOS_EXEC_OVERLAY_OK;
}

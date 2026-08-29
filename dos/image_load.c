// SPDX-License-Identifier: GPL-2.0-only
/*
 * DOS EXEC resident-image loading.
 *
 * Wrapped page geometry, short final MZ reads and stack words follow MS-DOS
 * behavior. Checked arithmetic, prepare-before-publish and bounded staging
 * keep loading transactional.
 */
#include "dos_image_load.h"

#include "overflow.h"

#define DOS_IMAGE_LOAD_BUFFER_BYTES 256u
#define DOS_PARAGRAPH_BYTES 16u
#define DOS_PSP_PARAGRAPHS 0x10u
#define DOS_COM_ADDRESS_SPACE_BYTES 0x10000u
#define DOS_COM_LOAD_OFFSET 0x100u
#define DOS_COM_MAXIMUM_IMAGE_BYTES 0xfeffu
#define DOS_MZ_PAGE_BYTES 512u
/* Resident MZ reads preserve these outer transfer boundaries. */
#define DOS_MZ_EXEC_BIG_THRESHOLD_PARAGRAPHS 0x1000u
#define DOS_MZ_EXEC_BIG_CHUNK_PARAGRAPHS 0x0fe0u

struct lease_geometry {
	uint16_t end_segment;
	uint64_t begin_linear;
	uint64_t end_linear;
};

static bool machine_is_usable(const struct dos_machine *machine)
{
	return machine != NULL && machine->ops != NULL &&
	       machine->ops->read_memory != NULL &&
	       machine->ops->write_memory != NULL &&
	       machine->address_limit != 0u &&
	       machine->address_limit <= DOS_GUEST_32_ADDRESS_LIMIT;
}

static enum dos_image_load_status
validate_lease_view(const struct dos_machine *machine,
		    const struct dos_memory_lease_view *lease_view,
		    struct lease_geometry *geometry)
{
	uint64_t end_segment;

	if (!machine_is_usable(machine) || lease_view == NULL ||
	    geometry == NULL || lease_view->handle.value == 0u ||
	    lease_view->handle.value == KERNEL_OBJECT_HANDLE_INVALID ||
	    lease_view->machine_context == 0u ||
	    lease_view->machine_context == KERNEL_OBJECT_HANDLE_INVALID ||
	    lease_view->arena_identity == 0u ||
	    lease_view->arena_identity == KERNEL_OBJECT_HANDLE_INVALID ||
	    lease_view->arena_generation == 0u || lease_view->owner == 0u ||
	    lease_view->paragraphs == 0u || lease_view->reserved != 0u)
		return DOS_IMAGE_LOAD_INVALID_ARGUMENT;
	if (lease_view->machine_context != machine->context)
		return DOS_IMAGE_LOAD_BAD_LEASE;
	if (check_add_overflow((uint64_t)lease_view->guest_segment,
			       (uint64_t)lease_view->paragraphs,
			       &end_segment) ||
	    end_segment > 0xffffu)
		return DOS_IMAGE_LOAD_BAD_LEASE;
	geometry->end_segment = (uint16_t)end_segment;
	geometry->begin_linear = (uint64_t)lease_view->guest_segment << 4;
	geometry->end_linear = end_segment << 4;
	return DOS_IMAGE_LOAD_OK;
}

static bool range_is_in_lease(const struct lease_geometry *geometry,
			      uint64_t start, uint64_t count)
{
	uint64_t end;

	return !check_add_overflow(start, count, &end) &&
	       start >= geometry->begin_linear && end <= geometry->end_linear;
}

static enum dos_image_load_status
validate_process_lease(uint16_t psp_segment, uint16_t block_end_segment,
		       const struct dos_memory_lease_view *lease_view,
		       const struct lease_geometry *geometry)
{
	if (psp_segment != lease_view->guest_segment ||
	    block_end_segment != geometry->end_segment ||
	    lease_view->paragraphs < DOS_PSP_PARAGRAPHS)
		return DOS_IMAGE_LOAD_BAD_LEASE;
	return DOS_IMAGE_LOAD_OK;
}

static enum dos_image_load_status
validate_com_process_geometry(const struct dos_com_process_plan *plan,
			      const struct dos_memory_lease_view *lease_view,
			      const struct lease_geometry *geometry)
{
	uint64_t block_bytes;
	uint64_t expected_capacity;
	uint64_t expected_load_segment;
	uint64_t expected_load_linear;
	uint64_t expected_stack;
	enum dos_image_load_status status;

	if (!dos_com_process_plan_has_valid_encoding(plan))
		return DOS_IMAGE_LOAD_INVALID_ARGUMENT;
	status = validate_process_lease(
	    plan->psp_segment, plan->block_end_segment, lease_view, geometry);
	if (status != DOS_IMAGE_LOAD_OK)
		return status;
	block_bytes = (uint64_t)lease_view->paragraphs << 4;
	if (block_bytes <= DOS_COM_LOAD_OFFSET)
		return DOS_IMAGE_LOAD_BAD_RESIDENT_RANGE;
	expected_capacity =
	    block_bytes >= DOS_COM_ADDRESS_SPACE_BYTES
		? DOS_COM_ADDRESS_SPACE_BYTES - DOS_COM_LOAD_OFFSET
		: block_bytes - DOS_COM_LOAD_OFFSET;
	expected_load_segment =
	    (uint64_t)plan->psp_segment + DOS_PSP_PARAGRAPHS;
	expected_load_linear =
	    ((uint64_t)plan->load_segment << 4) + plan->load_offset;
	if (expected_load_segment > 0xffffu || plan->load_offset != 0u ||
	    plan->load_segment != (uint16_t)expected_load_segment ||
	    plan->load_linear_address != expected_load_linear ||
	    plan->read_capacity != expected_capacity ||
	    plan->image_size >= plan->read_capacity ||
	    !range_is_in_lease(geometry, expected_load_linear,
			       plan->read_capacity))
		return DOS_IMAGE_LOAD_BAD_RESIDENT_RANGE;
	if (check_add_overflow(expected_capacity, (uint64_t)0xfeu,
			       &expected_stack) ||
	    expected_stack > 0xffffu || expected_stack < 2u ||
	    plan->stack_sentinel_offset != (uint16_t)expected_stack ||
	    plan->stack_sentinel_value != 0u ||
	    plan->load_only_stack_pointer !=
		(uint16_t)(plan->stack_sentinel_offset - 2u) ||
	    plan->initial_state.ss != plan->psp_segment ||
	    dos_register_low16(plan->initial_state.esp) !=
		plan->stack_sentinel_offset ||
	    plan->load_only_stack_value !=
		dos_register_low16(plan->initial_state.ebx))
		return DOS_IMAGE_LOAD_STALE_PLAN;
	return DOS_IMAGE_LOAD_OK;
}

static enum dos_image_load_status
validate_mz_process_geometry(const struct dos_mz_process_plan *plan,
			     const struct dos_memory_lease_view *lease_view,
			     const struct lease_geometry *geometry,
			     uint64_t *resident_bytes)
{
	uint64_t load_linear;
	uint64_t bytes;
	uint16_t initial_sp;
	enum dos_image_load_status status;

	if (resident_bytes == NULL ||
	    !dos_mz_process_plan_has_valid_encoding(plan))
		return DOS_IMAGE_LOAD_INVALID_ARGUMENT;
	status = validate_process_lease(
	    plan->psp_segment, plan->block_end_segment, lease_view, geometry);
	if (status != DOS_IMAGE_LOAD_OK)
		return status;
	if (check_mul_overflow((uint64_t)plan->resident_paragraphs,
			       (uint64_t)DOS_PARAGRAPH_BYTES, &bytes))
		return DOS_IMAGE_LOAD_BAD_RESIDENT_RANGE;
	load_linear = ((uint64_t)plan->load_segment << 4) + plan->load_offset;
	if (plan->load_offset != 0u ||
	    plan->load_linear_address != load_linear ||
	    plan->image_size > bytes ||
	    !range_is_in_lease(geometry, load_linear, bytes) ||
	    plan->relocation_factor != plan->load_segment)
		return DOS_IMAGE_LOAD_BAD_RESIDENT_RANGE;
	initial_sp = dos_register_low16(plan->initial_state.esp);
	if (plan->load_only_stack_pointer != (uint16_t)(initial_sp - 2u) ||
	    plan->load_only_stack_value !=
		dos_register_low16(plan->initial_state.ebx))
		return DOS_IMAGE_LOAD_STALE_PLAN;
	*resident_bytes = bytes;
	return DOS_IMAGE_LOAD_OK;
}

static enum dos_image_load_status
validate_reader(const struct dos_image_reader *reader,
		const struct dos_load_plan *image_plan)
{
	if (reader == NULL || reader->read == NULL ||
	    !dos_load_plan_has_valid_encoding(image_plan) ||
	    image_plan->target_kind != (uint8_t)DOS_LOAD_TARGET_PROCESS)
		return DOS_IMAGE_LOAD_INVALID_ARGUMENT;
	if (reader->size != image_plan->file_size)
		return DOS_IMAGE_LOAD_STALE_PLAN;
	return DOS_IMAGE_LOAD_OK;
}

static enum dos_image_load_status
validate_com_image(const struct dos_load_plan *image_plan,
		   const struct dos_com_process_plan *process_plan)
{
	uint64_t rounded_size;
	uint64_t paragraphs;

	if (image_plan->format != DOS_IMAGE_COM)
		return DOS_IMAGE_LOAD_WRONG_IMAGE_FORMAT;
	if (check_add_overflow(image_plan->image_size,
			       (uint64_t)DOS_PARAGRAPH_BYTES - 1u,
			       &rounded_size))
		return DOS_IMAGE_LOAD_FILE_RANGE_OVERFLOW;
	paragraphs = rounded_size >> 4;
	if (image_plan->image_file_offset != 0u ||
	    image_plan->file_size != image_plan->image_size ||
	    image_plan->image_size == 0u ||
	    image_plan->image_size > DOS_COM_MAXIMUM_IMAGE_BYTES ||
	    image_plan->minimum_image_paragraphs != paragraphs ||
	    image_plan->initial_ip != DOS_COM_LOAD_OFFSET ||
	    image_plan->relocation_count != 0u ||
	    process_plan->image_size != image_plan->image_size)
		return DOS_IMAGE_LOAD_STALE_PLAN;
	return DOS_IMAGE_LOAD_OK;
}

static uint32_t mz_exec_chunk_paragraphs(uint32_t remaining)
{
	return remaining >= DOS_MZ_EXEC_BIG_THRESHOLD_PARAGRAPHS
		   ? DOS_MZ_EXEC_BIG_CHUNK_PARAGRAPHS
		   : remaining;
}

static enum dos_image_load_status
source_mz_file_bytes(const struct dos_load_plan *image_plan,
		     uint32_t resident_paragraphs, uint32_t *file_bytes)
{
	uint64_t available = 0u;
	uint32_t remaining = resident_paragraphs;
	uint32_t completed = 0u;

	if (file_bytes == NULL)
		return DOS_IMAGE_LOAD_INVALID_ARGUMENT;
	if (image_plan->file_size > image_plan->image_file_offset)
		available =
		    image_plan->file_size - image_plan->image_file_offset;

	while (remaining != 0u) {
		uint32_t paragraphs = mz_exec_chunk_paragraphs(remaining);
		uint32_t requested = paragraphs * DOS_PARAGRAPH_BYTES;
		uint32_t read_count =
		    available < requested ? (uint32_t)available : requested;

		/* MS-DOS rejects a per-read deficit of 512 or more.
		 */
		if (requested - read_count >= DOS_MZ_PAGE_BYTES)
			return DOS_IMAGE_LOAD_BAD_FILE_RANGE;
		completed += read_count;
		available -= read_count;
		remaining -= paragraphs;
	}
	*file_bytes = completed;
	return DOS_IMAGE_LOAD_OK;
}

static enum dos_image_load_status
validate_mz_image(const struct dos_load_plan *image_plan,
		  const struct dos_mz_process_plan *process_plan,
		  uint64_t resident_bytes, uint32_t *file_bytes)
{
	uint64_t expected_resident;
	uint64_t expected_load_segment;
	uint16_t expected_cs;
	uint16_t expected_ss;
	enum dos_image_load_status status;

	if (file_bytes == NULL)
		return DOS_IMAGE_LOAD_INVALID_ARGUMENT;
	if (image_plan->format != DOS_IMAGE_MZ)
		return DOS_IMAGE_LOAD_WRONG_IMAGE_FORMAT;
	if (!dos_load_plan_has_wrapped_mz_geometry(image_plan))
		return DOS_IMAGE_LOAD_STALE_PLAN;
	expected_resident = image_plan->image_size;
	if (image_plan->load_high) {
		if (process_plan->resident_paragraphs >
		    process_plan->block_end_segment)
			return DOS_IMAGE_LOAD_STALE_PLAN;
		expected_load_segment =
		    (uint64_t)process_plan->block_end_segment -
		    process_plan->resident_paragraphs;
	} else {
		expected_load_segment =
		    (uint64_t)process_plan->psp_segment + DOS_PSP_PARAGRAPHS;
	}
	/* CS and SS relocation uses wrapping 16-bit additions. */
	expected_cs = (uint16_t)(expected_load_segment +
				 (uint64_t)image_plan->initial_cs);
	expected_ss = (uint16_t)(expected_load_segment +
				 (uint64_t)image_plan->initial_ss);
	if (expected_resident != resident_bytes ||
	    expected_load_segment > 0xffffu ||
	    process_plan->load_segment != (uint16_t)expected_load_segment ||
	    process_plan->image_file_offset != image_plan->image_file_offset ||
	    process_plan->image_size != image_plan->image_size ||
	    process_plan->resident_paragraphs != expected_resident / 16u ||
	    process_plan->relocation_count != image_plan->relocation_count ||
	    process_plan->relocation_table_offset !=
		image_plan->relocation_table_offset ||
	    process_plan->load_high != image_plan->load_high ||
	    process_plan->initial_state.cs != expected_cs ||
	    process_plan->initial_state.ss != expected_ss ||
	    dos_register_low16(process_plan->initial_state.eip) !=
		image_plan->initial_ip ||
	    dos_register_low16(process_plan->initial_state.esp) !=
		image_plan->initial_sp)
		return DOS_IMAGE_LOAD_STALE_PLAN;
	status = source_mz_file_bytes(
	    image_plan, process_plan->resident_paragraphs, file_bytes);
	return status;
}

static enum dos_image_load_status
preflight_com_target(const struct dos_machine *machine,
		     const struct dos_com_process_plan *plan)
{
	enum dos_machine_status status;

	status =
	    dos_machine_validate_far(machine, plan->load_segment,
				     plan->load_offset, plan->read_capacity);
	return status == DOS_MACHINE_OK ? DOS_IMAGE_LOAD_OK
					: DOS_IMAGE_LOAD_BAD_RESIDENT_RANGE;
}

static enum dos_image_load_status
preflight_mz_target(const struct dos_machine *machine,
		    const struct dos_mz_process_plan *plan)
{
	uint32_t remaining = plan->resident_paragraphs;
	uint32_t segment = plan->load_segment;

	while (remaining != 0u) {
		uint32_t paragraphs = mz_exec_chunk_paragraphs(remaining);
		size_t bytes = (size_t)paragraphs * DOS_PARAGRAPH_BYTES;

		if (segment > 0xffffu ||
		    dos_machine_validate_far(machine, (uint16_t)segment, 0u,
					     bytes) != DOS_MACHINE_OK)
			return DOS_IMAGE_LOAD_BAD_RESIDENT_RANGE;
		segment += paragraphs;
		remaining -= paragraphs;
	}
	return segment <= 0x10000u ? DOS_IMAGE_LOAD_OK
				   : DOS_IMAGE_LOAD_BAD_RESIDENT_RANGE;
}

static void clear_native_buffer(uint8_t *buffer, size_t count)
{
	size_t index;

	for (index = 0u; index < count; ++index)
		buffer[index] = 0u;
}

static enum dos_image_load_status
replace_guest_chunk(const struct dos_machine *machine, uint16_t segment,
		    uint16_t offset, const uint8_t *source, size_t count)
{
	uint8_t rollback[DOS_IMAGE_LOAD_BUFFER_BYTES];
	enum dos_machine_status status;

	if (count > sizeof(rollback))
		return DOS_IMAGE_LOAD_INVALID_ARGUMENT;
	status =
	    dos_machine_replace_far(machine, segment, offset, source, count,
				    rollback, sizeof(rollback), count);
	if (status == DOS_MACHINE_ROLLBACK_FAILED)
		return DOS_IMAGE_LOAD_MACHINE_POISONED;
	return status == DOS_MACHINE_OK ? DOS_IMAGE_LOAD_OK
					: DOS_IMAGE_LOAD_MACHINE_FAULT;
}

static enum dos_image_load_status
copy_exact_file_bytes(const struct dos_image_reader *reader,
		      const struct dos_machine *machine,
		      file_offset_t file_offset, uint16_t target_segment,
		      uint16_t target_offset, uint32_t count)
{
	uint8_t buffer[DOS_IMAGE_LOAD_BUFFER_BYTES];
	uint32_t completed = 0u;

	while (completed < count) {
		uint32_t remaining = count - completed;
		size_t chunk = remaining < sizeof(buffer) ? (size_t)remaining
							  : sizeof(buffer);
		file_offset_t current_file_offset;
		uint32_t current_target_offset;
		size_t bytes_read = 0u;
		enum dos_image_read_status read_status;
		enum dos_image_load_status status;

		if (check_add_overflow(file_offset, (file_offset_t)completed,
				       &current_file_offset) ||
		    check_add_overflow((uint32_t)target_offset, completed,
				       &current_target_offset) ||
		    current_target_offset > 0xffffu)
			return DOS_IMAGE_LOAD_FILE_RANGE_OVERFLOW;
		clear_native_buffer(buffer, chunk);
		read_status =
		    reader->read(reader->context, current_file_offset, buffer,
				 sizeof(buffer), chunk, &bytes_read);
		if (read_status != DOS_IMAGE_READ_OK)
			return DOS_IMAGE_LOAD_IMAGE_IO_ERROR;
		if (bytes_read != chunk)
			return DOS_IMAGE_LOAD_IMAGE_SHORT_READ;
		status = replace_guest_chunk(machine, target_segment,
					     (uint16_t)current_target_offset,
					     buffer, chunk);
		if (status != DOS_IMAGE_LOAD_OK)
			return status;
		completed += (uint32_t)chunk;
	}
	return DOS_IMAGE_LOAD_OK;
}

static void publish_result(struct dos_image_load_result *result,
			   kernel_object_handle_t lease_handle,
			   uint32_t written, uint32_t resident)
{
	result->lease_handle = lease_handle;
	result->file_bytes_written = written;
	result->resident_bytes = resident;
	result->untouched_bytes = resident - written;
	result->reserved = 0u;
}

enum dos_image_load_status
dos_image_load_com_resident(const struct dos_image_reader *reader,
			    const struct dos_machine *machine,
			    const struct dos_load_plan *image_plan,
			    const struct dos_com_process_plan *process_plan,
			    const struct dos_memory_lease_view *lease_view,
			    struct dos_image_load_result *result)
{
	struct lease_geometry geometry;
	enum dos_image_load_status status;

	if (result == NULL)
		return DOS_IMAGE_LOAD_INVALID_ARGUMENT;
	status = validate_reader(reader, image_plan);
	if (status != DOS_IMAGE_LOAD_OK)
		return status;
	status = validate_lease_view(machine, lease_view, &geometry);
	if (status != DOS_IMAGE_LOAD_OK)
		return status;
	status =
	    validate_com_process_geometry(process_plan, lease_view, &geometry);
	if (status != DOS_IMAGE_LOAD_OK)
		return status;
	status = validate_com_image(image_plan, process_plan);
	if (status != DOS_IMAGE_LOAD_OK)
		return status;
	status = preflight_com_target(machine, process_plan);
	if (status != DOS_IMAGE_LOAD_OK)
		return status;
	status = copy_exact_file_bytes(
	    reader, machine, 0u, process_plan->load_segment,
	    process_plan->load_offset, process_plan->image_size);
	if (status != DOS_IMAGE_LOAD_OK)
		return status;
	publish_result(result, lease_view->handle.value,
		       process_plan->image_size, process_plan->read_capacity);
	return DOS_IMAGE_LOAD_OK;
}

enum dos_image_load_status
dos_image_load_mz_resident(const struct dos_image_reader *reader,
			   const struct dos_machine *machine,
			   const struct dos_load_plan *image_plan,
			   const struct dos_mz_process_plan *process_plan,
			   const struct dos_memory_lease_view *lease_view,
			   struct dos_image_load_result *result)
{
	struct lease_geometry geometry;
	uint64_t resident_bytes64;
	uint32_t file_bytes;
	uint32_t remaining_file_bytes;
	uint32_t remaining_paragraphs;
	uint32_t segment;
	file_offset_t file_cursor;
	enum dos_image_load_status status;

	if (result == NULL)
		return DOS_IMAGE_LOAD_INVALID_ARGUMENT;
	status = validate_reader(reader, image_plan);
	if (status != DOS_IMAGE_LOAD_OK)
		return status;
	status = validate_lease_view(machine, lease_view, &geometry);
	if (status != DOS_IMAGE_LOAD_OK)
		return status;
	status = validate_mz_process_geometry(process_plan, lease_view,
					      &geometry, &resident_bytes64);
	if (status != DOS_IMAGE_LOAD_OK)
		return status;
	status = validate_mz_image(image_plan, process_plan, resident_bytes64,
				   &file_bytes);
	if (status != DOS_IMAGE_LOAD_OK)
		return status;
	status = preflight_mz_target(machine, process_plan);
	if (status != DOS_IMAGE_LOAD_OK)
		return status;

	remaining_file_bytes = file_bytes;
	remaining_paragraphs = process_plan->resident_paragraphs;
	segment = process_plan->load_segment;
	file_cursor = process_plan->image_file_offset;
	while (remaining_paragraphs != 0u && remaining_file_bytes != 0u) {
		uint32_t paragraphs =
		    mz_exec_chunk_paragraphs(remaining_paragraphs);
		uint32_t outer_bytes = paragraphs * DOS_PARAGRAPH_BYTES;
		uint32_t bytes = remaining_file_bytes < outer_bytes
				     ? remaining_file_bytes
				     : outer_bytes;

		/*
		 * Native staging stays small, but target offsets remain
		 * relative to the complete EXEC DS:0 transfer.  This
		 * preserves segment and A20 behavior rather than making buffer
		 * size guest-visible.
		 */
		status = copy_exact_file_bytes(reader, machine, file_cursor,
					       (uint16_t)segment, 0u, bytes);
		if (status != DOS_IMAGE_LOAD_OK)
			return status;
		if (check_add_overflow(file_cursor, (file_offset_t)bytes,
				       &file_cursor))
			return DOS_IMAGE_LOAD_FILE_RANGE_OVERFLOW;
		remaining_file_bytes -= bytes;
		remaining_paragraphs -= paragraphs;
		segment += paragraphs;
	}
	if (remaining_file_bytes != 0u)
		return DOS_IMAGE_LOAD_BAD_FILE_RANGE;
	publish_result(result, lease_view->handle.value, file_bytes,
		       (uint32_t)resident_bytes64);
	return DOS_IMAGE_LOAD_OK;
}

static enum dos_image_load_status
validate_stack_word(const struct dos_machine *machine,
		    const struct lease_geometry *geometry, uint16_t segment,
		    uint16_t offset)
{
	uint64_t linear = ((uint64_t)segment << 4) + offset;

	if (offset > 0xfffeu || !range_is_in_lease(geometry, linear, 2u) ||
	    dos_machine_validate_far(machine, segment, offset, 2u) !=
		DOS_MACHINE_OK)
		return DOS_IMAGE_LOAD_BAD_RESIDENT_RANGE;
	return DOS_IMAGE_LOAD_OK;
}

static enum dos_image_load_status
replace_stack_word(const struct dos_machine *machine, uint16_t segment,
		   uint16_t offset, uint16_t value)
{
	uint8_t replacement[2];

	replacement[0] = (uint8_t)value;
	replacement[1] = (uint8_t)(value >> 8);
	return replace_guest_chunk(machine, segment, offset, replacement,
				   sizeof(replacement));
}

enum dos_image_load_status dos_image_load_prepare_com_stack(
    const struct dos_machine *machine,
    const struct dos_com_process_plan *process_plan,
    const struct dos_memory_lease_view *lease_view)
{
	struct lease_geometry geometry;
	enum dos_image_load_status status;

	status = validate_lease_view(machine, lease_view, &geometry);
	if (status != DOS_IMAGE_LOAD_OK)
		return status;
	status =
	    validate_com_process_geometry(process_plan, lease_view, &geometry);
	if (status != DOS_IMAGE_LOAD_OK)
		return status;
	status =
	    validate_stack_word(machine, &geometry, process_plan->psp_segment,
				process_plan->stack_sentinel_offset);
	if (status != DOS_IMAGE_LOAD_OK)
		return status;
	if (process_plan->launch_mode == DOS_PROCESS_LAUNCH_LOAD_ONLY) {
		status = validate_stack_word(
		    machine, &geometry, process_plan->initial_state.ss,
		    process_plan->load_only_stack_pointer);
		if (status != DOS_IMAGE_LOAD_OK)
			return status;
	}
	status = replace_stack_word(machine, process_plan->psp_segment,
				    process_plan->stack_sentinel_offset,
				    process_plan->stack_sentinel_value);
	if (status != DOS_IMAGE_LOAD_OK)
		return status;
	if (process_plan->launch_mode == DOS_PROCESS_LAUNCH_LOAD_ONLY)
		return replace_stack_word(machine,
					  process_plan->initial_state.ss,
					  process_plan->load_only_stack_pointer,
					  process_plan->load_only_stack_value);
	return DOS_IMAGE_LOAD_OK;
}

enum dos_image_load_status
dos_image_load_prepare_mz_stack(const struct dos_machine *machine,
				const struct dos_mz_process_plan *process_plan,
				const struct dos_memory_lease_view *lease_view)
{
	struct lease_geometry geometry;
	uint64_t resident_bytes;
	uint16_t initial_sp;
	uint16_t first_push;
	uint16_t second_push;
	enum dos_image_load_status status;

	status = validate_lease_view(machine, lease_view, &geometry);
	if (status != DOS_IMAGE_LOAD_OK)
		return status;
	status = validate_mz_process_geometry(process_plan, lease_view,
					      &geometry, &resident_bytes);
	if (status != DOS_IMAGE_LOAD_OK)
		return status;
	(void)resident_bytes;
	initial_sp = dos_register_low16(process_plan->initial_state.esp);
	first_push = (uint16_t)(initial_sp - 2u);
	status = validate_stack_word(
	    machine, &geometry, process_plan->initial_state.ss, first_push);
	if (status != DOS_IMAGE_LOAD_OK)
		return status;
	if (process_plan->launch_mode == DOS_PROCESS_LAUNCH_LOAD_ONLY)
		return replace_stack_word(
		    machine, process_plan->initial_state.ss, first_push,
		    process_plan->load_only_stack_value);
	/* EXEC0 performs these PUSHes only after switching SS:SP. */
	second_push = (uint16_t)(initial_sp - 4u);
	return validate_stack_word(machine, &geometry,
				   process_plan->initial_state.ss, second_push);
}

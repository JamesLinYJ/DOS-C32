// SPDX-License-Identifier: GPL-2.0-only
/*
 * DOS EXEC environment construction.
 *
 * This implements the MS-DOS EXEC environment path.  The DOS-visible block
 * layout is preserved; bounded buffers, checked arithmetic and fail-closed
 * publication form the safety layer.
 */
#include "dos_environment.h"

#include "overflow.h"

#define DOS_ENVIRONMENT_COPY_CHUNK_BYTES 128u
#define DOS_ENVIRONMENT_MAX_PAYLOAD_BYTES 0xffffu

static enum dos_environment_status
scan_environment(const struct dos_machine *machine,
		 struct dos_far_pointer16 source, uint32_t *environment_bytes)
{
	uint8_t byte;
	uint32_t scanned = 0u;
	bool previous_was_nul = false;

	if (machine == NULL || machine->ops == NULL ||
	    machine->ops->read_memory == NULL || environment_bytes == NULL)
		return DOS_ENVIRONMENT_INVALID_ARGUMENT;

	/*
	 * Do not fetch past the first double NUL.  REPNE SCASB would not fault
	 * on an inaccessible byte after the terminator, so a speculative chunk
	 * read would reject a valid source. Construction below
	 * remains chunked; this bounded scan preserves the precise fault point.
	 */
	while (scanned < DOS_ENVIRONMENT_SCAN_LIMIT) {
		uint16_t offset = (uint16_t)((uint32_t)source.offset + scanned);

		if (dos_machine_read_far(machine, source.segment, offset, &byte,
					 sizeof(byte),
					 sizeof(byte)) != DOS_MACHINE_OK)
			return DOS_ENVIRONMENT_SOURCE_FAULT;
		++scanned;
		if (byte == 0u) {
			if (previous_was_nul) {
				*environment_bytes = scanned;
				return DOS_ENVIRONMENT_OK;
			}
			previous_was_nul = true;
		} else {
			previous_was_nul = false;
		}
	}
	return DOS_ENVIRONMENT_BAD_SOURCE;
}

static enum dos_environment_status
calculate_plan(struct dos_far_pointer16 source, uint32_t environment_bytes,
	       const struct dos_exec_name_plan *executable_name,
	       struct dos_environment_plan *staging)
{
	uint32_t executable_name_bytes;
	uint32_t payload_bytes;
	uint32_t rounded_bytes;

	executable_name_bytes = executable_name->bytes_including_nul;
	/*
	 * MS-DOS performs this sum in BX. A maliciously large argv[0] can
	 * wrap that 16-bit allocation calculation before REP MOVSB writes the
	 * target.  Wide checked arithmetic preserves normal DOS results and
	 * rejects an undersized or destination-offset-wrapping allocation.
	 */
	if (check_add_overflow(environment_bytes, 2u, &payload_bytes) ||
	    check_add_overflow(payload_bytes, executable_name_bytes,
			       &payload_bytes) ||
	    payload_bytes > DOS_ENVIRONMENT_MAX_PAYLOAD_BYTES ||
	    check_add_overflow(payload_bytes,
			       DOS_ENVIRONMENT_PARAGRAPH_BYTES - 1u,
			       &rounded_bytes))
		return DOS_ENVIRONMENT_RANGE_OVERFLOW;

	staging->source.offset = source.offset;
	staging->source.segment = source.segment;
	staging->environment_bytes = environment_bytes;
	staging->executable_name.source.offset = executable_name->source.offset;
	staging->executable_name.source.segment = executable_name->source.segment;
	staging->executable_name.bytes_including_nul =
	    executable_name->bytes_including_nul;
	staging->executable_name.reserved = executable_name->reserved;
	staging->payload_bytes = payload_bytes;
	staging->paragraphs = (uint16_t)(rounded_bytes >> 4);
	staging->allocation_bytes =
	    (uint32_t)staging->paragraphs * DOS_ENVIRONMENT_PARAGRAPH_BYTES;
	staging->reserved = 0u;
	return DOS_ENVIRONMENT_OK;
}

bool dos_environment_plan_has_valid_encoding(
    const struct dos_environment_plan *plan)
{
	struct dos_environment_plan expected;

	if (plan == NULL ||
	    !dos_exec_name_plan_has_valid_encoding(&plan->executable_name) ||
	    plan->environment_bytes < 2u ||
	    plan->environment_bytes > DOS_ENVIRONMENT_SCAN_LIMIT ||
	    plan->reserved != 0u)
		return false;
	if (calculate_plan(plan->source, plan->environment_bytes,
			   &plan->executable_name, &expected) !=
	    DOS_ENVIRONMENT_OK)
		return false;
	return plan->payload_bytes == expected.payload_bytes &&
	       plan->allocation_bytes == expected.allocation_bytes &&
	       plan->paragraphs == expected.paragraphs;
}

static void publish_plan(struct dos_environment_plan *destination,
			 const struct dos_environment_plan *source)
{
	destination->source.offset = source->source.offset;
	destination->source.segment = source->source.segment;
	destination->environment_bytes = source->environment_bytes;
	destination->executable_name.source.offset =
	    source->executable_name.source.offset;
	destination->executable_name.source.segment =
	    source->executable_name.source.segment;
	destination->executable_name.bytes_including_nul =
	    source->executable_name.bytes_including_nul;
	destination->executable_name.reserved =
	    source->executable_name.reserved;
	destination->payload_bytes = source->payload_bytes;
	destination->allocation_bytes = source->allocation_bytes;
	destination->paragraphs = source->paragraphs;
	destination->reserved = source->reserved;
}

enum dos_environment_status dos_environment_plan_create(
    const struct dos_machine *machine, struct dos_far_pointer16 source,
    const struct dos_exec_name_plan *executable_name,
    struct dos_environment_plan *plan)
{
	struct dos_environment_plan staging;
	enum dos_environment_status status;
	uint32_t environment_bytes;

	if (plan == NULL ||
	    !dos_exec_name_plan_has_valid_encoding(executable_name))
		return DOS_ENVIRONMENT_INVALID_ARGUMENT;
	status = scan_environment(machine, source, &environment_bytes);
	if (status != DOS_ENVIRONMENT_OK)
		return status;
	status = calculate_plan(source, environment_bytes, executable_name,
				&staging);
	if (status != DOS_ENVIRONMENT_OK)
		return status;
	publish_plan(plan, &staging);
	return DOS_ENVIRONMENT_OK;
}

static bool plans_match(const struct dos_environment_plan *left,
			const struct dos_environment_plan *right)
{
	return left->source.offset == right->source.offset &&
	       left->source.segment == right->source.segment &&
	       left->environment_bytes == right->environment_bytes &&
	       left->executable_name.source.offset ==
		   right->executable_name.source.offset &&
	       left->executable_name.source.segment ==
		   right->executable_name.source.segment &&
	       left->executable_name.bytes_including_nul ==
		   right->executable_name.bytes_including_nul &&
	       left->executable_name.reserved ==
		   right->executable_name.reserved &&
	       left->payload_bytes == right->payload_bytes &&
	       left->allocation_bytes == right->allocation_bytes &&
	       left->paragraphs == right->paragraphs &&
	       left->reserved == right->reserved;
}

static enum dos_environment_status
replace_target_bytes(const struct dos_machine *machine, uint16_t segment,
		     uint16_t offset, const uint8_t *source,
		     size_t source_capacity, size_t count)
{
	uint8_t rollback[DOS_ENVIRONMENT_COPY_CHUNK_BYTES];
	enum dos_machine_status status;

	if (count > sizeof(rollback) || count > source_capacity)
		return DOS_ENVIRONMENT_INVALID_ARGUMENT;
	status = dos_machine_replace_far(machine, segment, offset, source,
					 source_capacity, rollback,
					 sizeof(rollback), count);
	if (status == DOS_MACHINE_ROLLBACK_FAILED)
		return DOS_ENVIRONMENT_TARGET_POISONED;
	return status == DOS_MACHINE_OK ? DOS_ENVIRONMENT_OK
					: DOS_ENVIRONMENT_TARGET_FAULT;
}

static enum dos_environment_status copy_guest_environment(
    const struct dos_machine *machine, const struct dos_environment_plan *plan,
    struct dos_far_pointer16 destination, bool *target_started)
{
	uint8_t chunk[DOS_ENVIRONMENT_COPY_CHUNK_BYTES];
	uint32_t completed = 0u;

	while (completed < plan->environment_bytes) {
		uint32_t remaining = plan->environment_bytes - completed;
		size_t count = remaining < sizeof(chunk) ? (size_t)remaining
							 : sizeof(chunk);
		uint16_t source_offset =
		    (uint16_t)((uint32_t)plan->source.offset + completed);
		uint16_t destination_offset =
		    (uint16_t)((uint32_t)destination.offset + completed);

		if (dos_machine_read_far(machine, plan->source.segment,
					 source_offset, chunk, sizeof(chunk),
					 count) != DOS_MACHINE_OK)
			return *target_started ? DOS_ENVIRONMENT_TARGET_FAULT
					       : DOS_ENVIRONMENT_SOURCE_FAULT;
		{
			enum dos_environment_status status;

			status = replace_target_bytes(
			    machine, destination.segment, destination_offset,
			    chunk, sizeof(chunk), count);
			if (status != DOS_ENVIRONMENT_OK)
				return status;
		}
		*target_started = true;
		completed += (uint32_t)count;
	}
	return DOS_ENVIRONMENT_OK;
}

static enum dos_environment_status
write_bytes(const struct dos_machine *machine, uint16_t segment,
	    uint32_t offset, const uint8_t *source, size_t source_capacity,
	    size_t count, bool *target_started)
{
	enum dos_environment_status status;

	status = replace_target_bytes(machine, segment, (uint16_t)offset,
				      source, source_capacity, count);
	if (status != DOS_ENVIRONMENT_OK)
		return status;
	*target_started = true;
	return DOS_ENVIRONMENT_OK;
}

static enum dos_environment_status
copy_guest_executable_name(const struct dos_machine *machine,
			   const struct dos_environment_plan *plan,
			   uint16_t destination_segment,
			   bool *target_started)
{
	uint8_t chunk[DOS_ENVIRONMENT_COPY_CHUNK_BYTES];
	uint32_t completed = 0u;
	uint32_t destination_base = plan->environment_bytes + 2u;
	uint32_t executable_name_bytes =
	    plan->executable_name.bytes_including_nul;

	while (completed < executable_name_bytes) {
		uint32_t remaining = executable_name_bytes - completed;
		size_t count = remaining < DOS_ENVIRONMENT_COPY_CHUNK_BYTES
				   ? (size_t)remaining
				   : (size_t)DOS_ENVIRONMENT_COPY_CHUNK_BYTES;
		size_t index;
		enum dos_environment_status status;

		/*
		 * Preserve DStrLen's exact fault boundary even while copying in
		 * rollback-sized chunks.  An early NUL retires this stale plan
		 * without speculatively fetching the following guest byte.
		 */
		for (index = 0u; index < count; ++index) {
			uint32_t position = completed + (uint32_t)index;
			uint16_t source_offset = (uint16_t)(
			    (uint32_t)plan->executable_name.source.offset +
			    position);

			if (dos_machine_read_far(
				machine, plan->executable_name.source.segment,
				source_offset, &chunk[index], sizeof(chunk[index]),
				sizeof(chunk[index])) != DOS_MACHINE_OK)
				return *target_started
					       ? DOS_ENVIRONMENT_TARGET_FAULT
					       : DOS_ENVIRONMENT_SOURCE_FAULT;
			if (chunk[index] == 0u) {
				if (position + 1u != executable_name_bytes)
					return DOS_ENVIRONMENT_STALE_PLAN;
			} else if (position + 1u == executable_name_bytes) {
				return DOS_ENVIRONMENT_STALE_PLAN;
			}
		}
		status = write_bytes(machine, destination_segment,
				     destination_base + completed,
				     chunk, sizeof(chunk), count,
				     target_started);
		if (status != DOS_ENVIRONMENT_OK)
			return status;
		completed += (uint32_t)count;
	}
	return DOS_ENVIRONMENT_OK;
}

enum dos_environment_status dos_environment_build(
    const struct dos_machine *machine, const struct dos_environment_plan *plan,
    struct dos_far_pointer16 destination)
{
	static const uint8_t trailer[2] = {
	    (uint8_t)DOS_ENVIRONMENT_TRAILER_VALUE,
	    (uint8_t)(DOS_ENVIRONMENT_TRAILER_VALUE >> 8),
	};
	struct dos_environment_plan current;
	enum dos_environment_status status;
	uint32_t environment_bytes;
	bool target_started = false;

	if (machine == NULL || machine->ops == NULL ||
	    machine->ops->read_memory == NULL ||
	    machine->ops->write_memory == NULL || plan == NULL ||
	    destination.offset != 0u ||
	    !dos_environment_plan_has_valid_encoding(plan))
		return DOS_ENVIRONMENT_INVALID_ARGUMENT;
	status = scan_environment(machine, plan->source, &environment_bytes);
	if (status != DOS_ENVIRONMENT_OK)
		return status;
	status = calculate_plan(plan->source, environment_bytes,
				&plan->executable_name, &current);
	if (status != DOS_ENVIRONMENT_OK)
		return status;
	if (!plans_match(plan, &current))
		return DOS_ENVIRONMENT_STALE_PLAN;
	if (dos_machine_validate_far(machine, destination.segment,
				     destination.offset,
				     plan->payload_bytes) != DOS_MACHINE_OK)
		return DOS_ENVIRONMENT_TARGET_FAULT;

	status =
	    copy_guest_environment(machine, plan, destination, &target_started);
	if (status != DOS_ENVIRONMENT_OK)
		return status;
	status = write_bytes(machine, destination.segment,
			     plan->environment_bytes, trailer, sizeof(trailer),
			     sizeof(trailer), &target_started);
	if (status != DOS_ENVIRONMENT_OK)
		return status;
	return copy_guest_executable_name(machine, plan, destination.segment,
					  &target_started);
}

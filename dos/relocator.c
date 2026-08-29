// SPDX-License-Identifier: GPL-2.0-only
/*
 * Safe MS-DOS MZ relocation.
 *
 * Compatibility contract: each four-byte table item names offset:segment, EXEC adds
 *                 exec_rel_fac to the segment used to find the word, then
 *                 adds the same factor to that word with 16-bit wrapping.
 * Safety changes: checked file/range arithmetic, a complete validation pass,
 *                 fixed-size buffered reads, transactional two-byte writes,
 *                 and explicit poisoned-machine propagation.
 */
#include "dos_relocator.h"

#include "overflow.h"

#define DOS_RELOCATION_ENTRY_BYTES 4u
#define DOS_RELOCATION_WORD_BYTES 2u
#define DOS_RELOCATION_BUFFER_BYTES 64u
#define DOS_OFFSET_WORD_MAXIMUM 0xfffeu
#define DOS_RELOCATION_TABLE_OFFSET_MAXIMUM 0xffffu

static_assert_expression(DOS_RELOCATION_BUFFER_BYTES %
				 DOS_RELOCATION_ENTRY_BYTES ==
			     0u,
			 "relocation buffer must contain whole entries");
static_assert_expression(
    sizeof(kernel_object_handle_t) == 8,
    "reader and machine handles must remain canonical 64-bit");

static uint16_t read_le16(const uint8_t *bytes)
{
	return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static void write_le16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
}

static bool machine_is_usable(const struct dos_machine *machine)
{
	return machine != NULL && machine->ops != NULL &&
	       machine->ops->read_memory != NULL &&
	       machine->ops->write_memory != NULL &&
	       machine->address_limit != 0u &&
	       machine->address_limit <= DOS_GUEST_32_ADDRESS_LIMIT;
}

static enum dos_relocator_status
validate_request(const struct dos_image_reader *reader,
		 const struct dos_machine *machine,
		 const struct dos_relocator_request *request)
{
	uint64_t table_bytes;
	uint64_t table_end;
	uint64_t resident_end;

	if (reader == NULL || reader->read == NULL ||
	    !machine_is_usable(machine) || request == NULL)
		return DOS_RELOCATOR_INVALID_ARGUMENT;
	if (check_mul_overflow((uint64_t)request->relocation_count,
			       (uint64_t)DOS_RELOCATION_ENTRY_BYTES,
			       &table_bytes) ||
	    check_add_overflow(request->relocation_table_offset, table_bytes,
			       &table_end))
		return DOS_RELOCATOR_FILE_RANGE_OVERFLOW;
	/*
	 * Relocation seeks to 0:exec_rle_table without comparing the
	 * offset with either its 26-byte private header or the resident image
	 * file offset.  The immutable reader extent is the only file bound.
	 */
	if (request->relocation_table_offset >
	    DOS_RELOCATION_TABLE_OFFSET_MAXIMUM)
		return DOS_RELOCATOR_BAD_FILE_RANGE;
	/*
	 * A zero-entry table names no file bytes.  DOS still performs the
	 * absolute seek, and local $LSEEK accepts a position past EOF; the
	 * following zero-count path therefore succeeds.  A nonempty table is
	 * bounded by the immutable snapshot as the documented safety
	 * divergence from EXEC's stale-buffer reads.
	 */
	if (request->relocation_count != 0u && table_end > reader->size)
		return DOS_RELOCATOR_BAD_FILE_RANGE;
	if (check_add_overflow((uint64_t)request->resident_linear_address,
			       request->resident_size, &resident_end) ||
	    resident_end > machine->address_limit ||
	    resident_end > DOS_GUEST_32_ADDRESS_LIMIT ||
	    (request->relocation_count != 0u &&
	     request->resident_size < DOS_RELOCATION_WORD_BYTES))
		return DOS_RELOCATOR_BAD_RESIDENT_RANGE;
	return DOS_RELOCATOR_OK;
}

static enum dos_relocator_status
read_entries(const struct dos_image_reader *reader, file_offset_t offset,
	     uint8_t *buffer, size_t buffer_capacity, size_t count)
{
	size_t bytes_read = 0u;
	size_t index;
	enum dos_image_read_status status;

	if (buffer == NULL || count > buffer_capacity)
		return DOS_RELOCATOR_INVALID_ARGUMENT;
	/* A broken backend must not expose uninitialized native stack bytes. */
	for (index = 0u; index < count; ++index)
		buffer[index] = 0u;
	status = reader->read(reader->context, offset, buffer, buffer_capacity,
			      count, &bytes_read);
	if (status != DOS_IMAGE_READ_OK)
		return DOS_RELOCATOR_IMAGE_IO_ERROR;
	if (bytes_read != count)
		return DOS_RELOCATOR_IMAGE_SHORT_READ;
	return DOS_RELOCATOR_OK;
}

static enum dos_relocator_status
validate_entry(const struct dos_machine *machine,
	       const struct dos_relocator_request *request,
	       const uint8_t *encoded, dos_linear_address_t *target)
{
	uint16_t offset = read_le16(encoded);
	uint16_t segment = read_le16(encoded + 2u);
	uint16_t relocated_segment;
	uint64_t target64;
	uint64_t target_end;
	uint64_t resident_end;

	if (offset > DOS_OFFSET_WORD_MAXIMUM)
		return DOS_RELOCATOR_BAD_TARGET_OFFSET;
	/* Relocation uses a 16-bit ADD with no carry branch. */
	relocated_segment = (uint16_t)(segment + request->relocation_factor);
	target64 =
	    dos_far_to_linear(relocated_segment, offset, machine->a20_enabled);
	if (check_add_overflow(target64, (uint64_t)DOS_RELOCATION_WORD_BYTES,
			       &target_end) ||
	    check_add_overflow((uint64_t)request->resident_linear_address,
			       request->resident_size, &resident_end))
		return DOS_RELOCATOR_TARGET_OUTSIDE_RESIDENT;
	if (target64 < request->resident_linear_address ||
	    target_end > resident_end || target_end > machine->address_limit ||
	    target64 > (uint64_t)(dos_linear_address_t)-1)
		return DOS_RELOCATOR_TARGET_OUTSIDE_RESIDENT;
	*target = (dos_linear_address_t)target64;
	return DOS_RELOCATOR_OK;
}

static enum dos_relocator_status
validate_all_entries(const struct dos_image_reader *reader,
		     const struct dos_machine *machine,
		     const struct dos_relocator_request *request)
{
	uint8_t buffer[DOS_RELOCATION_BUFFER_BYTES];
	file_offset_t cursor = request->relocation_table_offset;
	uint32_t remaining = request->relocation_count;

	while (remaining != 0u) {
		size_t entries = remaining;
		size_t bytes;
		size_t index;
		enum dos_relocator_status status;

		if (entries >
		    DOS_RELOCATION_BUFFER_BYTES / DOS_RELOCATION_ENTRY_BYTES)
			entries = DOS_RELOCATION_BUFFER_BYTES /
				  DOS_RELOCATION_ENTRY_BYTES;
		if (check_mul_overflow(
			entries, (size_t)DOS_RELOCATION_ENTRY_BYTES, &bytes))
			return DOS_RELOCATOR_FILE_RANGE_OVERFLOW;
		status =
		    read_entries(reader, cursor, buffer, sizeof(buffer), bytes);
		if (status != DOS_RELOCATOR_OK)
			return status;
		for (index = 0u; index < entries; ++index) {
			dos_linear_address_t ignored_target;

			status = validate_entry(
			    machine, request,
			    buffer + index * DOS_RELOCATION_ENTRY_BYTES,
			    &ignored_target);
			if (status != DOS_RELOCATOR_OK)
				return status;
		}
		if (check_add_overflow(cursor, (file_offset_t)bytes, &cursor))
			return DOS_RELOCATOR_FILE_RANGE_OVERFLOW;
		remaining -= (uint32_t)entries;
	}
	return DOS_RELOCATOR_OK;
}

static enum dos_relocator_status
relocate_word(const struct dos_machine *machine, dos_linear_address_t target,
	      uint16_t factor)
{
	uint8_t original[DOS_RELOCATION_WORD_BYTES];
	uint8_t replacement[DOS_RELOCATION_WORD_BYTES];
	uint8_t rollback[DOS_RELOCATION_WORD_BYTES];
	uint16_t value;
	enum dos_machine_status status;

	status = dos_machine_read(machine, target, original, sizeof(original),
				  sizeof(original));
	if (status != DOS_MACHINE_OK)
		return DOS_RELOCATOR_MACHINE_FAULT;
	value = read_le16(original);
	/* The 16-bit ADD intentionally wraps the target word. */
	value = (uint16_t)(value + factor);
	write_le16(replacement, value);
	status = dos_machine_replace(machine, target, replacement,
				     sizeof(replacement), rollback,
				     sizeof(rollback), sizeof(replacement));
	if (status == DOS_MACHINE_ROLLBACK_FAILED)
		return DOS_RELOCATOR_MACHINE_POISONED;
	if (status != DOS_MACHINE_OK)
		return DOS_RELOCATOR_MACHINE_FAULT;
	return DOS_RELOCATOR_OK;
}

static enum dos_relocator_status
apply_all_entries(const struct dos_image_reader *reader,
		  const struct dos_machine *machine,
		  const struct dos_relocator_request *request)
{
	uint8_t buffer[DOS_RELOCATION_BUFFER_BYTES];
	file_offset_t cursor = request->relocation_table_offset;
	uint32_t remaining = request->relocation_count;

	while (remaining != 0u) {
		size_t entries = remaining;
		size_t bytes;
		size_t index;
		enum dos_relocator_status status;

		if (entries >
		    DOS_RELOCATION_BUFFER_BYTES / DOS_RELOCATION_ENTRY_BYTES)
			entries = DOS_RELOCATION_BUFFER_BYTES /
				  DOS_RELOCATION_ENTRY_BYTES;
		if (check_mul_overflow(
			entries, (size_t)DOS_RELOCATION_ENTRY_BYTES, &bytes))
			return DOS_RELOCATOR_FILE_RANGE_OVERFLOW;
		status =
		    read_entries(reader, cursor, buffer, sizeof(buffer), bytes);
		if (status != DOS_RELOCATOR_OK)
			return status;
		for (index = 0u; index < entries; ++index) {
			dos_linear_address_t target;

			/* Recheck the entry used for the write.  The reader
			 * handle is expected to identify a stable EXEC input,
			 * but changed data must never bypass the memory-range
			 * checks. */
			status = validate_entry(
			    machine, request,
			    buffer + index * DOS_RELOCATION_ENTRY_BYTES,
			    &target);
			if (status != DOS_RELOCATOR_OK)
				return status;
			status = relocate_word(machine, target,
					       request->relocation_factor);
			if (status != DOS_RELOCATOR_OK)
				return status;
		}
		if (check_add_overflow(cursor, (file_offset_t)bytes, &cursor))
			return DOS_RELOCATOR_FILE_RANGE_OVERFLOW;
		remaining -= (uint32_t)entries;
	}
	return DOS_RELOCATOR_OK;
}

enum dos_relocator_status
dos_relocator_apply(const struct dos_image_reader *reader,
		    const struct dos_machine *machine,
		    const struct dos_relocator_request *request,
		    struct dos_relocator_result *result)
{
	struct dos_relocator_result staging;
	enum dos_relocator_status status;

	if (result == NULL)
		return DOS_RELOCATOR_INVALID_ARGUMENT;
	status = validate_request(reader, machine, request);
	if (status != DOS_RELOCATOR_OK)
		return status;
	status = validate_all_entries(reader, machine, request);
	if (status != DOS_RELOCATOR_OK)
		return status;
	status = apply_all_entries(reader, machine, request);
	if (status != DOS_RELOCATOR_OK)
		return status;
	staging.validated_entries = request->relocation_count;
	staging.applied_entries = request->relocation_count;
	*result = staging;
	return DOS_RELOCATOR_OK;
}

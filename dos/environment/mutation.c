// SPDX-License-Identifier: GPL-2.0-only
/*
 * Transactional PSP environment mutation
 *
 * Compatibility contract: case-insensitive NAME replacement, paragraph-sized MCB-owned
 *   environment storage, a double NUL, word 1 and the preserved argv[0].
 * Safety changes: bounded streaming, acquire-before-publish, transactional PSP
 *   replacement, reverse unwind and sticky quarantine on uncertain rollback.
 */
#include "dos_environment_mutation.h"

#include "dos_abi.h"
#include "dos_environment.h"
#include "overflow.h"

#define ENVIRONMENT_CACHE_BYTES 128u
#define MCB_SIZE_OFFSET 3u

struct environment_reader {
	const struct dos_machine *machine;
	uint16_t segment;
	uint32_t capacity;
	uint32_t cache_base;
	size_t cache_length;
	uint8_t cache[ENVIRONMENT_CACHE_BYTES];
	bool cache_valid;
};

struct environment_entry {
	uint32_t start;
	uint32_t bytes;
	bool matches;
};

struct environment_layout {
	uint32_t capacity;
	uint32_t retained_bytes;
	uint32_t retained_entries;
	uint32_t matching_entries;
	uint32_t trailer_offset;
	uint32_t argv0_offset;
	uint32_t argv0_bytes;
	uint32_t payload_bytes;
};

static uint16_t read_le16(const uint8_t bytes[2])
{
	return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8u);
}

static void write_le16(uint8_t bytes[2], uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8u);
}

static uint8_t environment_upper(uint8_t value)
{
	return value >= (uint8_t)'a' && value <= (uint8_t)'z'
		       ? (uint8_t)(value - ((uint8_t)'a' - (uint8_t)'A'))
		       : value;
}

static enum dos_environment_mutation_status reader_byte(
	struct environment_reader *reader, uint32_t offset, uint8_t *value)
{
	uint32_t base;
	size_t amount;

	if (offset >= reader->capacity)
		return DOS_ENVIRONMENT_MUTATION_BAD_LAYOUT;
	if (!reader->cache_valid || offset < reader->cache_base ||
	    offset - reader->cache_base >= reader->cache_length) {
		base = offset - offset % ENVIRONMENT_CACHE_BYTES;
		amount = (size_t)(reader->capacity - base);
		if (amount > sizeof(reader->cache))
			amount = sizeof(reader->cache);
		if (dos_machine_read_far(reader->machine, reader->segment,
					 (uint16_t)base, reader->cache,
					 sizeof(reader->cache), amount) !=
		    DOS_MACHINE_OK)
			return DOS_ENVIRONMENT_MUTATION_MACHINE_FAULT;
		reader->cache_base = base;
		reader->cache_length = amount;
		reader->cache_valid = true;
	}
	*value = reader->cache[offset - reader->cache_base];
	return DOS_ENVIRONMENT_MUTATION_OK;
}

static enum dos_environment_mutation_status memory_status(
	enum dos_memory_status status)
{
	switch (status) {
	case DOS_MEMORY_OK:
		return DOS_ENVIRONMENT_MUTATION_OK;
	case DOS_MEMORY_INVALID_ARGUMENT:
		return DOS_ENVIRONMENT_MUTATION_INVALID_ARGUMENT;
	case DOS_MEMORY_INVALID_BLOCK:
		return DOS_ENVIRONMENT_MUTATION_BAD_BLOCK;
	case DOS_MEMORY_NOT_ENOUGH_MEMORY:
		return DOS_ENVIRONMENT_MUTATION_NOT_ENOUGH_MEMORY;
	case DOS_MEMORY_ARENA_DAMAGED:
	case DOS_MEMORY_IDENTITY_MISMATCH:
	case DOS_MEMORY_GENERATION_EXHAUSTED:
		return DOS_ENVIRONMENT_MUTATION_ARENA_DAMAGED;
	case DOS_MEMORY_MACHINE_FAULT:
		return DOS_ENVIRONMENT_MUTATION_MACHINE_FAULT;
	case DOS_MEMORY_MACHINE_POISONED:
		return DOS_ENVIRONMENT_MUTATION_POISONED;
	case DOS_MEMORY_OWNER_MISMATCH:
		return DOS_ENVIRONMENT_MUTATION_OWNER_MISMATCH;
	}
	return DOS_ENVIRONMENT_MUTATION_ARENA_DAMAGED;
}

static enum dos_environment_mutation_status poison_transaction(
	struct dos_memory_arena *arena)
{
	enum dos_memory_status status = dos_memory_arena_poison(arena);

	if (status != DOS_MEMORY_OK && status != DOS_MEMORY_MACHINE_POISONED)
		return DOS_ENVIRONMENT_MUTATION_ARENA_DAMAGED;
	return DOS_ENVIRONMENT_MUTATION_POISONED;
}

static enum dos_environment_mutation_status validate_owned_block(
	const struct dos_memory_arena *arena, const struct dos_machine *machine,
	uint16_t psp_segment, uint16_t block_segment, uint32_t *capacity)
{
	struct dos_memory_owner_rebind_value value;
	enum dos_memory_status status;
	uint16_t paragraphs;
	uint32_t bytes;

	status = dos_memory_prepare_owner_rebind_checked(
		arena, machine, block_segment, psp_segment, psp_segment, &value);
	if (status != DOS_MEMORY_OK)
		return memory_status(status);
	paragraphs = read_le16(value.replacement_bytes + MCB_SIZE_OFFSET);
	if (paragraphs == 0u)
		return DOS_ENVIRONMENT_MUTATION_BAD_BLOCK;
	bytes = (uint32_t)paragraphs * DOS_ENVIRONMENT_PARAGRAPH_BYTES;
	if (bytes > DOS_ENVIRONMENT_MUTATION_MAX_BLOCK_BYTES)
		return DOS_ENVIRONMENT_MUTATION_BAD_BLOCK;
	*capacity = bytes;
	return DOS_ENVIRONMENT_MUTATION_OK;
}

static enum dos_environment_mutation_status inspect_entry(
	struct environment_reader *reader, uint32_t start,
	const uint8_t *name, size_t name_length,
	struct environment_entry *entry)
{
	uint32_t cursor = start;
	size_t name_index = 0u;
	bool has_equal = false;
	bool matches = true;
	uint8_t byte;
	enum dos_environment_mutation_status status;

	status = reader_byte(reader, cursor, &byte);
	if (status != DOS_ENVIRONMENT_MUTATION_OK)
		return status;
	if (byte == 0u) {
		*entry = (struct environment_entry){
			.start = start,
			.bytes = 0u,
			.matches = false,
		};
		return DOS_ENVIRONMENT_MUTATION_OK;
	}
	for (;;) {
		status = reader_byte(reader, cursor, &byte);
		if (status != DOS_ENVIRONMENT_MUTATION_OK)
			return status;
		++cursor;
		if (byte == 0u)
			break;
		if (!has_equal && byte == (uint8_t)'=') {
			has_equal = true;
			matches = matches && name_index == name_length;
			continue;
		}
		if (!has_equal) {
			if (name_index >= name_length ||
			    environment_upper(byte) !=
				    environment_upper(name[name_index]))
				matches = false;
			++name_index;
		}
	}
	entry->start = start;
	entry->bytes = cursor - start;
	entry->matches = has_equal && matches;
	return DOS_ENVIRONMENT_MUTATION_OK;
}

static enum dos_environment_mutation_status scan_layout(
	const struct dos_machine *machine, uint16_t segment, uint32_t capacity,
	const uint8_t *name, size_t name_length,
	struct environment_layout *layout)
{
	struct environment_reader reader = {
		.machine = machine,
		.segment = segment,
		.capacity = capacity,
	};
	struct environment_layout prepared = {
		.capacity = capacity,
	};
	uint32_t cursor = 0u;
	enum dos_environment_mutation_status status;

	for (;;) {
		struct environment_entry entry;

		status = inspect_entry(&reader, cursor, name, name_length,
				       &entry);
		if (status != DOS_ENVIRONMENT_MUTATION_OK)
			return status;
		if (entry.bytes == 0u) {
			uint8_t second_zero;

			if (cursor == 0u) {
				status = reader_byte(&reader, 1u, &second_zero);
				if (status != DOS_ENVIRONMENT_MUTATION_OK)
					return status;
				if (second_zero != 0u)
					return DOS_ENVIRONMENT_MUTATION_BAD_LAYOUT;
				prepared.trailer_offset = 2u;
			} else {
				prepared.trailer_offset = cursor + 1u;
			}
			break;
		}
		if (entry.matches) {
			if (check_add_overflow(prepared.matching_entries, 1u,
					       &prepared.matching_entries))
				return DOS_ENVIRONMENT_MUTATION_RANGE_OVERFLOW;
		} else {
			if (check_add_overflow(prepared.retained_bytes,
					       entry.bytes,
					       &prepared.retained_bytes) ||
			    check_add_overflow(prepared.retained_entries, 1u,
					       &prepared.retained_entries))
				return DOS_ENVIRONMENT_MUTATION_RANGE_OVERFLOW;
		}
		if (check_add_overflow(cursor, entry.bytes, &cursor) ||
		    cursor >= capacity)
			return DOS_ENVIRONMENT_MUTATION_BAD_LAYOUT;
	}
	if (prepared.trailer_offset > capacity ||
	    2u > capacity - prepared.trailer_offset)
		return DOS_ENVIRONMENT_MUTATION_BAD_LAYOUT;
	{
		uint8_t trailer[2];

		status = reader_byte(&reader, prepared.trailer_offset,
				     &trailer[0]);
		if (status != DOS_ENVIRONMENT_MUTATION_OK)
			return status;
		status = reader_byte(&reader, prepared.trailer_offset + 1u,
				     &trailer[1]);
		if (status != DOS_ENVIRONMENT_MUTATION_OK)
			return status;
		if (read_le16(trailer) != DOS_ENVIRONMENT_TRAILER_VALUE)
			return DOS_ENVIRONMENT_MUTATION_BAD_LAYOUT;
	}
	prepared.argv0_offset = prepared.trailer_offset + 2u;
	cursor = prepared.argv0_offset;
	for (;;) {
		uint8_t byte;

		status = reader_byte(&reader, cursor, &byte);
		if (status != DOS_ENVIRONMENT_MUTATION_OK)
			return status;
		++cursor;
		if (byte == 0u)
			break;
	}
	prepared.argv0_bytes = cursor - prepared.argv0_offset;
	prepared.payload_bytes = cursor;
	*layout = prepared;
	return DOS_ENVIRONMENT_MUTATION_OK;
}

static enum dos_environment_mutation_status write_range(
	const struct dos_machine *machine, uint16_t segment, uint32_t *cursor,
	const uint8_t *source, size_t source_capacity, size_t count)
{
	uint32_t next;

	if (count == 0u)
		return DOS_ENVIRONMENT_MUTATION_OK;
	if (check_add_overflow(*cursor, (uint32_t)count, &next) ||
	    next > DOS_ENVIRONMENT_MUTATION_MAX_BLOCK_BYTES)
		return DOS_ENVIRONMENT_MUTATION_RANGE_OVERFLOW;
	if (dos_machine_write_far(machine, segment, (uint16_t)*cursor, source,
				  source_capacity, count) != DOS_MACHINE_OK)
		return DOS_ENVIRONMENT_MUTATION_MACHINE_FAULT;
	*cursor = next;
	return DOS_ENVIRONMENT_MUTATION_OK;
}

static enum dos_environment_mutation_status write_upper_name(
	const struct dos_machine *machine, uint16_t segment, uint32_t *cursor,
	const uint8_t *name, size_t name_length)
{
	uint8_t staging[DOS_ENVIRONMENT_MUTATION_MAX_NAME_BYTES];
	size_t index;

	for (index = 0u; index < name_length; ++index)
		staging[index] = environment_upper(name[index]);
	return write_range(machine, segment, cursor, staging, sizeof(staging),
			   name_length);
}

static enum dos_environment_mutation_status copy_guest_range(
	struct environment_reader *source, uint32_t source_offset,
	uint32_t count, uint16_t destination_segment,
	uint32_t *destination_offset)
{
	uint8_t staging[ENVIRONMENT_CACHE_BYTES];
	uint32_t completed = 0u;

	while (completed < count) {
		uint32_t remaining = count - completed;
		size_t amount = remaining < sizeof(staging)
				? (size_t)remaining
				: sizeof(staging);
		size_t index;
		enum dos_environment_mutation_status status;

		for (index = 0u; index < amount; ++index) {
			status = reader_byte(source,
					     source_offset + completed +
						     (uint32_t)index,
					     &staging[index]);
			if (status != DOS_ENVIRONMENT_MUTATION_OK)
				return status;
		}
		status = write_range(source->machine, destination_segment,
				     destination_offset, staging,
				     sizeof(staging), amount);
		if (status != DOS_ENVIRONMENT_MUTATION_OK)
			return status;
		completed += (uint32_t)amount;
	}
	return DOS_ENVIRONMENT_MUTATION_OK;
}

static enum dos_environment_mutation_status build_replacement(
	const struct dos_machine *machine, uint16_t source_segment,
	const struct environment_layout *source_layout,
	uint16_t destination_segment,
	enum dos_environment_mutation_action action, const uint8_t *name,
	size_t name_length, const uint8_t *value,
	size_t value_capacity, size_t value_length, uint32_t payload_bytes)
{
	struct environment_reader source = {
		.machine = machine,
		.segment = source_segment,
		.capacity = source_layout->capacity,
	};
	uint32_t source_cursor = 0u;
	uint32_t destination_cursor = 0u;
	uint32_t result_entries = source_layout->retained_entries;
	static const uint8_t equal = (uint8_t)'=';
	static const uint8_t zeros[2] = {0u, 0u};
	uint8_t trailer[2];
	enum dos_environment_mutation_status status;

	for (;;) {
		struct environment_entry entry;

		status = inspect_entry(&source, source_cursor, name, name_length,
				       &entry);
		if (status != DOS_ENVIRONMENT_MUTATION_OK)
			return status;
		if (entry.bytes == 0u)
			break;
		if (!entry.matches) {
			status = copy_guest_range(&source, entry.start, entry.bytes,
						  destination_segment,
						  &destination_cursor);
			if (status != DOS_ENVIRONMENT_MUTATION_OK)
				return status;
		}
		source_cursor += entry.bytes;
	}
	if (action == DOS_ENVIRONMENT_MUTATION_SET) {
		status = write_upper_name(machine, destination_segment,
					  &destination_cursor, name, name_length);
		if (status != DOS_ENVIRONMENT_MUTATION_OK)
			return status;
		status = write_range(machine, destination_segment,
				     &destination_cursor, &equal, sizeof(equal),
				     sizeof(equal));
		if (status != DOS_ENVIRONMENT_MUTATION_OK)
			return status;
		status = write_range(machine, destination_segment,
				     &destination_cursor, value, value_capacity,
				     value_length);
		if (status != DOS_ENVIRONMENT_MUTATION_OK)
			return status;
		status = write_range(machine, destination_segment,
				     &destination_cursor, zeros, sizeof(zeros), 1u);
		if (status != DOS_ENVIRONMENT_MUTATION_OK)
			return status;
		++result_entries;
	}
	status = write_range(machine, destination_segment, &destination_cursor,
			     zeros, sizeof(zeros),
			     result_entries == 0u ? 2u : 1u);
	if (status != DOS_ENVIRONMENT_MUTATION_OK)
		return status;
	status = reader_byte(&source, source_layout->trailer_offset,
			     &trailer[0]);
	if (status != DOS_ENVIRONMENT_MUTATION_OK)
		return status;
	status = reader_byte(&source, source_layout->trailer_offset + 1u,
			     &trailer[1]);
	if (status != DOS_ENVIRONMENT_MUTATION_OK)
		return status;
	status = write_range(machine, destination_segment, &destination_cursor,
			     trailer, sizeof(trailer), sizeof(trailer));
	if (status != DOS_ENVIRONMENT_MUTATION_OK)
		return status;
	status = copy_guest_range(&source, source_layout->argv0_offset,
				 source_layout->argv0_bytes,
				 destination_segment, &destination_cursor);
	if (status != DOS_ENVIRONMENT_MUTATION_OK)
		return status;
	return destination_cursor == payload_bytes
		       ? DOS_ENVIRONMENT_MUTATION_OK
		       : DOS_ENVIRONMENT_MUTATION_BAD_LAYOUT;
}

static enum dos_environment_mutation_status compare_guest_ranges(
	const struct dos_machine *machine, uint16_t first_segment,
	uint32_t first_offset, uint32_t first_capacity,
	uint16_t second_segment, uint32_t second_offset,
	uint32_t second_capacity, uint32_t count)
{
	struct environment_reader first = {
		.machine = machine,
		.segment = first_segment,
		.capacity = first_capacity,
	};
	struct environment_reader second = {
		.machine = machine,
		.segment = second_segment,
		.capacity = second_capacity,
	};
	uint32_t index;

	for (index = 0u; index < count; ++index) {
		uint8_t first_byte;
		uint8_t second_byte;
		enum dos_environment_mutation_status status;

		status = reader_byte(&first, first_offset + index, &first_byte);
		if (status != DOS_ENVIRONMENT_MUTATION_OK)
			return status;
		status = reader_byte(&second, second_offset + index,
				     &second_byte);
		if (status != DOS_ENVIRONMENT_MUTATION_OK)
			return status;
		if (first_byte != second_byte)
			return DOS_ENVIRONMENT_MUTATION_BAD_LAYOUT;
	}
	return DOS_ENVIRONMENT_MUTATION_OK;
}

static enum dos_environment_mutation_status revalidate_replacement(
	const struct dos_memory_arena *arena, const struct dos_machine *machine,
	uint16_t psp_segment, uint16_t old_segment,
	const struct environment_layout *old_layout, uint16_t new_segment,
	uint32_t expected_capacity, uint32_t expected_payload,
	enum dos_environment_mutation_action action, const uint8_t *name,
	size_t name_length)
{
	struct environment_layout layout;
	uint32_t capacity;
	enum dos_environment_mutation_status status;

	status = validate_owned_block(arena, machine, psp_segment, new_segment,
				      &capacity);
	if (status != DOS_ENVIRONMENT_MUTATION_OK)
		return status;
	if (capacity != expected_capacity)
		return DOS_ENVIRONMENT_MUTATION_BAD_BLOCK;
	status = scan_layout(machine, new_segment, capacity, name, name_length,
			     &layout);
	if (status != DOS_ENVIRONMENT_MUTATION_OK)
		return status;
	if (layout.retained_bytes != old_layout->retained_bytes ||
	    layout.retained_entries != old_layout->retained_entries ||
	    layout.payload_bytes != expected_payload ||
	    layout.argv0_bytes != old_layout->argv0_bytes ||
	    (action == DOS_ENVIRONMENT_MUTATION_SET &&
	     layout.matching_entries != 1u) ||
	    (action == DOS_ENVIRONMENT_MUTATION_DELETE &&
	     layout.matching_entries != 0u))
		return DOS_ENVIRONMENT_MUTATION_BAD_LAYOUT;
	return compare_guest_ranges(
		machine, old_segment, old_layout->argv0_offset,
		old_layout->capacity, new_segment, layout.argv0_offset,
		layout.capacity, old_layout->argv0_bytes);
}

static enum dos_environment_mutation_status discard_replacement(
	struct dos_memory_arena *arena, const struct dos_machine *machine,
	uint16_t segment, uint16_t owner,
	enum dos_environment_mutation_status original_status)
{
	enum dos_memory_status status = dos_memory_free_owned_checked(
		arena, machine, segment, owner);

	if (status != DOS_MEMORY_OK)
		return poison_transaction(arena);
	return original_status;
}

static bool mutation_arguments_are_valid(
	struct dos_memory_arena *arena, const struct dos_machine *machine,
	uint16_t psp_segment, enum dos_environment_mutation_action action,
	const uint8_t *name, size_t name_capacity, size_t name_length,
	const uint8_t *value, size_t value_capacity, size_t value_length)
{
	size_t index;

	if (arena == NULL || machine == NULL || psp_segment == 0u ||
	    (action != DOS_ENVIRONMENT_MUTATION_SET &&
	     action != DOS_ENVIRONMENT_MUTATION_DELETE) ||
	    name == NULL || name_length == 0u ||
	    name_length > DOS_ENVIRONMENT_MUTATION_MAX_NAME_BYTES ||
	    name_length > name_capacity || value_length > value_capacity ||
	    (value == NULL && value_length != 0u) ||
	    (action == DOS_ENVIRONMENT_MUTATION_DELETE &&
	     (value != NULL || value_capacity != 0u || value_length != 0u)))
		return false;
	for (index = 0u; index < name_length; ++index) {
		if (name[index] == 0u || name[index] == (uint8_t)'=')
			return false;
	}
	for (index = 0u; index < value_length; ++index) {
		if (value[index] == 0u)
			return false;
	}
	return true;
}

static enum dos_environment_mutation_status calculate_replacement_size(
	const struct environment_layout *layout,
	enum dos_environment_mutation_action action, size_t name_length,
	size_t value_length, uint32_t *payload_bytes, uint16_t *paragraphs)
{
	uint32_t payload = layout->retained_bytes;
	uint32_t result_entries = layout->retained_entries;
	uint32_t allocation_bytes;

	if (action == DOS_ENVIRONMENT_MUTATION_SET) {
		if (name_length > DOS_ENVIRONMENT_MUTATION_MAX_BLOCK_BYTES ||
		    value_length > DOS_ENVIRONMENT_MUTATION_MAX_BLOCK_BYTES ||
		    check_add_overflow(payload, (uint32_t)name_length, &payload) ||
		    check_add_overflow(payload, 1u, &payload) ||
		    check_add_overflow(payload, (uint32_t)value_length,
				       &payload) ||
		    check_add_overflow(payload, 1u, &payload))
			return DOS_ENVIRONMENT_MUTATION_RANGE_OVERFLOW;
		++result_entries;
	}
	if (check_add_overflow(payload, result_entries == 0u ? 2u : 1u,
			       &payload) ||
	    check_add_overflow(payload, 2u, &payload) ||
	    check_add_overflow(payload, layout->argv0_bytes, &payload) ||
	    payload > DOS_ENVIRONMENT_MUTATION_MAX_BLOCK_BYTES ||
	    check_add_overflow(payload, 15u, &allocation_bytes))
		return DOS_ENVIRONMENT_MUTATION_RANGE_OVERFLOW;
	allocation_bytes &= ~15u;
	if (allocation_bytes == 0u ||
	    allocation_bytes > DOS_ENVIRONMENT_MUTATION_MAX_BLOCK_BYTES)
		return DOS_ENVIRONMENT_MUTATION_RANGE_OVERFLOW;
	*payload_bytes = payload;
	*paragraphs = (uint16_t)(allocation_bytes /
				 DOS_ENVIRONMENT_PARAGRAPH_BYTES);
	return DOS_ENVIRONMENT_MUTATION_OK;
}

enum dos_environment_mutation_status dos_environment_mutate_psp(
	struct dos_memory_arena *arena, const struct dos_machine *machine,
	uint16_t psp_segment, enum dos_environment_mutation_action action,
	const uint8_t *name, size_t name_capacity, size_t name_length,
	const uint8_t *value, size_t value_capacity, size_t value_length)
{
	uint8_t encoded_old_segment[2];
	uint8_t encoded_new_segment[2];
	uint8_t publication_rollback[2];
	struct environment_layout old_layout;
	struct dos_memory_allocation_result allocation;
	uint16_t old_segment;
	uint16_t paragraphs;
	uint32_t payload_bytes;
	uint32_t replacement_capacity;
	enum dos_environment_mutation_status status;
	enum dos_memory_status arena_status;
	enum dos_machine_status publish_status;

	if (!mutation_arguments_are_valid(
			arena, machine, psp_segment, action, name, name_capacity,
			name_length, value, value_capacity, value_length))
		return DOS_ENVIRONMENT_MUTATION_INVALID_ARGUMENT;
	if (dos_machine_read_far(machine, psp_segment, 0x2cu,
				 encoded_old_segment, sizeof(encoded_old_segment),
				 sizeof(encoded_old_segment)) != DOS_MACHINE_OK)
		return DOS_ENVIRONMENT_MUTATION_MACHINE_FAULT;
	old_segment = read_le16(encoded_old_segment);
	if (old_segment == 0u)
		return DOS_ENVIRONMENT_MUTATION_BAD_PSP;
	status = validate_owned_block(arena, machine, psp_segment, old_segment,
				      &old_layout.capacity);
	if (status != DOS_ENVIRONMENT_MUTATION_OK)
		return status;
	status = scan_layout(machine, old_segment, old_layout.capacity, name,
			     name_length, &old_layout);
	if (status != DOS_ENVIRONMENT_MUTATION_OK)
		return status;
	if (action == DOS_ENVIRONMENT_MUTATION_DELETE &&
	    old_layout.matching_entries == 0u)
		return DOS_ENVIRONMENT_MUTATION_NOT_FOUND;
	status = calculate_replacement_size(&old_layout, action, name_length,
					    value_length, &payload_bytes,
					    &paragraphs);
	if (status != DOS_ENVIRONMENT_MUTATION_OK)
		return status;
	arena_status = dos_memory_allocate_checked(
		arena, machine, psp_segment, paragraphs, &allocation);
	if (arena_status != DOS_MEMORY_OK)
		return memory_status(arena_status);
	replacement_capacity =
		(uint32_t)paragraphs * DOS_ENVIRONMENT_PARAGRAPH_BYTES;
	status = build_replacement(
		machine, old_segment, &old_layout, allocation.block_segment,
		action, name, name_length, value, value_capacity,
		value_length, payload_bytes);
	if (status != DOS_ENVIRONMENT_MUTATION_OK)
		return discard_replacement(arena, machine,
					   allocation.block_segment, psp_segment,
					   status);
	status = revalidate_replacement(
		arena, machine, psp_segment, old_segment, &old_layout,
		allocation.block_segment, replacement_capacity, payload_bytes,
		action, name, name_length);
	if (status != DOS_ENVIRONMENT_MUTATION_OK)
		return discard_replacement(arena, machine,
					   allocation.block_segment, psp_segment,
					   status);
	write_le16(encoded_new_segment, allocation.block_segment);
	publish_status = dos_machine_replace_far(
		machine, psp_segment, 0x2cu, encoded_new_segment,
		sizeof(encoded_new_segment), publication_rollback,
		sizeof(publication_rollback), sizeof(encoded_new_segment));
	if (publish_status != DOS_MACHINE_OK) {
		if (publish_status == DOS_MACHINE_ROLLBACK_FAILED)
			return poison_transaction(arena);
		return discard_replacement(
			arena, machine, allocation.block_segment, psp_segment,
			DOS_ENVIRONMENT_MUTATION_MACHINE_FAULT);
	}
	if (read_le16(publication_rollback) != old_segment) {
		uint8_t stale_rollback[2];
		enum dos_machine_status restore_status = dos_machine_replace_far(
			machine, psp_segment, 0x2cu, publication_rollback,
			sizeof(publication_rollback), stale_rollback,
			sizeof(stale_rollback), sizeof(publication_rollback));

		if (restore_status != DOS_MACHINE_OK ||
		    read_le16(stale_rollback) != allocation.block_segment)
			return poison_transaction(arena);
		return discard_replacement(
			arena, machine, allocation.block_segment, psp_segment,
			DOS_ENVIRONMENT_MUTATION_STALE_PSP);
	}
	arena_status = dos_memory_free_owned_checked(
		arena, machine, old_segment, psp_segment);
	if (arena_status == DOS_MEMORY_OK)
		return DOS_ENVIRONMENT_MUTATION_OK;
	if (arena_status != DOS_MEMORY_MACHINE_FAULT)
		return poison_transaction(arena);
	{
		uint8_t rollback_current[2];
		enum dos_machine_status restore_status = dos_machine_replace_far(
			machine, psp_segment, 0x2cu, encoded_old_segment,
			sizeof(encoded_old_segment), rollback_current,
			sizeof(rollback_current), sizeof(encoded_old_segment));

		if (restore_status != DOS_MACHINE_OK ||
		    read_le16(rollback_current) != allocation.block_segment)
			return poison_transaction(arena);
	}
	status = memory_status(arena_status);
	return discard_replacement(arena, machine, allocation.block_segment,
				   psp_segment, status);
}

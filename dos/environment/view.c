// SPDX-License-Identifier: GPL-2.0-only
/*
 * Read-only PSP environment lookup.
 *
 * One bounded reader owns traversal state and publishes a result only after
 * the complete matching component has been validated. Matching follows
 * MS-DOS environment and PATH semantics.
 */
#include "dos_environment_view.h"

#include "dos_abi.h"
#include "dos_environment.h"

#define ENVIRONMENT_VIEW_CACHE_BYTES 128u
#define ENVIRONMENT_MCB_OWNER_OFFSET 1u
#define ENVIRONMENT_MCB_SIZE_OFFSET 3u

struct environment_reader {
	const struct dos_machine *machine;
	uint32_t limit;
	uint32_t cache_base;
	size_t cache_length;
	uint16_t segment;
	uint8_t cache[ENVIRONMENT_VIEW_CACHE_BYTES];
	bool cache_valid;
};

static uint16_t read_le16(const uint8_t bytes[2])
{
	return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8u);
}

static uint8_t environment_upper(uint8_t value)
{
	return value >= (uint8_t)'a' && value <= (uint8_t)'z'
		       ? (uint8_t)(value - ((uint8_t)'a' - (uint8_t)'A'))
		       : value;
}

static enum dos_environment_view_status reader_byte(
	struct environment_reader *reader, uint32_t offset, uint8_t *value)
{
	uint32_t base;
	size_t amount;

	if (offset >= reader->limit)
		return DOS_ENVIRONMENT_VIEW_BAD_BLOCK;
	if (!reader->cache_valid || offset < reader->cache_base ||
	    offset - reader->cache_base >= reader->cache_length) {
		base = offset - offset % ENVIRONMENT_VIEW_CACHE_BYTES;
		amount = (size_t)(reader->limit - base);
		if (amount > sizeof(reader->cache))
			amount = sizeof(reader->cache);
		if (dos_machine_read_far(reader->machine, reader->segment,
					 (uint16_t)base, reader->cache,
					 sizeof(reader->cache), amount) !=
		    DOS_MACHINE_OK)
			return DOS_ENVIRONMENT_VIEW_MACHINE_FAULT;
		reader->cache_base = base;
		reader->cache_length = amount;
		reader->cache_valid = true;
	}
	*value = reader->cache[offset - reader->cache_base];
	return DOS_ENVIRONMENT_VIEW_OK;
}

static enum dos_environment_view_status prepare_reader(
	const struct dos_machine *machine, uint16_t psp_segment,
	struct environment_reader *reader)
{
	uint8_t encoded_segment[2];
	uint8_t mcb[5];
	uint16_t environment_segment;
	uint16_t paragraphs;
	uint32_t limit;

	if (dos_machine_read_far(machine, psp_segment, 0x2cu,
				 encoded_segment, sizeof(encoded_segment),
				 sizeof(encoded_segment)) != DOS_MACHINE_OK)
		return DOS_ENVIRONMENT_VIEW_MACHINE_FAULT;
	environment_segment = read_le16(encoded_segment);
	if (environment_segment == 0u)
		return DOS_ENVIRONMENT_VIEW_BAD_PSP;
	if (dos_machine_read_far(machine, (uint16_t)(environment_segment - 1u),
				 0u, mcb, sizeof(mcb), sizeof(mcb)) !=
	    DOS_MACHINE_OK)
		return DOS_ENVIRONMENT_VIEW_MACHINE_FAULT;
	paragraphs = read_le16(mcb + ENVIRONMENT_MCB_SIZE_OFFSET);
	if ((mcb[0] != (uint8_t)'M' && mcb[0] != (uint8_t)'Z') ||
	    read_le16(mcb + ENVIRONMENT_MCB_OWNER_OFFSET) != psp_segment ||
	    paragraphs == 0u)
		return DOS_ENVIRONMENT_VIEW_BAD_BLOCK;
	limit = (uint32_t)paragraphs * DOS_ENVIRONMENT_PARAGRAPH_BYTES;
	if (limit > DOS_ENVIRONMENT_SCAN_LIMIT)
		limit = DOS_ENVIRONMENT_SCAN_LIMIT;
	*reader = (struct environment_reader){
		.machine = machine,
		.limit = limit,
		.segment = environment_segment,
	};
	return DOS_ENVIRONMENT_VIEW_OK;
}

static enum dos_environment_view_status locate_value(
	struct environment_reader *reader, const uint8_t *name,
	size_t name_length, uint32_t *value_start, size_t *value_length)
{
	uint32_t cursor = 0u;

	while (cursor < reader->limit) {
		uint32_t entry_start = cursor;
		uint32_t found_start = 0u;
		size_t name_index = 0u;
		size_t found_length = 0u;
		bool match = true;
		bool has_equal = false;
		uint8_t byte;
		enum dos_environment_view_status status;

		status = reader_byte(reader, cursor, &byte);
		if (status != DOS_ENVIRONMENT_VIEW_OK)
			return status;
		if (byte == 0u)
			return DOS_ENVIRONMENT_VIEW_NOT_FOUND;
		for (;;) {
			status = reader_byte(reader, cursor, &byte);
			if (status != DOS_ENVIRONMENT_VIEW_OK)
				return status;
			++cursor;
			if (byte == 0u)
				break;
			if (!has_equal && byte == (uint8_t)'=') {
				has_equal = true;
				match = match && name_index == name_length;
				found_start = cursor;
				continue;
			}
			if (!has_equal) {
				if (name_index >= name_length ||
				    environment_upper(byte) !=
					    environment_upper(name[name_index]))
					match = false;
				++name_index;
			} else if (match) {
				++found_length;
			}
		}
		if (has_equal && match) {
			*value_start = found_start;
			*value_length = found_length;
			return DOS_ENVIRONMENT_VIEW_OK;
		}
		if (cursor <= entry_start)
			return DOS_ENVIRONMENT_VIEW_BAD_BLOCK;
	}
	return DOS_ENVIRONMENT_VIEW_BAD_BLOCK;
}

enum dos_environment_view_status dos_environment_view_read_value(
	const struct dos_machine *machine, uint16_t psp_segment,
	const uint8_t *name, size_t name_length, uint32_t value_offset,
	uint8_t *destination, size_t destination_capacity,
	size_t *value_length, size_t *bytes_read)
{
	struct environment_reader reader;
	uint32_t value_start = 0u;
	size_t complete_length = 0u;
	size_t amount;
	size_t prepared_length;
	size_t prepared_bytes;
	size_t index;
	uint8_t prepared[DOS_ENVIRONMENT_VIEW_READ_BYTES];
	enum dos_environment_view_status status;

	if (machine == NULL || machine->ops == NULL ||
	    machine->ops->read_memory == NULL || psp_segment == 0u ||
	    name == NULL || name_length == 0u ||
	    name_length > DOS_COMMAND_TAIL_BYTES - 1u ||
	    destination_capacity > sizeof(prepared) ||
	    (destination == NULL && destination_capacity != 0u) ||
	    value_length == NULL || bytes_read == NULL)
		return DOS_ENVIRONMENT_VIEW_INVALID_ARGUMENT;
	for (index = 0u; index < name_length; ++index) {
		if (name[index] == 0u || name[index] == (uint8_t)'=')
			return DOS_ENVIRONMENT_VIEW_INVALID_ARGUMENT;
	}
	status = prepare_reader(machine, psp_segment, &reader);
	if (status != DOS_ENVIRONMENT_VIEW_OK)
		return status;
	status = locate_value(&reader, name, name_length, &value_start,
			      &complete_length);
	if (status != DOS_ENVIRONMENT_VIEW_OK)
		return status;
	if ((uint64_t)value_offset > (uint64_t)complete_length)
		return DOS_ENVIRONMENT_VIEW_INVALID_ARGUMENT;
	amount = complete_length - (size_t)value_offset;
	if (amount > destination_capacity)
		amount = destination_capacity;
	for (index = 0u; index < amount; ++index) {
		uint8_t byte;

		status = reader_byte(&reader,
				     value_start + value_offset + (uint32_t)index,
				     &byte);
		if (status != DOS_ENVIRONMENT_VIEW_OK)
			return status;
		prepared[index] = byte;
	}
	prepared_length = complete_length;
	prepared_bytes = amount;
	for (index = 0u; index < prepared_bytes; ++index)
		destination[index] = prepared[index];
	*value_length = prepared_length;
	*bytes_read = prepared_bytes;
	return DOS_ENVIRONMENT_VIEW_OK;
}

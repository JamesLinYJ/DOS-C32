// SPDX-License-Identifier: GPL-2.0-only
/* MS-DOS byte encoder for FindFirst/FindNext DTA state. */
#include "dos_find.h"

static uint16_t read_le16(const uint8_t *bytes)
{
	return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8u);
}

static uint32_t read_le32(const uint8_t *bytes)
{
	return (uint32_t)read_le16(bytes) |
	       ((uint32_t)read_le16(bytes + 2u) << 16u);
}

static uint64_t read_le64(const uint8_t *bytes)
{
	return (uint64_t)read_le32(bytes) |
	       ((uint64_t)read_le32(bytes + 4u) << 32u);
}

static void write_le16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8u);
}

static void write_le32(uint8_t *bytes, uint32_t value)
{
	write_le16(bytes, (uint16_t)value);
	write_le16(bytes + 2u, (uint16_t)(value >> 16u));
}

static void write_le64(uint8_t *bytes, uint64_t value)
{
	write_le32(bytes, (uint32_t)value);
	write_le32(bytes + 4u, (uint32_t)(value >> 32u));
}

enum dos_find_status dos_find_record_encode(
	const struct dos_find_record *record, uint8_t output[DOS_DTA_FIND_SIZE])
{
	size_t index;
	bool terminated = false;

	if (record == NULL || output == NULL)
		return DOS_FIND_INVALID_ARGUMENT;
	for (index = 0u; index < DOS_FIND_PACKED_NAME_BYTES; ++index) {
		if (record->packed_name[index] == 0u) {
			terminated = true;
			break;
		}
	}
	if (!terminated)
		return DOS_FIND_INVALID_NAME;
	for (index = 0u; index < DOS_DTA_FIND_SIZE; ++index)
		output[index] = 0u;
	output[0] = record->search_drive;
	for (index = 0u; index < DOS_FIND_NAME83_BYTES; ++index)
		output[1u + index] = record->search_name[index];
	output[12] = record->search_attributes;
	write_le64(output + 13u, record->search_handle);
	output[21] = record->found_attributes;
	write_le16(output + 22u, record->modified_time);
	write_le16(output + 24u, record->modified_date);
	write_le32(output + 26u, record->file_size);
	for (index = 0u; index < DOS_FIND_PACKED_NAME_BYTES; ++index)
		output[30u + index] = record->packed_name[index];
	return DOS_FIND_OK;
}

enum dos_find_status dos_find_record_decode(
	const uint8_t input[DOS_DTA_FIND_SIZE], struct dos_find_record *record)
{
	struct dos_find_record prepared = {0};
	size_t index;

	if (input == NULL || record == NULL)
		return DOS_FIND_INVALID_ARGUMENT;
	prepared.search_drive = input[0];
	for (index = 0u; index < DOS_FIND_NAME83_BYTES; ++index)
		prepared.search_name[index] = input[1u + index];
	prepared.search_attributes = input[12];
	prepared.search_handle = read_le64(input + 13u);
	prepared.found_attributes = input[21];
	prepared.modified_time = read_le16(input + 22u);
	prepared.modified_date = read_le16(input + 24u);
	prepared.file_size = read_le32(input + 26u);
	for (index = 0u; index < DOS_FIND_PACKED_NAME_BYTES; ++index)
		prepared.packed_name[index] = input[30u + index];
	*record = prepared;
	return DOS_FIND_OK;
}

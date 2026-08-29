// SPDX-License-Identifier: GPL-2.0-only
/*
 * DOS-C32 COUNTRY.SYS encoder and all-or-nothing validator.
 *
 * This is intentionally not the historical COUNTRY.SYS object layout.  The
 * file contains no native pointers or compiler structures: every field is a
 * fixed-width little-endian value, every region is length-delimited, and the
 * complete file is authenticated by CRC32 before any package is published.
 * All geometry is evaluated as 64-bit even on the current i386 kernel.
 */
#include "dos_country_file.h"

#include "overflow.h"
#include "string.h"

#define COUNTRY_DIRECTORY_OFFSET DOS_COUNTRY_HEADER_BYTES
#define COUNTRY_COMPLETE_FLAG 0x00000001u
#define COUNTRY_CRC_OFFSET 28u
#define COUNTRY_CRC_BYTES 4u

static const uint8_t country_magic[8] = {
	'C', '3', '2', 'N', 'L', 'S', '1', 0u,
};

static uint16_t read_le16(const uint8_t *bytes)
{
	return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8u);
}

static uint32_t read_le32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8u) |
	       ((uint32_t)bytes[2] << 16u) | ((uint32_t)bytes[3] << 24u);
}

static void write_le16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8u);
}

static void write_le32(uint8_t *bytes, uint32_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8u);
	bytes[2] = (uint8_t)(value >> 16u);
	bytes[3] = (uint8_t)(value >> 24u);
}

static bool bytes_equal(const uint8_t *left, const uint8_t *right,
			size_t count)
{
	size_t index;

	for (index = 0u; index < count; ++index)
		if (left[index] != right[index])
			return false;
	return true;
}

static uint32_t country_crc32(const uint8_t *file, size_t file_bytes)
{
	uint32_t crc = 0xffffffffu;
	size_t index;

	for (index = 0u; index < file_bytes; ++index) {
		uint8_t byte = file[index];
		uint32_t bit;

		if (index >= COUNTRY_CRC_OFFSET &&
		    index < COUNTRY_CRC_OFFSET + COUNTRY_CRC_BYTES)
			byte = 0u;
		crc ^= byte;
		for (bit = 0u; bit < 8u; ++bit)
			crc = (crc >> 1u) ^
			      (0xedb88320u & (0u - (crc & 1u)));
	}
	return ~crc;
}

static enum dos_country_status complete_package_geometry(uint16_t *count,
							  uint64_t *total)
{
	uint64_t result = DOS_COUNTRY_HEADER_BYTES;
	uint16_t complete_count = 0u;
	size_t index;

	for (index = 0u; index < dos_nls_package_count(); ++index) {
		const struct dos_nls_package *package = dos_nls_package_at(index);
		uint64_t payload;

		if (package == NULL)
			return DOS_COUNTRY_FORMAT;
		if (!package->complete)
			continue;
		if (complete_count == DOS_COUNTRY_MAX_PACKAGES ||
		    package->dbcs.length > DOS_COUNTRY_MAX_DBCS_BYTES)
			return DOS_COUNTRY_CAPACITY;
		payload = (uint64_t)DOS_COUNTRY_HIGH_TABLE_BYTES * 2u;
		if (check_add_overflow(payload, (uint64_t)package->dbcs.length,
				       &payload) ||
		    check_add_overflow(result, payload, &result))
			return DOS_COUNTRY_OVERFLOW;
		++complete_count;
	}
	if (complete_count == 0u ||
	    check_add_overflow(result,
		(uint64_t)complete_count * DOS_COUNTRY_ENTRY_BYTES, &result) ||
	    result > 0xffffffffu)
		return DOS_COUNTRY_OVERFLOW;
	*count = complete_count;
	*total = result;
	return DOS_COUNTRY_OK;
}

enum dos_country_status dos_country_encoded_size(uint64_t *encoded_bytes)
{
	uint64_t total = 0u;
	uint16_t count = 0u;
	enum dos_country_status status;

	if (encoded_bytes == NULL)
		return DOS_COUNTRY_INVALID_ARGUMENT;
	*encoded_bytes = 0u;
	status = complete_package_geometry(&count, &total);
	if (status != DOS_COUNTRY_OK)
		return status;
	*encoded_bytes = total;
	return DOS_COUNTRY_OK;
}

enum dos_country_status dos_country_encode(uint8_t *destination,
					    size_t capacity, size_t *written)
{
	uint64_t total64;
	uint64_t directory_bytes;
	uint64_t payload_cursor;
	uint16_t count;
	uint16_t output_index = 0u;
	size_t index;
	enum dos_country_status status;

	if (written != NULL)
		*written = 0u;
	if (destination == NULL || written == NULL)
		return DOS_COUNTRY_INVALID_ARGUMENT;
	status = complete_package_geometry(&count, &total64);
	if (status != DOS_COUNTRY_OK)
		return status;
	if (total64 > (uint64_t)capacity || total64 > (uint64_t)(size_t)-1)
		return DOS_COUNTRY_CAPACITY;
	if (memset_s(destination, capacity, 0, (size_t)total64) != MEMORY_OK ||
	    memcpy_s(destination, capacity, country_magic,
		     sizeof(country_magic), sizeof(country_magic)) != MEMORY_OK)
		return DOS_COUNTRY_CAPACITY;
	write_le16(destination + 8u, DOS_COUNTRY_FILE_VERSION);
	write_le16(destination + 10u, DOS_COUNTRY_HEADER_BYTES);
	write_le32(destination + 12u, (uint32_t)total64);
	write_le16(destination + 16u, count);
	write_le16(destination + 18u, DOS_COUNTRY_ENTRY_BYTES);
	write_le32(destination + 20u, COUNTRY_DIRECTORY_OFFSET);
	directory_bytes = (uint64_t)count * DOS_COUNTRY_ENTRY_BYTES;
	payload_cursor = DOS_COUNTRY_HEADER_BYTES + directory_bytes;
	write_le32(destination + 24u, (uint32_t)payload_cursor);

	for (index = 0u; index < dos_nls_package_count(); ++index) {
		const struct dos_nls_package *package = dos_nls_package_at(index);
		uint8_t *entry;
		uint32_t offset;

		if (package == NULL || !package->complete)
			continue;
		entry = destination + DOS_COUNTRY_HEADER_BYTES +
			(size_t)output_index * DOS_COUNTRY_ENTRY_BYTES;
		write_le16(entry, package->code_page);
		write_le16(entry + 2u, package->format.country);
		write_le32(entry + 4u, COUNTRY_COMPLETE_FLAG);
		write_le16(entry + 8u, package->format.date_format);
		entry[10] = package->format.currency_format;
		entry[11] = package->format.currency_digits;
		entry[12] = package->format.time_format;
		if (memcpy_s(entry + 13u, 5u, package->format.currency, 5u, 5u) !=
		    MEMORY_OK)
			return DOS_COUNTRY_FORMAT;
		entry[18] = package->format.thousands_separator;
		entry[19] = package->format.decimal_separator;
		entry[20] = package->format.date_separator;
		entry[21] = package->format.time_separator;
		entry[22] = package->format.list_separator;

		offset = (uint32_t)payload_cursor;
		write_le32(entry + 24u, offset);
		write_le16(entry + 28u, DOS_COUNTRY_HIGH_TABLE_BYTES);
		if (memcpy_s(destination + offset,
			 (size_t)(total64 - payload_cursor),
			 package->collate_high, DOS_COUNTRY_HIGH_TABLE_BYTES,
			 DOS_COUNTRY_HIGH_TABLE_BYTES) != MEMORY_OK)
			return DOS_COUNTRY_FORMAT;
		payload_cursor += DOS_COUNTRY_HIGH_TABLE_BYTES;

		offset = (uint32_t)payload_cursor;
		write_le32(entry + 32u, offset);
		write_le16(entry + 36u, DOS_COUNTRY_HIGH_TABLE_BYTES);
		if (memcpy_s(destination + offset,
			 (size_t)(total64 - payload_cursor),
			 package->upcase_high, DOS_COUNTRY_HIGH_TABLE_BYTES,
			 DOS_COUNTRY_HIGH_TABLE_BYTES) != MEMORY_OK)
			return DOS_COUNTRY_FORMAT;
		payload_cursor += DOS_COUNTRY_HIGH_TABLE_BYTES;

		offset = (uint32_t)payload_cursor;
		write_le32(entry + 40u, offset);
		write_le16(entry + 44u, package->dbcs.length);
		if (memcpy_s(destination + offset,
			 (size_t)(total64 - payload_cursor),
			 package->dbcs.ranges, package->dbcs.length,
			 package->dbcs.length) != MEMORY_OK)
			return DOS_COUNTRY_FORMAT;
		payload_cursor += package->dbcs.length;
		++output_index;
	}
	if (output_index != count || payload_cursor != total64)
		return DOS_COUNTRY_FORMAT;
	write_le32(destination + COUNTRY_CRC_OFFSET,
		   country_crc32(destination, (size_t)total64));
	*written = (size_t)total64;
	return DOS_COUNTRY_OK;
}

static bool file_region_is_exact(uint64_t offset, uint16_t length,
				 uint64_t expected_offset, uint64_t total,
				 uint64_t *next)
{
	uint64_t end;

	if (offset != expected_offset ||
	    check_add_overflow(offset, (uint64_t)length, &end) || end > total)
		return false;
	*next = end;
	return true;
}

enum dos_country_status dos_country_parse(const uint8_t *file,
					   size_t file_bytes,
					   struct dos_country_catalog *catalog)
{
	uint64_t total;
	uint64_t directory_end;
	uint64_t payload_cursor;
	uint16_t count;
	uint16_t index;
	uint16_t previous_code_page = 0u;

	if (file == NULL || catalog == NULL)
		return DOS_COUNTRY_INVALID_ARGUMENT;
	if (memset_s(catalog, sizeof(*catalog), 0, sizeof(*catalog)) != MEMORY_OK)
		return DOS_COUNTRY_INVALID_ARGUMENT;
	if (file_bytes < DOS_COUNTRY_HEADER_BYTES ||
	    !bytes_equal(file, country_magic, sizeof(country_magic)) ||
	    read_le16(file + 8u) != DOS_COUNTRY_FILE_VERSION ||
	    read_le16(file + 10u) != DOS_COUNTRY_HEADER_BYTES ||
	    read_le16(file + 18u) != DOS_COUNTRY_ENTRY_BYTES ||
	    read_le32(file + 20u) != COUNTRY_DIRECTORY_OFFSET)
		return DOS_COUNTRY_FORMAT;
	total = read_le32(file + 12u);
	count = read_le16(file + 16u);
	if (total != (uint64_t)file_bytes || count == 0u ||
	    count > DOS_COUNTRY_MAX_PACKAGES ||
	    check_add_overflow((uint64_t)DOS_COUNTRY_HEADER_BYTES,
		(uint64_t)count * DOS_COUNTRY_ENTRY_BYTES, &directory_end) ||
	    directory_end > total || read_le32(file + 24u) != directory_end)
		return DOS_COUNTRY_FORMAT;
	if (read_le32(file + COUNTRY_CRC_OFFSET) !=
	    country_crc32(file, file_bytes))
		return DOS_COUNTRY_CHECKSUM;
	payload_cursor = directory_end;
	for (index = 0u; index < count; ++index) {
		const uint8_t *entry = file + DOS_COUNTRY_HEADER_BYTES +
			(size_t)index * DOS_COUNTRY_ENTRY_BYTES;
		struct dos_country_package_storage *slot =
			&catalog->packages[index];
		uint16_t code_page = read_le16(entry);
		uint16_t collate_length = read_le16(entry + 28u);
		uint16_t upcase_length = read_le16(entry + 36u);
		uint16_t dbcs_length = read_le16(entry + 44u);

		if (code_page <= previous_code_page || read_le16(entry + 2u) == 0u ||
		    read_le32(entry + 4u) != COUNTRY_COMPLETE_FLAG ||
		    entry[23] != 0u || read_le16(entry + 30u) != 0u ||
		    read_le16(entry + 38u) != 0u || read_le16(entry + 46u) != 0u ||
		    collate_length != DOS_COUNTRY_HIGH_TABLE_BYTES ||
		    upcase_length != DOS_COUNTRY_HIGH_TABLE_BYTES ||
		    dbcs_length < 2u || dbcs_length > DOS_COUNTRY_MAX_DBCS_BYTES ||
		    (dbcs_length & 1u) != 0u)
			return DOS_COUNTRY_FORMAT;
		if (!file_region_is_exact(read_le32(entry + 24u), collate_length,
					  payload_cursor, total, &payload_cursor))
			return DOS_COUNTRY_FORMAT;
		if (memcpy_s(slot->collate_high, sizeof(slot->collate_high),
			 file + read_le32(entry + 24u), collate_length,
			 collate_length) != MEMORY_OK)
			return DOS_COUNTRY_FORMAT;
		if (!file_region_is_exact(read_le32(entry + 32u), upcase_length,
					  payload_cursor, total, &payload_cursor))
			return DOS_COUNTRY_FORMAT;
		if (memcpy_s(slot->upcase_high, sizeof(slot->upcase_high),
			 file + read_le32(entry + 32u), upcase_length,
			 upcase_length) != MEMORY_OK)
			return DOS_COUNTRY_FORMAT;
		if (!file_region_is_exact(read_le32(entry + 40u), dbcs_length,
					  payload_cursor, total, &payload_cursor))
			return DOS_COUNTRY_FORMAT;
		if (memcpy_s(slot->dbcs, sizeof(slot->dbcs),
			     file + read_le32(entry + 40u), dbcs_length,
			     dbcs_length) != MEMORY_OK)
			return DOS_COUNTRY_FORMAT;

		slot->package = (struct dos_nls_package){
		    .dbcs = {slot->dbcs, dbcs_length},
		    .format = {
			.currency = {entry[13], entry[14], entry[15], entry[16],
				     entry[17]},
			.country = read_le16(entry + 2u),
			.date_format = read_le16(entry + 8u),
			.thousands_separator = entry[18],
			.decimal_separator = entry[19],
			.date_separator = entry[20],
			.time_separator = entry[21],
			.currency_format = entry[10],
			.currency_digits = entry[11],
			.time_format = entry[12],
			.list_separator = entry[22],
		    },
		    .collate_high = slot->collate_high,
		    .upcase_high = slot->upcase_high,
		    .code_page = code_page,
		    .complete = true,
		};
		if (!dos_nls_validate_package(&slot->package))
			return DOS_COUNTRY_FORMAT;
		previous_code_page = code_page;
	}
	if (payload_cursor != total)
		return DOS_COUNTRY_FORMAT;
	catalog->package_count = count;
	catalog->format_version = DOS_COUNTRY_FILE_VERSION;
	catalog->checksum = read_le32(file + COUNTRY_CRC_OFFSET);
	return DOS_COUNTRY_OK;
}

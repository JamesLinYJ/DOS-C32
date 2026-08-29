// SPDX-License-Identifier: GPL-2.0-only
/* FAT12/16/32 table-entry codec for the I/O Manager FAT driver. */
#include "fat_table.h"

#include "overflow.h"

struct fat_entry_format {
	uint32_t value_mask;
	uint32_t reserved_first;
	uint32_t bad_value;
	uint32_t eoc_first;
	uint8_t stored_bytes;
	bool packed_12bit;
};

static bool fat_entry_format_prepare(uint8_t fat_bits,
				     struct fat_entry_format *format)
{
	if (format == NULL)
		return false;

	switch (fat_bits) {
	case FAT_TABLE_12:
		*format = (struct fat_entry_format){
			.value_mask = 0x00000fffu,
			.reserved_first = 0x00000ff0u,
			.bad_value = 0x00000ff7u,
			.eoc_first = 0x00000ff8u,
			.stored_bytes = 2u,
			.packed_12bit = true,
		};
		return true;
	case FAT_TABLE_16:
		*format = (struct fat_entry_format){
			.value_mask = 0x0000ffffu,
			.reserved_first = 0x0000fff0u,
			.bad_value = 0x0000fff7u,
			.eoc_first = 0x0000fff8u,
			.stored_bytes = 2u,
			.packed_12bit = false,
		};
		return true;
	case FAT_TABLE_32:
		*format = (struct fat_entry_format){
			.value_mask = 0x0fffffffu,
			.reserved_first = 0x0ffffff0u,
			.bad_value = 0x0ffffff7u,
			.eoc_first = 0x0ffffff8u,
			.stored_bytes = 4u,
			.packed_12bit = false,
		};
		return true;
	default:
		return false;
	}
}

static bool fat_entry_byte_offset(const struct fat_entry_format *format,
				  uint32_t entry, uint64_t *byte_offset)
{
	uint64_t scaled;

	if (format == NULL || byte_offset == NULL)
		return false;
	if (format->packed_12bit) {
		*byte_offset = (uint64_t)entry + ((uint64_t)entry >> 1);
		return true;
	}
	if (check_mul_overflow((uint64_t)entry,
			       (uint64_t)format->stored_bytes, &scaled))
		return false;
	*byte_offset = scaled;
	return true;
}

enum fat_table_status
fat_table_validate_layout(const struct fat_table_layout *layout)
{
	struct fat_entry_format format;
	uint64_t table_bytes;
	uint64_t final_offset;
	uint64_t required_bytes;

	if (layout == NULL)
		return FAT_TABLE_INVALID_ARGUMENT;
	if (!fat_entry_format_prepare(layout->fat_bits, &format) ||
	    layout->sector_bytes != BLOCK_DEVICE_SECTOR_BYTES ||
	    layout->fat_count == 0u || layout->sectors_per_fat == 0u ||
	    layout->entry_limit < 2u ||
	    layout->entry_limit > format.reserved_first)
		return FAT_TABLE_INVALID_LAYOUT;
	if (check_mul_overflow((uint64_t)layout->sectors_per_fat,
			       (uint64_t)layout->sector_bytes, &table_bytes))
		return FAT_TABLE_OVERFLOW;
	if (!fat_entry_byte_offset(&format, layout->entry_limit - 1u,
				   &final_offset) ||
	    check_add_overflow(final_offset, (uint64_t)format.stored_bytes,
			       &required_bytes))
		return FAT_TABLE_OVERFLOW;
	if (required_bytes > table_bytes)
		return FAT_TABLE_INVALID_LAYOUT;
	return FAT_TABLE_OK;
}

enum fat_table_status
fat_table_locate(const struct fat_table_layout *layout, uint32_t entry,
		 struct fat_table_position *position)
{
	struct fat_entry_format format;
	struct fat_table_position candidate;
	uint64_t byte_offset;
	enum fat_table_status status;

	if (position == NULL)
		return FAT_TABLE_INVALID_ARGUMENT;
	status = fat_table_validate_layout(layout);
	if (status != FAT_TABLE_OK)
		return status;
	if (entry >= layout->entry_limit)
		return FAT_TABLE_OUT_OF_RANGE;
	if (!fat_entry_format_prepare(layout->fat_bits, &format) ||
	    !fat_entry_byte_offset(&format, entry, &byte_offset))
		return FAT_TABLE_OVERFLOW;

	candidate.sector_index = (uint32_t)(byte_offset >> 9);
	candidate.byte_offset = (uint16_t)(byte_offset & 511u);
	candidate.sector_count =
		(format.packed_12bit &&
		 candidate.byte_offset == layout->sector_bytes - 1u)
			? 2u
			: 1u;
	candidate.reserved = 0u;
	if (candidate.sector_index >= layout->sectors_per_fat ||
	    (candidate.sector_count == 2u &&
	     candidate.sector_index >= layout->sectors_per_fat - 1u))
		return FAT_TABLE_INVALID_LAYOUT;
	*position = candidate;
	return FAT_TABLE_OK;
}

enum fat_table_status
fat_table_copy_lba(const struct fat_table_layout *layout, uint8_t copy_index,
		   uint32_t sector_index, block_lba_t *lba)
{
	block_lba_t copy_start;
	block_lba_t relative_lba;
	block_lba_t candidate;
	enum fat_table_status status;

	if (lba == NULL)
		return FAT_TABLE_INVALID_ARGUMENT;
	status = fat_table_validate_layout(layout);
	if (status != FAT_TABLE_OK)
		return status;
	if (copy_index >= layout->fat_count ||
	    sector_index >= layout->sectors_per_fat)
		return FAT_TABLE_OUT_OF_RANGE;
	if (check_mul_overflow((block_lba_t)copy_index,
			       (block_lba_t)layout->sectors_per_fat,
			       &copy_start) ||
	    check_add_overflow(copy_start, (block_lba_t)sector_index,
			       &relative_lba) ||
	    check_add_overflow(layout->fat_start, relative_lba, &candidate))
		return FAT_TABLE_OVERFLOW;
	*lba = candidate;
	return FAT_TABLE_OK;
}

static uint8_t fat_table_window_read(const union block_device_sector *first,
				     const union block_device_sector *second,
				     const struct fat_table_position *position,
				     uint8_t relative_offset)
{
	uint16_t offset = (uint16_t)(position->byte_offset + relative_offset);

	if (offset < BLOCK_DEVICE_SECTOR_BYTES)
		return first->bytes[offset];
	return second->bytes[offset - BLOCK_DEVICE_SECTOR_BYTES];
}

static uint8_t *fat_table_window_write(union block_device_sector *first,
				       union block_device_sector *second,
				       const struct fat_table_position *position,
				       uint8_t relative_offset)
{
	uint16_t offset = (uint16_t)(position->byte_offset + relative_offset);

	if (offset < BLOCK_DEVICE_SECTOR_BYTES)
		return &first->bytes[offset];
	return &second->bytes[offset - BLOCK_DEVICE_SECTOR_BYTES];
}

enum fat_table_entry_kind
fat_table_classify(const struct fat_table_layout *layout, uint32_t value)
{
	struct fat_entry_format format;

	if (fat_table_validate_layout(layout) != FAT_TABLE_OK ||
	    !fat_entry_format_prepare(layout->fat_bits, &format) ||
	    (value & ~format.value_mask) != 0u)
		return FAT_TABLE_ENTRY_INVALID;
	if (value == 0u)
		return FAT_TABLE_ENTRY_FREE;
	if (value == format.bad_value)
		return FAT_TABLE_ENTRY_BAD;
	if (value >= format.eoc_first)
		return FAT_TABLE_ENTRY_EOC;
	if (value == 1u ||
	    (value >= format.reserved_first && value < format.bad_value))
		return FAT_TABLE_ENTRY_RESERVED;
	if (value >= 2u && value < layout->entry_limit)
		return FAT_TABLE_ENTRY_DATA;
	return FAT_TABLE_ENTRY_INVALID;
}

uint32_t fat_table_eoc_value(const struct fat_table_layout *layout)
{
	struct fat_entry_format format;

	if (fat_table_validate_layout(layout) != FAT_TABLE_OK ||
	    !fat_entry_format_prepare(layout->fat_bits, &format))
		return 0u;
	return format.value_mask;
}

enum fat_table_status
fat_table_read(const struct fat_table_layout *layout, uint32_t entry,
	       const union block_device_sector *first,
	       const union block_device_sector *second, uint32_t *value,
	       enum fat_table_entry_kind *kind)
{
	struct fat_entry_format format;
	struct fat_table_position position;
	uint32_t decoded = 0u;
	uint8_t index;
	enum fat_table_status status;

	if (first == NULL || value == NULL || kind == NULL)
		return FAT_TABLE_INVALID_ARGUMENT;
	status = fat_table_locate(layout, entry, &position);
	if (status != FAT_TABLE_OK)
		return status;
	if (position.sector_count == 2u &&
	    (second == NULL || second == first))
		return FAT_TABLE_INVALID_ARGUMENT;
	if (!fat_entry_format_prepare(layout->fat_bits, &format))
		return FAT_TABLE_INVALID_LAYOUT;

	for (index = 0u; index < format.stored_bytes; ++index)
		decoded |= (uint32_t)fat_table_window_read(
			first, second, &position, index) << (index * 8u);
	if (format.packed_12bit && (entry & 1u) != 0u)
		decoded >>= 4;
	decoded &= format.value_mask;
	*value = decoded;
	*kind = fat_table_classify(layout, decoded);
	return FAT_TABLE_OK;
}

enum fat_table_status
fat_table_write(const struct fat_table_layout *layout, uint32_t entry,
		uint32_t value, union block_device_sector *first,
		union block_device_sector *second)
{
	struct fat_entry_format format;
	struct fat_table_position position;
	uint32_t stored_value = value;
	uint8_t *low;
	uint8_t *high;
	uint8_t index;
	enum fat_table_status status;

	if (first == NULL)
		return FAT_TABLE_INVALID_ARGUMENT;
	status = fat_table_locate(layout, entry, &position);
	if (status != FAT_TABLE_OK)
		return status;
	if (position.sector_count == 2u &&
	    (second == NULL || second == first))
		return FAT_TABLE_INVALID_ARGUMENT;
	if (!fat_entry_format_prepare(layout->fat_bits, &format))
		return FAT_TABLE_INVALID_LAYOUT;
	if (fat_table_classify(layout, value) == FAT_TABLE_ENTRY_INVALID)
		return FAT_TABLE_OUT_OF_RANGE;

	low = fat_table_window_write(first, second, &position, 0u);
	if (format.packed_12bit) {
		high = fat_table_window_write(first, second, &position, 1u);
		if ((entry & 1u) == 0u) {
			*low = (uint8_t)stored_value;
			*high = (uint8_t)((*high & 0xf0u) |
					 ((stored_value >> 8) & 0x0fu));
		} else {
			*low = (uint8_t)((*low & 0x0fu) |
					((stored_value << 4) & 0xf0u));
			*high = (uint8_t)(stored_value >> 4);
		}
		return FAT_TABLE_OK;
	}

	if (layout->fat_bits == FAT_TABLE_32) {
		uint32_t old_high =
			(uint32_t)(*fat_table_window_write(first, second,
						  &position, 3u) & 0xf0u)
			<< 24;

		stored_value |= old_high;
	}
	for (index = 0u; index < format.stored_bytes; ++index)
		*fat_table_window_write(first, second, &position, index) =
			(uint8_t)(stored_value >> (index * 8u));
	return FAT_TABLE_OK;
}

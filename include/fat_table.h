/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * FAT12/16/32 table-entry core
 *
 * Compatibility contract: preserve raw FAT entry kinds and mirrored table geometry
 * Safety changes: typed sectors, checked offsets, no VFS/native pointers
 */
#ifndef DOSC32_FAT_TABLE_H
#define DOSC32_FAT_TABLE_H

#include "address.h"
#include "block_device.h"
#include "compiler.h"
#include "types.h"

enum fat_table_variant {
	FAT_TABLE_12 = 12,
	FAT_TABLE_16 = 16,
	FAT_TABLE_32 = 32
};

enum fat_table_status {
	FAT_TABLE_OK = 0,
	FAT_TABLE_INVALID_ARGUMENT,
	FAT_TABLE_INVALID_LAYOUT,
	FAT_TABLE_OUT_OF_RANGE,
	FAT_TABLE_OVERFLOW
};

enum fat_table_entry_kind {
	FAT_TABLE_ENTRY_FREE = 0,
	FAT_TABLE_ENTRY_DATA,
	FAT_TABLE_ENTRY_RESERVED,
	FAT_TABLE_ENTRY_BAD,
	FAT_TABLE_ENTRY_EOC,
	FAT_TABLE_ENTRY_INVALID
};

struct fat_table_layout {
	block_lba_t fat_start;
	uint32_t sectors_per_fat;
	uint32_t entry_limit;
	uint16_t sector_bytes;
	uint8_t fat_count;
	uint8_t fat_bits;
} __aligned(8);

struct fat_table_position {
	uint32_t sector_index;
	uint16_t byte_offset;
	uint8_t sector_count;
	uint8_t reserved;
};

enum fat_table_status
fat_table_validate_layout(const struct fat_table_layout *layout) __must_check;
enum fat_table_status
fat_table_locate(const struct fat_table_layout *layout, uint32_t entry,
		 struct fat_table_position *position) __must_check;
enum fat_table_status
fat_table_copy_lba(const struct fat_table_layout *layout, uint8_t copy_index,
		   uint32_t sector_index, block_lba_t *lba) __must_check;
enum fat_table_status
fat_table_read(const struct fat_table_layout *layout, uint32_t entry,
	       const union block_device_sector *first,
	       const union block_device_sector *second, uint32_t *value,
	       enum fat_table_entry_kind *kind) __must_check;
enum fat_table_status
fat_table_write(const struct fat_table_layout *layout, uint32_t entry,
		uint32_t value, union block_device_sector *first,
		union block_device_sector *second) __must_check;
enum fat_table_entry_kind
fat_table_classify(const struct fat_table_layout *layout, uint32_t value);
uint32_t fat_table_eoc_value(const struct fat_table_layout *layout);

static_assert_expression(sizeof(struct fat_table_layout) == 24u,
			 "FAT table layout must be data-model independent");
static_assert_expression(__alignof__(struct fat_table_layout) == 8u,
			 "FAT table layout alignment changed");
static_assert_expression(sizeof(struct fat_table_position) == 8u,
			 "FAT table position layout changed");

#endif

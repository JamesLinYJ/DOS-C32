// SPDX-License-Identifier: GPL-2.0-only
/* Freestanding FAT12/16/32 entry addressing and encoding tests. */
#include "fat_table.h"
#include "test_entry.h"

static void clear_sector(union block_device_sector *sector)
{
	size_t index;

	for (index = 0u; index < ARRAY_SIZE(sector->bytes); ++index)
		sector->bytes[index] = 0u;
}

static void copy_sector(union block_device_sector *destination,
			const union block_device_sector *source)
{
	size_t index;

	for (index = 0u; index < ARRAY_SIZE(destination->bytes); ++index)
		destination->bytes[index] = source->bytes[index];
}

static bool sectors_equal(const union block_device_sector *left,
			  const union block_device_sector *right)
{
	size_t index;

	for (index = 0u; index < ARRAY_SIZE(left->bytes); ++index)
		if (left->bytes[index] != right->bytes[index])
			return false;
	return true;
}

static int test_fat12(void)
{
	const struct fat_table_layout layout = {
		.fat_start = 1u,
		.sectors_per_fat = 3u,
		.entry_limit = 1000u,
		.sector_bytes = BLOCK_DEVICE_SECTOR_BYTES,
		.fat_count = 2u,
		.fat_bits = FAT_TABLE_12,
	};
	union block_device_sector first;
	union block_device_sector second;
	struct fat_table_position position;
	enum fat_table_entry_kind kind;
	uint32_t value;

	clear_sector(&first);
	clear_sector(&second);
	if (fat_table_validate_layout(&layout) != FAT_TABLE_OK ||
	    fat_table_locate(&layout, 341u, &position) != FAT_TABLE_OK ||
	    position.sector_index != 0u || position.byte_offset != 511u ||
	    position.sector_count != 2u || position.reserved != 0u)
		return 1;
	first.bytes[511] = 0x05u;
	second.bytes[0] = 0x77u;
	if (fat_table_write(&layout, 341u, 0x2bcu, &first, &second) !=
		    FAT_TABLE_OK ||
	    first.bytes[511] != 0xc5u || second.bytes[0] != 0x2bu ||
	    fat_table_read(&layout, 341u, &first, &second, &value, &kind) !=
		    FAT_TABLE_OK ||
	    value != 0x2bcu || kind != FAT_TABLE_ENTRY_DATA)
		return 2;

	clear_sector(&first);
	first.bytes[511] = 0xa5u;
	if (fat_table_locate(&layout, 340u, &position) != FAT_TABLE_OK ||
	    position.byte_offset != 510u || position.sector_count != 1u ||
	    fat_table_write(&layout, 340u, 0x123u, &first, NULL) !=
		    FAT_TABLE_OK ||
	    first.bytes[510] != 0x23u || first.bytes[511] != 0xa1u ||
	    fat_table_read(&layout, 340u, &first, NULL, &value, &kind) !=
		    FAT_TABLE_OK ||
	    value != 0x123u || kind != FAT_TABLE_ENTRY_DATA)
		return 3;
	if (fat_table_classify(&layout, 0xff7u) != FAT_TABLE_ENTRY_BAD ||
	    fat_table_classify(&layout, 0xff8u) != FAT_TABLE_ENTRY_EOC ||
	    fat_table_eoc_value(&layout) != 0xfffu)
		return 4;
	return 0;
}

static int test_fat16(void)
{
	const struct fat_table_layout layout = {
		.fat_start = 7u,
		.sectors_per_fat = 8u,
		.entry_limit = 1000u,
		.sector_bytes = BLOCK_DEVICE_SECTOR_BYTES,
		.fat_count = 2u,
		.fat_bits = FAT_TABLE_16,
	};
	union block_device_sector sector;
	struct fat_table_position position;
	enum fat_table_entry_kind kind;
	block_lba_t lba;
	uint32_t value;

	clear_sector(&sector);
	if (fat_table_locate(&layout, 300u, &position) != FAT_TABLE_OK ||
	    position.sector_index != 1u || position.byte_offset != 88u ||
	    position.sector_count != 1u ||
	    fat_table_write(&layout, 300u, 0xffffu, &sector, NULL) !=
		    FAT_TABLE_OK ||
	    fat_table_read(&layout, 300u, &sector, NULL, &value, &kind) !=
		    FAT_TABLE_OK ||
	    value != 0xffffu || kind != FAT_TABLE_ENTRY_EOC)
		return 1;
	if (fat_table_copy_lba(&layout, 0u, 3u, &lba) != FAT_TABLE_OK ||
	    lba != 10u ||
	    fat_table_copy_lba(&layout, 1u, 3u, &lba) != FAT_TABLE_OK ||
	    lba != 18u ||
	    fat_table_copy_lba(&layout, 2u, 0u, &lba) !=
		    FAT_TABLE_OUT_OF_RANGE)
		return 2;
	return 0;
}

static int test_fat32(void)
{
	const struct fat_table_layout layout = {
		.fat_start = 32u,
		.sectors_per_fat = 1024u,
		.entry_limit = 100000u,
		.sector_bytes = BLOCK_DEVICE_SECTOR_BYTES,
		.fat_count = 2u,
		.fat_bits = FAT_TABLE_32,
	};
	union block_device_sector sector;
	struct fat_table_position position;
	enum fat_table_entry_kind kind;
	uint32_t value;

	clear_sector(&sector);
	if (fat_table_locate(&layout, 2u, &position) != FAT_TABLE_OK ||
	    position.byte_offset != 8u)
		return 1;
	sector.bytes[11] = 0xa0u;
	if (fat_table_write(&layout, 2u, 0x1234u, &sector, NULL) !=
		    FAT_TABLE_OK ||
	    sector.bytes[8] != 0x34u || sector.bytes[9] != 0x12u ||
	    sector.bytes[10] != 0u || sector.bytes[11] != 0xa0u ||
	    fat_table_read(&layout, 2u, &sector, NULL, &value, &kind) !=
		    FAT_TABLE_OK ||
	    value != 0x1234u || kind != FAT_TABLE_ENTRY_DATA)
		return 2;
	if (fat_table_classify(&layout, 0x0ffffff7u) !=
		    FAT_TABLE_ENTRY_BAD ||
	    fat_table_classify(&layout, 0x0ffffff8u) !=
		    FAT_TABLE_ENTRY_EOC ||
	    fat_table_eoc_value(&layout) != 0x0fffffffu)
		return 3;
	return 0;
}

static int test_failed_outputs_are_unchanged(void)
{
	const struct fat_table_layout layout = {
		.fat_start = 1u,
		.sectors_per_fat = 3u,
		.entry_limit = 1000u,
		.sector_bytes = BLOCK_DEVICE_SECTOR_BYTES,
		.fat_count = 2u,
		.fat_bits = FAT_TABLE_12,
	};
	struct fat_table_position position = {
		.sector_index = 0x11223344u,
		.byte_offset = 0x5566u,
		.sector_count = 0x77u,
		.reserved = 0x88u,
	};
	const struct fat_table_layout fat32_layout = {
		.fat_start = 32u,
		.sectors_per_fat = 1024u,
		.entry_limit = 100000u,
		.sector_bytes = BLOCK_DEVICE_SECTOR_BYTES,
		.fat_count = 2u,
		.fat_bits = FAT_TABLE_32,
	};
	union block_device_sector sector;
	union block_device_sector before;
	enum fat_table_entry_kind kind = FAT_TABLE_ENTRY_BAD;
	block_lba_t lba = 0x8877665544332211ull;
	uint32_t value = 0xa5a55a5au;

	clear_sector(&sector);
	if (fat_table_locate(&layout, layout.entry_limit, &position) !=
		    FAT_TABLE_OUT_OF_RANGE ||
	    position.sector_index != 0x11223344u ||
	    position.byte_offset != 0x5566u || position.sector_count != 0x77u ||
	    position.reserved != 0x88u)
		return 1;
	if (fat_table_copy_lba(&layout, layout.fat_count, 0u, &lba) !=
		    FAT_TABLE_OUT_OF_RANGE ||
	    lba != 0x8877665544332211ull)
		return 2;
	if (fat_table_read(&layout, 341u, &sector, NULL, &value, &kind) !=
		    FAT_TABLE_INVALID_ARGUMENT ||
	    value != 0xa5a55a5au || kind != FAT_TABLE_ENTRY_BAD)
		return 3;
	if (fat_table_read(&layout, 341u, &sector, &sector, &value, &kind) !=
		    FAT_TABLE_INVALID_ARGUMENT ||
	    value != 0xa5a55a5au || kind != FAT_TABLE_ENTRY_BAD)
		return 4;
	sector.bytes[0] = 0x3cu;
	sector.bytes[511] = 0xa5u;
	copy_sector(&before, &sector);
	if (fat_table_write(&layout, 341u, 0x2bcu, &sector, &sector) !=
		    FAT_TABLE_INVALID_ARGUMENT ||
	    !sectors_equal(&sector, &before))
		return 5;
	clear_sector(&sector);
	sector.bytes[8] = 0x11u;
	sector.bytes[9] = 0x22u;
	sector.bytes[10] = 0x33u;
	sector.bytes[11] = 0xa4u;
	copy_sector(&before, &sector);
	if (fat_table_write(&fat32_layout, 2u, 0xf0001234u, &sector, NULL) !=
		    FAT_TABLE_OUT_OF_RANGE ||
	    !sectors_equal(&sector, &before))
		return 6;
	return 0;
}

static int test_exact_table_capacity(void)
{
	struct fat_table_layout layout = {
		.fat_start = 0u,
		.sectors_per_fat = 1u,
		.sector_bytes = BLOCK_DEVICE_SECTOR_BYTES,
		.fat_count = 1u,
	};

	layout.fat_bits = FAT_TABLE_12;
	layout.entry_limit = 341u;
	if (fat_table_validate_layout(&layout) != FAT_TABLE_OK)
		return 1;
	layout.entry_limit = 342u;
	if (fat_table_validate_layout(&layout) != FAT_TABLE_INVALID_LAYOUT)
		return 2;
	layout.fat_bits = FAT_TABLE_16;
	layout.entry_limit = 256u;
	if (fat_table_validate_layout(&layout) != FAT_TABLE_OK)
		return 3;
	layout.entry_limit = 257u;
	if (fat_table_validate_layout(&layout) != FAT_TABLE_INVALID_LAYOUT)
		return 4;
	layout.fat_bits = FAT_TABLE_32;
	layout.entry_limit = 128u;
	if (fat_table_validate_layout(&layout) != FAT_TABLE_OK)
		return 5;
	layout.entry_limit = 129u;
	if (fat_table_validate_layout(&layout) != FAT_TABLE_INVALID_LAYOUT)
		return 6;
	return 0;
}

static int run_tests(void)
{
	struct fat_table_layout invalid = {
		.fat_start = 0u,
		.sectors_per_fat = 1u,
		.entry_limit = 2000u,
		.sector_bytes = BLOCK_DEVICE_SECTOR_BYTES,
		.fat_count = 2u,
		.fat_bits = FAT_TABLE_16,
	};
	struct fat_table_layout overflowing_lba = {
		.fat_start = ~(block_lba_t)0,
		.sectors_per_fat = 1u,
		.entry_limit = 100u,
		.sector_bytes = BLOCK_DEVICE_SECTOR_BYTES,
		.fat_count = 2u,
		.fat_bits = FAT_TABLE_16,
	};
	block_lba_t unchanged_lba = 0x1122334455667788ull;
	int result;

	result = test_fat12();
	if (result != 0)
		return 10 + result;
	result = test_fat16();
	if (result != 0)
		return 20 + result;
	result = test_fat32();
	if (result != 0)
		return 30 + result;
	result = test_failed_outputs_are_unchanged();
	if (result != 0)
		return 40 + result;
	result = test_exact_table_capacity();
	if (result != 0)
		return 50 + result;
	if (fat_table_validate_layout(&invalid) != FAT_TABLE_INVALID_LAYOUT)
		return 60;
	if (fat_table_copy_lba(&overflowing_lba, 1u, 0u, &unchanged_lba) !=
		    FAT_TABLE_OVERFLOW ||
	    unchanged_lba != 0x1122334455667788ull)
		return 61;
	return 0;
}

DOSC32_TEST_ENTRY(run_tests)

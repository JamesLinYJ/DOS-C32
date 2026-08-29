// SPDX-License-Identifier: GPL-2.0-only
/* Freestanding FAT12/16/32 BPB geometry tests. */
#include "fat_volume.h"
#include "test_entry.h"

static void write_le16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
}

static void write_le32(uint8_t *bytes, uint32_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
	bytes[2] = (uint8_t)(value >> 16);
	bytes[3] = (uint8_t)(value >> 24);
}

static void clear_sector(union block_device_sector *sector)
{
	size_t index;

	for (index = 0u; index < ARRAY_SIZE(sector->bytes); ++index)
		sector->bytes[index] = 0u;
}

static void fill_common(union block_device_sector *sector, uint8_t spc,
			uint16_t reserved, uint8_t fats,
			uint16_t root_entries)
{
	clear_sector(sector);
	sector->bytes[0] = 0xebu;
	sector->bytes[1] = 0x3cu;
	sector->bytes[2] = 0x90u;
	write_le16(sector->bytes + 11u, BLOCK_DEVICE_SECTOR_BYTES);
	sector->bytes[13] = spc;
	write_le16(sector->bytes + 14u, reserved);
	sector->bytes[16] = fats;
	write_le16(sector->bytes + 17u, root_entries);
	sector->bytes[21] = 0xf8u;
	sector->bytes[510] = 0x55u;
	sector->bytes[511] = 0xaau;
}

static int test_fat12(void)
{
	union block_device_sector boot;
	struct fat_volume_layout layout;

	fill_common(&boot, 1u, 1u, 2u, 224u);
	write_le16(boot.bytes + 19u, 2880u);
	write_le16(boot.bytes + 22u, 9u);
	if (fat_volume_parse_boot(&boot, 2880u, &layout) != FAT_VOLUME_OK ||
	    layout.fat_bits != FAT_TABLE_12 || layout.total_sectors != 2880u ||
	    layout.root_start != 19u || layout.root_sectors != 14u ||
	    layout.data_start != 33u || layout.data_clusters != 2847u ||
	    layout.cluster_limit != 2849u || layout.root_cluster != 0u ||
	    layout.sectors_per_fat != 9u || layout.fat_count != 2u)
		return 1;
	return 0;
}

static int test_fat16(void)
{
	union block_device_sector boot;
	struct fat_volume_layout layout;

	fill_common(&boot, 4u, 1u, 2u, 512u);
	write_le16(boot.bytes + 19u, 32768u);
	write_le16(boot.bytes + 22u, 32u);
	if (fat_volume_parse_boot(&boot, 32768u, &layout) != FAT_VOLUME_OK ||
	    layout.fat_bits != FAT_TABLE_16 || layout.root_start != 65u ||
	    layout.root_sectors != 32u || layout.data_start != 97u ||
	    layout.data_clusters != 8167u || layout.cluster_limit != 8169u ||
	    layout.sectors_per_cluster != 4u)
		return 1;
	return 0;
}

static int test_fat32(void)
{
	union block_device_sector boot;
	struct fat_volume_layout layout;

	fill_common(&boot, 1u, 32u, 2u, 0u);
	write_le32(boot.bytes + 32u, 70000u);
	write_le32(boot.bytes + 36u, 600u);
	write_le32(boot.bytes + 44u, 2u);
	write_le16(boot.bytes + 48u, 1u);
	write_le16(boot.bytes + 50u, 6u);
	if (fat_volume_parse_boot(&boot, 70000u, &layout) != FAT_VOLUME_OK ||
	    layout.fat_bits != FAT_TABLE_32 || layout.total_sectors != 70000u ||
	    layout.root_start != 1232u || layout.root_sectors != 0u ||
	    layout.data_start != 1232u || layout.data_clusters != 68768u ||
	    layout.cluster_limit != 68770u || layout.root_cluster != 2u ||
	    layout.fsinfo_sector != 1u || layout.backup_boot_sector != 6u)
		return 1;
	return 0;
}

static int run_tests(void)
{
	union block_device_sector boot;
	struct fat_volume_layout layout;
	uint8_t *layout_bytes = (uint8_t *)(void *)&layout;
	size_t index;
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

	fill_common(&boot, 1u, 32u, 2u, 0u);
	write_le32(boot.bytes + 32u, 70000u);
	write_le32(boot.bytes + 36u, 600u);
	write_le32(boot.bytes + 44u, 2u);
	write_le16(boot.bytes + 48u, 32u);
	for (index = 0u; index < sizeof(layout); ++index)
		layout_bytes[index] = 0xa5u;
	if (fat_volume_parse_boot(&boot, 70000u, &layout) !=
		    FAT_VOLUME_CORRUPT)
		return 40;
	for (index = 0u; index < sizeof(layout); ++index) {
		if (layout_bytes[index] != 0xa5u)
			return 41;
	}

	clear_sector(&boot);
	boot.bytes[3] = 'N';
	boot.bytes[4] = 'T';
	boot.bytes[5] = 'F';
	boot.bytes[6] = 'S';
	boot.bytes[7] = ' ';
	boot.bytes[8] = ' ';
	boot.bytes[9] = ' ';
	boot.bytes[10] = ' ';
	if (fat_volume_parse_boot(&boot, 1000u, &layout) !=
		    FAT_VOLUME_NO_MATCH)
		return 42;
	return 0;
}

DOSC32_TEST_ENTRY(run_tests)

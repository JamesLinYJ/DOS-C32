// SPDX-License-Identifier: GPL-2.0-only
/*
 * FAT12/16/32 BPB validator
 *
 * Compatibility contract: FAT volume geometry and cluster-number boundaries
 * Safety changes: byte decoding, checked geometry, all-or-nothing publication
 */
#include "fat_volume.h"

#include "overflow.h"

#define FAT12_CLUSTER_THRESHOLD 4085u
#define FAT16_CLUSTER_THRESHOLD 65525u
#define FAT_DIRECTORY_ENTRY_BYTES 32u
#define FAT32_EXTENDED_OFFSET 36u
#define FAT32_ROOT_CLUSTER_OFFSET 44u
#define FAT32_FSINFO_OFFSET 48u
#define FAT32_BACKUP_BOOT_OFFSET 50u

static uint16_t read_le16(const uint8_t *bytes)
{
	return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static uint32_t read_le32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
	       ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static bool is_power_of_two(uint8_t value)
{
	return value != 0u && (value & (uint8_t)(value - 1u)) == 0u;
}

static bool bytes_equal(const uint8_t *left, const char *right, size_t count)
{
	size_t index;

	for (index = 0u; index < count; ++index) {
		if (left[index] != (uint8_t)right[index])
			return false;
	}
	return true;
}

static bool has_dos_jump(const uint8_t *boot)
{
	return (boot[0] == 0xebu && boot[2] == 0x90u) || boot[0] == 0xe9u;
}

enum fat_volume_status
fat_volume_parse_boot(const union block_device_sector *boot_sector,
		      block_lba_t available_sectors,
		      struct fat_volume_layout *layout)
{
	const uint8_t *boot;
	struct fat_volume_layout prepared = { 0 };
	struct fat_table_layout table;
	uint32_t total16;
	uint32_t total32;
	uint32_t total;
	uint32_t fat16_length;
	uint32_t fat32_length;
	uint32_t fat_area;
	uint32_t root_bytes;
	uint32_t root_rounded;
	uint32_t root_start;
	uint32_t data_start;
	uint32_t data_sectors;
	uint32_t clusters;

	if (boot_sector == NULL || layout == NULL || available_sectors == 0u)
		return FAT_VOLUME_CORRUPT;
	boot = boot_sector->bytes;
	if (bytes_equal(boot + 3u, "EXFAT   ", 8u) ||
	    bytes_equal(boot + 3u, "NTFS    ", 8u))
		return FAT_VOLUME_NO_MATCH;
	if (!has_dos_jump(boot))
		return FAT_VOLUME_NO_MATCH;
	prepared.sector_bytes = read_le16(boot + 11u);
	prepared.sectors_per_cluster = boot[13];
	prepared.reserved_sectors = read_le16(boot + 14u);
	prepared.fat_count = boot[16];
	prepared.root_entries = read_le16(boot + 17u);
	total16 = read_le16(boot + 19u);
	prepared.media = boot[21];
	fat16_length = read_le16(boot + 22u);
	total32 = read_le32(boot + 32u);
	if (prepared.sector_bytes != BLOCK_DEVICE_SECTOR_BYTES)
		return FAT_VOLUME_UNSUPPORTED;
	if (!is_power_of_two(prepared.sectors_per_cluster) ||
	    prepared.sectors_per_cluster > 128u ||
	    prepared.reserved_sectors == 0u || prepared.fat_count == 0u ||
	    (prepared.media & 0xf0u) != 0xf0u)
		return FAT_VOLUME_CORRUPT;
	total = total16 != 0u ? total16 : total32;
	if (total == 0u || (block_lba_t)total > available_sectors)
		return FAT_VOLUME_CORRUPT;
	fat32_length = read_le32(boot + FAT32_EXTENDED_OFFSET);
	prepared.sectors_per_fat =
		fat16_length != 0u ? fat16_length : fat32_length;
	if (prepared.sectors_per_fat == 0u)
		return FAT_VOLUME_CORRUPT;
	if (check_mul_overflow((uint32_t)prepared.root_entries,
			       (uint32_t)FAT_DIRECTORY_ENTRY_BYTES,
			       &root_bytes) ||
	    check_add_overflow(root_bytes,
			       (uint32_t)BLOCK_DEVICE_SECTOR_BYTES - 1u,
			       &root_rounded))
		return FAT_VOLUME_OVERFLOW;
	prepared.root_sectors = root_rounded >> 9;
	if (check_mul_overflow((uint32_t)prepared.fat_count,
			       prepared.sectors_per_fat, &fat_area) ||
	    check_add_overflow((uint32_t)prepared.reserved_sectors, fat_area,
			       &root_start) ||
	    check_add_overflow(root_start, prepared.root_sectors, &data_start))
		return FAT_VOLUME_OVERFLOW;
	if (data_start >= total)
		return FAT_VOLUME_CORRUPT;
	data_sectors = total - data_start;
	clusters = data_sectors / prepared.sectors_per_cluster;
	if (clusters == 0u)
		return FAT_VOLUME_CORRUPT;
	if (clusters < FAT12_CLUSTER_THRESHOLD)
		prepared.fat_bits = FAT_TABLE_12;
	else if (clusters < FAT16_CLUSTER_THRESHOLD)
		prepared.fat_bits = FAT_TABLE_16;
	else
		prepared.fat_bits = FAT_TABLE_32;
	if (prepared.fat_bits == FAT_TABLE_32) {
		if (fat16_length != 0u || prepared.root_entries != 0u ||
		    fat32_length == 0u)
			return FAT_VOLUME_CORRUPT;
		prepared.root_cluster =
			read_le32(boot + FAT32_ROOT_CLUSTER_OFFSET) & 0x0fffffffu;
		prepared.fsinfo_sector =
			read_le16(boot + FAT32_FSINFO_OFFSET);
		prepared.backup_boot_sector =
			read_le16(boot + FAT32_BACKUP_BOOT_OFFSET);
	} else {
		if (fat16_length == 0u || prepared.root_entries == 0u)
			return FAT_VOLUME_CORRUPT;
	}
	if (check_add_overflow(clusters, 2u, &prepared.cluster_limit))
		return FAT_VOLUME_OVERFLOW;
	prepared.total_sectors = total;
	prepared.root_start = root_start;
	prepared.data_start = data_start;
	prepared.data_clusters = clusters;
	table = (struct fat_table_layout){
		.fat_start = prepared.reserved_sectors,
		.sectors_per_fat = prepared.sectors_per_fat,
		.entry_limit = prepared.cluster_limit,
		.sector_bytes = prepared.sector_bytes,
		.fat_count = prepared.fat_count,
		.fat_bits = prepared.fat_bits,
	};
	if (fat_table_validate_layout(&table) != FAT_TABLE_OK)
		return FAT_VOLUME_CORRUPT;
	if (prepared.fat_bits == FAT_TABLE_32 &&
	    (prepared.root_cluster < 2u ||
	     prepared.root_cluster >= prepared.cluster_limit ||
	     prepared.fsinfo_sector >= prepared.reserved_sectors ||
	     (prepared.backup_boot_sector != 0u &&
	      prepared.backup_boot_sector >= prepared.reserved_sectors)))
		return FAT_VOLUME_CORRUPT;
	*layout = prepared;
	return FAT_VOLUME_OK;
}

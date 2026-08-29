/* SPDX-License-Identifier: GPL-2.0-only */
/* Validated FAT12/16/32 on-disk volume geometry. */
#ifndef DOSC32_FAT_VOLUME_H
#define DOSC32_FAT_VOLUME_H

#include "address.h"
#include "block_device.h"
#include "compiler.h"
#include "fat_table.h"
#include "types.h"

enum fat_volume_status {
	FAT_VOLUME_OK = 0,
	FAT_VOLUME_NO_MATCH,
	FAT_VOLUME_CORRUPT,
	FAT_VOLUME_UNSUPPORTED,
	FAT_VOLUME_OVERFLOW
};

struct fat_volume_layout {
	uint32_t total_sectors;
	uint32_t sectors_per_fat;
	uint32_t root_start;
	uint32_t root_sectors;
	uint32_t data_start;
	uint32_t data_clusters;
	uint32_t cluster_limit;
	uint32_t root_cluster;
	uint32_t fsinfo_sector;
	uint32_t backup_boot_sector;
	uint16_t sector_bytes;
	uint16_t reserved_sectors;
	uint16_t root_entries;
	uint8_t sectors_per_cluster;
	uint8_t fat_count;
	uint8_t fat_bits;
	uint8_t media;
} __aligned(8);

enum fat_volume_status
fat_volume_parse_boot(const union block_device_sector *boot,
		      block_lba_t available_sectors,
		      struct fat_volume_layout *layout) __must_check;

static_assert_expression(sizeof(struct fat_volume_layout) == 56u,
			 "FAT volume layout must be data-model independent");
static_assert_expression(__alignof__(struct fat_volume_layout) == 8u,
			 "FAT volume layout alignment changed");

#endif

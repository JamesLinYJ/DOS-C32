/* SPDX-License-Identifier: GPL-2.0-only */
/* I/O Manager registration and mounted-volume snapshots for FAT. */
#ifndef DOSC32_FAT_DRIVER_H
#define DOSC32_FAT_DRIVER_H

#include "fat_volume.h"
#include "iomgr.h"

#define FAT_DRIVER_IDENTITY 0x4641544452495631ull

struct fat_driver_volume_snapshot {
	block_device_handle_t device;
	block_lba_t first_lba;
	block_lba_t sector_count;
	struct fat_volume_layout layout;
} __aligned(8);

enum iomgr_status fat_driver_register(void) __must_check;
enum iomgr_status
fat_driver_get_volume(iomgr_volume_handle_t volume,
		      struct fat_driver_volume_snapshot *snapshot) __must_check;

static_assert_expression(sizeof(struct fat_driver_volume_snapshot) == 80u,
			 "FAT driver volume snapshot layout changed");

#endif

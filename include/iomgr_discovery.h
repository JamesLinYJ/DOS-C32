/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DOSC32_IOMGR_DISCOVERY_H
#define DOSC32_IOMGR_DISCOVERY_H

#include "iomgr.h"

struct iomgr_boot_volume_locator {
	block_lba_t first_lba;
	block_lba_t sector_count;
	uint32_t logical_sector_bytes;
	uint32_t reserved;
} __aligned(8);

enum iomgr_status iomgr_mount_boot_volume(
	block_device_handle_t device,
	const struct iomgr_boot_volume_locator *locator,
	iomgr_volume_handle_t *volume) __must_check;

static_assert_expression(sizeof(struct iomgr_boot_volume_locator) == 24u,
			 "I/O Manager boot-volume locator layout changed");

#endif

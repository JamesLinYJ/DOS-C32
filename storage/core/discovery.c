// SPDX-License-Identifier: GPL-2.0-only
/* Exact boot-volume mounting, independent of partition or filesystem type. */
#include "iomgr_discovery.h"

enum iomgr_status iomgr_mount_boot_volume(
	block_device_handle_t device,
	const struct iomgr_boot_volume_locator *locator,
	iomgr_volume_handle_t *volume)
{
	struct block_device_geometry geometry;
	struct iomgr_mount_request request;

	if (locator == NULL || volume == NULL ||
	    device == BLOCK_DEVICE_HANDLE_INVALID)
		return IOMGR_INVALID_ARGUMENT;
	if (block_device_get_geometry(device, &geometry) != BLOCK_DEVICE_OK)
		return IOMGR_IO_ERROR;
	if (locator->reserved != 0u || locator->sector_count == 0u ||
	    locator->logical_sector_bytes != geometry.logical_sector_bytes ||
	    locator->first_lba >= geometry.sector_count ||
	    locator->sector_count > geometry.sector_count - locator->first_lba)
		return IOMGR_CORRUPT;
	request = (struct iomgr_mount_request){
		.device = device,
		.first_lba = locator->first_lba,
		.sector_count = locator->sector_count,
		.flags = geometry.writable == 1u ? 0u : IOMGR_MOUNT_READ_ONLY,
		.reserved = 0u,
	};
	return iomgr_mount(&request, volume);
}

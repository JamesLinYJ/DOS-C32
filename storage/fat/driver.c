// SPDX-License-Identifier: GPL-2.0-only
/*
 * I/O Manager FAT12/16/32 volume driver
 *
 * Compatibility contract: mounted FAT volume identity and validated DPB geometry
 * Safety changes: fixed generation slots and publish-after-complete mount
 */
#include "fat_driver.h"

#include "internal.h"
#include "iomgr_driver.h"

#define FAT_DRIVER_MAX_VOLUMES 8u
#define FAT_DRIVER_GENERATION_MAX 0xffffffffu

enum fat_driver_slot_state {
	FAT_DRIVER_SLOT_FREE = 0,
	FAT_DRIVER_SLOT_LIVE,
	FAT_DRIVER_SLOT_RETIRED
};

struct fat_driver_slot {
	enum fat_driver_slot_state state;
	uint32_t generation;
	struct fat_driver_volume_snapshot snapshot;
	uint32_t next_free_hint;
	uint32_t free_clusters;
	bool free_clusters_known;
};

static struct fat_driver_slot volumes[FAT_DRIVER_MAX_VOLUMES];

static kernel_object_handle_t make_context(size_t index, uint32_t generation)
{
	return ((uint64_t)generation << 32) | (uint64_t)(index + 1u);
}

static enum iomgr_status resolve_context(kernel_object_handle_t context,
					 struct fat_driver_slot **slot)
{
	uint32_t encoded_slot = (uint32_t)context;
	uint32_t generation = (uint32_t)(context >> 32);
	struct fat_driver_slot *candidate;

	if (context == 0u || context == KERNEL_OBJECT_HANDLE_INVALID ||
	    encoded_slot == 0u || encoded_slot > FAT_DRIVER_MAX_VOLUMES ||
	    generation == 0u)
		return IOMGR_STALE_HANDLE;
	candidate = &volumes[encoded_slot - 1u];
	if (candidate->state != FAT_DRIVER_SLOT_LIVE ||
	    candidate->generation != generation)
		return IOMGR_STALE_HANDLE;
	*slot = candidate;
	return IOMGR_OK;
}

static enum iomgr_probe_result
fat_probe(kernel_object_handle_t context,
	  const struct iomgr_mount_request *request)
{
	union block_device_sector boot;
	struct fat_volume_layout layout;
	enum fat_volume_status status;

	if (context != FAT_DRIVER_IDENTITY || request == NULL)
		return IOMGR_PROBE_IO_ERROR;
	if (block_device_read_sector(request->device, request->first_lba,
				     &boot) != BLOCK_DEVICE_OK)
		return IOMGR_PROBE_IO_ERROR;
	status = fat_volume_parse_boot(&boot, request->sector_count, &layout);
	if (status == FAT_VOLUME_OK)
		return IOMGR_PROBE_MATCH;
	if (status == FAT_VOLUME_NO_MATCH)
		return IOMGR_PROBE_NO_MATCH;
	if (status == FAT_VOLUME_UNSUPPORTED)
		return IOMGR_PROBE_UNSUPPORTED;
	return IOMGR_PROBE_CORRUPT;
}

static size_t reserve_slot(void)
{
	size_t index;

	for (index = 0u; index < FAT_DRIVER_MAX_VOLUMES; ++index) {
		if (volumes[index].state != FAT_DRIVER_SLOT_FREE)
			continue;
		if (volumes[index].generation == FAT_DRIVER_GENERATION_MAX) {
			volumes[index].state = FAT_DRIVER_SLOT_RETIRED;
			continue;
		}
		++volumes[index].generation;
		return index;
	}
	return FAT_DRIVER_MAX_VOLUMES;
}

static enum iomgr_driver_mount_status
fat_mount(kernel_object_handle_t context,
	  const struct iomgr_mount_request *request,
	  struct iomgr_driver_mount_result *result)
{
	union block_device_sector boot;
	struct block_device_geometry geometry;
	struct fat_driver_volume_snapshot prepared;
	enum fat_volume_status status;
	size_t slot_index;

	if (context != FAT_DRIVER_IDENTITY || request == NULL || result == NULL)
		return IOMGR_DRIVER_MOUNT_UNCERTAIN;
	if (block_device_read_sector(request->device, request->first_lba,
				     &boot) != BLOCK_DEVICE_OK)
		return IOMGR_DRIVER_MOUNT_IO_ERROR;
	status = fat_volume_parse_boot(&boot, request->sector_count,
				       &prepared.layout);
	if (status == FAT_VOLUME_CORRUPT || status == FAT_VOLUME_OVERFLOW)
		return IOMGR_DRIVER_MOUNT_CORRUPT;
	if (status == FAT_VOLUME_UNSUPPORTED || status == FAT_VOLUME_NO_MATCH)
		return IOMGR_DRIVER_MOUNT_UNSUPPORTED;
	prepared.device = request->device;
	prepared.first_lba = request->first_lba;
	prepared.sector_count = request->sector_count;
	slot_index = reserve_slot();
	if (slot_index == FAT_DRIVER_MAX_VOLUMES)
		return IOMGR_DRIVER_MOUNT_NO_RESOURCES;
	volumes[slot_index].snapshot = prepared;
	volumes[slot_index].next_free_hint = 2u;
	volumes[slot_index].free_clusters = 0u;
	volumes[slot_index].free_clusters_known = false;
	__asm__ volatile("" ::: "memory");
	volumes[slot_index].state = FAT_DRIVER_SLOT_LIVE;
	result->volume_context =
		make_context(slot_index, volumes[slot_index].generation);
	result->capabilities = IOMGR_VOLUME_CAP_READ |
			       IOMGR_VOLUME_CAP_CASE_PRESERVING;
	if ((request->flags & IOMGR_MOUNT_READ_ONLY) == 0u &&
	    block_device_get_geometry(request->device, &geometry) ==
		    BLOCK_DEVICE_OK &&
	    geometry.writable != 0u)
		result->capabilities |= IOMGR_VOLUME_CAP_WRITE;
	result->maximum_name_units = 12u;
	result->reserved = 0u;
	return IOMGR_DRIVER_MOUNT_OK;
}

static enum iomgr_driver_unmount_status
fat_unmount(kernel_object_handle_t context,
	    kernel_object_handle_t volume_context)
{
	struct fat_driver_slot *slot;

	if (context != FAT_DRIVER_IDENTITY ||
	    resolve_context(volume_context, &slot) != IOMGR_OK)
		return IOMGR_DRIVER_UNMOUNT_UNCERTAIN;
	slot->state = FAT_DRIVER_SLOT_FREE;
	return IOMGR_DRIVER_UNMOUNT_CLEAN;
}

enum iomgr_status fat_driver_register(void)
{
	const struct iomgr_driver_ops ops = {
		.abi_version = IOMGR_DRIVER_ABI_VERSION,
		.reserved = 0u,
		.identity = FAT_DRIVER_IDENTITY,
		.context = FAT_DRIVER_IDENTITY,
		.probe = fat_probe,
		.mount = fat_mount,
		.unmount = fat_unmount,
		.named = &fat_named_ops,
	};

	return iomgr_register_driver(&ops);
}

enum iomgr_status fat_driver_snapshot_from_context(
	kernel_object_handle_t volume_context,
	struct fat_driver_volume_snapshot *snapshot)
{
	struct fat_driver_slot *slot;
	enum iomgr_status status;

	if (snapshot == NULL)
		return IOMGR_INVALID_ARGUMENT;
	status = resolve_context(volume_context, &slot);
	if (status != IOMGR_OK)
		return status;
	*snapshot = slot->snapshot;
	return IOMGR_OK;
}

enum iomgr_status fat_driver_get_allocation_hint(
	kernel_object_handle_t volume_context, uint32_t *cluster)
{
	struct fat_driver_slot *slot;
	enum iomgr_status status;

	if (cluster == NULL)
		return IOMGR_INVALID_ARGUMENT;
	status = resolve_context(volume_context, &slot);
	if (status != IOMGR_OK)
		return status;
	*cluster = slot->next_free_hint;
	return IOMGR_OK;
}

enum iomgr_status fat_driver_set_allocation_hint(
	kernel_object_handle_t volume_context, uint32_t cluster)
{
	struct fat_driver_slot *slot;
	enum iomgr_status status = resolve_context(volume_context, &slot);

	if (status != IOMGR_OK)
		return status;
	if (cluster < 2u || cluster >= slot->snapshot.layout.cluster_limit)
		return IOMGR_INVALID_ARGUMENT;
	slot->next_free_hint = cluster;
	return IOMGR_OK;
}

enum iomgr_status fat_driver_get_free_clusters(
	kernel_object_handle_t volume_context, bool *known,
	uint32_t *free_clusters)
{
	struct fat_driver_slot *slot;
	enum iomgr_status status;

	if (known == NULL || free_clusters == NULL)
		return IOMGR_INVALID_ARGUMENT;
	status = resolve_context(volume_context, &slot);
	if (status != IOMGR_OK)
		return status;
	*known = slot->free_clusters_known;
	*free_clusters = slot->free_clusters;
	return IOMGR_OK;
}

enum iomgr_status fat_driver_publish_free_clusters(
	kernel_object_handle_t volume_context, uint32_t free_clusters)
{
	struct fat_driver_slot *slot;
	enum iomgr_status status = resolve_context(volume_context, &slot);

	if (status != IOMGR_OK)
		return status;
	if (free_clusters > slot->snapshot.layout.data_clusters)
		return IOMGR_INVALID_ARGUMENT;
	slot->free_clusters = free_clusters;
	slot->free_clusters_known = true;
	return IOMGR_OK;
}

enum iomgr_status fat_driver_adjust_free_clusters(
	kernel_object_handle_t volume_context, int32_t delta)
{
	struct fat_driver_slot *slot;
	int64_t adjusted;
	enum iomgr_status status = resolve_context(volume_context, &slot);

	if (status != IOMGR_OK)
		return status;
	if (!slot->free_clusters_known)
		return IOMGR_OK;
	adjusted = (int64_t)slot->free_clusters + delta;
	if (adjusted < 0 || adjusted > slot->snapshot.layout.data_clusters) {
		slot->free_clusters_known = false;
		return IOMGR_CORRUPT;
	}
	slot->free_clusters = (uint32_t)adjusted;
	return IOMGR_OK;
}

enum iomgr_status
fat_driver_get_volume(iomgr_volume_handle_t volume,
		      struct fat_driver_volume_snapshot *snapshot)
{
	struct fat_driver_slot *slot;
	kernel_object_handle_t volume_context;
	enum iomgr_status status;

	if (snapshot == NULL)
		return IOMGR_INVALID_ARGUMENT;
	status = iomgr_get_driver_volume_context(volume, FAT_DRIVER_IDENTITY,
						 &volume_context);
	if (status != IOMGR_OK)
		return status;
	status = resolve_context(volume_context, &slot);
	if (status != IOMGR_OK)
		return status;
	*snapshot = slot->snapshot;
	return IOMGR_OK;
}

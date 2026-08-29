// SPDX-License-Identifier: GPL-2.0-only
/*
 * I/O Manager volume owner
 *
 * DOS contract:   none; DOS drive/SFT adapters map this private status domain
 * Safety changes: fixed registries, generation handles, acquire-before-publish,
 *                 and quarantine after uncertain driver teardown
 */
#include "iomgr_driver.h"

#include "overflow.h"

#define IOMGR_MAX_DRIVERS 8u
#define IOMGR_MAX_VOLUMES 32u
#define IOMGR_MAX_FILES 64u
#define IOMGR_MAX_SEARCHES 32u
#define IOMGR_VOLUME_GENERATION_MAX 0xffffffffu
#define IOMGR_CAPABILITY_MASK                                                \
	(IOMGR_VOLUME_CAP_READ | IOMGR_VOLUME_CAP_WRITE |                    \
	 IOMGR_VOLUME_CAP_LONG_NAMES | IOMGR_VOLUME_CAP_CASE_PRESERVING |    \
	 IOMGR_VOLUME_CAP_CASE_SENSITIVE)

enum iomgr_volume_state {
	IOMGR_VOLUME_FREE = 0,
	IOMGR_VOLUME_RESERVED,
	IOMGR_VOLUME_LIVE,
	IOMGR_VOLUME_QUARANTINED,
	IOMGR_VOLUME_RETIRED
};

struct iomgr_driver_slot {
	bool occupied;
	struct iomgr_driver_ops ops;
};

struct iomgr_volume_slot {
	enum iomgr_volume_state state;
	uint32_t generation;
	uint8_t driver_slot;
	uint8_t reserved[3];
	kernel_object_handle_t driver_volume_context;
	struct iomgr_volume_info info;
};

enum iomgr_object_state {
	IOMGR_OBJECT_FREE = 0,
	IOMGR_OBJECT_LIVE,
	IOMGR_OBJECT_QUARANTINED,
	IOMGR_OBJECT_RETIRED
};

struct iomgr_named_object_slot {
	enum iomgr_object_state state;
	uint32_t generation;
	iomgr_volume_handle_t volume;
	kernel_object_handle_t driver_context;
};

static struct {
	bool initialized;
	struct iomgr_driver_slot drivers[IOMGR_MAX_DRIVERS];
	struct iomgr_volume_slot volumes[IOMGR_MAX_VOLUMES];
	struct iomgr_named_object_slot files[IOMGR_MAX_FILES];
	struct iomgr_named_object_slot searches[IOMGR_MAX_SEARCHES];
} manager;

static bool bytes_are_zero(const uint8_t *bytes, size_t count)
{
	size_t index;

	for (index = 0u; index < count; ++index) {
		if (bytes[index] != 0u)
			return false;
	}
	return true;
}

enum iomgr_status iomgr_initialize(void)
{
	size_t index;

	if (manager.initialized)
		return IOMGR_ALREADY_INITIALIZED;
	for (index = 0u; index < IOMGR_MAX_DRIVERS; ++index)
		manager.drivers[index].occupied = false;
	for (index = 0u; index < IOMGR_MAX_VOLUMES; ++index) {
		manager.volumes[index].state = IOMGR_VOLUME_FREE;
		manager.volumes[index].generation = 0u;
	}
	for (index = 0u; index < IOMGR_MAX_FILES; ++index) {
		manager.files[index].state = IOMGR_OBJECT_FREE;
		manager.files[index].generation = 0u;
	}
	for (index = 0u; index < IOMGR_MAX_SEARCHES; ++index) {
		manager.searches[index].state = IOMGR_OBJECT_FREE;
		manager.searches[index].generation = 0u;
	}
	manager.initialized = true;
	return IOMGR_OK;
}

enum iomgr_status iomgr_register_driver(const struct iomgr_driver_ops *ops)
{
	size_t index;
	size_t free_slot = IOMGR_MAX_DRIVERS;

	if (!manager.initialized)
		return IOMGR_NOT_INITIALIZED;
	if (ops == NULL || ops->abi_version != IOMGR_DRIVER_ABI_VERSION ||
	    ops->reserved != 0u || ops->identity == 0u ||
	    ops->identity == KERNEL_OBJECT_HANDLE_INVALID ||
	    ops->context == KERNEL_OBJECT_HANDLE_INVALID || ops->probe == NULL ||
	    ops->mount == NULL || ops->unmount == NULL)
		return IOMGR_INVALID_ARGUMENT;
	for (index = 0u; index < IOMGR_MAX_DRIVERS; ++index) {
		if (manager.drivers[index].occupied) {
			if (manager.drivers[index].ops.identity == ops->identity)
				return IOMGR_DUPLICATE_DRIVER;
		} else if (free_slot == IOMGR_MAX_DRIVERS) {
			free_slot = index;
		}
	}
	if (free_slot == IOMGR_MAX_DRIVERS)
		return IOMGR_NO_SLOT;
	manager.drivers[free_slot].ops = *ops;
	manager.drivers[free_slot].occupied = true;
	return IOMGR_OK;
}

static enum iomgr_status validate_mount_request(
	const struct iomgr_mount_request *request,
	struct iomgr_mount_request *prepared)
{
	struct block_device_geometry geometry;
	block_lba_t end;

	if (request == NULL || prepared == NULL ||
	    request->device == BLOCK_DEVICE_HANDLE_INVALID ||
	    request->sector_count == 0u || request->reserved != 0u ||
	    (request->flags & ~IOMGR_MOUNT_READ_ONLY) != 0u)
		return IOMGR_INVALID_ARGUMENT;
	if (check_add_overflow(request->first_lba, request->sector_count, &end))
		return IOMGR_INVALID_ARGUMENT;
	if (block_device_get_geometry(request->device, &geometry) !=
		BLOCK_DEVICE_OK)
		return IOMGR_IO_ERROR;
	if (geometry.logical_sector_bytes != BLOCK_DEVICE_SECTOR_BYTES)
		return IOMGR_UNSUPPORTED;
	if (request->first_lba >= geometry.sector_count ||
	    request->sector_count > geometry.sector_count - request->first_lba)
		return IOMGR_INVALID_ARGUMENT;
	*prepared = *request;
	if (geometry.writable == 0u)
		prepared->flags |= IOMGR_MOUNT_READ_ONLY;
	return IOMGR_OK;
}

static size_t reserve_volume_slot(void)
{
	size_t index;

	for (index = 0u; index < IOMGR_MAX_VOLUMES; ++index) {
		struct iomgr_volume_slot *slot = &manager.volumes[index];

		if (slot->state != IOMGR_VOLUME_FREE)
			continue;
		if (slot->generation == IOMGR_VOLUME_GENERATION_MAX) {
			slot->state = IOMGR_VOLUME_RETIRED;
			continue;
		}
		++slot->generation;
		slot->state = IOMGR_VOLUME_RESERVED;
		return index;
	}
	return IOMGR_MAX_VOLUMES;
}

static iomgr_volume_handle_t make_volume_handle(size_t index,
					 uint32_t generation)
{
	return ((uint64_t)generation << 32) | (uint64_t)(index + 1u);
}

static enum iomgr_status map_mount_status(
	enum iomgr_driver_mount_status status)
{
	if (status == IOMGR_DRIVER_MOUNT_CORRUPT)
		return IOMGR_CORRUPT;
	if (status == IOMGR_DRIVER_MOUNT_UNSUPPORTED)
		return IOMGR_UNSUPPORTED;
	if (status == IOMGR_DRIVER_MOUNT_NO_RESOURCES)
		return IOMGR_NO_SLOT;
	if (status == IOMGR_DRIVER_MOUNT_IO_ERROR)
		return IOMGR_IO_ERROR;
	return IOMGR_UNCERTAIN;
}

enum iomgr_status
iomgr_mount(const struct iomgr_mount_request *request,
	    iomgr_volume_handle_t *volume)
{
	struct iomgr_mount_request prepared;
	struct iomgr_driver_mount_result result;
	struct iomgr_volume_slot *volume_slot;
	enum iomgr_driver_mount_status mount_status;
	enum iomgr_probe_result probe;
	enum iomgr_status status;
	size_t slot_index;
	size_t driver_index;

	if (!manager.initialized)
		return IOMGR_NOT_INITIALIZED;
	if (volume == NULL)
		return IOMGR_INVALID_ARGUMENT;
	status = validate_mount_request(request, &prepared);
	if (status != IOMGR_OK)
		return status;
	slot_index = reserve_volume_slot();
	if (slot_index == IOMGR_MAX_VOLUMES)
		return IOMGR_NO_SLOT;
	volume_slot = &manager.volumes[slot_index];

	for (driver_index = 0u; driver_index < IOMGR_MAX_DRIVERS;
	     ++driver_index) {
		const struct iomgr_driver_ops *ops;

		if (!manager.drivers[driver_index].occupied)
			continue;
		ops = &manager.drivers[driver_index].ops;
		probe = ops->probe(ops->context, &prepared);
		if (probe == IOMGR_PROBE_NO_MATCH)
			continue;
		if (probe == IOMGR_PROBE_CORRUPT) {
			volume_slot->state = IOMGR_VOLUME_FREE;
			return IOMGR_CORRUPT;
		}
		if (probe == IOMGR_PROBE_UNSUPPORTED) {
			volume_slot->state = IOMGR_VOLUME_FREE;
			return IOMGR_UNSUPPORTED;
		}
		if (probe == IOMGR_PROBE_IO_ERROR) {
			volume_slot->state = IOMGR_VOLUME_FREE;
			return IOMGR_IO_ERROR;
		}
		if (probe != IOMGR_PROBE_MATCH) {
			volume_slot->state = IOMGR_VOLUME_QUARANTINED;
			return IOMGR_UNCERTAIN;
		}
		result = (struct iomgr_driver_mount_result){ 0 };
		mount_status = ops->mount(ops->context, &prepared, &result);
		if (mount_status != IOMGR_DRIVER_MOUNT_OK) {
			volume_slot->state =
				mount_status == IOMGR_DRIVER_MOUNT_UNCERTAIN
					? IOMGR_VOLUME_QUARANTINED
					: IOMGR_VOLUME_FREE;
			return map_mount_status(mount_status);
		}
		if (result.volume_context == KERNEL_OBJECT_HANDLE_INVALID ||
		    result.reserved != 0u || result.maximum_name_units == 0u ||
		    (result.capabilities & ~IOMGR_CAPABILITY_MASK) != 0u ||
		    (result.capabilities & IOMGR_VOLUME_CAP_READ) == 0u ||
		    ((prepared.flags & IOMGR_MOUNT_READ_ONLY) != 0u &&
		     (result.capabilities & IOMGR_VOLUME_CAP_WRITE) != 0u)) {
			volume_slot->state = IOMGR_VOLUME_QUARANTINED;
			return IOMGR_UNCERTAIN;
		}
		volume_slot->driver_slot = (uint8_t)driver_index;
		volume_slot->driver_volume_context = result.volume_context;
		volume_slot->info = (struct iomgr_volume_info){
			.driver_identity = ops->identity,
			.device = prepared.device,
			.first_lba = prepared.first_lba,
			.sector_count = prepared.sector_count,
			.capabilities = result.capabilities,
			.maximum_name_units = result.maximum_name_units,
			.reserved = 0u,
		};
		__asm__ volatile("" ::: "memory");
		volume_slot->state = IOMGR_VOLUME_LIVE;
		*volume = make_volume_handle(slot_index,
					    volume_slot->generation);
		return IOMGR_OK;
	}
	volume_slot->state = IOMGR_VOLUME_FREE;
	return IOMGR_NO_DRIVER;
}

static enum iomgr_status resolve_volume(iomgr_volume_handle_t handle,
					struct iomgr_volume_slot **volume)
{
	uint32_t encoded_slot;
	uint32_t generation;
	struct iomgr_volume_slot *slot;

	if (handle == IOMGR_VOLUME_HANDLE_INVALID || handle == 0u)
		return IOMGR_STALE_HANDLE;
	encoded_slot = (uint32_t)handle;
	generation = (uint32_t)(handle >> 32);
	if (encoded_slot == 0u || encoded_slot > IOMGR_MAX_VOLUMES ||
	    generation == 0u)
		return IOMGR_STALE_HANDLE;
	slot = &manager.volumes[encoded_slot - 1u];
	if (slot->generation != generation)
		return IOMGR_STALE_HANDLE;
	if (slot->state == IOMGR_VOLUME_QUARANTINED)
		return IOMGR_POISONED;
	if (slot->state != IOMGR_VOLUME_LIVE)
		return IOMGR_STALE_HANDLE;
	*volume = slot;
	return IOMGR_OK;
}

static bool volume_has_named_objects(iomgr_volume_handle_t volume)
{
	size_t index;

	for (index = 0u; index < IOMGR_MAX_FILES; ++index) {
		if (manager.files[index].state == IOMGR_OBJECT_LIVE &&
		    manager.files[index].volume == volume)
			return true;
	}
	for (index = 0u; index < IOMGR_MAX_SEARCHES; ++index) {
		if (manager.searches[index].state == IOMGR_OBJECT_LIVE &&
		    manager.searches[index].volume == volume)
			return true;
	}
	return false;
}

enum iomgr_status iomgr_unmount(iomgr_volume_handle_t volume)
{
	struct iomgr_volume_slot *slot;
	const struct iomgr_driver_ops *ops;
	enum iomgr_driver_unmount_status driver_status;
	enum iomgr_status status;

	if (!manager.initialized)
		return IOMGR_NOT_INITIALIZED;
	status = resolve_volume(volume, &slot);
	if (status != IOMGR_OK)
		return status;
	if (volume_has_named_objects(volume))
		return IOMGR_BUSY;
	ops = &manager.drivers[slot->driver_slot].ops;
	driver_status = ops->unmount(ops->context,
				     slot->driver_volume_context);
	if (driver_status == IOMGR_DRIVER_UNMOUNT_BUSY)
		return IOMGR_BUSY;
	if (driver_status != IOMGR_DRIVER_UNMOUNT_CLEAN) {
		slot->state = IOMGR_VOLUME_QUARANTINED;
		return IOMGR_UNCERTAIN;
	}
	slot->state = IOMGR_VOLUME_FREE;
	return IOMGR_OK;
}

enum iomgr_status
iomgr_get_volume_info(iomgr_volume_handle_t volume,
		      struct iomgr_volume_info *info)
{
	struct iomgr_volume_slot *slot;
	enum iomgr_status status;

	if (!manager.initialized)
		return IOMGR_NOT_INITIALIZED;
	if (info == NULL)
		return IOMGR_INVALID_ARGUMENT;
	status = resolve_volume(volume, &slot);
	if (status != IOMGR_OK)
		return status;
	if (!bytes_are_zero((const uint8_t *)&slot->info.reserved,
			    sizeof(slot->info.reserved)))
		return IOMGR_POISONED;
	*info = slot->info;
	return IOMGR_OK;
}

static bool path_is_valid(const struct iomgr_path *path)
{
	size_t index = 0u;

	if (path == NULL || path->bytes == NULL || path->length == 0u ||
	    path->length > IOMGR_PATH_MAX_BYTES)
		return false;
	while (index < path->length) {
		uint8_t first = path->bytes[index];
		uint32_t value;
		size_t count;
		size_t continuation;

		if (first == 0u)
			return false;
		if (first < 0x80u) {
			++index;
			continue;
		}
		if (first >= 0xc2u && first <= 0xdfu) {
			value = first & 0x1fu;
			count = 2u;
		} else if (first >= 0xe0u && first <= 0xefu) {
			value = first & 0x0fu;
			count = 3u;
		} else if (first >= 0xf0u && first <= 0xf4u) {
			value = first & 0x07u;
			count = 4u;
		} else {
			return false;
		}
		if (count > path->length - index)
			return false;
		for (continuation = 1u; continuation < count; ++continuation) {
			uint8_t byte = path->bytes[index + continuation];

			if ((byte & 0xc0u) != 0x80u)
				return false;
			value = (value << 6) | (uint32_t)(byte & 0x3fu);
		}
		if ((count == 3u && value < 0x800u) ||
		    (count == 4u && value < 0x10000u) ||
		    (value >= 0xd800u && value <= 0xdfffu) ||
		    value > 0x10ffffu)
			return false;
		index += count;
	}
	return true;
}

static bool timestamp_is_valid(const struct iomgr_timestamp *timestamp)
{
	return timestamp->month <= 12u && timestamp->day <= 31u &&
	       timestamp->hour <= 23u && timestamp->minute <= 59u &&
	       timestamp->second <= 60u && timestamp->centiseconds <= 199u;
}

static bool timestamp_update_is_valid(const struct iomgr_timestamp *timestamp)
{
	static const uint8_t month_days[12] = {
		31u, 28u, 31u, 30u, 31u, 30u,
		31u, 31u, 30u, 31u, 30u, 31u,
	};
	uint8_t limit;
	bool leap;

	if (timestamp->month == 0u || timestamp->month > 12u ||
	    timestamp->day == 0u || timestamp->hour > 23u ||
	    timestamp->minute > 59u || timestamp->second > 59u ||
	    timestamp->centiseconds > 99u)
		return false;
	limit = month_days[timestamp->month - 1u];
	leap = (timestamp->year % 4u) == 0u &&
	       ((timestamp->year % 100u) != 0u ||
		(timestamp->year % 400u) == 0u);
	if (timestamp->month == 2u && leap)
		++limit;
	return timestamp->day <= limit;
}

static bool node_info_is_valid(const struct iomgr_node_info *info)
{
	uint32_t mask = IOMGR_NODE_READ_ONLY | IOMGR_NODE_HIDDEN |
			IOMGR_NODE_SYSTEM | IOMGR_NODE_VOLUME_LABEL |
			IOMGR_NODE_DIRECTORY | IOMGR_NODE_ARCHIVE;

	return (info->attributes & ~mask) == 0u &&
	       timestamp_is_valid(&info->modified) &&
	       (((info->attributes & IOMGR_NODE_DIRECTORY) == 0u) ||
		info->size == 0u);
}

static bool named_status_is_valid(enum iomgr_status status)
{
	switch (status) {
	case IOMGR_OK:
	case IOMGR_INVALID_ARGUMENT:
	case IOMGR_NO_SLOT:
	case IOMGR_STALE_HANDLE:
	case IOMGR_READ_ONLY:
	case IOMGR_BUSY:
	case IOMGR_CORRUPT:
	case IOMGR_UNSUPPORTED:
	case IOMGR_IO_ERROR:
	case IOMGR_UNCERTAIN:
	case IOMGR_POISONED:
	case IOMGR_NOT_FOUND:
	case IOMGR_NOT_DIRECTORY:
	case IOMGR_IS_DIRECTORY:
	case IOMGR_INVALID_NAME:
	case IOMGR_ALREADY_EXISTS:
	case IOMGR_NO_SPACE:
	case IOMGR_END_OF_SEARCH:
		return true;
	case IOMGR_NOT_INITIALIZED:
	case IOMGR_ALREADY_INITIALIZED:
	case IOMGR_DUPLICATE_DRIVER:
	case IOMGR_NO_DRIVER:
	default:
		return false;
	}
}

static enum iomgr_status normalize_named_status(enum iomgr_status status)
{
	return named_status_is_valid(status) ? status : IOMGR_UNCERTAIN;
}

static size_t reserve_named_object(struct iomgr_named_object_slot *objects,
				   size_t count)
{
	size_t index;

	for (index = 0u; index < count; ++index) {
		if (objects[index].state != IOMGR_OBJECT_FREE)
			continue;
		if (objects[index].generation == 0xffffffffu) {
			objects[index].state = IOMGR_OBJECT_RETIRED;
			continue;
		}
		++objects[index].generation;
		objects[index].state = IOMGR_OBJECT_LIVE;
		return index;
	}
	return count;
}

static kernel_object_handle_t make_named_handle(size_t index,
					 uint32_t generation)
{
	return ((uint64_t)generation << 32) | (uint64_t)(index + 1u);
}

static enum iomgr_status resolve_named_object(
	kernel_object_handle_t handle, struct iomgr_named_object_slot *objects,
	size_t count, struct iomgr_named_object_slot **object)
{
	uint32_t encoded_slot = (uint32_t)handle;
	uint32_t generation = (uint32_t)(handle >> 32);
	struct iomgr_named_object_slot *slot;

	if (handle == 0u || handle == KERNEL_OBJECT_HANDLE_INVALID ||
	    encoded_slot == 0u || encoded_slot > count || generation == 0u)
		return IOMGR_STALE_HANDLE;
	slot = &objects[encoded_slot - 1u];
	if (slot->generation != generation)
		return IOMGR_STALE_HANDLE;
	if (slot->state == IOMGR_OBJECT_QUARANTINED)
		return IOMGR_POISONED;
	if (slot->state != IOMGR_OBJECT_LIVE)
		return IOMGR_STALE_HANDLE;
	*object = slot;
	return IOMGR_OK;
}

static const struct iomgr_driver_named_ops *volume_named_ops(
	const struct iomgr_volume_slot *volume)
{
	return manager.drivers[volume->driver_slot].ops.named;
}

static void quarantine_after_named_failure(iomgr_volume_handle_t handle,
					   struct iomgr_volume_slot *volume,
					   enum iomgr_status status)
{
	(void)handle;
	if (status == IOMGR_UNCERTAIN || status == IOMGR_POISONED)
		volume->state = IOMGR_VOLUME_QUARANTINED;
}

enum iomgr_status
iomgr_stat(iomgr_volume_handle_t handle, const struct iomgr_path *path,
	   struct iomgr_node_info *info)
{
	struct iomgr_volume_slot *volume;
	struct iomgr_node_info prepared;
	const struct iomgr_driver_named_ops *ops;
	enum iomgr_status status;

	if (info == NULL || !path_is_valid(path))
		return IOMGR_INVALID_ARGUMENT;
	status = resolve_volume(handle, &volume);
	if (status != IOMGR_OK)
		return status;
	ops = volume_named_ops(volume);
	if (ops == NULL || ops->stat == NULL)
		return IOMGR_UNSUPPORTED;
	status = normalize_named_status(
		ops->stat(volume->driver_volume_context, path, &prepared));
	if (status != IOMGR_OK) {
		quarantine_after_named_failure(handle, volume, status);
		return status;
	}
	if (!node_info_is_valid(&prepared)) {
		volume->state = IOMGR_VOLUME_QUARANTINED;
		return IOMGR_UNCERTAIN;
	}
	*info = prepared;
	return IOMGR_OK;
}

enum iomgr_status
iomgr_open_file(iomgr_volume_handle_t handle, const struct iomgr_path *path,
		struct iomgr_node_info *info, iomgr_file_handle_t *file)
{
	struct iomgr_volume_slot *volume;
	struct iomgr_named_object_slot *slot;
	struct iomgr_node_info prepared;
	kernel_object_handle_t driver_context;
	const struct iomgr_driver_named_ops *ops;
	enum iomgr_status status;
	size_t index;

	if (info == NULL || file == NULL || !path_is_valid(path))
		return IOMGR_INVALID_ARGUMENT;
	status = resolve_volume(handle, &volume);
	if (status != IOMGR_OK)
		return status;
	ops = volume_named_ops(volume);
	if (ops == NULL || ops->open_file == NULL || ops->close_file == NULL ||
	    ops->read_file == NULL)
		return IOMGR_UNSUPPORTED;
	index = reserve_named_object(manager.files, IOMGR_MAX_FILES);
	if (index == IOMGR_MAX_FILES)
		return IOMGR_NO_SLOT;
	slot = &manager.files[index];
	status = normalize_named_status(ops->open_file(
		volume->driver_volume_context, path, &driver_context, &prepared));
	if (status != IOMGR_OK) {
		slot->state = IOMGR_OBJECT_FREE;
		quarantine_after_named_failure(handle, volume, status);
		return status;
	}
	if (driver_context == 0u ||
	    driver_context == KERNEL_OBJECT_HANDLE_INVALID ||
	    !node_info_is_valid(&prepared) ||
	    (prepared.attributes & IOMGR_NODE_DIRECTORY) != 0u) {
		slot->state = IOMGR_OBJECT_QUARANTINED;
		volume->state = IOMGR_VOLUME_QUARANTINED;
		return IOMGR_UNCERTAIN;
	}
	slot->volume = handle;
	slot->driver_context = driver_context;
	*info = prepared;
	*file = make_named_handle(index, slot->generation);
	return IOMGR_OK;
}

enum iomgr_status
iomgr_create_file(iomgr_volume_handle_t handle, const struct iomgr_path *path,
		  uint32_t attributes, struct iomgr_node_info *info,
		  iomgr_file_handle_t *file)
{
	struct iomgr_volume_slot *volume;
	struct iomgr_named_object_slot *slot;
	struct iomgr_node_info prepared;
	kernel_object_handle_t driver_context;
	const struct iomgr_driver_named_ops *ops;
	enum iomgr_status status;
	size_t index;

	if (info == NULL || file == NULL || !path_is_valid(path) ||
	    (attributes & ~(IOMGR_NODE_READ_ONLY | IOMGR_NODE_HIDDEN |
			    IOMGR_NODE_SYSTEM | IOMGR_NODE_ARCHIVE)) != 0u)
		return IOMGR_INVALID_ARGUMENT;
	status = resolve_volume(handle, &volume);
	if (status != IOMGR_OK)
		return status;
	if ((volume->info.capabilities & IOMGR_VOLUME_CAP_WRITE) == 0u)
		return IOMGR_READ_ONLY;
	ops = volume_named_ops(volume);
	if (ops == NULL || ops->create_file == NULL || ops->close_file == NULL ||
	    ops->read_file == NULL || ops->write_file == NULL)
		return IOMGR_UNSUPPORTED;
	index = reserve_named_object(manager.files, IOMGR_MAX_FILES);
	if (index == IOMGR_MAX_FILES)
		return IOMGR_NO_SLOT;
	slot = &manager.files[index];
	status = normalize_named_status(ops->create_file(
		volume->driver_volume_context, path, attributes, handle,
		&driver_context, &prepared));
	if (status != IOMGR_OK) {
		slot->state = IOMGR_OBJECT_FREE;
		quarantine_after_named_failure(handle, volume, status);
		return status;
	}
	if (driver_context == 0u ||
	    driver_context == KERNEL_OBJECT_HANDLE_INVALID ||
	    !node_info_is_valid(&prepared) ||
	    (prepared.attributes & IOMGR_NODE_DIRECTORY) != 0u ||
	    prepared.size != 0u) {
		slot->state = IOMGR_OBJECT_QUARANTINED;
		volume->state = IOMGR_VOLUME_QUARANTINED;
		return IOMGR_UNCERTAIN;
	}
	slot->volume = handle;
	slot->driver_context = driver_context;
	*info = prepared;
	*file = make_named_handle(index, slot->generation);
	return IOMGR_OK;
}

enum iomgr_status
iomgr_read_file(iomgr_file_handle_t file, uint64_t offset,
		uint8_t *destination, size_t capacity, size_t count,
		size_t *bytes_read)
{
	struct iomgr_named_object_slot *slot;
	struct iomgr_volume_slot *volume;
	const struct iomgr_driver_named_ops *ops;
	enum iomgr_status status;
	size_t prepared = 0u;

	if (destination == NULL || bytes_read == NULL || count > capacity)
		return IOMGR_INVALID_ARGUMENT;
	status = resolve_named_object(file, manager.files, IOMGR_MAX_FILES,
				      &slot);
	if (status != IOMGR_OK)
		return status;
	status = resolve_volume(slot->volume, &volume);
	if (status != IOMGR_OK)
		return status;
	ops = volume_named_ops(volume);
	if (ops == NULL || ops->read_file == NULL)
		return IOMGR_UNSUPPORTED;
	status = normalize_named_status(ops->read_file(
		volume->driver_volume_context, slot->driver_context, offset,
		destination, capacity, count, &prepared));
	if (status != IOMGR_OK) {
		quarantine_after_named_failure(slot->volume, volume, status);
		return status;
	}
	if (prepared > count) {
		slot->state = IOMGR_OBJECT_QUARANTINED;
		volume->state = IOMGR_VOLUME_QUARANTINED;
		return IOMGR_UNCERTAIN;
	}
	*bytes_read = prepared;
	return IOMGR_OK;
}

enum iomgr_status
iomgr_write_file(iomgr_file_handle_t file, uint64_t offset,
		 const uint8_t *source, size_t source_capacity, size_t count,
		 size_t *bytes_written)
{
	struct iomgr_named_object_slot *slot;
	struct iomgr_volume_slot *volume;
	const struct iomgr_driver_named_ops *ops;
	enum iomgr_status status;
	size_t prepared = 0u;

	if (source == NULL || bytes_written == NULL || count > source_capacity)
		return IOMGR_INVALID_ARGUMENT;
	status = resolve_named_object(file, manager.files, IOMGR_MAX_FILES,
				      &slot);
	if (status != IOMGR_OK)
		return status;
	status = resolve_volume(slot->volume, &volume);
	if (status != IOMGR_OK)
		return status;
	if ((volume->info.capabilities & IOMGR_VOLUME_CAP_WRITE) == 0u)
		return IOMGR_READ_ONLY;
	ops = volume_named_ops(volume);
	if (ops == NULL || ops->write_file == NULL)
		return IOMGR_UNSUPPORTED;
	status = normalize_named_status(ops->write_file(
		volume->driver_volume_context, slot->driver_context, slot->volume,
		offset, source, source_capacity, count, &prepared));
	if (status != IOMGR_OK) {
		quarantine_after_named_failure(slot->volume, volume, status);
		return status;
	}
	if (prepared > count) {
		slot->state = IOMGR_OBJECT_QUARANTINED;
		volume->state = IOMGR_VOLUME_QUARANTINED;
		return IOMGR_UNCERTAIN;
	}
	*bytes_written = prepared;
	return IOMGR_OK;
}

enum iomgr_status iomgr_get_file_info(iomgr_file_handle_t file,
				      struct iomgr_node_info *info)
{
	struct iomgr_named_object_slot *slot;
	struct iomgr_volume_slot *volume;
	struct iomgr_node_info prepared;
	const struct iomgr_driver_named_ops *ops;
	enum iomgr_status status;

	if (info == NULL)
		return IOMGR_INVALID_ARGUMENT;
	status = resolve_named_object(file, manager.files, IOMGR_MAX_FILES,
				      &slot);
	if (status != IOMGR_OK)
		return status;
	status = resolve_volume(slot->volume, &volume);
	if (status != IOMGR_OK)
		return status;
	ops = volume_named_ops(volume);
	if (ops == NULL || ops->get_file_info == NULL)
		return IOMGR_UNSUPPORTED;
	status = normalize_named_status(ops->get_file_info(
		volume->driver_volume_context, slot->driver_context, &prepared));
	if (status != IOMGR_OK) {
		quarantine_after_named_failure(slot->volume, volume, status);
		return status;
	}
	if (!node_info_is_valid(&prepared) ||
	    (prepared.attributes & IOMGR_NODE_DIRECTORY) != 0u) {
		slot->state = IOMGR_OBJECT_QUARANTINED;
		volume->state = IOMGR_VOLUME_QUARANTINED;
		return IOMGR_UNCERTAIN;
	}
	*info = prepared;
	return IOMGR_OK;
}

enum iomgr_status iomgr_set_file_info(
	iomgr_file_handle_t file, const struct iomgr_file_update *update)
{
	struct iomgr_named_object_slot *slot;
	struct iomgr_volume_slot *volume;
	const struct iomgr_driver_named_ops *ops;
	enum iomgr_status status;

	if (update == NULL || update->valid != IOMGR_FILE_UPDATE_MODIFIED ||
	    !timestamp_update_is_valid(&update->modified))
		return IOMGR_INVALID_ARGUMENT;
	status = resolve_named_object(file, manager.files, IOMGR_MAX_FILES,
				      &slot);
	if (status != IOMGR_OK)
		return status;
	status = resolve_volume(slot->volume, &volume);
	if (status != IOMGR_OK)
		return status;
	if ((volume->info.capabilities & IOMGR_VOLUME_CAP_WRITE) == 0u)
		return IOMGR_READ_ONLY;
	ops = volume_named_ops(volume);
	if (ops == NULL || ops->set_file_info == NULL)
		return IOMGR_UNSUPPORTED;
	status = normalize_named_status(ops->set_file_info(
		volume->driver_volume_context, slot->driver_context, slot->volume,
		update));
	if (status != IOMGR_OK)
		quarantine_after_named_failure(slot->volume, volume, status);
	return status;
}

enum iomgr_status iomgr_close_file(iomgr_file_handle_t file)
{
	struct iomgr_named_object_slot *slot;
	struct iomgr_volume_slot *volume;
	const struct iomgr_driver_named_ops *ops;
	enum iomgr_status status;

	status = resolve_named_object(file, manager.files, IOMGR_MAX_FILES,
				      &slot);
	if (status != IOMGR_OK)
		return status;
	status = resolve_volume(slot->volume, &volume);
	if (status != IOMGR_OK)
		return status;
	ops = volume_named_ops(volume);
	if (ops == NULL || ops->close_file == NULL)
		return IOMGR_UNSUPPORTED;
	status = normalize_named_status(ops->close_file(
		volume->driver_volume_context, slot->driver_context));
	if (status != IOMGR_OK) {
		slot->state = IOMGR_OBJECT_QUARANTINED;
		quarantine_after_named_failure(slot->volume, volume, status);
		return status;
	}
	slot->state = IOMGR_OBJECT_FREE;
	return IOMGR_OK;
}

enum iomgr_status
iomgr_open_search(iomgr_volume_handle_t handle,
		  const struct iomgr_path *pattern, uint32_t attributes,
		  iomgr_search_handle_t *search)
{
	struct iomgr_volume_slot *volume;
	struct iomgr_named_object_slot *slot;
	const struct iomgr_driver_named_ops *ops;
	kernel_object_handle_t driver_context;
	enum iomgr_status status;
	size_t index;

	if (search == NULL || !path_is_valid(pattern) ||
	    (attributes & ~(IOMGR_NODE_READ_ONLY | IOMGR_NODE_HIDDEN |
			    IOMGR_NODE_SYSTEM | IOMGR_NODE_VOLUME_LABEL |
			    IOMGR_NODE_DIRECTORY | IOMGR_NODE_ARCHIVE)) != 0u)
		return IOMGR_INVALID_ARGUMENT;
	status = resolve_volume(handle, &volume);
	if (status != IOMGR_OK)
		return status;
	ops = volume_named_ops(volume);
	if (ops == NULL || ops->open_search == NULL ||
	    ops->search_next == NULL || ops->close_search == NULL)
		return IOMGR_UNSUPPORTED;
	index = reserve_named_object(manager.searches, IOMGR_MAX_SEARCHES);
	if (index == IOMGR_MAX_SEARCHES)
		return IOMGR_NO_SLOT;
	slot = &manager.searches[index];
	status = normalize_named_status(ops->open_search(
		volume->driver_volume_context, pattern, attributes,
		&driver_context));
	if (status != IOMGR_OK) {
		slot->state = IOMGR_OBJECT_FREE;
		quarantine_after_named_failure(handle, volume, status);
		return status;
	}
	if (driver_context == 0u ||
	    driver_context == KERNEL_OBJECT_HANDLE_INVALID) {
		slot->state = IOMGR_OBJECT_QUARANTINED;
		volume->state = IOMGR_VOLUME_QUARANTINED;
		return IOMGR_UNCERTAIN;
	}
	slot->volume = handle;
	slot->driver_context = driver_context;
	*search = make_named_handle(index, slot->generation);
	return IOMGR_OK;
}

enum iomgr_status
iomgr_search_next(iomgr_search_handle_t search,
		  struct iomgr_directory_entry *entry)
{
	struct iomgr_named_object_slot *slot;
	struct iomgr_volume_slot *volume;
	struct iomgr_directory_entry prepared;
	const struct iomgr_driver_named_ops *ops;
	enum iomgr_status status;

	if (entry == NULL)
		return IOMGR_INVALID_ARGUMENT;
	status = resolve_named_object(search, manager.searches,
				      IOMGR_MAX_SEARCHES, &slot);
	if (status != IOMGR_OK)
		return status;
	status = resolve_volume(slot->volume, &volume);
	if (status != IOMGR_OK)
		return status;
	ops = volume_named_ops(volume);
	if (ops == NULL || ops->search_next == NULL)
		return IOMGR_UNSUPPORTED;
	status = normalize_named_status(ops->search_next(
		volume->driver_volume_context, slot->driver_context, &prepared));
	if (status != IOMGR_OK) {
		quarantine_after_named_failure(slot->volume, volume, status);
		return status;
	}
	if (prepared.name_length == 0u ||
	    prepared.name_length > IOMGR_NAME_MAX_BYTES ||
	    prepared.name[prepared.name_length] != 0u ||
	    !node_info_is_valid(&prepared.info)) {
		slot->state = IOMGR_OBJECT_QUARANTINED;
		volume->state = IOMGR_VOLUME_QUARANTINED;
		return IOMGR_UNCERTAIN;
	}
	*entry = prepared;
	return IOMGR_OK;
}

enum iomgr_status iomgr_close_search(iomgr_search_handle_t search)
{
	struct iomgr_named_object_slot *slot;
	struct iomgr_volume_slot *volume;
	const struct iomgr_driver_named_ops *ops;
	enum iomgr_status status;

	status = resolve_named_object(search, manager.searches,
				      IOMGR_MAX_SEARCHES, &slot);
	if (status != IOMGR_OK)
		return status;
	status = resolve_volume(slot->volume, &volume);
	if (status != IOMGR_OK)
		return status;
	ops = volume_named_ops(volume);
	if (ops == NULL || ops->close_search == NULL)
		return IOMGR_UNSUPPORTED;
	status = normalize_named_status(ops->close_search(
		volume->driver_volume_context, slot->driver_context));
	if (status != IOMGR_OK) {
		slot->state = IOMGR_OBJECT_QUARANTINED;
		quarantine_after_named_failure(slot->volume, volume, status);
		return status;
	}
	slot->state = IOMGR_OBJECT_FREE;
	return IOMGR_OK;
}

enum iomgr_status
iomgr_query_space(iomgr_volume_handle_t handle, bool count_free,
		  struct iomgr_space_info *info)
{
	struct iomgr_volume_slot *volume;
	struct iomgr_space_info prepared;
	const struct iomgr_driver_named_ops *ops;
	enum iomgr_status status;

	if (info == NULL)
		return IOMGR_INVALID_ARGUMENT;
	status = resolve_volume(handle, &volume);
	if (status != IOMGR_OK)
		return status;
	ops = volume_named_ops(volume);
	if (ops == NULL || ops->query_space == NULL)
		return IOMGR_UNSUPPORTED;
	status = normalize_named_status(ops->query_space(
		volume->driver_volume_context, count_free, &prepared));
	if (status != IOMGR_OK) {
		quarantine_after_named_failure(handle, volume, status);
		return status;
	}
	if (prepared.reserved != 0u || prepared.allocation_unit_bytes == 0u ||
	    prepared.free_bytes > prepared.total_bytes) {
		volume->state = IOMGR_VOLUME_QUARANTINED;
		return IOMGR_UNCERTAIN;
	}
	*info = prepared;
	return IOMGR_OK;
}

enum iomgr_status
iomgr_create_directory(iomgr_volume_handle_t handle,
		       const struct iomgr_path *path)
{
	struct iomgr_volume_slot *volume;
	const struct iomgr_driver_named_ops *ops;
	enum iomgr_status status;

	if (!path_is_valid(path))
		return IOMGR_INVALID_ARGUMENT;
	status = resolve_volume(handle, &volume);
	if (status != IOMGR_OK)
		return status;
	if ((volume->info.capabilities & IOMGR_VOLUME_CAP_WRITE) == 0u)
		return IOMGR_READ_ONLY;
	ops = volume_named_ops(volume);
	if (ops == NULL || ops->create_directory == NULL)
		return IOMGR_UNSUPPORTED;
	status = normalize_named_status(ops->create_directory(
		volume->driver_volume_context, path, handle));
	if (status != IOMGR_OK)
		quarantine_after_named_failure(handle, volume, status);
	return status;
}

enum iomgr_status iomgr_rename(iomgr_volume_handle_t handle,
			       const struct iomgr_path *old_path,
			       const struct iomgr_path *new_path)
{
	struct iomgr_volume_slot *volume;
	const struct iomgr_driver_named_ops *ops;
	enum iomgr_status status;

	if (!path_is_valid(old_path) || !path_is_valid(new_path))
		return IOMGR_INVALID_ARGUMENT;
	status = resolve_volume(handle, &volume);
	if (status != IOMGR_OK)
		return status;
	if ((volume->info.capabilities & IOMGR_VOLUME_CAP_WRITE) == 0u)
		return IOMGR_READ_ONLY;
	ops = volume_named_ops(volume);
	if (ops == NULL || ops->rename == NULL)
		return IOMGR_UNSUPPORTED;
	status = normalize_named_status(ops->rename(
		volume->driver_volume_context, old_path, new_path, handle));
	if (status != IOMGR_OK)
		quarantine_after_named_failure(handle, volume, status);
	return status;
}

enum iomgr_status iomgr_quarantine_volume(iomgr_volume_handle_t volume)
{
	struct iomgr_volume_slot *slot;
	enum iomgr_status status;

	if (!manager.initialized)
		return IOMGR_NOT_INITIALIZED;
	status = resolve_volume(volume, &slot);
	if (status != IOMGR_OK)
		return status;
	slot->state = IOMGR_VOLUME_QUARANTINED;
	return IOMGR_OK;
}

enum iomgr_status iomgr_get_driver_volume_context(
	iomgr_volume_handle_t volume, uint64_t expected_driver_identity,
	kernel_object_handle_t *volume_context)
{
	struct iomgr_volume_slot *slot;
	enum iomgr_status status;

	if (!manager.initialized)
		return IOMGR_NOT_INITIALIZED;
	if (volume_context == NULL || expected_driver_identity == 0u ||
	    expected_driver_identity == KERNEL_OBJECT_HANDLE_INVALID)
		return IOMGR_INVALID_ARGUMENT;
	status = resolve_volume(volume, &slot);
	if (status != IOMGR_OK)
		return status;
	if (slot->info.driver_identity != expected_driver_identity)
		return IOMGR_NO_DRIVER;
	*volume_context = slot->driver_volume_context;
	return IOMGR_OK;
}

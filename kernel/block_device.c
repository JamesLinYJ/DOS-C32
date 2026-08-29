// SPDX-License-Identifier: GPL-2.0-only
/*
 * Architecture-independent, handle-based block-device registry.
 *
 * DOS contract: block devices ultimately surface through the MS-DOS device
 * chain and request packets; this lower layer only transports exact sectors.
 * Safety design: 64-bit generation handles, checked geometry and LBAs,
 * transactional reads, fixed-size transfer objects, no persistent data
 * pointers in filesystem state.
 */
#include "block_device.h"

#define BLOCK_DEVICE_MAXIMUM 8u
#define BLOCK_HANDLE_SLOT_MASK 0xffffffffu

/* Focused tests use a small terminal generation to exercise retirement. */
#ifdef BLOCK_DEVICE_TEST_GENERATION_MAX
#define BLOCK_DEVICE_GENERATION_LIMIT                                      \
	((uint32_t)BLOCK_DEVICE_TEST_GENERATION_MAX)
#else
#define BLOCK_DEVICE_GENERATION_LIMIT BLOCK_DEVICE_GENERATION_MAX
#endif

static_assert_expression(BLOCK_DEVICE_GENERATION_LIMIT > 0u,
			 "block-device generations require a nonzero value");
static_assert_expression(
    BLOCK_DEVICE_GENERATION_LIMIT <= BLOCK_DEVICE_GENERATION_MAX,
    "block-device test generation exceeds the persistent handle field");

struct registered_block_device {
	const struct block_device_ops *ops;
	kernel_object_handle_t context;
	struct block_device_geometry geometry;
	uint32_t generation;
	bool occupied;
};

static struct registered_block_device block_devices[BLOCK_DEVICE_MAXIMUM];

static bool geometry_reserved_is_zero(
    const struct block_device_geometry *geometry)
{
	return geometry->reserved[0] == 0u && geometry->reserved[1] == 0u &&
	       geometry->reserved[2] == 0u;
}

static bool adapter_status_is_valid(enum block_device_status status)
{
	switch (status) {
	case BLOCK_DEVICE_OK:
	case BLOCK_DEVICE_INVALID_ARGUMENT:
	case BLOCK_DEVICE_NOT_READY:
	case BLOCK_DEVICE_NO_MEDIA:
	case BLOCK_DEVICE_UNSUPPORTED:
	case BLOCK_DEVICE_OUT_OF_RANGE:
	case BLOCK_DEVICE_READ_ONLY:
	case BLOCK_DEVICE_TIMEOUT:
	case BLOCK_DEVICE_IO_ERROR:
		return true;
	case BLOCK_DEVICE_NO_SLOT:
	case BLOCK_DEVICE_STALE_HANDLE:
	case BLOCK_DEVICE_GENERATION_EXHAUSTED:
	default:
		return false;
	}
}

static enum block_device_status
normalize_adapter_status(enum block_device_status status)
{
	return adapter_status_is_valid(status) ? status : BLOCK_DEVICE_IO_ERROR;
}

static enum block_device_status find_reusable_slot(uint32_t *slot,
						    uint32_t *generation)
{
	bool every_generation_exhausted = true;
	uint32_t index;

	for (index = 0u; index < BLOCK_DEVICE_MAXIMUM; ++index) {
		if (block_devices[index].generation <
		    BLOCK_DEVICE_GENERATION_LIMIT)
			every_generation_exhausted = false;
		if (block_devices[index].occupied)
			continue;
		if (block_devices[index].generation >=
		    BLOCK_DEVICE_GENERATION_LIMIT)
			continue;
		*slot = index;
		*generation = block_devices[index].generation + 1u;
		return BLOCK_DEVICE_OK;
	}
	return every_generation_exhausted ? BLOCK_DEVICE_GENERATION_EXHAUSTED
				  : BLOCK_DEVICE_NO_SLOT;
}

static block_device_handle_t make_handle(uint32_t slot, uint32_t generation)
{
	return ((block_device_handle_t)generation << 32) |
	       (block_device_handle_t)(slot + 1u);
}

static enum block_device_status
resolve_handle(block_device_handle_t handle,
	       struct registered_block_device **device)
{
	uint32_t encoded_slot = (uint32_t)(handle & BLOCK_HANDLE_SLOT_MASK);
	uint32_t generation = (uint32_t)(handle >> 32);
	uint32_t slot;

	if (device == NULL || encoded_slot == 0u || generation == 0u ||
	    generation > BLOCK_DEVICE_GENERATION_LIMIT)
		return BLOCK_DEVICE_STALE_HANDLE;
	slot = encoded_slot - 1u;
	if (slot >= BLOCK_DEVICE_MAXIMUM || !block_devices[slot].occupied ||
	    block_devices[slot].generation != generation)
		return BLOCK_DEVICE_STALE_HANDLE;
	*device = &block_devices[slot];
	return BLOCK_DEVICE_OK;
}

enum block_device_status
block_device_register(const struct block_device_ops *ops,
		      kernel_object_handle_t context,
		      block_device_handle_t *handle)
{
	struct block_device_geometry geometry = {
	    .sector_count = 0u,
	    .logical_sector_bytes = 0u,
	    .writable = 0u,
	    .reserved = {0u},
	};
	enum block_device_status status;
	uint32_t slot;
	uint32_t generation;

	if (ops == NULL || ops->probe == NULL || ops->read_sector == NULL ||
	    handle == NULL)
		return BLOCK_DEVICE_INVALID_ARGUMENT;
	status = find_reusable_slot(&slot, &generation);
	if (status != BLOCK_DEVICE_OK)
		return status;
	status = normalize_adapter_status(ops->probe(context, &geometry));
	if (status != BLOCK_DEVICE_OK)
		return status;
	if (geometry.sector_count == 0u ||
	    geometry.logical_sector_bytes != BLOCK_DEVICE_SECTOR_BYTES ||
	    geometry.writable > 1u || !geometry_reserved_is_zero(&geometry))
		return BLOCK_DEVICE_UNSUPPORTED;
	if (geometry.writable == 1u && ops->write_sector == NULL)
		return BLOCK_DEVICE_UNSUPPORTED;

	block_devices[slot].ops = ops;
	block_devices[slot].context = context;
	block_devices[slot].geometry = geometry;
	block_devices[slot].generation = generation;
	block_devices[slot].occupied = true;
	*handle = make_handle(slot, generation);
	return BLOCK_DEVICE_OK;
}

enum block_device_status block_device_unregister(block_device_handle_t handle)
{
	struct registered_block_device *device;
	enum block_device_status status = resolve_handle(handle, &device);

	if (status != BLOCK_DEVICE_OK)
		return status;
	if (device->ops->flush != NULL) {
		status = normalize_adapter_status(
		    device->ops->flush(device->context));
		if (status != BLOCK_DEVICE_OK)
			return status;
	}
	device->ops = NULL;
	device->context = KERNEL_OBJECT_HANDLE_INVALID;
	device->geometry.sector_count = 0u;
	device->geometry.logical_sector_bytes = 0u;
	device->geometry.writable = 0u;
	device->geometry.reserved[0] = 0u;
	device->geometry.reserved[1] = 0u;
	device->geometry.reserved[2] = 0u;
	device->occupied = false;
	return BLOCK_DEVICE_OK;
}

enum block_device_status
block_device_get_geometry(block_device_handle_t handle,
			  struct block_device_geometry *geometry)
{
	struct registered_block_device *device;
	enum block_device_status status;

	if (geometry == NULL)
		return BLOCK_DEVICE_INVALID_ARGUMENT;
	status = resolve_handle(handle, &device);
	if (status != BLOCK_DEVICE_OK)
		return status;
	*geometry = device->geometry;
	return BLOCK_DEVICE_OK;
}

enum block_device_status
block_device_read_sector(block_device_handle_t handle, block_lba_t lba,
			 union block_device_sector *sector)
{
	union block_device_sector temporary;
	struct registered_block_device *device;
	enum block_device_status status;
	size_t index;

	if (sector == NULL)
		return BLOCK_DEVICE_INVALID_ARGUMENT;
	status = resolve_handle(handle, &device);
	if (status != BLOCK_DEVICE_OK)
		return status;
	if (lba >= device->geometry.sector_count)
		return BLOCK_DEVICE_OUT_OF_RANGE;
	status = normalize_adapter_status(
	    device->ops->read_sector(device->context, lba, &temporary));
	if (status != BLOCK_DEVICE_OK)
		return status;
	for (index = 0u; index < ARRAY_SIZE(temporary.words); ++index)
		sector->words[index] = temporary.words[index];
	return BLOCK_DEVICE_OK;
}

enum block_device_status
block_device_write_sector(block_device_handle_t handle, block_lba_t lba,
			  const union block_device_sector *sector)
{
	struct registered_block_device *device;
	enum block_device_status status;

	if (sector == NULL)
		return BLOCK_DEVICE_INVALID_ARGUMENT;
	status = resolve_handle(handle, &device);
	if (status != BLOCK_DEVICE_OK)
		return status;
	if (lba >= device->geometry.sector_count)
		return BLOCK_DEVICE_OUT_OF_RANGE;
	if (device->geometry.writable != 1u ||
	    device->ops->write_sector == NULL)
		return BLOCK_DEVICE_READ_ONLY;
	return normalize_adapter_status(
	    device->ops->write_sector(device->context, lba, sector));
}

enum block_device_status block_device_flush(block_device_handle_t handle)
{
	struct registered_block_device *device;
	enum block_device_status status = resolve_handle(handle, &device);

	if (status != BLOCK_DEVICE_OK)
		return status;
	if (device->ops->flush == NULL)
		return BLOCK_DEVICE_OK;
	return normalize_adapter_status(device->ops->flush(device->context));
}

const char *block_device_status_string(enum block_device_status status)
{
	switch (status) {
	case BLOCK_DEVICE_OK:
		return "ok";
	case BLOCK_DEVICE_INVALID_ARGUMENT:
		return "invalid argument";
	case BLOCK_DEVICE_NO_SLOT:
		return "block-device registry full";
	case BLOCK_DEVICE_STALE_HANDLE:
		return "invalid or stale block-device handle";
	case BLOCK_DEVICE_NOT_READY:
		return "device not ready";
	case BLOCK_DEVICE_NO_MEDIA:
		return "no media";
	case BLOCK_DEVICE_UNSUPPORTED:
		return "unsupported device geometry";
	case BLOCK_DEVICE_OUT_OF_RANGE:
		return "LBA outside device";
	case BLOCK_DEVICE_READ_ONLY:
		return "read-only device";
	case BLOCK_DEVICE_TIMEOUT:
		return "device timeout";
	case BLOCK_DEVICE_IO_ERROR:
		return "device I/O error";
	case BLOCK_DEVICE_GENERATION_EXHAUSTED:
		return "block-device handle generation exhausted";
	default:
		return "unknown block-device error";
	}
}

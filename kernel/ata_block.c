// SPDX-License-Identifier: GPL-2.0-only
/* Firmware-bound ATA adapter for the generic 512-byte block boundary. */
#include "ata_block.h"

struct ata_block_owner {
	kernel_object_handle_t identity;
	block_device_handle_t boot_handle;
	struct ata_device device;
	uint32_t poll_limit;
	ata_write_policy_t write_policy;
	uint8_t initialized;
	uint8_t device_constructed;
	uint8_t reserved;
};

static struct ata_block_owner owner;

static bool context_is_owner(kernel_object_handle_t context)
{
	return owner.initialized == 1u && context == owner.identity;
}

static bool info_reserved_is_zero(const struct ata_device_info *info)
{
	size_t index;

	for (index = 0u; index < ARRAY_SIZE(info->reserved); ++index) {
		if (info->reserved[index] != 0u)
			return false;
	}
	return true;
}

static enum block_device_status map_ata_status(ata_result_t result)
{
	switch (result) {
	case ATA_OK:
		return BLOCK_DEVICE_OK;
	case ATA_ERR_ARGUMENT:
		return BLOCK_DEVICE_INVALID_ARGUMENT;
	case ATA_ERR_NOT_INITIALIZED:
		return BLOCK_DEVICE_NOT_READY;
	case ATA_ERR_NO_DEVICE:
		return BLOCK_DEVICE_NO_MEDIA;
	case ATA_ERR_UNSUPPORTED:
		return BLOCK_DEVICE_UNSUPPORTED;
	case ATA_ERR_OUT_OF_RANGE:
		return BLOCK_DEVICE_OUT_OF_RANGE;
	case ATA_ERR_READ_ONLY:
		return BLOCK_DEVICE_READ_ONLY;
	case ATA_ERR_TIMEOUT:
		return BLOCK_DEVICE_TIMEOUT;
	case ATA_ERR_DEVICE:
	case ATA_ERR_NO_DRQ:
	case ATA_ERR_PROTOCOL:
	default:
		return BLOCK_DEVICE_IO_ERROR;
	}
}

static enum block_device_status
ata_block_probe(kernel_object_handle_t context,
		struct block_device_geometry *geometry)
{
	const struct ata_device_info *info;
	ata_result_t result;

	if (!context_is_owner(context) || geometry == NULL)
		return BLOCK_DEVICE_INVALID_ARGUMENT;
	result = ata_device_initialize(&owner.device);
	if (result != ATA_OK)
		return map_ata_status(result);
	info = ata_device_get_info(&owner.device);
	if (info == NULL || info->state != ATA_DEVICE_READY)
		return BLOCK_DEVICE_NOT_READY;
	if (info->write_sector_enabled > 1u || !info_reserved_is_zero(info))
		return BLOCK_DEVICE_UNSUPPORTED;
	geometry->sector_count = info->sector_count;
	geometry->logical_sector_bytes = info->logical_sector_bytes;
	geometry->writable = info->write_sector_enabled;
	return BLOCK_DEVICE_OK;
}

static enum block_device_status
ata_block_read(kernel_object_handle_t context, block_lba_t lba,
	       union block_device_sector *sector)
{
	union ata_sector ata_sector;
	ata_result_t result;
	size_t index;

	if (!context_is_owner(context) || sector == NULL)
		return BLOCK_DEVICE_INVALID_ARGUMENT;
	result = ata_device_read_sector(&owner.device, lba, &ata_sector);
	if (result != ATA_OK)
		return map_ata_status(result);
	for (index = 0u; index < ARRAY_SIZE(ata_sector.words); ++index)
		sector->words[index] = ata_sector.words[index];
	return BLOCK_DEVICE_OK;
}

static enum block_device_status
ata_block_write(kernel_object_handle_t context, block_lba_t lba,
		const union block_device_sector *sector)
{
	union ata_sector ata_sector;
	size_t index;

	if (!context_is_owner(context) || sector == NULL)
		return BLOCK_DEVICE_INVALID_ARGUMENT;
	for (index = 0u; index < ARRAY_SIZE(ata_sector.words); ++index)
		ata_sector.words[index] = sector->words[index];
	return map_ata_status(ata_device_write_sector(&owner.device, lba,
						      &ata_sector));
}

static enum block_device_status ata_block_flush(kernel_object_handle_t context)
{
	/* Each ATA write completes and conditionally flushes before returning. */
	return context_is_owner(context)
		   ? BLOCK_DEVICE_OK
		   : BLOCK_DEVICE_INVALID_ARGUMENT;
}

static const struct block_device_ops ata_block_ops = {
    .probe = ata_block_probe,
    .read_sector = ata_block_read,
    .write_sector = ata_block_write,
    .flush = ata_block_flush,
};

static block_device_handle_t ata_boot_block_device(void)
{
	struct block_device_geometry geometry;
	block_device_handle_t handle;

	if (owner.boot_handle != BLOCK_DEVICE_HANDLE_INVALID &&
	    block_device_get_geometry(owner.boot_handle, &geometry) ==
		BLOCK_DEVICE_OK)
		return owner.boot_handle;
	if (block_device_register(&ata_block_ops, owner.identity,
				  &handle) != BLOCK_DEVICE_OK)
		return BLOCK_DEVICE_HANDLE_INVALID;
	owner.boot_handle = handle;
	return owner.boot_handle;
}

enum ata_block_status ata_block_initialize(
	kernel_object_handle_t adapter_identity, uint32_t poll_limit,
	ata_write_policy_t write_policy)
{
	if (adapter_identity == 0u ||
	    adapter_identity == KERNEL_OBJECT_HANDLE_INVALID || poll_limit == 0u ||
	    write_policy > ATA_WRITE_POLICY_ALLOW)
		return ATA_BLOCK_INVALID_ARGUMENT;
	if (owner.initialized != 0u)
		return ATA_BLOCK_INVALID_STATE;
	owner.identity = adapter_identity;
	owner.boot_handle = BLOCK_DEVICE_HANDLE_INVALID;
	owner.poll_limit = poll_limit;
	owner.write_policy = write_policy;
	owner.device_constructed = 0u;
	owner.initialized = 1u;
	return ATA_BLOCK_OK;
}

static bool configurations_match(
	const struct ata_device_configuration *left,
	const struct ata_device_configuration *right)
{
	size_t index;

	if (left == NULL || right == NULL ||
	    left->poll_limit != right->poll_limit ||
	    left->command_port != right->command_port ||
	    left->control_port != right->control_port ||
	    left->device != right->device ||
	    left->write_policy != right->write_policy)
		return false;
	for (index = 0u; index < ARRAY_SIZE(left->reserved); ++index) {
		if (left->reserved[index] != right->reserved[index])
			return false;
	}
	return true;
}

static bool locator_has_typed_ata_identity(
	const struct x86_boot_device_locator *locator)
{
	struct x86_ata_device_identity identity;

	if (locator == NULL)
		return false;
	identity.sector_count = locator->sector_count;
	identity.command_port = locator->command_port;
	identity.control_port = locator->control_port;
	identity.logical_sector_bytes = locator->logical_sector_bytes;
	identity.device = locator->ata_device;
	identity.reserved = 0u;
	return x86_boot_device_locator_matches_ata(locator, &identity);
}

static enum ata_block_status ata_block_bind_locator(
	const struct x86_boot_device_locator *locator)
{
	const struct ata_device_configuration *bound;
	struct ata_device_configuration configuration = {0};

	if (locator == NULL ||
	    locator->logical_sector_bytes != ATA_SECTOR_BYTES ||
	    !locator_has_typed_ata_identity(locator))
		return ATA_BLOCK_LOCATOR_MISMATCH;
	configuration.poll_limit = owner.poll_limit;
	configuration.command_port = locator->command_port;
	configuration.control_port = locator->control_port;
	configuration.device = locator->ata_device;
	configuration.write_policy = owner.write_policy;
	if (owner.device_constructed != 0u) {
		bound = ata_device_configuration(&owner.device);
		return configurations_match(bound, &configuration)
			       ? ATA_BLOCK_OK
			       : ATA_BLOCK_LOCATOR_MISMATCH;
	}
	if (ata_device_construct(&owner.device, &configuration) != ATA_OK)
		return ATA_BLOCK_LOCATOR_MISMATCH;
	owner.device_constructed = 1u;
	return ATA_BLOCK_OK;
}

enum ata_block_status ata_block_resolve_boot_locator(
	kernel_object_handle_t adapter_identity,
	const struct x86_boot_device_locator *locator,
	block_device_handle_t *handle)
{
	const struct ata_device_configuration *configuration;
	const struct ata_device_info *info;
	block_device_handle_t prepared_handle;
	struct x86_ata_device_identity identity = {0};
	enum ata_block_status bind_status;

	if (locator == NULL || handle == NULL || adapter_identity == 0u ||
	    adapter_identity == KERNEL_OBJECT_HANDLE_INVALID)
		return ATA_BLOCK_INVALID_ARGUMENT;
	if (owner.initialized != 1u)
		return ATA_BLOCK_INVALID_STATE;
	if (adapter_identity != owner.identity)
		return ATA_BLOCK_STALE_IDENTITY;
	/* Bind validated firmware topology before touching any controller port. */
	bind_status = ata_block_bind_locator(locator);
	if (bind_status != ATA_BLOCK_OK)
		return bind_status;
	if (ata_device_initialize(&owner.device) != ATA_OK)
		return ATA_BLOCK_DEVICE_UNAVAILABLE;
	configuration = ata_device_configuration(&owner.device);
	info = ata_device_get_info(&owner.device);
	if (info == NULL || info->state != ATA_DEVICE_READY ||
	    configuration == NULL || info->logical_sector_bytes > 0xffffu)
		return ATA_BLOCK_DEVICE_UNAVAILABLE;
	identity.sector_count = info->sector_count;
	identity.command_port = configuration->command_port;
	identity.control_port = configuration->control_port;
	identity.logical_sector_bytes =
		(uint16_t)info->logical_sector_bytes;
	identity.device = configuration->device;
	if (!x86_boot_device_locator_matches_ata(locator, &identity))
		return ATA_BLOCK_LOCATOR_MISMATCH;
	prepared_handle = ata_boot_block_device();
	if (prepared_handle == BLOCK_DEVICE_HANDLE_INVALID)
		return ATA_BLOCK_REGISTRY_ERROR;
	*handle = prepared_handle;
	return ATA_BLOCK_OK;
}

// SPDX-License-Identifier: GPL-2.0-only
/* ATA device-object construction, independent from x86 port transport. */
#include "ata.h"

#define ATA_TASK_FILE_PORT_COUNT 8u
#define ATA_DEVICE_MAXIMUM 1u
#define ATA_DEVICE_LIFECYCLE_COOKIE 0x41544132u

static bool reserved_bytes_are_zero(const uint8_t *bytes, size_t count)
{
	size_t index;

	for (index = 0u; index < count; ++index) {
		if (bytes[index] != 0u)
			return false;
	}
	return true;
}

static bool ata_configuration_is_valid(
	const struct ata_device_configuration *configuration)
{
	uint32_t command_limit;

	if (configuration == NULL || configuration->poll_limit == 0u ||
	    configuration->command_port == 0u ||
	    configuration->control_port == 0u ||
	    configuration->device > ATA_DEVICE_MAXIMUM ||
	    configuration->write_policy > ATA_WRITE_POLICY_ALLOW ||
	    !reserved_bytes_are_zero(configuration->reserved,
				     sizeof(configuration->reserved)))
		return false;
	command_limit = (uint32_t)configuration->command_port +
		ATA_TASK_FILE_PORT_COUNT;
	if (command_limit > 0x10000u)
		return false;
	return configuration->control_port < configuration->command_port ||
	       configuration->control_port >= command_limit;
}

static bool ata_device_is_constructed(const struct ata_device *device)
{
	return device != NULL &&
	       device->lifecycle_cookie == ATA_DEVICE_LIFECYCLE_COOKIE;
}

ata_result_t ata_device_construct(struct ata_device *device,
	const struct ata_device_configuration *configuration)
{
	struct ata_device prepared = {0};

	if (device == NULL || !ata_configuration_is_valid(configuration))
		return ATA_ERR_ARGUMENT;
	prepared.configuration = *configuration;
	prepared.info.state = ATA_DEVICE_UNINITIALIZED;
	prepared.info.sector_count = 0u;
	prepared.info.logical_sector_bytes = 0u;
	prepared.info.lba28_supported = false;
	prepared.info.flush_cache_supported = false;
	prepared.info.write_sector_enabled = 0u;
	prepared.info.last_status = 0u;
	prepared.info.last_error = 0u;
	prepared.lifecycle_cookie = ATA_DEVICE_LIFECYCLE_COOKIE;
	prepared.reserved = 0u;
	*device = prepared;
	return ATA_OK;
}

const struct ata_device_configuration *ata_device_configuration(
	const struct ata_device *device)
{
	return ata_device_is_constructed(device) ? &device->configuration : NULL;
}

const struct ata_device_info *ata_device_get_info(
	const struct ata_device *device)
{
	return ata_device_is_constructed(device) ? &device->info : NULL;
}

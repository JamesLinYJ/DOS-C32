// SPDX-License-Identifier: GPL-2.0-only
/* Validated construction of a DOS drive namespace and internal paths. */
#include "dos_drive.h"

#include "dos_int21.h"

#define DOS_DRIVE_ROOT_BYTES 3u
#define DOS_DRIVE_VALID_MASK (((uint32_t)1u << DOS_DRIVE_COUNT) - 1u)

bool dos_int21_drive_config_is_valid(
	const struct dos_int21_drive_config *config)
{
	uint32_t current_bit;
	uint32_t boot_bit;
	uint32_t allowed_mask;

	if (config == NULL || config->reserved != 0u ||
	    config->current_drive >= DOS_DRIVE_COUNT ||
	    config->boot_drive == 0u || config->boot_drive > DOS_DRIVE_COUNT ||
	    config->last_drive == 0u || config->last_drive > DOS_DRIVE_COUNT ||
	    config->current_drive >= config->last_drive ||
	    config->boot_drive > config->last_drive ||
	    config->available_drive_mask == 0u ||
	    (config->available_drive_mask & ~DOS_DRIVE_VALID_MASK) != 0u)
		return false;
	current_bit = (uint32_t)1u << config->current_drive;
	boot_bit = (uint32_t)1u << (config->boot_drive - 1u);
	allowed_mask = ((uint32_t)1u << config->last_drive) - 1u;
	return (config->available_drive_mask & current_bit) != 0u &&
	       (config->available_drive_mask & boot_bit) != 0u &&
	       (config->available_drive_mask & ~allowed_mask) == 0u;
}

bool dos_drive_index_is_valid(uint8_t drive_index)
{
	return drive_index < DOS_DRIVE_COUNT;
}

enum dos_drive_status dos_drive_configure_single_volume(
	uint8_t drive_index, struct dos_int21_drive_config *config)
{
	struct dos_int21_drive_config prepared;

	if (config == NULL || !dos_drive_index_is_valid(drive_index))
		return DOS_DRIVE_INVALID_ARGUMENT;
	prepared = (struct dos_int21_drive_config){
		.available_drive_mask = (uint32_t)1u << drive_index,
		.current_drive = drive_index,
		.boot_drive = (uint8_t)(drive_index + 1u),
		.last_drive = (uint8_t)(drive_index + 1u),
		.reserved = 0u,
	};
	if (!dos_int21_drive_config_is_valid(&prepared))
		return DOS_DRIVE_INVALID_ARGUMENT;
	*config = prepared;
	return DOS_DRIVE_OK;
}

enum dos_drive_status dos_drive_resolve_designator(
	const struct dos_int21_drive_config *config, uint8_t designator,
	uint8_t *drive_index)
{
	uint8_t resolved;

	if (drive_index == NULL || !dos_int21_drive_config_is_valid(config))
		return DOS_DRIVE_INVALID_ARGUMENT;
	if (designator == 0u) {
		resolved = config->current_drive;
	} else {
		if (designator > DOS_DRIVE_COUNT)
			return DOS_DRIVE_UNAVAILABLE;
		resolved = (uint8_t)(designator - 1u);
	}
	if ((config->available_drive_mask & ((uint32_t)1u << resolved)) == 0u)
		return DOS_DRIVE_UNAVAILABLE;
	*drive_index = resolved;
	return DOS_DRIVE_OK;
}

enum dos_drive_status dos_drive_format_root(
	uint8_t drive_index, char *destination, size_t capacity,
	size_t *length)
{
	if (!dos_drive_index_is_valid(drive_index) || destination == NULL ||
	    length == NULL)
		return DOS_DRIVE_INVALID_ARGUMENT;
	if (capacity <= DOS_DRIVE_ROOT_BYTES)
		return DOS_DRIVE_CAPACITY;
	destination[0] = (char)('A' + drive_index);
	destination[1] = ':';
	destination[2] = '\\';
	destination[DOS_DRIVE_ROOT_BYTES] = '\0';
	*length = DOS_DRIVE_ROOT_BYTES;
	return DOS_DRIVE_OK;
}

enum dos_drive_status dos_drive_format_absolute(
	uint8_t drive_index, const char *component, size_t component_length,
	char *destination, size_t capacity, size_t *length)
{
	size_t root_length;
	size_t index;

	if (component == NULL || component_length == 0u ||
	    component[0] == '\\' || component[0] == '/' ||
	    component[0] == '\0')
		return DOS_DRIVE_INVALID_ARGUMENT;
	if (component_length > (size_t)-1 - DOS_DRIVE_ROOT_BYTES - 1u ||
	    capacity < DOS_DRIVE_ROOT_BYTES + component_length + 1u)
		return DOS_DRIVE_CAPACITY;
	for (index = 0u; index < component_length; ++index) {
		if (component[index] == '\0' || component[index] == ':')
			return DOS_DRIVE_INVALID_ARGUMENT;
	}
	if (dos_drive_format_root(drive_index, destination, capacity,
				  &root_length) != DOS_DRIVE_OK)
		return DOS_DRIVE_INVALID_ARGUMENT;
	for (index = 0u; index < component_length; ++index)
		destination[root_length + index] = component[index];
	destination[root_length + component_length] = '\0';
	*length = root_length + component_length;
	return DOS_DRIVE_OK;
}

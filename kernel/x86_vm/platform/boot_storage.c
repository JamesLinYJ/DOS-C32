// SPDX-License-Identifier: GPL-2.0-only
/*
 * Legacy-BIOS boot-device locator
 *
 * BIOS contract: INT 13h AH=41h identifies EDD support and AH=48h returns a
 * byte-defined device path plus an optional Device Parameter Table Extension.
 * Safety model: decode the real-mode handoff bytewise, require both independent
 * ATA device selectors to agree, and publish no locator until every checksum
 * and bound has been validated.
 */
#include "x86_boot_storage.h"

#define EDD_FIXED_DISK_ACCESS (1u << 0)
#define EDD_IDENTITY_FLAG_MASK                                            \
	(X86_BOOT_STORAGE_EXTENSIONS_PRESENT |                             \
	 X86_BOOT_STORAGE_PARAMETERS_PRESENT | X86_BOOT_STORAGE_DPTE_PRESENT)

#define EDD_PARAMETERS_MINIMUM_BYTES 30u
#define EDD_PARAMETERS_LENGTH 0u
#define EDD_PARAMETERS_SECTOR_COUNT 16u
#define EDD_PARAMETERS_SECTOR_BYTES 24u
#define EDD_PARAMETERS_DEVICE_PATH_KEY 30u
#define EDD_PARAMETERS_DEVICE_PATH_LENGTH 32u
#define EDD_PARAMETERS_DEVICE_PATH_RESERVED8 33u
#define EDD_PARAMETERS_DEVICE_PATH_RESERVED16 34u
#define EDD_PARAMETERS_HOST_BUS 36u
#define EDD_PARAMETERS_INTERFACE 40u
#define EDD_PARAMETERS_INTERFACE_PATH 48u
#define EDD_PARAMETERS_DEVICE_PATH 56u

#define EDD_DEVICE_PATH_KEY 0xbeddu
#define EDD_DEVICE_PATH_SHORT_BYTES 36u
#define EDD_DEVICE_PATH_FULL_BYTES 44u

#define EDD_HOST_BUS_BYTES 4u
#define EDD_INTERFACE_BYTES 8u
#define EDD_PCI_BUS_OFFSET 0u
#define EDD_PCI_SLOT_OFFSET 1u
#define EDD_PCI_FUNCTION_OFFSET 2u
#define EDD_PCI_CHANNEL_OFFSET 3u
#define EDD_PCI_SLOT_MAXIMUM 31u
#define EDD_PCI_FUNCTION_MAXIMUM 7u

#define EDD_DPTE_COMMAND_PORT 0u
#define EDD_DPTE_CONTROL_PORT 2u
#define EDD_DPTE_HEAD_PREFIX 4u
#define EDD_DPTE_REVISION 14u
#define EDD_DPTE_MINIMUM_REVISION 0x10u

#define ATA_HEAD_FIXED_BITS 0xa0u
#define ATA_HEAD_FIXED_MASK 0xafu
#define ATA_HEAD_DEVICE_BIT 0x10u

static uint16_t read_le16(const uint8_t *bytes)
{
	return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8u);
}

static uint64_t read_le64(const uint8_t *bytes)
{
	uint64_t value = 0u;
	uint32_t shift;

	for (shift = 0u; shift < 64u; shift += 8u)
		value |= (uint64_t)bytes[shift / 8u] << shift;
	return value;
}

static bool bytes_are_zero(const uint8_t *bytes, size_t count)
{
	size_t index;

	for (index = 0u; index < count; ++index) {
		if (bytes[index] != 0u)
			return false;
	}
	return true;
}

static bool bytes_equal(const uint8_t *bytes, const char *expected,
			 size_t count)
{
	size_t index;

	for (index = 0u; index < count; ++index) {
		if (bytes[index] != (uint8_t)expected[index])
			return false;
	}
	return true;
}

static bool checksum_is_zero(const uint8_t *bytes, size_t count)
{
	uint8_t checksum = 0u;
	size_t index;

	for (index = 0u; index < count; ++index)
		checksum = (uint8_t)(checksum + bytes[index]);
	return checksum == 0u;
}

static bool handoff_header_is_valid(
	const struct x86_boot_storage_handoff *handoff,
	uint8_t expected_boot_drive)
{
	return handoff->signature == X86_BOOT_STORAGE_SIGNATURE &&
	       handoff->version == X86_BOOT_STORAGE_VERSION &&
	       handoff->bytes == X86_BOOT_STORAGE_BYTES &&
	       (handoff->flags & ~X86_BOOT_STORAGE_FLAG_MASK) == 0u &&
	       handoff->boot_drive == expected_boot_drive &&
	       bytes_are_zero(handoff->reserved,
			      X86_BOOT_STORAGE_RESERVED_BYTES);
}

static enum x86_boot_storage_status decode_dpte(
	const uint8_t dpte[X86_BOOT_EDD_DPTE_BYTES],
	struct x86_boot_device_locator *prepared)
{
	uint16_t command_port = read_le16(dpte + EDD_DPTE_COMMAND_PORT);
	uint16_t control_port = read_le16(dpte + EDD_DPTE_CONTROL_PORT);
	uint8_t head_prefix = dpte[EDD_DPTE_HEAD_PREFIX];

	if (!checksum_is_zero(dpte, X86_BOOT_EDD_DPTE_BYTES) ||
	    dpte[EDD_DPTE_REVISION] < EDD_DPTE_MINIMUM_REVISION ||
	    command_port == 0u || command_port > 0xfff8u ||
	    control_port == 0u || control_port == command_port ||
	    (head_prefix & ATA_HEAD_FIXED_MASK) != ATA_HEAD_FIXED_BITS)
		return X86_BOOT_STORAGE_CORRUPT;
	prepared->command_port = command_port;
	prepared->control_port = control_port;
	prepared->ata_device =
		(head_prefix & ATA_HEAD_DEVICE_BIT) != 0u ? 1u : 0u;
	prepared->flags |= X86_BOOT_DEVICE_LOCATOR_DPTE;
	return X86_BOOT_STORAGE_OK;
}

static enum x86_boot_storage_status decode_device_path(
	const uint8_t parameters[X86_BOOT_EDD_PARAMETERS_BYTES],
	struct x86_boot_device_locator *prepared)
{
	static const char pci_bus[EDD_HOST_BUS_BYTES] = {'P', 'C', 'I', ' '};
	static const char isa_bus[EDD_HOST_BUS_BYTES] = {'I', 'S', 'A', ' '};
	static const char ata_interface[EDD_INTERFACE_BYTES] = {
		'A', 'T', 'A', ' ', ' ', ' ', ' ', ' '
	};
	const uint8_t *interface_path =
		parameters + EDD_PARAMETERS_INTERFACE_PATH;
	uint8_t path_length = parameters[EDD_PARAMETERS_DEVICE_PATH_LENGTH];
	uint8_t path_device = parameters[EDD_PARAMETERS_DEVICE_PATH];

	if (read_le16(parameters + EDD_PARAMETERS_DEVICE_PATH_KEY) !=
			EDD_DEVICE_PATH_KEY ||
	    (path_length != EDD_DEVICE_PATH_SHORT_BYTES &&
	     path_length != EDD_DEVICE_PATH_FULL_BYTES) ||
	    EDD_PARAMETERS_DEVICE_PATH_KEY + path_length >
			X86_BOOT_EDD_PARAMETERS_BYTES ||
	    parameters[EDD_PARAMETERS_DEVICE_PATH_RESERVED8] != 0u ||
	    read_le16(parameters + EDD_PARAMETERS_DEVICE_PATH_RESERVED16) != 0u ||
	    !checksum_is_zero(parameters + EDD_PARAMETERS_DEVICE_PATH_KEY,
			      path_length))
		return X86_BOOT_STORAGE_CORRUPT;
	if (!bytes_equal(parameters + EDD_PARAMETERS_INTERFACE, ata_interface,
			 EDD_INTERFACE_BYTES))
		return X86_BOOT_STORAGE_UNSUPPORTED;
	if (path_device > 1u || path_device != prepared->ata_device)
		return X86_BOOT_STORAGE_CORRUPT;

	if (bytes_equal(parameters + EDD_PARAMETERS_HOST_BUS, pci_bus,
			EDD_HOST_BUS_BYTES)) {
		if (interface_path[EDD_PCI_SLOT_OFFSET] >
				EDD_PCI_SLOT_MAXIMUM ||
		    interface_path[EDD_PCI_FUNCTION_OFFSET] >
				EDD_PCI_FUNCTION_MAXIMUM ||
		    !bytes_are_zero(interface_path + 4u, 4u))
			return X86_BOOT_STORAGE_CORRUPT;
		prepared->host_bus = X86_BOOT_HOST_BUS_PCI;
		prepared->pci_bus = interface_path[EDD_PCI_BUS_OFFSET];
		prepared->pci_slot = interface_path[EDD_PCI_SLOT_OFFSET];
		prepared->pci_function =
			interface_path[EDD_PCI_FUNCTION_OFFSET];
		prepared->ata_channel = interface_path[EDD_PCI_CHANNEL_OFFSET];
	} else if (bytes_equal(parameters + EDD_PARAMETERS_HOST_BUS, isa_bus,
			       EDD_HOST_BUS_BYTES)) {
		if (read_le16(interface_path) != prepared->command_port ||
		    !bytes_are_zero(interface_path + 2u, 6u))
			return X86_BOOT_STORAGE_CORRUPT;
		prepared->host_bus = X86_BOOT_HOST_BUS_ISA;
	} else {
		return X86_BOOT_STORAGE_UNSUPPORTED;
	}
	prepared->flags |= X86_BOOT_DEVICE_LOCATOR_EDD_PATH;
	return X86_BOOT_STORAGE_OK;
}

enum x86_boot_storage_status x86_boot_storage_decode(
	const struct x86_boot_info *boot_info, uint8_t expected_boot_drive,
	struct x86_boot_device_locator *locator)
{
	struct x86_boot_device_locator prepared = {0};
	const struct x86_boot_storage_handoff *handoff;
	const uint8_t *parameters;
	enum x86_boot_storage_status status;
	uint16_t parameter_bytes;
	uint16_t sector_bytes;
	uint16_t volume_sector_bytes;

	if (boot_info == NULL || locator == NULL)
		return X86_BOOT_STORAGE_INVALID_ARGUMENT;
	if (boot_info->signature != X86_BOOT_INFO_SIGNATURE ||
	    boot_info->version != X86_BOOT_INFO_VERSION ||
	    boot_info->header_bytes != X86_BOOT_INFO_HEADER_BYTES ||
	    boot_info->range_bytes != X86_BOOT_MEMORY_RANGE_BYTES)
		return X86_BOOT_STORAGE_CORRUPT;
	handoff = &boot_info->storage;
	if (!handoff_header_is_valid(handoff, expected_boot_drive))
		return X86_BOOT_STORAGE_CORRUPT;
	if (handoff->flags == 0u)
		return X86_BOOT_STORAGE_NOT_AVAILABLE;
	if ((handoff->flags & EDD_IDENTITY_FLAG_MASK) !=
			EDD_IDENTITY_FLAG_MASK ||
	    (handoff->interface_support & EDD_FIXED_DISK_ACCESS) == 0u)
		return X86_BOOT_STORAGE_NOT_AVAILABLE;
	/* A complete device identity with no validated boot-volume extent means
	 * the live VBR metadata was malformed, not that the device disappeared. */
	if ((handoff->flags & X86_BOOT_STORAGE_VOLUME_PRESENT) == 0u)
		return X86_BOOT_STORAGE_CORRUPT;

	parameters = handoff->parameters;
	parameter_bytes = read_le16(parameters + EDD_PARAMETERS_LENGTH);
	sector_bytes = read_le16(parameters + EDD_PARAMETERS_SECTOR_BYTES);
	if (parameter_bytes < EDD_PARAMETERS_MINIMUM_BYTES ||
	    parameter_bytes > X86_BOOT_EDD_PARAMETERS_BYTES ||
	    sector_bytes == 0u || (sector_bytes & (sector_bytes - 1u)) != 0u)
		return X86_BOOT_STORAGE_CORRUPT;
	prepared.sector_count =
		read_le64(parameters + EDD_PARAMETERS_SECTOR_COUNT);
	if (prepared.sector_count == 0u)
		return X86_BOOT_STORAGE_CORRUPT;
	prepared.boot_volume_first_lba =
		read_le64(handoff->volume_start_lba);
	prepared.boot_volume_sector_count =
		read_le64(handoff->volume_sector_count);
	volume_sector_bytes = read_le16(
		(const uint8_t *)&handoff->volume_sector_bytes);
	if (prepared.boot_volume_sector_count == 0u ||
	    volume_sector_bytes != sector_bytes ||
	    prepared.boot_volume_first_lba >= prepared.sector_count ||
	    prepared.boot_volume_sector_count >
		    prepared.sector_count - prepared.boot_volume_first_lba)
		return X86_BOOT_STORAGE_CORRUPT;
	prepared.logical_sector_bytes = sector_bytes;
	prepared.edd_interface_support = handoff->interface_support;
	prepared.bios_drive = handoff->boot_drive;
	prepared.edd_version = handoff->edd_version;

	status = decode_dpte(handoff->dpte, &prepared);
	if (status != X86_BOOT_STORAGE_OK)
		return status;
	status = decode_device_path(parameters, &prepared);
	if (status != X86_BOOT_STORAGE_OK)
		return status;
	if (prepared.flags != X86_BOOT_DEVICE_LOCATOR_FLAG_MASK)
		return X86_BOOT_STORAGE_CORRUPT;
	*locator = prepared;
	return X86_BOOT_STORAGE_OK;
}

bool x86_boot_device_locator_matches_ata(
	const struct x86_boot_device_locator *locator,
	const struct x86_ata_device_identity *identity)
{
	return locator != NULL && identity != NULL &&
	       locator->flags == X86_BOOT_DEVICE_LOCATOR_FLAG_MASK &&
	       locator->host_bus != X86_BOOT_HOST_BUS_UNKNOWN &&
	       locator->sector_count != 0u && identity->sector_count != 0u &&
	       locator->logical_sector_bytes != 0u &&
	       identity->logical_sector_bytes != 0u &&
	       locator->ata_device <= 1u && identity->device <= 1u &&
	       identity->reserved == 0u &&
	       bytes_are_zero(locator->reserved, sizeof(locator->reserved)) &&
	       locator->command_port == identity->command_port &&
	       locator->control_port == identity->control_port &&
	       locator->ata_device == identity->device &&
	       locator->logical_sector_bytes == identity->logical_sector_bytes &&
	       locator->sector_count == identity->sector_count;
}

// SPDX-License-Identifier: GPL-2.0-only
#include "ata.h"
#include "ata_block.h"
#include "object_identity.h"
#include "test_entry.h"

#define TEST_SECTOR_COUNT 32768u
#define TEST_COMMAND_PORT 0x0170u
#define TEST_CONTROL_PORT 0x0376u
#define TEST_DEVICE 1u
#define TEST_POLL_LIMIT 765432u
#define HANDLE_SENTINEL ((block_device_handle_t)0x8877665544332211ull)

#ifndef CONFIG_X86_ATA_WRITE_POLICY
#error "CONFIG_X86_ATA_WRITE_POLICY must be defined"
#endif

static ata_result_t initialize_result = ATA_OK;
static uint32_t construct_calls;
static uint32_t initialize_calls;
static uint32_t write_calls;
static uint8_t published_reserved_byte;
static struct ata_device_configuration constructed_configuration;

ata_result_t ata_device_construct(struct ata_device *device,
	const struct ata_device_configuration *configuration)
{
	size_t index;

	if (device == NULL || configuration == NULL)
		return ATA_ERR_ARGUMENT;
	++construct_calls;
	constructed_configuration = *configuration;
	device->configuration = *configuration;
	device->info.state = ATA_DEVICE_READY;
	device->info.sector_count = TEST_SECTOR_COUNT;
	device->info.logical_sector_bytes = ATA_SECTOR_BYTES;
	device->info.lba28_supported = true;
	device->info.flush_cache_supported = true;
	device->info.write_sector_enabled =
		configuration->write_policy == ATA_WRITE_POLICY_ALLOW ? 1u : 0u;
	device->info.last_status = 0u;
	device->info.last_error = 0u;
	for (index = 0u; index < ARRAY_SIZE(device->info.reserved); ++index)
		device->info.reserved[index] = 0u;
	return ATA_OK;
}

const struct ata_device_configuration *ata_device_configuration(
	const struct ata_device *device)
{
	return device != NULL ? &device->configuration : NULL;
}

ata_result_t ata_device_initialize(struct ata_device *device)
{
	++initialize_calls;
	device->info.reserved[0] = published_reserved_byte;
	return initialize_result;
}

const struct ata_device_info *ata_device_get_info(
	const struct ata_device *device)
{
	return device != NULL ? &device->info : NULL;
}

ata_result_t ata_device_read_sector(struct ata_device *device,
	block_lba_t lba, union ata_sector *sector)
{
	(void)device;
	if (sector == NULL || lba >= TEST_SECTOR_COUNT)
		return ATA_ERR_ARGUMENT;
	sector->words[0] = (uint16_t)lba;
	return ATA_OK;
}

ata_result_t ata_device_write_sector(struct ata_device *device,
	block_lba_t lba, const union ata_sector *sector)
{
	(void)device;
	if (sector == NULL || lba >= TEST_SECTOR_COUNT)
		return ATA_ERR_ARGUMENT;
	++write_calls;
	return ATA_OK;
}

const char *ata_result_string(ata_result_t result)
{
	(void)result;
	return "test";
}

static struct x86_boot_device_locator firmware_locator(void)
{
	struct x86_boot_device_locator locator = {
		.sector_count = TEST_SECTOR_COUNT,
		.flags = X86_BOOT_DEVICE_LOCATOR_FLAG_MASK,
		.command_port = TEST_COMMAND_PORT,
		.control_port = TEST_CONTROL_PORT,
		.logical_sector_bytes = ATA_SECTOR_BYTES,
		.edd_interface_support = 1u,
		.bios_drive = 0x80u,
		.edd_version = 0x30u,
		.ata_device = TEST_DEVICE,
		.host_bus = X86_BOOT_HOST_BUS_PCI,
		.pci_bus = 0u,
		.pci_slot = 1u,
		.pci_function = 1u,
		.ata_channel = 0u,
		.reserved = {0u},
	};

	return locator;
}

static int run_tests(void)
{
	struct kernel_object_identity_source identity_source =
		KERNEL_OBJECT_IDENTITY_SOURCE_INITIALIZER;
	struct x86_boot_device_locator locator = firmware_locator();
	struct block_device_geometry geometry;
	union block_device_sector sector = {.bytes = {0u}};
	kernel_object_handle_t owner_identity;
	kernel_object_handle_t stale_identity;
	block_device_handle_t first = HANDLE_SENTINEL;
	block_device_handle_t second = HANDLE_SENTINEL;

	if (kernel_object_identity_source_initialize(&identity_source) !=
		    KERNEL_OBJECT_IDENTITY_OK ||
	    kernel_object_identity_allocate(&identity_source, &owner_identity) !=
		    KERNEL_OBJECT_IDENTITY_OK ||
	    kernel_object_identity_allocate(&identity_source, &stale_identity) !=
		    KERNEL_OBJECT_IDENTITY_OK)
		return 1;
	if (ata_block_resolve_boot_locator(owner_identity, &locator, &first) !=
		    ATA_BLOCK_INVALID_STATE ||
	    first != HANDLE_SENTINEL)
		return 2;
	if (ata_block_initialize(0u, TEST_POLL_LIMIT,
				 (ata_write_policy_t)CONFIG_X86_ATA_WRITE_POLICY) !=
		    ATA_BLOCK_INVALID_ARGUMENT ||
	    ata_block_initialize(KERNEL_OBJECT_HANDLE_INVALID,
				 TEST_POLL_LIMIT,
				 (ata_write_policy_t)
				 CONFIG_X86_ATA_WRITE_POLICY) !=
		    ATA_BLOCK_INVALID_ARGUMENT ||
	    ata_block_initialize(owner_identity, 0u,
				 (ata_write_policy_t)CONFIG_X86_ATA_WRITE_POLICY) !=
		    ATA_BLOCK_INVALID_ARGUMENT ||
	    ata_block_initialize(owner_identity, TEST_POLL_LIMIT, 2u) !=
		    ATA_BLOCK_INVALID_ARGUMENT ||
	    ata_block_initialize(owner_identity, TEST_POLL_LIMIT,
				 (ata_write_policy_t)CONFIG_X86_ATA_WRITE_POLICY) !=
		    ATA_BLOCK_OK ||
	    ata_block_initialize(stale_identity, TEST_POLL_LIMIT,
				 (ata_write_policy_t)CONFIG_X86_ATA_WRITE_POLICY) !=
		    ATA_BLOCK_INVALID_STATE)
		return 3;
	if (ata_block_resolve_boot_locator(stale_identity, &locator, &first) !=
		    ATA_BLOCK_STALE_IDENTITY ||
	    first != HANDLE_SENTINEL)
		return 4;

	locator.logical_sector_bytes = 4096u;
	if (ata_block_resolve_boot_locator(owner_identity, &locator, &first) !=
		    ATA_BLOCK_LOCATOR_MISMATCH ||
	    first != HANDLE_SENTINEL || construct_calls != 0u ||
	    initialize_calls != 0u)
		return 5;
	locator = firmware_locator();
	initialize_result = ATA_ERR_NO_DEVICE;
	if (ata_block_resolve_boot_locator(owner_identity, &locator, &first) !=
		    ATA_BLOCK_DEVICE_UNAVAILABLE ||
	    first != HANDLE_SENTINEL || construct_calls != 1u ||
	    initialize_calls != 1u ||
	    constructed_configuration.poll_limit != TEST_POLL_LIMIT ||
	    constructed_configuration.command_port != TEST_COMMAND_PORT ||
	    constructed_configuration.control_port != TEST_CONTROL_PORT ||
	    constructed_configuration.device != TEST_DEVICE ||
	    constructed_configuration.write_policy !=
		    (ata_write_policy_t)CONFIG_X86_ATA_WRITE_POLICY)
		return 6;

	locator.command_port = 0x01f0u;
	locator.control_port = 0x03f6u;
	locator.ata_device = 0u;
	if (ata_block_resolve_boot_locator(owner_identity, &locator, &first) !=
		    ATA_BLOCK_LOCATOR_MISMATCH ||
	    first != HANDLE_SENTINEL || construct_calls != 1u ||
	    initialize_calls != 1u)
		return 7;
	locator = firmware_locator();
	initialize_result = ATA_OK;
	published_reserved_byte = 1u;
	if (ata_block_resolve_boot_locator(owner_identity, &locator, &first) !=
		    ATA_BLOCK_REGISTRY_ERROR ||
	    first != HANDLE_SENTINEL)
		return 8;
	published_reserved_byte = 0u;
	if (ata_block_resolve_boot_locator(owner_identity, &locator, &first) !=
		    ATA_BLOCK_OK ||
	    first == BLOCK_DEVICE_HANDLE_INVALID || first == HANDLE_SENTINEL ||
	    block_device_get_geometry(first, &geometry) != BLOCK_DEVICE_OK ||
	    geometry.sector_count != TEST_SECTOR_COUNT ||
	    geometry.logical_sector_bytes != ATA_SECTOR_BYTES ||
	    geometry.writable != CONFIG_X86_ATA_WRITE_POLICY)
		return 9;
	if (block_device_write_sector(first, 0u, &sector) !=
		    (CONFIG_X86_ATA_WRITE_POLICY == ATA_WRITE_POLICY_ALLOW
			     ? BLOCK_DEVICE_OK
			     : BLOCK_DEVICE_READ_ONLY) ||
	    write_calls !=
		    (CONFIG_X86_ATA_WRITE_POLICY == ATA_WRITE_POLICY_ALLOW ? 1u
								      : 0u))
		return 10;
	if (ata_block_resolve_boot_locator(owner_identity, &locator, &second) !=
		    ATA_BLOCK_OK ||
	    second != first)
		return 11;
	if (block_device_unregister(first) != BLOCK_DEVICE_OK ||
	    block_device_get_geometry(first, &geometry) !=
		    BLOCK_DEVICE_STALE_HANDLE)
		return 12;
	second = HANDLE_SENTINEL;
	if (ata_block_resolve_boot_locator(stale_identity, &locator, &second) !=
		    ATA_BLOCK_STALE_IDENTITY ||
	    second != HANDLE_SENTINEL)
		return 13;
	if (ata_block_resolve_boot_locator(owner_identity, &locator, &second) !=
		    ATA_BLOCK_OK ||
	    second == first || second == BLOCK_DEVICE_HANDLE_INVALID)
		return 14;
	return 0;
}

DOSC32_TEST_ENTRY(run_tests)

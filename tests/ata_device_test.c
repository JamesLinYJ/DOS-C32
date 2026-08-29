// SPDX-License-Identifier: GPL-2.0-only
#include "ata.h"
#include "io.h"
#include "test_entry.h"

#define TEST_SECTOR_COUNT 32768u
#define TEST_COMMAND_PORT 0x0170u
#define TEST_CONTROL_PORT 0x0376u
#define TEST_DEVICE 1u
#define TEST_POLL_LIMIT 765432u

#define TEST_ATA_STATUS_READY 0x40u
#define TEST_ATA_STATUS_DRQ 0x08u
#define TEST_ATA_COMMAND_WRITE 0x30u
#define TEST_ATA_COMMAND_IDENTIFY 0xecu
#define TEST_ATA_STATUS_COMMAND_PORT (TEST_COMMAND_PORT + 7u)
#define TEST_ATA_LBA_MID_PORT (TEST_COMMAND_PORT + 4u)
#define TEST_ATA_LBA_HIGH_PORT (TEST_COMMAND_PORT + 5u)

#define TEST_IDENTIFY_GENERAL_CONFIGURATION 0u
#define TEST_IDENTIFY_CAPABILITIES 49u
#define TEST_IDENTIFY_LBA28_LOW 60u
#define TEST_IDENTIFY_LBA28_HIGH 61u
#define TEST_IDENTIFY_PACKET_DEVICE 0x8000u
#define TEST_IDENTIFY_LBA_SUPPORTED 0x0200u

static union ata_sector emulated_identify;
static size_t identify_word_index;
static size_t write_word_count;
static uint32_t identify_command_count;
static uint32_t write_command_count;
static uint8_t active_command;

static void reset_emulated_controller(bool packet_device)
{
	size_t index;

	for (index = 0u; index < ARRAY_SIZE(emulated_identify.words); ++index)
		emulated_identify.words[index] = 0u;
	emulated_identify.words[TEST_IDENTIFY_GENERAL_CONFIGURATION] =
		packet_device ? TEST_IDENTIFY_PACKET_DEVICE : 0u;
	emulated_identify.words[TEST_IDENTIFY_CAPABILITIES] =
		TEST_IDENTIFY_LBA_SUPPORTED;
	emulated_identify.words[TEST_IDENTIFY_LBA28_LOW] =
		(uint16_t)TEST_SECTOR_COUNT;
	emulated_identify.words[TEST_IDENTIFY_LBA28_HIGH] =
		(uint16_t)(TEST_SECTOR_COUNT >> 16);
	identify_word_index = 0u;
	write_word_count = 0u;
	identify_command_count = 0u;
	write_command_count = 0u;
	active_command = 0u;
}

uint8_t inb(uint16_t port)
{
	if (port == TEST_CONTROL_PORT) {
		if (active_command == TEST_ATA_COMMAND_IDENTIFY &&
		    identify_word_index < ARRAY_SIZE(emulated_identify.words))
			return TEST_ATA_STATUS_DRQ;
		if (active_command == TEST_ATA_COMMAND_WRITE &&
		    write_word_count < ARRAY_SIZE(emulated_identify.words))
			return TEST_ATA_STATUS_DRQ;
		return TEST_ATA_STATUS_READY;
	}
	if (port == TEST_ATA_LBA_MID_PORT ||
	    port == TEST_ATA_LBA_HIGH_PORT)
		return 0u;
	return TEST_ATA_STATUS_READY;
}

void outb(uint16_t port, uint8_t value)
{
	if (port != TEST_ATA_STATUS_COMMAND_PORT)
		return;
	active_command = value;
	if (value == TEST_ATA_COMMAND_IDENTIFY) {
		identify_word_index = 0u;
		++identify_command_count;
	} else if (value == TEST_ATA_COMMAND_WRITE) {
		write_word_count = 0u;
		++write_command_count;
	}
}

uint16_t inw(uint16_t port)
{
	if (port != TEST_COMMAND_PORT ||
	    active_command != TEST_ATA_COMMAND_IDENTIFY ||
	    identify_word_index >= ARRAY_SIZE(emulated_identify.words))
		return 0u;
	return emulated_identify.words[identify_word_index++];
}

void outw(uint16_t port, uint16_t value)
{
	(void)value;
	if (port == TEST_COMMAND_PORT &&
	    active_command == TEST_ATA_COMMAND_WRITE)
		++write_word_count;
}

static bool reserved_bytes_are_zero(const uint8_t *bytes, size_t count)
{
	size_t index;

	for (index = 0u; index < count; ++index) {
		if (bytes[index] != 0u)
			return false;
	}
	return true;
}

static bool configuration_matches(
	const struct ata_device_configuration *configuration,
	ata_write_policy_t write_policy)
{
	return configuration != NULL &&
	       configuration->command_port == TEST_COMMAND_PORT &&
	       configuration->control_port == TEST_CONTROL_PORT &&
	       configuration->device == TEST_DEVICE &&
	       configuration->poll_limit == TEST_POLL_LIMIT &&
	       configuration->write_policy == write_policy &&
	       reserved_bytes_are_zero(configuration->reserved,
				       sizeof(configuration->reserved));
}

static int reject_invalid_configurations(struct ata_device *device)
{
	struct ata_device_configuration configuration = {
		.poll_limit = 1000u,
		.command_port = TEST_COMMAND_PORT,
		.control_port = TEST_CONTROL_PORT,
		.device = TEST_DEVICE,
		.write_policy = ATA_WRITE_POLICY_ALLOW,
		.reserved = {0u},
	};

	if (ata_device_construct(NULL, &configuration) != ATA_ERR_ARGUMENT ||
	    ata_device_construct(device, NULL) != ATA_ERR_ARGUMENT)
		return 1;
	configuration.poll_limit = 0u;
	if (ata_device_construct(device, &configuration) != ATA_ERR_ARGUMENT)
		return 1;
	configuration.poll_limit = 1000u;
	configuration.command_port = 0u;
	if (ata_device_construct(device, &configuration) != ATA_ERR_ARGUMENT)
		return 1;
	configuration.command_port = 0xfff9u;
	if (ata_device_construct(device, &configuration) != ATA_ERR_ARGUMENT)
		return 1;
	configuration.command_port = TEST_COMMAND_PORT;
	configuration.control_port = TEST_COMMAND_PORT + 7u;
	if (ata_device_construct(device, &configuration) != ATA_ERR_ARGUMENT)
		return 1;
	configuration.control_port = TEST_CONTROL_PORT;
	configuration.device = 2u;
	if (ata_device_construct(device, &configuration) != ATA_ERR_ARGUMENT)
		return 1;
	configuration.device = TEST_DEVICE;
	configuration.write_policy = 2u;
	if (ata_device_construct(device, &configuration) != ATA_ERR_ARGUMENT)
		return 1;
	configuration.write_policy = ATA_WRITE_POLICY_ALLOW;
	configuration.reserved[3] = 1u;
	return ata_device_construct(device, &configuration) == ATA_ERR_ARGUMENT
		       ? 0
		       : 1;
}

static int run_tests(void)
{
	struct ata_device device;
	struct ata_device_configuration configuration = {
		.poll_limit = TEST_POLL_LIMIT,
		.command_port = TEST_COMMAND_PORT,
		.control_port = TEST_CONTROL_PORT,
		.device = TEST_DEVICE,
		.write_policy = ATA_WRITE_POLICY_ALLOW,
		.reserved = {0u},
	};
	union ata_sector sector = {.bytes = {0u}};
	const struct ata_device_info *info;

	if (reject_invalid_configurations(&device) != 0)
		return 1;
	if (ata_device_construct(&device, &configuration) != ATA_OK)
		return 2;
	if (!configuration_matches(ata_device_configuration(&device),
				   ATA_WRITE_POLICY_ALLOW))
		return 3;
	info = ata_device_get_info(&device);
	if (info == NULL || info->state != ATA_DEVICE_UNINITIALIZED ||
	    info->sector_count != 0u || info->logical_sector_bytes != 0u ||
	    info->lba28_supported != 0u ||
	    info->flush_cache_supported != 0u ||
	    info->write_sector_enabled != 0u ||
	    !reserved_bytes_are_zero(info->reserved, sizeof(info->reserved)))
		return 4;

	reset_emulated_controller(false);
	if (ata_device_initialize(&device) != ATA_OK)
		return 5;
	info = ata_device_get_info(&device);
	if (info == NULL || info->state != ATA_DEVICE_READY ||
	    info->sector_count != TEST_SECTOR_COUNT ||
	    info->logical_sector_bytes != ATA_SECTOR_BYTES ||
	    info->lba28_supported != 1u ||
	    info->write_sector_enabled != 1u ||
	    identify_command_count != 1u || write_command_count != 0u)
		return 6;

	configuration.write_policy = ATA_WRITE_POLICY_READ_ONLY;
	if (ata_device_construct(&device, &configuration) != ATA_OK)
		return 7;
	reset_emulated_controller(false);
	if (ata_device_initialize(&device) != ATA_OK)
		return 8;
	info = ata_device_get_info(&device);
	if (info == NULL || info->state != ATA_DEVICE_READY ||
	    info->write_sector_enabled != 0u ||
	    ata_device_write_sector(&device, 0u, &sector) !=
		    ATA_ERR_READ_ONLY ||
	    identify_command_count != 1u || write_command_count != 0u)
		return 9;

	configuration.write_policy = ATA_WRITE_POLICY_ALLOW;
	if (ata_device_construct(&device, &configuration) != ATA_OK)
		return 10;
	reset_emulated_controller(true);
	if (ata_device_initialize(&device) != ATA_ERR_UNSUPPORTED)
		return 11;
	info = ata_device_get_info(&device);
	if (info == NULL || info->state != ATA_DEVICE_UNSUPPORTED ||
	    info->write_sector_enabled != 0u ||
	    identify_command_count != 1u || write_command_count != 0u)
		return 12;

	device.lifecycle_cookie ^= 1u;
	if (ata_device_configuration(&device) != NULL ||
	    ata_device_get_info(&device) != NULL)
		return 13;
	return 0;
}

DOSC32_TEST_ENTRY(run_tests)

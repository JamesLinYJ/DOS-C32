// SPDX-License-Identifier: GPL-2.0-only
/*
 * Instance-based ATA PIO backend for the DOS-C32 legacy BIOS target.
 *
 * Bounded polling and explicit task-file state transitions protect each
 * transfer. Device policy and the compact synchronous interface are
 * DOS-C32-specific: IDENTIFY is mandatory, sectors
 * are exactly 512 bytes, and only explicitly bounded LBA28 I/O is accepted.
 */
#include "ata.h"

#include "io.h"

#define ATA_DATA_OFFSET 0u
#define ATA_ERROR_FEATURES_OFFSET 1u
#define ATA_SECTOR_COUNT_OFFSET 2u
#define ATA_LBA_LOW_OFFSET 3u
#define ATA_LBA_MID_OFFSET 4u
#define ATA_LBA_HIGH_OFFSET 5u
#define ATA_DRIVE_OFFSET 6u
#define ATA_STATUS_COMMAND_OFFSET 7u

#define ATA_STATUS_ERROR 0x01u
#define ATA_STATUS_DRQ 0x08u
#define ATA_STATUS_DEVICE_FAULT 0x20u
#define ATA_STATUS_BUSY 0x80u

#define ATA_COMMAND_READ 0x20u
#define ATA_COMMAND_WRITE 0x30u
#define ATA_COMMAND_CACHE_FLUSH 0xe7u
#define ATA_COMMAND_IDENTIFY 0xecu

#define ATA_DRIVE_BASE 0xa0u
#define ATA_DRIVE_LBA_BASE 0xe0u
#define ATA_DRIVE_DEVICE_SHIFT 4u

#define ATA_IDENTIFY_GENERAL_CONFIGURATION 0u
#define ATA_IDENTIFY_CAPABILITIES 49u
#define ATA_IDENTIFY_LBA28_LOW 60u
#define ATA_IDENTIFY_LBA28_HIGH 61u
#define ATA_IDENTIFY_COMMAND_SET_2 83u
#define ATA_IDENTIFY_SECTOR_SIZE 106u
#define ATA_IDENTIFY_LOGICAL_WORDS_LOW 117u
#define ATA_IDENTIFY_LOGICAL_WORDS_HIGH 118u

#define ATA_IDENTIFY_PACKET_DEVICE 0x8000u
#define ATA_IDENTIFY_LBA_SUPPORTED 0x0200u
#define ATA_IDENTIFY_WORD_VALID 0x4000u
#define ATA_IDENTIFY_WORD_VALID_MASK 0xc000u
#define ATA_IDENTIFY_FLUSH_SUPPORTED 0x1000u
#define ATA_IDENTIFY_LONG_LOGICAL_SECTOR 0x1000u

typedef enum {
	ATA_WAIT_IDLE = 0,
	ATA_WAIT_DATA,
	ATA_WAIT_COMPLETE
} ata_wait_state_t;

static uint16_t ata_task_file_port(const struct ata_device *device,
	uint16_t offset)
{
	return (uint16_t)(device->configuration.command_port + offset);
}

static uint8_t ata_drive_selector(const struct ata_device *device,
	bool lba_mode)
{
	uint8_t selector = lba_mode ? ATA_DRIVE_LBA_BASE : ATA_DRIVE_BASE;

	return (uint8_t)(selector |
		(device->configuration.device << ATA_DRIVE_DEVICE_SHIFT));
}

static bool ata_status_is_missing(uint8_t status)
{
	return status == 0x00u || status == 0xffu;
}

static void ata_400ns_delay(const struct ata_device *device)
{
	(void)inb(device->configuration.control_port);
	(void)inb(device->configuration.control_port);
	(void)inb(device->configuration.control_port);
	(void)inb(device->configuration.control_port);
}

static void ata_record_status(struct ata_device *device, uint8_t status)
{
	device->info.last_status = status;
	if (!ata_status_is_missing(status) &&
	    (status & ATA_STATUS_ERROR) != 0u &&
	    (status & ATA_STATUS_BUSY) == 0u)
		device->info.last_error = inb(ata_task_file_port(
			device, ATA_ERROR_FEATURES_OFFSET));
}

static ata_result_t ata_wait(struct ata_device *device,
	ata_wait_state_t wanted)
{
	uint32_t remaining;
	uint8_t status = 0xffu;

	for (remaining = device->configuration.poll_limit;
	     remaining != 0u; --remaining) {
		status = inb(device->configuration.control_port);
		if (ata_status_is_missing(status)) {
			ata_record_status(device, status);
			if (device->info.state == ATA_DEVICE_READY) {
				device->info.write_sector_enabled = 0u;
				device->info.state = ATA_DEVICE_ABSENT;
			}
			return ATA_ERR_NO_DEVICE;
		}

		if ((status & ATA_STATUS_BUSY) != 0u)
			continue;

		/* An old ERR bit must not prevent a fresh command at idle. */
		if (wanted == ATA_WAIT_IDLE) {
			if ((status & ATA_STATUS_DRQ) == 0u) {
				ata_record_status(device, status);
				return ATA_OK;
			}
			continue;
		}

		if ((status & (ATA_STATUS_ERROR | ATA_STATUS_DEVICE_FAULT)) != 0u) {
			ata_record_status(device, status);
			return ATA_ERR_DEVICE;
		}

		if (wanted == ATA_WAIT_DATA) {
			if ((status & ATA_STATUS_DRQ) != 0u) {
				ata_record_status(device, status);
				return ATA_OK;
			}
		} else if ((status & ATA_STATUS_DRQ) == 0u) {
			ata_record_status(device, status);
			return ATA_OK;
		}
	}

	ata_record_status(device, status);
	if (wanted == ATA_WAIT_DATA &&
	    (status & ATA_STATUS_BUSY) == 0u &&
	    (status & ATA_STATUS_DRQ) == 0u)
		return ATA_ERR_NO_DRQ;
	if ((wanted == ATA_WAIT_IDLE || wanted == ATA_WAIT_COMPLETE) &&
	    (status & ATA_STATUS_BUSY) == 0u &&
	    (status & ATA_STATUS_DRQ) != 0u)
		return ATA_ERR_PROTOCOL;
	return ATA_ERR_TIMEOUT;
}

static void ata_select_device(struct ata_device *device, bool lba_mode,
	uint8_t lba_high)
{
	uint8_t selector = ata_drive_selector(device, lba_mode);

	if (lba_mode)
		selector = (uint8_t)(selector | (lba_high & 0x0fu));
	outb(ata_task_file_port(device, ATA_DRIVE_OFFSET), selector);
	ata_400ns_delay(device);
}

static ata_result_t ata_issue_command(struct ata_device *device,
	uint8_t command, ata_wait_state_t completion)
{
	ata_result_t result;

	result = ata_wait(device, ATA_WAIT_IDLE);
	if (result != ATA_OK)
		return result;

	device->info.last_error = 0u;
	outb(ata_task_file_port(device, ATA_STATUS_COMMAND_OFFSET), command);
	ata_400ns_delay(device);
	return ata_wait(device, completion);
}

static ata_result_t ata_validate_identify(struct ata_device *device,
	const union ata_sector *identify)
{
	uint16_t sector_size_word;
	uint32_t logical_words = ATA_SECTOR_BYTES / sizeof(uint16_t);
	uint32_t reported_sectors;
	block_lba_t usable_sectors;
	uint16_t command_set;

	if ((identify->words[ATA_IDENTIFY_GENERAL_CONFIGURATION] &
	     ATA_IDENTIFY_PACKET_DEVICE) != 0u)
		return ATA_ERR_UNSUPPORTED;

	if ((identify->words[ATA_IDENTIFY_CAPABILITIES] &
	     ATA_IDENTIFY_LBA_SUPPORTED) == 0u)
		return ATA_ERR_UNSUPPORTED;

	sector_size_word = identify->words[ATA_IDENTIFY_SECTOR_SIZE];
	if ((sector_size_word & ATA_IDENTIFY_WORD_VALID_MASK) ==
	    ATA_IDENTIFY_WORD_VALID &&
	    (sector_size_word & ATA_IDENTIFY_LONG_LOGICAL_SECTOR) != 0u) {
		logical_words =
			(uint32_t)identify->words[ATA_IDENTIFY_LOGICAL_WORDS_LOW] |
			((uint32_t)identify->words[ATA_IDENTIFY_LOGICAL_WORDS_HIGH]
			 << 16);
	}
	if (logical_words != ATA_SECTOR_BYTES / sizeof(uint16_t))
		return ATA_ERR_UNSUPPORTED;

	reported_sectors =
		(uint32_t)identify->words[ATA_IDENTIFY_LBA28_LOW] |
		((uint32_t)identify->words[ATA_IDENTIFY_LBA28_HIGH] << 16);
	if (reported_sectors == 0u)
		return ATA_ERR_UNSUPPORTED;

	usable_sectors = reported_sectors;
	if (usable_sectors > ATA_LBA28_SECTOR_COUNT)
		usable_sectors = ATA_LBA28_SECTOR_COUNT;

	command_set = identify->words[ATA_IDENTIFY_COMMAND_SET_2];
	device->info.lba28_supported = true;
	device->info.logical_sector_bytes = ATA_SECTOR_BYTES;
	device->info.sector_count = usable_sectors;
	device->info.flush_cache_supported =
		(command_set & ATA_IDENTIFY_WORD_VALID_MASK) ==
		ATA_IDENTIFY_WORD_VALID &&
		(command_set & ATA_IDENTIFY_FLUSH_SUPPORTED) != 0u;
	return ATA_OK;
}

static ata_result_t ata_result_for_state(const struct ata_device *device)
{
	switch (device->info.state) {
	case ATA_DEVICE_READY:
		return ATA_OK;
	case ATA_DEVICE_ABSENT:
		return ATA_ERR_NO_DEVICE;
	case ATA_DEVICE_UNSUPPORTED:
		return ATA_ERR_UNSUPPORTED;
	case ATA_DEVICE_FAULTED:
		return ATA_ERR_DEVICE;
	case ATA_DEVICE_PROBING:
	case ATA_DEVICE_UNINITIALIZED:
	default:
		return ATA_ERR_NOT_INITIALIZED;
	}
}

ata_result_t ata_device_initialize(struct ata_device *device)
{
	union ata_sector identify;
	ata_result_t result;
	uint8_t signature_mid;
	uint8_t signature_high;
	size_t index;

	if (ata_device_configuration(device) == NULL)
		return ATA_ERR_NOT_INITIALIZED;
	if (device->info.state == ATA_DEVICE_READY)
		return ATA_OK;

	device->info.state = ATA_DEVICE_PROBING;
	device->info.sector_count = 0u;
	device->info.logical_sector_bytes = 0u;
	device->info.lba28_supported = false;
	device->info.flush_cache_supported = false;
	device->info.write_sector_enabled = 0u;
	device->info.last_status = 0u;
	device->info.last_error = 0u;

	ata_select_device(device, false, 0u);
	result = ata_wait(device, ATA_WAIT_IDLE);
	if (result != ATA_OK)
		goto probe_failed;

	/* IDENTIFY DEVICE requires a zeroed non-data task file. */
	outb(ata_task_file_port(device, ATA_SECTOR_COUNT_OFFSET), 0u);
	outb(ata_task_file_port(device, ATA_LBA_LOW_OFFSET), 0u);
	outb(ata_task_file_port(device, ATA_LBA_MID_OFFSET), 0u);
	outb(ata_task_file_port(device, ATA_LBA_HIGH_OFFSET), 0u);
	result = ata_issue_command(device, ATA_COMMAND_IDENTIFY, ATA_WAIT_DATA);
	if (result != ATA_OK) {
		/* ATAPI signatures are not ATA disks and are deliberately rejected. */
		if (result != ATA_ERR_NO_DEVICE) {
			signature_mid = inb(ata_task_file_port(
				device, ATA_LBA_MID_OFFSET));
			signature_high = inb(ata_task_file_port(
				device, ATA_LBA_HIGH_OFFSET));
			if (signature_mid != 0u || signature_high != 0u)
				result = ATA_ERR_UNSUPPORTED;
		}
		goto probe_failed;
	}

	signature_mid = inb(ata_task_file_port(device, ATA_LBA_MID_OFFSET));
	signature_high = inb(ata_task_file_port(device, ATA_LBA_HIGH_OFFSET));
	if (signature_mid != 0u || signature_high != 0u) {
		result = ATA_ERR_UNSUPPORTED;
		goto probe_failed;
	}

	for (index = 0; index < ARRAY_SIZE(identify.words); ++index)
		identify.words[index] = inw(ata_task_file_port(
			device, ATA_DATA_OFFSET));
	ata_400ns_delay(device);
	result = ata_wait(device, ATA_WAIT_COMPLETE);
	if (result != ATA_OK)
		goto probe_failed;

	result = ata_validate_identify(device, &identify);
	if (result != ATA_OK)
		goto probe_failed;

	/*
	 * IDENTIFY has now proved an ordinary LBA ATA disk.  This capability
	 * combines that fact with platform authorization; it is not a media
	 * write-protect probe.
	 */
	device->info.write_sector_enabled =
		device->configuration.write_policy == ATA_WRITE_POLICY_ALLOW
			? 1u
			: 0u;
	device->info.state = ATA_DEVICE_READY;
	return ATA_OK;

probe_failed:
	if (result == ATA_ERR_NO_DEVICE)
		device->info.state = ATA_DEVICE_ABSENT;
	else if (result == ATA_ERR_UNSUPPORTED)
		device->info.state = ATA_DEVICE_UNSUPPORTED;
	else
		device->info.state = ATA_DEVICE_FAULTED;
	return result;
}

static ata_result_t ata_ensure_ready(struct ata_device *device)
{
	if (ata_device_configuration(device) == NULL)
		return ATA_ERR_NOT_INITIALIZED;
	if (device->info.state == ATA_DEVICE_UNINITIALIZED)
		return ata_device_initialize(device);
	return ata_result_for_state(device);
}

static ata_result_t ata_prepare_lba(struct ata_device *device,
	block_lba_t lba)
{
	ata_result_t result;

	result = ata_ensure_ready(device);
	if (result != ATA_OK)
		return result;
	if (lba > ATA_LBA28_MAX || lba >= device->info.sector_count)
		return ATA_ERR_OUT_OF_RANGE;

	result = ata_wait(device, ATA_WAIT_IDLE);
	if (result != ATA_OK)
		return result;

	ata_select_device(device, true, (uint8_t)(lba >> 24));
	result = ata_wait(device, ATA_WAIT_IDLE);
	if (result != ATA_OK)
		return result;

	outb(ata_task_file_port(device, ATA_SECTOR_COUNT_OFFSET), 1u);
	outb(ata_task_file_port(device, ATA_LBA_LOW_OFFSET), (uint8_t)lba);
	outb(ata_task_file_port(device, ATA_LBA_MID_OFFSET),
	     (uint8_t)(lba >> 8));
	outb(ata_task_file_port(device, ATA_LBA_HIGH_OFFSET),
	     (uint8_t)(lba >> 16));
	return ATA_OK;
}

ata_result_t ata_device_read_sector(struct ata_device *device,
	block_lba_t lba, union ata_sector *sector)
{
	ata_result_t result;
	size_t index;

	if (sector == NULL)
		return ATA_ERR_ARGUMENT;

	result = ata_prepare_lba(device, lba);
	if (result != ATA_OK)
		return result;
	result = ata_issue_command(device, ATA_COMMAND_READ, ATA_WAIT_DATA);
	if (result != ATA_OK)
		return result;

	for (index = 0; index < ARRAY_SIZE(sector->words); ++index)
		sector->words[index] = inw(ata_task_file_port(
			device, ATA_DATA_OFFSET));
	ata_400ns_delay(device);
	return ata_wait(device, ATA_WAIT_COMPLETE);
}

static ata_result_t ata_flush_cache(struct ata_device *device)
{
	if (!device->info.flush_cache_supported)
		return ATA_OK;
	return ata_issue_command(device, ATA_COMMAND_CACHE_FLUSH,
		ATA_WAIT_COMPLETE);
}

ata_result_t ata_device_write_sector(struct ata_device *device,
	block_lba_t lba, const union ata_sector *sector)
{
	ata_result_t result;
	size_t index;

	if (sector == NULL)
		return ATA_ERR_ARGUMENT;

	result = ata_ensure_ready(device);
	if (result != ATA_OK)
		return result;
	if (device->info.write_sector_enabled != 1u)
		return ATA_ERR_READ_ONLY;

	result = ata_prepare_lba(device, lba);
	if (result != ATA_OK)
		return result;
	result = ata_issue_command(device, ATA_COMMAND_WRITE, ATA_WAIT_DATA);
	if (result != ATA_OK)
		return result;

	for (index = 0; index < ARRAY_SIZE(sector->words); ++index)
		outw(ata_task_file_port(device, ATA_DATA_OFFSET),
		     sector->words[index]);
	ata_400ns_delay(device);
	result = ata_wait(device, ATA_WAIT_COMPLETE);
	if (result != ATA_OK)
		return result;

	/* Never flush before WRITE SECTORS has reached command completion. */
	return ata_flush_cache(device);
}

const char *ata_result_string(ata_result_t result)
{
	switch (result) {
	case ATA_OK:
		return "ok";
	case ATA_ERR_ARGUMENT:
		return "invalid argument";
	case ATA_ERR_NOT_INITIALIZED:
		return "device not initialized";
	case ATA_ERR_NO_DEVICE:
		return "no ATA device";
	case ATA_ERR_UNSUPPORTED:
		return "unsupported ATA device";
	case ATA_ERR_OUT_OF_RANGE:
		return "LBA outside device or LBA28 range";
	case ATA_ERR_READ_ONLY:
		return "ATA writes disabled by platform policy";
	case ATA_ERR_TIMEOUT:
		return "device timeout";
	case ATA_ERR_DEVICE:
		return "device error";
	case ATA_ERR_NO_DRQ:
		return "data request timeout";
	case ATA_ERR_PROTOCOL:
		return "ATA protocol error";
	default:
		return "unknown ATA error";
	}
}

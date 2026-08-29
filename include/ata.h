/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DOSC32_ATA_H
#define DOSC32_ATA_H

#include "address.h"
#include "compiler.h"
#include "types.h"

#define ATA_SECTOR_BYTES 512u
#define ATA_LBA28_SECTOR_COUNT (((block_lba_t)1u) << 28)
#define ATA_LBA28_MAX (ATA_LBA28_SECTOR_COUNT - (block_lba_t)1u)

typedef uint8_t ata_write_policy_t;

/* Authorization values; neither value reports sensed media protection. */
enum ata_write_policy {
	ATA_WRITE_POLICY_READ_ONLY = 0,
	ATA_WRITE_POLICY_ALLOW = 1
};

struct ata_device_configuration {
	uint32_t poll_limit;
	uint16_t command_port;
	uint16_t control_port;
	uint8_t device;
	ata_write_policy_t write_policy;
	uint8_t reserved[6];
} __aligned(8);

union ata_sector {
	uint8_t bytes[ATA_SECTOR_BYTES];
	uint16_t words[ATA_SECTOR_BYTES / sizeof(uint16_t)];
};

enum ata_device_state {
	ATA_DEVICE_UNINITIALIZED = 0,
	ATA_DEVICE_PROBING,
	ATA_DEVICE_ABSENT,
	ATA_DEVICE_UNSUPPORTED,
	ATA_DEVICE_READY,
	ATA_DEVICE_FAULTED
};

typedef uint8_t ata_device_state_t;

typedef enum {
	ATA_OK = 0,
	ATA_ERR_ARGUMENT,
	ATA_ERR_NOT_INITIALIZED,
	ATA_ERR_NO_DEVICE,
	ATA_ERR_UNSUPPORTED,
	ATA_ERR_OUT_OF_RANGE,
	ATA_ERR_READ_ONLY,
	ATA_ERR_TIMEOUT,
	ATA_ERR_DEVICE,
	ATA_ERR_NO_DRQ,
	ATA_ERR_PROTOCOL
} ata_result_t;

struct ata_device_info {
	block_lba_t sector_count;
	uint32_t logical_sector_bytes;
	ata_device_state_t state;
	uint8_t lba28_supported;
	uint8_t flush_cache_supported;
	/* Platform authorization after non-packet IDENTIFY, not media sensing. */
	uint8_t write_sector_enabled;
	uint8_t last_status;
	uint8_t last_error;
	uint8_t reserved[6];
} __aligned(8);

struct ata_device {
	struct ata_device_configuration configuration;
	struct ata_device_info info;
	uint32_t lifecycle_cookie;
	uint32_t reserved;
} __aligned(8);

/* Construction validates instance data but performs no hardware access. */
__must_check ata_result_t ata_device_construct(
	struct ata_device *device,
	const struct ata_device_configuration *configuration);
const struct ata_device_configuration *ata_device_configuration(
	const struct ata_device *device);
/* Read/write initialize the explicitly constructed device lazily. */
__must_check ata_result_t ata_device_initialize(struct ata_device *device);
const struct ata_device_info *ata_device_get_info(
	const struct ata_device *device);
__must_check ata_result_t ata_device_read_sector(struct ata_device *device,
	block_lba_t lba, union ata_sector *sector);
__must_check ata_result_t ata_device_write_sector(struct ata_device *device,
	block_lba_t lba, const union ata_sector *sector);
const char *ata_result_string(ata_result_t result);

_Static_assert(sizeof(union ata_sector) == ATA_SECTOR_BYTES,
	"ATA sector transfer object must be exactly 512 bytes");
_Static_assert(sizeof(((struct ata_device_info *)0)->sector_count) == 8,
	"ATA capacity must use the stable 64-bit block-LBA model");
_Static_assert(sizeof(ata_write_policy_t) == 1u,
	"ATA write policy must remain an explicit byte");
_Static_assert(sizeof(struct ata_device_configuration) == 16u,
	"ATA device configuration layout changed");
_Static_assert(_Alignof(struct ata_device_configuration) == 8u,
	"ATA device configuration alignment changed");
_Static_assert(__builtin_offsetof(struct ata_device_configuration,
				  write_policy) == 9u,
	"ATA device write-policy offset changed");
_Static_assert(sizeof(struct ata_device_info) == 24u,
	"ATA published device-info layout changed");
_Static_assert(_Alignof(struct ata_device_info) == 8u,
	"ATA published device-info alignment changed");
_Static_assert(__builtin_offsetof(struct ata_device_info,
				  write_sector_enabled) == 15u,
	"ATA write capability offset changed");
_Static_assert(sizeof(struct ata_device) == 48u,
	"ATA device object layout changed");
_Static_assert(_Alignof(struct ata_device) == 8u,
	"ATA device object alignment changed");

#endif

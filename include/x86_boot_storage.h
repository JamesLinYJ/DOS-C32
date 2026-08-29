/* SPDX-License-Identifier: GPL-2.0-only */
/* Validated legacy-BIOS boot-device identity for native block drivers. */
#ifndef DOSC32_X86_BOOT_STORAGE_H
#define DOSC32_X86_BOOT_STORAGE_H

#include "compiler.h"
#include "types.h"
#include "x86_boot_info.h"

#define X86_BOOT_DEVICE_LOCATOR_EDD_PATH (1u << 0)
#define X86_BOOT_DEVICE_LOCATOR_DPTE (1u << 1)
#define X86_BOOT_DEVICE_LOCATOR_FLAG_MASK                              \
	(X86_BOOT_DEVICE_LOCATOR_EDD_PATH | X86_BOOT_DEVICE_LOCATOR_DPTE)

enum x86_boot_storage_status {
	X86_BOOT_STORAGE_OK = 0,
	X86_BOOT_STORAGE_INVALID_ARGUMENT,
	X86_BOOT_STORAGE_NOT_AVAILABLE,
	X86_BOOT_STORAGE_CORRUPT,
	X86_BOOT_STORAGE_UNSUPPORTED
};

enum x86_boot_host_bus {
	X86_BOOT_HOST_BUS_UNKNOWN = 0,
	X86_BOOT_HOST_BUS_ISA,
	X86_BOOT_HOST_BUS_PCI
};

struct x86_boot_device_locator {
	uint64_t sector_count;
	uint64_t boot_volume_first_lba;
	uint64_t boot_volume_sector_count;
	uint32_t flags;
	uint16_t command_port;
	uint16_t control_port;
	uint16_t logical_sector_bytes;
	uint16_t edd_interface_support;
	uint8_t bios_drive;
	uint8_t edd_version;
	uint8_t ata_device;
	uint8_t host_bus;
	uint8_t pci_bus;
	uint8_t pci_slot;
	uint8_t pci_function;
	uint8_t ata_channel;
	uint8_t reserved[4];
} __aligned(8);

struct x86_ata_device_identity {
	uint64_t sector_count;
	uint16_t command_port;
	uint16_t control_port;
	uint16_t logical_sector_bytes;
	uint8_t device;
	uint8_t reserved;
} __aligned(8);

enum x86_boot_storage_status x86_boot_storage_decode(
	const struct x86_boot_info *boot_info, uint8_t expected_boot_drive,
	struct x86_boot_device_locator *locator) __must_check;

bool x86_boot_device_locator_matches_ata(
	const struct x86_boot_device_locator *locator,
	const struct x86_ata_device_identity *identity) __must_check;

static_assert_expression(sizeof(struct x86_boot_device_locator) == 48u,
			 "x86 boot-device locator layout changed");
static_assert_expression(__alignof__(struct x86_boot_device_locator) == 8u,
			 "x86 boot-device locator alignment changed");
static_assert_expression(sizeof(struct x86_ata_device_identity) == 16u,
			 "x86 ATA identity layout changed");
static_assert_expression(__alignof__(struct x86_ata_device_identity) == 8u,
			 "x86 ATA identity alignment changed");

#endif

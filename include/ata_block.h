/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DOSC32_ATA_BLOCK_H
#define DOSC32_ATA_BLOCK_H

#include "ata.h"
#include "block_device.h"
#include "x86_boot_storage.h"

enum ata_block_status {
	ATA_BLOCK_OK = 0,
	ATA_BLOCK_INVALID_ARGUMENT,
	ATA_BLOCK_INVALID_STATE,
	ATA_BLOCK_STALE_IDENTITY,
	ATA_BLOCK_DEVICE_UNAVAILABLE,
	ATA_BLOCK_LOCATOR_MISMATCH,
	ATA_BLOCK_REGISTRY_ERROR
};

/*
 * Bind identity and explicit platform write authorization before publishing
 * any callback.  The policy is independent of firmware device discovery.
 */
enum ata_block_status ata_block_initialize(
	kernel_object_handle_t adapter_identity,
	uint32_t poll_limit,
	ata_write_policy_t write_policy) __must_check;

/* Resolve only a firmware locator that proves this backend owns the device. */
enum ata_block_status ata_block_resolve_boot_locator(
	kernel_object_handle_t adapter_identity,
	const struct x86_boot_device_locator *locator,
	block_device_handle_t *handle) __must_check;

#endif

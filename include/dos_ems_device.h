/* SPDX-License-Identifier: GPL-2.0-only */
/* I/O Manager publication of the DOS EMS manager's standard device name. */
#ifndef DOSC32_DOS_EMS_DEVICE_H
#define DOSC32_DOS_EMS_DEVICE_H

#include "compiler.h"
#include "dos_ems.h"
#include "iomgr_device.h"
#include "types.h"

struct dos_personality;

enum dos_ems_publication_status {
	DOS_EMS_PUBLICATION_READY = 0,
	DOS_EMS_PUBLICATION_CONFLICT,
	DOS_EMS_PUBLICATION_INVALID_ARGUMENT,
	DOS_EMS_PUBLICATION_MACHINE_FAULT,
	DOS_EMS_PUBLICATION_POISONED
};

/*
 * Registers the exact eight-byte name owned by personality->ems_config.
 * The endpoint intentionally has no read, write or private-control capability:
 * EMS/VCPI remains the public INT 67h ABI until a documented control contract
 * is implemented.  The generic INT 21h device bridge creates JFT/SFT entries.
 */
enum iomgr_status dos_ems_device_register(
	const struct dos_personality *personality,
	kernel_object_handle_t device_identity,
	kernel_object_handle_t device_context,
	iomgr_device_registration_handle_t *registration) __must_check;

/*
 * Prepares EMS off to the side, reserves the standard I/O Manager name, then
 * publishes INT 67h.  Duplicate-name and publish failures unregister the
 * device and leave personality unchanged.
 */
enum dos_ems_publication_status dos_ems_runtime_publish(
	struct dos_personality *personality,
	const struct dos_ems_page_ops *page_ops,
	kernel_object_handle_t page_context,
	const struct dos_ems_page_frame_binding *page_frame,
	const struct dos_vcpi_platform_ops *vcpi_ops,
	kernel_object_handle_t vcpi_context,
	const struct dos_ems_runtime_config *config,
	kernel_object_handle_t device_identity,
	kernel_object_handle_t device_context,
	iomgr_device_registration_handle_t *registration) __must_check;

#endif

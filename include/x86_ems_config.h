/* SPDX-License-Identifier: GPL-2.0-only */
/* Legacy-PC EMS policy candidates and validated page-frame lease resolver. */
#ifndef DOSC32_X86_EMS_CONFIG_H
#define DOSC32_X86_EMS_CONFIG_H

#include "dos_ems.h"

enum x86_ems_runtime_config_status {
	X86_EMS_RUNTIME_CONFIG_READY = 0,
	X86_EMS_RUNTIME_CONFIG_UNAVAILABLE,
	X86_EMS_RUNTIME_CONFIG_CONFLICT,
	X86_EMS_RUNTIME_CONFIG_INVALID_ARGUMENT,
	X86_EMS_RUNTIME_CONFIG_FAULT,
	X86_EMS_RUNTIME_CONFIG_POISONED
};

struct x86_ems_runtime_binding {
	struct dos_ems_runtime_config config;
	struct dos_ems_page_frame_binding page_frame;
	uint8_t acquired;
	uint8_t reserved[7];
} __aligned(8);

/* Returns the centralized policy candidate, never a hardware discovery fact. */
bool x86_ems_runtime_config_candidate(
	struct dos_ems_runtime_config *config) __must_check;

/*
 * The platform acquire callback validates the complete UMA range, current
 * page-table permissions, mapping support and device/resource conflicts before
 * returning a lease.  Output remains unchanged on every non-READY result.
 */
enum x86_ems_runtime_config_status x86_ems_runtime_config_resolve(
	const struct dos_ems_page_frame_ops *ops,
	kernel_object_handle_t context,
	struct x86_ems_runtime_binding *binding) __must_check;
enum x86_ems_runtime_config_status x86_ems_runtime_config_release(
	struct x86_ems_runtime_binding *binding) __must_check;

static_assert_expression(__alignof__(struct x86_ems_runtime_binding) == 8u,
			 "x86 EMS runtime binding alignment changed");

#endif

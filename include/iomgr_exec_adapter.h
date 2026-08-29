/* SPDX-License-Identifier: GPL-2.0-only */
/* Filesystem-neutral immutable reader for the common DOS EXEC lease. */
#ifndef DOSC32_IOMGR_EXEC_ADAPTER_H
#define DOSC32_IOMGR_EXEC_ADAPTER_H

#include "compiler.h"
#include "dos_exec_file_lease.h"
#include "iomgr.h"

enum iomgr_exec_adapter_status {
	IOMGR_EXEC_ADAPTER_READY = 0,
	IOMGR_EXEC_ADAPTER_INVALID_ARGUMENT,
	IOMGR_EXEC_ADAPTER_INVALID_STATE
};

enum iomgr_exec_adapter_status iomgr_exec_adapter_initialize(
	kernel_object_handle_t adapter_identity,
	kernel_object_handle_t adapter_context,
	iomgr_volume_handle_t volume) __must_check;
const struct dos_exec_file_lease_ops *iomgr_exec_adapter_ops(void) __must_check;
kernel_object_handle_t iomgr_exec_adapter_context(void) __must_check;

#endif

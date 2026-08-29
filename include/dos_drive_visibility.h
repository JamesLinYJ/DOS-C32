/* SPDX-License-Identifier: GPL-2.0-only */
/* Production GetVisDrv adapter for the DOS EXEC default-FCB result. */
#ifndef DOSC32_DOS_DRIVE_VISIBILITY_H
#define DOSC32_DOS_DRIVE_VISIBILITY_H

#include "compiler.h"
#include "dos_drive.h"
#include "dos_exec_transaction.h"
#include "types.h"

enum dos_drive_visibility_adapter_status {
	DOS_DRIVE_VISIBILITY_READY = 0,
	DOS_DRIVE_VISIBILITY_INVALID_ARGUMENT,
	DOS_DRIVE_VISIBILITY_INVALID_STATE
};

/* current_drive is zero-based (0=A:, 2=C:); visible bit zero denotes A:. */
enum dos_drive_visibility_adapter_status dos_drive_visibility_initialize(
	kernel_object_handle_t adapter_identity,
	kernel_object_handle_t adapter_context, uint8_t current_drive,
	uint32_t visible_drives) __must_check;
const struct dos_exec_drive_visibility_ops *dos_drive_visibility_ops(void)
	__must_check;
kernel_object_handle_t dos_drive_visibility_context(void) __must_check;

#endif

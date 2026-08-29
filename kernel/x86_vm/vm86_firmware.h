/* SPDX-License-Identifier: GPL-2.0-only */
/* Private firmware-shadow lifecycle adapter for direct VM86 execution. */
#ifndef DOSC32_X86_VM_VM86_FIRMWARE_H
#define DOSC32_X86_VM_VM86_FIRMWARE_H

#include "exec_backend.h"
#include "x86_guest_space.h"

enum dos_exec_backend_release_status x86_vm86_firmware_execution_release(
	kernel_object_handle_t machine_identity,
	const struct x86_guest_space_firmware_binding *binding) __must_check;

#endif

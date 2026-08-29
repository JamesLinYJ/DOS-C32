/* SPDX-License-Identifier: GPL-2.0-only */
/* Private page COW owner for legacy firmware runtime writes. */
#ifndef DOSC32_X86_VM_MEMORY_FIRMWARE_SHADOW_H
#define DOSC32_X86_VM_MEMORY_FIRMWARE_SHADOW_H

#include "x86_guest_space.h"

enum x86_firmware_shadow_status {
	X86_FIRMWARE_SHADOW_OK = 0,
	X86_FIRMWARE_SHADOW_INVALID_ARGUMENT,
	X86_FIRMWARE_SHADOW_INVALID_STATE,
	X86_FIRMWARE_SHADOW_MACHINE_MISMATCH,
	X86_FIRMWARE_SHADOW_STALE_BINDING,
	X86_FIRMWARE_SHADOW_NOT_APPLICABLE,
	X86_FIRMWARE_SHADOW_CAPACITY_EXHAUSTED,
	X86_FIRMWARE_SHADOW_NO_MEMORY,
	X86_FIRMWARE_SHADOW_RETRY,
	X86_FIRMWARE_SHADOW_PAGING_MISMATCH,
	X86_FIRMWARE_SHADOW_POISONED
};

enum x86_firmware_shadow_status x86_firmware_shadow_initialize(
	kernel_object_handle_t address_space_identity,
	kernel_object_handle_t machine_identity, uint64_t address_space_generation,
	const struct x86_paging_binding *paging) __must_check;
enum x86_firmware_shadow_status x86_firmware_shadow_execution_acquire(
	kernel_object_handle_t machine_identity,
	struct x86_guest_space_firmware_binding *binding) __must_check;
enum x86_firmware_shadow_status x86_firmware_shadow_execution_release(
	kernel_object_handle_t machine_identity,
	const struct x86_guest_space_firmware_binding *binding) __must_check;
enum x86_firmware_shadow_status x86_firmware_shadow_execution_quarantine(
	kernel_object_handle_t machine_identity,
	const struct x86_guest_space_firmware_binding *binding) __must_check;
enum x86_firmware_shadow_status x86_firmware_shadow_write_fault(
	kernel_object_handle_t machine_identity,
	const struct x86_guest_space_firmware_binding *binding,
	uint32_t page_fault_error, uint32_t fault_address) __must_check;
bool x86_firmware_shadow_translate(
	uint32_t address, bool writable,
	struct x86_paging_guest_translation *translation) __must_check;

#endif

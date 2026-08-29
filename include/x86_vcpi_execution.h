/* SPDX-License-Identifier: GPL-2.0-only */
/* Capability gate between VCPI ABI decoding and protected execution. */
#ifndef DOSC32_X86_VCPI_EXECUTION_H
#define DOSC32_X86_VCPI_EXECUTION_H

#include "compiler.h"
#include "dos_vcpi.h"
#include "types.h"

enum x86_vcpi_execution_status {
	X86_VCPI_EXECUTION_OK = 0,
	X86_VCPI_EXECUTION_INVALID_ARGUMENT,
	X86_VCPI_EXECUTION_INVALID_STATE,
	X86_VCPI_EXECUTION_UNAVAILABLE
};

/*
 * Boot-lifetime binding only.  It deliberately contains no fallback CR0,
 * identity low-page translation or placeholder mode switch.  VCPI may be
 * advertised only after a real backend supplies all three operations.
 *
 * Backend contract:
 * - low-page translation reflects the active guest mapping, including an
 *   EMS page frame; it is not an unconditional identity-map shortcut;
 * - read_virtual_cr0 reports the compatibility CPU's state, never host CR0;
 * - DE01 copies and validates the caller's page-table/GDT destinations before
 *   returning COMPLETED;
 * - DE0C validates a bounded copy of CR3, descriptor-table pointers,
 *   selectors and the target entry before committing any CPU-mode change.
 *   TRANSFERRED means the old interrupt frame will never be resumed.
 */
struct x86_vcpi_execution_binding {
	const struct dos_vcpi_platform_ops *ops;
	kernel_object_handle_t context;
	uint8_t initialized;
	uint8_t constructed;
	uint8_t reserved[6];
} __aligned(8);

void x86_vcpi_execution_construct(
	struct x86_vcpi_execution_binding *binding);
enum x86_vcpi_execution_status x86_vcpi_execution_bind(
	struct x86_vcpi_execution_binding *binding,
	const struct dos_vcpi_platform_ops *ops,
	kernel_object_handle_t context) __must_check;
enum x86_vcpi_execution_status x86_vcpi_execution_resolve(
	const struct x86_vcpi_execution_binding *binding,
	const struct dos_vcpi_platform_ops **ops,
	kernel_object_handle_t *context) __must_check;

static_assert_expression(
	__alignof__(struct x86_vcpi_execution_binding) == 8u,
	"VCPI execution binding alignment changed");

#endif

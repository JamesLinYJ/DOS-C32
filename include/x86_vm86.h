/* SPDX-License-Identifier: GPL-2.0-only */
/* DOS-C32-specific direct i386 VM86 execution backend. */
#ifndef DOSC32_X86_VM86_H
#define DOSC32_X86_VM86_H

#include "dos_machine.h"
#include "exec_backend.h"
#include "x86_runtime.h"
#include "x86_vm_config.h"

#ifndef CONFIG_BOOT_SELFTESTS
#define CONFIG_BOOT_SELFTESTS 0
#endif

enum x86_vm86_backend_status {
	X86_VM86_BACKEND_OK = 0,
	X86_VM86_BACKEND_INVALID_ARGUMENT,
	X86_VM86_BACKEND_INVALID_STATE
};

enum x86_vm86_interrupt_delivery_status {
	X86_VM86_INTERRUPT_INACTIVE = 0,
	X86_VM86_INTERRUPT_DEFERRED,
	X86_VM86_INTERRUPT_NONE,
	X86_VM86_INTERRUPT_DELIVERED,
	X86_VM86_INTERRUPT_SESSION_FAULT,
	X86_VM86_INTERRUPT_SYSTEM_FAULT
};

enum x86_vm86_backend_status x86_vm86_backend_initialize(
	kernel_object_handle_t adapter_identity,
	kernel_object_handle_t adapter_context) __must_check;
const struct dos_exec_backend_ops *x86_vm86_backend_ops(void) __must_check;
kernel_object_handle_t x86_vm86_backend_context(void) __must_check;

#if CONFIG_X86_VM_ACCEPTANCE_DIAGNOSTICS
/* Acceptance-only bounded receipts. They are absent from production images. */
bool x86_vm86_last_software_interrupt(
	struct dos_cpu_state *state, uint8_t *vector);
bool x86_vm86_recent_software_interrupt(
	uint32_t previous, struct dos_cpu_state *state, uint8_t *vector);
#endif

/* Claims one modeled PIC request only when the live frame can accept it. */
enum x86_vm86_interrupt_delivery_status
x86_vm86_deliver_pending_interrupt(
	struct x86_trap_frame *frame) __must_check;

/* Called only by the common exception dispatcher while its frame is live. */
bool x86_vm86_handle_trap(struct x86_trap_frame *frame);

#if CONFIG_BOOT_SELFTESTS
/* Reversible test-image path: session -> VM86 -> INT21 -> resume. */
bool x86_vm86_boot_self_test(
	kernel_object_handle_t session_table_identity,
	kernel_object_handle_t personality_identity,
	kernel_object_handle_t runtime_identity,
	kernel_object_handle_t memory_arena_identity);
#endif

#endif

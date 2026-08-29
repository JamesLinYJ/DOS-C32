/* SPDX-License-Identifier: GPL-2.0-only */
/* i386 descriptor tables and the fixed C/assembly exception boundary. */
#ifndef DOSC32_X86_RUNTIME_H
#define DOSC32_X86_RUNTIME_H

#include "compiler.h"
#include "types.h"

#ifndef CONFIG_BOOT_SELFTESTS
#define CONFIG_BOOT_SELFTESTS 0
#endif

#define X86_EXCEPTION_COUNT 32u

enum x86_exception_vector {
	X86_EXCEPTION_DEBUG = 1u,
	X86_EXCEPTION_BREAKPOINT = 3u,
	X86_EXCEPTION_GENERAL_PROTECTION = 13u,
	X86_EXCEPTION_PAGE_FAULT = 14u
};

/*
 * Exact stack prefix built by boot/x86_traps.S.  The CPU-owned return frame
 * starts at instruction_pointer.  A future VM86 tail follows flags when the
 * saved flags contain VM; it is deliberately not exposed as a native pointer.
 */
struct x86_trap_frame {
	uint32_t gs;
	uint32_t fs;
	uint32_t es;
	uint32_t ds;
	uint32_t edi;
	uint32_t esi;
	uint32_t ebp;
	uint32_t saved_stack_pointer;
	uint32_t ebx;
	uint32_t edx;
	uint32_t ecx;
	uint32_t eax;
	uint32_t vector;
	uint32_t error_code;
	uint32_t instruction_pointer;
	uint32_t code_segment;
	uint32_t flags;
};

/* Installs a high-memory GDT/TSS and the exception IDT with IF still clear. */
void x86_runtime_initialize(void);

/* Updates the ring-0 entry stack immediately before a VM86 IRET. */
bool x86_runtime_set_kernel_stack(uint32_t stack_pointer);

/* Assembly entry point; public only to make the C/assembly ABI explicit. */
void x86_trap_dispatch(struct x86_trap_frame *frame);

#if CONFIG_BOOT_SELFTESTS
/* Test-image-only proof that one exception can enter C and return. */
bool x86_runtime_breakpoint_self_test(void);
#endif

static_assert_expression(sizeof(struct x86_trap_frame) == 68u,
			 "i386 trap-frame prefix must remain fixed width");
static_assert_expression(
	__builtin_offsetof(struct x86_trap_frame, vector) == 48u,
	"i386 trap vector offset changed");
static_assert_expression(
	__builtin_offsetof(struct x86_trap_frame, instruction_pointer) == 56u,
	"i386 CPU return-frame offset changed");

#endif

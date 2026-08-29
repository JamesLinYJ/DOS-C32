/* SPDX-License-Identifier: GPL-2.0-only */

#define MULTIBOOT_HEADER_MAGIC 0x1badb002
#define MULTIBOOT_HEADER_FLAGS 0
#define QEMU_DEBUG_EXIT_PORT 0x00f4
#define TEST_STACK_BYTES 32768

.section .multiboot, "a"
.align 4
.long MULTIBOOT_HEADER_MAGIC
.long MULTIBOOT_HEADER_FLAGS
.long -(MULTIBOOT_HEADER_MAGIC + MULTIBOOT_HEADER_FLAGS)

.section .text
.code32
.global _start
.type _start, @function
.extern dos_ems_integration_test_run
_start:
	cli
	movl $test_stack_top, %esp
	xorl %ebp, %ebp
	call dos_ems_integration_test_run
	movw $QEMU_DEBUG_EXIT_PORT, %dx
	outl %eax, %dx
1:
	hlt
	jmp 1b
.size _start, .-_start

.section .bss
.align 16
test_stack:
	.skip TEST_STACK_BYTES
test_stack_top:

.section .note.GNU-stack, "", @progbits

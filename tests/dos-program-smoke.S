/* SPDX-License-Identifier: GPL-2.0-only */
/* Independent 16-bit COM acceptance program for the production EXEC path. */
.code16
.section .text
.global _start

_start:
    pushw %cs
    popw %es
    movw $0x0020, %bx
    movb $0x4a, %ah
    int $0x21
    jc failed

    movw $0x0001, %bx
    movb $0x48, %ah
    int $0x21
    jc failed
    movw %ax, %es
    movb $0x49, %ah
    int $0x21
    jc failed

	pushw %cs
	popw %ds
	movw $success_message, %dx
	movb $0x09, %ah
	int $0x21
    movw $0x4c00, %ax
    int $0x21

failed:
	pushw %cs
	popw %ds
	movw $failure_message, %dx
	movb $0x09, %ah
	int $0x21
    movw $0x4c01, %ax
    int $0x21

success_message:
	.ascii "DOS-C32 REAL COM OK\r\n$"
failure_message:
	.ascii "DOS-C32 REAL COM FAILED\r\n$"

/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Real-boot BIOS keyboard regression.
 *
 * The temporary INT 16h hook proves that the software interrupt traversed the
 * guest IVT before chaining to SeaBIOS.  SeaBIOS then waits for IRQ1 and
 * returns the architectural scan-code/ASCII pair.  A personality callback
 * that handles INT 16h without reflecting it through the IVT cannot pass.
 */
	.code16
	.section .text
	.globl _start

_start:
	pushw %cs
	popw %ds

	/* Save and validate the SeaBIOS INT 16h entry before installing the hook. */
	movw $0x3516, %ax
	int $0x21
	movw %bx, %cs:old_int16
	movw %es, %cs:old_int16 + 2
	testw %bx, %bx
	jz invalid_ivt
	movw %es, %ax
	cmpw $0xf000, %ax
	jne invalid_ivt

	pushw %cs
	popw %ds
	movw $int16_hook, %dx
	movw $0x2516, %ax
	int $0x21

	movw $ready_message, %dx
	call print_string

	/* AH=00 must block in the real BIOS path until monitor-injected IRQ1. */
	xorw %ax, %ax
	int $0x16
	movw %ax, %cs:key_value

	cmpb $1, %cs:hook_entered
	jne hook_not_entered
	cmpw $0x1e61, %cs:key_value
	jne wrong_key

	call restore_int16
	movw $pass_message, %dx
	call print_string
	movw %cs:key_value, %ax
	call print_hex16
	movw $newline, %dx
	call print_string
	movw $0x4c00, %ax
	int $0x21

hook_not_entered:
	call restore_int16
	movw $hook_failure_message, %dx
	call print_string
	movw $0x4c03, %ax
	int $0x21

wrong_key:
	call restore_int16
	movw $key_failure_message, %dx
	call print_string
	movw %cs:key_value, %ax
	call print_hex16
	movw $newline, %dx
	call print_string
	movw $0x4c04, %ax
	int $0x21

invalid_ivt:
	pushw %cs
	popw %ds
	movw $ivt_failure_message, %dx
	call print_string
	movw %cs:old_int16 + 2, %ax
	call print_hex16
	movb $':', %dl
	call print_char
	movw %cs:old_int16, %ax
	call print_hex16
	movw $newline, %dx
	call print_string
	movw $0x4c02, %ax
	int $0x21

/* Mark entry before SeaBIOS blocks, then chain with an interrupt-like frame. */
int16_hook:
	movb $1, %cs:hook_entered
	pushfw
	lcall *%cs:old_int16
	iret

restore_int16:
	pushw %ds
	movw %cs:old_int16 + 2, %ax
	movw %ax, %ds
	movw %cs:old_int16, %dx
	movw $0x2516, %ax
	int $0x21
	popw %ds
	ret

print_hex16:
	pushw %ax
	movb %ah, %al
	call print_hex8
	popw %ax
	/* fall through */
print_hex8:
	pushw %ax
	shrb $4, %al
	call print_nibble
	popw %ax
	andb $0x0f, %al
print_nibble:
	cmpb $10, %al
	jb decimal_digit
	addb $('A' - 10), %al
	jmp emit_digit
decimal_digit:
	addb $'0', %al
emit_digit:
	movb %al, %dl
	/* fall through */
print_char:
	pushw %ax
	movb $0x02, %ah
	int $0x21
	popw %ax
	ret

print_string:
	pushw %ax
	movb $0x09, %ah
	int $0x21
	popw %ax
	ret

	.p2align 1
old_int16:
	.word 0x0000, 0x0000
hook_entered:
	.byte 0x00
	.p2align 1
key_value:
	.word 0x0000

ready_message:
	.ascii "BIOS KEY IRQ READY\r\n$"
pass_message:
	.ascii "BIOS KEY IRQ PASS AX=$"
ivt_failure_message:
	.ascii "BIOS KEY IRQ FAIL IVT=$"
hook_failure_message:
	.ascii "BIOS KEY IRQ FAIL IVT HOOK\r\n$"
key_failure_message:
	.ascii "BIOS KEY IRQ FAIL AX=$"
newline:
	.ascii "\r\n$"

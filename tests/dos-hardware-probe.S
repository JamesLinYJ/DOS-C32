/* SPDX-License-Identifier: GPL-2.0-only */
	.code16
	.section .text
	.globl _start

_start:
	pushw %cs
	popw %ds
	movw $heading, %dx
	call print_string

	movb $0xc0, %ah
	int $0x15
	pushfw
	popw %si
	pushw %ax
	movw $int15_label, %dx
	call print_string
	popw %ax
	call print_hex16
	movw $flags_label, %dx
	call print_string
	movw %si, %ax
	call print_hex16
	movw $pointer_label, %dx
	call print_string
	movw %es, %ax
	call print_hex16
	movb $':', %dl
	call print_char
	movw %bx, %ax
	call print_hex16
	movw $newline, %dx
	call print_string

	testw $1, %si
	jnz done
	movw $bytes_label, %dx
	call print_string
	xorw %si, %si
byte_loop:
	movb %es:(%bx,%si), %al
	call print_hex8
	movb $' ', %dl
	call print_char
	incw %si
	cmpw $16, %si
	jb byte_loop
	movw $newline, %dx
	call print_string

done:
	/* Windows Setup and DOS timing loops read the BDA counter directly. */
	movw $0x0040, %ax
	movw %ax, %es
	movw %es:0x006c, %ax
timer_wait:
	cmpw %es:0x006c, %ax
	je timer_wait
	movw $timer_label, %dx
	call print_string

	movw $0xf000, %ax
	movw %ax, %es
	movw $rom_label, %dx
	call print_string
	movb %es:0xfffe, %al
	call print_hex8
	movw $newline, %dx
	call print_string
	movw $video_rom_label, %dx
	call print_string
	movw $0xc000, %ax
	movw %ax, %es
	xorw %si, %si
video_rom_loop:
	movb %es:(%si), %al
	call print_hex8
	movb $' ', %dl
	call print_char
	incw %si
	cmpw $32, %si
	jb video_rom_loop
	movw $newline, %dx
	call print_string
	movw $0x4c00, %ax
	int $0x21

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

heading:
	.ascii "DOS-C32 BIOS HARDWARE PROBE\r\n$"
int15_label:
	.ascii "INT15/C0 AX=$"
flags_label:
	.ascii " FLAGS=$"
pointer_label:
	.ascii " ES:BX=$"
bytes_label:
	.ascii "CONFIG BYTES=$"
rom_label:
	.ascii "ROM MODEL BYTE=$"
video_rom_label:
	.ascii "VIDEO ROM BYTES=$"
timer_label:
	.ascii "BIOS TIMER ADVANCED\r\n$"
newline:
	.ascii "\r\n$"

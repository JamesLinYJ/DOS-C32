/* SPDX-License-Identifier: GPL-2.0-only */
ENTRY(_start)

PHDRS
{
	text PT_LOAD FLAGS(5);
	data PT_LOAD FLAGS(6);
}

SECTIONS
{
	. = 0x00100000;
	.multiboot : { KEEP(*(.multiboot)) } :text
	.text : { *(.text*) } :text
	.rodata : { *(.rodata*) } :text
	. = ALIGN(0x1000);
	.data : { *(.data*) } :data
	.bss : { *(.bss*) *(COMMON) } :data
	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}

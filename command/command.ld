/* SPDX-License-Identifier: GPL-2.0-only */
OUTPUT_FORMAT(elf32-i386)
OUTPUT_ARCH(i386)
ENTRY(command_main)

SECTIONS
{
    .c32_header 0 :
    {
        LONG(0x58323343) /* C32X */
        SHORT(64)
        SHORT(1)
        LONG(0x00200000)
        LONG(64)
        LONG(__c32_file_end - __c32_image_base)
        LONG(__c32_memory_end - __c32_image_base)
        LONG(command_main - __c32_image_base)
        LONG(0x00400000)
        LONG(1)
        LONG(0)
        . = 64;
    }

    __c32_image_base = 0x00200000;
    .text __c32_image_base : AT(64)
    {
        KEEP(*(.text.entry))
        *(.text .text.*)
    }
    .rodata : AT(LOADADDR(.text) + (ADDR(.rodata) - ADDR(.text)))
    {
        *(.rodata .rodata.*)
    }
    .data : AT(LOADADDR(.text) + (ADDR(.data) - ADDR(.text)))
    {
        *(.data .data.*)
    }
    __c32_file_end = .;
    .bss (NOLOAD) :
    {
        *(.bss .bss.*)
        *(COMMON)
    }
    __c32_memory_end = .;

    ASSERT(__c32_memory_end <= 0x003e0000,
           "COMMAND exceeds the protected C32 image aperture")
    /DISCARD/ : { *(.comment) *(.eh_frame*) *(.note*) }
}

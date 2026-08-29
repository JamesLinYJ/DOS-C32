/* SPDX-License-Identifier: GPL-2.0-only */
OUTPUT_FORMAT(binary)
OUTPUT_ARCH(i386)
ENTRY(io_start)

SECTIONS
{
    . = 0x8000;
    .io_loader :
    {
        KEEP(*(.io_loader))
    }

    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
        *(*)
    }
}

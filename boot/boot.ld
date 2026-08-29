/* SPDX-License-Identifier: GPL-2.0-only */
OUTPUT_FORMAT(binary)
OUTPUT_ARCH(i386)
ENTRY(boot_start)

SECTIONS
{
    . = __bios_boot_load_address;
    .boot :
    {
        KEEP(*(.boot))
    }

    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
        *(*)
    }
}

/* SPDX-License-Identifier: GPL-2.0-only */
OUTPUT_FORMAT(elf32-i386)
ENTRY(mbr_start)
SECTIONS
{
    . = 0x0600;
    .mbr : { *(.mbr) }
    /DISCARD/ : { *(*) }
}

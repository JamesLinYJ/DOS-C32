/* SPDX-License-Identifier: GPL-2.0-only */
OUTPUT_FORMAT(elf32-i386)
OUTPUT_ARCH(i386)
ENTRY(_start)

PHDRS
{
    bootstrap PT_LOAD FLAGS(5);  /* PF_R | PF_X */
    text PT_LOAD FLAGS(5);       /* PF_R | PF_X */
    data PT_LOAD FLAGS(6);       /* PF_R | PF_W */
    stack PT_GNU_STACK FLAGS(6); /* non-executable stack contract */
}

SECTIONS
{
    . = __kernel_load_base;
    __kernel_staging_start = .;

    .entry : ALIGN(16)
    {
        KEEP(*(.entry16))
    } :bootstrap

    __kernel_staging_entry_end = .;
    __kernel_staging_payload_start = ALIGN(__kernel_staging_entry_end, 16);
    __kernel_runtime_start = __kernel_runtime_base;
    __kernel_relocation_delta =
        __kernel_runtime_start - __kernel_staging_payload_start;

    . = __kernel_runtime_start;

    . = ALIGN(16);
    .text : AT(ADDR(.text) - __kernel_relocation_delta)
    {
        /* Ordinary DOS objects use one .text section and remain present even
         * before every dispatcher edge is wired.  Only explicitly split
         * library functions are eligible for --gc-sections. */
        KEEP(*(.text))
        KEEP(*(.text.hot .text.unlikely .text.startup .text.exit))
        *(.text.*)
    } :text

    . = ALIGN(16);
    .rodata : AT(ADDR(.rodata) - __kernel_relocation_delta)
    {
        *(.rodata .rodata.*)
    } :text

    /* Keep writable storage on a distinct future page-protection boundary. */
    . = ALIGN(4096);
    .data : AT(ADDR(.data) - __kernel_relocation_delta)
    {
        *(.data .data.*)
    } :data

    __kernel_runtime_file_end = .;
    __kernel_staging_file_end = LOADADDR(.data) + SIZEOF(.data);

    .bss : ALIGN(16)
    {
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        __bss_end = .;
    } :data

    __kernel_runtime_end = .;

    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}

ASSERT(__kernel_staging_file_end <= __kernel_load_limit,
       "kernel exceeds the configured BIOS loader window")
ASSERT((__kernel_runtime_start & 0xfff) == 0,
       "runtime kernel must be page aligned")
ASSERT(__kernel_runtime_start >= 0x00400000,
       "runtime kernel must leave the DOS and XMS apertures available")
ASSERT(__kernel_runtime_end < __kernel_stack_floor,
       "kernel and BSS overlap the protected-mode stack")
ASSERT(__kernel_stack_floor < __kernel_stack_top,
       "protected-mode stack bounds are reversed")

/* SPDX-License-Identifier: GPL-2.0-only */
/* The only post-boot assembly: enter i386 protected mode and call C. */
#include "x86_boot_info.h"

#ifndef KERNEL_LOAD_BASE
#error "KERNEL_LOAD_BASE must be supplied by the Legacy-BIOS configuration"
#endif
#ifndef KERNEL_STACK_TOP
#error "KERNEL_STACK_TOP must be supplied by the Legacy-BIOS configuration"
#endif
#ifndef KERNEL_STACK_FLOOR
#error "KERNEL_STACK_FLOOR must be supplied by the Legacy-BIOS configuration"
#endif
#ifndef KERNEL_RUNTIME_BASE
#error "KERNEL_RUNTIME_BASE must be supplied by the Legacy-BIOS configuration"
#endif
#ifndef CONFIG_X86_A20_ENABLE_ATTEMPTS
#error "A20 enable-attempt policy must be supplied by the Legacy-BIOS configuration"
#endif
#ifndef CONFIG_X86_A20_SHORT_TEST_LOOPS
#error "A20 short-test policy must be supplied by the Legacy-BIOS configuration"
#endif
#ifndef CONFIG_X86_A20_LONG_TEST_LOOPS
#error "A20 long-test policy must be supplied by the Legacy-BIOS configuration"
#endif
#ifndef CONFIG_X86_A20_KBC_POLL_LIMIT
#error "A20 KBC-poll policy must be supplied by the Legacy-BIOS configuration"
#endif
#ifndef CONFIG_X86_A20_KBC_ABSENT_SAMPLES
#error "A20 KBC-absence policy must be supplied by the Legacy-BIOS configuration"
#endif
#ifndef CONFIG_X86_STACK_GUARD_RDRAND_ATTEMPTS
#error "stack-guard RDRAND retry policy must be supplied by the Legacy-BIOS configuration"
#endif
#ifndef CONFIG_X86_BOOT_RTC_SNAPSHOT_ATTEMPTS
#error "RTC snapshot retry policy must be supplied by the Legacy-BIOS configuration"
#endif

#if (KERNEL_LOAD_BASE & 0x0f) != 0
#error "kernel load base must be paragraph aligned"
#endif
#if KERNEL_RUNTIME_BASE >= KERNEL_STACK_FLOOR || \
	KERNEL_STACK_FLOOR >= KERNEL_STACK_TOP
#error "kernel runtime and stack layout is invalid"
#endif
#if CONFIG_X86_A20_ENABLE_ATTEMPTS <= 0 || \
	CONFIG_X86_A20_SHORT_TEST_LOOPS <= 0 || \
	CONFIG_X86_A20_LONG_TEST_LOOPS < CONFIG_X86_A20_SHORT_TEST_LOOPS || \
	CONFIG_X86_A20_KBC_POLL_LIMIT <= 0 || \
	CONFIG_X86_A20_KBC_ABSENT_SAMPLES <= 0 || \
	CONFIG_X86_STACK_GUARD_RDRAND_ATTEMPTS <= 0 || \
	CONFIG_X86_BOOT_RTC_SNAPSHOT_ATTEMPTS <= 0
#error "A20 transition bounds must be positive and ordered"
#endif

#define KERNEL_LOAD_SEGMENT (KERNEL_LOAD_BASE >> 4)

#define X86_E820_FUNCTION 0x0000e820
#define X86_E820_SIGNATURE 0x534d4150
#define X86_E820_MINIMUM_BYTES 20
#define X86_E820_SCRATCH_ADDRESS                                      \
	(X86_BOOT_INFO_ADDRESS + X86_BOOT_INFO_HEADER_BYTES +          \
	 X86_BOOT_MEMORY_RANGE_COUNT * X86_BOOT_MEMORY_RANGE_BYTES)
#define X86_E820_STATE_ADDRESS                                        \
	(X86_E820_SCRATCH_ADDRESS + X86_BOOT_MEMORY_RANGE_BYTES)
#define X86_E820_STATE_COUNT_OFFSET 0
#define X86_E820_STATE_OVERFLOW_OFFSET 2
#define X86_E820_STATE_BYTES 4
#define X86_E820_CLEAR_BYTES                                          \
	(X86_BOOT_INFO_HEADER_BYTES +                                  \
	 X86_BOOT_MEMORY_RANGE_COUNT * X86_BOOT_MEMORY_RANGE_BYTES +   \
	 X86_BOOT_MEMORY_RANGE_BYTES + X86_E820_STATE_BYTES)

/* Architectural A20 alias pair used to detect address-line wrapping. */
#define X86_A20_TEST_OFFSET (4 * 0x80)
#define X86_A20_HIGH_ALIAS_OFFSET (X86_A20_TEST_OFFSET + 0x10)
#define X86_KBC_DATA_PORT 0x60
#define X86_KBC_STATUS_COMMAND_PORT 0x64
#define X86_KBC_OUTPUT_FULL (1 << 0)
#define X86_KBC_INPUT_FULL (1 << 1)
#define X86_KBC_WRITE_OUTPUT_PORT 0xd1
#define X86_KBC_A20_OUTPUT 0xdf
#define X86_KBC_NULL_COMMAND 0xff
#define X86_FAST_A20_PORT 0x92
#define X86_FAST_A20_ENABLE (1 << 1)
#define X86_FAST_A20_RESET (1 << 0)

/* The live VBR remains at its architectural Legacy-BIOS load address. */
#define X86_VBR_ADDRESS 0x00007c00
#define X86_FAT_BPB_SECTOR_BYTES_OFFSET 11
#define X86_FAT_BPB_TOTAL_SECTORS16_OFFSET 19
#define X86_FAT_BPB_HIDDEN_SECTORS_OFFSET 28
#define X86_FAT_BPB_TOTAL_SECTORS32_OFFSET 32
#define X86_NATIVE_SECTOR_BYTES 512

#define X86_BDA_TIMER_TICKS 0x0000046c
#define X86_EFLAGS_CPUID (1 << 21)
#define X86_CPUID_FEATURES 1
#define X86_CPUID_TSC (1 << 4)
#define X86_CPUID_RDRAND (1 << 30)
#define X86_STACK_GUARD_SOURCE_BOOT_STATE (1 << 0)
#define X86_STACK_GUARD_SOURCE_TSC (1 << 1)
#define X86_STACK_GUARD_SOURCE_RDRAND (1 << 2)

#if X86_BOOT_INFO_ADDRESS != 0x00005000
#error "legacy BIOS boot information must remain at physical 0x5000"
#endif
#if (X86_E820_CLEAR_BYTES & 1) != 0
#error "E820 bootstrap storage must be word aligned"
#endif
#if X86_BOOT_INFO_ADDRESS + X86_E820_CLEAR_BYTES > 0x00010000
#error "E820 bootstrap storage must fit in the first real-mode segment"
#endif

.set CODE_SELECTOR, 0x08
.set DATA_SELECTOR, 0x10
.set E820_CLEAR_WORDS, X86_E820_CLEAR_BYTES >> 1
.set E820_RANGE_WORDS, X86_BOOT_MEMORY_RANGE_BYTES >> 1
.set E820_PRIVATE_WORDS, \
	(X86_BOOT_MEMORY_RANGE_BYTES + X86_E820_STATE_BYTES) >> 1
.set E820_FAILURE_CLEAR_WORDS, \
	(X86_BOOT_MEMORY_RANGE_COUNT * X86_BOOT_MEMORY_RANGE_BYTES + \
	 X86_BOOT_MEMORY_RANGE_BYTES + X86_E820_STATE_BYTES) >> 1

.section .entry16, "ax", @progbits
.code16
.global _start
.global boot_drive
.extern kmain
.extern __bss_start
.extern __bss_end
.extern __kernel_staging_payload_start
.extern __kernel_runtime_start
.extern __kernel_runtime_file_end
.extern __stack_chk_guard
.extern __stack_chk_guard_source_mask

_start:
    cli
    cld
    movw $KERNEL_LOAD_SEGMENT, %ax
    movw %ax, %ds
    movw %ax, %es
    /* Both symbols share .entry16, so this is a relocation-free DS offset. */
    movb %dl, boot_drive - _start

    /*
     * Build the fixed low-memory BIOS handoff before enabling protected mode.
     * The header, all unused range slots, the overflow scratch range, and the
     * private enumeration state start cleared.  The flags remain zero until a
     * complete, non-truncated map has been collected and validated.
     */
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movw $X86_BOOT_INFO_ADDRESS, %di
    xorl %eax, %eax
    movw $E820_CLEAR_WORDS, %cx
    rep stosw

    movw $X86_BOOT_INFO_ADDRESS, %di
    movl $X86_BOOT_INFO_SIGNATURE, \
	  %es:X86_BOOT_INFO_SIGNATURE_OFFSET(%di)
    movw $X86_BOOT_INFO_VERSION, \
	  %es:X86_BOOT_INFO_VERSION_OFFSET(%di)
    movw $X86_BOOT_INFO_HEADER_BYTES, \
	  %es:X86_BOOT_INFO_HEADER_BYTES_OFFSET(%di)
    movw $X86_BOOT_MEMORY_RANGE_BYTES, \
	  %es:X86_BOOT_INFO_RANGE_BYTES_OFFSET(%di)

    /*
     * Capture the firmware's identity for the actual boot device before any
     * protected-mode driver is selected.  AH=41h proves that the drive offers
     * EDD, AH=48h supplies a byte-defined device path, and the DPTE provides
     * the independent ATA task-file selector used by the native backend.
     * Missing or incomplete identity is left unpublished; C validates every
     * length, checksum, selector, and cross-field relationship later.
     */
    movl $X86_BOOT_STORAGE_SIGNATURE, \
	  %es:X86_BOOT_INFO_STORAGE_OFFSET + \
	      X86_BOOT_STORAGE_SIGNATURE_OFFSET(%di)
    movw $X86_BOOT_STORAGE_VERSION, \
	  %es:X86_BOOT_INFO_STORAGE_OFFSET + \
	      X86_BOOT_STORAGE_VERSION_OFFSET(%di)
    movw $X86_BOOT_STORAGE_BYTES, \
	  %es:X86_BOOT_INFO_STORAGE_OFFSET + \
	      X86_BOOT_STORAGE_BYTES_OFFSET(%di)
    movb %dl, %es:X86_BOOT_INFO_STORAGE_OFFSET + \
	  X86_BOOT_STORAGE_BOOT_DRIVE_OFFSET(%di)

    /*
     * Publish the exact volume that supplied this running kernel.  Both loader
     * stages consumed the still-live VBR at 0000:7C00, so its BPB hidden LBA
     * and total-sector fields are boot-chain facts rather than a later MBR
     * guess.  The protected decoder checks this extent against EDD capacity.
     */
    cmpw $0xaa55, %es:X86_VBR_ADDRESS + 510
    jne .Lvbr_unavailable
    cmpw $X86_NATIVE_SECTOR_BYTES, \
	  %es:X86_VBR_ADDRESS + X86_FAT_BPB_SECTOR_BYTES_OFFSET
    jne .Lvbr_unavailable
    movzwl %es:X86_VBR_ADDRESS + \
	    X86_FAT_BPB_TOTAL_SECTORS16_OFFSET, %eax
    testl %eax, %eax
    jnz .Lvbr_sector_count_ready
    movl %es:X86_VBR_ADDRESS + X86_FAT_BPB_TOTAL_SECTORS32_OFFSET, %eax
.Lvbr_sector_count_ready:
    testl %eax, %eax
    jz .Lvbr_unavailable
    movl %es:X86_VBR_ADDRESS + X86_FAT_BPB_HIDDEN_SECTORS_OFFSET, %edx
    movl %edx, %ecx
    addl %eax, %ecx
    jc .Lvbr_unavailable
    movl %edx, %es:X86_BOOT_INFO_STORAGE_OFFSET + \
	  X86_BOOT_STORAGE_VOLUME_START_LBA_OFFSET(%di)
    movl $0, %es:X86_BOOT_INFO_STORAGE_OFFSET + \
	  X86_BOOT_STORAGE_VOLUME_START_LBA_OFFSET + 4(%di)
    movl %eax, %es:X86_BOOT_INFO_STORAGE_OFFSET + \
	  X86_BOOT_STORAGE_VOLUME_SECTOR_COUNT_OFFSET(%di)
    movl $0, %es:X86_BOOT_INFO_STORAGE_OFFSET + \
	  X86_BOOT_STORAGE_VOLUME_SECTOR_COUNT_OFFSET + 4(%di)
    movw $X86_NATIVE_SECTOR_BYTES, \
	  %es:X86_BOOT_INFO_STORAGE_OFFSET + \
	  X86_BOOT_STORAGE_VOLUME_SECTOR_BYTES_OFFSET(%di)
    orl $X86_BOOT_STORAGE_VOLUME_PRESENT, \
	 %es:X86_BOOT_INFO_STORAGE_OFFSET + X86_BOOT_STORAGE_FLAGS_OFFSET(%di)
.Lvbr_unavailable:

	/* EDX held the BPB hidden-sector LBA above, so reload the firmware drive
	 * identity before every INT 13h operation that consumes DL. */
	movb %es:X86_BOOT_INFO_STORAGE_OFFSET + \
	      X86_BOOT_STORAGE_BOOT_DRIVE_OFFSET(%di), %dl
	movw $0x55aa, %bx
	movb $0x41, %ah
    int $0x13
    jc .Ledd_extensions_unavailable
    cmpw $0xaa55, %bx
    jne .Ledd_extensions_unavailable
    testw $0x0001, %cx
    jz .Ledd_extensions_unavailable
    pushw %ax
    pushw %cx
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    popw %cx
    popw %ax
    movw $X86_BOOT_INFO_ADDRESS, %di
    movw %cx, %es:X86_BOOT_INFO_STORAGE_OFFSET + \
	  X86_BOOT_STORAGE_INTERFACE_SUPPORT_OFFSET(%di)
    movb %ah, %es:X86_BOOT_INFO_STORAGE_OFFSET + \
	  X86_BOOT_STORAGE_EDD_VERSION_OFFSET(%di)
    orl $X86_BOOT_STORAGE_EXTENSIONS_PRESENT, \
	 %es:X86_BOOT_INFO_STORAGE_OFFSET + \
	     X86_BOOT_STORAGE_FLAGS_OFFSET(%di)

    movw $(X86_BOOT_INFO_ADDRESS + X86_BOOT_INFO_STORAGE_OFFSET + \
	   X86_BOOT_STORAGE_PARAMETERS_OFFSET), %si
    movw $X86_BOOT_EDD_PARAMETERS_BYTES, \
	  X86_BOOT_EDD_PARAMETERS_LENGTH_OFFSET(%si)
    movb %es:X86_BOOT_INFO_STORAGE_OFFSET + \
	  X86_BOOT_STORAGE_BOOT_DRIVE_OFFSET(%di), %dl
    movb $0x48, %ah
    int $0x13
    jc .Ledd_parameters_unavailable
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    cld
    movw $X86_BOOT_INFO_ADDRESS, %di
    orl $X86_BOOT_STORAGE_PARAMETERS_PRESENT, \
	 %es:X86_BOOT_INFO_STORAGE_OFFSET + \
	     X86_BOOT_STORAGE_FLAGS_OFFSET(%di)

    movw %es:X86_BOOT_INFO_STORAGE_OFFSET + \
	  X86_BOOT_STORAGE_PARAMETERS_OFFSET + \
	  X86_BOOT_EDD_PARAMETERS_DPTE_POINTER_OFFSET(%di), %si
    movw %es:X86_BOOT_INFO_STORAGE_OFFSET + \
	  X86_BOOT_STORAGE_PARAMETERS_OFFSET + \
	  X86_BOOT_EDD_PARAMETERS_DPTE_POINTER_OFFSET + 2(%di), %bx
    movw %si, %ax
    orw %bx, %ax
    jz .Ledd_dpte_unavailable
    cmpw $0xffff, %si
    jne .Ledd_dpte_not_all_ones
    cmpw $0xffff, %bx
    je .Ledd_dpte_unavailable
.Ledd_dpte_not_all_ones:
    cmpw $0xfff0, %si
    ja .Ledd_dpte_unavailable
    movzwl %bx, %eax
    shll $4, %eax
    movzwl %si, %edx
    addl %edx, %eax
    cmpl $0x000ffff0, %eax
    ja .Ledd_dpte_unavailable

    movw %bx, %ds
    xorw %ax, %ax
    movw %ax, %es
    movw $(X86_BOOT_INFO_ADDRESS + X86_BOOT_INFO_STORAGE_OFFSET + \
	   X86_BOOT_STORAGE_DPTE_OFFSET), %di
    movw $(X86_BOOT_EDD_DPTE_BYTES >> 1), %cx
    cld
    rep movsw
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movw $X86_BOOT_INFO_ADDRESS, %di
    orl $X86_BOOT_STORAGE_DPTE_PRESENT, \
	 %es:X86_BOOT_INFO_STORAGE_OFFSET + \
	     X86_BOOT_STORAGE_FLAGS_OFFSET(%di)
    jmp .Ledd_done

.Ledd_extensions_unavailable:
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    cld
    jmp .Ledd_done

.Ledd_parameters_unavailable:
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    cld
    jmp .Ledd_done

.Ledd_dpte_unavailable:
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    cld

.Ledd_done:
	/* INT 10h/1Ah facts exist only while firmware services remain callable. */
	call .Lcapture_boot_platform

	xorl %ebx, %ebx
.Le820_next:
    /*
     * Some BIOSes update only fields that changed from the prior result and
     * require every call to reuse the same buffer.  Keep scratch contents
     * between calls, while restoring the requested extended-attribute value.
     * The scratch contents remain private until a complete record validates.
     */
    movw $X86_E820_SCRATCH_ADDRESS, %di
    movl $X86_BOOT_MEMORY_ENABLED, \
	  %es:X86_BOOT_MEMORY_RANGE_ATTRIBUTES_OFFSET(%di)

    movl $X86_E820_FUNCTION, %eax
    movl $X86_E820_SIGNATURE, %edx
    movl $X86_BOOT_MEMORY_RANGE_BYTES, %ecx
    int $0x15
    jc .Le820_failed
    cmpl $X86_E820_SIGNATURE, %eax
    jne .Le820_failed
    cmpl $X86_E820_MINIMUM_BYTES, %ecx
    jb .Le820_failed

    /* Treat the BIOS segment registers as clobbered after the interrupt. */
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movw $X86_E820_SCRATCH_ADDRESS, %di
    cmpl $X86_BOOT_MEMORY_RANGE_BYTES, %ecx
    jae .Le820_attributes_returned
    /* A short legacy record has no complete attributes field. */
    movl $X86_BOOT_MEMORY_ENABLED, \
	  %es:X86_BOOT_MEMORY_RANGE_ATTRIBUTES_OFFSET(%di)
    jmp .Le820_accept

.Le820_attributes_returned:
    testl $X86_BOOT_MEMORY_ENABLED, \
	  %es:X86_BOOT_MEMORY_RANGE_ATTRIBUTES_OFFSET(%di)
    jz .Le820_entry_done

.Le820_accept:
	/* Firmware is allowed to report ignorable empty/undefined records. */
    movl %es:X86_BOOT_MEMORY_RANGE_LENGTH_OFFSET(%di), %eax
    orl %es:X86_BOOT_MEMORY_RANGE_LENGTH_OFFSET + 4(%di), %eax
    jz .Le820_entry_done
    cmpl $0, %es:X86_BOOT_MEMORY_RANGE_TYPE_OFFSET(%di)
    je .Le820_entry_done
    /* The later C owners reject overflowing ranges.  Check here too because
     * relocation and the first kernel stack must be proven before C runs. */
    movl %es:X86_BOOT_MEMORY_RANGE_BASE_OFFSET(%di), %eax
    movl %es:X86_BOOT_MEMORY_RANGE_BASE_OFFSET + 4(%di), %edx
    addl %es:X86_BOOT_MEMORY_RANGE_LENGTH_OFFSET(%di), %eax
    adcl %es:X86_BOOT_MEMORY_RANGE_LENGTH_OFFSET + 4(%di), %edx
    jc .Le820_failed
    movw %es:X86_E820_STATE_ADDRESS + \
	  X86_E820_STATE_COUNT_OFFSET, %bp
    cmpw $X86_BOOT_MEMORY_RANGE_COUNT, %bp
    jb .Le820_store_count
    movb $1, %es:X86_E820_STATE_ADDRESS + \
	  X86_E820_STATE_OVERFLOW_OFFSET
    jmp .Le820_entry_done

.Le820_store_count:
    movw $X86_E820_SCRATCH_ADDRESS, %si
    movw %bp, %di
    imulw $X86_BOOT_MEMORY_RANGE_BYTES, %di
    addw $(X86_BOOT_INFO_ADDRESS + X86_BOOT_INFO_RANGES_OFFSET), %di
    movw $E820_RANGE_WORDS, %cx
    cld
    rep movsw
    incw %bp
    movw %bp, %es:X86_E820_STATE_ADDRESS + \
	  X86_E820_STATE_COUNT_OFFSET

.Le820_entry_done:
    testl %ebx, %ebx
    jne .Le820_next
    cmpb $0, %es:X86_E820_STATE_ADDRESS + \
	  X86_E820_STATE_OVERFLOW_OFFSET
    jne .Le820_failed
    movw %es:X86_E820_STATE_ADDRESS + \
	  X86_E820_STATE_COUNT_OFFSET, %bp
    testw %bp, %bp
    jz .Le820_failed

    /* Erase private scratch state before publishing the completed map. */
    movw $X86_E820_SCRATCH_ADDRESS, %di
    xorl %eax, %eax
    movw $E820_PRIVATE_WORDS, %cx
    cld
    rep stosw
    movw $X86_BOOT_INFO_ADDRESS, %di
    movw %bp, %es:X86_BOOT_INFO_RANGE_COUNT_OFFSET(%di)
    movl $X86_BOOT_INFO_FLAG_MASK, %es:X86_BOOT_INFO_FLAGS_OFFSET(%di)
    jmp .Lvalidate_boot_layout

.Le820_failed:
    /* Partial or truncated maps are unusable: remove every unpublished byte. */
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movw $X86_BOOT_INFO_ADDRESS, %di
    xorl %eax, %eax
    movw %ax, %es:X86_BOOT_INFO_RANGE_COUNT_OFFSET(%di)
    movl %eax, %es:X86_BOOT_INFO_FLAGS_OFFSET(%di)
    addw $X86_BOOT_INFO_RANGES_OFFSET, %di
    movw $E820_FAILURE_CLEAR_WORDS, %cx
    cld
    rep stosw
	    jmp .Lmemory_map_error

.Lvalidate_boot_layout:
    /*
     * The compact image is still executing below 1 MiB.  Prove that the
     * complete relocated kernel/BSS/stack aperture is usable RAM before the
     * first byte is copied there.  A non-usable overlap wins even if firmware
     * also supplied a broad usable entry.
     */
    movw %es:X86_BOOT_INFO_ADDRESS + X86_BOOT_INFO_RANGE_COUNT_OFFSET, %cx
    movw $(X86_BOOT_INFO_ADDRESS + X86_BOOT_INFO_RANGES_OFFSET), %si
    xorw %bp, %bp
.Lvalidate_boot_layout_next:
    movl %es:X86_BOOT_MEMORY_RANGE_BASE_OFFSET(%si), %ebx
    movl %es:X86_BOOT_MEMORY_RANGE_BASE_OFFSET + 4(%si), %edi
    movl %ebx, %eax
    movl %edi, %edx
    addl %es:X86_BOOT_MEMORY_RANGE_LENGTH_OFFSET(%si), %eax
    adcl %es:X86_BOOT_MEMORY_RANGE_LENGTH_OFFSET + 4(%si), %edx
    jc .Lboot_layout_error
    testl %edi, %edi
    jnz .Lvalidate_boot_layout_advance
    cmpl $X86_BOOT_MEMORY_USABLE, \
	  %es:X86_BOOT_MEMORY_RANGE_TYPE_OFFSET(%si)
    jne .Lvalidate_boot_layout_reserved
    cmpl $KERNEL_RUNTIME_BASE, %ebx
    ja .Lvalidate_boot_layout_advance
    testl %edx, %edx
    jnz .Lvalidate_boot_layout_covered
    cmpl $KERNEL_STACK_TOP, %eax
    jb .Lvalidate_boot_layout_advance
.Lvalidate_boot_layout_covered:
    movw $1, %bp
    jmp .Lvalidate_boot_layout_advance

.Lvalidate_boot_layout_reserved:
    cmpl $KERNEL_STACK_TOP, %ebx
    jae .Lvalidate_boot_layout_advance
    testl %edx, %edx
    jnz .Lboot_layout_error
    cmpl $KERNEL_RUNTIME_BASE, %eax
    ja .Lboot_layout_error

.Lvalidate_boot_layout_advance:
    addw $X86_BOOT_MEMORY_RANGE_BYTES, %si
    loop .Lvalidate_boot_layout_next
    testw %bp, %bp
    jz .Lboot_layout_error
    jmp .Le820_done

.Lmemory_map_error:
    movw $(memory_map_error - _start), %si
    jmp .Lprint_boot_error

.Lboot_layout_error:
    movw $(boot_layout_error - _start), %si

.Lprint_boot_error:
    movw $KERNEL_LOAD_SEGMENT, %ax
    movw %ax, %ds
    cld
.Lprint_boot_error_next:
    lodsb
    testb %al, %al
    jz .Lboot_error_halt
#if CONFIG_X86_DEBUGCON
    outb %al, $0xe9
#endif
    movb $0x0e, %ah
    movw $0x0007, %bx
    int $0x10
    jmp .Lprint_boot_error_next
.Lboot_error_halt:
    cli
    hlt
	jmp .Lboot_error_halt

/*
 * Capture optional machine-instance facts without turning absence into a boot
 * failure.  Display Combination Code identifies the active/inactive adapter.
 * RTC is sampled date/time/date so a midnight rollover cannot publish a torn
 * calendar.  C independently validates every BCD field before capability use.
 */
.Lcapture_boot_platform:
	pushw %ax
	pushw %bx
	pushw %cx
	pushw %dx
	pushw %si
	pushw %di
	pushw %bp
	pushw %ds
	pushw %es

	xorw %ax, %ax
	movw %ax, %ds
	movw %ax, %es
	movw $0x1a00, %ax
	int $0x10
	cmpb $0x1a, %al
	jne .Lcapture_rtc
	xorw %ax, %ax
	movw %ax, %ds
	movw %ax, %es
	movw $X86_BOOT_INFO_ADDRESS, %di
	movb %bl, %es:X86_BOOT_INFO_PLATFORM_OFFSET + \
		X86_BOOT_PLATFORM_DISPLAY_ACTIVE_DCC_OFFSET(%di)
	movb %bh, %es:X86_BOOT_INFO_PLATFORM_OFFSET + \
		X86_BOOT_PLATFORM_DISPLAY_INACTIVE_DCC_OFFSET(%di)
	orl $X86_BOOT_PLATFORM_DISPLAY_DCC_PRESENT, \
		%es:X86_BOOT_INFO_PLATFORM_OFFSET + \
		X86_BOOT_PLATFORM_FLAGS_OFFSET(%di)

.Lcapture_rtc:
	movw $CONFIG_X86_BOOT_RTC_SNAPSHOT_ATTEMPTS, %bp
.Lcapture_rtc_next:
	movb $0x04, %ah
	int $0x1a
	jc .Lcapture_rtc_retry
	xorw %ax, %ax
	movw %ax, %ds
	movw %ax, %es
	movw $X86_BOOT_INFO_ADDRESS, %di
	movb %ch, %es:X86_BOOT_INFO_PLATFORM_OFFSET + \
		X86_BOOT_PLATFORM_RTC_CENTURY_BCD_OFFSET(%di)
	movb %cl, %es:X86_BOOT_INFO_PLATFORM_OFFSET + \
		X86_BOOT_PLATFORM_RTC_YEAR_BCD_OFFSET(%di)
	movb %dh, %es:X86_BOOT_INFO_PLATFORM_OFFSET + \
		X86_BOOT_PLATFORM_RTC_MONTH_BCD_OFFSET(%di)
	movb %dl, %es:X86_BOOT_INFO_PLATFORM_OFFSET + \
		X86_BOOT_PLATFORM_RTC_DAY_BCD_OFFSET(%di)

	movb $0x02, %ah
	int $0x1a
	jc .Lcapture_rtc_retry
	xorw %ax, %ax
	movw %ax, %ds
	movw %ax, %es
	movw $X86_BOOT_INFO_ADDRESS, %di
	movb %ch, %es:X86_BOOT_INFO_PLATFORM_OFFSET + \
		X86_BOOT_PLATFORM_RTC_HOUR_BCD_OFFSET(%di)
	movb %cl, %es:X86_BOOT_INFO_PLATFORM_OFFSET + \
		X86_BOOT_PLATFORM_RTC_MINUTE_BCD_OFFSET(%di)
	movb %dh, %es:X86_BOOT_INFO_PLATFORM_OFFSET + \
		X86_BOOT_PLATFORM_RTC_SECOND_BCD_OFFSET(%di)
	movb %dl, %es:X86_BOOT_INFO_PLATFORM_OFFSET + \
		X86_BOOT_PLATFORM_RTC_DAYLIGHT_OFFSET(%di)

	movb $0x04, %ah
	int $0x1a
	jc .Lcapture_rtc_retry
	xorw %ax, %ax
	movw %ax, %ds
	movw %ax, %es
	movw $X86_BOOT_INFO_ADDRESS, %di
	cmpb %es:X86_BOOT_INFO_PLATFORM_OFFSET + \
		X86_BOOT_PLATFORM_RTC_CENTURY_BCD_OFFSET(%di), %ch
	jne .Lcapture_rtc_retry
	cmpb %es:X86_BOOT_INFO_PLATFORM_OFFSET + \
		X86_BOOT_PLATFORM_RTC_YEAR_BCD_OFFSET(%di), %cl
	jne .Lcapture_rtc_retry
	cmpb %es:X86_BOOT_INFO_PLATFORM_OFFSET + \
		X86_BOOT_PLATFORM_RTC_MONTH_BCD_OFFSET(%di), %dh
	jne .Lcapture_rtc_retry
	cmpb %es:X86_BOOT_INFO_PLATFORM_OFFSET + \
		X86_BOOT_PLATFORM_RTC_DAY_BCD_OFFSET(%di), %dl
	jne .Lcapture_rtc_retry
	orl $X86_BOOT_PLATFORM_RTC_PRESENT, \
		%es:X86_BOOT_INFO_PLATFORM_OFFSET + \
		X86_BOOT_PLATFORM_FLAGS_OFFSET(%di)
	jmp .Lcapture_boot_platform_done

.Lcapture_rtc_retry:
	xorw %ax, %ax
	movw %ax, %ds
	movw %ax, %es
	decw %bp
	jnz .Lcapture_rtc_next

.Lcapture_boot_platform_done:
	popw %es
	popw %ds
	popw %bp
	popw %di
	popw %si
	popw %dx
	popw %cx
	popw %bx
	popw %ax
	cld
	ret

/*
 * Return AX=1 when A20 is observably enabled, otherwise AX=0.  The low word
 * at vector 80h is restored before returning and the high alias is read-only.
 * A locked no-op serializes the probe without depending on an I/O-delay port.
 * ECX supplies the bounded number of observations.
 */
.La20_test:
    pushw %fs
    pushw %gs
    pushl %ebx
    pushl %edx
    xorw %ax, %ax
    movw %ax, %fs
    movw $0xffff, %ax
    movw %ax, %gs
    movl %fs:X86_A20_TEST_OFFSET, %edx
    movl %edx, %ebx
.La20_test_next:
    incl %ebx
    movl %ebx, %fs:X86_A20_TEST_OFFSET
    lock addl $0, %fs:X86_A20_TEST_OFFSET
    movl %gs:X86_A20_HIGH_ALIAS_OFFSET, %eax
    xorl %ebx, %eax
    jnz .La20_test_enabled
    decl %ecx
    jnz .La20_test_next
    xorl %eax, %eax
    jmp .La20_test_restore
.La20_test_enabled:
    movl $1, %eax
.La20_test_restore:
    movl %edx, %fs:X86_A20_TEST_OFFSET
    popl %edx
    popl %ebx
    popw %gs
    popw %fs
    ret

/* Return AX=0 only after both 8042 buffers have drained. */
.La20_empty_kbc:
    movl $CONFIG_X86_A20_KBC_POLL_LIMIT, %ecx
    movl $CONFIG_X86_A20_KBC_ABSENT_SAMPLES, %edx
.La20_empty_kbc_next:
    inb $X86_KBC_STATUS_COMMAND_PORT, %al
    cmpb $0xff, %al
    jne .La20_empty_kbc_status
    decl %edx
    jz .La20_empty_kbc_failed
.La20_empty_kbc_status:
    testb $X86_KBC_OUTPUT_FULL, %al
    jz .La20_empty_kbc_input
    inb $X86_KBC_DATA_PORT, %al
    jmp .La20_empty_kbc_advance
.La20_empty_kbc_input:
    testb $X86_KBC_INPUT_FULL, %al
    jz .La20_empty_kbc_ready
.La20_empty_kbc_advance:
    decl %ecx
    jnz .La20_empty_kbc_next
.La20_empty_kbc_failed:
    movw $1, %ax
    ret
.La20_empty_kbc_ready:
    xorw %ax, %ax
    ret

/* Return AX=0 only when every keyboard-controller transition completed. */
.La20_enable_kbc:
    call .La20_empty_kbc
    testw %ax, %ax
    jnz .La20_enable_kbc_done
    movb $X86_KBC_WRITE_OUTPUT_PORT, %al
    outb %al, $X86_KBC_STATUS_COMMAND_PORT
    call .La20_empty_kbc
    testw %ax, %ax
    jnz .La20_enable_kbc_done
    movb $X86_KBC_A20_OUTPUT, %al
    outb %al, $X86_KBC_DATA_PORT
    call .La20_empty_kbc
    testw %ax, %ax
    jnz .La20_enable_kbc_done
    /* A null command safely releases controllers sharing UHCI wiring. */
    movb $X86_KBC_NULL_COMMAND, %al
    outb %al, $X86_KBC_STATUS_COMMAND_PORT
    call .La20_empty_kbc
.La20_enable_kbc_done:
    ret

.La20_enable_fast:
    inb $X86_FAST_A20_PORT, %al
    orb $X86_FAST_A20_ENABLE, %al
    andb $~X86_FAST_A20_RESET, %al
    outb %al, $X86_FAST_A20_PORT
    ret

/* Return AX=1 only after a verified transition; all failure paths are bounded. */
.La20_enable:
    pushl %ebx
    movl $CONFIG_X86_A20_ENABLE_ATTEMPTS, %ebx
.La20_enable_next:
    movl $CONFIG_X86_A20_SHORT_TEST_LOOPS, %ecx
    call .La20_test
    testw %ax, %ax
    jnz .La20_enable_ready

    /* BIOS function 2401h is the least hardware-specific transition. */
    pushl %ebx
    movw $0x2401, %ax
    int $0x15
    popl %ebx
    movw $KERNEL_LOAD_SEGMENT, %ax
    movw %ax, %ds
    movw %ax, %es
    cld
    movl $CONFIG_X86_A20_SHORT_TEST_LOOPS, %ecx
    call .La20_test
    testw %ax, %ax
    jnz .La20_enable_ready

    call .La20_enable_kbc
    movl $CONFIG_X86_A20_LONG_TEST_LOOPS, %ecx
    call .La20_test
    testw %ax, %ax
    jnz .La20_enable_ready

    call .La20_enable_fast
    movl $CONFIG_X86_A20_LONG_TEST_LOOPS, %ecx
    call .La20_test
    testw %ax, %ax
    jnz .La20_enable_ready
    decl %ebx
    jnz .La20_enable_next
    xorw %ax, %ax
    popl %ebx
    ret
.La20_enable_ready:
    movw $1, %ax
    popl %ebx
    ret

.Le820_done:
    /* Restore the relocation-free bootstrap data segment expected below. */
    cld
    movw $KERNEL_LOAD_SEGMENT, %ax
    movw %ax, %ds
    movw %ax, %es

    /*
     * A20 is discovered state, not a PC-board constant.  Try the firmware,
     * a responsive 8042, and the fast gate in bounded order, checking the
     * architectural 000000h/100000h alias after every transition.  No method
     * is trusted merely because its command completed.
     */
    call .La20_enable
    testw %ax, %ax
    jnz .La20_ready
    movw $(a20_error - _start), %si
    jmp .Lprint_boot_error

.La20_ready:

    lgdt gdt_descriptor - _start
    movl %cr0, %eax
    orl $1, %eax
    movl %eax, %cr0
    ljmpl $CODE_SELECTOR, $protected_entry

.code32
protected_entry:
    movw $DATA_SELECTOR, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss
    /*
     * The BIOS could only stage the image in conventional memory. Copy the
     * linked runtime payload above 1 MiB before calling any C. Source and
     * destination layout have the same gaps, so one bounded copy covers RX,
     * RO, and initialized RW bytes without interpreting ELF structures.
     */
    movl $__kernel_staging_payload_start, %esi
    movl $__kernel_runtime_start, %edi
    movl $__kernel_runtime_file_end, %ecx
    subl %edi, %ecx
    rep movsb
    ljmpl $CODE_SELECTOR, $relocated_entry

.section .text.boot, "ax", @progbits
.code32
relocated_entry:
    /* The bootstrap GDT will become guest-reclaimable low memory. */
    lgdt runtime_gdt_descriptor
    ljmpl $CODE_SELECTOR, $runtime_segments_ready

runtime_segments_ready:
    movw $DATA_SELECTOR, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss
    movl $KERNEL_STACK_TOP, %esp
    xorl %ebp, %ebp

    /* A binary image has no storage for .bss, so clear it explicitly. */
    movl $__bss_start, %edi
    movl $__bss_end, %ecx
    subl %edi, %ecx
    xorl %eax, %eax
    rep stosb

    /* No stack-protected C frame exists yet, so this is the only safe point
     * to replace the zero bootstrap guard. */
    call .Lseed_stack_guard

    /* cdecl: push the second parameter before the first parameter. */
    pushl $X86_BOOT_INFO_ADDRESS
    movzbl boot_drive, %eax
    pushl %eax
    call kmain
    addl $8, %esp

1:
    cli
    hlt
    jmp 1b

/*
 * Seed the global GCC stack guard before kmain.  RDRAND is used only after
 * CPUID advertises it and a bounded retry succeeds; TSC is likewise feature
 * gated.  Older i386-class machines still get a per-boot diversified weak
 * seed from the live BIOS tick and validated boot facts, recorded honestly in
 * the source mask rather than being called hardware entropy.
 */
.Lseed_stack_guard:
    pushl %ebx
    pushl %esi
    pushl %edi
    pushl %ebp

    movl X86_BOOT_INFO_ADDRESS + X86_BOOT_INFO_SIGNATURE_OFFSET, %ebp
    xorl X86_BOOT_INFO_ADDRESS + X86_BOOT_INFO_FLAGS_OFFSET, %ebp
    xorl X86_BOOT_INFO_ADDRESS + X86_BOOT_INFO_STORAGE_OFFSET + \
	 X86_BOOT_STORAGE_VOLUME_START_LBA_OFFSET, %ebp
    xorl X86_BOOT_INFO_ADDRESS + X86_BOOT_INFO_STORAGE_OFFSET + \
	 X86_BOOT_STORAGE_VOLUME_SECTOR_COUNT_OFFSET, %ebp
    xorl X86_BDA_TIMER_TICKS, %ebp
    xorl %esp, %ebp
    movl $X86_STACK_GUARD_SOURCE_BOOT_STATE, %edi

    /* Probe the EFLAGS.ID bit before executing CPUID on genuine i386 CPUs. */
    pushfl
    popl %eax
    movl %eax, %edx
    xorl $X86_EFLAGS_CPUID, %eax
    pushl %eax
    popfl
    pushfl
    popl %eax
    pushl %edx
    popfl
    xorl %edx, %eax
    testl $X86_EFLAGS_CPUID, %eax
    jz .Lstack_guard_mix

    xorl %eax, %eax
    /* CPUID, encoded so the boot object remains buildable for -march=i386. */
    .byte 0x0f, 0xa2
    movl %eax, %esi
    xorl %ebx, %ebp
    xorl %ecx, %ebp
    xorl %edx, %ebp
    cmpl $X86_CPUID_FEATURES, %esi
    jb .Lstack_guard_mix

    movl $X86_CPUID_FEATURES, %eax
    .byte 0x0f, 0xa2
    xorl %eax, %ebp
    xorl %ecx, %ebp
    xorl %edx, %ebp
    movl %ecx, %esi
    movl %edx, %ebx

    testl $X86_CPUID_TSC, %ebx
    jz .Lstack_guard_rdrand
    /* RDTSC */
    .byte 0x0f, 0x31
    xorl %eax, %ebp
    roll $13, %ebp
    xorl %edx, %ebp
    orl $X86_STACK_GUARD_SOURCE_TSC, %edi

.Lstack_guard_rdrand:
    testl $X86_CPUID_RDRAND, %esi
    jz .Lstack_guard_mix
    movl $CONFIG_X86_STACK_GUARD_RDRAND_ATTEMPTS, %ecx
.Lstack_guard_rdrand_next:
    /* RDRAND EAX */
    .byte 0x0f, 0xc7, 0xf0
    jc .Lstack_guard_rdrand_ready
    loop .Lstack_guard_rdrand_next
    jmp .Lstack_guard_mix
.Lstack_guard_rdrand_ready:
    xorl %eax, %ebp
    orl $X86_STACK_GUARD_SOURCE_RDRAND, %edi

.Lstack_guard_mix:
    movl %ebp, %eax
    shrl $16, %eax
    xorl %eax, %ebp
    imull $0x7feb352d, %ebp
    movl %ebp, %eax
    shrl $15, %eax
    xorl %eax, %ebp
    imull $0x846ca68b, %ebp
    movl %ebp, %eax
    shrl $16, %eax
    xorl %eax, %ebp
    orl $1, %ebp
    movl %ebp, __stack_chk_guard
    movl %edi, __stack_chk_guard_source_mask

    popl %ebp
    popl %edi
    popl %esi
    popl %ebx
    ret

.section .rodata.gdt, "a", @progbits
.align 8
runtime_gdt_start:
    .quad 0x0000000000000000      /* null */
    .quad 0x00cf9a000000ffff      /* base 0, 4 GiB, ring-0 code */
    .quad 0x00cf92000000ffff      /* base 0, 4 GiB, ring-0 data */
runtime_gdt_end:

runtime_gdt_descriptor:
    .word runtime_gdt_end - runtime_gdt_start - 1
    .long runtime_gdt_start

.section .entry16, "ax", @progbits
.code32

.align 8
gdt_start:
    .quad 0x0000000000000000      /* null */
    .quad 0x00cf9a000000ffff      /* base 0, 4 GiB, ring-0 code */
    .quad 0x00cf92000000ffff      /* base 0, 4 GiB, ring-0 data */
gdt_end:

gdt_descriptor:
    .word gdt_end - gdt_start - 1
    .long gdt_start

boot_drive:
    .byte 0
memory_map_error:
    .asciz "DOS-C32: BIOS memory map unavailable or invalid.\r\n"
boot_layout_error:
    .asciz "DOS-C32: kernel runtime layout is not usable RAM.\r\n"
a20_error:
    .asciz "DOS-C32: A20 could not be enabled and verified.\r\n"

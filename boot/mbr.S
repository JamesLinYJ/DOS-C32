/* SPDX-License-Identifier: GPL-2.0-only */
/* Legacy BIOS MBR: relocate, find the active partition, and load its VBR. */
#ifndef PARTITION_START_LBA
#error "PARTITION_START_LBA must be supplied by the build"
#endif
#ifndef PARTITION_SECTORS
#error "PARTITION_SECTORS must be supplied by the build"
#endif

#define BIOS_DISK_INTERRUPT 0x13
#define BIOS_VIDEO_INTERRUPT 0x10
#define BIOS_EDD_CHECK_FUNCTION 0x41
#define BIOS_EDD_READ_FUNCTION 0x42
#define MBR_BIOS_LOAD_BASE 0x7c00
#define MBR_RELOCATE_BASE 0x0600
#define MBR_PARTITION_TABLE_OFFSET 446
#define MBR_PARTITION_ENTRY_BYTES 16
#define MBR_ACTIVE_FLAG 0x80
#define FAT16_PARTITION_TYPE 0x06

.section .mbr, "ax", @progbits
.code16
.global mbr_start

/* These instructions execute once at 0000:7C00 but are linked for 0000:0600. */
mbr_start:
    cli
    cld
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw $MBR_BIOS_LOAD_BASE, %sp
    movw $MBR_BIOS_LOAD_BASE, %si
    movw $MBR_RELOCATE_BASE, %di
    movw $256, %cx
    rep movsw
    ljmp $0, $relocated

relocated:
    sti
    movb %dl, boot_drive
    movw $0x55aa, %bx
    movb $BIOS_EDD_CHECK_FUNCTION, %ah
    int $BIOS_DISK_INTERRUPT
    jc mbr_error
    cmpw $0xaa55, %bx
    jne mbr_error
    testw $1, %cx
    jz mbr_error

    movw $partition_table, %si
    movw $4, %cx
find_active:
    cmpb $MBR_ACTIVE_FLAG, (%si)
    je active_found
    addw $MBR_PARTITION_ENTRY_BYTES, %si
    loop find_active
    jmp mbr_error

active_found:
    movl 8(%si), %eax
    testl %eax, %eax
    jz mbr_error
    movl %eax, disk_packet + 8
    movw $disk_packet, %si
    movb boot_drive, %dl
    movb $BIOS_EDD_READ_FUNCTION, %ah
    int $BIOS_DISK_INTERRUPT
    jc mbr_error
    cmpw $0xaa55, MBR_BIOS_LOAD_BASE + 510
    jne mbr_error
    movb boot_drive, %dl
    ljmp $0, $MBR_BIOS_LOAD_BASE

mbr_error:
    movw $error_message, %si
1:
    lodsb
    testb %al, %al
    jz 2f
    movb $0x0e, %ah
    movw $0x0007, %bx
    int $BIOS_VIDEO_INTERRUPT
    jmp 1b
2:
    cli
3:
    hlt
    jmp 3b

.align 4
disk_packet:
    .byte 0x10, 0
    .word 1
    .word MBR_BIOS_LOAD_BASE, 0
    .long 0, 0
boot_drive:
    .byte 0
error_message:
    .asciz "DOS-C32 MBR: no bootable FAT16 partition.\r\n"

.org MBR_PARTITION_TABLE_OFFSET
partition_table:
    /* Active FAT16, LBA-first.  CHS fields are compatibility sentinels. */
    .byte MBR_ACTIVE_FLAG, 0x20, 0x21, 0x00
    .byte FAT16_PARTITION_TYPE, 0xfe, 0xff, 0xff
    .long PARTITION_START_LBA
    .long PARTITION_SECTORS
    .zero 3 * MBR_PARTITION_ENTRY_BYTES
    .word 0xaa55

.section .note.GNU-stack, "", @progbits

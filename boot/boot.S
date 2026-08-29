/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef BIOS_BOOT_LOAD_ADDRESS
#error "BIOS_BOOT_LOAD_ADDRESS must be supplied by the build"
#endif
#ifndef BIOS_SECTOR_BYTES
#error "BIOS_SECTOR_BYTES must be supplied by the build"
#endif
#ifndef BIOS_SECTOR_PARAGRAPHS
#error "BIOS_SECTOR_PARAGRAPHS must be supplied by the build"
#endif
#ifndef BIOS_READ_RETRIES
#error "BIOS_READ_RETRIES must be supplied by the build"
#endif
#ifndef IO_LOAD_BASE
#error "IO_LOAD_BASE must be supplied by the build"
#endif
#ifndef IO_MAX_BYTES
#error "IO_MAX_BYTES must be supplied by the build"
#endif
#ifndef FAT_SECTORS_PER_CLUSTER
#error "FAT_SECTORS_PER_CLUSTER must be supplied by the build"
#endif
#ifndef FAT_RESERVED_SECTORS
#error "FAT_RESERVED_SECTORS must be supplied by the build"
#endif
#ifndef FAT_COPIES
#error "FAT_COPIES must be supplied by the build"
#endif
#ifndef FAT_ROOT_ENTRIES
#error "FAT_ROOT_ENTRIES must be supplied by the build"
#endif
#ifndef FAT_SECTORS_PER_COPY
#error "FAT_SECTORS_PER_COPY must be supplied by the build"
#endif
#ifndef FAT_MEDIA_DESCRIPTOR
#error "FAT_MEDIA_DESCRIPTOR must be supplied by the build"
#endif
#ifndef BIOS_HEADS
#error "BIOS_HEADS must be supplied by the build"
#endif
#ifndef BIOS_SECTORS_PER_TRACK
#error "BIOS_SECTORS_PER_TRACK must be supplied by the build"
#endif
#ifndef BIOS_DRIVE_NUMBER
#error "BIOS_DRIVE_NUMBER must be supplied by the build"
#endif
#ifndef IMAGE_SECTORS
#error "IMAGE_SECTORS must be supplied by the build"
#endif
#ifndef IMAGE_VOLUME_ID
#error "IMAGE_VOLUME_ID must be supplied by the build"
#endif
#ifndef FAT_HIDDEN_SECTORS
#error "FAT_HIDDEN_SECTORS must be supplied by the build"
#endif

/* Legacy BIOS and FAT16 ABI values, named separately from build policy. */
#define DOS_PARAGRAPH_BYTES 16
#define EDD_SECTORS_PER_REQUEST 1
#define EDD_PACKET_BYTES 0x10
#define EDD_BUFFER_OFFSET_FIELD 4
#define EDD_BUFFER_SEGMENT_FIELD 6
#define EDD_START_LBA_FIELD 8
#define BIOS_DISK_INTERRUPT 0x13
#define BIOS_VIDEO_INTERRUPT 0x10
#define BIOS_EDD_CHECK_FUNCTION 0x41
#define BIOS_EDD_READ_FUNCTION 0x42
#define BIOS_EDD_REQUEST_SIGNATURE 0x55aa
#define BIOS_EDD_RESPONSE_SIGNATURE 0xaa55
#define BIOS_EDD_FIXED_DISK_ACCESS 0x0001
#define BIOS_VIDEO_TTY_FUNCTION 0x0e
#define BIOS_VIDEO_TTY_PAGE_COLOR 0x0007
#define FAT16_EXTENDED_BOOT_SIGNATURE 0x29
#define BOOT_SECTOR_SIGNATURE 0xaa55

#define FAT_DIRECTORY_ENTRY_BYTES 32
/* The stage-one loader requires the BIOS-standard 512-byte sector size. */
#define FAT_DIRECTORY_ENTRIES_PER_SECTOR 16
#define FAT_DIRECTORY_FIRST_CLUSTER 26
#define FAT_DIRECTORY_FILE_SIZE 28
#define FAT_FIRST_DATA_CLUSTER 2
#define ROOT_SCRATCH_BASE 0x0600

#if BIOS_SECTOR_BYTES <= 0 || (BIOS_SECTOR_BYTES % DOS_PARAGRAPH_BYTES) != 0
#error "BIOS sector size must be a positive paragraph multiple"
#endif
#if (IO_LOAD_BASE % DOS_PARAGRAPH_BYTES) != 0
#error "IO.SYS load base must be paragraph aligned"
#endif
#if FAT_RESERVED_SECTORS < 2 || FAT_RESERVED_SECTORS > 0xffff
#error "reserved-sector count does not fit the FAT16 BPB"
#endif
#if IO_MAX_BYTES < 1 || IO_MAX_BYTES > \
	(FAT_SECTORS_PER_CLUSTER * BIOS_SECTOR_BYTES)
#error "IO.SYS must fit in one FAT cluster for the stage-one loader"
#endif
#if IMAGE_SECTORS < 1 || IMAGE_SECTORS > 0xffffffff
#error "image sector count does not fit the FAT BPB"
#endif
#if IMAGE_SECTORS <= 0xffff
#define BPB_TOTAL_SECTORS16 IMAGE_SECTORS
#define BPB_TOTAL_SECTORS32 0
#else
#define BPB_TOTAL_SECTORS16 0
#define BPB_TOTAL_SECTORS32 IMAGE_SECTORS
#endif

/*
 * BIOS sector zero.  This is a bounded FAT16 loader.  The FAT16 BPB is public
 * disk-format state; all payload reads use EDD LBA semantics and avoid CHS and
 * track-boundary hazards.
 */
.section .boot, "ax", @progbits
.code16
.set IO_LOAD_SEGMENT_VALUE, IO_LOAD_BASE >> 4
.set ROOT_ENTRY_ROUNDING, FAT_DIRECTORY_ENTRIES_PER_SECTOR - 1
.set SECTOR_ROUNDING, BIOS_SECTOR_BYTES - 1

.global boot_start

    jmp boot_start
    nop

/* FAT16 BIOS Parameter Block: must match the mkfs.fat flags in Makefile. */
    .ascii "DOSC32  "             /* OEM name, 8 bytes */
bpb_bytes_per_sector:
    .word BIOS_SECTOR_BYTES       /* bytes per sector */
bpb_sectors_per_cluster:
    .byte FAT_SECTORS_PER_CLUSTER
bpb_reserved_sectors:
    .word FAT_RESERVED_SECTORS
bpb_fat_count:
    .byte FAT_COPIES
bpb_root_entries:
    .word FAT_ROOT_ENTRIES
    .word BPB_TOTAL_SECTORS16     /* total sectors, zero when 32-bit */
    .byte FAT_MEDIA_DESCRIPTOR
bpb_sectors_per_fat:
    .word FAT_SECTORS_PER_COPY
    .word BIOS_SECTORS_PER_TRACK  /* informational EDD geometry */
    .word BIOS_HEADS              /* informational EDD geometry */
bpb_hidden_sectors:
    .long FAT_HIDDEN_SECTORS      /* partition start LBA */
    .long BPB_TOTAL_SECTORS32     /* total sectors, zero when 16-bit */
    .byte BIOS_DRIVE_NUMBER
    .byte 0                       /* reserved */
    .byte FAT16_EXTENDED_BOOT_SIGNATURE
    .long IMAGE_VOLUME_ID
    .ascii "DOSC32     "          /* volume label, 11 bytes */
    .ascii "FAT16   "             /* filesystem identifier */

boot_start:
    cli
    cld
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw $BIOS_BOOT_LOAD_ADDRESS, %sp
    sti
    movb %dl, boot_drive

    /* Require INT 13h extensions, which makes the image geometry-independent. */
    movw $BIOS_EDD_REQUEST_SIGNATURE, %bx
    movb $BIOS_EDD_CHECK_FUNCTION, %ah
    int $BIOS_DISK_INTERRUPT
    jc unsupported
    cmpw $BIOS_EDD_RESPONSE_SIGNATURE, %bx
    jne unsupported
    testw $BIOS_EDD_FIXED_DISK_ACCESS, %cx
    jz unsupported

    cmpw $BIOS_SECTOR_BYTES, bpb_bytes_per_sector
    jne boot_geometry_error
    movw bpb_root_entries, %ax
    testw %ax, %ax
    jz boot_geometry_error
    testw $ROOT_ENTRY_ROUNDING, %ax
    jnz boot_geometry_error

boot_geometry_valid:

    /* All FAT LBAs are absolute: hidden + reserved + FAT/root offsets. */
    movzwl bpb_sectors_per_fat, %eax
    movzbl bpb_fat_count, %ecx
    imull %ecx, %eax
    movzwl bpb_reserved_sectors, %ecx
    addl %ecx, %eax
    addl bpb_hidden_sectors, %eax
    movl %eax, root_lba
    movzwl bpb_root_entries, %eax
    addl $ROOT_ENTRY_ROUNDING, %eax
    shrl $4, %eax
    movw %ax, root_sectors
    addl root_lba, %eax
    movl %eax, first_data_lba

scan_root_sector:
    cmpw $0, root_sectors
    je boot_error
    movl root_lba, %eax
    movl %eax, disk_address_packet + EDD_START_LBA_FIELD
    movw $ROOT_SCRATCH_BASE, disk_address_packet + EDD_BUFFER_OFFSET_FIELD
    movw $0, disk_address_packet + EDD_BUFFER_SEGMENT_FIELD
    call read_sector
    jc boot_error
    movw $ROOT_SCRATCH_BASE, %si
    movw $FAT_DIRECTORY_ENTRIES_PER_SECTOR, %dx
scan_root_entry:
    pushw %si
    movw $io_system_name, %di
    movw $11, %cx
    repe cmpsb
    popw %si
    je io_system_found
    addw $FAT_DIRECTORY_ENTRY_BYTES, %si
    decw %dx
    jnz scan_root_entry
    incl root_lba
    decw root_sectors
    jmp scan_root_sector

io_system_found:
    movzwl FAT_DIRECTORY_FIRST_CLUSTER(%si), %eax
    cmpw $FAT_FIRST_DATA_CLUSTER, %ax
    jb boot_error
    subl $FAT_FIRST_DATA_CLUSTER, %eax
    movzbl bpb_sectors_per_cluster, %ecx
    imull %ecx, %eax
    addl first_data_lba, %eax
    movl %eax, disk_address_packet + EDD_START_LBA_FIELD
    movl FAT_DIRECTORY_FILE_SIZE(%si), %eax
    testl %eax, %eax
    jz boot_error
    cmpl $IO_MAX_BYTES, %eax
    ja boot_error
    addl $SECTOR_ROUNDING, %eax
    shrl $9, %eax
    movw %ax, %cx
    movw $0, disk_address_packet + EDD_BUFFER_OFFSET_FIELD
    movw $IO_LOAD_SEGMENT_VALUE, \
	 disk_address_packet + EDD_BUFFER_SEGMENT_FIELD
load_io_sector:
    call read_sector
    jc boot_error
    addl $1, disk_address_packet + EDD_START_LBA_FIELD
    addw $BIOS_SECTOR_PARAGRAPHS, \
	 disk_address_packet + EDD_BUFFER_SEGMENT_FIELD
    loop load_io_sector
    movb boot_drive, %dl
    ljmp $0x0000, $IO_LOAD_BASE

read_sector:
    movb $BIOS_READ_RETRIES, retry_count
1:
    pushw %cx
    pushw %dx
    movw $disk_address_packet, %si
    movb boot_drive, %dl
    movb $BIOS_EDD_READ_FUNCTION, %ah
    int $BIOS_DISK_INTERRUPT
    popw %dx
    popw %cx
    jnc 2f
    decb retry_count
    jnz 1b
    stc
2:
    ret

unsupported:
boot_error:
    movw $error_message, %si
    jmp print_error

boot_geometry_error:
    movw $geometry_error_message, %si

print_error:
    lodsb
    testb %al, %al
    jz halt
#if CONFIG_X86_DEBUGCON
    outb %al, $0xe9
#endif
    movb $BIOS_VIDEO_TTY_FUNCTION, %ah
    movw $BIOS_VIDEO_TTY_PAGE_COLOR, %bx
    int $BIOS_VIDEO_INTERRUPT
    jmp print_error

halt:
    cli
1:
    hlt
    jmp 1b

.align 4
disk_address_packet:
    .byte EDD_PACKET_BYTES
    .byte 0
    .word EDD_SECTORS_PER_REQUEST
    .word 0x0000
    .word 0x0000
    .long 0
    .long 0

boot_drive:
    .byte 0
retry_count:
    .byte 0
root_sectors:
    .word 0
root_lba:
    .long 0
first_data_lba:
    .long 0
io_system_name:
    .ascii "IO      SYS"
error_message:
    .asciz "DOS-C32 boot error\r\n"
geometry_error_message:
    .asciz "Bad FAT16 geometry\r\n"

    .org BIOS_SECTOR_BYTES - 2
    .word BOOT_SECTOR_SIGNATURE

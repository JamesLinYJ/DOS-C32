/* SPDX-License-Identifier: GPL-2.0-only */
/* FAT16 IO.SYS loader: load DOSKRNL.SYS by its real cluster chain. */
#ifndef BIOS_SECTOR_BYTES
#error "BIOS_SECTOR_BYTES must be supplied by the build"
#endif
#ifndef BIOS_SECTOR_PARAGRAPHS
#error "BIOS_SECTOR_PARAGRAPHS must be supplied by the build"
#endif
#ifndef BIOS_READ_RETRIES
#error "BIOS_READ_RETRIES must be supplied by the build"
#endif
#ifndef KERNEL_LOAD_BASE
#error "KERNEL_LOAD_BASE must be supplied by the build"
#endif
#ifndef KERNEL_LOAD_LIMIT
#error "KERNEL_LOAD_LIMIT must be supplied by the build"
#endif

#define DOS_PARAGRAPH_BYTES 16
#define BIOS_DISK_INTERRUPT 0x13
#define BIOS_VIDEO_INTERRUPT 0x10
#define BIOS_EDD_READ_FUNCTION 0x42
#define BIOS_VIDEO_TTY_FUNCTION 0x0e
#define BIOS_VIDEO_TTY_PAGE_COLOR 0x0007
#if CONFIG_X86_DEBUGCON
#define QEMU_DEBUG_OUTPUT_PORT 0xe9
#endif
#define EDD_PACKET_BYTES 0x10
#define EDD_SECTORS_PER_REQUEST 1
#define EDD_BUFFER_SEGMENT_FIELD 6
#define EDD_START_LBA_FIELD 8
#define FAT_DIRECTORY_ENTRY_BYTES 32
#define FAT_DIRECTORY_ENTRIES_PER_SECTOR 16
#define FAT_DIRECTORY_FIRST_CLUSTER 26
#define FAT_DIRECTORY_FILE_SIZE 28
#define FAT_FIRST_DATA_CLUSTER 2
#define FAT16_END_OF_CHAIN 0xfff8
#define FAT16_LAST_DATA_CLUSTER 0xffef
#define ROOT_SCRATCH_BASE 0x0600
#define BOOT_SECTOR_BASE 0x7c00
#define BPB_BYTES_PER_SECTOR (BOOT_SECTOR_BASE + 11)
#define BPB_SECTORS_PER_CLUSTER (BOOT_SECTOR_BASE + 13)
#define BPB_RESERVED_SECTORS (BOOT_SECTOR_BASE + 14)
#define BPB_FAT_COUNT (BOOT_SECTOR_BASE + 16)
#define BPB_ROOT_ENTRIES (BOOT_SECTOR_BASE + 17)
#define BPB_SECTORS_PER_FAT (BOOT_SECTOR_BASE + 22)
#define BPB_HIDDEN_SECTORS (BOOT_SECTOR_BASE + 28)

#if BIOS_SECTOR_BYTES != 512
#error "IO.SYS currently requires 512-byte FAT16 sectors"
#endif
#if KERNEL_LOAD_LIMIT <= KERNEL_LOAD_BASE || \
	(KERNEL_LOAD_BASE % DOS_PARAGRAPH_BYTES) != 0 || \
	(KERNEL_LOAD_LIMIT % DOS_PARAGRAPH_BYTES) != 0
#error "DOSKRNL.SYS staging window is invalid"
#endif

.section .io_loader, "ax", @progbits
.code16
.global io_start
.set KERNEL_LOAD_SEGMENT_VALUE, KERNEL_LOAD_BASE >> 4
.set KERNEL_LOAD_LIMIT_SEGMENT_VALUE, \
	KERNEL_LOAD_LIMIT >> 4
.set KERNEL_MAX_SECTORS_VALUE, \
	(KERNEL_LOAD_LIMIT - KERNEL_LOAD_BASE) >> 9
.set ROOT_ENTRY_ROUNDING, FAT_DIRECTORY_ENTRIES_PER_SECTOR - 1
.set SECTOR_ROUNDING, BIOS_SECTOR_BYTES - 1
.set SECTOR_OFFSET_MASK, BIOS_SECTOR_BYTES - 1

io_start:
    cli
    cld
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw $BOOT_SECTOR_BASE, %sp
    sti
    movb %dl, boot_drive

    /* Reject geometry that would make this FAT16 loader address a different
     * root or data area than the filesystem driver. */
    cmpw $BIOS_SECTOR_BYTES, BPB_BYTES_PER_SECTOR
    jne loader_geometry_error
    movb BPB_SECTORS_PER_CLUSTER, %al
    testb %al, %al
    jz loader_geometry_error
    movb %al, %ah
    decb %ah
    testb %ah, %al
    jnz loader_geometry_error
    cmpw $0, BPB_RESERVED_SECTORS
    je loader_geometry_error
    cmpb $0, BPB_FAT_COUNT
    je loader_geometry_error
    cmpw $0, BPB_SECTORS_PER_FAT
    je loader_geometry_error
    movw BPB_ROOT_ENTRIES, %ax
    testw %ax, %ax
    jz loader_geometry_error
    testw $ROOT_ENTRY_ROUNDING, %ax
    jnz loader_geometry_error

    /* Derive the FAT and root geometry from the BPB loaded at 0000:7C00. */
    movzwl BPB_SECTORS_PER_FAT, %eax
    movzbl BPB_FAT_COUNT, %ecx
    imull %ecx, %eax
    movzwl BPB_RESERVED_SECTORS, %ecx
    addl %ecx, %eax
    addl BPB_HIDDEN_SECTORS, %eax
    movl %eax, root_lba
    movzwl BPB_ROOT_ENTRIES, %eax
    addl $ROOT_ENTRY_ROUNDING, %eax
    shrl $4, %eax
    movw %ax, root_sectors
    addl root_lba, %eax
    movl %eax, first_data_lba

scan_root_sector:
    cmpw $0, root_sectors
    je loader_error
    movl root_lba, %eax
    movl %eax, disk_address_packet + EDD_START_LBA_FIELD
    movw $ROOT_SCRATCH_BASE, disk_address_packet + 4
    movw $0, disk_address_packet + EDD_BUFFER_SEGMENT_FIELD
    call read_sector
    jc loader_error
    movw $ROOT_SCRATCH_BASE, %si
    movw $FAT_DIRECTORY_ENTRIES_PER_SECTOR, %dx
scan_root_entry:
    pushw %si
    movw $kernel_system_name, %di
    movw $11, %cx
    repe cmpsb
    popw %si
    je kernel_system_found
    addw $FAT_DIRECTORY_ENTRY_BYTES, %si
    decw %dx
    jnz scan_root_entry
    incl root_lba
    decw root_sectors
    jmp scan_root_sector

kernel_system_found:
    movw FAT_DIRECTORY_FIRST_CLUSTER(%si), %ax
    cmpw $FAT_FIRST_DATA_CLUSTER, %ax
    jb loader_error
    movw %ax, current_cluster
    movl FAT_DIRECTORY_FILE_SIZE(%si), %eax
    testl %eax, %eax
    jz loader_error
    addl $SECTOR_ROUNDING, %eax
    jc loader_error
    shrl $9, %eax
    cmpl $KERNEL_MAX_SECTORS_VALUE, %eax
    ja loader_error
    movw %ax, remaining_sectors
    movw $KERNEL_LOAD_SEGMENT_VALUE, destination_segment

load_cluster:
    movzwl current_cluster, %eax
    cmpw $FAT_FIRST_DATA_CLUSTER, %ax
    jb loader_error
    cmpw $FAT16_LAST_DATA_CLUSTER, %ax
    ja loader_error
    subl $FAT_FIRST_DATA_CLUSTER, %eax
    movzbl BPB_SECTORS_PER_CLUSTER, %ecx
    imull %ecx, %eax
    addl first_data_lba, %eax
    movl %eax, cluster_lba
    movb BPB_SECTORS_PER_CLUSTER, %al
    movb %al, cluster_sectors

load_cluster_sector:
    cmpw $0, remaining_sectors
    je kernel_loaded
    cmpb $0, cluster_sectors
    je read_next_cluster
    movw destination_segment, %ax
    cmpw $KERNEL_LOAD_LIMIT_SEGMENT_VALUE, %ax
    jae loader_error
    movw %ax, disk_address_packet + EDD_BUFFER_SEGMENT_FIELD
    movw $0, disk_address_packet + 4
    movl cluster_lba, %eax
    movl %eax, disk_address_packet + EDD_START_LBA_FIELD
    call read_sector
    jc loader_error
    addw $BIOS_SECTOR_PARAGRAPHS, destination_segment
    incl cluster_lba
    decw remaining_sectors
    decb cluster_sectors
    jmp load_cluster_sector

read_next_cluster:
    movzwl current_cluster, %eax
    shll $1, %eax
    movw %ax, fat_byte_offset
    shrl $9, %eax
    movzwl BPB_RESERVED_SECTORS, %ecx
    addl %ecx, %eax
    addl BPB_HIDDEN_SECTORS, %eax
    movl %eax, disk_address_packet + EDD_START_LBA_FIELD
    movw $ROOT_SCRATCH_BASE, disk_address_packet + 4
    movw $0, disk_address_packet + EDD_BUFFER_SEGMENT_FIELD
    call read_sector
    jc loader_error
    movw fat_byte_offset, %bx
    andw $SECTOR_OFFSET_MASK, %bx
    movw ROOT_SCRATCH_BASE(%bx), %ax
    cmpw $FAT16_END_OF_CHAIN, %ax
    jae loader_error
    movw %ax, current_cluster
    jmp load_cluster

kernel_loaded:
    movb boot_drive, %dl
    ljmp $KERNEL_LOAD_SEGMENT_VALUE, $0x0000

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

loader_geometry_error:
    movw $geometry_error_message, %si
    jmp print_error
loader_error:
    movw $error_message, %si
print_error:
    lodsb
    testb %al, %al
    jz halt
#if CONFIG_X86_DEBUGCON
    outb %al, $QEMU_DEBUG_OUTPUT_PORT
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
    .word 0
    .word 0
    .long 0
    .long 0
root_lba:
    .long 0
first_data_lba:
    .long 0
cluster_lba:
    .long 0
root_sectors:
    .word 0
remaining_sectors:
    .word 0
current_cluster:
    .word 0
destination_segment:
    .word 0
fat_byte_offset:
    .word 0
boot_drive:
    .byte 0
retry_count:
    .byte 0
cluster_sectors:
    .byte 0
kernel_system_name:
    .ascii "DOSKRNL SYS"
error_message:
    .asciz "DOS-C32: DOSKRNL.SYS load failed.\r\n"
geometry_error_message:
    .asciz "DOS-C32: invalid FAT16 boot geometry.\r\n"

.section .note.GNU-stack, "", @progbits

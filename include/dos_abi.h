/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MS-DOS guest-visible and on-disk binary layouts.
 *
 * Layouts are byte-defined rather than native compiler ABI structures. A DOS
 * pointer is always a 16:16 guest value. Native pointers,
 * size_t, uintptr_t and compiler-sized enums must never be added here.
 * Untrusted bytes must still be decoded and validated before use; __packed
 * describes compatibility, it does not make unaligned native access safe.
 */
#ifndef DOSC32_DOS_ABI_H
#define DOSC32_DOS_ABI_H

#include "compiler.h"
#include "types.h"

#define DOS_ABI_OFFSET_OF(type, member) __builtin_offsetof(type, member)

/* A real-mode guest address.  It is not a native C pointer. */
struct dos_far_pointer16 {
	uint16_t offset;
	uint16_t segment;
} __packed;

/* Memory-control block preceding an allocation. */
struct dos_mcb40 {
	uint8_t signature;
	uint16_t owner_psp;
	uint16_t size_paragraphs;
	uint8_t reserved[3];
	uint8_t owner_name[8];
} __packed;

#define DOS_MCB_SIGNATURE_NORMAL 0x4du
#define DOS_MCB_SIGNATURE_END 0x5au
#define DOS_MCB_FREE_OWNER 0u

/* BIOS parameter block beginning at boot-sector offset 11. */
struct dos_bpb40 {
	uint16_t bytes_per_sector;
	uint8_t sectors_per_cluster;
	uint16_t reserved_sectors;
	uint8_t fat_count;
	uint16_t root_entries;
	uint16_t total_sectors16;
	uint8_t media_descriptor;
	uint16_t sectors_per_fat;
	uint16_t sectors_per_track;
	uint16_t heads;
	uint32_t hidden_sectors;
	uint32_t total_sectors32;
	uint8_t extended[6];
} __packed;

/* FAT directory entry. */
struct dos_directory_entry40 {
	uint8_t name[11];
	uint8_t attributes;
	uint16_t code_page;
	uint16_t extended_attribute_cluster;
	uint8_t reserved_attribute;
	uint8_t reserved[5];
	uint16_t modified_time;
	uint16_t modified_date;
	uint16_t first_cluster;
	uint32_t file_size;
} __packed;

/* DOS drive parameter block. */
struct dos_dpb40 {
	uint8_t drive;
	uint8_t unit;
	uint16_t sector_size;
	uint8_t cluster_mask;
	uint8_t cluster_shift;
	uint16_t first_fat;
	uint8_t fat_count;
	uint16_t root_entries;
	uint16_t first_data_sector;
	uint16_t maximum_cluster;
	uint16_t fat_size;
	uint16_t directory_sector;
	struct dos_far_pointer16 driver;
	uint8_t media_descriptor;
	uint8_t first_access;
	struct dos_far_pointer16 next_dpb;
	uint16_t next_free_cluster;
	uint16_t free_cluster_count;
} __packed;

#define DOS_CDS_PATH_LENGTH 67u

/* DOS current-directory list entry. */
struct dos_cds40 {
	uint8_t text[DOS_CDS_PATH_LENGTH];
	uint16_t flags;
	struct dos_far_pointer16 device_or_dpb;
	/* curdir_ID is a local cluster word or the low word of curdir_netID. */
	uint16_t identifier_low;
	uint16_t identifier_high;
	uint16_t user_word;
	uint16_t path_end;
	uint8_t type;
	struct dos_far_pointer16 ifs_header;
	uint8_t filesystem_data[2];
} __packed;

/* SFT block header. Entries begin at byte 6. */
struct dos_sft_table_prefix40 {
	struct dos_far_pointer16 next;
	uint16_t entry_count;
} __packed;

#define DOS_SFT_TABLE_ENTRIES_OFFSET 6u

/* SFTable is the first WORD of entry zero, so SIZE SF is eight bytes. */
struct dos_sft_table40 {
	struct dos_sft_table_prefix40 prefix;
	uint16_t first_entry_word;
} __packed;

/* System file-table entry. */
struct dos_sft_entry40 {
	uint16_t reference_count;
	uint16_t mode;
	uint8_t attributes;
	uint16_t flags;
	struct dos_far_pointer16 device_or_dpb;
	uint16_t first_cluster;
	uint16_t modified_time;
	uint16_t modified_date;
	uint32_t size;
	uint32_t position;
	uint16_t cluster_position;
	uint32_t directory_sector;
	uint8_t directory_position;
	uint8_t name[11];
	struct dos_far_pointer16 sharing_next;
	uint16_t user_id;
	uint16_t process_id;
	uint16_t machine_file_table;
	uint16_t last_cluster;
	struct dos_far_pointer16 ifs_header;
} __packed;

/*
 * DOS system file-control block.
 *
 * The DWORD at offset 16 is fcb_FILSIZ while an FCB is open.  SEARCH
 * FIRST/NEXT also aliases its high word as fcb_DRVBP (FILDIRENT starts at
 * the low word).  Keep it as four guest bytes; interpretation is operation
 * dependent.
 */
struct dos_fcb40 {
	uint8_t drive;
	uint8_t name[8];
	uint8_t extension[3];
	uint16_t current_extent;
	uint16_t record_size;
	uint32_t file_size_or_search_state;
	uint16_t modified_date;
	uint16_t modified_time;
	uint8_t reserved[8];
	uint8_t next_record;
	uint8_t random_record[4];
} __packed;

#define DOS_EXTENDED_FCB_MARKER 0xffu

/* Seven-byte prefix recognized for an extended FCB. */
struct dos_extended_fcb40 {
	uint8_t marker;
	uint8_t reserved[5];
	uint8_t attributes;
	struct dos_fcb40 fcb;
} __packed;

/* Find buffer stored in the caller-selected DTA. */
struct dos_find_buffer40 {
	uint8_t search_drive;
	uint8_t search_name[11];
	uint8_t search_attributes;
	uint16_t last_entry;
	uint16_t directory_start;
	uint8_t network_reserved[4];
	uint8_t found_attributes;
	uint16_t modified_time;
	uint16_t modified_date;
	uint32_t file_size;
	uint8_t packed_name[13];
} __packed;

#define DOS_DTA_FIND_SIZE 43u

/* Process data block through offset 5cH. */
#define DOS_PSP_DEFAULT_HANDLES 20u

struct dos_psp_prefix40 {
	uint16_t exit_instruction;
	uint16_t block_length;
	uint8_t reserved0;
	uint8_t cpm_call[5];
	struct dos_far_pointer16 exit_vector;
	struct dos_far_pointer16 control_c_vector;
	struct dos_far_pointer16 fatal_abort_vector;
	uint16_t parent_psp;
	uint8_t jft[DOS_PSP_DEFAULT_HANDLES];
	uint16_t environment_segment;
	struct dos_far_pointer16 user_stack;
	uint16_t jft_length;
	struct dos_far_pointer16 jft_pointer;
	struct dos_far_pointer16 next_psp;
	uint8_t pad1[0x14];
	uint8_t call_system[5];
	uint8_t pad2[7];
} __packed;

#define DOS_PSP_FIRST_FCB_OFFSET 0x5cu
#define DOS_PSP_SECOND_FCB_OFFSET 0x6cu
#define DOS_PSP_COMMAND_TAIL_OFFSET 0x80u
#define DOS_PSP_SIZE 0x100u
#define DOS_COMMAND_TAIL_BYTES 127u

/* The first byte is a count; the used data is conventionally CR-terminated. */
struct dos_command_tail40 {
	uint8_t length;
	uint8_t data[DOS_COMMAND_TAIL_BYTES];
} __packed;

/*
 * The two default FCBs and command tail overlap in the real PSP.  Separate
 * union views preserve that historical aliasing instead of inventing three
 * sequential objects.  The command tail is also the process's default DTA.
 */
struct dos_psp_first_fcb_view40 {
	struct dos_fcb40 first_fcb;
	uint8_t remaining[DOS_PSP_SIZE - DOS_PSP_FIRST_FCB_OFFSET -
			  sizeof(struct dos_fcb40)];
} __packed;

struct dos_psp_second_fcb_view40 {
	uint8_t before_second[DOS_PSP_SECOND_FCB_OFFSET -
			      DOS_PSP_FIRST_FCB_OFFSET];
	struct dos_fcb40 second_fcb;
	uint8_t remaining[DOS_PSP_SIZE - DOS_PSP_SECOND_FCB_OFFSET -
			  sizeof(struct dos_fcb40)];
} __packed;

struct dos_psp_command_tail_view40 {
	uint8_t before_tail[DOS_PSP_COMMAND_TAIL_OFFSET -
			    DOS_PSP_FIRST_FCB_OFFSET];
	struct dos_command_tail40 command_tail;
} __packed;

union dos_psp_compatibility_area40 {
	uint8_t bytes[DOS_PSP_SIZE - DOS_PSP_FIRST_FCB_OFFSET];
	struct dos_psp_first_fcb_view40 first;
	struct dos_psp_second_fcb_view40 second;
	struct dos_psp_command_tail_view40 command;
};

struct dos_psp40 {
	struct dos_psp_prefix40 prefix;
	union dos_psp_compatibility_area40 compatibility;
} __packed;

/* Device and request headers. */
struct dos_device_header40 {
	struct dos_far_pointer16 next;
	uint16_t attributes;
	uint16_t strategy_offset;
	uint16_t interrupt_offset;
	uint8_t name_or_units[8];
} __packed;

struct dos_device_request_header40 {
	uint8_t length;
	uint8_t unit;
	uint8_t function;
	uint16_t status;
	uint8_t queue_reserved[8];
} __packed;

#define DOS_DEVICE_ATTRIBUTE_CHARACTER 0x8000u
#define DOS_DEVICE_ATTRIBUTE_IOCTL 0x4000u
#define DOS_BLOCK_DEVICE_ATTRIBUTE_EXTENDED 0x0002u

#define DOS_DEVICE_STATUS_ERROR 0x8000u
#define DOS_DEVICE_STATUS_BUSY 0x0200u
#define DOS_DEVICE_STATUS_DONE 0x0100u
#define DOS_DEVICE_STATUS_ERROR_MASK 0x00ffu

#define DOS_DEVICE_REQUEST_INIT 0u
#define DOS_DEVICE_REQUEST_MEDIA_CHECK 1u
#define DOS_DEVICE_REQUEST_BUILD_BPB 2u
#define DOS_DEVICE_REQUEST_IOCTL_READ 3u
#define DOS_DEVICE_REQUEST_READ 4u
#define DOS_DEVICE_REQUEST_WRITE 8u
#define DOS_DEVICE_REQUEST_WRITE_VERIFY 9u
#define DOS_DEVICE_REQUEST_IOCTL_WRITE 12u
#define DOS_DEVICE_REQUEST_GENERIC_IOCTL 19u

/* Function 0 initialization request, 26 bytes. */
struct dos_device_init_request40 {
	struct dos_device_request_header40 header;
	uint8_t unit_count;
	struct dos_far_pointer16 break_address;
	struct dos_far_pointer16 bpb_array;
	uint8_t first_drive;
	uint8_t reserved[3];
} __packed;

/* Function 1 media-check request, 15 bytes. */
struct dos_device_media_check_request40 {
	struct dos_device_request_header40 header;
	uint8_t media_descriptor;
	uint8_t result;
} __packed;

/* Function 2 BPB request, 22 bytes. */
struct dos_device_build_bpb_request40 {
	struct dos_device_request_header40 header;
	uint8_t media_descriptor;
	struct dos_far_pointer16 transfer_buffer;
	struct dos_far_pointer16 bpb;
} __packed;

/* Functions 4/8/9 read/write request, 22 bytes. */
struct dos_device_rw_request40 {
	struct dos_device_request_header40 header;
	uint8_t media_descriptor;
	struct dos_far_pointer16 transfer_buffer;
	uint16_t sector_count;
	uint16_t start_sector;
} __packed;

/*
 * An extended-driver read/write request grows from 22 to 30 bytes,
 * writes ffffH to start_sector, and supplies the 32-bit sector at byte 26.
 */
struct dos_device_extended_rw_request40 {
	struct dos_device_rw_request40 legacy;
	uint32_t volume_id;
	uint32_t start_sector32;
} __packed;

/* Function 19 generic IOCTL request. */
struct dos_device_generic_ioctl_request40 {
	struct dos_device_request_header40 header;
	uint8_t major_function;
	uint8_t minor_function;
	uint16_t si;
	uint16_t di;
	struct dos_far_pointer16 packet;
} __packed;

/* Absolute 32-bit read/write and media-ID structures. */
struct dos_absolute_rw40 {
	uint32_t sector;
	uint16_t sector_count;
	struct dos_far_pointer16 buffer;
} __packed;

struct dos_media_id40 {
	uint16_t information_level;
	uint32_t serial;
	uint8_t label[11];
	uint8_t filesystem[8];
} __packed;

/* EXEC0, EXEC1 and EXEC3 parameter blocks. */
#define DOS_EXEC_LOAD_AND_EXECUTE 0u
#define DOS_EXEC_LOAD_ONLY 1u
/* EXEC accepts AL=3 and explicitly rejects AL=2. */
#define DOS_EXEC_OVERLAY 3u
/* Values below are bit tests on an already validated subfunction. */
#define DOS_EXEC_FUNCTION_NO_EXECUTE_BIT 0x01u
#define DOS_EXEC_FUNCTION_OVERLAY_BIT 0x02u

static inline bool dos_exec_subfunction_is_valid(uint8_t subfunction)
{
	return subfunction == DOS_EXEC_LOAD_AND_EXECUTE ||
	       subfunction == DOS_EXEC_LOAD_ONLY ||
	       subfunction == DOS_EXEC_OVERLAY;
}

struct dos_exec_parameter_block40 {
	uint16_t environment_segment;
	struct dos_far_pointer16 command_line;
	struct dos_far_pointer16 first_fcb;
	struct dos_far_pointer16 second_fcb;
} __packed;

struct dos_exec_load_result40 {
	struct dos_exec_parameter_block40 parameters;
	uint16_t initial_sp;
	uint16_t initial_ss;
	uint16_t initial_ip;
	uint16_t initial_cs;
} __packed;

struct dos_exec_overlay_block40 {
	uint16_t load_segment;
	uint16_t relocation_factor;
} __packed;

/* MZ executable header fields. */
struct dos_mz_header40 {
	uint16_t signature;
	uint16_t bytes_in_last_page;
	uint16_t pages;
	uint16_t relocation_count;
	uint16_t header_paragraphs;
	uint16_t minimum_extra_paragraphs;
	uint16_t maximum_extra_paragraphs;
	uint16_t initial_ss;
	uint16_t initial_sp;
	uint16_t checksum;
	uint16_t initial_ip;
	uint16_t initial_cs;
	uint16_t relocation_table_offset;
	uint16_t overlay_number;
	uint32_t symbol_table_offset;
} __packed;

/* An MZ relocation item is the offset:segment pair stored in the file. */
struct dos_mz_relocation40 {
	uint16_t offset;
	uint16_t segment;
} __packed;

/* A symbol entry has a one-byte length followed by up to 255 bytes. */
struct dos_mz_symbol_entry40 {
	uint32_t value;
	uint16_t type;
	uint8_t name_length;
	uint8_t name[255];
} __packed;

#define DOS_MZ_SIGNATURE 0x5a4du
#define DOS_MZ_OLD_SIGNATURE 0x4d5au

/* Size invariants: these must remain identical under -m32 and -m64. */
static_assert_expression(sizeof(struct dos_far_pointer16) == 4,
	"DOS far pointer ABI changed");
static_assert_expression(sizeof(struct dos_mcb40) == 16,
	"DOS MCB ABI changed");
static_assert_expression(sizeof(struct dos_bpb40) == 31,
	"DOS BPB ABI changed");
static_assert_expression(sizeof(struct dos_directory_entry40) == 32,
	"DOS directory entry ABI changed");
static_assert_expression(sizeof(struct dos_dpb40) == 33,
	"DOS DPB ABI changed");
static_assert_expression(sizeof(struct dos_cds40) == 88,
	"DOS CDS ABI changed");
static_assert_expression(sizeof(struct dos_sft_table_prefix40) == 6,
	"DOS SFT table prefix ABI changed");
static_assert_expression(sizeof(struct dos_sft_table40) == 8,
	"DOS SF structure changed");
static_assert_expression(sizeof(struct dos_sft_entry40) == 59,
	"DOS SFT entry ABI changed");
static_assert_expression(sizeof(struct dos_fcb40) == 37,
	"DOS FCB ABI changed");
static_assert_expression(sizeof(struct dos_extended_fcb40) == 44,
	"DOS extended FCB ABI changed");
static_assert_expression(sizeof(struct dos_find_buffer40) == DOS_DTA_FIND_SIZE,
	"DOS find/DTA ABI changed");
static_assert_expression(sizeof(struct dos_psp_prefix40) ==
			 DOS_PSP_FIRST_FCB_OFFSET,
	"DOS PSP prefix ABI changed");
static_assert_expression(sizeof(struct dos_command_tail40) == 0x80,
	"DOS command-tail ABI changed");
static_assert_expression(sizeof(union dos_psp_compatibility_area40) == 0xa4,
	"DOS PSP overlay area ABI changed");
static_assert_expression(sizeof(struct dos_psp40) == DOS_PSP_SIZE,
	"DOS PSP ABI changed");
static_assert_expression(sizeof(struct dos_device_header40) == 18,
	"DOS device header ABI changed");
static_assert_expression(sizeof(struct dos_device_request_header40) == 13,
	"DOS device request header ABI changed");
static_assert_expression(sizeof(struct dos_device_init_request40) == 26,
	"DOS device INIT request ABI changed");
static_assert_expression(sizeof(struct dos_device_media_check_request40) == 15,
	"DOS device media-check request ABI changed");
static_assert_expression(sizeof(struct dos_device_build_bpb_request40) == 22,
	"DOS device BPB request ABI changed");
static_assert_expression(sizeof(struct dos_device_rw_request40) == 22,
	"DOS device read/write request ABI changed");
static_assert_expression(sizeof(struct dos_device_extended_rw_request40) == 30,
	"DOS extended device read/write request ABI changed");
static_assert_expression(sizeof(struct dos_device_generic_ioctl_request40) == 23,
	"DOS generic IOCTL request ABI changed");
static_assert_expression(sizeof(struct dos_absolute_rw40) == 10,
	"DOS absolute read/write ABI changed");
static_assert_expression(sizeof(struct dos_media_id40) == 25,
	"DOS media-ID ABI changed");
static_assert_expression(sizeof(struct dos_exec_parameter_block40) == 14,
	"DOS EXEC parameter ABI changed");
static_assert_expression(sizeof(struct dos_exec_load_result40) == 22,
	"DOS EXEC load-result ABI changed");
static_assert_expression(sizeof(struct dos_exec_overlay_block40) == 4,
	"DOS EXEC overlay ABI changed");
static_assert_expression(sizeof(struct dos_mz_header40) == 32,
	"DOS MZ header ABI changed");
static_assert_expression(sizeof(struct dos_mz_relocation40) == 4,
	"DOS MZ relocation ABI changed");
static_assert_expression(sizeof(struct dos_mz_symbol_entry40) == 262,
	"DOS MZ symbol-entry ABI changed");

/* High-risk fixed offsets used directly by DOS and 16-bit applications. */
static_assert_expression(DOS_ABI_OFFSET_OF(struct dos_mcb40, owner_psp) == 1,
	"MCB owner offset changed");
static_assert_expression(DOS_ABI_OFFSET_OF(struct dos_mcb40, owner_name) == 8,
	"MCB name offset changed");
static_assert_expression(DOS_ABI_OFFSET_OF(struct dos_dpb40, driver) == 19,
	"DPB driver offset changed");
static_assert_expression(DOS_ABI_OFFSET_OF(struct dos_dpb40, next_dpb) == 25,
	"DPB link offset changed");
static_assert_expression(DOS_ABI_OFFSET_OF(struct dos_cds40, flags) == 67,
	"CDS flag offset changed");
static_assert_expression(DOS_ABI_OFFSET_OF(struct dos_cds40, identifier_low) ==
			 73,
	"CDS identifier offset changed");
static_assert_expression(DOS_ABI_OFFSET_OF(struct dos_cds40, ifs_header) == 82,
	"CDS IFS-header offset changed");
static_assert_expression(DOS_ABI_OFFSET_OF(struct dos_sft_table40,
					  first_entry_word) ==
			 DOS_SFT_TABLE_ENTRIES_OFFSET,
	"SFT entry-array offset changed");
/* This field ordering is required for MS Windows compatibility. */
static_assert_expression(DOS_ABI_OFFSET_OF(struct dos_sft_entry40, flags) == 5,
	"SFT flag offset changed");
static_assert_expression(DOS_ABI_OFFSET_OF(struct dos_sft_entry40,
					  device_or_dpb) == 7,
	"SFT device/DPB offset changed");
static_assert_expression(DOS_ABI_OFFSET_OF(struct dos_sft_entry40, size) == 17,
	"SFT size offset changed");
static_assert_expression(DOS_ABI_OFFSET_OF(struct dos_sft_entry40, position) == 21,
	"SFT position offset changed");
static_assert_expression(DOS_ABI_OFFSET_OF(struct dos_sft_entry40,
					  cluster_position) == 25,
	"SFT filesystem-data offset changed");
static_assert_expression(DOS_ABI_OFFSET_OF(struct dos_sft_entry40, name) == 32,
	"SFT name offset changed");
static_assert_expression(DOS_ABI_OFFSET_OF(struct dos_sft_entry40,
					  sharing_next) == 43,
	"SFT sharing-chain offset changed");
static_assert_expression(DOS_ABI_OFFSET_OF(struct dos_sft_entry40, last_cluster) ==
			 53,
	"SFT last-cluster offset changed");
static_assert_expression(DOS_ABI_OFFSET_OF(struct dos_sft_entry40, ifs_header) ==
			 55,
	"SFT IFS-header offset changed");
static_assert_expression(DOS_ABI_OFFSET_OF(struct dos_psp_prefix40, cpm_call) ==
			 5,
	"PSP CP/M entry offset changed");
static_assert_expression(DOS_ABI_OFFSET_OF(struct dos_psp_prefix40, exit_vector) ==
			 0x0a,
	"PSP exit-vector offset changed");
static_assert_expression(DOS_ABI_OFFSET_OF(struct dos_psp_prefix40, parent_psp) ==
			 0x16,
	"PSP parent offset changed");
static_assert_expression(DOS_ABI_OFFSET_OF(struct dos_psp_prefix40, jft) == 0x18,
	"PSP JFT offset changed");
static_assert_expression(DOS_ABI_OFFSET_OF(struct dos_psp_prefix40,
					  environment_segment) == 0x2c,
	"PSP environment offset changed");
static_assert_expression(DOS_ABI_OFFSET_OF(struct dos_psp_prefix40, user_stack) ==
			 0x2e,
	"PSP saved-stack offset changed");
static_assert_expression(DOS_ABI_OFFSET_OF(struct dos_psp_prefix40, jft_length) ==
			 0x32,
	"PSP JFT length offset changed");
static_assert_expression(DOS_ABI_OFFSET_OF(struct dos_psp_prefix40, jft_pointer) ==
			 0x34,
	"PSP JFT pointer offset changed");
static_assert_expression(DOS_ABI_OFFSET_OF(struct dos_psp_prefix40, next_psp) ==
			 0x38,
	"PSP next-PDB offset changed");
static_assert_expression(DOS_ABI_OFFSET_OF(struct dos_psp_prefix40, call_system) ==
			 0x50,
	"PSP INT 21 entry offset changed");
static_assert_expression(DOS_ABI_OFFSET_OF(struct dos_psp40,
					  compatibility.first.first_fcb) ==
			 DOS_PSP_FIRST_FCB_OFFSET,
	"PSP first FCB offset changed");
static_assert_expression(DOS_ABI_OFFSET_OF(struct dos_psp40,
					  compatibility.second.second_fcb) ==
			 DOS_PSP_SECOND_FCB_OFFSET,
	"PSP second FCB offset changed");
static_assert_expression(DOS_ABI_OFFSET_OF(struct dos_psp40,
					  compatibility.command.command_tail) ==
			 DOS_PSP_COMMAND_TAIL_OFFSET,
	"PSP command-tail/default-DTA offset changed");
static_assert_expression(DOS_ABI_OFFSET_OF(struct dos_fcb40, current_extent) == 12,
	"FCB extent offset changed");
static_assert_expression(DOS_ABI_OFFSET_OF(struct dos_fcb40,
					  file_size_or_search_state) == 16,
	"FCB size/search-state offset changed");
static_assert_expression(DOS_ABI_OFFSET_OF(struct dos_fcb40, reserved) == 24,
	"FCB reserved offset changed");
static_assert_expression(DOS_ABI_OFFSET_OF(struct dos_fcb40, random_record) == 33,
	"FCB random-record offset changed");
static_assert_expression(DOS_ABI_OFFSET_OF(struct dos_extended_fcb40, fcb) == 7,
	"extended FCB payload offset changed");
static_assert_expression(DOS_ABI_OFFSET_OF(struct dos_find_buffer40,
					  found_attributes) == 21,
	"find/DTA result offset changed");
static_assert_expression(DOS_ABI_OFFSET_OF(struct dos_find_buffer40, packed_name) ==
			 30,
	"find/DTA packed-name offset changed");
static_assert_expression(DOS_ABI_OFFSET_OF(struct dos_device_header40,
					  strategy_offset) == 6,
	"device strategy offset changed");
static_assert_expression(DOS_ABI_OFFSET_OF(struct dos_device_header40,
					  interrupt_offset) == 8,
	"device interrupt offset changed");
static_assert_expression(DOS_ABI_OFFSET_OF(struct dos_device_request_header40,
					  status) == 3,
	"device request status offset changed");
static_assert_expression(DOS_ABI_OFFSET_OF(struct dos_device_rw_request40,
					  transfer_buffer) == 14,
	"device request transfer offset changed");
static_assert_expression(DOS_ABI_OFFSET_OF(struct dos_device_rw_request40,
					  sector_count) == 18,
	"device request count offset changed");
static_assert_expression(DOS_ABI_OFFSET_OF(struct dos_device_rw_request40,
					  start_sector) == 20,
	"device request start-sector offset changed");
static_assert_expression(DOS_ABI_OFFSET_OF(struct dos_device_extended_rw_request40,
					  start_sector32) == 26,
	"extended request 32-bit sector offset changed");
static_assert_expression(DOS_ABI_OFFSET_OF(struct dos_exec_load_result40,
					  initial_sp) == 14,
	"EXEC load-result SP offset changed");
static_assert_expression(DOS_ABI_OFFSET_OF(struct dos_exec_load_result40,
					  initial_cs) == 20,
	"EXEC load-result CS offset changed");
static_assert_expression(DOS_ABI_OFFSET_OF(struct dos_mz_header40,
					  relocation_table_offset) == 24,
	"MZ relocation-table offset changed");
static_assert_expression(DOS_ABI_OFFSET_OF(struct dos_mz_header40,
					  symbol_table_offset) == 28,
	"MZ symbol-table offset changed");

#endif

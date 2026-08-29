/* SPDX-License-Identifier: GPL-2.0-only */
/* Stable public volume boundary for the DOS-C32 I/O Manager. */
#ifndef DOSC32_IOMGR_H
#define DOSC32_IOMGR_H

#include "address.h"
#include "block_device.h"
#include "compiler.h"
#include "types.h"

#define IOMGR_VOLUME_HANDLE_INVALID KERNEL_OBJECT_HANDLE_INVALID
#define IOMGR_FILE_HANDLE_INVALID KERNEL_OBJECT_HANDLE_INVALID
#define IOMGR_SEARCH_HANDLE_INVALID KERNEL_OBJECT_HANDLE_INVALID
#define IOMGR_NAME_MAX_BYTES 255u
#define IOMGR_PATH_MAX_BYTES 4096u
#define IOMGR_MOUNT_READ_ONLY (1u << 0)
#define IOMGR_VOLUME_CAP_READ (1u << 0)
#define IOMGR_VOLUME_CAP_WRITE (1u << 1)
#define IOMGR_VOLUME_CAP_LONG_NAMES (1u << 2)
#define IOMGR_VOLUME_CAP_CASE_PRESERVING (1u << 3)
#define IOMGR_VOLUME_CAP_CASE_SENSITIVE (1u << 4)

typedef kernel_object_handle_t iomgr_volume_handle_t;
typedef kernel_object_handle_t iomgr_file_handle_t;
typedef kernel_object_handle_t iomgr_search_handle_t;

enum iomgr_status {
	IOMGR_OK = 0,
	IOMGR_INVALID_ARGUMENT,
	IOMGR_NOT_INITIALIZED,
	IOMGR_ALREADY_INITIALIZED,
	IOMGR_NO_SLOT,
	IOMGR_DUPLICATE_DRIVER,
	IOMGR_NO_DRIVER,
	IOMGR_STALE_HANDLE,
	IOMGR_READ_ONLY,
	IOMGR_BUSY,
	IOMGR_CORRUPT,
	IOMGR_UNSUPPORTED,
	IOMGR_IO_ERROR,
	IOMGR_UNCERTAIN,
	IOMGR_POISONED,
	IOMGR_NOT_FOUND,
	IOMGR_NOT_DIRECTORY,
	IOMGR_IS_DIRECTORY,
	IOMGR_INVALID_NAME,
	IOMGR_ALREADY_EXISTS,
	IOMGR_NO_SPACE,
	IOMGR_END_OF_SEARCH
};

#define IOMGR_NODE_READ_ONLY (1u << 0)
#define IOMGR_NODE_HIDDEN (1u << 1)
#define IOMGR_NODE_SYSTEM (1u << 2)
#define IOMGR_NODE_VOLUME_LABEL (1u << 3)
#define IOMGR_NODE_DIRECTORY (1u << 4)
#define IOMGR_NODE_ARCHIVE (1u << 5)

/* Transient counted UTF-8 path. It is never retained by the manager/driver. */
struct iomgr_path {
	const uint8_t *bytes;
	size_t length;
};

struct iomgr_timestamp {
	uint16_t year;
	uint8_t month;
	uint8_t day;
	uint8_t hour;
	uint8_t minute;
	uint8_t second;
	uint8_t centiseconds;
};

struct iomgr_node_info {
	uint64_t size;
	uint32_t attributes;
	struct iomgr_timestamp modified;
} __aligned(8);

#define IOMGR_FILE_UPDATE_MODIFIED (1ull << 0)

/* Generic metadata update. Compatibility ABIs must translate into this form
 * before crossing the I/O Manager boundary. */
struct iomgr_file_update {
	uint64_t valid;
	struct iomgr_timestamp modified;
} __aligned(8);

struct iomgr_directory_entry {
	struct iomgr_node_info info;
	uint16_t name_length;
	uint8_t name[IOMGR_NAME_MAX_BYTES + 1u];
} __aligned(8);

struct iomgr_space_info {
	uint64_t total_bytes;
	uint64_t free_bytes;
	uint32_t allocation_unit_bytes;
	uint32_t reserved;
} __aligned(8);

struct iomgr_mount_request {
	block_device_handle_t device;
	block_lba_t first_lba;
	block_lba_t sector_count;
	uint32_t flags;
	uint32_t reserved;
} __aligned(8);

struct iomgr_volume_info {
	uint64_t driver_identity;
	block_device_handle_t device;
	block_lba_t first_lba;
	block_lba_t sector_count;
	uint32_t capabilities;
	uint16_t maximum_name_units;
	uint16_t reserved;
} __aligned(8);

enum iomgr_status iomgr_initialize(void) __must_check;
enum iomgr_status
iomgr_mount(const struct iomgr_mount_request *request,
	    iomgr_volume_handle_t *volume) __must_check;
enum iomgr_status iomgr_unmount(iomgr_volume_handle_t volume) __must_check;
enum iomgr_status
iomgr_get_volume_info(iomgr_volume_handle_t volume,
		      struct iomgr_volume_info *info) __must_check;
enum iomgr_status
iomgr_stat(iomgr_volume_handle_t volume, const struct iomgr_path *path,
	   struct iomgr_node_info *info) __must_check;
enum iomgr_status
iomgr_open_file(iomgr_volume_handle_t volume, const struct iomgr_path *path,
		struct iomgr_node_info *info,
		iomgr_file_handle_t *file) __must_check;
enum iomgr_status
iomgr_create_file(iomgr_volume_handle_t volume, const struct iomgr_path *path,
		  uint32_t attributes, struct iomgr_node_info *info,
		  iomgr_file_handle_t *file) __must_check;
enum iomgr_status
iomgr_read_file(iomgr_file_handle_t file, uint64_t offset,
		uint8_t *destination, size_t capacity, size_t count,
		size_t *bytes_read) __must_check;
enum iomgr_status
iomgr_write_file(iomgr_file_handle_t file, uint64_t offset,
		 const uint8_t *source, size_t source_capacity, size_t count,
		 size_t *bytes_written) __must_check;
enum iomgr_status iomgr_get_file_info(iomgr_file_handle_t file,
				      struct iomgr_node_info *info) __must_check;
enum iomgr_status iomgr_set_file_info(
	iomgr_file_handle_t file,
	const struct iomgr_file_update *update) __must_check;
enum iomgr_status iomgr_close_file(iomgr_file_handle_t file) __must_check;
enum iomgr_status
iomgr_open_search(iomgr_volume_handle_t volume,
		  const struct iomgr_path *pattern, uint32_t attributes,
		  iomgr_search_handle_t *search) __must_check;
enum iomgr_status
iomgr_search_next(iomgr_search_handle_t search,
		  struct iomgr_directory_entry *entry) __must_check;
enum iomgr_status iomgr_close_search(iomgr_search_handle_t search) __must_check;
enum iomgr_status
iomgr_query_space(iomgr_volume_handle_t volume, bool count_free,
		  struct iomgr_space_info *info) __must_check;
enum iomgr_status
iomgr_create_directory(iomgr_volume_handle_t volume,
		       const struct iomgr_path *path) __must_check;
enum iomgr_status iomgr_rename(iomgr_volume_handle_t volume,
			       const struct iomgr_path *old_path,
			       const struct iomgr_path *new_path) __must_check;

static_assert_expression(sizeof(iomgr_volume_handle_t) == 8u,
			 "I/O Manager volume handles must remain 64-bit");
static_assert_expression(sizeof(iomgr_file_handle_t) == 8u,
			 "I/O Manager file handles must remain 64-bit");
static_assert_expression(sizeof(iomgr_search_handle_t) == 8u,
			 "I/O Manager search handles must remain 64-bit");
static_assert_expression(sizeof(struct iomgr_mount_request) == 32u,
			 "I/O Manager mount request layout changed");
static_assert_expression(sizeof(struct iomgr_volume_info) == 40u,
			 "I/O Manager volume info layout changed");
static_assert_expression(sizeof(struct iomgr_timestamp) == 8u,
			 "I/O Manager timestamp layout changed");
static_assert_expression(sizeof(struct iomgr_node_info) == 24u,
			 "I/O Manager node info layout changed");
static_assert_expression(sizeof(struct iomgr_file_update) == 16u,
			 "I/O Manager file update layout changed");
static_assert_expression(sizeof(struct iomgr_directory_entry) == 288u,
			 "I/O Manager directory entry layout changed");
static_assert_expression(sizeof(struct iomgr_space_info) == 24u,
			 "I/O Manager space info layout changed");

#endif

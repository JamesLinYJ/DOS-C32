/* SPDX-License-Identifier: GPL-2.0-only */
/* Private-to-kernel driver registration boundary for the I/O Manager. */
#ifndef DOSC32_IOMGR_DRIVER_H
#define DOSC32_IOMGR_DRIVER_H

#include "iomgr.h"

#define IOMGR_DRIVER_ABI_VERSION 1u

enum iomgr_probe_result {
	IOMGR_PROBE_NO_MATCH = 0,
	IOMGR_PROBE_MATCH,
	IOMGR_PROBE_CORRUPT,
	IOMGR_PROBE_UNSUPPORTED,
	IOMGR_PROBE_IO_ERROR
};

enum iomgr_driver_mount_status {
	IOMGR_DRIVER_MOUNT_OK = 0,
	IOMGR_DRIVER_MOUNT_CORRUPT,
	IOMGR_DRIVER_MOUNT_UNSUPPORTED,
	IOMGR_DRIVER_MOUNT_NO_RESOURCES,
	IOMGR_DRIVER_MOUNT_IO_ERROR,
	IOMGR_DRIVER_MOUNT_UNCERTAIN
};

enum iomgr_driver_unmount_status {
	IOMGR_DRIVER_UNMOUNT_CLEAN = 0,
	IOMGR_DRIVER_UNMOUNT_BUSY,
	IOMGR_DRIVER_UNMOUNT_UNCERTAIN
};

struct iomgr_driver_mount_result {
	kernel_object_handle_t volume_context;
	uint32_t capabilities;
	uint16_t maximum_name_units;
	uint16_t reserved;
} __aligned(8);

struct iomgr_driver_named_ops {
	enum iomgr_status (*stat)(kernel_object_handle_t volume_context,
				  const struct iomgr_path *path,
				  struct iomgr_node_info *info);
	enum iomgr_status (*open_file)(kernel_object_handle_t volume_context,
				       const struct iomgr_path *path,
				       kernel_object_handle_t *file_context,
				       struct iomgr_node_info *info);
	enum iomgr_status (*create_file)(
		kernel_object_handle_t volume_context,
		const struct iomgr_path *path, uint32_t attributes,
		iomgr_volume_handle_t volume,
		kernel_object_handle_t *file_context,
		struct iomgr_node_info *info);
	enum iomgr_status (*read_file)(kernel_object_handle_t volume_context,
				       kernel_object_handle_t file_context,
				       uint64_t offset, uint8_t *destination,
				       size_t capacity, size_t count,
				       size_t *bytes_read);
	enum iomgr_status (*write_file)(
		kernel_object_handle_t volume_context,
		kernel_object_handle_t file_context,
		iomgr_volume_handle_t volume, uint64_t offset,
		const uint8_t *source, size_t source_capacity, size_t count,
		size_t *bytes_written);
	enum iomgr_status (*get_file_info)(
		kernel_object_handle_t volume_context,
		kernel_object_handle_t file_context,
		struct iomgr_node_info *info);
	enum iomgr_status (*set_file_info)(
		kernel_object_handle_t volume_context,
		kernel_object_handle_t file_context,
		iomgr_volume_handle_t volume,
		const struct iomgr_file_update *update);
	enum iomgr_status (*close_file)(kernel_object_handle_t volume_context,
					kernel_object_handle_t file_context);
	enum iomgr_status (*open_search)(kernel_object_handle_t volume_context,
					 const struct iomgr_path *pattern,
					 uint32_t attributes,
					 kernel_object_handle_t *search_context);
	enum iomgr_status (*search_next)(kernel_object_handle_t volume_context,
					 kernel_object_handle_t search_context,
					 struct iomgr_directory_entry *entry);
	enum iomgr_status (*close_search)(kernel_object_handle_t volume_context,
					  kernel_object_handle_t search_context);
	enum iomgr_status (*query_space)(kernel_object_handle_t volume_context,
					 bool count_free,
					 struct iomgr_space_info *info);
	enum iomgr_status (*create_directory)(
		kernel_object_handle_t volume_context,
		const struct iomgr_path *path, iomgr_volume_handle_t volume);
	enum iomgr_status (*rename)(kernel_object_handle_t volume_context,
				    const struct iomgr_path *old_path,
				    const struct iomgr_path *new_path,
				    iomgr_volume_handle_t volume);
};

struct iomgr_driver_ops {
	uint32_t abi_version;
	uint32_t reserved;
	uint64_t identity;
	kernel_object_handle_t context;
	enum iomgr_probe_result (*probe)(
		kernel_object_handle_t context,
		const struct iomgr_mount_request *request);
	enum iomgr_driver_mount_status (*mount)(
		kernel_object_handle_t context,
		const struct iomgr_mount_request *request,
		struct iomgr_driver_mount_result *result);
	enum iomgr_driver_unmount_status (*unmount)(
		kernel_object_handle_t context,
		kernel_object_handle_t volume_context);
	const struct iomgr_driver_named_ops *named;
};

enum iomgr_status
iomgr_register_driver(const struct iomgr_driver_ops *ops) __must_check;
enum iomgr_status
iomgr_quarantine_volume(iomgr_volume_handle_t volume) __must_check;
/*
 * Filesystem drivers use this only to recover their own private mounted-volume
 * context from a validated public handle.  A caller cannot request another
 * driver's context by guessing a handle or identity.
 */
enum iomgr_status iomgr_get_driver_volume_context(
	iomgr_volume_handle_t volume, uint64_t expected_driver_identity,
	kernel_object_handle_t *volume_context) __must_check;

static_assert_expression(sizeof(struct iomgr_driver_mount_result) == 16u,
			 "I/O Manager driver mount result layout changed");

#endif

/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DOSC32_STORAGE_FAT_INTERNAL_H
#define DOSC32_STORAGE_FAT_INTERNAL_H

#include "fat_driver.h"
#include "iomgr_driver.h"

enum iomgr_status fat_driver_snapshot_from_context(
	kernel_object_handle_t volume_context,
	struct fat_driver_volume_snapshot *snapshot) __must_check;
enum iomgr_status fat_driver_get_allocation_hint(
	kernel_object_handle_t volume_context, uint32_t *cluster) __must_check;
enum iomgr_status fat_driver_set_allocation_hint(
	kernel_object_handle_t volume_context, uint32_t cluster) __must_check;
enum iomgr_status fat_driver_get_free_clusters(
	kernel_object_handle_t volume_context, bool *known,
	uint32_t *free_clusters) __must_check;
enum iomgr_status fat_driver_publish_free_clusters(
	kernel_object_handle_t volume_context,
	uint32_t free_clusters) __must_check;
enum iomgr_status fat_driver_adjust_free_clusters(
	kernel_object_handle_t volume_context, int32_t delta) __must_check;
enum iomgr_status fat_short_name_encode(const uint8_t *component,
					size_t length,
					uint8_t output[11]) __must_check;
enum iomgr_status fat_create_directory(
	kernel_object_handle_t volume_context, const struct iomgr_path *path,
	iomgr_volume_handle_t volume) __must_check;
enum iomgr_status fat_rename(kernel_object_handle_t volume_context,
			     const struct iomgr_path *old_path,
			     const struct iomgr_path *new_path,
			     iomgr_volume_handle_t volume) __must_check;

struct fat_created_file {
	block_lba_t directory_lba;
	uint32_t first_cluster;
	uint32_t size;
	uint32_t attributes;
	uint16_t directory_offset;
	uint16_t reserved;
} __aligned(8);

enum iomgr_status fat_create_file(
	kernel_object_handle_t volume_context, const struct iomgr_path *path,
	uint32_t attributes, iomgr_volume_handle_t volume,
	struct fat_created_file *created) __must_check;

struct fat_write_result {
	uint32_t first_cluster;
	uint32_t size;
	uint32_t cursor_cluster;
	uint32_t cursor_cluster_index;
};

enum iomgr_status fat_write_file_sector(
	kernel_object_handle_t volume_context, iomgr_volume_handle_t volume,
	uint32_t first_cluster, uint32_t size, block_lba_t directory_lba,
	uint16_t directory_offset, uint32_t cursor_cluster,
	uint32_t cursor_cluster_index, bool cursor_valid, uint64_t offset,
	const uint8_t *source, size_t source_capacity, size_t count,
	struct fat_write_result *result) __must_check;
enum iomgr_status fat_set_file_modified(
	kernel_object_handle_t volume_context, iomgr_volume_handle_t volume,
	block_lba_t directory_lba, uint16_t directory_offset,
	uint32_t expected_first_cluster, uint32_t expected_size,
	const struct iomgr_timestamp *modified) __must_check;

extern const struct iomgr_driver_named_ops fat_named_ops;

#endif

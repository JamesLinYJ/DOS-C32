// SPDX-License-Identifier: GPL-2.0-only
/* Freestanding I/O Manager sector transaction failure tests. */
#include "iomgr_driver.h"
#include "iomgr_transaction.h"
#include "test_entry.h"

#define TEST_DEVICE ((block_device_handle_t)0x54584e44u)
#define TEST_DRIVER_ID 0x54584e4452495631ull
#define TEST_DRIVER_CONTEXT ((kernel_object_handle_t)0x111u)
#define TEST_VOLUME_CONTEXT ((kernel_object_handle_t)0x222u)
#define TEST_FIRST_LBA 10u
#define TEST_SECTORS 8u

static union block_device_sector disk[TEST_SECTORS];
static uint32_t read_calls;
static uint32_t write_calls;
static uint32_t flush_calls;
static uint32_t fail_write_mask;

static void fill_sector(union block_device_sector *sector, uint8_t value)
{
	size_t index;

	for (index = 0u; index < ARRAY_SIZE(sector->bytes); ++index)
		sector->bytes[index] = value;
}

static bool sector_is(const union block_device_sector *sector, uint8_t value)
{
	size_t index;

	for (index = 0u; index < ARRAY_SIZE(sector->bytes); ++index) {
		if (sector->bytes[index] != value)
			return false;
	}
	return true;
}

enum block_device_status
block_device_get_geometry(block_device_handle_t handle,
			  struct block_device_geometry *geometry)
{
	if (handle != TEST_DEVICE || geometry == NULL)
		return BLOCK_DEVICE_STALE_HANDLE;
	geometry->sector_count = 100u;
	geometry->logical_sector_bytes = BLOCK_DEVICE_SECTOR_BYTES;
	geometry->writable = 1u;
	geometry->reserved[0] = 0u;
	geometry->reserved[1] = 0u;
	geometry->reserved[2] = 0u;
	return BLOCK_DEVICE_OK;
}

enum block_device_status
block_device_read_sector(block_device_handle_t handle, block_lba_t lba,
			 union block_device_sector *sector)
{
	if (handle != TEST_DEVICE || sector == NULL || lba < TEST_FIRST_LBA ||
	    lba >= TEST_FIRST_LBA + TEST_SECTORS)
		return BLOCK_DEVICE_OUT_OF_RANGE;
	++read_calls;
	*sector = disk[lba - TEST_FIRST_LBA];
	return BLOCK_DEVICE_OK;
}

enum block_device_status
block_device_write_sector(block_device_handle_t handle, block_lba_t lba,
			  const union block_device_sector *sector)
{
	if (handle != TEST_DEVICE || sector == NULL || lba < TEST_FIRST_LBA ||
	    lba >= TEST_FIRST_LBA + TEST_SECTORS)
		return BLOCK_DEVICE_OUT_OF_RANGE;
	++write_calls;
	if (write_calls < 32u &&
	    (fail_write_mask & (1u << write_calls)) != 0u)
		return BLOCK_DEVICE_IO_ERROR;
	disk[lba - TEST_FIRST_LBA] = *sector;
	return BLOCK_DEVICE_OK;
}

enum block_device_status block_device_flush(block_device_handle_t handle)
{
	if (handle != TEST_DEVICE)
		return BLOCK_DEVICE_STALE_HANDLE;
	++flush_calls;
	return BLOCK_DEVICE_OK;
}

static enum iomgr_probe_result
test_probe(kernel_object_handle_t context,
	   const struct iomgr_mount_request *request)
{
	return context == TEST_DRIVER_CONTEXT && request != NULL
		       ? IOMGR_PROBE_MATCH
		       : IOMGR_PROBE_IO_ERROR;
}

static enum iomgr_driver_mount_status
test_mount(kernel_object_handle_t context,
	   const struct iomgr_mount_request *request,
	   struct iomgr_driver_mount_result *result)
{
	if (context != TEST_DRIVER_CONTEXT || request == NULL || result == NULL)
		return IOMGR_DRIVER_MOUNT_UNCERTAIN;
	result->volume_context = TEST_VOLUME_CONTEXT;
	result->capabilities =
		IOMGR_VOLUME_CAP_READ | IOMGR_VOLUME_CAP_WRITE;
	result->maximum_name_units = 255u;
	result->reserved = 0u;
	return IOMGR_DRIVER_MOUNT_OK;
}

static enum iomgr_driver_unmount_status
test_unmount(kernel_object_handle_t context,
	     kernel_object_handle_t volume_context)
{
	return context == TEST_DRIVER_CONTEXT &&
		       volume_context == TEST_VOLUME_CONTEXT
		       ? IOMGR_DRIVER_UNMOUNT_CLEAN
		       : IOMGR_DRIVER_UNMOUNT_UNCERTAIN;
}

static void reset_disk(void)
{
	size_t index;

	for (index = 0u; index < TEST_SECTORS; ++index)
		fill_sector(&disk[index], (uint8_t)index);
	read_calls = 0u;
	write_calls = 0u;
	flush_calls = 0u;
	fail_write_mask = 0u;
}

static int run_tests(void)
{
	const struct iomgr_driver_ops driver = {
		.abi_version = IOMGR_DRIVER_ABI_VERSION,
		.reserved = 0u,
		.identity = TEST_DRIVER_ID,
		.context = TEST_DRIVER_CONTEXT,
		.probe = test_probe,
		.mount = test_mount,
		.unmount = test_unmount,
	};
	const struct iomgr_mount_request request = {
		.device = TEST_DEVICE,
		.first_lba = TEST_FIRST_LBA,
		.sector_count = TEST_SECTORS,
		.flags = 0u,
		.reserved = 0u,
	};
	union block_device_sector after_b;
	union block_device_sector after_c;
	struct iomgr_volume_info info;
	iomgr_volume_handle_t volume;
	iomgr_transaction_handle_t transaction;

	reset_disk();
	fill_sector(&after_b, 0xbbu);
	fill_sector(&after_c, 0xccu);
	if (iomgr_initialize() != IOMGR_OK ||
	    iomgr_register_driver(&driver) != IOMGR_OK ||
	    iomgr_mount(&request, &volume) != IOMGR_OK)
		return 1;
	if (iomgr_transaction_begin(volume, &transaction) != IOMGR_OK ||
	    iomgr_transaction_stage(transaction, 2u, &after_c) != IOMGR_OK ||
	    iomgr_transaction_stage(transaction, 2u, &after_b) != IOMGR_OK ||
	    iomgr_transaction_stage(transaction, 3u, &after_c) != IOMGR_OK ||
	    read_calls != 2u ||
	    iomgr_transaction_commit(transaction) != IOMGR_OK ||
	    !sector_is(&disk[2], 0xbbu) || !sector_is(&disk[3], 0xccu) ||
	    write_calls != 2u || flush_calls != 1u ||
	    iomgr_transaction_commit(transaction) != IOMGR_STALE_HANDLE)
		return 2;

	reset_disk();
	if (iomgr_transaction_begin(volume, &transaction) != IOMGR_OK ||
	    iomgr_transaction_stage(transaction, 2u, &after_b) != IOMGR_OK ||
	    iomgr_transaction_stage(transaction, 3u, &after_c) != IOMGR_OK)
		return 3;
	fail_write_mask = 1u << 2;
	if (iomgr_transaction_commit(transaction) != IOMGR_IO_ERROR ||
	    !sector_is(&disk[2], 2u) || !sector_is(&disk[3], 3u) ||
	    write_calls != 3u || flush_calls != 1u ||
	    iomgr_get_volume_info(volume, &info) != IOMGR_OK)
		return 4;

	reset_disk();
	if (iomgr_transaction_begin(volume, &transaction) != IOMGR_OK ||
	    iomgr_transaction_stage(transaction, 2u, &after_b) != IOMGR_OK ||
	    iomgr_transaction_stage(transaction, 3u, &after_c) != IOMGR_OK)
		return 5;
	fail_write_mask = (1u << 2) | (1u << 3);
	if (iomgr_transaction_commit(transaction) != IOMGR_UNCERTAIN ||
	    iomgr_transaction_commit(transaction) != IOMGR_POISONED ||
	    iomgr_get_volume_info(volume, &info) != IOMGR_POISONED)
		return 6;
	return 0;
}

DOSC32_TEST_ENTRY(run_tests)

// SPDX-License-Identifier: GPL-2.0-only
/* Freestanding exact boot-volume locator tests. */
#include "iomgr_discovery.h"
#include "test_entry.h"

#define TEST_DEVICE ((block_device_handle_t)0x1234u)
#define TEST_VOLUME ((iomgr_volume_handle_t)0x5678u)
#define VOLUME_SENTINEL ((iomgr_volume_handle_t)0xa5a55a5af00ff00full)

static struct block_device_geometry geometry;
static enum block_device_status geometry_status;
static enum iomgr_status mount_result;
static struct iomgr_mount_request observed_request;
static uint32_t geometry_calls;
static uint32_t mount_calls;

static struct iomgr_boot_volume_locator locator(block_lba_t first,
						 block_lba_t count)
{
	return (struct iomgr_boot_volume_locator){
		.first_lba = first,
		.sector_count = count,
		.logical_sector_bytes = BLOCK_DEVICE_SECTOR_BYTES,
		.reserved = 0u,
	};
}

static void reset_fixture(bool writable)
{
	geometry = (struct block_device_geometry){
		.sector_count = 1000u,
		.logical_sector_bytes = BLOCK_DEVICE_SECTOR_BYTES,
		.writable = writable ? 1u : 0u,
		.reserved = {0u},
	};
	geometry_status = BLOCK_DEVICE_OK;
	mount_result = IOMGR_OK;
	observed_request = (struct iomgr_mount_request){0};
	geometry_calls = 0u;
	mount_calls = 0u;
}

enum block_device_status
block_device_get_geometry(block_device_handle_t handle,
			  struct block_device_geometry *output)
{
	++geometry_calls;
	if (handle != TEST_DEVICE || output == NULL)
		return BLOCK_DEVICE_INVALID_ARGUMENT;
	if (geometry_status == BLOCK_DEVICE_OK)
		*output = geometry;
	return geometry_status;
}

enum iomgr_status iomgr_mount(const struct iomgr_mount_request *request,
			      iomgr_volume_handle_t *volume)
{
	++mount_calls;
	if (request == NULL || volume == NULL)
		return IOMGR_INVALID_ARGUMENT;
	observed_request = *request;
	if (mount_result == IOMGR_OK)
		*volume = TEST_VOLUME;
	return mount_result;
}

static bool request_matches(block_lba_t first, block_lba_t count,
			    uint32_t flags)
{
	return observed_request.device == TEST_DEVICE &&
	       observed_request.first_lba == first &&
	       observed_request.sector_count == count &&
	       observed_request.flags == flags && observed_request.reserved == 0u;
}

static int test_exact_extents(void)
{
	struct iomgr_boot_volume_locator direct = locator(0u, 1000u);
	struct iomgr_boot_volume_locator partition = locator(100u, 700u);
	iomgr_volume_handle_t volume = VOLUME_SENTINEL;

	reset_fixture(true);
	if (iomgr_mount_boot_volume(TEST_DEVICE, &direct, &volume) != IOMGR_OK ||
	    volume != TEST_VOLUME || geometry_calls != 1u || mount_calls != 1u ||
	    !request_matches(0u, 1000u, 0u))
		return 1;
	reset_fixture(false);
	volume = VOLUME_SENTINEL;
	if (iomgr_mount_boot_volume(TEST_DEVICE, &partition, &volume) !=
		    IOMGR_OK ||
	    volume != TEST_VOLUME ||
	    !request_matches(100u, 700u, IOMGR_MOUNT_READ_ONLY))
		return 2;
	return 0;
}

static int test_locator_validation(void)
{
	struct iomgr_boot_volume_locator invalid = locator(100u, 700u);
	iomgr_volume_handle_t volume = VOLUME_SENTINEL;

	reset_fixture(true);
	invalid.sector_count = 0u;
	if (iomgr_mount_boot_volume(TEST_DEVICE, &invalid, &volume) !=
		    IOMGR_CORRUPT ||
	    volume != VOLUME_SENTINEL || mount_calls != 0u)
		return 1;
	invalid = locator(100u, 901u);
	if (iomgr_mount_boot_volume(TEST_DEVICE, &invalid, &volume) !=
		    IOMGR_CORRUPT ||
	    mount_calls != 0u)
		return 2;
	invalid = locator(1000u, 1u);
	if (iomgr_mount_boot_volume(TEST_DEVICE, &invalid, &volume) !=
		    IOMGR_CORRUPT ||
	    mount_calls != 0u)
		return 3;
	invalid = locator(100u, 700u);
	invalid.logical_sector_bytes = 4096u;
	if (iomgr_mount_boot_volume(TEST_DEVICE, &invalid, &volume) !=
		    IOMGR_CORRUPT ||
	    mount_calls != 0u)
		return 4;
	invalid = locator(100u, 700u);
	invalid.reserved = 1u;
	return iomgr_mount_boot_volume(TEST_DEVICE, &invalid, &volume) ==
		       IOMGR_CORRUPT &&
	       mount_calls == 0u
	       ? 0
	       : 5;
}

static int test_failures_are_atomic(void)
{
	struct iomgr_boot_volume_locator valid = locator(100u, 700u);
	iomgr_volume_handle_t volume = VOLUME_SENTINEL;

	reset_fixture(true);
	if (iomgr_mount_boot_volume(BLOCK_DEVICE_HANDLE_INVALID, &valid,
				    &volume) != IOMGR_INVALID_ARGUMENT ||
	    iomgr_mount_boot_volume(TEST_DEVICE, NULL, &volume) !=
		    IOMGR_INVALID_ARGUMENT ||
	    iomgr_mount_boot_volume(TEST_DEVICE, &valid, NULL) !=
		    IOMGR_INVALID_ARGUMENT ||
	    volume != VOLUME_SENTINEL || geometry_calls != 0u)
		return 1;
	geometry_status = BLOCK_DEVICE_STALE_HANDLE;
	if (iomgr_mount_boot_volume(TEST_DEVICE, &valid, &volume) !=
		    IOMGR_IO_ERROR ||
	    volume != VOLUME_SENTINEL || mount_calls != 0u)
		return 2;
	reset_fixture(true);
	mount_result = IOMGR_CORRUPT;
	return iomgr_mount_boot_volume(TEST_DEVICE, &valid, &volume) ==
		       IOMGR_CORRUPT &&
	       volume == VOLUME_SENTINEL && mount_calls == 1u
	       ? 0
	       : 3;
}

static int run_tests(void)
{
	int status = test_exact_extents();

	if (status != 0)
		return 10 + status;
	status = test_locator_validation();
	if (status != 0)
		return 20 + status;
	status = test_failures_are_atomic();
	return status == 0 ? 0 : 30 + status;
}

DOSC32_TEST_ENTRY(run_tests)

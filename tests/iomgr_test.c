// SPDX-License-Identifier: GPL-2.0-only
/* Freestanding I/O Manager driver and volume lifetime tests. */
#include "iomgr_driver.h"
#include "test_entry.h"

#define TEST_DEVICE ((block_device_handle_t)0x1234u)
#define DRIVER_ONE_ID 0x494f4d4752445231ull
#define DRIVER_TWO_ID 0x494f4d4752445232ull
#define DRIVER_ONE_CONTEXT ((kernel_object_handle_t)0x101u)
#define DRIVER_TWO_CONTEXT ((kernel_object_handle_t)0x202u)
#define DRIVER_VOLUME_CONTEXT ((kernel_object_handle_t)0x303u)
#define DRIVER_FILE_CONTEXT ((kernel_object_handle_t)0x404u)
#define DRIVER_SEARCH_CONTEXT ((kernel_object_handle_t)0x505u)

static bool geometry_writable = true;
static enum iomgr_probe_result first_probe_result = IOMGR_PROBE_NO_MATCH;
static enum iomgr_driver_unmount_status unmount_result =
	IOMGR_DRIVER_UNMOUNT_CLEAN;
static uint32_t first_probe_calls;
static uint32_t second_probe_calls;
static uint32_t mount_calls;
static uint32_t unmount_calls;
static uint32_t search_position;
static uint32_t create_calls;

static struct iomgr_node_info test_node_info(void)
{
	return (struct iomgr_node_info){
		.size = 4u,
		.attributes = IOMGR_NODE_ARCHIVE,
		.modified = {
			.year = 2026u,
			.month = 8u,
			.day = 28u,
			.hour = 12u,
			.minute = 30u,
			.second = 10u,
			.centiseconds = 0u,
		},
	};
}

static enum iomgr_status named_stat(kernel_object_handle_t volume_context,
				    const struct iomgr_path *path,
				    struct iomgr_node_info *info)
{
	if (volume_context != DRIVER_VOLUME_CONTEXT || path == NULL ||
	    path->length == 0u || info == NULL)
		return IOMGR_INVALID_ARGUMENT;
	*info = test_node_info();
	return IOMGR_OK;
}

static enum iomgr_status named_open_file(
	kernel_object_handle_t volume_context, const struct iomgr_path *path,
	kernel_object_handle_t *file_context, struct iomgr_node_info *info)
{
	if (volume_context != DRIVER_VOLUME_CONTEXT || path == NULL ||
	    file_context == NULL || info == NULL)
		return IOMGR_INVALID_ARGUMENT;
	*file_context = DRIVER_FILE_CONTEXT;
	*info = test_node_info();
	return IOMGR_OK;
}

static enum iomgr_status named_read_file(
	kernel_object_handle_t volume_context,
	kernel_object_handle_t file_context, uint64_t offset,
	uint8_t *destination, size_t capacity, size_t count, size_t *bytes_read)
{
	size_t index;

	if (volume_context != DRIVER_VOLUME_CONTEXT ||
	    file_context != DRIVER_FILE_CONTEXT || destination == NULL ||
	    bytes_read == NULL || offset > 4u || count > capacity)
		return IOMGR_INVALID_ARGUMENT;
	if (count > 4u - (size_t)offset)
		count = 4u - (size_t)offset;
	for (index = 0u; index < count; ++index)
		destination[index] = (uint8_t)'R';
	*bytes_read = count;
	return IOMGR_OK;
}

static enum iomgr_status named_close_file(
	kernel_object_handle_t volume_context,
	kernel_object_handle_t file_context)
{
	return volume_context == DRIVER_VOLUME_CONTEXT &&
		       file_context == DRIVER_FILE_CONTEXT
		       ? IOMGR_OK
		       : IOMGR_INVALID_ARGUMENT;
}

static enum iomgr_status named_open_search(
	kernel_object_handle_t volume_context, const struct iomgr_path *pattern,
	uint32_t attributes, kernel_object_handle_t *search_context)
{
	(void)attributes;
	if (volume_context != DRIVER_VOLUME_CONTEXT || pattern == NULL ||
	    search_context == NULL)
		return IOMGR_INVALID_ARGUMENT;
	search_position = 0u;
	*search_context = DRIVER_SEARCH_CONTEXT;
	return IOMGR_OK;
}

static enum iomgr_status named_search_next(
	kernel_object_handle_t volume_context,
	kernel_object_handle_t search_context,
	struct iomgr_directory_entry *entry)
{
	if (volume_context != DRIVER_VOLUME_CONTEXT ||
	    search_context != DRIVER_SEARCH_CONTEXT || entry == NULL)
		return IOMGR_INVALID_ARGUMENT;
	if (search_position != 0u)
		return IOMGR_END_OF_SEARCH;
	entry->info = test_node_info();
	entry->name_length = 4u;
	entry->name[0] = 'T';
	entry->name[1] = 'E';
	entry->name[2] = 'S';
	entry->name[3] = 'T';
	entry->name[4] = 0u;
	++search_position;
	return IOMGR_OK;
}

static enum iomgr_status named_close_search(
	kernel_object_handle_t volume_context,
	kernel_object_handle_t search_context)
{
	return volume_context == DRIVER_VOLUME_CONTEXT &&
		       search_context == DRIVER_SEARCH_CONTEXT
		       ? IOMGR_OK
		       : IOMGR_INVALID_ARGUMENT;
}

static enum iomgr_status named_query_space(
	kernel_object_handle_t volume_context, bool count_free,
	struct iomgr_space_info *info)
{
	if (volume_context != DRIVER_VOLUME_CONTEXT || info == NULL)
		return IOMGR_INVALID_ARGUMENT;
	*info = (struct iomgr_space_info){
		.total_bytes = 4096u,
		.free_bytes = count_free ? 2048u : 0u,
		.allocation_unit_bytes = 512u,
		.reserved = 0u,
	};
	return IOMGR_OK;
}

static enum iomgr_status named_create_directory(
	kernel_object_handle_t volume_context, const struct iomgr_path *path,
	iomgr_volume_handle_t volume)
{
	if (volume_context != DRIVER_VOLUME_CONTEXT || path == NULL ||
	    volume == IOMGR_VOLUME_HANDLE_INVALID)
		return IOMGR_INVALID_ARGUMENT;
	++create_calls;
	return IOMGR_OK;
}

static const struct iomgr_driver_named_ops named_ops = {
	.stat = named_stat,
	.open_file = named_open_file,
	.read_file = named_read_file,
	.close_file = named_close_file,
	.open_search = named_open_search,
	.search_next = named_search_next,
	.close_search = named_close_search,
	.query_space = named_query_space,
	.create_directory = named_create_directory,
};

enum block_device_status
block_device_get_geometry(block_device_handle_t handle,
			  struct block_device_geometry *geometry)
{
	if (handle != TEST_DEVICE || geometry == NULL)
		return BLOCK_DEVICE_STALE_HANDLE;
	geometry->sector_count = 1000u;
	geometry->logical_sector_bytes = BLOCK_DEVICE_SECTOR_BYTES;
	geometry->writable = geometry_writable ? 1u : 0u;
	geometry->reserved[0] = 0u;
	geometry->reserved[1] = 0u;
	geometry->reserved[2] = 0u;
	return BLOCK_DEVICE_OK;
}

static enum iomgr_probe_result
first_probe(kernel_object_handle_t context,
	    const struct iomgr_mount_request *request)
{
	++first_probe_calls;
	if (context != DRIVER_ONE_CONTEXT || request == NULL)
		return IOMGR_PROBE_IO_ERROR;
	return first_probe_result;
}

static enum iomgr_probe_result
second_probe(kernel_object_handle_t context,
	     const struct iomgr_mount_request *request)
{
	++second_probe_calls;
	if (context != DRIVER_TWO_CONTEXT || request == NULL)
		return IOMGR_PROBE_IO_ERROR;
	return IOMGR_PROBE_MATCH;
}

static enum iomgr_driver_mount_status
test_mount(kernel_object_handle_t context,
	   const struct iomgr_mount_request *request,
	   struct iomgr_driver_mount_result *result)
{
	++mount_calls;
	if (context != DRIVER_TWO_CONTEXT || request == NULL || result == NULL)
		return IOMGR_DRIVER_MOUNT_IO_ERROR;
	result->volume_context = DRIVER_VOLUME_CONTEXT;
	result->capabilities = IOMGR_VOLUME_CAP_READ |
		IOMGR_VOLUME_CAP_LONG_NAMES |
		IOMGR_VOLUME_CAP_CASE_PRESERVING;
	if ((request->flags & IOMGR_MOUNT_READ_ONLY) == 0u)
		result->capabilities |= IOMGR_VOLUME_CAP_WRITE;
	result->maximum_name_units = 255u;
	result->reserved = 0u;
	return IOMGR_DRIVER_MOUNT_OK;
}

static enum iomgr_driver_unmount_status
test_unmount(kernel_object_handle_t context,
	     kernel_object_handle_t volume_context)
{
	++unmount_calls;
	if (context != DRIVER_TWO_CONTEXT ||
	    volume_context != DRIVER_VOLUME_CONTEXT)
		return IOMGR_DRIVER_UNMOUNT_UNCERTAIN;
	return unmount_result;
}

static enum iomgr_driver_mount_status
unused_mount(kernel_object_handle_t context,
	     const struct iomgr_mount_request *request,
	     struct iomgr_driver_mount_result *result)
{
	(void)context;
	(void)request;
	(void)result;
	return IOMGR_DRIVER_MOUNT_IO_ERROR;
}

static enum iomgr_driver_unmount_status
unused_unmount(kernel_object_handle_t context,
	       kernel_object_handle_t volume_context)
{
	(void)context;
	(void)volume_context;
	return IOMGR_DRIVER_UNMOUNT_UNCERTAIN;
}

static int run_tests(void)
{
	const struct iomgr_driver_ops first_driver = {
		.abi_version = IOMGR_DRIVER_ABI_VERSION,
		.reserved = 0u,
		.identity = DRIVER_ONE_ID,
		.context = DRIVER_ONE_CONTEXT,
		.probe = first_probe,
		.mount = unused_mount,
		.unmount = unused_unmount,
	};
	const struct iomgr_driver_ops second_driver = {
		.abi_version = IOMGR_DRIVER_ABI_VERSION,
		.reserved = 0u,
		.identity = DRIVER_TWO_ID,
		.context = DRIVER_TWO_CONTEXT,
		.probe = second_probe,
		.mount = test_mount,
		.unmount = test_unmount,
		.named = &named_ops,
	};
	struct iomgr_mount_request request = {
		.device = TEST_DEVICE,
		.first_lba = 10u,
		.sector_count = 100u,
		.flags = 0u,
		.reserved = 0u,
	};
	struct iomgr_volume_info info;
	struct iomgr_node_info node_info;
	struct iomgr_directory_entry directory_entry;
	struct iomgr_space_info space_info;
	static const uint8_t path_bytes[] = {'/', 'T', 'E', 'S', 'T'};
	static const uint8_t bad_path_bytes[] = {0xc0u, 0x80u};
	const struct iomgr_path path = {
		.bytes = path_bytes,
		.length = sizeof(path_bytes),
	};
	const struct iomgr_path bad_path = {
		.bytes = bad_path_bytes,
		.length = sizeof(bad_path_bytes),
	};
	iomgr_file_handle_t file;
	iomgr_search_handle_t search;
	uint8_t read_buffer[4] = {0u};
	size_t bytes_read = 0u;
	iomgr_volume_handle_t first_handle = 0xa5a5a5a5a5a5a5a5ull;
	iomgr_volume_handle_t second_handle = 0x5a5a5a5a5a5a5a5aull;
	uint32_t saved_second_probe_calls;

	if (iomgr_register_driver(&first_driver) != IOMGR_NOT_INITIALIZED ||
	    iomgr_initialize() != IOMGR_OK ||
	    iomgr_initialize() != IOMGR_ALREADY_INITIALIZED ||
	    iomgr_register_driver(&first_driver) != IOMGR_OK ||
	    iomgr_register_driver(&first_driver) != IOMGR_DUPLICATE_DRIVER ||
	    iomgr_register_driver(&second_driver) != IOMGR_OK)
		return 1;
	if (iomgr_mount(&request, &first_handle) != IOMGR_OK ||
	    first_handle == IOMGR_VOLUME_HANDLE_INVALID || first_handle == 0u ||
	    first_probe_calls != 1u || second_probe_calls != 1u ||
	    mount_calls != 1u)
		return 2;
	if (iomgr_get_volume_info(first_handle, &info) != IOMGR_OK ||
	    info.driver_identity != DRIVER_TWO_ID || info.device != TEST_DEVICE ||
	    info.first_lba != 10u || info.sector_count != 100u ||
	    info.capabilities !=
		(IOMGR_VOLUME_CAP_READ | IOMGR_VOLUME_CAP_WRITE |
		 IOMGR_VOLUME_CAP_LONG_NAMES |
		 IOMGR_VOLUME_CAP_CASE_PRESERVING) ||
	    info.maximum_name_units != 255u || info.reserved != 0u)
		return 3;
	if (iomgr_stat(first_handle, &bad_path, &node_info) !=
		    IOMGR_INVALID_ARGUMENT ||
	    iomgr_stat(first_handle, &path, &node_info) != IOMGR_OK ||
	    node_info.size != 4u ||
	    iomgr_open_file(first_handle, &path, &node_info, &file) !=
		    IOMGR_OK ||
	    iomgr_unmount(first_handle) != IOMGR_BUSY ||
	    iomgr_read_file(file, 1u, read_buffer, sizeof(read_buffer), 3u,
			    &bytes_read) != IOMGR_OK ||
	    bytes_read != 3u || read_buffer[0] != 'R' ||
	    iomgr_close_file(file) != IOMGR_OK ||
	    iomgr_close_file(file) != IOMGR_STALE_HANDLE ||
	    iomgr_open_search(first_handle, &path, 0u, &search) != IOMGR_OK ||
	    iomgr_search_next(search, &directory_entry) != IOMGR_OK ||
	    directory_entry.name_length != 4u ||
	    iomgr_search_next(search, &directory_entry) != IOMGR_END_OF_SEARCH ||
	    iomgr_close_search(search) != IOMGR_OK ||
	    iomgr_query_space(first_handle, true, &space_info) != IOMGR_OK ||
	    space_info.free_bytes != 2048u ||
	    iomgr_create_directory(first_handle, &path) != IOMGR_OK ||
	    create_calls != 1u)
		return 31;

	unmount_result = IOMGR_DRIVER_UNMOUNT_BUSY;
	if (iomgr_unmount(first_handle) != IOMGR_BUSY || unmount_calls != 1u ||
	    iomgr_get_volume_info(first_handle, &info) != IOMGR_OK)
		return 4;
	unmount_result = IOMGR_DRIVER_UNMOUNT_CLEAN;
	if (iomgr_unmount(first_handle) != IOMGR_OK || unmount_calls != 2u ||
	    iomgr_get_volume_info(first_handle, &info) != IOMGR_STALE_HANDLE)
		return 5;

	geometry_writable = false;
	if (iomgr_mount(&request, &second_handle) != IOMGR_OK ||
	    second_handle == first_handle ||
	    iomgr_get_volume_info(second_handle, &info) != IOMGR_OK ||
	    (info.capabilities & IOMGR_VOLUME_CAP_WRITE) != 0u)
		return 6;
	unmount_result = IOMGR_DRIVER_UNMOUNT_UNCERTAIN;
	if (iomgr_unmount(second_handle) != IOMGR_UNCERTAIN ||
	    iomgr_get_volume_info(second_handle, &info) != IOMGR_POISONED ||
	    iomgr_unmount(second_handle) != IOMGR_POISONED)
		return 7;

	geometry_writable = true;
	first_probe_result = IOMGR_PROBE_CORRUPT;
	saved_second_probe_calls = second_probe_calls;
	second_handle = 0x5a5a5a5a5a5a5a5aull;
	if (iomgr_mount(&request, &second_handle) != IOMGR_CORRUPT ||
	    second_handle != 0x5a5a5a5a5a5a5a5aull ||
	    second_probe_calls != saved_second_probe_calls)
		return 8;

	first_probe_result = IOMGR_PROBE_NO_MATCH;
	request.first_lba = 950u;
	request.sector_count = 100u;
	if (iomgr_mount(&request, &second_handle) != IOMGR_INVALID_ARGUMENT ||
	    second_handle != 0x5a5a5a5a5a5a5a5aull)
		return 9;
	return 0;
}

DOSC32_TEST_ENTRY(run_tests)

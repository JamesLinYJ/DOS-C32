// SPDX-License-Identifier: GPL-2.0-only
/* Register- and byte-level tests for the backend-independent INT 21h core. */
#include "dos_abi.h"
#include "test_entry.h"
#include "dos_int21.h"
#include "dos_jft.h"
#include "dos_nls.h"
#include "dos_sft_adapter.h"
#include "iomgr_device.h"

#define TEST_MEMORY_BYTES 0x00110000u
#define TEST_CONTEXT_HANDLE 0x49323154u
#define TEST_ARENA_HEAD 0x2000u
#define TEST_ARENA_END 0x2020u
#define TEST_ARENA_IDENTITY ((kernel_object_handle_t)0x4932314152454e41ull)
#define TEST_RUNTIME_IDENTITY ((kernel_object_handle_t)0x49323152554e5449ull)
#define TEST_ZERO_RUNTIME_IDENTITY                                             \
	((kernel_object_handle_t)0x49323152554e5a30ull)
#define TEST_OUTPUT_CONTEXT ((kernel_object_handle_t)0x4932314f55545054ull)
#define TEST_INPUT_CONTEXT ((kernel_object_handle_t)0x493231494e505554ull)
#define TEST_DPB_CONTEXT ((kernel_object_handle_t)0x4932314450424354ull)
#define TEST_DISK_SPACE_CONTEXT                                             \
	((kernel_object_handle_t)0x4932315350414345ull)
#define TEST_FIND_CONTEXT ((kernel_object_handle_t)0x49323146494e4443ull)
#define TEST_DIRECTORY_CONTEXT ((kernel_object_handle_t)0x4932314449524354ull)
#define TEST_FILE_CONTEXT ((kernel_object_handle_t)0x49323146494c4543ull)
#define TEST_FILE_HANDLE ((kernel_object_handle_t)0x200000005ull)
#define TEST_SEARCH_HANDLE_FIRST ((kernel_object_handle_t)0x200000007ull)
#define TEST_SEARCH_HANDLE_SECOND ((kernel_object_handle_t)0x300000007ull)
#define TEST_OUTPUT_BYTES 64u
#define TEST_INPUT_BYTES 16u
#define TEST_DOS_DEVICE_NAME_BYTES 8u
#define TEST_DEVICE_CONTEXT                                             \
	((kernel_object_handle_t)0x444556434f4e5445ull)
#define TEST_DEVICE_IDENTITY 0x4445564944454e54ull
#define TEST_DEVICE_INSTANCE_BASE                                      \
	((kernel_object_handle_t)0x4445564900000000ull)
#define TEST_SFT_IDENTITY ((kernel_object_handle_t)0x5346544944454e54ull)
#define TEST_SFT_CONTEXT ((kernel_object_handle_t)0x534654434f4e5445ull)
#define TEST_SFT_CLOSE_IDENTITY                                        \
	((kernel_object_handle_t)0x534654434c4f5345ull)

static const struct dos_int21_drive_config test_drive_config = {
	.available_drive_mask = (uint32_t)1u << 2u,
	.current_drive = 2u,
	.boot_drive = 3u,
	.last_drive = 3u,
	.reserved = 0u,
};

static uint8_t guest_memory[TEST_MEMORY_BYTES];
static uint32_t read_failures_remaining;
static uint32_t write_failures_remaining;
static size_t first_read_partial_bytes;
static size_t first_write_partial_bytes;
static uint8_t output_bytes[TEST_OUTPUT_BYTES];
static size_t output_count;
static uint8_t input_bytes[TEST_INPUT_BYTES];
static size_t input_count;
static size_t input_index;
static uint8_t post_flush_character;
static bool post_flush_character_available;
static bool input_flush_succeeds;
static uint32_t input_flush_calls;
static uint8_t last_dpb_drive;
static enum dos_error directory_create_result;
static uint32_t directory_create_calls;
static uint8_t last_current_directory_drive;
static uint16_t file_set_date;
static uint16_t file_set_time;
static enum dos_error bridge_attributes_result;
static uint16_t bridge_attributes_value;
static uint32_t bridge_attributes_calls;

struct bridge_file_state {
	enum dos_error open_result;
	enum dos_error close_result;
	uint32_t open_calls;
	uint32_t read_calls;
	uint32_t write_calls;
	uint32_t close_calls;
	size_t path_length;
	uint8_t path[128];
};

static struct bridge_file_state bridge_file;
static uint32_t device_open_calls;
static uint32_t device_close_calls;
static uint32_t device_read_calls;
static uint32_t device_write_calls;
static uint32_t device_control_read_calls;
static uint32_t device_control_write_calls;
static uint32_t device_query_calls;
static size_t device_read_limit;
static size_t device_write_limit;
static size_t device_control_limit;
static size_t device_last_capacity;
static size_t device_last_count;
static enum iomgr_device_callback_status device_open_result;
static enum iomgr_device_callback_status device_close_result;
static enum iomgr_device_callback_status device_read_result;
static enum iomgr_device_callback_status device_write_result;
static enum iomgr_device_callback_status device_control_result;
static enum iomgr_device_callback_status device_query_result;
static uint32_t device_query_state;
static uint8_t device_written[64];
static size_t device_written_count;
static uint8_t device_control_written[64];
static size_t device_control_written_count;
static bool device_reenter_read;
static iomgr_device_handle_t device_reentry_handle;
static enum iomgr_status device_reentry_status;

static size_t device_transfer_count(size_t requested, size_t limit)
{
	return limit < requested ? limit : requested;
}

static void reset_device_callbacks(void)
{
	device_open_calls = 0u;
	device_close_calls = 0u;
	device_read_calls = 0u;
	device_write_calls = 0u;
	device_control_read_calls = 0u;
	device_control_write_calls = 0u;
	device_query_calls = 0u;
	device_read_limit = (size_t)-1;
	device_write_limit = (size_t)-1;
	device_control_limit = (size_t)-1;
	device_last_capacity = 0u;
	device_last_count = 0u;
	device_open_result = IOMGR_DEVICE_CALLBACK_OK;
	device_close_result = IOMGR_DEVICE_CALLBACK_OK;
	device_read_result = IOMGR_DEVICE_CALLBACK_OK;
	device_write_result = IOMGR_DEVICE_CALLBACK_OK;
	device_control_result = IOMGR_DEVICE_CALLBACK_OK;
	device_query_result = IOMGR_DEVICE_CALLBACK_OK;
	device_query_state = IOMGR_DEVICE_STATE_READ_READY |
			     IOMGR_DEVICE_STATE_WRITE_READY;
	device_written_count = 0u;
	device_control_written_count = 0u;
	device_reenter_read = false;
	device_reentry_handle = IOMGR_DEVICE_HANDLE_INVALID;
	device_reentry_status = IOMGR_OK;
}

static enum iomgr_device_callback_status test_device_open_callback(
	kernel_object_handle_t context, kernel_object_handle_t *instance_context)
{
	if (context != TEST_DEVICE_CONTEXT || instance_context == NULL)
		return IOMGR_DEVICE_CALLBACK_INVALID_ARGUMENT;
	++device_open_calls;
	if (device_open_result != IOMGR_DEVICE_CALLBACK_OK)
		return device_open_result;
	*instance_context = TEST_DEVICE_INSTANCE_BASE + device_open_calls;
	return IOMGR_DEVICE_CALLBACK_OK;
}

static enum iomgr_device_callback_status test_device_close_callback(
	kernel_object_handle_t context, kernel_object_handle_t instance_context)
{
	if (context != TEST_DEVICE_CONTEXT ||
	    instance_context <= TEST_DEVICE_INSTANCE_BASE)
		return IOMGR_DEVICE_CALLBACK_INVALID_ARGUMENT;
	++device_close_calls;
	return device_close_result;
}

static enum iomgr_device_callback_status test_device_read_callback(
	kernel_object_handle_t context, kernel_object_handle_t instance_context,
	uint8_t *destination, size_t capacity, size_t count, size_t *bytes_read)
{
	size_t completed;
	size_t index;

	if (context != TEST_DEVICE_CONTEXT ||
	    instance_context <= TEST_DEVICE_INSTANCE_BASE ||
	    bytes_read == NULL || count > capacity ||
	    (destination == NULL && count != 0u))
		return IOMGR_DEVICE_CALLBACK_INVALID_ARGUMENT;
	++device_read_calls;
	device_last_capacity = capacity;
	device_last_count = count;
	if (device_reenter_read) {
		struct iomgr_device_info ignored;

		device_reentry_status = iomgr_device_query_info(
			device_reentry_handle, &ignored);
		return IOMGR_DEVICE_CALLBACK_BUSY;
	}
	if (device_read_result != IOMGR_DEVICE_CALLBACK_OK)
		return device_read_result;
	completed = device_transfer_count(count, device_read_limit);
	for (index = 0u; index < completed; ++index)
		destination[index] = (uint8_t)(0xa0u + index);
	*bytes_read = completed;
	return IOMGR_DEVICE_CALLBACK_OK;
}

static enum iomgr_device_callback_status test_device_write_callback(
	kernel_object_handle_t context, kernel_object_handle_t instance_context,
	const uint8_t *source, size_t source_capacity, size_t count,
	size_t *bytes_written)
{
	size_t completed;
	size_t index;

	if (context != TEST_DEVICE_CONTEXT ||
	    instance_context <= TEST_DEVICE_INSTANCE_BASE ||
	    bytes_written == NULL || count > source_capacity ||
	    (source == NULL && count != 0u))
		return IOMGR_DEVICE_CALLBACK_INVALID_ARGUMENT;
	++device_write_calls;
	device_last_capacity = source_capacity;
	device_last_count = count;
	if (device_write_result != IOMGR_DEVICE_CALLBACK_OK)
		return device_write_result;
	completed = device_transfer_count(count, device_write_limit);
	if (completed > sizeof(device_written))
		return IOMGR_DEVICE_CALLBACK_INVALID_ARGUMENT;
	for (index = 0u; index < completed; ++index)
		device_written[index] = source[index];
	device_written_count = completed;
	*bytes_written = completed;
	return IOMGR_DEVICE_CALLBACK_OK;
}

static enum iomgr_device_callback_status test_device_control_callback(
	kernel_object_handle_t context, kernel_object_handle_t instance_context,
	uint64_t operation, const uint8_t *input, size_t input_capacity,
	size_t control_input_count, uint8_t *output, size_t output_capacity,
	size_t *bytes_returned)
{
	size_t completed;
	size_t index;

	if (context != TEST_DEVICE_CONTEXT ||
	    instance_context <= TEST_DEVICE_INSTANCE_BASE ||
	    bytes_returned == NULL || control_input_count > input_capacity)
		return IOMGR_DEVICE_CALLBACK_INVALID_ARGUMENT;
	if (device_control_result != IOMGR_DEVICE_CALLBACK_OK)
		return device_control_result;
	if (operation == DOS_INT21_DEVICE_CONTROL_READ) {
		if (input != NULL || input_capacity != 0u ||
		    control_input_count != 0u ||
		    (output == NULL && output_capacity != 0u))
			return IOMGR_DEVICE_CALLBACK_INVALID_ARGUMENT;
		++device_control_read_calls;
		completed = device_transfer_count(output_capacity,
						  device_control_limit);
		for (index = 0u; index < completed; ++index)
			output[index] = (uint8_t)(0xc0u + index);
		*bytes_returned = completed;
		return IOMGR_DEVICE_CALLBACK_OK;
	}
	if (operation == DOS_INT21_DEVICE_CONTROL_WRITE) {
		if ((input == NULL && control_input_count != 0u) ||
		    (output == NULL && output_capacity != 0u) ||
		    output_capacity < control_input_count)
			return IOMGR_DEVICE_CALLBACK_INVALID_ARGUMENT;
		++device_control_write_calls;
		completed = device_transfer_count(control_input_count,
						  device_control_limit);
		if (completed > sizeof(device_control_written))
			return IOMGR_DEVICE_CALLBACK_INVALID_ARGUMENT;
		for (index = 0u; index < completed; ++index)
			device_control_written[index] = input[index];
		device_control_written_count = completed;
		*bytes_returned = completed;
		return IOMGR_DEVICE_CALLBACK_OK;
	}
	return IOMGR_DEVICE_CALLBACK_UNSUPPORTED;
}

static enum iomgr_device_callback_status test_device_query_callback(
	kernel_object_handle_t context, kernel_object_handle_t instance_context,
	struct iomgr_device_query_result *result)
{
	if (context != TEST_DEVICE_CONTEXT ||
	    instance_context <= TEST_DEVICE_INSTANCE_BASE || result == NULL)
		return IOMGR_DEVICE_CALLBACK_INVALID_ARGUMENT;
	++device_query_calls;
	if (device_query_result != IOMGR_DEVICE_CALLBACK_OK)
		return device_query_result;
	*result = (struct iomgr_device_query_result){
		.pending_read_bytes = 0u,
		.state = device_query_state,
		.reserved = 0u,
	};
	return IOMGR_DEVICE_CALLBACK_OK;
}

static const struct iomgr_device_ops test_device_ops = {
	.abi_version = IOMGR_DEVICE_ABI_VERSION,
	.reserved = 0u,
	.identity = TEST_DEVICE_IDENTITY,
	.context = TEST_DEVICE_CONTEXT,
	.capabilities = IOMGR_DEVICE_CAP_READ | IOMGR_DEVICE_CAP_WRITE |
			IOMGR_DEVICE_CAP_CONTROL,
	.reserved2 = 0u,
	.open = test_device_open_callback,
	.close = test_device_close_callback,
	.read = test_device_read_callback,
	.write = test_device_write_callback,
	.control = test_device_control_callback,
	.query_info = test_device_query_callback,
};

static enum dos_error test_file_open(
	kernel_object_handle_t context, const uint8_t *path, size_t path_length,
	kernel_object_handle_t *file, uint64_t *size)
{
	(void)context;
	(void)path;
	(void)path_length;
	(void)file;
	(void)size;
	return DOS_ERROR_INVALID_DATA;
}

static enum dos_error test_file_create(
	kernel_object_handle_t context, const uint8_t *path, size_t path_length,
	uint16_t attributes, kernel_object_handle_t *file, uint64_t *size)
{
	(void)context;
	(void)path;
	(void)path_length;
	(void)attributes;
	(void)file;
	(void)size;
	return DOS_ERROR_INVALID_DATA;
}

static enum dos_error test_file_read(
	kernel_object_handle_t context, kernel_object_handle_t file,
	uint64_t offset, uint8_t *destination, size_t capacity, size_t count,
	size_t *bytes_read)
{
	(void)context;
	(void)file;
	(void)offset;
	(void)destination;
	(void)capacity;
	(void)count;
	(void)bytes_read;
	return DOS_ERROR_INVALID_DATA;
}

static enum dos_error test_file_write(
	kernel_object_handle_t context, kernel_object_handle_t file,
	uint64_t offset, const uint8_t *source, size_t source_capacity,
	size_t count, size_t *bytes_written)
{
	(void)context;
	(void)file;
	(void)offset;
	(void)source;
	(void)source_capacity;
	(void)count;
	(void)bytes_written;
	return DOS_ERROR_INVALID_DATA;
}

static enum dos_error test_file_get_time(
	kernel_object_handle_t context, kernel_object_handle_t file,
	uint16_t *date, uint16_t *time)
{
	if (context != TEST_FILE_CONTEXT || file != TEST_FILE_HANDLE ||
	    date == NULL || time == NULL)
		return DOS_ERROR_INVALID_DATA;
	*date = 0x58a5u;
	*time = 0x7c21u;
	return DOS_SUCCESS;
}

static enum dos_error test_file_set_time(
	kernel_object_handle_t context, kernel_object_handle_t file,
	uint16_t date, uint16_t time)
{
	if (context != TEST_FILE_CONTEXT || file != TEST_FILE_HANDLE)
		return DOS_ERROR_INVALID_DATA;
	file_set_date = date;
	file_set_time = time;
	return DOS_SUCCESS;
}

static enum dos_error test_file_rename(
	kernel_object_handle_t context, const uint8_t *old_path,
	size_t old_path_length, const uint8_t *new_path,
	size_t new_path_length)
{
	(void)context;
	(void)old_path;
	(void)old_path_length;
	(void)new_path;
	(void)new_path_length;
	return DOS_ERROR_INVALID_DATA;
}

static enum dos_error test_file_close(kernel_object_handle_t context,
				      kernel_object_handle_t file)
{
	(void)context;
	(void)file;
	return DOS_ERROR_INVALID_DATA;
}

static const struct dos_int21_file_ops test_file_ops = {
	.open = test_file_open,
	.create = test_file_create,
	.read = test_file_read,
	.write = test_file_write,
	.get_time = test_file_get_time,
	.set_time = test_file_set_time,
	.rename = test_file_rename,
	.close = test_file_close,
};

static enum dos_error bridge_file_open(
	kernel_object_handle_t context, const uint8_t *path, size_t path_length,
	kernel_object_handle_t *file, uint64_t *size)
{
	size_t index;

	if (context != TEST_FILE_CONTEXT || path == NULL || path_length == 0u ||
	    path_length > sizeof(bridge_file.path) || file == NULL || size == NULL)
		return DOS_ERROR_INVALID_DATA;
	++bridge_file.open_calls;
	bridge_file.path_length = path_length;
	for (index = 0u; index < path_length; ++index)
		bridge_file.path[index] = path[index];
	if (bridge_file.open_result != DOS_SUCCESS)
		return bridge_file.open_result;
	*file = TEST_FILE_HANDLE;
	*size = 1u;
	return DOS_SUCCESS;
}

static enum dos_error bridge_file_attributes(
	kernel_object_handle_t context, const uint8_t *path, size_t path_length,
	uint16_t *attributes)
{
	if (context != TEST_FILE_CONTEXT || path == NULL || path_length == 0u ||
	    path[path_length - 1u] != 0u || attributes == NULL)
		return DOS_ERROR_INVALID_DATA;
	++bridge_attributes_calls;
	if (bridge_attributes_result != DOS_SUCCESS)
		return bridge_attributes_result;
	*attributes = bridge_attributes_value;
	return DOS_SUCCESS;
}

static enum dos_error bridge_file_create(
	kernel_object_handle_t context, const uint8_t *path, size_t path_length,
	uint16_t attributes, kernel_object_handle_t *file, uint64_t *size)
{
	(void)context;
	(void)path;
	(void)path_length;
	(void)attributes;
	(void)file;
	(void)size;
	return DOS_ERROR_ACCESS_DENIED;
}

static enum dos_error bridge_file_read(
	kernel_object_handle_t context, kernel_object_handle_t file,
	uint64_t offset, uint8_t *destination, size_t capacity, size_t count,
	size_t *bytes_read)
{
	if (context != TEST_FILE_CONTEXT || file != TEST_FILE_HANDLE ||
	    destination == NULL || bytes_read == NULL || count > capacity)
		return DOS_ERROR_INVALID_DATA;
	++bridge_file.read_calls;
	if (offset != 0u || count == 0u) {
		*bytes_read = 0u;
		return DOS_SUCCESS;
	}
	destination[0] = (uint8_t)'F';
	*bytes_read = 1u;
	return DOS_SUCCESS;
}

static enum dos_error bridge_file_write(
	kernel_object_handle_t context, kernel_object_handle_t file,
	uint64_t offset, const uint8_t *source, size_t source_capacity,
	size_t count, size_t *bytes_written)
{
	(void)offset;
	if (context != TEST_FILE_CONTEXT || file != TEST_FILE_HANDLE ||
	    bytes_written == NULL || count > source_capacity ||
	    (source == NULL && count != 0u))
		return DOS_ERROR_INVALID_DATA;
	++bridge_file.write_calls;
	*bytes_written = count;
	return DOS_SUCCESS;
}

static enum dos_error bridge_file_get_time(
	kernel_object_handle_t context, kernel_object_handle_t file,
	uint16_t *date, uint16_t *time)
{
	if (context != TEST_FILE_CONTEXT || file != TEST_FILE_HANDLE ||
	    date == NULL || time == NULL)
		return DOS_ERROR_INVALID_DATA;
	*date = 0u;
	*time = 0u;
	return DOS_SUCCESS;
}

static enum dos_error bridge_file_set_time(
	kernel_object_handle_t context, kernel_object_handle_t file,
	uint16_t date, uint16_t time)
{
	(void)date;
	(void)time;
	return context == TEST_FILE_CONTEXT && file == TEST_FILE_HANDLE
		       ? DOS_SUCCESS
		       : DOS_ERROR_INVALID_DATA;
}

static enum dos_error bridge_file_rename(
	kernel_object_handle_t context, const uint8_t *old_path,
	size_t old_path_length, const uint8_t *new_path,
	size_t new_path_length)
{
	(void)context;
	(void)old_path;
	(void)old_path_length;
	(void)new_path;
	(void)new_path_length;
	return DOS_ERROR_ACCESS_DENIED;
}

static enum dos_error bridge_file_close(kernel_object_handle_t context,
					kernel_object_handle_t file)
{
	if (context != TEST_FILE_CONTEXT || file != TEST_FILE_HANDLE)
		return DOS_ERROR_INVALID_DATA;
	++bridge_file.close_calls;
	return bridge_file.close_result;
}

static const struct dos_int21_file_ops bridge_file_ops = {
	.open = bridge_file_open,
	.create = bridge_file_create,
	.read = bridge_file_read,
	.write = bridge_file_write,
	.get_time = bridge_file_get_time,
	.set_time = bridge_file_set_time,
	.rename = bridge_file_rename,
	.close = bridge_file_close,
};

static enum dos_sft_backend_close_status test_sft_backend_close(
	kernel_object_handle_t context, enum dos_sft_backend_kind backend_kind,
	kernel_object_handle_t backend_handle, enum dos_error *exact_error)
{
	enum iomgr_status status;

	if (context != TEST_FILE_CONTEXT || exact_error == NULL)
		return DOS_SFT_BACKEND_CLOSE_UNCERTAIN;
	*exact_error = DOS_SUCCESS;
	if (backend_kind == DOS_SFT_BACKEND_STANDARD)
		return DOS_SFT_BACKEND_CLOSE_OK;
	if (backend_kind == DOS_SFT_BACKEND_FILE) {
		*exact_error = bridge_file_close(context, backend_handle);
		return *exact_error == DOS_SUCCESS
			       ? DOS_SFT_BACKEND_CLOSE_OK
			       : DOS_SFT_BACKEND_CLOSE_EXACT_FAILURE;
	}
	if (backend_kind != DOS_SFT_BACKEND_DEVICE)
		return DOS_SFT_BACKEND_CLOSE_UNCERTAIN;
	status = iomgr_device_close(backend_handle);
	if (status == IOMGR_OK)
		return DOS_SFT_BACKEND_CLOSE_OK;
	if (status == IOMGR_UNCERTAIN || status == IOMGR_POISONED ||
	    status == IOMGR_NOT_INITIALIZED || status == IOMGR_CORRUPT)
		return DOS_SFT_BACKEND_CLOSE_UNCERTAIN;
	*exact_error = status == IOMGR_STALE_HANDLE || status == IOMGR_NOT_FOUND
			       ? DOS_ERROR_INVALID_HANDLE
			       : DOS_ERROR_ACCESS_DENIED;
	return DOS_SFT_BACKEND_CLOSE_EXACT_FAILURE;
}

static enum dos_error test_create_directory(
	kernel_object_handle_t context, const uint8_t *path, size_t path_length)
{
	static const uint8_t expected[] = "C:\\WINDOWS";
	size_t index;

	if (context != TEST_DIRECTORY_CONTEXT || path == NULL ||
	    path_length != sizeof(expected))
		return DOS_ERROR_INVALID_DATA;
	for (index = 0u; index < sizeof(expected); ++index) {
		if (path[index] != expected[index])
			return DOS_ERROR_INVALID_DATA;
	}
	++directory_create_calls;
	return directory_create_result;
}

static enum dos_error test_get_current_directory(
	kernel_object_handle_t context, uint8_t drive, uint8_t *path,
	size_t capacity, size_t *path_length)
{
	if (context != TEST_DIRECTORY_CONTEXT || path == NULL ||
	    capacity == 0u || path_length == NULL)
		return DOS_ERROR_INVALID_DATA;
	last_current_directory_drive = drive;
	*path_length = 0u;
	return DOS_SUCCESS;
}

static enum dos_error test_find_first(
	kernel_object_handle_t context, const uint8_t *path, size_t path_length,
	uint8_t attributes, struct dos_find_record *record)
{
	size_t index;

	if (context != TEST_FIND_CONTEXT || path == NULL || path_length != 6u ||
	    path[0] != (uint8_t)'*' || path[1] != (uint8_t)'.' ||
	    path[2] != (uint8_t)'T' || path[3] != (uint8_t)'X' ||
	    path[4] != (uint8_t)'T' || path[5] != 0u || attributes != 5u ||
	    record == NULL)
		return DOS_ERROR_INVALID_DATA;
	*record = (struct dos_find_record){
		.file_size = 1234u,
		.search_handle = TEST_SEARCH_HANDLE_FIRST,
		.modified_time = 0x1122u,
		.modified_date = 0x3344u,
		.search_drive = 3u,
		.search_name = {0u},
		.search_attributes = attributes,
		.found_attributes = 0x20u,
		.packed_name = {'A', '.', 'T', 'X', 'T', 0u},
	};
	for (index = 0u; index < DOS_FIND_NAME83_BYTES; ++index)
		record->search_name[index] = (uint8_t)'?';
	return DOS_SUCCESS;
}

static enum dos_error test_find_next(
	kernel_object_handle_t context, const struct dos_find_record *previous,
	struct dos_find_record *record)
{
	if (context != TEST_FIND_CONTEXT || previous == NULL || record == NULL)
		return DOS_ERROR_INVALID_DATA;
	if (previous->search_handle != TEST_SEARCH_HANDLE_FIRST)
		return DOS_ERROR_NO_MORE_FILES;
	*record = *previous;
	record->search_handle = TEST_SEARCH_HANDLE_SECOND;
	record->file_size = 5678u;
	record->packed_name[0] = (uint8_t)'B';
	return DOS_SUCCESS;
}

static const struct dos_int21_find_ops test_find_ops = {
	.first = test_find_first,
	.next = test_find_next,
};

static enum dos_error test_get_dpb(kernel_object_handle_t context,
				   uint8_t drive, uint16_t *segment,
				   uint16_t *offset)
{
	if (context != TEST_DPB_CONTEXT || segment == NULL || offset == NULL)
		return DOS_ERROR_INVALID_DATA;
	last_dpb_drive = drive;
	if (drive != 0u && drive != 3u)
		return DOS_ERROR_INVALID_DRIVE;
	*segment = 0x0f00u;
	*offset = 0x0200u;
	return DOS_SUCCESS;
}

static enum dos_error test_get_disk_space(
	kernel_object_handle_t context, uint8_t drive,
	struct dos_int21_disk_space *space)
{
	if (context != TEST_DISK_SPACE_CONTEXT || space == NULL)
		return DOS_ERROR_INVALID_DATA;
	if (drive != 0u && drive != 3u)
		return DOS_ERROR_INVALID_DRIVE;
	*space = (struct dos_int21_disk_space){
		.total_bytes = 100u * 2048u,
		.free_bytes = 40u * 2048u,
		.allocation_unit_bytes = 2048u,
		.reserved = 0u,
	};
	return DOS_SUCCESS;
}

static bool test_output_character(kernel_object_handle_t context,
				  uint8_t character)
{
	if (context != TEST_OUTPUT_CONTEXT || output_count >= TEST_OUTPUT_BYTES)
		return false;
	output_bytes[output_count++] = character;
	return true;
}

static bool test_input_status(kernel_object_handle_t context)
{
	return context == TEST_INPUT_CONTEXT &&
	       (input_index < input_count || post_flush_character_available);
}

static bool test_input_character(kernel_object_handle_t context,
				 uint8_t *character)
{
	if (context != TEST_INPUT_CONTEXT || character == NULL)
		return false;
	if (input_index < input_count) {
		*character = input_bytes[input_index++];
		return true;
	}
	if (!post_flush_character_available)
		return false;
	*character = post_flush_character;
	post_flush_character_available = false;
	return true;
}

static bool test_input_flush(kernel_object_handle_t context)
{
	if (context != TEST_INPUT_CONTEXT)
		return false;
	++input_flush_calls;
	input_index = input_count;
	return input_flush_succeeds;
}

static uint16_t read_le16(const uint8_t *bytes)
{
	return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static void write_le16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
}

static uint8_t *guest_segment(uint16_t segment)
{
	return guest_memory + ((uint32_t)segment << 4);
}

static void stage_bytes(uint16_t segment, uint16_t offset,
			const uint8_t *bytes, size_t count)
{
	uint8_t *destination = guest_segment(segment) + offset;
	size_t index;

	for (index = 0u; index < count; ++index)
		destination[index] = bytes[index];
}

static bool configure_device_bridge(struct dos_int21_context *context)
{
	return dos_int21_set_file_services(context, &bridge_file_ops,
					   TEST_FILE_CONTEXT) ==
			DOS_INT21_HANDLED &&
	       dos_int21_set_file_attributes_query(
		       context, bridge_file_attributes, TEST_FILE_CONTEXT) ==
		       DOS_INT21_HANDLED &&
	       dos_int21_bind_sft_services(context, TEST_SFT_CONTEXT) ==
		       DOS_INT21_HANDLED;
}

static bool register_test_device(
	const uint8_t name_bytes[TEST_DOS_DEVICE_NAME_BYTES],
	iomgr_device_registration_handle_t *registration)
{
	const struct iomgr_device_name name = {
		.bytes = name_bytes,
		.length = TEST_DOS_DEVICE_NAME_BYTES,
	};

	return iomgr_device_register(&name, &test_device_ops, registration) ==
	       IOMGR_OK;
}

static void stage_inline_jft(uint16_t psp_segment)
{
	uint8_t *psp = guest_segment(psp_segment);
	size_t index;

	for (index = 0u; index < DOS_PSP_DEFAULT_HANDLES; ++index)
		psp[__builtin_offsetof(struct dos_psp_prefix40, jft) + index] =
			DOS_JFT_UNUSED;
	psp[__builtin_offsetof(struct dos_psp_prefix40, jft)] = 0u;
	psp[__builtin_offsetof(struct dos_psp_prefix40, jft) + 1u] = 0u;
	psp[__builtin_offsetof(struct dos_psp_prefix40, jft) + 2u] = 0u;
	write_le16(
		psp + __builtin_offsetof(struct dos_psp_prefix40, jft_length),
		DOS_PSP_DEFAULT_HANDLES);
	write_le16(
		psp + __builtin_offsetof(struct dos_psp_prefix40, jft_pointer),
		(uint16_t)__builtin_offsetof(struct dos_psp_prefix40, jft));
	write_le16(
		psp + __builtin_offsetof(struct dos_psp_prefix40, jft_pointer) +
			2u,
		psp_segment);
}

static enum dos_machine_status
test_read_memory(kernel_object_handle_t context, dos_linear_address_t address,
		 void *destination, size_t destination_capacity, size_t count)
{
	uint8_t *output = (uint8_t *)destination;
	size_t index;
	size_t partial_count;

	if (context != TEST_CONTEXT_HANDLE || count > destination_capacity ||
	    (uint64_t)address > TEST_MEMORY_BYTES ||
	    (uint64_t)count > TEST_MEMORY_BYTES - (uint64_t)address)
		return DOS_MACHINE_ADDRESS_FAULT;
	if (read_failures_remaining != 0u) {
		--read_failures_remaining;
		partial_count = first_read_partial_bytes < count
				    ? first_read_partial_bytes
				    : count;
		first_read_partial_bytes = 0u;
		for (index = 0u; index < partial_count; ++index)
			output[index] = guest_memory[address + index];
		return DOS_MACHINE_ADDRESS_FAULT;
	}
	for (index = 0u; index < count; ++index)
		output[index] = guest_memory[address + index];
	return DOS_MACHINE_OK;
}

static enum dos_machine_status
test_write_memory(kernel_object_handle_t context, dos_linear_address_t address,
		  const void *source, size_t source_capacity, size_t count)
{
	const uint8_t *input = (const uint8_t *)source;
	size_t index;
	size_t partial_count;

	if (context != TEST_CONTEXT_HANDLE || count > source_capacity ||
	    (uint64_t)address > TEST_MEMORY_BYTES ||
	    (uint64_t)count > TEST_MEMORY_BYTES - (uint64_t)address)
		return DOS_MACHINE_ADDRESS_FAULT;
	if (write_failures_remaining != 0u) {
		--write_failures_remaining;
		partial_count = first_write_partial_bytes < count
				    ? first_write_partial_bytes
				    : count;
		first_write_partial_bytes = 0u;
		for (index = 0u; index < partial_count; ++index)
			guest_memory[address + index] = input[index];
		return DOS_MACHINE_ADDRESS_FAULT;
	}
	for (index = 0u; index < count; ++index)
		guest_memory[address + index] = input[index];
	return DOS_MACHINE_OK;
}

static bool states_equal(const struct dos_cpu_state *left,
			 const struct dos_cpu_state *right)
{
	return left->eax == right->eax && left->ebx == right->ebx &&
	       left->ecx == right->ecx && left->edx == right->edx &&
	       left->esi == right->esi && left->edi == right->edi &&
	       left->ebp == right->ebp && left->esp == right->esp &&
	       left->eip == right->eip && left->eflags == right->eflags &&
	       left->cs == right->cs && left->ss == right->ss &&
	       left->ds == right->ds && left->es == right->es &&
	       left->fs == right->fs && left->gs == right->gs &&
	       left->mode == right->mode;
}

static struct dos_cpu_state test_state(uint16_t ax)
{
	return (struct dos_cpu_state){
	    .eax = 0xa5a50000u | (uint32_t)ax,
	    .ebx = 0xb6b60000u,
	    .ecx = 0xc7c70077u,
	    .edx = 0xd8d81234u,
	    .esi = 0xe9e92345u,
	    .edi = 0xfafa3456u,
	    .ebp = 0x1b1b4567u,
	    .esp = 0x2c2c5678u,
	    .eip = 0x3d3d6789u,
	    .eflags = 0x000002d7u,
	    .cs = 0x1111u,
	    .ss = 0x2222u,
	    .ds = 0x3333u,
	    .es = 0x4444u,
	    .fs = 0x5555u,
	    .gs = 0x6666u,
	    .mode = DOS_CPU_VM86,
	};
}

static bool reset_runtime(struct dos_machine *machine,
			  struct dos_memory_arena *arena,
			  struct dos_int21_context *context)
{
	static const struct dos_machine_ops operations = {
	    .read_memory = test_read_memory,
	    .write_memory = test_write_memory,
	    .read_port = NULL,
	    .write_port = NULL,
	    .set_a20 = NULL,
	};
	size_t index;

	for (index = 0u; index < TEST_MEMORY_BYTES; ++index)
		guest_memory[index] = 0u;
	read_failures_remaining = 0u;
	write_failures_remaining = 0u;
	first_read_partial_bytes = 0u;
	first_write_partial_bytes = 0u;
	output_count = 0u;
	input_count = 0u;
	input_index = 0u;
	post_flush_character = 0u;
	post_flush_character_available = false;
	input_flush_succeeds = true;
	input_flush_calls = 0u;
	directory_create_calls = 0u;
	directory_create_result = DOS_SUCCESS;
	bridge_file = (struct bridge_file_state){
		.open_result = DOS_ERROR_INVALID_DATA,
		.close_result = DOS_SUCCESS,
	};
	bridge_attributes_result = DOS_SUCCESS;
	bridge_attributes_value = 0x10u;
	bridge_attributes_calls = 0u;
	*arena = (struct dos_memory_arena)DOS_MEMORY_ARENA_INITIALIZER(
	    TEST_ARENA_IDENTITY);
	if (dos_machine_configure(machine, &operations, TEST_CONTEXT_HANDLE,
				  TEST_MEMORY_BYTES, true) != DOS_MACHINE_OK ||
	    dos_memory_arena_initialize(arena, machine, TEST_ARENA_HEAD,
					TEST_ARENA_END) != DOS_SUCCESS ||
	    dos_int21_context_initialize(context, machine, arena,
					 TEST_RUNTIME_IDENTITY,
					 0x1234u, &test_drive_config) !=
		DOS_INT21_HANDLED)
		return false;
	return true;
}

static int test_create_directory_dispatch(void)
{
	static const uint8_t path[] = "C:\\WINDOWS";
	struct dos_machine machine;
	struct dos_memory_arena arena;
	struct dos_int21_context context;
	struct dos_cpu_state registers;
	struct dos_cpu_state before;
	uint8_t *guest_path;
	size_t index;

	if (!reset_runtime(&machine, &arena, &context))
		return 1;
	guest_path = guest_segment(0x0200u) + 0x0100u;
	for (index = 0u; index < sizeof(path); ++index)
		guest_path[index] = path[index];
	registers = test_state(0x395au);
	registers.ds = 0x0200u;
	dos_register_set_low16(&registers.edx, 0x0100u);
	before = registers;
	if (dos_int21_dispatch(&context, &registers) !=
		    DOS_INT21_UNIMPLEMENTED ||
	    !states_equal(&registers, &before) ||
	    dos_int21_set_directory_create(&context, NULL,
					 TEST_DIRECTORY_CONTEXT) !=
		    DOS_INT21_INVALID_ARGUMENT ||
	    dos_int21_set_directory_create(&context, test_create_directory,
					 TEST_DIRECTORY_CONTEXT) !=
		    DOS_INT21_HANDLED)
		return 2;
	registers = before;
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    (registers.eflags & DOS_EFLAGS_CF) != 0u ||
	    directory_create_calls != 1u)
		return 3;
	directory_create_result = DOS_ERROR_CANNOT_MAKE;
	registers = before;
	registers.eflags &= ~DOS_EFLAGS_CF;
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    (registers.eflags & DOS_EFLAGS_CF) == 0u ||
	    dos_register_low16(registers.eax) != DOS_ERROR_ACCESS_DENIED ||
	    context.extended_error.code != DOS_ERROR_CANNOT_MAKE ||
	    directory_create_calls != 2u)
		return 4;
	return 0;
}

static int test_console_write_and_list_of_lists(void)
{
	struct dos_machine machine;
	struct dos_memory_arena arena;
	struct dos_int21_context context;
	struct dos_cpu_state registers;
	struct dos_cpu_state before;
	uint8_t *text = guest_segment(0x0200u) + 0x0100u;

	if (!reset_runtime(&machine, &arena, &context) ||
	    read_le16(guest_segment(0x0f00u)) != TEST_ARENA_HEAD)
		return 1;
	stage_inline_jft(context.process_runtime.current_psp);
	if (dos_int21_bind_sft_services(&context, TEST_SFT_CONTEXT) !=
	    DOS_INT21_HANDLED)
		return 1;
	registers = test_state(0x5200u);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    registers.es != 0x0f00u ||
	    dos_register_low16(registers.ebx) != 2u)
		return 2;
	registers = test_state(0x34a5u);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    registers.es != 0x0f00u ||
	    dos_register_low16(registers.ebx) != 0x0180u ||
	    guest_segment(0x0f00u)[0x0180u] != 0u)
		return 3;

	registers = test_state(0x0200u);
	before = registers;
	if (dos_int21_dispatch(&context, &registers) !=
		    DOS_INT21_UNIMPLEMENTED ||
	    !states_equal(&registers, &before) ||
	    dos_int21_set_console_output(&context, NULL,
					 TEST_OUTPUT_CONTEXT) !=
		    DOS_INT21_INVALID_ARGUMENT ||
	    dos_int21_set_console_output(&context, test_output_character,
					 TEST_OUTPUT_CONTEXT) != DOS_INT21_HANDLED)
		return 4;

	registers = test_state(0x0200u);
	dos_register_set_low8(&registers.edx, (uint8_t)'A');
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    dos_register_low16(registers.eax) != 0x0241u ||
	    output_count != 1u || output_bytes[0] != (uint8_t)'A')
		return 4;

	text[0] = (uint8_t)'O';
	text[1] = (uint8_t)'K';
	text[2] = (uint8_t)'$';
	output_count = 0u;
	registers = test_state(0x0900u);
	registers.ds = 0x0200u;
	dos_register_set_low16(&registers.edx, 0x0100u);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    dos_register_low16(registers.eax) != 0x0924u ||
	    output_count != 2u || output_bytes[0] != (uint8_t)'O' ||
	    output_bytes[1] != (uint8_t)'K')
		return 5;

	text[0] = (uint8_t)'4';
	text[1] = (uint8_t)'0';
	output_count = 0u;
	registers = test_state(0x4000u);
	registers.ds = 0x0200u;
	dos_register_set_low16(&registers.ebx, 1u);
	dos_register_set_low16(&registers.ecx, 2u);
	dos_register_set_low16(&registers.edx, 0x0100u);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    dos_register_low16(registers.eax) != 2u ||
	    (registers.eflags & DOS_EFLAGS_CF) != 0u || output_count != 2u ||
	    output_bytes[0] != (uint8_t)'4' ||
	    output_bytes[1] != (uint8_t)'0')
		return 6;

	registers = test_state(0x4000u);
	dos_register_set_low16(&registers.ebx, 7u);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    dos_register_low16(registers.eax) != DOS_ERROR_INVALID_HANDLE ||
	    (registers.eflags & DOS_EFLAGS_CF) == 0u)
		return 7;
	return 0;
}

static int test_console_input_and_flush(void)
{
	struct dos_machine machine;
	struct dos_memory_arena arena;
	struct dos_int21_context context;
	struct dos_cpu_state registers;
	struct dos_cpu_state expected;

	if (!reset_runtime(&machine, &arena, &context))
		return 1;
	registers = test_state(0x0ce5u);
	expected = registers;
	if (dos_int21_dispatch(&context, &registers) !=
		    DOS_INT21_UNIMPLEMENTED ||
	    !states_equal(&registers, &expected) ||
	    dos_int21_set_console_input_flush(&context, NULL,
					      TEST_INPUT_CONTEXT) !=
		    DOS_INT21_INVALID_ARGUMENT ||
	    dos_int21_set_console_input_flush(&context, test_input_flush,
					      TEST_INPUT_CONTEXT) !=
		    DOS_INT21_HANDLED)
		return 2;

	input_bytes[0] = (uint8_t)'X';
	input_bytes[1] = (uint8_t)'Y';
	input_count = 2u;
	registers = test_state(0x0ce5u);
	expected = registers;
	dos_register_set_low16(&expected.eax, 0x0c00u);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    !states_equal(&registers, &expected) || input_flush_calls != 1u ||
	    input_index != input_count)
		return 3;

	registers = test_state(0x0c01u);
	expected = registers;
	if (dos_int21_dispatch(&context, &registers) !=
		    DOS_INT21_UNIMPLEMENTED ||
	    !states_equal(&registers, &expected) || input_flush_calls != 1u)
		return 4;
	if (dos_int21_set_console_output(&context, test_output_character,
					 TEST_OUTPUT_CONTEXT) != DOS_INT21_HANDLED ||
	    dos_int21_set_console_input_status(&context, test_input_status,
					       TEST_INPUT_CONTEXT) !=
		    DOS_INT21_HANDLED ||
	    dos_int21_set_console_input(&context, test_input_character,
					TEST_INPUT_CONTEXT) != DOS_INT21_HANDLED)
		return 5;

	input_bytes[0] = (uint8_t)'S';
	input_count = 1u;
	input_index = 0u;
	post_flush_character = (uint8_t)'N';
	post_flush_character_available = true;
	output_count = 0u;
	registers = test_state(0x0c01u);
	expected = registers;
	dos_register_set_low16(&expected.eax, 0x0c4eu);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    !states_equal(&registers, &expected) || input_flush_calls != 2u ||
	    input_index != input_count || post_flush_character_available ||
	    output_count != 1u || output_bytes[0] != (uint8_t)'N')
		return 6;

	input_count = 0u;
	input_index = 0u;
	registers = test_state(0x0600u);
	dos_register_set_low8(&registers.edx, 0xffu);
	registers.eflags &= ~DOS_EFLAGS_ZF;
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    dos_register_low16(registers.eax) != 0x0600u ||
	    (registers.eflags & DOS_EFLAGS_ZF) == 0u)
		return 7;

	input_bytes[0] = (uint8_t)'K';
	input_count = 1u;
	input_index = 0u;
	registers = test_state(0x0600u);
	dos_register_set_low8(&registers.edx, 0xffu);
	registers.eflags |= DOS_EFLAGS_ZF;
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    dos_register_low16(registers.eax) != 0x064bu ||
	    (registers.eflags & DOS_EFLAGS_ZF) != 0u || input_index != 1u)
		return 8;

	output_count = 0u;
	registers = test_state(0x0600u);
	dos_register_set_low8(&registers.edx, (uint8_t)'Q');
	expected = registers;
	dos_register_set_low8(&expected.eax, (uint8_t)'Q');
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    !states_equal(&registers, &expected) || output_count != 1u ||
	    output_bytes[0] != (uint8_t)'Q')
		return 9;

	input_flush_succeeds = false;
	registers = test_state(0x0ce5u);
	expected = registers;
	if (dos_int21_dispatch(&context, &registers) !=
		    DOS_INT21_MACHINE_FAULT ||
	    !states_equal(&registers, &expected) || input_flush_calls != 3u)
		return 10;
	return 0;
}

static int test_context_and_version(void)
{
	struct dos_machine machine;
	struct dos_machine broken_machine = {0};
	struct dos_memory_arena arena;
	struct dos_int21_context context;
	struct dos_int21_context zero_context;
	struct dos_int21_context untouched;
	struct dos_cpu_state registers;

	if (!reset_runtime(&machine, &arena, &context))
		return 1;
	if (dos_int21_context_initialize(&zero_context, &machine, &arena,
					 TEST_ZERO_RUNTIME_IDENTITY,
					 0u, &test_drive_config) !=
		DOS_INT21_HANDLED ||
	    zero_context.process_runtime.current_psp != 0u ||
	    zero_context.process_runtime.dta.segment != 0u ||
	    zero_context.process_runtime.dta.offset !=
		DOS_PSP_COMMAND_TAIL_OFFSET)
		return 2;
	untouched = context;
	if (dos_int21_context_initialize(&context, &broken_machine, &arena,
					 TEST_RUNTIME_IDENTITY,
					 0x1234u, &test_drive_config) !=
		DOS_INT21_MACHINE_FAULT ||
	    context.machine.ops != untouched.machine.ops ||
	    context.machine.context != untouched.machine.context ||
	    context.process_runtime.current_psp !=
		untouched.process_runtime.current_psp)
		return 3;
	untouched = context;
	if (dos_int21_context_initialize(
		&context, &machine, &arena, KERNEL_OBJECT_HANDLE_INVALID,
		0x1234u, &test_drive_config) != DOS_INT21_INVALID_ARGUMENT ||
	    context.process_runtime.identity !=
		untouched.process_runtime.identity ||
	    context.process_runtime.generation !=
		untouched.process_runtime.generation)
		return 4;
	if (dos_int21_set_version_identity(&context, 0x5au, 0x00123456u) !=
		DOS_INT21_HANDLED ||
	    dos_int21_set_version_identity(&context, 0x5au, 0x01000000u) !=
		DOS_INT21_INVALID_ARGUMENT)
		return 5;

	registers = test_state(0x307fu);
	registers.ebx |= 0xaaaau;
	registers.ecx = 0xc7c7aaaau;
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    registers.eax != 0xa5a51706u || registers.ebx != 0xb6b65a12u ||
	    registers.ecx != 0xc7c73456u || registers.eflags != 0x000002d7u ||
	    registers.edx != 0xd8d81234u || registers.esi != 0xe9e92345u ||
	    registers.edi != 0xfafa3456u || registers.es != 0x4444u)
		return 6;
	return 0;
}

static int test_drive_parameter_block_dispatch(void)
{
	struct dos_machine machine;
	struct dos_memory_arena arena;
	struct dos_int21_context context;
	struct dos_cpu_state registers;
	struct dos_cpu_state before;

	if (!reset_runtime(&machine, &arena, &context))
		return 1;
	registers = test_state(0x32a5u);
	before = registers;
	if (dos_int21_dispatch(&context, &registers) !=
		    DOS_INT21_UNIMPLEMENTED ||
	    !states_equal(&registers, &before) ||
	    dos_int21_set_dpb_query(&context, NULL, TEST_DPB_CONTEXT) !=
		    DOS_INT21_INVALID_ARGUMENT ||
	    dos_int21_set_dpb_query(&context, test_get_dpb,
				    TEST_DPB_CONTEXT) != DOS_INT21_HANDLED)
		return 2;

	last_dpb_drive = 0xffu;
	registers = test_state(0x1fa5u);
	registers.eflags |= DOS_EFLAGS_CF;
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    dos_register_low8(registers.eax) != 0u || registers.ds != 0x0f00u ||
	    dos_register_low16(registers.ebx) != 0x0200u ||
	    (registers.eflags & DOS_EFLAGS_CF) == 0u || last_dpb_drive != 0u)
		return 3;

	last_dpb_drive = 0xffu;
	registers = test_state(0x32a5u);
	dos_register_set_low8(&registers.edx, 3u);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    dos_register_low8(registers.eax) != 0u || registers.ds != 0x0f00u ||
	    dos_register_low16(registers.ebx) != 0x0200u || last_dpb_drive != 3u)
		return 4;

	registers = test_state(0x32a5u);
	dos_register_set_low8(&registers.edx, 4u);
	before = registers;
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    dos_register_low8(registers.eax) != 0xffu ||
	    registers.ds != before.ds ||
	    dos_register_low16(registers.ebx) !=
		dos_register_low16(before.ebx))
		return 5;
	return 0;
}

static int test_global_code_page(void)
{
	struct dos_machine machine;
	struct dos_memory_arena arena;
	struct dos_int21_context context;
	struct dos_cpu_state registers;
	struct dos_cpu_state before;

	if (!reset_runtime(&machine, &arena, &context) ||
	    context.nls.active == NULL || context.nls.active->code_page != 437u ||
	    context.nls.system_code_page != 437u)
		return 1;
	registers = test_state(0x6601u);
	registers.eflags |= DOS_EFLAGS_CF;
	before = registers;
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    dos_register_low16(registers.eax) != 0x6601u ||
	    dos_register_low16(registers.ebx) != 437u ||
	    dos_register_low16(registers.edx) != 437u ||
	    (registers.eflags & DOS_EFLAGS_CF) != 0u ||
	    registers.ecx != before.ecx || registers.esi != before.esi ||
	    registers.edi != before.edi)
		return 2;

	registers = test_state(0x6602u);
	dos_register_set_low16(&registers.ebx, 437u);
	registers.eflags |= DOS_EFLAGS_CF;
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    (registers.eflags & DOS_EFLAGS_CF) != 0u ||
	    dos_register_low16(registers.eax) != 0x6602u)
		return 3;

	registers = test_state(0x6602u);
	dos_register_set_low16(&registers.ebx, 936u);
	registers.eflags |= DOS_EFLAGS_CF;
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    (registers.eflags & DOS_EFLAGS_CF) != 0u ||
	    context.nls.active->code_page != 936u ||
	    context.nls.active->format.country != 86u ||
	    context.nls.generation != 2u ||
	    read_le16(guest_segment(0x0e00u) + 0x0102u) != 4u ||
	    guest_segment(0x0e00u)[0x0104u] != 0x81u ||
	    guest_segment(0x0e00u)[0x0105u] != 0xfcu ||
	    guest_segment(0x0e00u)[0x0106u] != 0u ||
	    guest_segment(0x0e00u)[0x0107u] != 0u ||
	    guest_segment(0x0e00u)[2u + 0x80u] != 0x80u)
		return 4;
	registers = test_state(0x6601u);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    dos_register_low16(registers.ebx) != 936u ||
	    dos_register_low16(registers.edx) != 437u)
		return 5;
	registers = test_state(0x6602u);
	dos_register_set_low16(&registers.ebx, 850u);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    (registers.eflags & DOS_EFLAGS_CF) == 0u ||
	    dos_register_low16(registers.eax) != DOS_ERROR_INVALID_FUNCTION ||
	    context.nls.active->code_page != 936u ||
	    context.nls.generation != 2u)
		return 6;
	registers = test_state(0x6600u);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    (registers.eflags & DOS_EFLAGS_CF) == 0u ||
	    dos_register_low16(registers.eax) != DOS_ERROR_INVALID_FUNCTION)
		return 7;
	return 0;
}

static int test_extended_code_system(void)
{
	struct dos_machine machine;
	struct dos_memory_arena arena;
	struct dos_int21_context context;
	struct dos_cpu_state registers;
	struct dos_cpu_state before;
	struct dos_nls_dbcs_table table;
	struct dos_nls_runtime nls_runtime;
	struct dos_nls_switch first_switch;
	struct dos_nls_switch second_switch;
	uint8_t *dbcs;

	if (!reset_runtime(&machine, &arena, &context) ||
	    context.interim_console_mode)
		return 1;
	registers = test_state(0x6300u);
	registers.eflags |= DOS_EFLAGS_CF;
	before = registers;
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    dos_register_low16(registers.eax) != 0x6300u ||
	    registers.ds != 0x0e00u ||
	    dos_register_low16(registers.esi) != 0x0104u ||
	    (registers.eflags & DOS_EFLAGS_CF) != 0u ||
	    registers.ebx != before.ebx || registers.ecx != before.ecx ||
	    registers.edx != before.edx || registers.edi != before.edi)
		return 2;
	dbcs = guest_segment(registers.ds) + dos_register_low16(registers.esi);
	if (dbcs[0] != 0u || dbcs[1] != 0u)
		return 3;
	if (!dos_nls_get_dbcs_table(932u, &table) || table.length != 6u ||
	    table.ranges[0] != 0x81u || table.ranges[1] != 0x9fu ||
	    table.ranges[2] != 0xe0u || table.ranges[3] != 0xfcu ||
	    table.ranges[4] != 0u || table.ranges[5] != 0u ||
	    !dos_nls_get_dbcs_table(934u, &table) || table.length != 4u ||
	    table.ranges[0] != 0x81u || table.ranges[1] != 0xbfu ||
	    !dos_nls_get_dbcs_table(936u, &table) || table.length != 4u ||
	    table.ranges[0] != 0x81u || table.ranges[1] != 0xfcu ||
	    !dos_nls_get_dbcs_table(938u, &table) || table.length != 4u ||
	    table.ranges[0] != 0x81u || table.ranges[1] != 0xfcu ||
	    dos_nls_get_dbcs_table(999u, &table) ||
	    dos_nls_get_dbcs_table(437u, NULL))
		return 4;
	if (!dos_nls_runtime_initialize(&nls_runtime, 437u, 437u) ||
	    !dos_nls_prepare_switch(&nls_runtime, 932u, &first_switch) ||
	    !dos_nls_prepare_switch(&nls_runtime, 936u, &second_switch) ||
	    !dos_nls_commit_switch(&nls_runtime, &second_switch) ||
	    nls_runtime.active->code_page != 936u ||
	    nls_runtime.generation != 2u ||
	    dos_nls_commit_switch(&nls_runtime, &first_switch))
		return 5;
	dos_nls_abort_switch(&first_switch);

	registers = test_state(0x6301u);
	dos_register_set_low8(&registers.edx, 1u);
	registers.eflags |= DOS_EFLAGS_CF;
	before = registers;
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    !context.interim_console_mode ||
	    (registers.eflags & DOS_EFLAGS_CF) != 0u ||
	    registers.eax != before.eax || registers.edx != before.edx)
		return 6;
	registers = test_state(0x6302u);
	dos_register_set_low8(&registers.edx, 0xaau);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    dos_register_low8(registers.edx) != 1u ||
	    (registers.eflags & DOS_EFLAGS_CF) != 0u)
		return 7;

	registers = test_state(0x63ffu);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    (registers.eflags & DOS_EFLAGS_CF) == 0u ||
	    dos_register_low16(registers.eax) != DOS_ERROR_INVALID_FUNCTION)
		return 8;
	return 0;
}

static int test_fixed_disk_device_control(void)
{
	struct dos_machine machine;
	struct dos_memory_arena arena;
	struct dos_int21_context context;
	struct dos_cpu_state registers;
	struct dos_cpu_state before;

	if (!reset_runtime(&machine, &arena, &context))
		return 1;
	registers = test_state(0x4408u);
	dos_register_set_low16(&registers.ebx, 3u);
	registers.eflags |= DOS_EFLAGS_CF;
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    dos_register_low16(registers.eax) != 1u ||
	    (registers.eflags & DOS_EFLAGS_CF) != 0u)
		return 2;
	registers = test_state(0x4408u);
	dos_register_set_low16(&registers.ebx, 2u);
	registers.eflags &= ~DOS_EFLAGS_CF;
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    dos_register_low16(registers.eax) != DOS_ERROR_INVALID_DRIVE ||
	    (registers.eflags & DOS_EFLAGS_CF) == 0u)
		return 3;
	registers = test_state(0x4409u);
	before = registers;
	if (dos_int21_dispatch(&context, &registers) !=
		    DOS_INT21_UNIMPLEMENTED ||
	    !states_equal(&registers, &before))
		return 4;
	return 0;
}

static int test_disk_free_space(void)
{
	struct dos_machine machine;
	struct dos_memory_arena arena;
	struct dos_int21_context context;
	struct dos_cpu_state registers;
	struct dos_cpu_state before;

	if (!reset_runtime(&machine, &arena, &context))
		return 1;
	registers = test_state(0x3644u);
	before = registers;
	if (dos_int21_dispatch(&context, &registers) !=
		    DOS_INT21_UNIMPLEMENTED ||
	    !states_equal(&registers, &before) ||
	    dos_int21_set_disk_space_query(&context, NULL,
					   TEST_DISK_SPACE_CONTEXT) !=
		    DOS_INT21_INVALID_ARGUMENT ||
	    dos_int21_set_disk_space_query(&context, test_get_disk_space,
					   TEST_DISK_SPACE_CONTEXT) !=
		    DOS_INT21_HANDLED)
		return 2;

	registers = test_state(0x3644u);
	dos_register_set_low8(&registers.edx, 3u);
	registers.eflags |= DOS_EFLAGS_CF;
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    dos_register_low16(registers.eax) != 4u ||
	    dos_register_low16(registers.ebx) != 40u ||
	    dos_register_low16(registers.ecx) != 512u ||
	    dos_register_low16(registers.edx) != 100u ||
	    (registers.eflags & DOS_EFLAGS_CF) == 0u)
		return 3;

	registers = test_state(0x3644u);
	dos_register_set_low8(&registers.edx, 4u);
	before = registers;
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    dos_register_low16(registers.eax) != 0xffffu ||
	    dos_register_low16(registers.ebx) !=
		dos_register_low16(before.ebx) ||
	    dos_register_low16(registers.ecx) !=
		dos_register_low16(before.ecx) ||
	    dos_register_low16(registers.edx) !=
		dos_register_low16(before.edx))
		return 4;
	return 0;
}

static int test_disk_transfer_address(void)
{
	struct dos_machine machine;
	struct dos_memory_arena arena;
	struct dos_int21_context context;
	struct dos_cpu_state registers;
	struct dos_cpu_state before;

	if (!reset_runtime(&machine, &arena, &context))
		return 1;
	registers = test_state(0x1a44u);
	registers.ds = 0x5044u;
	dos_register_set_low16(&registers.edx, 0x8002u);
	before = registers;
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    !states_equal(&registers, &before) ||
	    context.process_runtime.dta.segment != 0x5044u ||
	    context.process_runtime.dta.offset != 0x8002u)
		return 2;
	registers = test_state(0x2f55u);
	registers.eflags |= DOS_EFLAGS_CF;
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    registers.es != 0x5044u ||
	    dos_register_low16(registers.ebx) != 0x8002u ||
	    (registers.eflags & DOS_EFLAGS_CF) == 0u)
		return 3;
	return 0;
}

static int test_find_first_next_dta(void)
{
	struct dos_machine machine;
	struct dos_memory_arena arena;
	struct dos_int21_context context;
	struct dos_cpu_state registers;
	struct dos_find_record record;
	uint8_t *path = guest_segment(0x0200u) + 0x0100u;
	uint8_t *dta = guest_segment(0x1234u) + DOS_PSP_COMMAND_TAIL_OFFSET;
	uint8_t before[DOS_DTA_FIND_SIZE];
	size_t index;

	if (!reset_runtime(&machine, &arena, &context))
		return 1;
	path[0] = (uint8_t)'*';
	path[1] = (uint8_t)'.';
	path[2] = (uint8_t)'T';
	path[3] = (uint8_t)'X';
	path[4] = (uint8_t)'T';
	path[5] = 0u;
	registers = test_state(0x4e44u);
	registers.ds = 0x0200u;
	dos_register_set_low16(&registers.edx, 0x0100u);
	dos_register_set_low16(&registers.ecx, 5u);
	if (dos_int21_dispatch(&context, &registers) !=
		    DOS_INT21_UNIMPLEMENTED ||
	    dos_int21_set_find_services(&context, &test_find_ops,
					TEST_FIND_CONTEXT) != DOS_INT21_HANDLED)
		return 2;
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    (registers.eflags & DOS_EFLAGS_CF) != 0u ||
	    dos_find_record_decode(dta, &record) != DOS_FIND_OK ||
	    record.search_handle != TEST_SEARCH_HANDLE_FIRST ||
	    record.file_size != 1234u || record.packed_name[0] != (uint8_t)'A')
		return 3;
	registers = test_state(0x4f44u);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    (registers.eflags & DOS_EFLAGS_CF) != 0u ||
	    dos_find_record_decode(dta, &record) != DOS_FIND_OK ||
	    record.search_handle != TEST_SEARCH_HANDLE_SECOND ||
	    record.packed_name[0] != (uint8_t)'B')
		return 4;
	for (index = 0u; index < sizeof(before); ++index)
		before[index] = dta[index];
	registers = test_state(0x4f44u);
	registers.eflags &= ~DOS_EFLAGS_CF;
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    (registers.eflags & DOS_EFLAGS_CF) == 0u ||
	    dos_register_low16(registers.eax) != DOS_ERROR_NO_MORE_FILES)
		return 5;
	for (index = 0u; index < sizeof(before); ++index) {
		if (before[index] != dta[index])
			return 6;
	}
	return 0;
}

static int test_current_psp(void)
{
	struct dos_machine machine;
	struct dos_memory_arena arena;
	struct dos_int21_context context;
	struct dos_process_runtime_snapshot before_set;
	struct dos_cpu_state registers;
	struct dos_cpu_state before;

	if (!reset_runtime(&machine, &arena, &context))
		return 1;
	if (dos_process_runtime_snapshot(&context.process_runtime,
					 &before_set) !=
		DOS_PROCESS_RUNTIME_OK ||
	    dos_int21_set_current_psp(&context, 0x4321u) != DOS_INT21_HANDLED ||
	    dos_process_runtime_preflight_exec(&context.process_runtime,
					       &before_set) !=
		DOS_PROCESS_RUNTIME_STALE_SNAPSHOT ||
	    context.process_runtime.dta.segment != 0x1234u ||
	    context.process_runtime.dta.offset != DOS_PSP_COMMAND_TAIL_OFFSET ||
	    dos_int21_set_current_psp(&context, 0u) != DOS_INT21_HANDLED ||
	    context.process_runtime.current_psp != 0u)
		return 2;

	registers = test_state(0x51aau);
	registers.ebx |= 0xbeefu;
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    registers.eax != 0xa5a551aau || registers.ebx != 0xb6b60000u ||
	    registers.eflags != 0x000002d7u)
		return 3;

	/* AH=50h stores BX verbatim without rewriting the caller state. */
	registers = test_state(0x50aau);
	registers.ebx |= 0x4321u;
	before = registers;
	context.extended_error.locus = DOS_ERROR_LOCUS_SERIAL_DEVICE;
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    !states_equal(&registers, &before) ||
	    context.process_runtime.current_psp != 0x4321u ||
	    context.extended_error.locus != DOS_ERROR_LOCUS_SERIAL_DEVICE)
		return 4;
	registers = test_state(0x62bbu);
	registers.ebx |= 0xcafeu;
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    registers.eax != 0xa5a562bbu || registers.ebx != 0xb6b64321u ||
	    registers.eflags != 0x000002d7u)
		return 5;

	registers = test_state(0x50ccu);
	before = registers;
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    !states_equal(&registers, &before) ||
	    context.process_runtime.current_psp != 0u)
		return 6;
	/* AH=50h does not require a guest-memory operation. */
	context.machine.ops = NULL;
	registers = test_state(0x50aau);
	registers.ebx |= 0xffffu;
	before = registers;
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    !states_equal(&registers, &before) ||
	    context.process_runtime.current_psp != 0xffffu)
		return 7;
	return 0;
}

static int test_interrupt_vectors(void)
{
	struct dos_machine machine;
	struct dos_memory_arena arena;
	struct dos_int21_context context;
	struct dos_cpu_state registers;
	struct dos_cpu_state expected;
	struct dos_extended_error error_before;
	uint8_t *encoded_vector = guest_memory + (0x80u * 4u);

	if (!reset_runtime(&machine, &arena, &context))
		return 1;
	registers = test_state(0x2580u);
	dos_register_set_low16(&registers.edx, 0xbeefu);
	registers.ds = 0xcafeu;
	expected = registers;
	context.extended_error.locus = DOS_ERROR_LOCUS_SERIAL_DEVICE;
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    !states_equal(&registers, &expected) ||
	    read_le16(encoded_vector) != 0xbeefu ||
	    read_le16(encoded_vector + 2u) != 0xcafeu ||
	    context.extended_error.locus != DOS_ERROR_LOCUS_UNKNOWN)
		return 2;

	registers = test_state(0x3580u);
	registers.ebx |= 0x1111u;
	registers.es = 0x2222u;
	registers.eflags &= ~DOS_EFLAGS_CF;
	expected = registers;
	dos_register_set_low16(&expected.ebx, 0xbeefu);
	expected.es = 0xcafeu;
	context.extended_error.locus = DOS_ERROR_LOCUS_SERIAL_DEVICE;
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    !states_equal(&registers, &expected) ||
	    context.extended_error.locus != DOS_ERROR_LOCUS_UNKNOWN)
		return 3;

	/* A backend boundary fault is private: no guest result is fabricated.
	 */
	context.machine.address_limit = 4u;
	context.extended_error = (struct dos_extended_error){
	    .code = DOS_ERROR_ACCESS_DENIED,
	    .error_class = DOS_ERROR_CLASS_AUTHORIZATION,
	    .action = DOS_ERROR_ACTION_ABORT,
	    .locus = DOS_ERROR_LOCUS_NETWORK,
	};
	error_before = context.extended_error;
	registers = test_state(0x2502u);
	expected = registers;
	if (dos_int21_dispatch(&context, &registers) !=
		DOS_INT21_MACHINE_FAULT ||
	    !states_equal(&registers, &expected) ||
	    context.extended_error.code != error_before.code ||
	    context.extended_error.error_class != error_before.error_class ||
	    context.extended_error.action != error_before.action ||
	    context.extended_error.locus != error_before.locus)
		return 4;

	registers = test_state(0x3502u);
	expected = registers;
	if (dos_int21_dispatch(&context, &registers) !=
		DOS_INT21_MACHINE_FAULT ||
	    !states_equal(&registers, &expected) ||
	    context.extended_error.code != error_before.code ||
	    context.extended_error.error_class != error_before.error_class ||
	    context.extended_error.action != error_before.action ||
	    context.extended_error.locus != error_before.locus)
		return 5;

	context.machine.address_limit = TEST_MEMORY_BYTES;
	write_le16(encoded_vector, 0x1111u);
	write_le16(encoded_vector + 2u, 0x2222u);
	write_failures_remaining = 1u;
	first_write_partial_bytes = 2u;
	registers = test_state(0x2580u);
	dos_register_set_low16(&registers.edx, 0x3333u);
	registers.ds = 0x4444u;
	expected = registers;
	if (dos_int21_dispatch(&context, &registers) !=
		DOS_INT21_MACHINE_FAULT ||
	    !states_equal(&registers, &expected) || context.machine_poisoned ||
	    read_le16(encoded_vector) != 0x1111u ||
	    read_le16(encoded_vector + 2u) != 0x2222u ||
	    context.extended_error.code != error_before.code ||
	    context.extended_error.error_class != error_before.error_class ||
	    context.extended_error.action != error_before.action ||
	    context.extended_error.locus != error_before.locus)
		return 6;

	/* A partial backend read never publishes a half-decoded ES:BX. */
	read_failures_remaining = 1u;
	first_read_partial_bytes = 2u;
	registers = test_state(0x3580u);
	expected = registers;
	if (dos_int21_dispatch(&context, &registers) !=
		DOS_INT21_MACHINE_FAULT ||
	    !states_equal(&registers, &expected) || context.machine_poisoned ||
	    context.extended_error.code != error_before.code ||
	    context.extended_error.error_class != error_before.error_class ||
	    context.extended_error.action != error_before.action ||
	    context.extended_error.locus != error_before.locus)
		return 7;

	/* If both the write and rollback fail, the machine becomes terminal. */
	write_failures_remaining = 2u;
	first_write_partial_bytes = 2u;
	registers = test_state(0x2580u);
	dos_register_set_low16(&registers.edx, 0x5555u);
	registers.ds = 0x6666u;
	expected = registers;
	if (dos_int21_dispatch(&context, &registers) !=
		DOS_INT21_MACHINE_POISONED ||
	    !states_equal(&registers, &expected) || !context.machine_poisoned ||
	    read_le16(encoded_vector) != 0x5555u ||
	    read_le16(encoded_vector + 2u) != 0x2222u ||
	    context.extended_error.code != error_before.code ||
	    context.extended_error.error_class != error_before.error_class ||
	    context.extended_error.action != error_before.action ||
	    context.extended_error.locus != error_before.locus)
		return 8;

	registers = test_state(0x50aau);
	registers.ebx |= 0x7777u;
	expected = registers;
	if (dos_int21_dispatch(&context, &registers) !=
		DOS_INT21_MACHINE_POISONED ||
	    !states_equal(&registers, &expected) ||
	    context.process_runtime.current_psp != 0x1234u)
		return 9;

	/* Ensure the helper is exercised independently of host byte order. */
	write_le16(encoded_vector, 0x1234u);
	write_le16(encoded_vector + 2u, 0x5678u);
	return read_le16(encoded_vector) == 0x1234u &&
		       read_le16(encoded_vector + 2u) == 0x5678u
		   ? 0
		   : 10;
}

static int test_allocate_and_extended_error(void)
{
	struct dos_machine machine;
	struct dos_memory_arena arena;
	struct dos_int21_context context;
	struct dos_cpu_state registers;
	uint16_t block_segment;

	if (!reset_runtime(&machine, &arena, &context))
		return 1;
	if (dos_int21_set_current_psp(&context, 0x4567u) != DOS_INT21_HANDLED)
		return 2;

	registers = test_state(0x4800u);
	registers.ebx |= 5u;
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    registers.eax != 0xa5a52001u || registers.ebx != 0xb6b60005u ||
	    (registers.eflags & DOS_EFLAGS_CF) != 0u ||
	    read_le16(guest_segment(TEST_ARENA_HEAD) + 1u) != 0x4567u)
		return 3;
	block_segment = dos_register_low16(registers.eax);

	registers = test_state(0x4800u);
	registers.ebx |= 30u;
	registers.eflags &= ~DOS_EFLAGS_CF;
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    registers.eax != 0xa5a50008u || registers.ebx != 0xb6b60019u ||
	    (registers.eflags & DOS_EFLAGS_CF) == 0u ||
	    context.extended_error.code != DOS_ERROR_NOT_ENOUGH_MEMORY ||
	    context.extended_error.error_class !=
		DOS_ERROR_CLASS_OUT_OF_RESOURCE ||
	    context.extended_error.action != DOS_ERROR_ACTION_ABORT ||
	    context.extended_error.locus != DOS_ERROR_LOCUS_MEMORY)
		return 4;

	dos_int21_set_extended_error_pointer(&context, 0xa123u, 0xbeefu);
	registers = test_state(0x59aau);
	registers.ebx |= 0xffffu;
	registers.ecx = 0xc7c7aa77u;
	registers.edi = 0xfafa1111u;
	registers.es = 0x9999u;
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    registers.eax != 0xa5a50008u || registers.ebx != 0xb6b60104u ||
	    registers.ecx != 0xc7c70577u || registers.edi != 0xfafabeefu ||
	    registers.es != 0xa123u || (registers.eflags & DOS_EFLAGS_CF) != 0u)
		return 5;

	registers = test_state(0x49aau);
	registers.es = block_segment;
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    registers.eax != 0xa5a549aau ||
	    (registers.eflags & DOS_EFLAGS_CF) != 0u ||
	    read_le16(guest_segment(TEST_ARENA_HEAD) + 1u) != 0u)
		return 6;
	registers = test_state(0x49bbu);
	registers.es = 0x7777u;
	registers.eflags &= ~DOS_EFLAGS_CF;
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    registers.eax != 0xa5a50009u ||
	    (registers.eflags & DOS_EFLAGS_CF) == 0u ||
	    context.extended_error.code != DOS_ERROR_INVALID_BLOCK)
		return 7;

	/* The guest API permits CurrentPDB zero and stores it verbatim. Preserve
	 * that DOS-visible quirk; native EXEC leases reject owner zero
	 * at their stricter API boundary. */
	if (!reset_runtime(&machine, &arena, &context) ||
	    dos_int21_set_current_psp(&context, 0u) != DOS_INT21_HANDLED)
		return 8;
	registers = test_state(0x4800u);
	registers.ebx |= 1u;
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    dos_register_low16(registers.eax) != TEST_ARENA_HEAD + 1u ||
	    (registers.eflags & DOS_EFLAGS_CF) != 0u ||
	    read_le16(guest_segment(TEST_ARENA_HEAD) + 1u) != 0u)
		return 9;
	return 0;
}

static int test_resize(void)
{
	struct dos_machine machine;
	struct dos_memory_arena arena;
	struct dos_int21_context context;
	struct dos_cpu_state registers;
	uint16_t first;
	uint16_t second;

	if (!reset_runtime(&machine, &arena, &context))
		return 1;
	registers = test_state(0x4800u);
	registers.ebx |= 5u;
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED)
		return 2;
	first = dos_register_low16(registers.eax);
	registers = test_state(0x4800u);
	registers.ebx |= 5u;
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED)
		return 3;
	second = dos_register_low16(registers.eax);

	registers = test_state(0x4a00u);
	registers.ebx |= 7u;
	registers.es = first;
	registers.eflags &= ~DOS_EFLAGS_CF;
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    registers.eax != 0xa5a50008u || registers.ebx != 0xb6b60005u ||
	    (registers.eflags & DOS_EFLAGS_CF) == 0u)
		return 4;
	registers = test_state(0x4900u);
	registers.es = second;
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    (registers.eflags & DOS_EFLAGS_CF) != 0u)
		return 5;
	if (dos_int21_set_current_psp(&context, 0x4567u) != DOS_INT21_HANDLED)
		return 6;
	registers = test_state(0x4a00u);
	registers.ebx |= 10u;
	registers.es = first;
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    registers.eax != (0xa5a50000u | first) ||
	    registers.ebx != 0xb6b6000au ||
	    (registers.eflags & DOS_EFLAGS_CF) != 0u ||
	    read_le16(guest_segment((uint16_t)(first - 1u)) + 1u) != 0x4567u ||
	    read_le16(guest_segment((uint16_t)(first - 1u)) + 3u) != 10u)
		return 7;
	return 0;
}

static int test_allocation_strategy_and_dispatch_gaps(void)
{
	static const uint8_t cpm_holes[] = {0x18u, 0x1du, 0x1eu, 0x20u, 0x61u};
	struct dos_machine machine;
	struct dos_memory_arena arena;
	struct dos_int21_context context;
	struct dos_cpu_state registers;
	struct dos_cpu_state before;
	struct dos_extended_error extended_before;
	size_t index;

	if (!reset_runtime(&machine, &arena, &context))
		return 1;
	registers = test_state(0x5800u);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    registers.eax != 0xa5a50000u ||
	    (registers.eflags & DOS_EFLAGS_CF) != 0u)
		return 2;
	registers = test_state(0x5801u);
	registers.ebx |= 7u;
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    registers.eax != 0xa5a55801u || registers.ebx != 0xb6b60007u ||
	    context.memory_arena.strategy != 7u ||
	    arena.strategy != DOS_ALLOC_FIRST_FIT ||
	    (registers.eflags & DOS_EFLAGS_CF) != 0u)
		return 3;
	registers = test_state(0x5802u);
	registers.eflags &= ~DOS_EFLAGS_CF;
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    registers.eax != 0xa5a50001u ||
	    (registers.eflags & DOS_EFLAGS_CF) == 0u ||
	    context.extended_error.code != DOS_ERROR_INVALID_FUNCTION ||
	    context.extended_error.locus != DOS_ERROR_LOCUS_MEMORY)
		return 4;

	/* An unsupported AH above 6Ch changes AL only and preserves caller CF. */
	registers = test_state(0x7e55u);
	before = registers;
	dos_register_set_low8(&before.eax, 0u);
	extended_before = context.extended_error;
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    !states_equal(&registers, &before) ||
	    context.extended_error.code != extended_before.code ||
	    context.extended_error.error_class != extended_before.error_class ||
	    context.extended_error.action != extended_before.action ||
	    context.extended_error.locus != extended_before.locus)
		return 5;
	registers = test_state(0x8055u);
	registers.eflags &= ~DOS_EFLAGS_CF;
	before = registers;
	dos_register_set_low8(&before.eax, 0u);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    !states_equal(&registers, &before))
		return 6;

	/* Valid DOS table entries compose with future dispatchers untouched. */
	registers = test_state(0x3d55u);
	before = registers;
	extended_before = context.extended_error;
	if (dos_int21_dispatch(&context, &registers) !=
		DOS_INT21_UNIMPLEMENTED ||
	    !states_equal(&registers, &before) ||
	    context.extended_error.code != extended_before.code ||
	    context.extended_error.error_class != extended_before.error_class ||
	    context.extended_error.action != extended_before.action ||
	    context.extended_error.locus != extended_before.locus)
		return 7;

	/* Every reserved compatibility hole returns AL=0 without rewriting CF. */
	for (index = 0u; index < ARRAY_SIZE(cpm_holes); ++index) {
		registers = test_state(
		    (uint16_t)((uint16_t)cpm_holes[index] << 8) | 0x55u);
		if ((index & 1u) != 0u)
			registers.eflags &= ~DOS_EFLAGS_CF;
		before = registers;
		dos_register_set_low8(&before.eax, 0u);
		context.extended_error.locus = DOS_ERROR_LOCUS_SERIAL_DEVICE;
		if (dos_int21_dispatch(&context, &registers) !=
			DOS_INT21_HANDLED ||
		    !states_equal(&registers, &before) ||
		    context.extended_error.locus != DOS_ERROR_LOCUS_UNKNOWN)
			return 8;
	}
	return 0;
}

static int test_arena_damage_and_machine_fault(void)
{
	struct dos_machine machine;
	struct dos_memory_arena arena;
	struct dos_int21_context context;
	struct dos_cpu_state registers;
	struct dos_cpu_state before;

	if (!reset_runtime(&machine, &arena, &context))
		return 1;
	guest_segment(TEST_ARENA_HEAD)[0] = 0u;
	registers = test_state(0x4800u);
	registers.ebx |= 9u;
	registers.eflags &= ~DOS_EFLAGS_CF;
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    registers.eax != 0xa5a50007u || registers.ebx != 0xb6b60009u ||
	    (registers.eflags & DOS_EFLAGS_CF) == 0u ||
	    context.extended_error.code != DOS_ERROR_ARENA_TRASHED)
		return 2;

	context.machine.ops = NULL;
	context.extended_error.locus = DOS_ERROR_LOCUS_SERIAL_DEVICE;
	registers = test_state(0x4800u);
	registers.ebx |= 4u;
	before = registers;
	if (dos_int21_dispatch(&context, &registers) !=
		DOS_INT21_MACHINE_FAULT ||
	    !states_equal(&registers, &before) ||
	    context.extended_error.locus != DOS_ERROR_LOCUS_SERIAL_DEVICE)
		return 3;
	return 0;
}

static int test_typed_memory_fault_and_poison(void)
{
	struct dos_machine machine;
	struct dos_memory_arena arena;
	struct dos_int21_context context;
	struct dos_cpu_state registers;
	struct dos_cpu_state before;
	struct dos_extended_error error_before = {
	    .code = DOS_ERROR_ACCESS_DENIED,
	    .error_class = DOS_ERROR_CLASS_AUTHORIZATION,
	    .action = DOS_ERROR_ACTION_ABORT,
	    .locus = DOS_ERROR_LOCUS_NETWORK,
	};

	/* A backend read fault is not forged into DOS_ERROR_ARENA_TRASHED. */
	if (!reset_runtime(&machine, &arena, &context))
		return 1;
	context.extended_error = error_before;
	read_failures_remaining = 1u;
	first_read_partial_bytes = 5u;
	registers = test_state(0x4800u);
	registers.ebx |= 5u;
	before = registers;
	if (dos_int21_dispatch(&context, &registers) !=
		DOS_INT21_MACHINE_FAULT ||
	    !states_equal(&registers, &before) || context.machine_poisoned ||
	    context.memory_arena.machine_poisoned ||
	    context.extended_error.code != error_before.code ||
	    context.extended_error.error_class != error_before.error_class ||
	    context.extended_error.action != error_before.action ||
	    context.extended_error.locus != error_before.locus)
		return 2;

	/* A partial MCB write with a successful rollback is also private. */
	if (!reset_runtime(&machine, &arena, &context))
		return 3;
	context.extended_error = error_before;
	write_failures_remaining = 1u;
	first_write_partial_bytes = 4u;
	registers = test_state(0x4800u);
	registers.ebx |= 5u;
	before = registers;
	if (dos_int21_dispatch(&context, &registers) !=
		DOS_INT21_MACHINE_FAULT ||
	    !states_equal(&registers, &before) || context.machine_poisoned ||
	    context.memory_arena.machine_poisoned ||
	    guest_segment(TEST_ARENA_HEAD)[0] != DOS_MCB_SIGNATURE_END ||
	    read_le16(guest_segment(TEST_ARENA_HEAD) + 1u) != 0u ||
	    context.extended_error.code != error_before.code ||
	    context.extended_error.error_class != error_before.error_class ||
	    context.extended_error.action != error_before.action ||
	    context.extended_error.locus != error_before.locus)
		return 4;

	/* Failed rollback poisons both the arena and the dispatcher. */
	if (!reset_runtime(&machine, &arena, &context))
		return 5;
	context.extended_error = error_before;
	write_failures_remaining = 2u;
	first_write_partial_bytes = 4u;
	registers = test_state(0x4800u);
	registers.ebx |= 5u;
	before = registers;
	if (dos_int21_dispatch(&context, &registers) !=
		DOS_INT21_MACHINE_POISONED ||
	    !states_equal(&registers, &before) || !context.machine_poisoned ||
	    !context.memory_arena.machine_poisoned ||
	    context.extended_error.code != error_before.code ||
	    context.extended_error.error_class != error_before.error_class ||
	    context.extended_error.action != error_before.action ||
	    context.extended_error.locus != error_before.locus)
		return 6;
	registers = test_state(0x50aau);
	registers.ebx |= 0x7777u;
	before = registers;
	if (dos_int21_dispatch(&context, &registers) !=
		DOS_INT21_MACHINE_POISONED ||
	    !states_equal(&registers, &before) ||
	    context.process_runtime.current_psp != 0x1234u)
		return 7;

	/* An already-poisoned arena is quarantined before any DOS function. */
	if (!reset_runtime(&machine, &arena, &context))
		return 8;
	context.memory_arena.machine_poisoned = true;
	registers = test_state(0x3000u);
	before = registers;
	if (dos_int21_dispatch(&context, &registers) !=
		DOS_INT21_MACHINE_POISONED ||
	    !states_equal(&registers, &before) || !context.machine_poisoned)
		return 9;
	return 0;
}

static int test_terminate_and_child_return_tuple(void)
{
	struct dos_machine machine;
	struct dos_memory_arena arena;
	struct dos_int21_context context;
	struct dos_cpu_state registers;
	struct dos_cpu_state before;

	if (!reset_runtime(&machine, &arena, &context))
		return 1;
	registers = test_state(0x4c7fu);
	before = registers;
	if (dos_int21_dispatch(&context, &registers) !=
		DOS_INT21_PROCESS_EXITED ||
	    !states_equal(&registers, &before))
		return 2;
	registers = test_state(0x00a5u);
	before = registers;
	if (dos_int21_dispatch(&context, &registers) !=
		DOS_INT21_PROCESS_EXITED ||
	    !states_equal(&registers, &before))
		return 3;
	if (dos_int21_publish_child_return(&context, 2u, 0x7fu) !=
		DOS_INT21_HANDLED)
		return 4;
	registers = test_state(0x4d55u);
	registers.eflags |= DOS_EFLAGS_CF;
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    dos_register_low16(registers.eax) != 0x027fu ||
	    (registers.eflags & DOS_EFLAGS_CF) != 0u)
		return 5;
	registers = test_state(0x4d55u);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    dos_register_low16(registers.eax) != 0u)
		return 6;
	return 0;
}

static int test_set_handle_count(void)
{
	struct dos_machine machine;
	struct dos_memory_arena arena;
	struct dos_int21_context context;
	struct dos_cpu_state registers;
	struct dos_cpu_state expected;
	uint8_t *psp;
	uint8_t *external;
	uint16_t external_segment;
	size_t index;

	if (!reset_runtime(&machine, &arena, &context))
		return 1;
	stage_inline_jft(context.process_runtime.current_psp);
	psp = guest_segment(context.process_runtime.current_psp);

	/* Setup's BX=20 path is the required equal-size no-op. */
	registers = test_state(0x6717u);
	dos_register_set_low16(&registers.ebx, DOS_PSP_DEFAULT_HANDLES);
	expected = registers;
	expected.eflags &= ~DOS_EFLAGS_CF;
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    !states_equal(&registers, &expected) ||
	    read_le16(psp + __builtin_offsetof(struct dos_psp_prefix40,
					       jft_length)) !=
		DOS_PSP_DEFAULT_HANDLES)
		return 2;

	/* The internal clamp must not leak through DOS's saved user BX. */
	registers = test_state(0x67a5u);
	dos_register_set_low16(&registers.ebx, 3u);
	expected = registers;
	expected.eflags &= ~DOS_EFLAGS_CF;
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    !states_equal(&registers, &expected))
		return 3;

	registers = test_state(0x67a5u);
	dos_register_set_low16(&registers.ebx, 0xffffu);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    (registers.eflags & DOS_EFLAGS_CF) == 0u ||
	    dos_register_low16(registers.eax) != DOS_ERROR_INVALID_FUNCTION ||
	    context.extended_error.code != DOS_ERROR_INVALID_FUNCTION)
		return 4;

	registers = test_state(0x6717u);
	dos_register_set_low16(&registers.ebx, 33u);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    (registers.eflags & DOS_EFLAGS_CF) != 0u ||
	    dos_register_low16(registers.ebx) != 33u ||
	    read_le16(psp + __builtin_offsetof(struct dos_psp_prefix40,
					       jft_length)) != 33u ||
	    read_le16(psp + __builtin_offsetof(struct dos_psp_prefix40,
					       jft_pointer)) != 0u)
		return 5;
	external_segment = read_le16(
		psp + __builtin_offsetof(struct dos_psp_prefix40, jft_pointer) +
		2u);
	external = guest_segment(external_segment);
	if (external_segment == 0u || external[0] != 0u ||
	    external[1] != 0u || external[2] != 0u ||
	    read_le16(guest_segment((uint16_t)(external_segment - 1u)) + 1u) !=
		context.process_runtime.current_psp)
		return 6;
	for (index = 3u; index < 33u; ++index) {
		if (external[index] != DOS_JFT_UNUSED)
			return 7;
	}

	external[32] = 7u;
	registers = test_state(0x6700u);
	dos_register_set_low16(&registers.ebx, DOS_PSP_DEFAULT_HANDLES);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    (registers.eflags & DOS_EFLAGS_CF) == 0u ||
	    dos_register_low16(registers.eax) !=
		DOS_ERROR_TOO_MANY_OPEN_FILES ||
	    read_le16(psp + __builtin_offsetof(struct dos_psp_prefix40,
					       jft_length)) != 33u)
		return 8;

	external[32] = DOS_JFT_UNUSED;
	registers = test_state(0x6700u);
	dos_register_set_low16(&registers.ebx, DOS_PSP_DEFAULT_HANDLES);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    (registers.eflags & DOS_EFLAGS_CF) != 0u ||
	    read_le16(psp + __builtin_offsetof(struct dos_psp_prefix40,
					       jft_length)) !=
		DOS_PSP_DEFAULT_HANDLES ||
	    read_le16(psp + __builtin_offsetof(struct dos_psp_prefix40,
					       jft_pointer)) !=
		(uint16_t)__builtin_offsetof(struct dos_psp_prefix40, jft) ||
	    read_le16(psp + __builtin_offsetof(struct dos_psp_prefix40,
					       jft_pointer) + 2u) !=
		context.process_runtime.current_psp ||
	    read_le16(guest_segment((uint16_t)(external_segment - 1u)) + 1u) !=
		0u)
		return 9;
	for (index = 0u; index < DOS_PSP_DEFAULT_HANDLES; ++index) {
		uint8_t expected_entry = index < 3u ? 0u : DOS_JFT_UNUSED;

		if (psp[__builtin_offsetof(struct dos_psp_prefix40, jft) + index] !=
		    expected_entry)
			return 10;
	}
	return 0;
}

static int test_file_date_time_query(void)
{
	struct dos_machine machine;
	struct dos_memory_arena arena;
	struct dos_int21_context context;
	struct dos_cpu_state registers;
	struct dos_sft_registry_publish_record record = {
		.backend_handle = TEST_FILE_HANDLE,
		.position = 0u,
		.size = 1u,
		.flags = 0u,
		.mode = 0u,
		.information = 0x42u,
		.backend_kind = DOS_SFT_BACKEND_FILE,
		.reserved = 0u,
	};
	dos_sft_reference_handle_t reference;
	uint8_t sfn;

	if (!reset_runtime(&machine, &arena, &context))
		return 1;
	stage_inline_jft(context.process_runtime.current_psp);
	if (dos_int21_set_file_services(&context, &test_file_ops,
					TEST_FILE_CONTEXT) != DOS_INT21_HANDLED ||
	    dos_int21_bind_sft_services(&context, TEST_SFT_CONTEXT) !=
		    DOS_INT21_HANDLED ||
	    dos_sft_registry_reserve(TEST_SFT_CONTEXT, &sfn, &reference) !=
		    DOS_SFT_REGISTRY_READY ||
	    dos_sft_registry_publish(TEST_SFT_CONTEXT, reference, &record) !=
		    DOS_SFT_REGISTRY_READY)
		return 2;
	guest_segment(context.process_runtime.current_psp)
		[__builtin_offsetof(struct dos_psp_prefix40, jft) + 5u] = sfn;
	registers = test_state(0x5700u);
	dos_register_set_low16(&registers.ebx, 5u);
	registers.eflags |= DOS_EFLAGS_CF;
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    (registers.eflags & DOS_EFLAGS_CF) != 0u ||
	    dos_register_low16(registers.ecx) != 0x7c21u ||
	    dos_register_low16(registers.edx) != 0x58a5u)
		return 3;
	registers = test_state(0x5701u);
	dos_register_set_low16(&registers.ebx, 5u);
	dos_register_set_low16(&registers.ecx, 0x3462u);
	dos_register_set_low16(&registers.edx, 0x5a31u);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    (registers.eflags & DOS_EFLAGS_CF) != 0u ||
	    file_set_time != 0x3462u || file_set_date != 0x5a31u)
		return 4;
	registers = test_state(0x5700u);
	dos_register_set_low16(&registers.ebx, 6u);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    (registers.eflags & DOS_EFLAGS_CF) == 0u ||
	    dos_register_low16(registers.eax) != DOS_ERROR_INVALID_HANDLE)
		return 5;
	registers = test_state(0x3e00u);
	dos_register_set_low16(&registers.ebx, 5u);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    (registers.eflags & DOS_EFLAGS_CF) != 0u)
		return 6;
	return 0;
}

static int test_dynamic_drive_state(void)
{
	static const uint8_t valid_fcb_source[] = "D:FILE.TXT";
	static const uint8_t invalid_fcb_source[] = "B:FILE.TXT";
	const struct dos_int21_drive_config drives = {
		.available_drive_mask = ((uint32_t)1u << 2u) |
					(uint32_t)1u << 3u,
		.current_drive = 3u,
		.boot_drive = 3u,
		.last_drive = 4u,
		.reserved = 0u,
	};
	struct dos_int21_drive_config invalid = drives;
	struct dos_machine machine;
	struct dos_memory_arena arena;
	struct dos_int21_context context;
	struct dos_cpu_state registers;
	uint8_t *fcb;

	invalid.reserved = 1u;
	if (!dos_int21_drive_config_is_valid(&drives) ||
	    dos_int21_drive_config_is_valid(&invalid) ||
	    dos_int21_drive_config_is_valid(NULL) ||
	    !reset_runtime(&machine, &arena, &context) ||
	    dos_int21_context_initialize(
		&context, &machine, &arena, TEST_RUNTIME_IDENTITY, 0x1234u,
		&drives) != DOS_INT21_HANDLED)
		return 1;

	registers = test_state(0x1900u);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    dos_register_low8(registers.eax) != 3u)
		return 2;
	registers = test_state(0x3305u);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    dos_register_low8(registers.edx) != 3u)
		return 3;

	last_current_directory_drive = 0xffu;
	if (dos_int21_set_current_directory_query(
		    &context, test_get_current_directory,
		    TEST_DIRECTORY_CONTEXT) != DOS_INT21_HANDLED)
		return 4;
	registers = test_state(0x4700u);
	registers.ds = 0x2200u;
	dos_register_set_low16(&registers.esi, 0x0100u);
	dos_register_set_low8(&registers.edx, 0u);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    (registers.eflags & DOS_EFLAGS_CF) != 0u ||
	    last_current_directory_drive != 4u)
		return 5;

	registers = test_state(0x4408u);
	dos_register_set_low8(&registers.ebx, 0u);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    (registers.eflags & DOS_EFLAGS_CF) != 0u ||
	    dos_register_low16(registers.eax) != 1u)
		return 6;
	registers = test_state(0x4408u);
	dos_register_set_low8(&registers.ebx, 4u);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    (registers.eflags & DOS_EFLAGS_CF) != 0u)
		return 7;
	registers = test_state(0x4408u);
	dos_register_set_low8(&registers.ebx, 2u);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    (registers.eflags & DOS_EFLAGS_CF) == 0u ||
	    dos_register_low16(registers.eax) != DOS_ERROR_INVALID_DRIVE)
		return 8;

	registers = test_state(0x0e00u);
	dos_register_set_low8(&registers.edx, 2u);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    dos_register_low8(registers.eax) != 4u ||
	    context.current_drive != 2u)
		return 9;
	registers = test_state(0x0e00u);
	dos_register_set_low8(&registers.edx, 1u);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    dos_register_low8(registers.eax) != 4u ||
	    context.current_drive != 2u)
		return 10;

	stage_bytes(0x2200u, 0x0200u, valid_fcb_source,
		    sizeof(valid_fcb_source));
	registers = test_state(0x2900u);
	registers.ds = 0x2200u;
	registers.es = 0x2200u;
	dos_register_set_low16(&registers.esi, 0x0200u);
	dos_register_set_low16(&registers.edi, 0x0300u);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    dos_register_low8(registers.eax) != 0u)
		return 11;
	fcb = guest_segment(0x2200u) + 0x0300u;
	if (fcb[0] != 4u)
		return 12;
	stage_bytes(0x2200u, 0x0200u, invalid_fcb_source,
		    sizeof(invalid_fcb_source));
	registers = test_state(0x2900u);
	registers.ds = 0x2200u;
	registers.es = 0x2200u;
	dos_register_set_low16(&registers.esi, 0x0200u);
	dos_register_set_low16(&registers.edi, 0x0320u);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    dos_register_low8(registers.eax) != 0xffu)
		return 13;
	return 0;
}

static int test_character_device_bridge(void)
{
	static const uint8_t registered_name[TEST_DOS_DEVICE_NAME_BYTES] = {
		'G', 'E', 'N', 'D', 'E', 'V', ' ', ' ',
	};
	static const uint8_t device_path[] = "gendev.txt";
	static const uint8_t file_path[] = "NORMAL.TXT";
	static const uint8_t missing_parent[] = "C:\\MISSING\\GENDEV";
	static const uint8_t qualified_device[] = "C:\\DIR\\GENDEV.TXT";
	static const uint8_t root_dev_device[] = "C:\\DEV\\GENDEV.TXT";
	static const uint8_t unc_path[] = "\\\\SERVER\\SHARE\\GENDEV";
	struct dos_machine machine;
	struct dos_memory_arena arena;
	struct dos_int21_context context;
	struct dos_cpu_state registers;
	struct dos_sft_registry_view view;
	iomgr_device_registration_handle_t registration;
	uint8_t *jft;
	uint8_t sfn;
	uint16_t mode;

	if (!reset_runtime(&machine, &arena, &context))
		return 1;
	stage_inline_jft(context.process_runtime.current_psp);
	reset_device_callbacks();
	if (!configure_device_bridge(&context) ||
	    !register_test_device(registered_name, &registration))
		return 2;
	jft = guest_segment(context.process_runtime.current_psp) +
	      __builtin_offsetof(struct dos_psp_prefix40, jft);
	stage_bytes(0x2400u, 0x0100u, device_path, sizeof(device_path));
	for (mode = 0u; mode <= 0xffu; ++mode) {
		bool valid = (mode & 0x07u) <= 2u && (mode & 0x08u) == 0u &&
			     (mode & 0x70u) <= 0x40u;

		registers = test_state((uint16_t)(0x3d00u | mode));
		registers.ds = 0x2400u;
		dos_register_set_low16(&registers.edx, 0x0100u);
		if (dos_int21_dispatch(&context, &registers) !=
		    DOS_INT21_HANDLED)
			return 3;
		if (!valid) {
			if ((registers.eflags & DOS_EFLAGS_CF) == 0u ||
			    dos_register_low16(registers.eax) !=
				    DOS_ERROR_INVALID_ACCESS)
				return 4;
			continue;
		}
		if ((registers.eflags & DOS_EFLAGS_CF) != 0u ||
		    dos_register_low16(registers.eax) != 3u ||
		    jft[3] == DOS_JFT_UNUSED)
			return 5;
		registers = test_state(0x3e00u);
		dos_register_set_low16(&registers.ebx, 3u);
		if (dos_int21_dispatch(&context, &registers) !=
			    DOS_INT21_HANDLED ||
		    (registers.eflags & DOS_EFLAGS_CF) != 0u ||
		    jft[3] != DOS_JFT_UNUSED)
			return 6;
	}
	if (device_open_calls != 30u || device_close_calls != 30u)
		return 7;
	reset_device_callbacks();
	bridge_attributes_result = DOS_ERROR_PATH_NOT_FOUND;
	stage_bytes(0x2400u, 0x0100u, missing_parent,
		    sizeof(missing_parent));
	registers = test_state(0x3d02u);
	registers.ds = 0x2400u;
	dos_register_set_low16(&registers.edx, 0x0100u);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    (registers.eflags & DOS_EFLAGS_CF) == 0u ||
	    dos_register_low16(registers.eax) != DOS_ERROR_PATH_NOT_FOUND ||
	    bridge_attributes_calls != 1u || device_open_calls != 0u ||
	    bridge_file.open_calls != 0u)
		return 40;
	bridge_attributes_result = DOS_SUCCESS;
	bridge_attributes_value = 0u;
	registers = test_state(0x3d02u);
	registers.ds = 0x2400u;
	dos_register_set_low16(&registers.edx, 0x0100u);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    (registers.eflags & DOS_EFLAGS_CF) == 0u ||
	    dos_register_low16(registers.eax) != DOS_ERROR_PATH_NOT_FOUND ||
	    bridge_attributes_calls != 2u)
		return 41;
	bridge_attributes_value = 0x10u;
	stage_bytes(0x2400u, 0x0100u, qualified_device,
		    sizeof(qualified_device));
	registers = test_state(0x3d02u);
	registers.ds = 0x2400u;
	dos_register_set_low16(&registers.edx, 0x0100u);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    (registers.eflags & DOS_EFLAGS_CF) != 0u ||
	    dos_register_low16(registers.eax) != 3u ||
	    bridge_attributes_calls != 3u || device_open_calls != 1u)
		return 42;
	registers = test_state(0x3e00u);
	dos_register_set_low16(&registers.ebx, 3u);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED)
		return 43;
	bridge_attributes_result = DOS_ERROR_PATH_NOT_FOUND;
	stage_bytes(0x2400u, 0x0100u, root_dev_device,
		    sizeof(root_dev_device));
	registers = test_state(0x3d02u);
	registers.ds = 0x2400u;
	dos_register_set_low16(&registers.edx, 0x0100u);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    (registers.eflags & DOS_EFLAGS_CF) != 0u ||
	    bridge_attributes_calls != 3u || device_open_calls != 2u)
		return 44;
	registers = test_state(0x3e00u);
	dos_register_set_low16(&registers.ebx, 3u);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED)
		return 45;
	bridge_file.open_result = DOS_SUCCESS;
	stage_bytes(0x2400u, 0x0100u, unc_path, sizeof(unc_path));
	registers = test_state(0x3d02u);
	registers.ds = 0x2400u;
	dos_register_set_low16(&registers.edx, 0x0100u);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    (registers.eflags & DOS_EFLAGS_CF) != 0u ||
	    bridge_file.open_calls != 1u || device_open_calls != 2u)
		return 46;
	registers = test_state(0x3e00u);
	dos_register_set_low16(&registers.ebx, 3u);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED)
		return 47;
	bridge_file.open_calls = 0u;
	bridge_file.close_calls = 0u;
	reset_device_callbacks();
	stage_bytes(0x2400u, 0x0100u, device_path, sizeof(device_path));
	registers = test_state(0x3d02u);
	registers.ds = 0x2400u;
	dos_register_set_low16(&registers.edx, 0x0100u);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    dos_register_low16(registers.eax) != 3u)
		return 8;
	sfn = jft[3];
	if (dos_sft_registry_resolve(TEST_SFT_CONTEXT, sfn, &view) !=
	    DOS_SFT_REGISTRY_READY)
		return 9;
	registers = test_state(0x4400u);
	dos_register_set_low16(&registers.ebx, 3u);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    (registers.eflags & DOS_EFLAGS_CF) != 0u ||
	    dos_register_low16(registers.eax) != 0xc0c0u ||
	    dos_register_low16(registers.edx) != 0xc0c0u)
		return 10;
	device_read_limit = 2u;
	registers = test_state(0x3f00u);
	registers.ds = 0x2400u;
	dos_register_set_low16(&registers.ebx, 3u);
	dos_register_set_low16(&registers.ecx, 4u);
	dos_register_set_low16(&registers.edx, 0x0200u);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    dos_register_low16(registers.eax) != 2u ||
	    guest_segment(0x2400u)[0x0200u] != 0xa0u ||
	    guest_segment(0x2400u)[0x0201u] != 0xa1u)
		return 11;
	guest_segment(0x2400u)[0x0200u] = 0x31u;
	guest_segment(0x2400u)[0x0201u] = 0x32u;
	guest_segment(0x2400u)[0x0202u] = 0x33u;
	device_write_limit = 2u;
	registers = test_state(0x4000u);
	registers.ds = 0x2400u;
	dos_register_set_low16(&registers.ebx, 3u);
	dos_register_set_low16(&registers.ecx, 3u);
	dos_register_set_low16(&registers.edx, 0x0200u);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    dos_register_low16(registers.eax) != 2u ||
	    device_written_count != 2u || device_written[1] != 0x32u)
		return 12;
	device_control_limit = 2u;
	registers = test_state(0x4402u);
	registers.ds = 0x2400u;
	dos_register_set_low16(&registers.ebx, 3u);
	dos_register_set_low16(&registers.ecx, 3u);
	dos_register_set_low16(&registers.edx, 0x0210u);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    dos_register_low16(registers.eax) != 2u ||
	    guest_segment(0x2400u)[0x0210u] != 0xc0u)
		return 13;
	guest_segment(0x2400u)[0x0220u] = 0x41u;
	guest_segment(0x2400u)[0x0221u] = 0x42u;
	guest_segment(0x2400u)[0x0222u] = 0x43u;
	registers = test_state(0x4403u);
	registers.ds = 0x2400u;
	dos_register_set_low16(&registers.ebx, 3u);
	dos_register_set_low16(&registers.ecx, 3u);
	dos_register_set_low16(&registers.edx, 0x0220u);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    dos_register_low16(registers.eax) != 2u ||
	    device_control_written_count != 2u ||
	    device_control_written[1] != 0x42u)
		return 14;
	registers = test_state(0x3e00u);
	dos_register_set_low16(&registers.ebx, 3u);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    jft[3] != DOS_JFT_UNUSED)
		return 15;
	bridge_file.open_result = DOS_SUCCESS;
	stage_bytes(0x2400u, 0x0100u, file_path, sizeof(file_path));
	registers = test_state(0x3d02u);
	registers.ds = 0x2400u;
	dos_register_set_low16(&registers.edx, 0x0100u);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    dos_register_low16(registers.eax) != 3u ||
	    bridge_file.open_calls != 1u)
		return 16;
	registers = test_state(0x4400u);
	dos_register_set_low16(&registers.ebx, 3u);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    dos_register_low16(registers.edx) != 0x42u)
		return 17;
	registers = test_state(0x4402u);
	dos_register_set_low16(&registers.ebx, 3u);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    (registers.eflags & DOS_EFLAGS_CF) == 0u ||
	    dos_register_low16(registers.eax) != DOS_ERROR_INVALID_FUNCTION)
		return 18;
	registers = test_state(0x3e00u);
	dos_register_set_low16(&registers.ebx, 3u);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    bridge_file.close_calls != 1u)
		return 19;
	/* Read/write enforce sf_mode; generic control calls intentionally do not. */
	stage_bytes(0x2400u, 0x0100u, device_path, sizeof(device_path));
	registers = test_state(0x3d01u);
	registers.ds = 0x2400u;
	dos_register_set_low16(&registers.edx, 0x0100u);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED)
		return 20;
	registers = test_state(0x3f00u);
	registers.ds = 0x2400u;
	dos_register_set_low16(&registers.ebx, 3u);
	dos_register_set_low16(&registers.ecx, 1u);
	dos_register_set_low16(&registers.edx, 0x0200u);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    (registers.eflags & DOS_EFLAGS_CF) == 0u ||
	    dos_register_low16(registers.eax) != DOS_ERROR_ACCESS_DENIED)
		return 21;
	registers = test_state(0x4402u);
	registers.ds = 0x2400u;
	dos_register_set_low16(&registers.ebx, 3u);
	dos_register_set_low16(&registers.ecx, 1u);
	dos_register_set_low16(&registers.edx, 0x0210u);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    (registers.eflags & DOS_EFLAGS_CF) != 0u)
		return 22;
	registers = test_state(0x3e00u);
	dos_register_set_low16(&registers.ebx, 3u);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED)
		return 23;
	registers = test_state(0x3d00u);
	registers.ds = 0x2400u;
	dos_register_set_low16(&registers.edx, 0x0100u);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED)
		return 24;
	registers = test_state(0x4000u);
	registers.ds = 0x2400u;
	dos_register_set_low16(&registers.ebx, 3u);
	dos_register_set_low16(&registers.ecx, 1u);
	dos_register_set_low16(&registers.edx, 0x0200u);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    (registers.eflags & DOS_EFLAGS_CF) == 0u ||
	    dos_register_low16(registers.eax) != DOS_ERROR_ACCESS_DENIED)
		return 25;
	registers = test_state(0x4403u);
	registers.ds = 0x2400u;
	dos_register_set_low16(&registers.ebx, 3u);
	dos_register_set_low16(&registers.ecx, 1u);
	dos_register_set_low16(&registers.edx, 0x0220u);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    (registers.eflags & DOS_EFLAGS_CF) != 0u)
		return 26;
	registers = test_state(0x3e00u);
	dos_register_set_low16(&registers.ebx, 3u);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED)
		return 27;
	stage_bytes(0x2400u, 0x0100u, device_path, sizeof(device_path));
	registers = test_state(0x3d02u);
	registers.ds = 0x2400u;
	dos_register_set_low16(&registers.edx, 0x0100u);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED)
		return 28;
	sfn = jft[3];
	if (dos_sft_registry_resolve(TEST_SFT_CONTEXT, sfn, &view) !=
	    DOS_SFT_REGISTRY_READY)
		return 29;
	device_reentry_handle = view.backend_handle;
	device_reenter_read = true;
	registers = test_state(0x3f00u);
	registers.ds = 0x2400u;
	dos_register_set_low16(&registers.ebx, 3u);
	dos_register_set_low16(&registers.ecx, 1u);
	dos_register_set_low16(&registers.edx, 0x0200u);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    (registers.eflags & DOS_EFLAGS_CF) == 0u ||
	    device_reentry_status != IOMGR_BUSY || context.machine_poisoned)
		return 30;
	device_reenter_read = false;
	device_close_result = IOMGR_DEVICE_CALLBACK_BUSY;
	registers = test_state(0x3e00u);
	dos_register_set_low16(&registers.ebx, 3u);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    (registers.eflags & DOS_EFLAGS_CF) == 0u ||
	    jft[3] != sfn)
		return 31;
	device_close_result = IOMGR_DEVICE_CALLBACK_OK;
	registers = test_state(0x3e00u);
	dos_register_set_low16(&registers.ebx, 3u);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED ||
	    jft[3] != DOS_JFT_UNUSED)
		return 32;
	registers = test_state(0x3d02u);
	registers.ds = 0x2400u;
	dos_register_set_low16(&registers.edx, 0x0100u);
	if (dos_int21_dispatch(&context, &registers) != DOS_INT21_HANDLED)
		return 33;
	sfn = jft[3];
	device_close_result = IOMGR_DEVICE_CALLBACK_UNCERTAIN;
	registers = test_state(0x3e00u);
	dos_register_set_low16(&registers.ebx, 3u);
	if (dos_int21_dispatch(&context, &registers) !=
		    DOS_INT21_MACHINE_POISONED ||
	    !context.machine_poisoned || jft[3] != sfn ||
	    dos_sft_registry_resolve(TEST_SFT_CONTEXT, sfn, &view) !=
		    DOS_SFT_REGISTRY_POISONED)
		return 34;
	(void)registration;
	return 0;
}

static int run_tests(void)
{
	const struct dos_sft_backend_close_ops close_ops = {
		.identity = TEST_SFT_CLOSE_IDENTITY,
		.context = TEST_FILE_CONTEXT,
		.close = test_sft_backend_close,
	};
	int status;

	if (iomgr_device_initialize() != IOMGR_OK ||
	    dos_sft_registry_initialize(TEST_SFT_IDENTITY, TEST_SFT_CONTEXT) !=
		    DOS_SFT_REGISTRY_READY ||
	    dos_sft_registry_bind_backend_close(TEST_SFT_CONTEXT, &close_ops) !=
		    DOS_SFT_REGISTRY_READY ||
	    dos_sft_registry_install(0u, 0u, 2u, 3u) !=
		    DOS_SFT_REGISTRY_READY)
		return 1;

	status = test_context_and_version();
	if (status != 0)
		return 10 + status;
	status = test_drive_parameter_block_dispatch();
	if (status != 0)
		return 15 + status;
	status = test_global_code_page();
	if (status != 0)
		return 18 + status;
	status = test_extended_code_system();
	if (status != 0)
		return 19 + status;
	status = test_fixed_disk_device_control();
	if (status != 0)
		return 20 + status;
	status = test_disk_free_space();
	if (status != 0)
		return 23 + status;
	status = test_disk_transfer_address();
	if (status != 0)
		return 25 + status;
	status = test_find_first_next_dta();
	if (status != 0)
		return 28 + status;
	status = test_create_directory_dispatch();
	if (status != 0)
		return 29 + status;
	status = test_current_psp();
	if (status != 0)
		return 20 + status;
	status = test_console_write_and_list_of_lists();
	if (status != 0)
		return 30 + status;
	status = test_console_input_and_flush();
	if (status != 0)
		return 35 + status;
	status = test_interrupt_vectors();
	if (status != 0)
		return 40 + status;
	status = test_allocate_and_extended_error();
	if (status != 0)
		return 50 + status;
	status = test_resize();
	if (status != 0)
		return 60 + status;
	status = test_allocation_strategy_and_dispatch_gaps();
	if (status != 0)
		return 70 + status;
	status = test_arena_damage_and_machine_fault();
	if (status != 0)
		return 80 + status;
	status = test_typed_memory_fault_and_poison();
	if (status != 0)
		return 90 + status;
	status = test_terminate_and_child_return_tuple();
	if (status != 0)
		return 100 + status;
	status = test_set_handle_count();
	if (status != 0)
		return 105 + status;
	status = test_file_date_time_query();
	if (status != 0)
		return 110 + status;
	status = test_dynamic_drive_state();
	if (status != 0)
		return 115 + status;
	status = test_character_device_bridge();
	if (status != 0)
		return 120 + status;
	return 0;
}

DOSC32_TEST_ENTRY(run_tests)

// SPDX-License-Identifier: GPL-2.0-only
/*
 * I/O Manager -> DOS EXEC immutable-reader adapter
 *
 * OPEN, IOCTL, immutable reads and CLOSE retain MS-DOS ordering. Filesystem
 * selection remains entirely below the I/O Manager boundary.
 */
#include "iomgr_exec_adapter.h"

struct iomgr_exec_adapter_owner {
	kernel_object_handle_t context;
	iomgr_volume_handle_t volume;
	bool initialized;
};

static struct iomgr_exec_adapter_owner owner;

static enum dos_exec_file_adapter_status adapter_open(
	kernel_object_handle_t context, const uint8_t *path,
	size_t path_length, struct dos_exec_file_open_result *result);
static enum dos_exec_file_adapter_status adapter_probe_device(
	kernel_object_handle_t context, kernel_object_handle_t reader_context,
	struct dos_exec_file_probe_result *result);
static enum dos_image_read_status adapter_read(
	kernel_object_handle_t reader_context, file_offset_t offset,
	void *destination, size_t destination_capacity, size_t count,
	size_t *bytes_read);
static enum dos_exec_file_close_result adapter_close(
	kernel_object_handle_t context, kernel_object_handle_t reader_context);

static struct dos_exec_file_lease_ops operations = {
	.identity = KERNEL_OBJECT_HANDLE_INVALID,
	.open = adapter_open,
	.probe_device = adapter_probe_device,
	.read = adapter_read,
	.close = adapter_close,
};

static bool identity_is_valid(kernel_object_handle_t identity)
{
	return identity != 0u && identity != KERNEL_OBJECT_HANDLE_INVALID;
}

enum iomgr_exec_adapter_status iomgr_exec_adapter_initialize(
	kernel_object_handle_t adapter_identity,
	kernel_object_handle_t adapter_context, iomgr_volume_handle_t volume)
{
	struct iomgr_volume_info info;

	if (!identity_is_valid(adapter_identity) ||
	    !identity_is_valid(adapter_context) ||
	    iomgr_get_volume_info(volume, &info) != IOMGR_OK)
		return IOMGR_EXEC_ADAPTER_INVALID_ARGUMENT;
	if (owner.initialized)
		return IOMGR_EXEC_ADAPTER_INVALID_STATE;
	owner.context = adapter_context;
	owner.volume = volume;
	owner.initialized = true;
	operations.identity = adapter_identity;
	return IOMGR_EXEC_ADAPTER_READY;
}

const struct dos_exec_file_lease_ops *iomgr_exec_adapter_ops(void)
{
	return owner.initialized ? &operations : NULL;
}

kernel_object_handle_t iomgr_exec_adapter_context(void)
{
	return owner.initialized ? owner.context : KERNEL_OBJECT_HANDLE_INVALID;
}

static struct iomgr_path dos_path(const uint8_t *path, size_t length)
{
	struct iomgr_path prepared;

	if (length >= 2u && path[1] == ':') {
		prepared.bytes = path + 2u;
		prepared.length = length - 2u;
	} else {
		prepared.bytes = path;
		prepared.length = length;
	}
	return prepared;
}

static enum dos_exec_file_adapter_status adapter_open(
	kernel_object_handle_t context, const uint8_t *path,
	size_t path_length, struct dos_exec_file_open_result *result)
{
	struct dos_exec_file_open_result prepared = {
		.reader_context = KERNEL_OBJECT_HANDLE_INVALID,
		.size = 0u,
		.failure_detail = (uint32_t)IOMGR_INVALID_ARGUMENT,
		.reserved = 0u,
	};
	struct iomgr_node_info info;
	struct iomgr_path requested;
	iomgr_file_handle_t file;
	enum iomgr_status status;
	size_t index;

	if (result == NULL)
		return DOS_EXEC_FILE_ADAPTER_FAULT;
	if (!owner.initialized || context != owner.context || path == NULL ||
	    path_length < 2u || path[path_length - 1u] != 0u) {
		*result = prepared;
		return DOS_EXEC_FILE_ADAPTER_FAULT;
	}
	for (index = 0u; index + 1u < path_length; ++index) {
		if (path[index] == 0u) {
			*result = prepared;
			return DOS_EXEC_FILE_ADAPTER_FAULT;
		}
	}
	requested = dos_path(path, path_length - 1u);
	status = iomgr_open_file(owner.volume, &requested, &info, &file);
	if (status != IOMGR_OK) {
		prepared.failure_detail = (uint32_t)status;
		*result = prepared;
		return DOS_EXEC_FILE_ADAPTER_FAULT;
	}
	if (info.size > (uint64_t)(~(file_offset_t)0u)) {
		prepared.failure_detail = (uint32_t)IOMGR_UNSUPPORTED;
		if (iomgr_close_file(file) != IOMGR_OK)
			prepared.failure_detail = (uint32_t)IOMGR_IO_ERROR;
		*result = prepared;
		return DOS_EXEC_FILE_ADAPTER_FAULT;
	}
	prepared.reader_context = file;
	prepared.size = (file_offset_t)info.size;
	prepared.failure_detail = 0u;
	*result = prepared;
	return DOS_EXEC_FILE_ADAPTER_OK;
}

static enum dos_exec_file_adapter_status adapter_probe_device(
	kernel_object_handle_t context, kernel_object_handle_t reader_context,
	struct dos_exec_file_probe_result *result)
{
	if (result == NULL)
		return DOS_EXEC_FILE_ADAPTER_FAULT;
	*result = (struct dos_exec_file_probe_result){
		.failure_detail = (uint32_t)IOMGR_INVALID_ARGUMENT,
		.is_device = 0u,
		.reserved = {0u},
	};
	if (!owner.initialized || context != owner.context ||
	    reader_context == IOMGR_FILE_HANDLE_INVALID)
		return DOS_EXEC_FILE_ADAPTER_FAULT;
	result->failure_detail = 0u;
	return DOS_EXEC_FILE_ADAPTER_OK;
}

static enum dos_image_read_status adapter_read(
	kernel_object_handle_t reader_context, file_offset_t offset,
	void *destination, size_t destination_capacity, size_t count,
	size_t *bytes_read)
{
	if (!owner.initialized || bytes_read == NULL ||
	    (destination == NULL && count != 0u) || count > destination_capacity)
		return DOS_IMAGE_READ_IO_ERROR;
	return iomgr_read_file(reader_context, offset, destination,
			       destination_capacity, count, bytes_read) == IOMGR_OK
		       ? DOS_IMAGE_READ_OK
		       : DOS_IMAGE_READ_IO_ERROR;
}

static enum dos_exec_file_close_result adapter_close(
	kernel_object_handle_t context, kernel_object_handle_t reader_context)
{
	if (!owner.initialized || context != owner.context)
		return DOS_EXEC_FILE_CLOSE_RETAINED;
	return iomgr_close_file(reader_context) == IOMGR_OK
		       ? DOS_EXEC_FILE_CLOSE_CLOSED
		       : DOS_EXEC_FILE_CLOSE_RETAINED;
}

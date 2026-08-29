// SPDX-License-Identifier: GPL-2.0-only
/* Load one native C32 image through the I/O Manager into its user aperture. */
#include "c32_process.h"

#include "iomgr.h"
#include "string.h"
#include "x86_paging.h"

static void clear_bytes(uint8_t *destination, size_t count)
{
	while (count-- != 0u)
		*destination++ = 0u;
}

static void close_failed_image(iomgr_file_handle_t file)
{
	enum iomgr_status status = iomgr_close_file(file);

	(void)status;
}

static void clear_process_aperture(const struct c32_image_plan *plan)
{
	clear_bytes((uint8_t *)(uintptr_t)plan->image_base,
		    plan->memory_bytes);
	clear_bytes((uint8_t *)(uintptr_t)X86_PROTECTED_USER_STACK_FLOOR,
		    X86_PROTECTED_USER_STACK_TOP -
			    X86_PROTECTED_USER_STACK_FLOOR);
}

enum c32_process_load_status c32_process_load(
	iomgr_volume_handle_t volume, const char *path, size_t path_capacity,
	struct c32_image_plan *plan)
{
	uint8_t header[C32_IMAGE_HEADER_BYTES];
	struct c32_image_plan prepared;
	struct iomgr_node_info info;
	struct iomgr_path iomgr_path;
	iomgr_file_handle_t file;
	enum iomgr_status status;
	uint32_t complete_file_size;
	size_t bytes_read;
	size_t path_length;

	if (volume == IOMGR_VOLUME_HANDLE_INVALID || path == NULL ||
	    path_capacity == 0u || plan == NULL)
		return C32_PROCESS_LOAD_INVALID_ARGUMENT;
	path_length = strnlen(path, path_capacity);
	if (path_length == 0u || path_length == path_capacity)
		return C32_PROCESS_LOAD_INVALID_ARGUMENT;
	iomgr_path.bytes = (const uint8_t *)path;
	iomgr_path.length = path_length;
	status = iomgr_open_file(volume, &iomgr_path, &info, &file);
	if (status == IOMGR_NOT_FOUND)
		return C32_PROCESS_LOAD_NOT_FOUND;
	if (status != IOMGR_OK)
		return C32_PROCESS_LOAD_IO_ERROR;
	if (info.size > 0xffffffffu) {
		close_failed_image(file);
		return C32_PROCESS_LOAD_IO_ERROR;
	}
	complete_file_size = (uint32_t)info.size;
	status = iomgr_read_file(file, 0u, header, sizeof(header),
				 sizeof(header), &bytes_read);
	if (status != IOMGR_OK || bytes_read != sizeof(header)) {
		close_failed_image(file);
		return C32_PROCESS_LOAD_IO_ERROR;
	}
	if (c32_image_plan_create(header, sizeof(header), complete_file_size,
				  &prepared) != C32_IMAGE_OK) {
		close_failed_image(file);
		return C32_PROCESS_LOAD_BAD_IMAGE;
	}
	/* Prove every supervisor page before I/O or zeroing touches it.  The two
	 * ranges remain unpublished until the complete image has closed cleanly. */
	if (!x86_paging_supervisor_range_is_writable(prepared.image_base,
						     prepared.memory_bytes) ||
	    !x86_paging_supervisor_range_is_writable(
		    X86_PROTECTED_USER_STACK_FLOOR,
		    X86_PROTECTED_USER_STACK_TOP -
			    X86_PROTECTED_USER_STACK_FLOOR)) {
		close_failed_image(file);
		return C32_PROCESS_LOAD_PAGING_ERROR;
	}
	clear_process_aperture(&prepared);
	status = iomgr_read_file(file, prepared.image_offset,
				 (uint8_t *)(uintptr_t)prepared.image_base,
				 prepared.file_bytes, prepared.file_bytes,
				 &bytes_read);
	if (status != IOMGR_OK || bytes_read != prepared.file_bytes) {
		close_failed_image(file);
		clear_process_aperture(&prepared);
		return C32_PROCESS_LOAD_IO_ERROR;
	}
	if (iomgr_close_file(file) != IOMGR_OK) {
		clear_process_aperture(&prepared);
		return C32_PROCESS_LOAD_IO_ERROR;
	}
	if (!x86_paging_grant_user_range(prepared.image_base,
					 prepared.memory_bytes)) {
		clear_process_aperture(&prepared);
		return C32_PROCESS_LOAD_PAGING_ERROR;
	}
	if (!x86_paging_grant_user_range(
		    X86_PROTECTED_USER_STACK_FLOOR,
		    X86_PROTECTED_USER_STACK_TOP -
			    X86_PROTECTED_USER_STACK_FLOOR)) {
		(void)x86_paging_revoke_user_range(prepared.image_base,
						   prepared.memory_bytes);
		(void)x86_paging_revoke_user_range(
			X86_PROTECTED_USER_STACK_FLOOR,
			X86_PROTECTED_USER_STACK_TOP -
				X86_PROTECTED_USER_STACK_FLOOR);
		clear_process_aperture(&prepared);
		return C32_PROCESS_LOAD_PAGING_ERROR;
	}
	*plan = prepared;
	return C32_PROCESS_LOAD_OK;
}

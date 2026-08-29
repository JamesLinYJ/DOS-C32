/* SPDX-License-Identifier: GPL-2.0-only */
/* Synchronous i386 Ring-3 execution and C32 system-call boundary. */
#ifndef DOSC32_X86_USER_H
#define DOSC32_X86_USER_H

#include "object_identity.h"
#include "types.h"
#include "x86_runtime.h"

typedef bool (*x86_user_console_write_fn)(
	kernel_object_handle_t context, const char *text, size_t count);
typedef size_t (*x86_user_console_read_line_fn)(
	kernel_object_handle_t context, char *text, size_t capacity);
typedef bool (*x86_user_console_clear_fn)(kernel_object_handle_t context);
typedef bool (*x86_user_dos_exec_fn)(
	kernel_object_handle_t context, const uint8_t *path, size_t path_length,
	const uint8_t *command_tail, size_t command_tail_length);
typedef bool (*x86_user_dos_chdir_fn)(
	kernel_object_handle_t context, const uint8_t *path, size_t path_length);
typedef bool (*x86_user_dos_getcwd_fn)(
	kernel_object_handle_t context, char *path, size_t capacity,
	size_t *path_length);
enum x86_user_file_open_status {
	X86_USER_FILE_OPEN_OK = 0,
	X86_USER_FILE_OPEN_NOT_FOUND,
	X86_USER_FILE_OPEN_ERROR
};

typedef enum x86_user_file_open_status (*x86_user_dos_file_open_fn)(
	kernel_object_handle_t context, const uint8_t *path, size_t path_length,
	kernel_object_handle_t *file);
typedef bool (*x86_user_dos_file_read_fn)(
	kernel_object_handle_t context, kernel_object_handle_t file,
	uint64_t offset, uint8_t *destination, size_t capacity,
	size_t *bytes_read);
typedef bool (*x86_user_dos_file_close_fn)(
	kernel_object_handle_t context, kernel_object_handle_t file);

enum x86_user_environment_status {
	X86_USER_ENVIRONMENT_OK = 0,
	X86_USER_ENVIRONMENT_NOT_FOUND,
	X86_USER_ENVIRONMENT_ERROR
};

/*
 * A zero-capacity call returns only value_length.  Nonzero calls copy one
 * bounded range and return both the stable complete length and bytes read.
 */
typedef enum x86_user_environment_status (*x86_user_dos_environment_get_fn)(
	kernel_object_handle_t context, const uint8_t *name, size_t name_length,
	uint32_t value_offset, uint8_t *destination, size_t capacity,
	size_t *value_length, size_t *bytes_read);

struct x86_user_services {
	x86_user_console_write_fn console_write;
	x86_user_console_read_line_fn console_read_line;
	x86_user_console_clear_fn console_clear;
	x86_user_dos_exec_fn dos_exec;
	x86_user_dos_chdir_fn dos_chdir;
	x86_user_dos_getcwd_fn dos_getcwd;
	x86_user_dos_file_open_fn dos_file_open;
	x86_user_dos_file_read_fn dos_file_read;
	x86_user_dos_file_close_fn dos_file_close;
	x86_user_dos_environment_get_fn dos_environment_get;
	kernel_object_handle_t context;
};

enum x86_user_run_status {
	X86_USER_RUN_OK = 0,
	X86_USER_RUN_INVALID_ARGUMENT,
	X86_USER_RUN_BUSY,
	X86_USER_RUN_ENTRY_FAILED,
	X86_USER_RUN_FAULT,
	X86_USER_RUN_RESOURCE_FAULT
};

enum x86_user_run_status x86_user_run(
	uint32_t entry_point, uint32_t stack_top,
	const struct x86_user_services *services, uint32_t *exit_code)
	__must_check;

/* Called only by the common exception dispatcher. */
bool x86_user_handle_trap(struct x86_trap_frame *frame);

#endif

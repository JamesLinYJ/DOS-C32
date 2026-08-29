// SPDX-License-Identifier: GPL-2.0-only
/*
 * INT 21h legacy and extended error mapping.
 *
 * This is a typed representation of DOS's observable table semantics.  A
 * legacy call can receive a permitted fallback in AX while function 59h sees
 * the primary error code and its class/action/locus.  Call-specific metadata is
 * preserved instead of guessed by this common layer.
 */
#include "dos_error.h"

#define DOS_ERROR_METADATA_KEEP 0xffu

struct error_metadata_entry {
	uint8_t code;
	uint8_t error_class;
	uint8_t action;
	uint8_t locus;
};

struct int21_error_map {
	uint8_t function;
	uint8_t count;
	const uint8_t *allowed;
};

#define ERROR_METADATA(error, class_value, action_value, locus_value)       \
	{                                                                     \
		(uint8_t)(error), (uint8_t)(class_value),                      \
		(uint8_t)(action_value), (uint8_t)(locus_value)                \
	}

static const struct error_metadata_entry error_metadata[] = {
	ERROR_METADATA(DOS_ERROR_INVALID_FUNCTION,
		       DOS_ERROR_CLASS_APPLICATION, DOS_ERROR_ACTION_ABORT,
		       DOS_ERROR_METADATA_KEEP),
	ERROR_METADATA(DOS_ERROR_FILE_NOT_FOUND, DOS_ERROR_CLASS_NOT_FOUND,
		       DOS_ERROR_ACTION_ASK_USER, DOS_ERROR_LOCUS_DISK),
	ERROR_METADATA(DOS_ERROR_PATH_NOT_FOUND, DOS_ERROR_CLASS_NOT_FOUND,
		       DOS_ERROR_ACTION_ASK_USER, DOS_ERROR_LOCUS_DISK),
	ERROR_METADATA(DOS_ERROR_TOO_MANY_OPEN_FILES,
		       DOS_ERROR_CLASS_OUT_OF_RESOURCE, DOS_ERROR_ACTION_ABORT,
		       DOS_ERROR_LOCUS_UNKNOWN),
	ERROR_METADATA(DOS_ERROR_ACCESS_DENIED,
		       DOS_ERROR_CLASS_AUTHORIZATION, DOS_ERROR_ACTION_ASK_USER,
		       DOS_ERROR_METADATA_KEEP),
	ERROR_METADATA(DOS_ERROR_INVALID_HANDLE,
		       DOS_ERROR_CLASS_APPLICATION, DOS_ERROR_ACTION_ABORT,
		       DOS_ERROR_LOCUS_UNKNOWN),
	ERROR_METADATA(DOS_ERROR_ARENA_TRASHED,
		       DOS_ERROR_CLASS_APPLICATION, DOS_ERROR_ACTION_PANIC,
		       DOS_ERROR_LOCUS_MEMORY),
	ERROR_METADATA(DOS_ERROR_NOT_ENOUGH_MEMORY,
		       DOS_ERROR_CLASS_OUT_OF_RESOURCE, DOS_ERROR_ACTION_ABORT,
		       DOS_ERROR_LOCUS_MEMORY),
	ERROR_METADATA(DOS_ERROR_INVALID_BLOCK, DOS_ERROR_CLASS_APPLICATION,
		       DOS_ERROR_ACTION_ABORT, DOS_ERROR_LOCUS_MEMORY),
	ERROR_METADATA(DOS_ERROR_BAD_ENVIRONMENT,
		       DOS_ERROR_CLASS_APPLICATION, DOS_ERROR_ACTION_ABORT,
		       DOS_ERROR_LOCUS_MEMORY),
	ERROR_METADATA(DOS_ERROR_BAD_FORMAT, DOS_ERROR_CLASS_BAD_FORMAT,
		       DOS_ERROR_ACTION_ASK_USER, DOS_ERROR_LOCUS_UNKNOWN),
	ERROR_METADATA(DOS_ERROR_INVALID_ACCESS,
		       DOS_ERROR_CLASS_APPLICATION, DOS_ERROR_ACTION_ABORT,
		       DOS_ERROR_LOCUS_UNKNOWN),
	ERROR_METADATA(DOS_ERROR_INVALID_DATA, DOS_ERROR_CLASS_BAD_FORMAT,
		       DOS_ERROR_ACTION_ABORT, DOS_ERROR_LOCUS_UNKNOWN),
	ERROR_METADATA(DOS_ERROR_INVALID_DRIVE, DOS_ERROR_CLASS_NOT_FOUND,
		       DOS_ERROR_ACTION_ASK_USER, DOS_ERROR_LOCUS_DISK),
	ERROR_METADATA(DOS_ERROR_CURRENT_DIRECTORY,
		       DOS_ERROR_CLASS_AUTHORIZATION, DOS_ERROR_ACTION_ASK_USER,
		       DOS_ERROR_LOCUS_DISK),
	ERROR_METADATA(DOS_ERROR_NOT_SAME_DEVICE, DOS_ERROR_CLASS_UNKNOWN,
		       DOS_ERROR_ACTION_ASK_USER, DOS_ERROR_LOCUS_DISK),
	ERROR_METADATA(DOS_ERROR_NO_MORE_FILES, DOS_ERROR_CLASS_NOT_FOUND,
		       DOS_ERROR_ACTION_ASK_USER, DOS_ERROR_LOCUS_DISK),
	ERROR_METADATA(DOS_ERROR_FILE_EXISTS, DOS_ERROR_CLASS_ALREADY_EXISTS,
		       DOS_ERROR_ACTION_ASK_USER, DOS_ERROR_LOCUS_DISK),
	ERROR_METADATA(DOS_ERROR_SHARING_VIOLATION, DOS_ERROR_CLASS_LOCKED,
		       DOS_ERROR_ACTION_DELAY_RETRY, DOS_ERROR_LOCUS_DISK),
	ERROR_METADATA(DOS_ERROR_LOCK_VIOLATION, DOS_ERROR_CLASS_LOCKED,
		       DOS_ERROR_ACTION_DELAY_RETRY, DOS_ERROR_LOCUS_DISK),
	ERROR_METADATA(DOS_ERROR_OUT_OF_STRUCTURES,
		       DOS_ERROR_CLASS_OUT_OF_RESOURCE, DOS_ERROR_ACTION_ABORT,
		       DOS_ERROR_METADATA_KEEP),
	ERROR_METADATA(DOS_ERROR_INVALID_PASSWORD,
		       DOS_ERROR_CLASS_AUTHORIZATION, DOS_ERROR_ACTION_ASK_USER,
		       DOS_ERROR_LOCUS_UNKNOWN),
	ERROR_METADATA(DOS_ERROR_CANNOT_MAKE,
		       DOS_ERROR_CLASS_OUT_OF_RESOURCE, DOS_ERROR_ACTION_ABORT,
		       DOS_ERROR_LOCUS_DISK),
	ERROR_METADATA(DOS_ERROR_NOT_SUPPORTED, DOS_ERROR_CLASS_BAD_FORMAT,
		       DOS_ERROR_ACTION_ASK_USER, DOS_ERROR_LOCUS_NETWORK),
	ERROR_METADATA(DOS_ERROR_ALREADY_ASSIGNED,
		       DOS_ERROR_CLASS_ALREADY_EXISTS, DOS_ERROR_ACTION_ASK_USER,
		       DOS_ERROR_LOCUS_NETWORK),
	ERROR_METADATA(DOS_ERROR_INVALID_PARAMETER, DOS_ERROR_CLASS_BAD_FORMAT,
		       DOS_ERROR_ACTION_ASK_USER, DOS_ERROR_LOCUS_UNKNOWN),
	ERROR_METADATA(DOS_ERROR_FAIL_I24, DOS_ERROR_CLASS_UNKNOWN,
		       DOS_ERROR_ACTION_ABORT, DOS_ERROR_LOCUS_UNKNOWN),
	ERROR_METADATA(DOS_ERROR_SHARING_BUFFER_EXCEEDED,
		       DOS_ERROR_CLASS_OUT_OF_RESOURCE, DOS_ERROR_ACTION_ABORT,
		       DOS_ERROR_LOCUS_MEMORY),
	ERROR_METADATA(DOS_ERROR_HANDLE_EOF, DOS_ERROR_CLASS_OUT_OF_RESOURCE,
		       DOS_ERROR_ACTION_ABORT, DOS_ERROR_LOCUS_UNKNOWN),
	ERROR_METADATA(DOS_ERROR_HANDLE_DISK_FULL,
		       DOS_ERROR_CLASS_OUT_OF_RESOURCE, DOS_ERROR_ACTION_ABORT,
		       DOS_ERROR_LOCUS_UNKNOWN),
	ERROR_METADATA(DOS_ERROR_SYS_COMPONENT_NOT_LOADED,
		       DOS_ERROR_CLASS_UNKNOWN, DOS_ERROR_ACTION_ABORT,
		       DOS_ERROR_LOCUS_DISK),
};

static const uint8_t international_errors[] = {
	DOS_ERROR_INVALID_FUNCTION, DOS_ERROR_FILE_NOT_FOUND
};
static const uint8_t mkdir_errors[] = {
	DOS_ERROR_PATH_NOT_FOUND, DOS_ERROR_FILE_NOT_FOUND,
	DOS_ERROR_ACCESS_DENIED
};
static const uint8_t rmdir_errors[] = {
	DOS_ERROR_CURRENT_DIRECTORY, DOS_ERROR_PATH_NOT_FOUND,
	DOS_ERROR_FILE_NOT_FOUND, DOS_ERROR_ACCESS_DENIED
};
static const uint8_t chdir_errors[] = {
	DOS_ERROR_FILE_NOT_FOUND, DOS_ERROR_PATH_NOT_FOUND
};
static const uint8_t create_errors[] = {
	DOS_ERROR_PATH_NOT_FOUND, DOS_ERROR_FILE_NOT_FOUND,
	DOS_ERROR_TOO_MANY_OPEN_FILES, DOS_ERROR_ACCESS_DENIED
};
static const uint8_t open_errors[] = {
	DOS_ERROR_PATH_NOT_FOUND, DOS_ERROR_FILE_NOT_FOUND,
	DOS_ERROR_INVALID_ACCESS, DOS_ERROR_TOO_MANY_OPEN_FILES,
	DOS_ERROR_NOT_DOS_DISK, DOS_ERROR_ACCESS_DENIED
};
static const uint8_t close_errors[] = { DOS_ERROR_INVALID_HANDLE };
static const uint8_t read_write_errors[] = {
	DOS_ERROR_INVALID_HANDLE, DOS_ERROR_ACCESS_DENIED
};
static const uint8_t unlink_errors[] = {
	DOS_ERROR_PATH_NOT_FOUND, DOS_ERROR_FILE_NOT_FOUND,
	DOS_ERROR_ACCESS_DENIED
};
static const uint8_t lseek_errors[] = {
	DOS_ERROR_INVALID_HANDLE, DOS_ERROR_INVALID_FUNCTION
};
static const uint8_t chmod_errors[] = {
	DOS_ERROR_PATH_NOT_FOUND, DOS_ERROR_FILE_NOT_FOUND,
	DOS_ERROR_INVALID_FUNCTION, DOS_ERROR_ACCESS_DENIED
};
static const uint8_t ioctl_errors[] = {
	DOS_ERROR_INVALID_DRIVE, DOS_ERROR_INVALID_DATA,
	DOS_ERROR_INVALID_FUNCTION, DOS_ERROR_INVALID_HANDLE,
	DOS_ERROR_ACCESS_DENIED
};
static const uint8_t dup_errors[] = {
	DOS_ERROR_INVALID_HANDLE, DOS_ERROR_TOO_MANY_OPEN_FILES
};
static const uint8_t current_dir_errors[] = {
	DOS_ERROR_NOT_DOS_DISK, DOS_ERROR_INVALID_DRIVE
};
static const uint8_t alloc_errors[] = {
	DOS_ERROR_ARENA_TRASHED, DOS_ERROR_NOT_ENOUGH_MEMORY
};
static const uint8_t dealloc_errors[] = {
	DOS_ERROR_ARENA_TRASHED, DOS_ERROR_INVALID_BLOCK
};
static const uint8_t setblock_errors[] = {
	DOS_ERROR_ARENA_TRASHED, DOS_ERROR_INVALID_BLOCK,
	DOS_ERROR_NOT_ENOUGH_MEMORY
};
static const uint8_t exec_errors[] = {
	DOS_ERROR_PATH_NOT_FOUND, DOS_ERROR_INVALID_FUNCTION,
	DOS_ERROR_FILE_NOT_FOUND, DOS_ERROR_TOO_MANY_OPEN_FILES,
	DOS_ERROR_BAD_FORMAT, DOS_ERROR_BAD_ENVIRONMENT,
	DOS_ERROR_NOT_ENOUGH_MEMORY, DOS_ERROR_ACCESS_DENIED
};
static const uint8_t find_first_errors[] = {
	DOS_ERROR_PATH_NOT_FOUND, DOS_ERROR_FILE_NOT_FOUND,
	DOS_ERROR_NO_MORE_FILES
};
static const uint8_t find_next_errors[] = { DOS_ERROR_NO_MORE_FILES };
static const uint8_t rename_errors[] = {
	DOS_ERROR_NOT_SAME_DEVICE, DOS_ERROR_PATH_NOT_FOUND,
	DOS_ERROR_FILE_NOT_FOUND, DOS_ERROR_CURRENT_DIRECTORY,
	DOS_ERROR_ACCESS_DENIED
};
static const uint8_t file_times_errors[] = {
	DOS_ERROR_INVALID_HANDLE, DOS_ERROR_NOT_ENOUGH_MEMORY,
	DOS_ERROR_INVALID_DATA, DOS_ERROR_INVALID_FUNCTION
};
static const uint8_t alloc_oper_errors[] = { DOS_ERROR_INVALID_FUNCTION };
static const uint8_t temp_file_errors[] = {
	DOS_ERROR_PATH_NOT_FOUND, DOS_ERROR_FILE_NOT_FOUND,
	DOS_ERROR_TOO_MANY_OPEN_FILES, DOS_ERROR_ACCESS_DENIED
};
static const uint8_t new_file_errors[] = {
	DOS_ERROR_FILE_EXISTS, DOS_ERROR_PATH_NOT_FOUND,
	DOS_ERROR_FILE_NOT_FOUND, DOS_ERROR_TOO_MANY_OPEN_FILES,
	DOS_ERROR_ACCESS_DENIED
};
static const uint8_t lock_errors[] = {
	DOS_ERROR_INVALID_HANDLE, DOS_ERROR_INVALID_FUNCTION,
	DOS_ERROR_SHARING_BUFFER_EXCEEDED, DOS_ERROR_LOCK_VIOLATION
};
static const uint8_t country_errors[] = {
	DOS_ERROR_INVALID_FUNCTION, DOS_ERROR_FILE_NOT_FOUND
};
static const uint8_t commit_errors[] = { DOS_ERROR_INVALID_HANDLE };
static const uint8_t ext_handle_errors[] = {
	DOS_ERROR_TOO_MANY_OPEN_FILES, DOS_ERROR_NOT_ENOUGH_MEMORY,
	DOS_ERROR_INVALID_FUNCTION
};
static const uint8_t extended_open_errors[] = {
	DOS_ERROR_PATH_NOT_FOUND, DOS_ERROR_FILE_NOT_FOUND,
	DOS_ERROR_INVALID_ACCESS, DOS_ERROR_TOO_MANY_OPEN_FILES,
	DOS_ERROR_FILE_EXISTS, DOS_ERROR_NOT_ENOUGH_MEMORY,
	DOS_ERROR_NOT_DOS_DISK, DOS_ERROR_INVALID_DATA,
	DOS_ERROR_INVALID_FUNCTION, DOS_ERROR_ACCESS_DENIED
};
static const uint8_t media_id_errors[] = {
	DOS_ERROR_INVALID_DRIVE, DOS_ERROR_INVALID_DATA,
	DOS_ERROR_INVALID_FUNCTION, DOS_ERROR_ACCESS_DENIED
};

#define INT21_ERROR_MAP(function_number, array)                           \
	{                                                                   \
		(function_number), (uint8_t)(sizeof(array) / sizeof((array)[0])), \
		(array)                                                       \
	}

static const struct int21_error_map int21_error_maps[] = {
	INT21_ERROR_MAP(0x38u, international_errors),
	INT21_ERROR_MAP(0x39u, mkdir_errors),
	INT21_ERROR_MAP(0x3au, rmdir_errors),
	INT21_ERROR_MAP(0x3bu, chdir_errors),
	INT21_ERROR_MAP(0x3cu, create_errors),
	INT21_ERROR_MAP(0x3du, open_errors),
	INT21_ERROR_MAP(0x3eu, close_errors),
	INT21_ERROR_MAP(0x3fu, read_write_errors),
	INT21_ERROR_MAP(0x40u, read_write_errors),
	INT21_ERROR_MAP(0x41u, unlink_errors),
	INT21_ERROR_MAP(0x42u, lseek_errors),
	INT21_ERROR_MAP(0x43u, chmod_errors),
	INT21_ERROR_MAP(0x44u, ioctl_errors),
	INT21_ERROR_MAP(0x45u, dup_errors),
	INT21_ERROR_MAP(0x46u, dup_errors),
	INT21_ERROR_MAP(0x47u, current_dir_errors),
	INT21_ERROR_MAP(0x48u, alloc_errors),
	INT21_ERROR_MAP(0x49u, dealloc_errors),
	INT21_ERROR_MAP(0x4au, setblock_errors),
	INT21_ERROR_MAP(0x4bu, exec_errors),
	INT21_ERROR_MAP(0x4eu, find_first_errors),
	INT21_ERROR_MAP(0x4fu, find_next_errors),
	INT21_ERROR_MAP(0x56u, rename_errors),
	INT21_ERROR_MAP(0x57u, file_times_errors),
	INT21_ERROR_MAP(0x58u, alloc_oper_errors),
	INT21_ERROR_MAP(0x5au, temp_file_errors),
	INT21_ERROR_MAP(0x5bu, new_file_errors),
	INT21_ERROR_MAP(0x5cu, lock_errors),
	INT21_ERROR_MAP(0x65u, country_errors),
	INT21_ERROR_MAP(0x66u, country_errors),
	INT21_ERROR_MAP(0x67u, ext_handle_errors),
	INT21_ERROR_MAP(0x68u, commit_errors),
	INT21_ERROR_MAP(0x6au, commit_errors),
	INT21_ERROR_MAP(0x69u, media_id_errors),
	INT21_ERROR_MAP(0x6cu, extended_open_errors),
};

void dos_error_record_extended(struct dos_extended_error *extended,
			       enum dos_error error)
{
	size_t index;

	if (extended == NULL)
		return;
	extended->code = (uint16_t)error;
	for (index = 0u; index < sizeof(error_metadata) /
					     sizeof(error_metadata[0]); ++index) {
		const struct error_metadata_entry *entry = &error_metadata[index];

		if ((uint16_t)entry->code != (uint16_t)error)
			continue;
		if (entry->error_class != DOS_ERROR_METADATA_KEEP)
			extended->error_class = entry->error_class;
		if (entry->action != DOS_ERROR_METADATA_KEEP)
			extended->action = entry->action;
		if (entry->locus != DOS_ERROR_METADATA_KEEP)
			extended->locus = entry->locus;
		return;
	}
}

enum dos_error dos_error_map_int21(uint8_t function, enum dos_error error,
				   struct dos_extended_error *extended)
{
	size_t map_index;

	dos_error_record_extended(extended, error);
	for (map_index = 0u; map_index < sizeof(int21_error_maps) /
					       sizeof(int21_error_maps[0]);
	     ++map_index) {
		const struct int21_error_map *map = &int21_error_maps[map_index];
		size_t error_index;

		if (map->function != function)
			continue;
		for (error_index = 0u; error_index < map->count; ++error_index) {
			if ((uint8_t)error == map->allowed[error_index])
				return error;
		}
		return (enum dos_error)map->allowed[map->count - 1u];
	}
	return error;
}

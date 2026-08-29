/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DOSC32_DOS_PATH_H
#define DOSC32_DOS_PATH_H

#include "compiler.h"
#include "types.h"

/* MS-DOS COMMAND and FAT path buffers admit 67 bytes plus NUL. */
#define DOS_PATH_CAPACITY 68u

enum dos_path_status {
	DOS_PATH_OK = 0,
	DOS_PATH_INVALID_ARGUMENT,
	DOS_PATH_INVALID_DRIVE,
	DOS_PATH_TOO_LONG,
	DOS_PATH_INVALID_CHARACTER
};

/*
 * Resolve one bounded DOS path span against a canonical drive-rooted path.
 * The output is absolute, uppercase, backslash-separated and is unchanged
 * unless the complete input has been validated and normalized.
 */
enum dos_path_status dos_path_canonicalize(
	const char *current_path, size_t current_capacity,
	const char *input, size_t input_length,
	char output[DOS_PATH_CAPACITY]) __must_check;

bool dos_path_is_explicit(const char *path, size_t path_length);

#endif

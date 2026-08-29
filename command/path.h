/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DOSC32_COMMAND_PATH_H
#define DOSC32_COMMAND_PATH_H

#include "compiler.h"
#include "types.h"

/* One native COMMAND input line carries at most 126 PATH value bytes. */
#define COMMAND_PATH_UPDATE_CAPACITY 127u

enum command_path_parse_status {
	COMMAND_PATH_PARSE_DISPLAY = 0,
	COMMAND_PATH_PARSE_UPDATE,
	COMMAND_PATH_PARSE_TOO_MANY_PARAMETERS,
	COMMAND_PATH_PARSE_TOO_LONG,
	COMMAND_PATH_PARSE_INVALID_ARGUMENT
};

/*
 * Decode the arguments after the PATH command.  UPDATE is published only
 * after the complete span has been validated; DISPLAY leaves output intact.
 */
enum command_path_parse_status command_path_parse(
	const uint8_t *arguments, size_t arguments_length, uint8_t *value,
	size_t value_capacity, size_t *value_length) __must_check;

#endif

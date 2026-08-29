/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DOSC32_COMMAND_DIRECTORY_H
#define DOSC32_COMMAND_DIRECTORY_H

#include "compiler.h"
#include "types.h"

#define COMMAND_DIRECTORY_PATH_CAPACITY 127u

enum command_directory_action {
	COMMAND_DIRECTORY_QUERY_CURRENT = 0,
	COMMAND_DIRECTORY_QUERY_DRIVE,
	COMMAND_DIRECTORY_CHANGE
};

enum command_directory_parse_status {
	COMMAND_DIRECTORY_PARSE_OK = 0,
	COMMAND_DIRECTORY_PARSE_TOO_MANY_PARAMETERS,
	COMMAND_DIRECTORY_PARSE_TOO_LONG,
	COMMAND_DIRECTORY_PARSE_INVALID_ARGUMENT
};

struct command_directory_plan {
	uint8_t path[COMMAND_DIRECTORY_PATH_CAPACITY];
	size_t path_length;
	enum command_directory_action action;
	uint8_t drive;
};

enum command_directory_parse_status command_directory_parse(
	const uint8_t *arguments, size_t arguments_length,
	struct command_directory_plan *plan) __must_check;

bool command_drive_switch_parse(const char *command, size_t command_length,
				uint8_t *drive);

#endif

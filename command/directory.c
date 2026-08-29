// SPDX-License-Identifier: GPL-2.0-only
/*
 * DOS-C32 protected COMMAND CHDIR and drive-command parser
 *
 * Compatibility contract: no operand queries the default drive, a bare D: operand
 *                queries that drive, and only a real path changes directory
 * Safety changes: counted input and validate-before-publish fixed path state
 */
#include "directory.h"

#include "string.h"

static bool is_drive_letter(uint8_t character)
{
	character = (uint8_t)ascii_toupper((char)character);
	return character >= (uint8_t)'A' && character <= (uint8_t)'Z';
}

bool command_drive_switch_parse(const char *command, size_t command_length,
				uint8_t *drive)
{
	if (command == NULL || drive == NULL || command_length != 2u ||
	    command[1] != ':' || !is_drive_letter((uint8_t)command[0]))
		return false;
	*drive = (uint8_t)ascii_toupper(command[0]);
	return true;
}

enum command_directory_parse_status command_directory_parse(
	const uint8_t *arguments, size_t arguments_length,
	struct command_directory_plan *plan)
{
	struct command_directory_plan prepared = {
		.path = {0u},
		.path_length = 0u,
		.action = COMMAND_DIRECTORY_QUERY_CURRENT,
		.drive = 0u,
	};
	size_t begin = 0u;
	size_t end;
	size_t cursor;

	if ((arguments == NULL && arguments_length != 0u) || plan == NULL)
		return COMMAND_DIRECTORY_PARSE_INVALID_ARGUMENT;
	while (begin < arguments_length &&
	       ascii_isspace((char)arguments[begin]))
		++begin;
	if (begin == arguments_length) {
		*plan = prepared;
		return COMMAND_DIRECTORY_PARSE_OK;
	}
	end = begin;
	while (end < arguments_length &&
	       !ascii_isspace((char)arguments[end]))
		++end;
	cursor = end;
	while (cursor < arguments_length &&
	       ascii_isspace((char)arguments[cursor]))
		++cursor;
	if (cursor != arguments_length)
		return COMMAND_DIRECTORY_PARSE_TOO_MANY_PARAMETERS;
	prepared.path_length = end - begin;
	if (prepared.path_length >= sizeof(prepared.path))
		return COMMAND_DIRECTORY_PARSE_TOO_LONG;
	if (prepared.path_length == 2u && arguments[begin + 1u] == ':' &&
	    is_drive_letter(arguments[begin])) {
		prepared.action = COMMAND_DIRECTORY_QUERY_DRIVE;
		prepared.drive =
			(uint8_t)ascii_toupper((char)arguments[begin]);
	} else {
		prepared.action = COMMAND_DIRECTORY_CHANGE;
		for (cursor = 0u; cursor < prepared.path_length; ++cursor) {
			uint8_t character = arguments[begin + cursor];

			if (character == 0u || character < 0x20u)
				return COMMAND_DIRECTORY_PARSE_INVALID_ARGUMENT;
			prepared.path[cursor] = character;
		}
		prepared.path[prepared.path_length] = 0u;
	}
	*plan = prepared;
	return COMMAND_DIRECTORY_PARSE_OK;
}

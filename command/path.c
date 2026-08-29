// SPDX-License-Identifier: GPL-2.0-only
/*
 * DOS-C32 protected COMMAND PATH command parser
 *
 * Compatibility contract: no argument displays PATH; a lone semicolon clears it;
 *                 semicolons inside a value are separators, not arguments
 * Safety changes: counted input, fixed capacity, validate-before-publish
 */
#include "path.h"

#include "string.h"

static bool is_command_whitespace(uint8_t character)
{
	return ascii_isspace((char)character);
}

enum command_path_parse_status command_path_parse(
	const uint8_t *arguments, size_t arguments_length, uint8_t *value,
	size_t value_capacity, size_t *value_length)
{
	uint8_t prepared[COMMAND_PATH_UPDATE_CAPACITY];
	size_t begin = 0u;
	size_t end;
	size_t cursor;
	size_t length;

	if ((arguments == NULL && arguments_length != 0u) || value == NULL ||
	    value_capacity == 0u || value_length == NULL)
		return COMMAND_PATH_PARSE_INVALID_ARGUMENT;
	while (begin < arguments_length &&
	       is_command_whitespace(arguments[begin]))
		++begin;
	if (begin == arguments_length)
		return COMMAND_PATH_PARSE_DISPLAY;

	if (arguments[begin] == (uint8_t)';') {
		end = begin + 1u;
		while (end < arguments_length &&
		       is_command_whitespace(arguments[end]))
			++end;
		if (end != arguments_length)
			return COMMAND_PATH_PARSE_TOO_MANY_PARAMETERS;
		length = 0u;
	} else {
		end = begin;
		while (end < arguments_length &&
		       !is_command_whitespace(arguments[end]))
			++end;
		cursor = end;
		while (cursor < arguments_length &&
		       is_command_whitespace(arguments[cursor]))
			++cursor;
		if (cursor != arguments_length)
			return COMMAND_PATH_PARSE_TOO_MANY_PARAMETERS;
		length = end - begin;
	}
	if (length >= sizeof(prepared) || length >= value_capacity)
		return COMMAND_PATH_PARSE_TOO_LONG;
	for (cursor = 0u; cursor < length; ++cursor) {
		uint8_t character = arguments[begin + cursor];

		if (character == 0u || character < 0x20u)
			return COMMAND_PATH_PARSE_INVALID_ARGUMENT;
		prepared[cursor] =
			(uint8_t)ascii_toupper((char)character);
	}
	prepared[length] = 0u;

	for (cursor = 0u; cursor <= length; ++cursor)
		value[cursor] = prepared[cursor];
	*value_length = length;
	return COMMAND_PATH_PARSE_UPDATE;
}

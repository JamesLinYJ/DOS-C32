// SPDX-License-Identifier: GPL-2.0-only
/* Bounded canonical DOS path construction shared by COMMAND clients. */
#include "dos_path.h"

#include "string.h"

static bool is_separator(char character)
{
	return character == '\\' || character == '/';
}

static void reset_to_root(char path[DOS_PATH_CAPACITY], size_t *length)
{
	path[0] = 'C';
	path[1] = ':';
	path[2] = '\\';
	path[3] = '\0';
	*length = 3u;
}

static void remove_last_component(char path[DOS_PATH_CAPACITY],
				  size_t *length)
{
	size_t cursor = *length;

	if (cursor <= 3u) {
		reset_to_root(path, length);
		return;
	}
	while (cursor > 3u && path[cursor - 1u] != '\\')
		--cursor;
	*length = cursor > 3u ? cursor - 1u : 3u;
	path[*length] = '\0';
}

static enum dos_path_status append_component(
	char path[DOS_PATH_CAPACITY], size_t *length,
	const char *component, size_t component_length)
{
	size_t needed = component_length;
	size_t index;
	bool add_separator = *length > 3u;

	if (add_separator)
		++needed;
	if (needed >= DOS_PATH_CAPACITY - *length)
		return DOS_PATH_TOO_LONG;
	if (add_separator)
		path[(*length)++] = '\\';
	for (index = 0u; index < component_length; ++index) {
		uint8_t character = (uint8_t)component[index];

		if (character == 0u || character == ':' || character < 0x20u)
			return DOS_PATH_INVALID_CHARACTER;
		path[(*length)++] = ascii_toupper(component[index]);
	}
	path[*length] = '\0';
	return DOS_PATH_OK;
}

enum dos_path_status dos_path_canonicalize(
	const char *current_path, size_t current_capacity,
	const char *input, size_t input_length,
	char output[DOS_PATH_CAPACITY])
{
	char prepared[DOS_PATH_CAPACITY];
	const char *cursor;
	const char *end;
	const char *component;
	size_t current_length;
	size_t component_length;
	size_t length;
	enum dos_path_status status;

	if (current_path == NULL || current_capacity == 0u || input == NULL ||
	    output == NULL)
		return DOS_PATH_INVALID_ARGUMENT;
	current_length = strnlen(current_path,
				 current_capacity < DOS_PATH_CAPACITY
					 ? current_capacity
					 : DOS_PATH_CAPACITY);
	if (current_length < 3u || current_length >= current_capacity ||
	    current_length >= DOS_PATH_CAPACITY ||
	    ascii_toupper(current_path[0]) != 'C' || current_path[1] != ':' ||
	    current_path[2] != '\\')
		return DOS_PATH_INVALID_ARGUMENT;
	if (input_length >= DOS_PATH_CAPACITY)
		return DOS_PATH_TOO_LONG;
	for (length = 0u; length < input_length; ++length) {
		if (input[length] == '\0')
			return DOS_PATH_INVALID_CHARACTER;
	}

	cursor = input;
	end = input + input_length;
	if ((size_t)(end - cursor) >= 2u && cursor[1] == ':') {
		if (ascii_toupper(cursor[0]) != 'C')
			return DOS_PATH_INVALID_DRIVE;
		cursor += 2;
	}
	if (cursor < end && is_separator(*cursor)) {
		reset_to_root(prepared, &length);
		while (cursor < end && is_separator(*cursor))
			++cursor;
	} else {
		if (strscpy_s(prepared, sizeof(prepared), current_path,
			      current_length + 1u) == STRSCPY_TRUNCATED)
			return DOS_PATH_INVALID_ARGUMENT;
		length = current_length;
	}

	while (cursor < end) {
		component = cursor;
		while (cursor < end && !is_separator(*cursor))
			++cursor;
		component_length = (size_t)(cursor - component);
		while (cursor < end && is_separator(*cursor))
			++cursor;
		if (component_length == 0u ||
		    (component_length == 1u && component[0] == '.'))
			continue;
		if (component_length == 2u && component[0] == '.' &&
		    component[1] == '.') {
			remove_last_component(prepared, &length);
			continue;
		}
		status = append_component(prepared, &length, component,
					  component_length);
		if (status != DOS_PATH_OK)
			return status;
	}
	if (memcpy_s(output, DOS_PATH_CAPACITY, prepared, length + 1u,
		     length + 1u) != MEMORY_OK)
		return DOS_PATH_INVALID_ARGUMENT;
	return DOS_PATH_OK;
}

bool dos_path_is_explicit(const char *path, size_t path_length)
{
	size_t index;

	if (path == NULL)
		return false;
	for (index = 0u; index < path_length; ++index) {
		if (is_separator(path[index]) || path[index] == ':')
			return true;
	}
	return false;
}

// SPDX-License-Identifier: GPL-2.0-only
/* Protected COMMAND PATH command parser tests. */
#include "command/path.h"
#include "test_entry.h"

static bool bytes_equal(const uint8_t *left, const uint8_t *right,
			size_t count)
{
	size_t index;

	for (index = 0u; index < count; ++index) {
		if (left[index] != right[index])
			return false;
	}
	return true;
}

static int run_tests(void)
{
	static const uint8_t expected_path[] = "C:\\BIN;C:\\TOOLS";
	static const uint8_t path_arguments[] = "c:\\bin;c:\\tools\t  ";
	static const uint8_t clear_arguments[] = "; \t";
	static const uint8_t invalid_clear[] = ";C:\\BIN";
	static const uint8_t extra_argument[] = "C:\\BIN C:\\ALT";
	static const uint8_t embedded_nul[] = {'C', ':', '\\', 'B', 0u, 'N'};
	uint8_t value[COMMAND_PATH_UPDATE_CAPACITY] = {'K', 'E', 'E', 'P', 0u};
	uint8_t too_long[COMMAND_PATH_UPDATE_CAPACITY];
	size_t value_length = 4u;
	size_t index;
	enum command_path_parse_status status;

	status = command_path_parse(NULL, 0u, value, sizeof(value),
				    &value_length);
	if (status != COMMAND_PATH_PARSE_DISPLAY || value_length != 4u ||
	    !bytes_equal(value, (const uint8_t *)"KEEP", 5u))
		return 1;

	status = command_path_parse((const uint8_t *)" \t", 2u, value,
				    sizeof(value), &value_length);
	if (status != COMMAND_PATH_PARSE_DISPLAY || value_length != 4u ||
	    !bytes_equal(value, (const uint8_t *)"KEEP", 5u))
		return 2;

	status = command_path_parse(path_arguments,
				    sizeof(path_arguments) - 1u, value,
				    sizeof(value), &value_length);
	if (status != COMMAND_PATH_PARSE_UPDATE ||
	    value_length != sizeof(expected_path) - 1u ||
	    !bytes_equal(value, expected_path, sizeof(expected_path)))
		return 3;

	status = command_path_parse(invalid_clear, sizeof(invalid_clear) - 1u,
				    value, sizeof(value), &value_length);
	if (status != COMMAND_PATH_PARSE_TOO_MANY_PARAMETERS ||
	    value_length != sizeof(expected_path) - 1u ||
	    !bytes_equal(value, expected_path, sizeof(expected_path)))
		return 4;

	status = command_path_parse(extra_argument,
				    sizeof(extra_argument) - 1u, value,
				    sizeof(value), &value_length);
	if (status != COMMAND_PATH_PARSE_TOO_MANY_PARAMETERS ||
	    value_length != sizeof(expected_path) - 1u ||
	    !bytes_equal(value, expected_path, sizeof(expected_path)))
		return 5;

	status = command_path_parse(embedded_nul, sizeof(embedded_nul), value,
				    sizeof(value), &value_length);
	if (status != COMMAND_PATH_PARSE_INVALID_ARGUMENT ||
	    value_length != sizeof(expected_path) - 1u ||
	    !bytes_equal(value, expected_path, sizeof(expected_path)))
		return 6;

	for (index = 0u; index < sizeof(too_long); ++index)
		too_long[index] = 'A';
	status = command_path_parse(too_long, sizeof(too_long), value,
				    sizeof(value), &value_length);
	if (status != COMMAND_PATH_PARSE_TOO_LONG ||
	    value_length != sizeof(expected_path) - 1u ||
	    !bytes_equal(value, expected_path, sizeof(expected_path)))
		return 7;

	status = command_path_parse((const uint8_t *)"ABCD", 4u, value, 4u,
				    &value_length);
	if (status != COMMAND_PATH_PARSE_TOO_LONG ||
	    value_length != sizeof(expected_path) - 1u ||
	    !bytes_equal(value, expected_path, sizeof(expected_path)))
		return 8;

	status = command_path_parse(clear_arguments,
				    sizeof(clear_arguments) - 1u, value,
				    sizeof(value), &value_length);
	if (status != COMMAND_PATH_PARSE_UPDATE || value_length != 0u ||
	    value[0] != 0u)
		return 9;

	return 0;
}

DOSC32_TEST_ENTRY(run_tests)

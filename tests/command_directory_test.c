// SPDX-License-Identifier: GPL-2.0-only
/* Protected COMMAND CHDIR/drive parsing tests. */
#include "command/directory.h"
#include "test_entry.h"

static bool path_equal(const uint8_t *left, const uint8_t *right,
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
	static const uint8_t root[] = "C:\\";
	static const uint8_t extra[] = "C:\\ TEMP";
	static const uint8_t embedded_nul[] = {'C', ':', '\\', 0u, 'X'};
	struct command_directory_plan plan;
	struct command_directory_plan sentinel = {
		.path = {'K', 'E', 'E', 'P', 0u},
		.path_length = 4u,
		.action = COMMAND_DIRECTORY_CHANGE,
		.drive = (uint8_t)'Z',
	};
	uint8_t too_long[COMMAND_DIRECTORY_PATH_CAPACITY];
	uint8_t drive;
	size_t index;
	enum command_directory_parse_status status;

	plan = sentinel;
	status = command_directory_parse(NULL, 0u, &plan);
	if (status != COMMAND_DIRECTORY_PARSE_OK ||
	    plan.action != COMMAND_DIRECTORY_QUERY_CURRENT ||
	    plan.path_length != 0u || plan.drive != 0u)
		return 1;

	status = command_directory_parse((const uint8_t *)"c:", 2u, &plan);
	if (status != COMMAND_DIRECTORY_PARSE_OK ||
	    plan.action != COMMAND_DIRECTORY_QUERY_DRIVE ||
	    plan.drive != (uint8_t)'C')
		return 2;

	status = command_directory_parse(root, sizeof(root) - 1u, &plan);
	if (status != COMMAND_DIRECTORY_PARSE_OK ||
	    plan.action != COMMAND_DIRECTORY_CHANGE ||
	    plan.path_length != sizeof(root) - 1u ||
	    !path_equal(plan.path, root, sizeof(root)))
		return 3;

	status = command_directory_parse((const uint8_t *)"C:\\TEMP\t ",
				    sizeof("C:\\TEMP\t ") - 1u, &plan);
	if (status != COMMAND_DIRECTORY_PARSE_OK ||
	    plan.action != COMMAND_DIRECTORY_CHANGE ||
	    plan.path_length != sizeof("C:\\TEMP") - 1u ||
	    !path_equal(plan.path, (const uint8_t *)"C:\\TEMP",
			sizeof("C:\\TEMP")))
		return 4;

	plan = sentinel;
	status = command_directory_parse(extra, sizeof(extra) - 1u, &plan);
	if (status != COMMAND_DIRECTORY_PARSE_TOO_MANY_PARAMETERS ||
	    plan.action != sentinel.action ||
	    plan.path_length != sentinel.path_length ||
	    plan.drive != sentinel.drive ||
	    !path_equal(plan.path, sentinel.path, 5u))
		return 5;

	status = command_directory_parse(embedded_nul, sizeof(embedded_nul),
				    &plan);
	if (status != COMMAND_DIRECTORY_PARSE_INVALID_ARGUMENT ||
	    plan.action != sentinel.action ||
	    plan.path_length != sentinel.path_length ||
	    plan.drive != sentinel.drive ||
	    !path_equal(plan.path, sentinel.path, 5u))
		return 6;

	for (index = 0u; index < sizeof(too_long); ++index)
		too_long[index] = 'A';
	status = command_directory_parse(too_long, sizeof(too_long), &plan);
	if (status != COMMAND_DIRECTORY_PARSE_TOO_LONG ||
	    plan.action != sentinel.action ||
	    plan.path_length != sentinel.path_length ||
	    plan.drive != sentinel.drive ||
	    !path_equal(plan.path, sentinel.path, 5u))
		return 7;

	if (!command_drive_switch_parse("c:", 2u, &drive) ||
	    drive != (uint8_t)'C' ||
	    !command_drive_switch_parse("D:", 2u, &drive) ||
	    drive != (uint8_t)'D' ||
	    command_drive_switch_parse("C:\\", 3u, &drive) ||
	    command_drive_switch_parse("C", 1u, &drive))
		return 8;

	return 0;
}

DOSC32_TEST_ENTRY(run_tests)

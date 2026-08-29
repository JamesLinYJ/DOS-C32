// SPDX-License-Identifier: GPL-2.0-only
/* Freestanding COMMAND PATH1/PATH2 semantic tests. */
#include "shell_external_command.h"
#include "test_entry.h"

#define MAX_PROBES 16u

struct probe_fixture {
	const char *found[4];
	size_t found_count;
	char seen[MAX_PROBES][DOS_PATH_CAPACITY];
	size_t seen_count;
	bool force_error;
};

static bool text_equal(const char *left, const char *right)
{
	size_t index = 0u;

	while (left[index] != '\0' && right[index] != '\0') {
		if (left[index] != right[index])
			return false;
		++index;
	}
	return left[index] == right[index];
}

static enum shell_external_probe_status fixture_probe(
	const char *absolute_path, size_t path_length, void *context)
{
	struct probe_fixture *fixture = context;
	size_t index;

	if (fixture->seen_count >= MAX_PROBES ||
	    path_length >= DOS_PATH_CAPACITY)
		return SHELL_EXTERNAL_PROBE_ERROR;
	for (index = 0u; index < path_length; ++index)
		fixture->seen[fixture->seen_count][index] = absolute_path[index];
	fixture->seen[fixture->seen_count][path_length] = '\0';
	++fixture->seen_count;
	if (fixture->force_error)
		return SHELL_EXTERNAL_PROBE_ERROR;
	for (index = 0u; index < fixture->found_count; ++index) {
		if (text_equal(absolute_path, fixture->found[index]))
			return SHELL_EXTERNAL_PROBE_FOUND;
	}
	return SHELL_EXTERNAL_PROBE_NOT_FOUND;
}

static struct shell_external_request request_for(
	const char *command, size_t command_length, const char *current,
	size_t current_capacity, const char *path, size_t path_length,
	struct probe_fixture *fixture)
{
	struct shell_external_request request = {
		.command = command,
		.command_length = command_length,
		.current_path = current,
		.current_path_capacity = current_capacity,
		.search_path = path,
		.search_path_length = path_length,
		.probe = fixture_probe,
		.probe_context = fixture,
	};

	return request;
}

static int run_tests(void)
{
	static const char root[] = "C:\\";
	static const char work[] = "C:\\WORK";
	static const char path[] = "C:\\BIN;C:\\ALT";
	struct shell_external_result result = { .absolute_path = "unchanged" };
	struct probe_fixture fixture = {
		.found = { "C:\\TOOL.EXE", "C:\\TOOL.BAT" },
		.found_count = 2u,
	};
	struct shell_external_request request = request_for(
		"tool", sizeof("tool") - 1u, root, sizeof(root), NULL, 0u,
		&fixture);
	enum shell_external_status status;

	status = shell_external_resolve(&request, &result);
	if (status != SHELL_EXTERNAL_FOUND ||
	    result.type != SHELL_EXTERNAL_EXE ||
	    !text_equal(result.absolute_path, "C:\\TOOL.EXE") ||
	    fixture.seen_count != 2u ||
	    !text_equal(fixture.seen[0], "C:\\TOOL.COM") ||
	    !text_equal(fixture.seen[1], "C:\\TOOL.EXE"))
		return 1;

	fixture = (struct probe_fixture){
		.found = { "C:\\ALT\\MSD.COM" },
		.found_count = 1u,
	};
	request = request_for("msd", sizeof("msd") - 1u, work,
			      sizeof(work), path, sizeof(path) - 1u, &fixture);
	status = shell_external_resolve(&request, &result);
	if (status != SHELL_EXTERNAL_FOUND ||
	    result.type != SHELL_EXTERNAL_COM ||
	    !text_equal(result.absolute_path, "C:\\ALT\\MSD.COM") ||
	    fixture.seen_count != 7u)
		return 2;

	/* The first matching directory wins even when PATH has a COM. */
	fixture = (struct probe_fixture){
		.found = { "C:\\WORK\\RUN.BAT", "C:\\BIN\\RUN.COM" },
		.found_count = 2u,
	};
	request = request_for("run", sizeof("run") - 1u, work,
			      sizeof(work), path, sizeof(path) - 1u, &fixture);
	status = shell_external_resolve(&request, &result);
	if (status != SHELL_EXTERNAL_FOUND ||
	    result.type != SHELL_EXTERNAL_BAT ||
	    !text_equal(result.absolute_path, "C:\\WORK\\RUN.BAT") ||
	    fixture.seen_count != 3u)
		return 3;

	/* An explicit directory suppresses PATH fallback. */
	fixture = (struct probe_fixture){
		.found = { "C:\\ALT\\MSD.COM" },
		.found_count = 1u,
	};
	request = request_for("BIN\\MSD", sizeof("BIN\\MSD") - 1u, root,
			      sizeof(root), path, sizeof(path) - 1u, &fixture);
	status = shell_external_resolve(&request, &result);
	if (status != SHELL_EXTERNAL_NOT_FOUND || fixture.seen_count != 3u ||
	    !text_equal(fixture.seen[0], "C:\\BIN\\MSD.COM"))
		return 4;

	fixture = (struct probe_fixture){
		.found = { "C:\\BIN\\MSD.EXE" },
		.found_count = 1u,
	};
	request = request_for("c:/bin/./msd.exe",
			      sizeof("c:/bin/./msd.exe") - 1u, work,
			      sizeof(work), path, sizeof(path) - 1u, &fixture);
	status = shell_external_resolve(&request, &result);
	if (status != SHELL_EXTERNAL_FOUND ||
	    result.type != SHELL_EXTERNAL_EXE || fixture.seen_count != 1u ||
	    !text_equal(result.absolute_path, "C:\\BIN\\MSD.EXE"))
		return 5;

	fixture = (struct probe_fixture){0};
	request = request_for("README.TXT", sizeof("README.TXT") - 1u,
			      root, sizeof(root), path, sizeof(path) - 1u,
			      &fixture);
	status = shell_external_resolve(&request, &result);
	if (status != SHELL_EXTERNAL_NOT_FOUND || fixture.seen_count != 0u)
		return 6;

	result = (struct shell_external_result){ .absolute_path = "sentinel" };
	fixture = (struct probe_fixture){ .force_error = true };
	request = request_for("FAIL", sizeof("FAIL") - 1u, root,
			      sizeof(root), NULL, 0u, &fixture);
	status = shell_external_resolve(&request, &result);
	if (status != SHELL_EXTERNAL_STORAGE_ERROR ||
	    !text_equal(result.absolute_path, "sentinel"))
		return 7;

	fixture = (struct probe_fixture){0};
	request = request_for("D:\\MSD", sizeof("D:\\MSD") - 1u, root,
			      sizeof(root), NULL, 0u, &fixture);
	status = shell_external_resolve(&request, &result);
	if (status != SHELL_EXTERNAL_PATH_ERROR || fixture.seen_count != 0u ||
	    !text_equal(result.absolute_path, "sentinel"))
		return 8;

	/* Empty PATH elements are skipped; relative elements use current cwd. */
	fixture = (struct probe_fixture){
		.found = { "C:\\WORK\\TOOLS\\BUILD.EXE" },
		.found_count = 1u,
	};
	request = request_for("build", sizeof("build") - 1u, work,
			      sizeof(work), ";TOOLS;;C:\\ALT;",
			      sizeof(";TOOLS;;C:\\ALT;") - 1u, &fixture);
	status = shell_external_resolve(&request, &result);
	if (status != SHELL_EXTERNAL_FOUND ||
	    result.type != SHELL_EXTERNAL_EXE || fixture.seen_count != 5u ||
	    !text_equal(result.absolute_path,
			"C:\\WORK\\TOOLS\\BUILD.EXE"))
		return 9;

	/* A bad PATH drive is an error and does not fall through to later dirs. */
	result = (struct shell_external_result){ .absolute_path = "sentinel" };
	fixture = (struct probe_fixture){0};
	request = request_for("TOOL", sizeof("TOOL") - 1u, root,
			      sizeof(root), "D:\\BIN;C:\\ALT",
			      sizeof("D:\\BIN;C:\\ALT") - 1u, &fixture);
	status = shell_external_resolve(&request, &result);
	if (status != SHELL_EXTERNAL_PATH_ERROR || fixture.seen_count != 3u ||
	    !text_equal(result.absolute_path, "sentinel"))
		return 10;

	return 0;
}

DOSC32_TEST_ENTRY(run_tests)

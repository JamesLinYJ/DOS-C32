/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DOSC32_SHELL_EXTERNAL_COMMAND_H
#define DOSC32_SHELL_EXTERNAL_COMMAND_H

#include "compiler.h"
#include "dos_path.h"
#include "types.h"

enum shell_external_file_type {
	SHELL_EXTERNAL_COM = 0,
	SHELL_EXTERNAL_EXE,
	SHELL_EXTERNAL_BAT
};

enum shell_external_probe_status {
	SHELL_EXTERNAL_PROBE_FOUND = 0,
	SHELL_EXTERNAL_PROBE_NOT_FOUND,
	SHELL_EXTERNAL_PROBE_ERROR
};

typedef enum shell_external_probe_status (*shell_external_probe_t)(
	const char *absolute_path, size_t path_length, void *context);

enum shell_external_status {
	SHELL_EXTERNAL_FOUND = 0,
	SHELL_EXTERNAL_NOT_FOUND,
	SHELL_EXTERNAL_INVALID_COMMAND,
	SHELL_EXTERNAL_PATH_ERROR,
	SHELL_EXTERNAL_STORAGE_ERROR
};

struct shell_external_request {
	const char *command;
	size_t command_length;
	const char *current_path;
	size_t current_path_capacity;
	const char *search_path;
	size_t search_path_length;
	shell_external_probe_t probe;
	void *probe_context;
};

struct shell_external_result {
	char absolute_path[DOS_PATH_CAPACITY];
	size_t path_length;
	enum shell_external_file_type type;
};

/* Result is published only after a complete, successful directory search. */
enum shell_external_status shell_external_resolve(
	const struct shell_external_request *request,
	struct shell_external_result *result) __must_check;

#endif

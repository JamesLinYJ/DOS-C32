/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DOSC32_SHELL_H
#define DOSC32_SHELL_H

#include "iomgr.h"
#include "types.h"

enum shell_external_run_status {
	SHELL_EXTERNAL_RUN_OK = 0,
	SHELL_EXTERNAL_RUN_DOS_ERROR,
	SHELL_EXTERNAL_RUN_BLOCKED,
	SHELL_EXTERNAL_RUN_FAULT,
	/* Protection UI already reported and acknowledged the machine fault. */
	SHELL_EXTERNAL_RUN_FAULT_REPORTED
};

typedef enum shell_external_run_status (*shell_external_runner_t)(
	const uint8_t *path, size_t path_length, const uint8_t *command_tail,
	size_t command_tail_length);
typedef bool (*shell_directory_commit_t)(const char *canonical_path,
					 size_t path_capacity);

bool shell_init(iomgr_volume_handle_t volume, uint8_t drive_index)
	__must_check;
void shell_set_external_runner(shell_external_runner_t runner);
/*
 * Publishes a validated CHDIR atomically to the DOS file-service owner.
 * Registering the callback also publishes the shell's initial root state.
 */
bool shell_set_directory_commit(shell_directory_commit_t commit);
void shell_run_autoexec(void);
void shell_run(void);
/* line_capacity is the complete readable extent, including any final NUL. */
void shell_execute_line(const char *line, size_t line_capacity);

#endif

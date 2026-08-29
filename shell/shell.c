// SPDX-License-Identifier: GPL-2.0-only
/*
 * DOS-C32 first-stage COMMAND interpreter
 *
 * Compatibility contract: case-insensitive internal-command dispatch, DOS 8.3 paths,
 *                 a boot-selected current drive, and TYPE stopping before ^Z.
 * Safety changes: command lines and paths are bounded, state changes are
 *                 committed only after I/O Manager validation, and file
 *                 reads are capacity-carrying operations.
 */
#include "shell.h"

#include "console.h"
#include "dos_abi.h"
#include "dos_drive.h"
#include "dos_path.h"
#include "dos_ui.h"
#include "iomgr.h"
#include "keyboard.h"
#include "overflow.h"
#include "string.h"
#include "shell_external_command.h"
#include "types.h"

#define SHELL_COMMAND_CAPACITY 128u
#define SHELL_PATH_CAPACITY DOS_PATH_CAPACITY
#define SHELL_FILE_BUFFER_SIZE 256u
#define SHELL_TEXT_LIMIT 96u
#define DOS_TEXT_EOF 0x1au

struct command_arguments {
	const char *text;
	size_t capacity;
	size_t length;
};

typedef void (*command_handler_t)(const struct command_arguments *arguments);

struct command_entry {
	const char *name;
	command_handler_t handler;
};

struct directory_summary {
	uint32_t file_count;
	uint32_t directory_count;
	uint32_t total_bytes;
	bool overflow;
};

static iomgr_volume_handle_t shell_volume;
static char current_path[SHELL_PATH_CAPACITY];
static uint8_t shell_drive_index;
static bool echo_commands;
static bool exit_requested;
static shell_external_runner_t external_runner;
static shell_directory_commit_t directory_commit;

static void command_dir(const struct command_arguments *arguments);
static void command_type(const struct command_arguments *arguments);
static void command_cd(const struct command_arguments *arguments);
static void command_echo(const struct command_arguments *arguments);
static void command_rem(const struct command_arguments *arguments);
static void command_cls(const struct command_arguments *arguments);
static void command_ver(const struct command_arguments *arguments);
static void command_vol(const struct command_arguments *arguments);
static void command_exit(const struct command_arguments *arguments);
static void write_ui(enum dos_ui_message_id id);

static enum iomgr_status canonical_iomgr_path(
	const char *canonical, size_t capacity, struct iomgr_path *path)
{
	size_t length;

	if (canonical == NULL || capacity == 0u || path == NULL)
		return IOMGR_INVALID_ARGUMENT;
	length = strnlen(canonical, capacity);
	if (length == capacity || length < 3u || canonical[1] != ':' ||
	    canonical[2] != '\\' ||
	    ascii_toupper(canonical[0]) != (char)('A' + shell_drive_index))
		return IOMGR_INVALID_NAME;
	path->bytes = (const uint8_t *)canonical + 2u;
	path->length = length - 2u;
	return IOMGR_OK;
}

static enum iomgr_status resolve_shell_path(
	const char *input, size_t input_length,
	char canonical[DOS_PATH_CAPACITY], struct iomgr_path *path)
{
	if (dos_path_canonicalize(current_path, sizeof(current_path), input,
				  input_length, canonical) != DOS_PATH_OK)
		return IOMGR_INVALID_NAME;
	return canonical_iomgr_path(canonical, DOS_PATH_CAPACITY, path);
}

static enum shell_external_probe_status probe_external(
	const char *absolute_path, size_t path_length, void *context)
{
	char canonical[DOS_PATH_CAPACITY];
	struct iomgr_path requested;
	struct iomgr_node_info info;
	iomgr_file_handle_t file;
	enum iomgr_status status;

	(void)context;
	status = resolve_shell_path(absolute_path, path_length, canonical,
				    &requested);
	if (status == IOMGR_OK)
		status = iomgr_open_file(shell_volume, &requested, &info, &file);
	if (status == IOMGR_OK) {
		if (iomgr_close_file(file) != IOMGR_OK)
			return SHELL_EXTERNAL_PROBE_ERROR;
		return SHELL_EXTERNAL_PROBE_FOUND;
	}
	return status == IOMGR_NOT_FOUND ? SHELL_EXTERNAL_PROBE_NOT_FOUND
					 : SHELL_EXTERNAL_PROBE_ERROR;
}

static bool run_external_command(const char *command, size_t command_length,
				 const struct command_arguments *arguments)
{
	struct shell_external_request request = {
		.command = command,
		.command_length = command_length,
		.current_path = current_path,
		.current_path_capacity = sizeof(current_path),
		.search_path = NULL,
		.search_path_length = 0u,
		.probe = probe_external,
		.probe_context = NULL,
	};
	struct shell_external_result resolved;
	uint8_t tail[DOS_COMMAND_TAIL_BYTES - 1u];
	size_t tail_length = 0u;
	size_t index;
	enum shell_external_status resolve_status;
	enum shell_external_run_status run_status;

	resolve_status = shell_external_resolve(&request, &resolved);
	if (resolve_status == SHELL_EXTERNAL_NOT_FOUND)
		return false;
	if (resolve_status != SHELL_EXTERNAL_FOUND ||
	    resolved.type == SHELL_EXTERNAL_BAT || external_runner == NULL) {
		write_ui(DOS_UI_COMMAND_NOT_FOUND);
		console_write(command, command_length);
		console_putc('\n');
		return true;
	}
	if (arguments->length != 0u) {
		if (arguments->length + 1u > sizeof(tail)) {
			write_ui(DOS_UI_COMMAND_LINE_TOO_LONG);
			return true;
		}
		tail[tail_length++] = ' ';
		for (index = 0u; index < arguments->length; ++index)
			tail[tail_length++] = (uint8_t)arguments->text[index];
	}
	run_status = external_runner(
		(const uint8_t *)resolved.absolute_path, resolved.path_length,
		tail, tail_length);
	if (run_status == SHELL_EXTERNAL_RUN_DOS_ERROR)
		console_write_literal("Program load failed.\n");
	else if (run_status == SHELL_EXTERNAL_RUN_BLOCKED)
		console_write_literal("Program stopped at an unsupported DOS service.\n");
	else if (run_status == SHELL_EXTERNAL_RUN_FAULT)
		console_write_literal("Program stopped after a machine fault.\n");
	return true;
}

/* Built-in command table for the first milestone. */
static const struct command_entry command_table[] = {
	{ "DIR", command_dir },	  { "TYPE", command_type },
	{ "CD", command_cd },	  { "CHDIR", command_cd },
	{ "ECHO", command_echo }, { "REM", command_rem },
	{ "CLS", command_cls },	  { "VER", command_ver },
	{ "VOL", command_vol },	  { "EXIT", command_exit },
};

static void write_text(const char *text, size_t capacity)
{
	if (text == NULL)
		return;
	console_write(text, strnlen(text, capacity));
}

static void write_path(const char *path)
{
	write_text(path, SHELL_PATH_CAPACITY);
}

static void write_ui(enum dos_ui_message_id id)
{
	struct dos_ui_text text = dos_ui_text_get(id);

	console_write(text.data, text.length);
}

static void write_shell_drive(void)
{
	console_putc((char)('A' + shell_drive_index));
}

static void write_iomgr_error(const char *operation, enum iomgr_status status)
{
	write_text(operation, SHELL_TEXT_LIMIT);
	write_ui(DOS_UI_IOMGR_ERROR_PREFIX);
	switch (status) {
	case IOMGR_CORRUPT:
	case IOMGR_POISONED:
		write_ui(DOS_UI_IOMGR_CORRUPT);
		return;
	case IOMGR_UNSUPPORTED:
	case IOMGR_NO_DRIVER:
		write_ui(DOS_UI_IOMGR_UNSUPPORTED);
		return;
	case IOMGR_IO_ERROR:
	case IOMGR_UNCERTAIN:
		write_ui(DOS_UI_IOMGR_IO_ERROR);
		return;
	case IOMGR_READ_ONLY:
		write_ui(DOS_UI_IOMGR_READ_ONLY);
		return;
	case IOMGR_NO_SPACE:
	case IOMGR_NO_SLOT:
		write_ui(DOS_UI_IOMGR_NO_SPACE);
		return;
	case IOMGR_NOT_FOUND:
		write_ui(DOS_UI_IOMGR_NOT_FOUND);
		return;
	case IOMGR_INVALID_ARGUMENT:
	case IOMGR_NOT_DIRECTORY:
	case IOMGR_IS_DIRECTORY:
	case IOMGR_INVALID_NAME:
		write_ui(DOS_UI_IOMGR_INVALID_PATH);
		return;
	case IOMGR_BUSY:
		write_ui(DOS_UI_IOMGR_BUSY);
		return;
	default:
		write_ui(DOS_UI_IOMGR_INTERNAL_ERROR);
		console_write_u32((uint32_t)status);
		console_write_literal(".\n");
		return;
	}
}

static void syntax_error(void)
{
	write_ui(DOS_UI_SHELL_SYNTAX_ERROR);
}

static bool text_equal_ascii_case(const char *left, size_t left_length,
				  const char *right)
{
	size_t index;

	for (index = 0u; index < left_length; ++index) {
		if (right[index] == '\0' ||
		    ascii_toupper(left[index]) != ascii_toupper(right[index]))
			return false;
	}
	return right[index] == '\0';
}

static bool has_no_arguments(size_t arguments_length)
{
	if (arguments_length == 0u)
		return true;
	syntax_error();
	return false;
}

static bool is_single_argument(const char *arguments, size_t arguments_length,
			       bool allow_empty)
{
	size_t index;

	if (arguments_length == 0u) {
		if (allow_empty)
			return true;
		syntax_error();
		return false;
	}
	for (index = 0u; index < arguments_length; ++index) {
		if (ascii_isspace(arguments[index])) {
			syntax_error();
			return false;
		}
	}
	return true;
}

static void write_number_field(uint32_t value, size_t width)
{
	uint32_t probe = value;
	size_t digits = 1u;

	while (probe >= 10u) {
		probe /= 10u;
		++digits;
	}
	while (width > digits) {
		console_putc(' ');
		--width;
	}
	console_write_u32(value);
}

static void write_two_digits(uint32_t value)
{
	console_putc((char)('0' + (value / 10u) % 10u));
	console_putc((char)('0' + value % 10u));
}

static void write_directory_date(const struct iomgr_directory_entry *entry)
{
	const struct iomgr_timestamp *time = &entry->info.modified;

	write_two_digits(time->month);
	console_putc('-');
	write_two_digits(time->day);
	console_putc('-');
	write_two_digits(time->year % 100u);
	console_putc(' ');
	write_two_digits(time->hour);
	console_putc(':');
	write_two_digits(time->minute);
}

static void list_entry(const struct iomgr_directory_entry *entry,
		       struct directory_summary *summary)
{
	size_t name_length;
	uint32_t updated;

	/* COMMAND's ordinary DIR search does not request hidden/system files.
	 */
	if ((entry->info.attributes &
	     (IOMGR_NODE_HIDDEN | IOMGR_NODE_SYSTEM |
	      IOMGR_NODE_VOLUME_LABEL)) != 0u)
		return;

	name_length = entry->name_length;
	console_write((const char *)entry->name, name_length);
	while (name_length < 13u) {
		console_putc(' ');
		++name_length;
	}
	console_putc(' ');
	if ((entry->info.attributes & IOMGR_NODE_DIRECTORY) != 0u) {
		console_write_literal("     <DIR> ");
		if (check_add_overflow(summary->directory_count, 1u,
				       &updated)) {
			summary->overflow = true;
		} else {
			summary->directory_count = updated;
		}
	} else {
		if (entry->info.size > 0xffffffffu) {
			console_write_literal("  <TOO BIG> ");
			summary->overflow = true;
		} else {
			write_number_field((uint32_t)entry->info.size, 10u);
			console_putc(' ');
		}
		if (check_add_overflow(summary->file_count, 1u, &updated)) {
			summary->overflow = true;
		} else {
			summary->file_count = updated;
		}
		if (entry->info.size > 0xffffffffu ||
		    check_add_overflow(summary->total_bytes,
				       (uint32_t)entry->info.size,
				       &updated)) {
			summary->overflow = true;
		} else {
			summary->total_bytes = updated;
		}
	}
	write_directory_date(entry);
	console_putc('\n');
}

static enum iomgr_status query_volume_label(
	uint8_t label[IOMGR_NAME_MAX_BYTES + 1u], size_t *label_length)
{
	static const uint8_t label_pattern_bytes[] = "\\*.*";
	const struct iomgr_path label_pattern = {
		.bytes = label_pattern_bytes,
		.length = sizeof(label_pattern_bytes) - 1u,
	};
	struct iomgr_directory_entry entry;
	iomgr_search_handle_t search;
	enum iomgr_status status;

	*label_length = 0u;
	label[0] = 0u;
	status = iomgr_open_search(shell_volume, &label_pattern,
				   IOMGR_NODE_VOLUME_LABEL, &search);
	if (status != IOMGR_OK)
		return status;
	status = iomgr_search_next(search, &entry);
	if (status == IOMGR_OK) {
		if (memcpy_s(label, IOMGR_NAME_MAX_BYTES + 1u, entry.name,
			     sizeof(entry.name), entry.name_length + 1u) !=
		    MEMORY_OK)
			status = IOMGR_CORRUPT;
		else
			*label_length = entry.name_length;
	} else if (status == IOMGR_END_OF_SEARCH) {
		status = IOMGR_OK;
	}
	if (iomgr_close_search(search) != IOMGR_OK && status == IOMGR_OK)
		status = IOMGR_IO_ERROR;
	return status;
}

static enum iomgr_status write_volume_description(void)
{
	uint8_t label[IOMGR_NAME_MAX_BYTES + 1u];
	size_t label_length;
	enum iomgr_status status = query_volume_label(label, &label_length);

	if (status != IOMGR_OK)
		return status;
	if (label_length == 0u) {
		console_write_literal("Drive ");
		write_shell_drive();
		write_ui(DOS_UI_VOLUME_NO_LABEL);
		return IOMGR_OK;
	}
	write_ui(DOS_UI_VOLUME_LABEL);
	write_shell_drive();
	console_write_literal(": ");
	console_write((const char *)label, label_length);
	console_putc('\n');
	return IOMGR_OK;
}

static void command_dir(const struct command_arguments *arguments)
{
	struct directory_summary summary = { 0u, 0u, 0u, false };
	struct iomgr_directory_entry entry;
	struct iomgr_node_info node;
	struct iomgr_space_info space;
	struct iomgr_path requested;
	iomgr_search_handle_t search = IOMGR_SEARCH_HANDLE_INVALID;
	char display_path[SHELL_PATH_CAPACITY];
	char search_path[SHELL_PATH_CAPACITY];
	size_t display_length;
	enum iomgr_status status;

	if (!is_single_argument(arguments->text, arguments->length, true))
		return;
	status = resolve_shell_path(arguments->text, arguments->length,
				    display_path, &requested);
	if (status != IOMGR_OK) {
		syntax_error();
		return;
	}
	status = write_volume_description();
	if (status != IOMGR_OK) {
		write_iomgr_error("DIR volume query", status);
		return;
	}
	write_ui(DOS_UI_DIRECTORY_HEADING);
	write_path(display_path);
	console_write_literal("\n\n");

	display_length = strnlen(display_path, sizeof(display_path));
	if (display_length == sizeof(display_path) ||
	    memcpy_s(search_path, sizeof(search_path), display_path,
		     sizeof(display_path), display_length + 1u) != MEMORY_OK) {
		write_ui(DOS_UI_PATH_TOO_LONG);
		return;
	}
	status = iomgr_stat(shell_volume, &requested, &node);
	if (status == IOMGR_OK &&
	    (node.attributes & IOMGR_NODE_DIRECTORY) != 0u) {
		if (display_length != 3u) {
			if (display_length + 1u >= sizeof(search_path)) {
				write_ui(DOS_UI_PATH_TOO_LONG);
				return;
			}
			search_path[display_length++] = '\\';
		}
		if (display_length + sizeof("*.*") > sizeof(search_path)) {
			write_ui(DOS_UI_PATH_TOO_LONG);
			return;
		}
		search_path[display_length++] = '*';
		search_path[display_length++] = '.';
		search_path[display_length++] = '*';
		search_path[display_length] = '\0';
	} else if (status != IOMGR_OK && status != IOMGR_NOT_FOUND &&
		   status != IOMGR_INVALID_NAME) {
		write_iomgr_error("DIR", status);
		return;
	}
	status = canonical_iomgr_path(search_path, sizeof(search_path),
				      &requested);
	if (status == IOMGR_OK)
		status = iomgr_open_search(shell_volume, &requested,
					   IOMGR_NODE_DIRECTORY, &search);
	if (status != IOMGR_OK) {
		write_iomgr_error("DIR", status);
		return;
	}
	for (;;) {
		status = iomgr_search_next(search, &entry);
		if (status != IOMGR_OK)
			break;
		list_entry(&entry, &summary);
	}
	if (iomgr_close_search(search) != IOMGR_OK) {
		write_iomgr_error("DIR close", IOMGR_IO_ERROR);
		return;
	}
	if (status != IOMGR_END_OF_SEARCH) {
		write_iomgr_error("DIR", status);
		return;
	}
	console_putc('\n');
	write_ui(DOS_UI_TOTAL_FILES);
	console_write_u32(summary.file_count);
	if (summary.overflow) {
		console_write_literal(", ");
		write_ui(DOS_UI_SIZE_OVERFLOW);
	} else {
		write_ui(DOS_UI_TOTAL_BYTES);
		console_write_u32(summary.total_bytes);
		console_putc('\n');
	}

	status = iomgr_query_space(shell_volume, true, &space);
	if (status != IOMGR_OK) {
		write_iomgr_error("DIR free-space query", status);
		return;
	}
	write_ui(DOS_UI_TOTAL_DIRECTORIES);
	console_write_u32(summary.directory_count);
	if (space.free_bytes > 0xffffffffu) {
		console_write_literal(", ");
		write_ui(DOS_UI_FREE_SPACE_OVERFLOW);
	} else {
		write_ui(DOS_UI_FREE_BYTES);
		console_write_u32((uint32_t)space.free_bytes);
		console_putc('\n');
	}
}

static void command_type(const struct command_arguments *arguments)
{
	uint8_t buffer[SHELL_FILE_BUFFER_SIZE];
	char canonical[DOS_PATH_CAPACITY];
	struct iomgr_path requested;
	struct iomgr_node_info info;
	iomgr_file_handle_t file;
	uint64_t offset = 0u;
	enum iomgr_status status;
	size_t bytes_read;
	size_t printable;
	bool done = false;

	if (!is_single_argument(arguments->text, arguments->length, false))
		return;
	status = resolve_shell_path(arguments->text, arguments->length,
				    canonical, &requested);
	if (status == IOMGR_OK)
		status = iomgr_open_file(shell_volume, &requested, &info, &file);
	if (status != IOMGR_OK) {
		write_iomgr_error("TYPE", status);
		return;
	}
	while (!done) {
		status = iomgr_read_file(file, offset, buffer, sizeof(buffer),
					 sizeof(buffer), &bytes_read);
		if (status != IOMGR_OK) {
			write_iomgr_error("TYPE", status);
			break;
		}
		if (bytes_read == 0u)
			break;
		offset += bytes_read;
		printable = 0u;
		while (printable < bytes_read &&
		       buffer[printable] != DOS_TEXT_EOF)
			++printable;
		if (printable != 0u)
			console_write((const char *)buffer, printable);
		if (printable != bytes_read)
			done = true;
	}
	if (iomgr_close_file(file) != IOMGR_OK)
		write_iomgr_error("TYPE close", IOMGR_IO_ERROR);
}

static void command_cd(const struct command_arguments *arguments)
{
	char new_path[SHELL_PATH_CAPACITY];
	struct iomgr_path requested;
	struct iomgr_node_info info;
	enum iomgr_status status;

	if (!is_single_argument(arguments->text, arguments->length, true))
		return;
	if (arguments->length == 0u ||
	    (arguments->length == 2u &&
	     ascii_toupper(arguments->text[0]) ==
		     (char)('A' + shell_drive_index) &&
	     arguments->text[1] == ':')) {
		write_path(current_path);
		console_putc('\n');
		return;
	}
	status = resolve_shell_path(arguments->text, arguments->length, new_path,
				    &requested);
	if (status != IOMGR_OK) {
		write_ui(DOS_UI_INVALID_DRIVE_OR_PATH);
		return;
	}
	status = iomgr_stat(shell_volume, &requested, &info);
	if (status == IOMGR_OK &&
	    (info.attributes & IOMGR_NODE_DIRECTORY) == 0u)
		status = IOMGR_NOT_DIRECTORY;
	if (status != IOMGR_OK) {
		write_iomgr_error("CHDIR", status);
		return;
	}
	if (directory_commit != NULL &&
	    !directory_commit(new_path, sizeof(new_path))) {
		write_ui(DOS_UI_INVALID_DRIVE_OR_PATH);
		return;
	}
	if (strscpy_s(current_path, sizeof(current_path), new_path,
		      sizeof(new_path)) == STRSCPY_TRUNCATED) {
		write_ui(DOS_UI_PATH_TOO_LONG);
		return;
	}
}

static void command_echo(const struct command_arguments *arguments)
{
	if (arguments->length == 0u) {
		write_ui(DOS_UI_ECHO_STATUS);
		if (echo_commands)
			write_ui(DOS_UI_ENABLED);
		else
			write_ui(DOS_UI_DISABLED);
		return;
	}
	if (text_equal_ascii_case(arguments->text, arguments->length, "ON")) {
		echo_commands = true;
		return;
	}
	if (text_equal_ascii_case(arguments->text, arguments->length, "OFF")) {
		echo_commands = false;
		return;
	}
	console_write(arguments->text, arguments->length);
	console_putc('\n');
}

static void command_rem(const struct command_arguments *arguments)
{
	(void)arguments;
}

static void command_cls(const struct command_arguments *arguments)
{
	if (has_no_arguments(arguments->length))
		console_clear();
}

static void command_ver(const struct command_arguments *arguments)
{
	if (!has_no_arguments(arguments->length))
		return;
	write_ui(DOS_UI_VERSION);
}

static void command_vol(const struct command_arguments *arguments)
{
	enum iomgr_status status;

	if (!is_single_argument(arguments->text, arguments->length, true))
		return;
	if (arguments->length != 0u &&
	    !(arguments->length == 2u &&
	      ascii_toupper(arguments->text[0]) ==
		      (char)('A' + shell_drive_index) &&
	      arguments->text[1] == ':')) {
		write_ui(DOS_UI_INVALID_DRIVE);
		write_shell_drive();
		console_write_literal(":.\n");
		return;
	}
	status = write_volume_description();
	if (status != IOMGR_OK) {
		write_iomgr_error("VOL", status);
		return;
	}
}

static void command_exit(const struct command_arguments *arguments)
{
	if (has_no_arguments(arguments->length))
		exit_requested = true;
}

bool shell_init(iomgr_volume_handle_t volume, uint8_t drive_index)
{
	size_t root_length;

	if (volume == IOMGR_VOLUME_HANDLE_INVALID ||
	    dos_drive_format_root(drive_index, current_path,
				  sizeof(current_path), &root_length) != DOS_DRIVE_OK)
		return false;
	(void)root_length;
	shell_volume = volume;
	shell_drive_index = drive_index;
	echo_commands = true;
	exit_requested = false;
	return true;
}

void shell_set_external_runner(shell_external_runner_t runner)
{
	external_runner = runner;
}

bool shell_set_directory_commit(shell_directory_commit_t commit)
{
	directory_commit = commit;
	return commit == NULL || commit(current_path, sizeof(current_path));
}

void shell_execute_line(const char *line, size_t line_capacity)
{
	char command_line[SHELL_COMMAND_CAPACITY];
	char *cursor;
	char *command;
	struct command_arguments arguments;
	char *end;
	size_t probe_capacity;
	size_t length;
	size_t command_length;
	size_t index;

	if (line == NULL || line_capacity == 0u)
		return;
	probe_capacity = line_capacity < sizeof(command_line)
				 ? line_capacity
				 : sizeof(command_line);
	length = strnlen(line, probe_capacity);
	if (length == probe_capacity) {
		write_ui(DOS_UI_COMMAND_LINE_TOO_LONG);
		return;
	}
	if (strscpy_s(command_line, sizeof(command_line), line, length + 1u) ==
	    STRSCPY_TRUNCATED) {
		write_ui(DOS_UI_COMMAND_LINE_TOO_LONG);
		return;
	}

	cursor = command_line;
	end = command_line + length;
	while (end > cursor && ascii_isspace(end[-1]))
		--end;
	*end = '\0';
	while (cursor < end && ascii_isspace(*cursor))
		++cursor;
	if (cursor < end && *cursor == '@') {
		++cursor;
		while (cursor < end && ascii_isspace(*cursor))
			++cursor;
	}
	if (cursor == end)
		return;

	command = cursor;
	while (cursor < end && !ascii_isspace(*cursor))
		++cursor;
	command_length = (size_t)(cursor - command);
	if (cursor == end) {
		arguments.text = cursor;
	} else {
		*cursor++ = '\0';
		while (cursor < end && ascii_isspace(*cursor))
			++cursor;
		arguments.text = cursor;
	}
	arguments.capacity =
		sizeof(command_line) - (size_t)(arguments.text - command_line);
	arguments.length = (size_t)(end - arguments.text);

	for (index = 0u; index < ARRAY_SIZE(command_table); ++index) {
		if (text_equal_ascii_case(command, command_length,
					  command_table[index].name)) {
			command_table[index].handler(&arguments);
			return;
		}
	}
	if (run_external_command(command, command_length, &arguments))
		return;
	write_ui(DOS_UI_COMMAND_NOT_FOUND);
	console_write(command, command_length);
	console_putc('\n');
}

static bool batch_line_is_suppressed(const char *line, size_t line_length)
{
	size_t index = 0u;

	while (index < line_length && ascii_isspace(line[index]))
		++index;
	return index < line_length && line[index] == '@';
}

static void execute_batch_line(char line[SHELL_COMMAND_CAPACITY],
			       size_t *length, bool *overflow)
{
	if (*overflow) {
		write_ui(DOS_UI_AUTOEXEC_LINE_TOO_LONG);
	} else if (*length != 0u) {
		line[*length] = '\0';
		if (echo_commands && !batch_line_is_suppressed(line, *length)) {
			write_path(current_path);
			console_putc('>');
			console_write(line, *length);
			console_putc('\n');
		}
		shell_execute_line(line, SHELL_COMMAND_CAPACITY);
	}
	*length = 0u;
	*overflow = false;
}

void shell_run_autoexec(void)
{
	static const uint8_t autoexec_path_bytes[] = "\\AUTOEXEC.BAT";
	const struct iomgr_path autoexec_path = {
		.bytes = autoexec_path_bytes,
		.length = sizeof(autoexec_path_bytes) - 1u,
	};
	uint8_t input[SHELL_FILE_BUFFER_SIZE];
	char line[SHELL_COMMAND_CAPACITY];
	struct iomgr_node_info info;
	iomgr_file_handle_t file;
	uint64_t offset = 0u;
	enum iomgr_status status;
	size_t bytes_read;
	size_t index;
	size_t length = 0u;
	bool line_overflow = false;
	bool previous_was_carriage_return = false;
	bool read_failed = false;
	bool stop = false;

	status = iomgr_open_file(shell_volume, &autoexec_path, &info, &file);
	if (status == IOMGR_NOT_FOUND)
		return;
	if (status != IOMGR_OK) {
		write_iomgr_error("AUTOEXEC.BAT", status);
		return;
	}

	while (!stop) {
		status = iomgr_read_file(file, offset, input, sizeof(input),
					 sizeof(input), &bytes_read);
		if (status != IOMGR_OK) {
			write_iomgr_error("AUTOEXEC.BAT", status);
			read_failed = true;
			break;
		}
		if (bytes_read == 0u)
			break;
		offset += bytes_read;
		for (index = 0u; index < bytes_read; ++index) {
			if (input[index] == DOS_TEXT_EOF) {
				execute_batch_line(line, &length,
						   &line_overflow);
				stop = true;
				break;
			}
			if (input[index] == '\r') {
				execute_batch_line(line, &length,
						   &line_overflow);
				previous_was_carriage_return = true;
				if (exit_requested) {
					stop = true;
					break;
				}
				continue;
			}
			if (input[index] == '\n') {
				if (!previous_was_carriage_return)
					execute_batch_line(line, &length,
							   &line_overflow);
				previous_was_carriage_return = false;
				if (exit_requested) {
					stop = true;
					break;
				}
				continue;
			}
			previous_was_carriage_return = false;
			if (length + 1u >= sizeof(line)) {
				line_overflow = true;
				continue;
			}
			line[length++] = (char)input[index];
		}
	}
	if (!stop && !read_failed && (length != 0u || line_overflow))
		execute_batch_line(line, &length, &line_overflow);
	if (iomgr_close_file(file) != IOMGR_OK)
		write_iomgr_error("AUTOEXEC.BAT close", IOMGR_IO_ERROR);
}

void shell_run(void)
{
	char line[SHELL_COMMAND_CAPACITY];

	while (!exit_requested) {
		write_path(current_path);
		console_putc('>');
		(void)keyboard_readline(line, sizeof(line));
		shell_execute_line(line, sizeof(line));
	}
}

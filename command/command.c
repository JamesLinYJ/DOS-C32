// SPDX-License-Identifier: GPL-2.0-only
/*
 * DOS-C32 COMMAND: native GNU11 Ring-3 command interpreter
 *
 * Compatibility contract: current-directory/PATH search and COMMAND built-in
 * behavior follow MS-DOS semantics. Counted PATH/environment spans and
 * retained probe ownership keep resolution bounded and transactional.
 */
#include "c32_syscall.h"
#include "directory.h"
#include "path.h"
#include "shell_external_command.h"
#include "string.h"
#include "types.h"

#define COMMAND_INPUT_MAX 126u
#define COMMAND_NAME_MAX 120u
#define COMMAND_BATCH_NESTING_MAX 8u
#define COMMAND_ENVIRONMENT_VALUE_CAPACITY 32768u
#define COMMAND_TEXT_EOF 0x1au

struct command_external_probe {
	uint32_t file;
	bool has_file;
};

static char input[COMMAND_INPUT_MAX + 1u];
static uint8_t command_tail[COMMAND_INPUT_MAX];
static char filename[COMMAND_NAME_MAX + 5u];
/* Transient ENV_GET scratch; the current PSP environment remains the owner. */
static uint8_t environment_value[COMMAND_ENVIRONMENT_VALUE_CAPACITY];
static uint32_t batch_depth;
static bool echo_commands = true;

static const char path_environment_name[] = "PATH";

static uint32_t c32_call3(uint32_t number, uint32_t first, uint32_t second,
			  uint32_t third)
{
	register uint32_t eax __asm__("eax") = number;
	register uint32_t ebx __asm__("ebx") = first;
	register uint32_t ecx __asm__("ecx") = second;
	register uint32_t edx __asm__("edx") = third;

	__asm__ volatile("int $0x30"
			 : "+a"(eax), "+b"(ebx), "+c"(ecx), "+d"(edx)
			 :
			 : "cc", "memory");
	return eax;
}

static uint32_t c32_exec(const char *path, size_t path_length,
			 const uint8_t *tail, size_t tail_length)
{
	register uint32_t eax __asm__("eax") = C32_SYSCALL_DOS_EXEC;
	register uint32_t ebx __asm__("ebx") = (uint32_t)(uintptr_t)path;
	register uint32_t ecx __asm__("ecx") = (uint32_t)path_length;
	register uint32_t edx __asm__("edx") = (uint32_t)(uintptr_t)tail;
	register uint32_t esi __asm__("esi") = (uint32_t)tail_length;

	__asm__ volatile("int $0x30"
			 : "+a"(eax), "+b"(ebx), "+c"(ecx), "+d"(edx),
			   "+S"(esi)
			 :
			 : "cc", "memory");
	return eax;
}

static uint32_t c32_environment_get(const char *name, size_t name_length,
				    uint8_t *value, size_t value_capacity)
{
	register uint32_t eax __asm__("eax") = C32_SYSCALL_DOS_ENV_GET;
	register uint32_t ebx __asm__("ebx") = (uint32_t)(uintptr_t)name;
	register uint32_t ecx __asm__("ecx") = (uint32_t)name_length;
	register uint32_t edx __asm__("edx") = (uint32_t)(uintptr_t)value;
	register uint32_t esi __asm__("esi") = (uint32_t)value_capacity;

	__asm__ volatile("int $0x30"
			 : "+a"(eax), "+b"(ebx), "+c"(ecx), "+d"(edx),
			   "+S"(esi)
			 :
			 : "cc", "memory");
	return eax;
}

static bool c32_environment_set(const char *name, size_t name_length,
				const uint8_t *value, size_t value_length)
{
	register uint32_t eax __asm__("eax") = C32_SYSCALL_DOS_ENV_SET;
	register uint32_t ebx __asm__("ebx") = (uint32_t)(uintptr_t)name;
	register uint32_t ecx __asm__("ecx") = (uint32_t)name_length;
	register uint32_t edx __asm__("edx") = (uint32_t)(uintptr_t)value;
	register uint32_t esi __asm__("esi") = (uint32_t)value_length;

	__asm__ volatile("int $0x30"
			 : "+a"(eax), "+b"(ebx), "+c"(ecx), "+d"(edx),
			   "+S"(esi)
			 :
			 : "cc", "memory");
	return eax == 0u;
}

static uint32_t c32_file_open(const char *path, size_t path_length)
{
	return c32_call3(C32_SYSCALL_DOS_FILE_OPEN,
			 (uint32_t)(uintptr_t)path, (uint32_t)path_length, 0u);
}

static uint32_t c32_file_read(uint32_t file, uint8_t *buffer,
			      size_t capacity)
{
	return c32_call3(C32_SYSCALL_DOS_FILE_READ, file,
			 (uint32_t)(uintptr_t)buffer, (uint32_t)capacity);
}

static bool c32_file_close(uint32_t file)
{
	return c32_call3(C32_SYSCALL_DOS_FILE_CLOSE, file, 0u, 0u) == 0u;
}

static size_t literal_length(const char *text)
{
	size_t length = 0u;

	while (text[length] != '\0')
		++length;
	return length;
}

static void console_write(const char *text)
{
	(void)c32_call3(C32_SYSCALL_CONSOLE_WRITE,
			(uint32_t)(uintptr_t)text, (uint32_t)literal_length(text),
			0u);
}

static void console_write_span(const uint8_t *text, size_t length)
{
	if (length == 0u)
		return;
	(void)c32_call3(C32_SYSCALL_CONSOLE_WRITE,
			(uint32_t)(uintptr_t)text, (uint32_t)length, 0u);
}

static size_t console_read_line(void)
{
	uint32_t result = c32_call3(
		C32_SYSCALL_CONSOLE_READ_LINE, (uint32_t)(uintptr_t)input,
		(uint32_t)sizeof(input), 0u);

	return result == C32_SYSCALL_ERROR ? 0u : (size_t)result;
}

static void clear_screen(void)
{
	(void)c32_call3(C32_SYSCALL_CONSOLE_CLEAR, 0u, 0u, 0u);
}

static bool change_directory(const uint8_t *path, size_t path_length)
{
	return c32_call3(C32_SYSCALL_DOS_CHDIR,
			 (uint32_t)(uintptr_t)path, (uint32_t)path_length, 0u) ==
	       0u;
}

static bool get_current_directory(char *path, size_t capacity)
{
	return c32_call3(C32_SYSCALL_DOS_GETCWD,
			 (uint32_t)(uintptr_t)path, (uint32_t)capacity, 0u) !=
	       C32_SYSCALL_ERROR;
}

static _Noreturn void process_exit(uint32_t code)
{
	(void)c32_call3(C32_SYSCALL_PROCESS_EXIT, code, 0u, 0u);
	for (;;)
		__asm__ volatile("ud2");
}

static bool string_equal(const char *left, const char *right)
{
	size_t index = 0u;

	while (left[index] == right[index]) {
		if (left[index] == '\0')
			return true;
		++index;
	}
	return false;
}

static bool span_equal(const uint8_t *text, size_t length,
		       const char *literal)
{
	size_t index = 0u;

	while (literal[index] != '\0') {
		if (index >= length ||
		    ascii_toupper((char)text[index]) != literal[index])
			return false;
		++index;
	}
	return index == length;
}

static size_t parse_command(const char *line, size_t input_length,
			    size_t *tail_length, bool *name_overflow)
{
	size_t source = 0u;
	size_t name_length = 0u;
	size_t tail = 0u;
	bool overflow = false;

	while (source < input_length && ascii_isspace(line[source]))
		++source;
	if (source < input_length && line[source] == '@') {
		++source;
		while (source < input_length && ascii_isspace(line[source]))
			++source;
	}
	while (source < input_length && !ascii_isspace(line[source])) {
		if (name_length < COMMAND_NAME_MAX)
			filename[name_length++] = ascii_toupper(line[source]);
		else
			overflow = true;
		++source;
	}
	filename[name_length] = '\0';
	while (source < input_length && ascii_isspace(line[source]))
		++source;
	while (source < input_length && tail < sizeof(command_tail))
		command_tail[tail++] = (uint8_t)line[source++];
	*tail_length = tail;
	*name_overflow = overflow;
	return name_length;
}

static void write_prompt(void)
{
	char path[DOS_PATH_CAPACITY];

	if (get_current_directory(path, sizeof(path)))
		console_write(path);
	else
		console_write("?:\\");
	console_write(">");
}

static void execute_line(const char *line, size_t line_length);

static bool line_is_suppressed(const char *line, size_t length)
{
	size_t index = 0u;

	while (index < length && ascii_isspace(line[index]))
		++index;
	return index < length && line[index] == '@';
}

static void execute_batch_line(char *line, size_t *length, bool *overflow)
{
	if (*overflow) {
		console_write("Batch line too long\r\n");
	} else if (*length != 0u) {
		if (echo_commands && !line_is_suppressed(line, *length)) {
			write_prompt();
			(void)c32_call3(C32_SYSCALL_CONSOLE_WRITE,
					(uint32_t)(uintptr_t)line,
					(uint32_t)*length, 0u);
			console_write("\r\n");
		}
		execute_line(line, *length);
	}
	*length = 0u;
	*overflow = false;
}

static void execute_batch(uint32_t file)
{
	uint8_t input_buffer[128];
	char line[COMMAND_INPUT_MAX + 1u];
	size_t length = 0u;
	bool overflow = false;
	bool previous_was_carriage_return = false;
	bool stop = false;

	if (batch_depth >= COMMAND_BATCH_NESTING_MAX) {
		console_write("Batch nesting too deep\r\n");
		if (!c32_file_close(file))
			console_write("Batch file close failed\r\n");
		return;
	}
	++batch_depth;
	while (!stop) {
		uint32_t bytes_read =
			c32_file_read(file, input_buffer, sizeof(input_buffer));
		size_t index;

		if (bytes_read == C32_SYSCALL_ERROR) {
			console_write("Batch file read failed\r\n");
			break;
		}
		if (bytes_read == 0u)
			break;
		for (index = 0u; index < (size_t)bytes_read; ++index) {
			uint8_t character = input_buffer[index];

			if (character == COMMAND_TEXT_EOF) {
				execute_batch_line(line, &length, &overflow);
				stop = true;
				break;
			}
			if (character == '\r') {
				execute_batch_line(line, &length, &overflow);
				previous_was_carriage_return = true;
				continue;
			}
			if (character == '\n') {
				if (!previous_was_carriage_return)
					execute_batch_line(line, &length,
							   &overflow);
				previous_was_carriage_return = false;
				continue;
			}
			previous_was_carriage_return = false;
			if (length >= COMMAND_INPUT_MAX) {
				overflow = true;
				continue;
			}
			line[length++] = (char)character;
		}
	}
	if (!stop && (length != 0u || overflow))
		execute_batch_line(line, &length, &overflow);
	if (!c32_file_close(file))
		console_write("Batch file close failed\r\n");
	--batch_depth;
}

static enum shell_external_probe_status command_external_probe(
	const char *absolute_path, size_t path_length, void *context)
{
	struct command_external_probe *probe = context;
	uint32_t file;

	if (probe == NULL || probe->has_file)
		return SHELL_EXTERNAL_PROBE_ERROR;
	file = c32_file_open(absolute_path, path_length);
	if (file == C32_SYSCALL_NOT_FOUND)
		return SHELL_EXTERNAL_PROBE_NOT_FOUND;
	if (file == C32_SYSCALL_ERROR)
		return SHELL_EXTERNAL_PROBE_ERROR;
	/* Keep the successful lookup open.  BAT consumes this exact handle;
	 * COM/EXE closes it before the path-only EXEC syscall. */
	probe->file = file;
	probe->has_file = true;
	return SHELL_EXTERNAL_PROBE_FOUND;
}

static bool close_external_probe(struct command_external_probe *probe)
{
	if (probe == NULL || !probe->has_file)
		return false;
	if (!c32_file_close(probe->file))
		return false;
	probe->has_file = false;
	return true;
}

static bool command_search_path(size_t *path_length)
{
	uint32_t result = c32_environment_get(
		path_environment_name, sizeof(path_environment_name) - 1u,
		environment_value, sizeof(environment_value));

	if (path_length == NULL)
		return false;
	if (result == C32_SYSCALL_NOT_FOUND) {
		*path_length = 0u;
		return true;
	}
	if (result == C32_SYSCALL_ERROR ||
	    result == C32_SYSCALL_BUFFER_TOO_SMALL ||
	    result >= sizeof(environment_value))
		return false;
	*path_length = (size_t)result;
	return true;
}

static void execute_external(size_t name_length, size_t tail_length)
{
	char current_path[DOS_PATH_CAPACITY];
	struct command_external_probe probe = {
		.file = C32_SYSCALL_ERROR,
		.has_file = false,
	};
	struct shell_external_request request;
	struct shell_external_result result;
	enum shell_external_status status;
	size_t search_path_length;

	if (!get_current_directory(current_path, sizeof(current_path))) {
		console_write("Program search failed\r\n");
		return;
	}
	if (dos_path_is_explicit(filename, name_length)) {
		search_path_length = 0u;
	} else if (!command_search_path(&search_path_length)) {
		console_write("Program search failed\r\n");
		return;
	}
	request = (struct shell_external_request){
		.command = filename,
		.command_length = name_length,
		.current_path = current_path,
		.current_path_capacity = sizeof(current_path),
		.search_path = (const char *)environment_value,
		.search_path_length = search_path_length,
		.probe = command_external_probe,
		.probe_context = &probe,
	};
	status = shell_external_resolve(&request, &result);
	if (status != SHELL_EXTERNAL_FOUND) {
		if (probe.has_file)
			(void)close_external_probe(&probe);
		if (status == SHELL_EXTERNAL_PATH_ERROR)
			console_write(
				"The system cannot find the path specified.\r\n");
		else if (status == SHELL_EXTERNAL_STORAGE_ERROR)
			console_write("Program search failed\r\n");
		else {
			console_write("Command not found: ");
			console_write_span((const uint8_t *)filename, name_length);
			console_write("\r\n");
		}
		return;
	}
	if (!probe.has_file) {
		console_write("Program search failed\r\n");
		return;
	}
	if (result.type == SHELL_EXTERNAL_BAT) {
		uint32_t file = probe.file;

		probe.has_file = false;
		execute_batch(file);
		return;
	}
	if (!close_external_probe(&probe)) {
		console_write("Program search failed\r\n");
		return;
	}
	if (c32_exec(result.absolute_path, result.path_length, command_tail,
		     tail_length) != 0u)
		console_write("Program load or execution failed\r\n");
}

static void execute_path_command(size_t arguments_length)
{
	uint8_t updated_path[COMMAND_PATH_UPDATE_CAPACITY];
	size_t updated_length = 0u;
	enum command_path_parse_status status;

	status = command_path_parse(command_tail, arguments_length, updated_path,
				    sizeof(updated_path), &updated_length);
	if (status == COMMAND_PATH_PARSE_DISPLAY) {
		size_t path_length;

		if (!command_search_path(&path_length)) {
			console_write("Unable to read the DOS environment\r\n");
			return;
		}

		console_write("PATH=");
		console_write_span(environment_value, path_length);
		console_write("\r\n");
		return;
	}
	if (status == COMMAND_PATH_PARSE_TOO_MANY_PARAMETERS) {
		console_write("Too many parameters\r\n");
		return;
	}
	if (status != COMMAND_PATH_PARSE_UPDATE) {
		console_write("Invalid PATH value\r\n");
		return;
	}
	if (!c32_environment_set(path_environment_name,
				 sizeof(path_environment_name) - 1u,
				 updated_path, updated_length))
		console_write("Unable to update the DOS environment\r\n");
}

static bool display_current_directory(uint8_t drive)
{
	char path[DOS_PATH_CAPACITY];

	if (!get_current_directory(path, sizeof(path))) {
		console_write("Unable to query the current directory\r\n");
		return false;
	}
	if (drive != 0u && (uint8_t)ascii_toupper(path[0]) != drive) {
		console_write("Invalid drive specification\r\n");
		return false;
	}
	console_write(path);
	console_write("\r\n");
	return true;
}

static void execute_directory_command(size_t arguments_length)
{
	struct command_directory_plan plan;
	enum command_directory_parse_status status;

	status = command_directory_parse(command_tail, arguments_length, &plan);
	if (status == COMMAND_DIRECTORY_PARSE_TOO_MANY_PARAMETERS) {
		console_write("Too many parameters\r\n");
		return;
	}
	if (status != COMMAND_DIRECTORY_PARSE_OK) {
		console_write("The system cannot find the path specified.\r\n");
		return;
	}
	if (plan.action == COMMAND_DIRECTORY_QUERY_CURRENT) {
		(void)display_current_directory(0u);
		return;
	}
	if (plan.action == COMMAND_DIRECTORY_QUERY_DRIVE) {
		(void)display_current_directory(plan.drive);
		return;
	}
	if (!change_directory(plan.path, plan.path_length))
		console_write("The system cannot find the path specified.\r\n");
}

static void execute_drive_switch(uint8_t drive, size_t arguments_length)
{
	char path[DOS_PATH_CAPACITY];
	uint8_t root[3];

	if (arguments_length != 0u) {
		console_write("Too many parameters\r\n");
		return;
	}
	if (!get_current_directory(path, sizeof(path))) {
		console_write("Invalid drive specification\r\n");
		return;
	}
	if ((uint8_t)ascii_toupper(path[0]) == drive)
		return;
	root[0] = drive;
	root[1] = (uint8_t)':';
	root[2] = (uint8_t)'\\';
	if (!change_directory(root, sizeof(root)))
		console_write("Invalid drive specification\r\n");
}

static void execute_line(const char *line, size_t line_length)
{
	size_t name_length;
	size_t tail_length;
	uint8_t drive = 0u;
	bool drive_switch;
	bool name_overflow;

	name_length = parse_command(line, line_length, &tail_length,
				    &name_overflow);
	if (name_length == 0u)
		return;
	if (name_overflow) {
		console_write("Command name is too long\r\n");
		return;
	}
	drive_switch =
		command_drive_switch_parse(filename, name_length, &drive);
	if (string_equal(filename, "VER")) {
		console_write("DOS-C32 Version 6.23\r\n");
	} else if (string_equal(filename, "CLS")) {
		clear_screen();
	} else if (string_equal(filename, "CD") ||
		   string_equal(filename, "CHDIR")) {
		execute_directory_command(tail_length);
	} else if (drive_switch) {
		execute_drive_switch(drive, tail_length);
	} else if (string_equal(filename, "ECHO")) {
		if (span_equal(command_tail, tail_length, "OFF"))
			echo_commands = false;
		else if (span_equal(command_tail, tail_length, "ON"))
			echo_commands = true;
		else {
			(void)c32_call3(C32_SYSCALL_CONSOLE_WRITE,
					(uint32_t)(uintptr_t)command_tail,
					(uint32_t)tail_length, 0u);
			console_write("\r\n");
		}
	} else if (string_equal(filename, "PATH")) {
		execute_path_command(tail_length);
	} else if (string_equal(filename, "REM")) {
		return;
	} else if (string_equal(filename, "HELP")) {
		console_write(
			"Built-ins: CD CHDIR CLS ECHO PATH REM VER HELP EXIT\r\n");
		console_write(
			"COM and EXE programs run in a protected compatibility environment.\r\n");
		console_write("BAT files are interpreted by protected COMMAND.\r\n");
	} else if (string_equal(filename, "EXIT")) {
		process_exit(0u);
	} else {
		execute_external(name_length, tail_length);
	}
}

_Noreturn void command_main(void);

_Noreturn __attribute__((section(".text.entry"))) void command_main(void)
{
	console_write("DOS-C32 COMMAND.COM (32-bit protected mode)\r\n");
	for (;;) {
		size_t input_length;

		write_prompt();
		input_length = console_read_line();
		execute_line(input, input_length);
	}
}

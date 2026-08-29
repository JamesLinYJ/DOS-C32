// SPDX-License-Identifier: GPL-2.0-only
/* Freestanding tests for capacity-carrying COMMAND input and IOMGR paths. */
#include "console.h"
#include "dos_drive.h"
#include "test_entry.h"
#include "dos_ui.h"
#include "iomgr.h"
#include "keyboard.h"
#include "shell.h"

#define CONSOLE_CAPTURE_CAPACITY 512u
#define TEST_VOLUME ((iomgr_volume_handle_t)0x100000001ull)

static uint32_t console_write_calls;
static uint32_t console_clear_calls;
static uint32_t keyboard_readline_calls;
static uint32_t iomgr_open_calls;
static uint32_t iomgr_other_calls;
static size_t last_path_length;
static char last_path[16];
static char console_capture[CONSOLE_CAPTURE_CAPACITY];
static size_t console_capture_length;
static bool autoexec_read_failure_test;

static bool text_equals(const char *left, const char *right, size_t count);

void console_init(void)
{
}

void console_clear(void)
{
	++console_clear_calls;
}

void console_putc(char character)
{
	++console_write_calls;
	if (console_capture_length < sizeof(console_capture))
		console_capture[console_capture_length++] = character;
}

void console_write(const char *text, size_t count)
{
	size_t available;
	size_t index;

	++console_write_calls;
	if (text == NULL)
		return;
	available = sizeof(console_capture) - console_capture_length;
	if (count > available)
		count = available;
	for (index = 0u; index < count; ++index)
		console_capture[console_capture_length++] = text[index];
}

void console_write_u32(uint32_t value)
{
	(void)value;
	++console_write_calls;
}

void console_write_hex(uint32_t value)
{
	(void)value;
	++console_write_calls;
}

void console_backspace(void)
{
	++console_write_calls;
}

void keyboard_init(void)
{
}

char keyboard_getchar(void)
{
	return '\0';
}

size_t keyboard_readline(char *buffer, size_t capacity)
{
	static const char exit_line[] = "EXIT";
	size_t index;

	++keyboard_readline_calls;
	if (buffer == NULL || capacity < sizeof(exit_line))
		return 0u;
	for (index = 0u; index < sizeof(exit_line); ++index)
		buffer[index] = exit_line[index];
	return sizeof(exit_line) - 1u;
}

enum iomgr_status iomgr_open_file(iomgr_volume_handle_t volume,
				  const struct iomgr_path *path,
				  struct iomgr_node_info *info,
				  iomgr_file_handle_t *file)
{
	size_t index = 0u;

	(void)info;
	if (volume != TEST_VOLUME || path == NULL)
		return IOMGR_INVALID_ARGUMENT;
	++iomgr_open_calls;
	last_path_length = path->length;
	while (index + 1u < sizeof(last_path) && index < path->length) {
		last_path[index] = (char)path->bytes[index];
		++index;
	}
	last_path[index] = '\0';
	if (autoexec_read_failure_test &&
	    path->length == sizeof("\\AUTOEXEC.BAT") - 1u &&
	    text_equals((const char *)path->bytes, "\\AUTOEXEC.BAT",
			path->length)) {
		*file = 1u;
		return IOMGR_OK;
	}
	return IOMGR_NOT_FOUND;
}

enum iomgr_status iomgr_stat(iomgr_volume_handle_t volume,
			     const struct iomgr_path *path,
			     struct iomgr_node_info *info)
{
	(void)volume;
	(void)path;
	(void)info;
	++iomgr_other_calls;
	return IOMGR_NOT_FOUND;
}

enum iomgr_status iomgr_read_file(iomgr_file_handle_t file, uint64_t offset,
				  uint8_t *destination, size_t capacity,
				  size_t count, size_t *bytes_read)
{
	(void)file;
	(void)offset;
	++iomgr_other_calls;
	if (autoexec_read_failure_test && offset == 0u) {
		size_t index;

		if (destination == NULL || bytes_read == NULL || capacity < 127u ||
		    count < 127u)
			return IOMGR_INVALID_ARGUMENT;
		for (index = 0u; index < 127u; ++index)
			destination[index] = 'A';
		*bytes_read = 127u;
		return IOMGR_OK;
	}
	if (autoexec_read_failure_test)
		return IOMGR_CORRUPT;
	return IOMGR_IO_ERROR;
}

enum iomgr_status iomgr_close_file(iomgr_file_handle_t file)
{
	(void)file;
	++iomgr_other_calls;
	return IOMGR_OK;
}

enum iomgr_status iomgr_open_search(iomgr_volume_handle_t volume,
				    const struct iomgr_path *pattern,
				    uint32_t attributes,
				    iomgr_search_handle_t *search)
{
	(void)volume;
	(void)pattern;
	(void)attributes;
	(void)search;
	++iomgr_other_calls;
	return IOMGR_NOT_FOUND;
}

enum iomgr_status iomgr_search_next(iomgr_search_handle_t search,
				    struct iomgr_directory_entry *entry)
{
	(void)search;
	(void)entry;
	++iomgr_other_calls;
	return IOMGR_END_OF_SEARCH;
}

enum iomgr_status iomgr_close_search(iomgr_search_handle_t search)
{
	(void)search;
	++iomgr_other_calls;
	return IOMGR_OK;
}

enum iomgr_status iomgr_query_space(iomgr_volume_handle_t volume,
				    bool count_free,
				    struct iomgr_space_info *info)
{
	(void)volume;
	(void)count_free;
	(void)info;
	++iomgr_other_calls;
	return IOMGR_IO_ERROR;
}

static bool text_equals(const char *left, const char *right, size_t count)
{
	size_t index;

	for (index = 0u; index < count; ++index) {
		if (left[index] != right[index])
			return false;
	}
	return true;
}

static void reset_console_capture(void)
{
	console_capture_length = 0u;
}

static bool console_equals(const char *expected, size_t expected_size)
{
	return expected_size != 0u &&
	       console_capture_length == expected_size - 1u &&
	       text_equals(console_capture, expected, expected_size - 1u);
}

static int run_tests(void)
{
	static const char type_line[] = "TYPE FILE.TXT";
	static const char type_path[] = "\\FILE.TXT";
	static const char unknown_line[] = "FROBNICATE";
	static const char unknown_message[] = "Command not found: FROBNICATE\n";
	static const char bad_ver_line[] = "VER EXTRA";
	static const char syntax_message[] =
		"Syntax error: check the command and its arguments.\n";
	static const char autoexec_corrupt_message[] =
		"AUTOEXEC.BAT failed: filesystem metadata is corrupt.\n";
	char one_byte_line[1] = { 'E' };
	char two_byte_line[2] = { 'E', 'X' };
	char unterminated_exit[4] = { 'E', 'X', 'I', 'T' };
	uint32_t saved_console_calls;

	if (!shell_init(TEST_VOLUME, DOS_FIRST_FIXED_DRIVE_INDEX))
		return 1;
	saved_console_calls = console_write_calls;
	shell_execute_line(NULL, 1u);
	shell_execute_line(type_line, 0u);
	if (console_write_calls != saved_console_calls ||
	    iomgr_open_calls != 0u || iomgr_other_calls != 0u ||
	    keyboard_readline_calls != 0u)
		return 1;

	shell_execute_line(one_byte_line, sizeof(one_byte_line));
	shell_execute_line(two_byte_line, sizeof(two_byte_line));
	shell_execute_line(unterminated_exit, sizeof(unterminated_exit));
	if (iomgr_open_calls != 0u || iomgr_other_calls != 0u ||
	    console_clear_calls != 0u || keyboard_readline_calls != 0u)
		return 2;

	/* The unterminated EXIT object must not alter shell state. */
	shell_run();
	if (keyboard_readline_calls != 1u || iomgr_open_calls != 0u ||
	    iomgr_other_calls != 0u)
		return 3;

	if (!shell_init(TEST_VOLUME, DOS_FIRST_FIXED_DRIVE_INDEX))
		return 4;
	shell_execute_line(type_line, sizeof(type_line));
	if (iomgr_open_calls != 1u || iomgr_other_calls != 0u ||
	    last_path_length != sizeof(type_path) - 1u ||
	    !text_equals(last_path, type_path, sizeof(type_path)))
		return 4;

	reset_console_capture();
	shell_execute_line(unknown_line, sizeof(unknown_line));
	if (!console_equals(unknown_message, sizeof(unknown_message)))
		return 5;
	reset_console_capture();
	shell_execute_line(bad_ver_line, sizeof(bad_ver_line));
	if (!console_equals(syntax_message, sizeof(syntax_message)))
		return 6;

	reset_console_capture();
	autoexec_read_failure_test = true;
	shell_run_autoexec();
	autoexec_read_failure_test = false;
	if (!console_equals(autoexec_corrupt_message,
			    sizeof(autoexec_corrupt_message)))
		return 7;

	for (size_t id = 0u; id < (size_t)DOS_UI_MESSAGE_COUNT; ++id) {
		struct dos_ui_text text =
			dos_ui_text_get((enum dos_ui_message_id)id);

		if (text.data == NULL || text.length == 0u ||
		    text.data[text.length] != '\0')
			return 8;
	}
	return 0;
}

DOSC32_TEST_ENTRY(run_tests)

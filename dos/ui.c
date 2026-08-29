// SPDX-License-Identifier: GPL-2.0-only
/* Central English message catalog for boot and COMMAND presentation. */
#include "dos_ui.h"

#define UI_TEXT(value) { (value), sizeof(value) - 1u }

static const struct dos_ui_text messages[DOS_UI_MESSAGE_COUNT] = {
	[DOS_UI_BOOT_BANNER] = UI_TEXT(
		"DOS-C32: an MS-DOS-compatible protected-mode system\n"),
	[DOS_UI_PLATFORM_BANNER] = UI_TEXT("Platform: 32-bit i386, Legacy BIOS "
					   "| License: GPL-2.0-only\n\n"),
	[DOS_UI_BOOT_UNSUPPORTED_DRIVE] = UI_TEXT(
		"Boot failed: firmware identity does not match a supported device "
		"for BIOS drive "),
	[DOS_UI_BOOT_VOLUME_ERROR] =
		UI_TEXT("Could not mount the boot volume: "),
	[DOS_UI_BOOT_VOLUME_CORRUPT] =
		UI_TEXT("filesystem or partition metadata is corrupt.\n"),
	[DOS_UI_BOOT_VOLUME_UNSUPPORTED] =
		UI_TEXT("the filesystem format is not supported.\n"),
	[DOS_UI_BOOT_VOLUME_IO_ERROR] =
		UI_TEXT("the boot device could not be read.\n"),
	[DOS_UI_BOOT_VOLUME_NOT_FOUND] =
		UI_TEXT("no supported volume or active partition was found.\n"),
	[DOS_UI_BOOT_VOLUME_INTERNAL_ERROR] =
		UI_TEXT("an internal I/O Manager error occurred; status "),
	[DOS_UI_IOMGR_ERROR_PREFIX] = UI_TEXT(" failed: "),
	[DOS_UI_IOMGR_CORRUPT] =
		UI_TEXT("filesystem metadata is corrupt.\n"),
	[DOS_UI_IOMGR_UNSUPPORTED] =
		UI_TEXT("the operation is not supported by this filesystem.\n"),
	[DOS_UI_IOMGR_IO_ERROR] =
		UI_TEXT("the storage device could not complete the request.\n"),
	[DOS_UI_IOMGR_READ_ONLY] = UI_TEXT("the volume is read-only.\n"),
	[DOS_UI_IOMGR_NO_SPACE] = UI_TEXT("the volume has no free space.\n"),
	[DOS_UI_IOMGR_NOT_FOUND] = UI_TEXT("the requested path was not found.\n"),
	[DOS_UI_IOMGR_INVALID_PATH] = UI_TEXT("the requested path is invalid.\n"),
	[DOS_UI_IOMGR_BUSY] = UI_TEXT("the resource is busy.\n"),
	[DOS_UI_IOMGR_INTERNAL_ERROR] =
		UI_TEXT("an internal I/O Manager error occurred; status "),
	[DOS_UI_SHELL_SYNTAX_ERROR] =
		UI_TEXT("Syntax error: check the command and its arguments.\n"),
	[DOS_UI_VOLUME_NO_LABEL] = UI_TEXT(" has no volume label.\n"),
	[DOS_UI_VOLUME_LABEL] = UI_TEXT("Volume label for drive "),
	[DOS_UI_DIRECTORY_HEADING] = UI_TEXT("Directory: "),
	[DOS_UI_TOTAL_FILES] = UI_TEXT("Files: "),
	[DOS_UI_TOTAL_BYTES] = UI_TEXT(", total bytes: "),
	[DOS_UI_TOTAL_DIRECTORIES] = UI_TEXT("Directories: "),
	[DOS_UI_FREE_BYTES] = UI_TEXT(", free bytes: "),
	[DOS_UI_SIZE_OVERFLOW] = UI_TEXT(
		"The total size exceeds the supported display range.\n"),
	[DOS_UI_FREE_SPACE_OVERFLOW] =
		UI_TEXT("More than 4 GiB is available.\n"),
	[DOS_UI_INVALID_DRIVE_OR_PATH] =
		UI_TEXT("The drive or path is invalid.\n"),
	[DOS_UI_PATH_TOO_LONG] =
		UI_TEXT("CD failed: the path exceeds the supported length.\n"),
	[DOS_UI_ECHO_STATUS] = UI_TEXT("Command echoing is "),
	[DOS_UI_ENABLED] = UI_TEXT("enabled.\n"),
	[DOS_UI_DISABLED] = UI_TEXT("disabled.\n"),
	[DOS_UI_VERSION] =
		UI_TEXT("\nDOS-C32 compatibility target: MS-DOS\n\n"),
	[DOS_UI_INVALID_DRIVE] = UI_TEXT(
		"The drive specification is invalid; available drive: "),
	[DOS_UI_COMMAND_LINE_TOO_LONG] =
		UI_TEXT("The command line exceeds the 127-byte DOS limit.\n"),
	[DOS_UI_COMMAND_NOT_FOUND] = UI_TEXT("Command not found: "),
	[DOS_UI_AUTOEXEC_LINE_TOO_LONG] = UI_TEXT(
		"AUTOEXEC.BAT: a line exceeds the 127-byte DOS limit.\n"),
	[DOS_UI_FATAL_STACK_CORRUPTION] =
		UI_TEXT("\nFatal error: kernel stack corruption detected; the "
			"system is halted.\n"),
};

struct dos_ui_text dos_ui_text_get(enum dos_ui_message_id id)
{
	static const char invalid[] =
		"Internal error: invalid message identifier.\n";
	struct dos_ui_text fallback = UI_TEXT(invalid);

	if ((uint32_t)id >= (uint32_t)DOS_UI_MESSAGE_COUNT ||
	    messages[id].data == NULL || messages[id].length == 0u)
		return fallback;
	return messages[id];
}

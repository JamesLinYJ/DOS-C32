// SPDX-License-Identifier: GPL-2.0-only
/*
 * DOS-C32 protected-mode kernel entry
 *
 * Compatibility contract: initialize the boot volume before starting COMMAND.
 * Safety changes: fail closed when the BIOS drive cannot be represented by
 *                 the first-stage ATA driver; all diagnostics are bounded.
 */
#include "console.h"
#include "c32_process.h"
#include "address.h"
#include "ata_block.h"
#include "block_device.h"
#include "dos_abi.h"
#include "dos_drive_visibility.h"
#include "dos_country_file.h"
#include "dos_dpb.h"
#include "dos_drive.h"
#include "dos_ems_device.h"
#include "dos_exec_gate.h"
#include "dos_exec_native.h"
#include "dos_environment_view.h"
#include "dos_execution_loop.h"
#include "dos_find.h"
#include "dos_runtime_owner.h"
#include "dos_sft_adapter.h"
#include "dos_termination.h"
#include "dos_ui.h"
#include "dos_path.h"
#include "fat_driver.h"
#include "iomgr.h"
#include "iomgr_device.h"
#include "iomgr_discovery.h"
#include "iomgr_exec_adapter.h"
#include "io.h"
#include "keyboard.h"
#include "object_identity.h"
#include "shell.h"
#include "string.h"
#include "types.h"
#include "x86_guest_fault_ui.h"
#include "x86_boot_info.h"
#include "x86_ems_config.h"
#include "x86_ems_memory.h"
#include "x86_guest_memory_runtime.h"
#include "x86_xms_memory.h"
#include "x86_paging.h"
#include "x86_guest_space.h"
#include "x86_i8042.h"
#include "x86_legacy_bios.h"
#include "x86_legacy_input_runtime.h"
#include "x86_legacy_irq.h"
#include "x86_memory_map.h"
#include "x86_runtime.h"
#include "x86_user.h"
#include "x86_vm86.h"

#include "../config/x86-guest-space.h"
#include "../config/x86-legacy-input.h"
#include "../config/x86-legacy-irq.h"
#include "../config/x86-native-i8042.h"

#if CONFIG_X86_VM_ACCEPTANCE_DIAGNOSTICS
#define DIAGNOSTIC_TEXT_MAX 64u
#endif

static_assert_expression(
	DOS_EXEC_BACKEND_SESSION_SLOT_COUNT > 1u,
	"the EXEC owner needs one parent slot and at least one child slot");
static_assert_expression(
	CONFIG_X86_ATA_WRITE_POLICY == ATA_WRITE_POLICY_READ_ONLY ||
	CONFIG_X86_ATA_WRITE_POLICY == ATA_WRITE_POLICY_ALLOW,
	"boot ATA write policy must be read-only or allow");

static struct kernel_object_identity_source object_id_source =
	KERNEL_OBJECT_IDENTITY_SOURCE_INITIALIZER;
static struct dos_runtime_owner_bindings runtime_bindings;
static iomgr_volume_handle_t system_volume = IOMGR_VOLUME_HANDLE_INVALID;
static iomgr_device_registration_handle_t ems_device_registration =
	IOMGR_DEVICE_REGISTRATION_HANDLE_INVALID;
static struct x86_ems_runtime_binding ems_runtime_binding;
static struct dos_int21_drive_config system_drive_config;
static char dos_current_directory_path[DOS_PATH_CAPACITY];
static char command_interpreter_path[DOS_PATH_CAPACITY];
static size_t command_interpreter_path_length;
static char country_file_path[DOS_PATH_CAPACITY];
static uint8_t country_file_storage[DOS_COUNTRY_MAX_FILE_BYTES];
static struct dos_country_catalog country_catalog;
static struct x86_guest_space_pit_binding native_pit_binding;
static struct x86_native_irq_action_binding native_pit_action_binding;
static kernel_object_handle_t native_irq_source_identity =
	KERNEL_OBJECT_HANDLE_INVALID;
static uint64_t native_pit_input_quantum;
static struct x86_legacy_input_runtime legacy_input_runtime;
static kernel_object_handle_t legacy_input_runtime_identity =
	KERNEL_OBJECT_HANDLE_INVALID;
static kernel_object_handle_t legacy_input_io_context_identity =
	KERNEL_OBJECT_HANDLE_INVALID;
static struct serio_port *legacy_input_serio_ports
	[CONFIG_X86_LEGACY_INPUT_SERIO_PORT_CAPACITY];
static struct serio_driver *legacy_input_serio_drivers
	[CONFIG_X86_LEGACY_INPUT_SERIO_DRIVER_CAPACITY];
static struct input_device *legacy_input_devices
	[CONFIG_X86_LEGACY_INPUT_DEVICE_CAPACITY];
static struct input_handler *legacy_input_handlers
	[CONFIG_X86_LEGACY_INPUT_HANDLER_CAPACITY];
static struct serio_raw_event legacy_input_raw_events
	[CONFIG_X86_LEGACY_INPUT_RAW_QUEUE_CAPACITY];
static struct input_event legacy_input_decoded_events
	[CONFIG_X86_LEGACY_INPUT_DECODED_QUEUE_CAPACITY];
static struct keyboard_key_record legacy_input_console_keys
	[CONFIG_X86_LEGACY_INPUT_CONSOLE_QUEUE_CAPACITY];

static const char command_interpreter_component[] = "COMMAND.COM";
static const char country_file_component[] = "COUNTRY.SYS";

#if CONFIG_X86_VM_ACCEPTANCE_DIAGNOSTICS
/* Acceptance-only tracing. Production builds compile this entire block out. */
static void storage_path_diagnostic(const char *operation,
				    const uint8_t *path, size_t path_length,
				    enum iomgr_status status);
static void storage_io_diagnostic(const char *operation,
				  kernel_object_handle_t file, uint64_t offset,
				  size_t count, enum iomgr_status status);
static void storage_call_diagnostic(void);
static void exec_child_diagnostic(uint16_t child_psp);
static void multiplex_call_diagnostic(
	const struct dos_execution_step_result *step);
#else
#define storage_path_diagnostic(...) ((void)0)
#define storage_io_diagnostic(...) ((void)0)
#define storage_call_diagnostic() ((void)0)
#define exec_child_diagnostic(...) ((void)0)
#define multiplex_call_diagnostic(...) ((void)0)
#endif

static bool initialize_system_drive_namespace(uint8_t bios_boot_drive)
{
	struct dos_int21_drive_config prepared_config;
	char prepared_root[DOS_PATH_CAPACITY];
	char prepared_command[DOS_PATH_CAPACITY];
	char prepared_country[DOS_PATH_CAPACITY];
	size_t root_length;
	size_t command_length;
	size_t country_length;
	uint8_t drive_index;

	/* A:/B: retain their BIOS floppy number.  DOS assigns the active boot
	 * partition of the fixed-disk set to the first fixed letter, C:.  Later
	 * mounted partitions are assigned by the volume namespace, not by DL. */
	if (bios_boot_drive < X86_BIOS_FIRST_FIXED_DISK) {
		if (bios_boot_drive >= X86_BIOS_FLOPPY_DRIVE_COUNT)
			return false;
		drive_index = bios_boot_drive;
	} else {
		drive_index = DOS_FIRST_FIXED_DRIVE_INDEX;
	}
	if (dos_drive_configure_single_volume(drive_index, &prepared_config) !=
			DOS_DRIVE_OK ||
	    dos_drive_format_root(drive_index, prepared_root,
				  sizeof(prepared_root), &root_length) !=
			DOS_DRIVE_OK ||
	    dos_drive_format_absolute(
		    drive_index, command_interpreter_component,
		    sizeof(command_interpreter_component) - 1u, prepared_command,
		    sizeof(prepared_command), &command_length) != DOS_DRIVE_OK ||
	    dos_drive_format_absolute(
		    drive_index, country_file_component,
		    sizeof(country_file_component) - 1u, prepared_country,
		    sizeof(prepared_country), &country_length) != DOS_DRIVE_OK)
		return false;
	if (memcpy_s(dos_current_directory_path,
		     sizeof(dos_current_directory_path), prepared_root,
		     sizeof(prepared_root), root_length + 1u) != MEMORY_OK ||
	    memcpy_s(command_interpreter_path, sizeof(command_interpreter_path),
		     prepared_command, sizeof(prepared_command),
		     command_length + 1u) != MEMORY_OK ||
	    memcpy_s(country_file_path, sizeof(country_file_path),
		     prepared_country, sizeof(prepared_country),
		     country_length + 1u) != MEMORY_OK)
		return false;
	system_drive_config = prepared_config;
	command_interpreter_path_length = command_length;
	return true;
}

static enum iomgr_status canonical_iomgr_path(
	const char *canonical, size_t capacity, struct iomgr_path *path)
{
	uint8_t drive_index;
	uint8_t resolved_drive;
	size_t length;

	if (canonical == NULL || path == NULL || capacity == 0u)
		return IOMGR_INVALID_ARGUMENT;
	length = strnlen(canonical, capacity);
	if (length == capacity || length < 2u || canonical[1] != ':' ||
	    ascii_toupper(canonical[0]) < 'A' ||
	    ascii_toupper(canonical[0]) > 'Z')
		return IOMGR_INVALID_NAME;
	drive_index = (uint8_t)(ascii_toupper(canonical[0]) - 'A');
	if (dos_drive_resolve_designator(
		    &system_drive_config, (uint8_t)(drive_index + 1u),
		    &resolved_drive) != DOS_DRIVE_OK ||
	    resolved_drive != drive_index)
		return IOMGR_NOT_FOUND;
	path->bytes = (const uint8_t *)canonical + 2u;
	path->length = length - 2u;
	return path->length == 0u ? IOMGR_INVALID_NAME : IOMGR_OK;
}

static enum iomgr_status resolve_dos_iomgr_path(
	const uint8_t *input, size_t input_length, char output[DOS_PATH_CAPACITY],
	struct iomgr_path *path)
{
	if (input == NULL || input_length == 0u ||
	    dos_path_canonicalize(dos_current_directory_path,
				  sizeof(dos_current_directory_path),
				  (const char *)input, input_length, output) !=
		    DOS_PATH_OK)
		return IOMGR_INVALID_NAME;
	return canonical_iomgr_path(output, DOS_PATH_CAPACITY, path);
}

static enum dos_error iomgr_dos_error(enum iomgr_status status,
				      bool path_operation)
{
	if (status == IOMGR_NOT_FOUND)
		return path_operation ? DOS_ERROR_PATH_NOT_FOUND
				      : DOS_ERROR_FILE_NOT_FOUND;
	if (status == IOMGR_NOT_DIRECTORY || status == IOMGR_INVALID_NAME)
		return DOS_ERROR_PATH_NOT_FOUND;
	if (status == IOMGR_IS_DIRECTORY || status == IOMGR_READ_ONLY)
		return DOS_ERROR_ACCESS_DENIED;
	if (status == IOMGR_NO_SLOT)
		return DOS_ERROR_TOO_MANY_OPEN_FILES;
	if (status == IOMGR_STALE_HANDLE)
		return DOS_ERROR_INVALID_HANDLE;
	if (status == IOMGR_IO_ERROR)
		return DOS_ERROR_READ_FAULT;
	return DOS_ERROR_GENERAL_FAILURE;
}

static bool dos_console_output(kernel_object_handle_t context,
			       uint8_t character)
{
	if (context != runtime_bindings.machine_identity)
		return false;
	console_putc((char)character);
	return true;
}

static bool dos_console_input_status(kernel_object_handle_t context)
{
	return context == runtime_bindings.machine_identity &&
	       keyboard_character_available();
}

static bool dos_console_input_character(kernel_object_handle_t context,
					uint8_t *character)
{
	if (context != runtime_bindings.machine_identity || character == NULL)
		return false;
	*character = (uint8_t)keyboard_getchar();
	return true;
}

static bool dos_console_input_flush(kernel_object_handle_t context)
{
	if (context != runtime_bindings.machine_identity)
		return false;
	keyboard_flush();
	return true;
}

static enum dos_absolute_disk_status dos_absolute_disk_read(
	kernel_object_handle_t context, uint8_t drive, uint32_t sector,
	uint8_t *destination, size_t capacity)
{
	union block_device_sector data;
	struct iomgr_volume_info volume;
	block_lba_t physical;
	enum block_device_status status;
	size_t index;

	if (context != runtime_bindings.machine_identity || destination == NULL ||
	    capacity < sizeof(data))
		return DOS_ABSOLUTE_DISK_IO_ERROR;
	if (drive != system_drive_config.current_drive)
		return DOS_ABSOLUTE_DISK_BAD_DRIVE;
	if (iomgr_get_volume_info(system_volume, &volume) != IOMGR_OK ||
	    (block_lba_t)sector >= volume.sector_count ||
	    check_add_overflow(volume.first_lba, (block_lba_t)sector,
			       &physical))
		return DOS_ABSOLUTE_DISK_IO_ERROR;
	status = block_device_read_sector(volume.device, physical, &data);
	if (status == BLOCK_DEVICE_OUT_OF_RANGE)
		return DOS_ABSOLUTE_DISK_OUT_OF_RANGE;
	if (status != BLOCK_DEVICE_OK)
		return DOS_ABSOLUTE_DISK_IO_ERROR;
	for (index = 0u; index < sizeof(data.bytes); ++index)
		destination[index] = data.bytes[index];
	return DOS_ABSOLUTE_DISK_OK;
}

static enum dos_error dos_file_attributes(
	kernel_object_handle_t context, const uint8_t *path, size_t path_length,
	uint16_t *attributes)
{
	char canonical[DOS_PATH_CAPACITY];
	struct iomgr_path requested;
	struct iomgr_node_info info;
	enum iomgr_status status;

	if (context != runtime_bindings.machine_identity || path == NULL ||
	    attributes == NULL)
		return DOS_ERROR_INVALID_DATA;
	if (path_length == 0u || path[path_length - 1u] != 0u)
		return DOS_ERROR_INVALID_DATA;
	status = resolve_dos_iomgr_path(path, path_length - 1u, canonical,
					&requested);
	if (status == IOMGR_OK)
		status = iomgr_stat(system_volume, &requested, &info);
	if (status == IOMGR_OK) {
		*attributes = (uint16_t)info.attributes;
		return DOS_SUCCESS;
	}
	return iomgr_dos_error(status, false);
}

static enum dos_error dos_change_directory(
	kernel_object_handle_t context, const uint8_t *path, size_t path_length)
{
	char canonical_path[DOS_PATH_CAPACITY];
	struct iomgr_path requested;
	struct iomgr_node_info info;
	enum iomgr_status status;
	size_t input_length;

	if (context != runtime_bindings.machine_identity || path == NULL ||
	    path_length == 0u || path[path_length - 1u] != 0u)
		return DOS_ERROR_INVALID_DATA;
	input_length = path_length - 1u;
	if (dos_path_canonicalize(dos_current_directory_path,
				  sizeof(dos_current_directory_path),
				  (const char *)path, input_length,
				  canonical_path) != DOS_PATH_OK)
		return DOS_ERROR_PATH_NOT_FOUND;
	status = canonical_iomgr_path(canonical_path, sizeof(canonical_path),
				      &requested);
	if (status == IOMGR_OK)
		status = iomgr_stat(system_volume, &requested, &info);
	if (status == IOMGR_OK &&
	    (info.attributes & IOMGR_NODE_DIRECTORY) != 0u) {
		if (strscpy_s(dos_current_directory_path,
			      sizeof(dos_current_directory_path), canonical_path,
			      sizeof(canonical_path)) == STRSCPY_TRUNCATED)
			return DOS_ERROR_PATH_NOT_FOUND;
		return DOS_SUCCESS;
	}
	if (status == IOMGR_OK)
		status = IOMGR_NOT_DIRECTORY;
	return iomgr_dos_error(status, true);
}

static enum dos_error dos_create_directory(
	kernel_object_handle_t context, const uint8_t *path, size_t path_length)
{
	char canonical_path[DOS_PATH_CAPACITY];
	struct iomgr_path requested;
	enum iomgr_status status;

	if (context != runtime_bindings.machine_identity || path == NULL ||
	    path_length == 0u || path[path_length - 1u] != 0u)
		return DOS_ERROR_INVALID_DATA;
	status = resolve_dos_iomgr_path(path, path_length - 1u, canonical_path,
					&requested);
	if (status == IOMGR_OK)
		status = iomgr_create_directory(system_volume, &requested);
	if (status == IOMGR_OK)
		return DOS_SUCCESS;
	if (status == IOMGR_ALREADY_EXISTS)
		return DOS_ERROR_ACCESS_DENIED;
	if (status == IOMGR_NO_SPACE || status == IOMGR_NO_SLOT)
		return DOS_ERROR_CANNOT_MAKE;
	return iomgr_dos_error(status, true);
}

static enum dos_error dos_get_current_directory(
	kernel_object_handle_t context, uint8_t drive, uint8_t *path,
	size_t capacity, size_t *path_length)
{
	uint8_t resolved_drive;
	size_t length;

	if (context != runtime_bindings.machine_identity || path == NULL ||
	    path_length == NULL)
		return DOS_ERROR_INVALID_DATA;
	if (dos_drive_resolve_designator(&system_drive_config, drive,
					 &resolved_drive) != DOS_DRIVE_OK)
		return DOS_ERROR_INVALID_DRIVE;
	length = strnlen(dos_current_directory_path,
			 sizeof(dos_current_directory_path));
	if (length < 3u || length >= sizeof(dos_current_directory_path) ||
	    (uint8_t)(ascii_toupper(dos_current_directory_path[0]) - 'A') !=
		    resolved_drive ||
	    length - 3u >= capacity)
		return DOS_ERROR_PATH_NOT_FOUND;
	if (memcpy_s(path, capacity, dos_current_directory_path + 3u,
		     length - 3u, length - 3u) != MEMORY_OK)
		return DOS_ERROR_INVALID_DATA;
	*path_length = length - 3u;
	return DOS_SUCCESS;
}

static enum dos_error dos_get_drive_parameter_block(
	kernel_object_handle_t context, uint8_t drive, uint16_t *segment,
	uint16_t *offset)
{
	uint8_t resolved_drive;

	if (context != runtime_bindings.machine_identity || segment == NULL ||
	    offset == NULL)
		return DOS_ERROR_INVALID_DATA;
	/* AH=32h numbers A: as one; zero selects the current/default drive. */
	if (dos_drive_resolve_designator(&system_drive_config, drive,
					 &resolved_drive) != DOS_DRIVE_OK)
		return DOS_ERROR_INVALID_DRIVE;
	(void)resolved_drive;
	*segment = DOS_DPB_SEGMENT;
	*offset = DOS_DPB_OFFSET;
	return DOS_SUCCESS;
}

static enum dos_error dos_get_disk_space(
	kernel_object_handle_t context, uint8_t drive,
	struct dos_int21_disk_space *space)
{
	struct iomgr_space_info info;
	enum iomgr_status status;
	uint8_t resolved_drive;

	if (context != runtime_bindings.machine_identity || space == NULL)
		return DOS_ERROR_INVALID_DATA;
	/* AH=36h uses zero for the default drive and one-based explicit drives. */
	if (dos_drive_resolve_designator(&system_drive_config, drive,
					 &resolved_drive) != DOS_DRIVE_OK)
		return DOS_ERROR_INVALID_DRIVE;
	(void)resolved_drive;
	status = iomgr_query_space(system_volume, true, &info);
	if (status != IOMGR_OK)
		return iomgr_dos_error(status, false);
	*space = (struct dos_int21_disk_space){
		.total_bytes = info.total_bytes,
		.free_bytes = info.free_bytes,
		.allocation_unit_bytes = info.allocation_unit_bytes,
		.reserved = 0u,
	};
	return DOS_SUCCESS;
}

static uint16_t dos_find_date(const struct iomgr_timestamp *time)
{
	uint16_t year = time->year < 1980u ? 0u : (uint16_t)(time->year - 1980u);

	if (year > 127u)
		year = 127u;
	return (uint16_t)((year << 9) | ((uint16_t)time->month << 5) |
			  time->day);
}

static uint16_t dos_find_time(const struct iomgr_timestamp *time)
{
	return (uint16_t)(((uint16_t)time->hour << 11) |
			  ((uint16_t)time->minute << 5) |
			  ((uint16_t)time->second >> 1));
}

static enum dos_error dos_find_result(iomgr_search_handle_t search,
				      uint8_t search_attributes,
				      const struct iomgr_directory_entry *entry,
				      struct dos_find_record *record)
{
	struct dos_find_record prepared;

	if (search == IOMGR_SEARCH_HANDLE_INVALID || entry == NULL ||
	    record == NULL || entry->info.size > 0xffffffffu ||
	    entry->name_length >= DOS_FIND_PACKED_NAME_BYTES)
		return DOS_ERROR_INVALID_DATA;
	prepared = (struct dos_find_record){
		.file_size = (uint32_t)entry->info.size,
		.search_handle = search,
		.modified_time = dos_find_time(&entry->info.modified),
		.modified_date = dos_find_date(&entry->info.modified),
		.search_drive = system_drive_config.current_drive + 1u,
		.search_name = {0u},
		.search_attributes = search_attributes,
		.found_attributes = (uint8_t)entry->info.attributes,
		.packed_name = {0u},
	};

	if (memcpy_s(prepared.packed_name, sizeof(prepared.packed_name),
		     entry->name, sizeof(entry->name),
		     entry->name_length + 1u) != MEMORY_OK)
		return DOS_ERROR_INVALID_DATA;
	*record = prepared;
	return DOS_SUCCESS;
}

static bool close_find_search(iomgr_search_handle_t search)
{
	enum iomgr_status status = iomgr_close_search(search);

	return status == IOMGR_OK;
}

static enum dos_error dos_find_first(kernel_object_handle_t context,
				     const uint8_t *path, size_t path_length,
				     uint8_t attributes,
				     struct dos_find_record *record)
{
	char canonical[DOS_PATH_CAPACITY];
	struct iomgr_path pattern;
	struct iomgr_directory_entry entry;
	iomgr_search_handle_t search;
	enum iomgr_status status;

	if (context != runtime_bindings.machine_identity || path == NULL ||
	    path_length == 0u || record == NULL ||
	    path[path_length - 1u] != 0u)
		return DOS_ERROR_INVALID_DATA;
	status = resolve_dos_iomgr_path(path, path_length - 1u, canonical,
					&pattern);
	if (status == IOMGR_OK)
		status = iomgr_open_search(system_volume, &pattern, attributes,
					   &search);
	if (status != IOMGR_OK)
		return iomgr_dos_error(status, true);
	status = iomgr_search_next(search, &entry);
	if (status == IOMGR_END_OF_SEARCH) {
		if (iomgr_close_search(search) != IOMGR_OK)
			return DOS_ERROR_GENERAL_FAILURE;
		return DOS_ERROR_NO_MORE_FILES;
	}
	if (status != IOMGR_OK) {
		if (!close_find_search(search))
			return DOS_ERROR_GENERAL_FAILURE;
		return iomgr_dos_error(status, false);
	}
	{
		enum dos_error result =
			dos_find_result(search, attributes, &entry, record);

		if (result != DOS_SUCCESS && !close_find_search(search))
			return DOS_ERROR_GENERAL_FAILURE;
		return result;
	}
}

static enum dos_error dos_find_next(kernel_object_handle_t context,
				    const struct dos_find_record *previous,
				    struct dos_find_record *record)
{
	struct iomgr_directory_entry entry;
	enum iomgr_status status;

	if (context != runtime_bindings.machine_identity || previous == NULL ||
	    record == NULL)
		return DOS_ERROR_INVALID_DATA;
	if (previous->search_drive !=
			(uint8_t)(system_drive_config.current_drive + 1u) ||
	    previous->search_handle == IOMGR_SEARCH_HANDLE_INVALID)
		return DOS_ERROR_NO_MORE_FILES;
	status = iomgr_search_next(previous->search_handle, &entry);
	if (status == IOMGR_END_OF_SEARCH) {
		if (iomgr_close_search(previous->search_handle) != IOMGR_OK)
			return DOS_ERROR_GENERAL_FAILURE;
		return DOS_ERROR_NO_MORE_FILES;
	}
	if (status != IOMGR_OK) {
		if (!close_find_search(previous->search_handle))
			return DOS_ERROR_GENERAL_FAILURE;
		return iomgr_dos_error(status, false);
	}
	{
		enum dos_error result = dos_find_result(
			previous->search_handle, previous->search_attributes,
			&entry, record);

		if (result != DOS_SUCCESS &&
		    !close_find_search(previous->search_handle))
			return DOS_ERROR_GENERAL_FAILURE;
		return result;
	}
}

static const struct dos_int21_find_ops dos_iomgr_find_ops = {
	.first = dos_find_first,
	.next = dos_find_next,
};

static bool initialize_dos_drive_parameter_block(void)
{
	enum {
		DRIVER_INDEX = DOS_DPB_DRIVER_OFFSET - DOS_DPB_OFFSET,
		STORAGE_BYTES = DRIVER_INDEX + DOS_DPB_DRIVER_BYTES
	};
	const struct dos_machine *machine = x86_guest_space_machine();
	struct fat_driver_volume_snapshot volume;
	uint8_t storage[STORAGE_BYTES] = {0u};
	struct dos_dpb_parameters parameters;

	if (machine == NULL ||
	    fat_driver_get_volume(system_volume, &volume) != IOMGR_OK ||
	    volume.layout.fat_bits != FAT_TABLE_16 ||
	    volume.layout.sectors_per_fat > 0xffffu ||
	    volume.layout.cluster_limit == 0u ||
	    volume.layout.cluster_limit - 1u > 0xffffu)
		return false;
	parameters = (struct dos_dpb_parameters){
		.root_start = volume.layout.root_start,
		.data_start = volume.layout.data_start,
		.bytes_per_sector = volume.layout.sector_bytes,
		.reserved_sectors = volume.layout.reserved_sectors,
		.root_entries = volume.layout.root_entries,
		.sectors_per_fat = (uint16_t)volume.layout.sectors_per_fat,
		.maximum_cluster =
			(uint16_t)(volume.layout.cluster_limit - 1u),
		.drive = system_drive_config.current_drive,
		.unit = 0u,
		.sectors_per_cluster = volume.layout.sectors_per_cluster,
		.fat_count = volume.layout.fat_count,
		.media = volume.layout.media,
	};
	if (dos_dpb_encode(&parameters, storage) != DOS_DPB_OK)
		return false;
	dos_dpb_encode_driver_header(storage + DRIVER_INDEX);
	return dos_machine_write_far(machine, DOS_DPB_SEGMENT, DOS_DPB_OFFSET,
				     storage, sizeof(storage), sizeof(storage)) ==
	       DOS_MACHINE_OK;
}

static bool shell_commit_directory(const char *canonical_path,
				   size_t path_capacity)
{
	char prepared[DOS_PATH_CAPACITY];
	size_t length;

	if (canonical_path == NULL || path_capacity == 0u)
		return false;
	length = strnlen(canonical_path, path_capacity);
	if (length < 3u || length >= path_capacity ||
	    strscpy_s(prepared, sizeof(prepared), canonical_path,
		      path_capacity) == STRSCPY_TRUNCATED)
		return false;
	if (memcpy_s(dos_current_directory_path,
		     sizeof(dos_current_directory_path), prepared, length + 1u,
		     length + 1u) != MEMORY_OK)
		return false;
	return true;
}

static enum dos_error dos_iomgr_runtime_open(
	kernel_object_handle_t context, const uint8_t *path, size_t path_length,
	kernel_object_handle_t *file, uint64_t *size)
{
	char canonical[DOS_PATH_CAPACITY];
	struct iomgr_path requested;
	struct iomgr_node_info info;
	iomgr_file_handle_t opened;
	enum iomgr_status status;

	if (context != runtime_bindings.machine_identity || path == NULL ||
	    path_length == 0u || path[path_length - 1u] != 0u || file == NULL ||
	    size == NULL)
		return DOS_ERROR_INVALID_DATA;
	status = resolve_dos_iomgr_path(path, path_length - 1u, canonical,
					&requested);
	if (status == IOMGR_OK)
		status = iomgr_open_file(system_volume, &requested, &info,
					 &opened);
	/* Temporary acceptance diagnostic: the Windows setup failure occurs
	 * after several successful compressed-name probes.  Recording every open
	 * establishes the last valid source name without tracing guest
	 * instructions. */
	storage_path_diagnostic("open", path, path_length - 1u, status);
	if (status != IOMGR_OK) {
		if (status == IOMGR_INVALID_ARGUMENT)
			storage_call_diagnostic();
		return iomgr_dos_error(status, false);
	}
	*file = opened;
	*size = info.size;
	return DOS_SUCCESS;
}

static enum dos_error dos_iomgr_runtime_create(
	kernel_object_handle_t context, const uint8_t *path, size_t path_length,
	uint16_t attributes, kernel_object_handle_t *file, uint64_t *size)
{
	char canonical[DOS_PATH_CAPACITY];
	struct iomgr_path requested;
	struct iomgr_node_info info;
	iomgr_file_handle_t created;
	enum iomgr_status status;

	if (context != runtime_bindings.machine_identity || path == NULL ||
	    path_length == 0u || path[path_length - 1u] != 0u || file == NULL ||
	    size == NULL)
		return DOS_ERROR_INVALID_DATA;
	status = resolve_dos_iomgr_path(path, path_length - 1u, canonical,
					&requested);
	if (status == IOMGR_OK)
		status = iomgr_create_file(system_volume, &requested, attributes,
					   &info, &created);
	storage_path_diagnostic("create", path, path_length - 1u, status);
	if (status != IOMGR_OK)
		return iomgr_dos_error(status, false);
	if (info.size != 0u) {
		if (iomgr_close_file(created) != IOMGR_OK)
			return DOS_ERROR_GENERAL_FAILURE;
		return DOS_ERROR_INVALID_DATA;
	}
	*file = created;
	*size = 0u;
	return DOS_SUCCESS;
}

static enum dos_error dos_iomgr_runtime_read(
	kernel_object_handle_t context, kernel_object_handle_t file,
	uint64_t offset, uint8_t *destination, size_t capacity, size_t count,
	size_t *bytes_read)
{
	enum iomgr_status status;

	if (bytes_read != NULL)
		*bytes_read = 0u;
	if (context != runtime_bindings.machine_identity || bytes_read == NULL ||
	    count > capacity || (destination == NULL && count != 0u))
		return DOS_ERROR_INVALID_DATA;
	status = iomgr_read_file(file, offset, destination, capacity, count,
				 bytes_read);
	if (status != IOMGR_OK)
		storage_io_diagnostic("read", file, offset, count, status);
	return status == IOMGR_OK ? DOS_SUCCESS
				  : iomgr_dos_error(status, false);
}

static enum dos_error dos_iomgr_runtime_write(
	kernel_object_handle_t context, kernel_object_handle_t file,
	uint64_t offset, const uint8_t *source, size_t source_capacity,
	size_t count, size_t *bytes_written)
{
	enum iomgr_status status;

	if (bytes_written != NULL)
		*bytes_written = 0u;
	if (context != runtime_bindings.machine_identity ||
	    bytes_written == NULL || count > source_capacity ||
	    (source == NULL && count != 0u))
		return DOS_ERROR_INVALID_DATA;
	status = iomgr_write_file(file, offset, source, source_capacity, count,
				  bytes_written);
	if (status != IOMGR_OK)
		storage_io_diagnostic("write", file, offset, count, status);
	return status == IOMGR_OK ? DOS_SUCCESS
				  : iomgr_dos_error(status, false);
}

static enum dos_error dos_iomgr_runtime_get_time(
	kernel_object_handle_t context, kernel_object_handle_t file,
	uint16_t *date, uint16_t *time)
{
	struct iomgr_node_info info;
	enum iomgr_status status;

	if (context != runtime_bindings.machine_identity || date == NULL ||
	    time == NULL)
		return DOS_ERROR_INVALID_DATA;
	status = iomgr_get_file_info(file, &info);
	if (status != IOMGR_OK)
		storage_io_diagnostic("get-time", file, 0u, 0u, status);
	if (status != IOMGR_OK)
		return iomgr_dos_error(status, false);
	*date = dos_find_date(&info.modified);
	*time = dos_find_time(&info.modified);
	return DOS_SUCCESS;
}

static enum dos_error dos_iomgr_runtime_set_time(
	kernel_object_handle_t context, kernel_object_handle_t file,
	uint16_t date, uint16_t time)
{
	struct iomgr_file_update update = {
		.valid = IOMGR_FILE_UPDATE_MODIFIED,
		.modified = {
			.year = (uint16_t)(1980u + ((date >> 9u) & 0x7fu)),
			.month = (uint8_t)((date >> 5u) & 0x0fu),
			.day = (uint8_t)(date & 0x1fu),
			.hour = (uint8_t)((time >> 11u) & 0x1fu),
			.minute = (uint8_t)((time >> 5u) & 0x3fu),
			.second = (uint8_t)((time & 0x1fu) * 2u),
			.centiseconds = 0u,
		},
	};
	enum iomgr_status status;

	if (context != runtime_bindings.machine_identity)
		return DOS_ERROR_INVALID_DATA;
	status = iomgr_set_file_info(file, &update);
	if (status != IOMGR_OK)
		storage_io_diagnostic("set-time", file, 0u, 0u, status);
	return status == IOMGR_OK ? DOS_SUCCESS
				  : iomgr_dos_error(status, false);
}

static enum dos_error dos_iomgr_runtime_rename(
	kernel_object_handle_t context, const uint8_t *old_path,
	size_t old_path_length, const uint8_t *new_path,
	size_t new_path_length)
{
	char old_canonical[DOS_PATH_CAPACITY];
	char new_canonical[DOS_PATH_CAPACITY];
	struct iomgr_path old_requested;
	struct iomgr_path new_requested;
	enum iomgr_status status;

	if (context != runtime_bindings.machine_identity || old_path == NULL ||
	    new_path == NULL || old_path_length == 0u || new_path_length == 0u ||
	    old_path[old_path_length - 1u] != 0u ||
	    new_path[new_path_length - 1u] != 0u)
		return DOS_ERROR_INVALID_DATA;
	status = resolve_dos_iomgr_path(old_path, old_path_length - 1u,
					old_canonical, &old_requested);
	if (status == IOMGR_OK)
		status = resolve_dos_iomgr_path(new_path, new_path_length - 1u,
						new_canonical, &new_requested);
	if (status == IOMGR_OK)
		status = iomgr_rename(system_volume, &old_requested,
				      &new_requested);
	if (status != IOMGR_OK)
		storage_path_diagnostic("rename", old_path,
					old_path_length - 1u, status);
	return status == IOMGR_OK ? DOS_SUCCESS
				  : iomgr_dos_error(status, false);
}

static enum dos_error dos_iomgr_runtime_close(
	kernel_object_handle_t context, kernel_object_handle_t file)
{
	if (context != runtime_bindings.machine_identity)
		return DOS_ERROR_INVALID_DATA;
	{
		enum iomgr_status status = iomgr_close_file(file);

		if (status != IOMGR_OK)
			storage_io_diagnostic("close", file, 0u, 0u, status);
		return status == IOMGR_OK ? DOS_SUCCESS
					  : DOS_ERROR_INVALID_HANDLE;
	}
}

static enum dos_sft_backend_close_status dos_sft_runtime_close(
	kernel_object_handle_t context, enum dos_sft_backend_kind backend_kind,
	kernel_object_handle_t backend_handle, enum dos_error *exact_error)
{
	enum iomgr_status status;

	if (exact_error == NULL || context != runtime_bindings.machine_identity)
		return DOS_SFT_BACKEND_CLOSE_UNCERTAIN;
	*exact_error = DOS_SUCCESS;
	if (backend_kind == DOS_SFT_BACKEND_STANDARD)
		return DOS_SFT_BACKEND_CLOSE_OK;
	if (backend_kind == DOS_SFT_BACKEND_FILE) {
		*exact_error = dos_iomgr_runtime_close(context, backend_handle);
		return *exact_error == DOS_SUCCESS
			       ? DOS_SFT_BACKEND_CLOSE_OK
			       : DOS_SFT_BACKEND_CLOSE_EXACT_FAILURE;
	}
	if (backend_kind != DOS_SFT_BACKEND_DEVICE)
		return DOS_SFT_BACKEND_CLOSE_UNCERTAIN;
	status = iomgr_device_close(backend_handle);
	if (status == IOMGR_OK)
		return DOS_SFT_BACKEND_CLOSE_OK;
	if (status == IOMGR_UNCERTAIN || status == IOMGR_POISONED ||
	    status == IOMGR_NOT_INITIALIZED || status == IOMGR_CORRUPT)
		return DOS_SFT_BACKEND_CLOSE_UNCERTAIN;
	if (status == IOMGR_STALE_HANDLE || status == IOMGR_NOT_FOUND)
		*exact_error = DOS_ERROR_INVALID_HANDLE;
	else if (status == IOMGR_UNSUPPORTED)
		*exact_error = DOS_ERROR_INVALID_FUNCTION;
	else if (status == IOMGR_INVALID_ARGUMENT)
		*exact_error = DOS_ERROR_INVALID_DATA;
	else if (status == IOMGR_NO_SLOT)
		*exact_error = DOS_ERROR_TOO_MANY_OPEN_FILES;
	else if (status == IOMGR_NO_SPACE)
		*exact_error = DOS_ERROR_HANDLE_DISK_FULL;
	else
		*exact_error = DOS_ERROR_ACCESS_DENIED;
	return DOS_SFT_BACKEND_CLOSE_EXACT_FAILURE;
}

static const struct dos_int21_file_ops dos_iomgr_runtime_ops = {
	.open = dos_iomgr_runtime_open,
	.create = dos_iomgr_runtime_create,
	.read = dos_iomgr_runtime_read,
	.write = dos_iomgr_runtime_write,
	.get_time = dos_iomgr_runtime_get_time,
	.set_time = dos_iomgr_runtime_set_time,
	.rename = dos_iomgr_runtime_rename,
	.close = dos_iomgr_runtime_close,
};

static void load_country_file(void)
{
	const struct dos_nls_package *packages[DOS_COUNTRY_MAX_PACKAGES];
	struct dos_personality *personality = dos_runtime_owner_personality();
	struct iomgr_path path;
	struct iomgr_node_info info;
	iomgr_file_handle_t file = IOMGR_FILE_HANDLE_INVALID;
	enum iomgr_status io_status;
	size_t bytes_read = 0u;
	size_t index;
	bool valid = false;

	if (personality == NULL ||
	    canonical_iomgr_path(country_file_path, sizeof(country_file_path),
				 &path) != IOMGR_OK)
		goto fallback;
	io_status = iomgr_open_file(system_volume, &path, &info, &file);
	if (io_status != IOMGR_OK || info.size == 0u ||
	    info.size > sizeof(country_file_storage))
		goto close;
	io_status = iomgr_read_file(file, 0u, country_file_storage,
				    sizeof(country_file_storage), (size_t)info.size,
				    &bytes_read);
	if (io_status != IOMGR_OK || bytes_read != (size_t)info.size ||
	    dos_country_parse(country_file_storage, bytes_read,
			      &country_catalog) != DOS_COUNTRY_OK)
		goto close;
	for (index = 0u; index < country_catalog.package_count; ++index)
		packages[index] = &country_catalog.packages[index].package;
	valid = dos_nls_runtime_publish_catalog(&personality->int21.nls, packages,
						country_catalog.package_count);
close:
	if (file != IOMGR_FILE_HANDLE_INVALID &&
	    iomgr_close_file(file) != IOMGR_OK)
		valid = false;
fallback:
	if (!valid)
		console_write_literal(
			"DOS-C32: COUNTRY.SYS rejected; using built-in CP437.\n");
}
#if CONFIG_BOOT_SELFTESTS
static kernel_object_handle_t vm86_session_table_identity;
static kernel_object_handle_t dos_personality_identity;
static kernel_object_handle_t dos_runtime_identity;
static kernel_object_handle_t dos_memory_arena_identity;
static kernel_object_handle_t guest_memory_test_identity;
#endif

void kmain(uint32_t boot_drive, const struct x86_boot_info *boot_info);

static void write_ui(enum dos_ui_message_id id)
{
	struct dos_ui_text text = dos_ui_text_get(id);

	console_write(text.data, text.length);
}

#if CONFIG_X86_VM_ACCEPTANCE_DIAGNOSTICS
static void serial_write_u32(uint32_t value)
{
	char digits[10];
	size_t count = 0u;

	if (value == 0u) {
		console_serial_write("0", 1u);
		return;
	}
	while (value != 0u) {
		digits[count++] = (char)('0' + value % 10u);
		value /= 10u;
	}
	while (count != 0u)
		console_serial_write(&digits[--count], 1u);
}

static void serial_write_hex_byte(uint8_t value)
{
	static const char hexadecimal[] = "0123456789ABCDEF";
	char encoded[2];

	encoded[0] = hexadecimal[value >> 4u];
	encoded[1] = hexadecimal[value & 0x0fu];
	console_serial_write(encoded, sizeof(encoded));
}

static void storage_path_diagnostic(const char *operation,
				    const uint8_t *path, size_t path_length,
				    enum iomgr_status status)
{
	static const char prefix[] = "[iomgr] ";
	static const char separator[] = " status=";

	console_serial_write(prefix, sizeof(prefix) - 1u);
	console_serial_write(operation, strnlen(operation, DIAGNOSTIC_TEXT_MAX));
	console_serial_write(" ", 1u);
	if (path != NULL)
		console_serial_write((const char *)path, path_length);
	console_serial_write(" bytes=", sizeof(" bytes=") - 1u);
	if (path != NULL) {
		size_t index;

		for (index = 0u; index < path_length; ++index) {
			if (index != 0u)
				console_serial_write(":", 1u);
			serial_write_hex_byte(path[index]);
		}
	}
	console_serial_write(" length=", sizeof(" length=") - 1u);
	serial_write_u32((uint32_t)path_length);
	console_serial_write(separator, sizeof(separator) - 1u);
	serial_write_u32((uint32_t)status);
	console_serial_write("\n", 1u);
}

static void storage_io_diagnostic(const char *operation,
				  kernel_object_handle_t file, uint64_t offset,
				  size_t count, enum iomgr_status status)
{
	static const char prefix[] = "[iomgr] ";

	console_serial_write(prefix, sizeof(prefix) - 1u);
	console_serial_write(operation, strnlen(operation, DIAGNOSTIC_TEXT_MAX));
	console_serial_write(" handle=", sizeof(" handle=") - 1u);
	serial_write_u32((uint32_t)file);
	console_serial_write(" offset=", sizeof(" offset=") - 1u);
	serial_write_u32((uint32_t)offset);
	console_serial_write(" count=", sizeof(" count=") - 1u);
	serial_write_u32((uint32_t)count);
	console_serial_write(" status=", sizeof(" status=") - 1u);
	serial_write_u32((uint32_t)status);
	console_serial_write("\n", 1u);
}

static void storage_call_diagnostic(void)
{
	const struct dos_machine *machine = x86_guest_space_machine();
	struct dos_cpu_state state;
	uint8_t stack[48];
	uint8_t vector;
	uint16_t frame_bp;
	uint32_t depth;
	uint32_t previous;
	size_t index;

	if (!x86_vm86_last_software_interrupt(&state, &vector))
		return;
	console_serial_write("[vm86-call] vector=",
			     sizeof("[vm86-call] vector=") - 1u);
	serial_write_u32(vector);
	console_serial_write(" cs=", sizeof(" cs=") - 1u);
	serial_write_u32(state.cs);
	console_serial_write(" ip=", sizeof(" ip=") - 1u);
	serial_write_u32(state.eip);
	console_serial_write(" ax=", sizeof(" ax=") - 1u);
	serial_write_u32(dos_register_low16(state.eax));
	console_serial_write(" bx=", sizeof(" bx=") - 1u);
	serial_write_u32(dos_register_low16(state.ebx));
	console_serial_write(" cx=", sizeof(" cx=") - 1u);
	serial_write_u32(dos_register_low16(state.ecx));
	console_serial_write(" dx=", sizeof(" dx=") - 1u);
	serial_write_u32(dos_register_low16(state.edx));
	console_serial_write(" ds=", sizeof(" ds=") - 1u);
	serial_write_u32(state.ds);
	console_serial_write(" ss=", sizeof(" ss=") - 1u);
	serial_write_u32(state.ss);
	console_serial_write(" sp=", sizeof(" sp=") - 1u);
	serial_write_u32(dos_register_low16(state.esp));
	console_serial_write(" bp=", sizeof(" bp=") - 1u);
	serial_write_u32(dos_register_low16(state.ebp));
	console_serial_write("\n", 1u);
	if (machine == NULL)
		return;
	frame_bp = dos_register_low16(state.ebp);
	for (depth = 0u; depth < 4u; ++depth) {
		uint16_t next_bp;

		if (dos_machine_read_far(machine, state.ss, frame_bp, stack,
					 sizeof(stack), sizeof(stack)) != DOS_MACHINE_OK)
			return;
		console_serial_write("[vm86-frame] bp=",
				     sizeof("[vm86-frame] bp=") - 1u);
		serial_write_u32(frame_bp);
		console_serial_write(" bytes=", sizeof(" bytes=") - 1u);
		for (index = 0u; index < sizeof(stack); ++index) {
			if (index != 0u)
				console_serial_write(":", 1u);
			serial_write_hex_byte(stack[index]);
		}
		console_serial_write("\n", 1u);
		next_bp = (uint16_t)stack[0] |
			  ((uint16_t)stack[1] << 8u);
		if (next_bp <= frame_bp)
			break;
		frame_bp = next_bp;
	}
	for (previous = 16u; previous != 0u; --previous) {
		if (!x86_vm86_recent_software_interrupt(previous - 1u, &state,
							 &vector))
			continue;
		console_serial_write("[vm86-recent] vector=",
				     sizeof("[vm86-recent] vector=") - 1u);
		serial_write_u32(vector);
		console_serial_write(" ax=", sizeof(" ax=") - 1u);
		serial_write_u32(dos_register_low16(state.eax));
		console_serial_write(" bx=", sizeof(" bx=") - 1u);
		serial_write_u32(dos_register_low16(state.ebx));
		console_serial_write(" cx=", sizeof(" cx=") - 1u);
		serial_write_u32(dos_register_low16(state.ecx));
		console_serial_write(" dx=", sizeof(" dx=") - 1u);
		serial_write_u32(dos_register_low16(state.edx));
		console_serial_write(" cs=", sizeof(" cs=") - 1u);
		serial_write_u32(state.cs);
		console_serial_write(" ip=", sizeof(" ip=") - 1u);
		serial_write_u32(state.eip);
		console_serial_write("\n", 1u);
	}
}

static void exec_child_diagnostic(uint16_t child_psp)
{
	const struct dos_machine *machine = x86_guest_space_machine();
	uint8_t tail[DOS_COMMAND_TAIL_BYTES + 1u];
	size_t tail_length;
	size_t index;

	if (machine == NULL || child_psp == 0u ||
	    dos_machine_read_far(machine, child_psp,
				 DOS_PSP_COMMAND_TAIL_OFFSET, tail, sizeof(tail),
				 sizeof(tail)) != DOS_MACHINE_OK)
		return;
	tail_length = tail[0];
	if (tail_length > DOS_COMMAND_TAIL_BYTES - 1u)
		return;
	console_serial_write("[exec-child] psp=",
			     sizeof("[exec-child] psp=") - 1u);
	serial_write_u32(child_psp);
	console_serial_write(" tail-length=",
			     sizeof(" tail-length=") - 1u);
	serial_write_u32((uint32_t)tail_length);
	console_serial_write(" tail=", sizeof(" tail=") - 1u);
	if (tail_length != 0u)
		console_serial_write((const char *)tail + 1u, tail_length);
	console_serial_write(" bytes=", sizeof(" bytes=") - 1u);
	for (index = 0u; index < tail_length; ++index) {
		if (index != 0u)
			console_serial_write(":", 1u);
		serial_write_hex_byte(tail[index + 1u]);
	}
	console_serial_write("\n", 1u);
}

static void multiplex_call_diagnostic(
	const struct dos_execution_step_result *step)
{
	static uint32_t emitted;
	struct dos_cpu_state input;
	uint8_t vector;

	if (step == NULL || step->event.vector != 0x2fu || emitted >= 64u ||
	    !x86_vm86_last_software_interrupt(&input, &vector) ||
	    vector != 0x2fu)
		return;
	++emitted;
	console_serial_write("[multiplex] in-ax=",
			     sizeof("[multiplex] in-ax=") - 1u);
	serial_write_u32(dos_register_low16(input.eax));
	console_serial_write(" in-bx=", sizeof(" in-bx=") - 1u);
	serial_write_u32(dos_register_low16(input.ebx));
	console_serial_write(" out-ax=", sizeof(" out-ax=") - 1u);
	serial_write_u32(dos_register_low16(step->state.eax));
	console_serial_write(" out-bx=", sizeof(" out-bx=") - 1u);
	serial_write_u32(dos_register_low16(step->state.ebx));
	console_serial_write(" out-cx=", sizeof(" out-cx=") - 1u);
	serial_write_u32(dos_register_low16(step->state.ecx));
	console_serial_write(" out-dx=", sizeof(" out-dx=") - 1u);
	serial_write_u32(dos_register_low16(step->state.edx));
	console_serial_write(" out-es=", sizeof(" out-es=") - 1u);
	serial_write_u32(step->state.es);
	console_serial_write(" step=", sizeof(" step=") - 1u);
	serial_write_u32(step->status);
	console_serial_write(" disposition=",
			     sizeof(" disposition=") - 1u);
	serial_write_u32((uint32_t)step->interrupt.disposition);
	console_serial_write("\n", 1u);
}
#endif

static void write_boot_volume_error(enum iomgr_status status)
{
	write_ui(DOS_UI_BOOT_VOLUME_ERROR);
	switch (status) {
	case IOMGR_CORRUPT:
		write_ui(DOS_UI_BOOT_VOLUME_CORRUPT);
		break;
	case IOMGR_UNSUPPORTED:
		write_ui(DOS_UI_BOOT_VOLUME_UNSUPPORTED);
		break;
	case IOMGR_IO_ERROR:
		write_ui(DOS_UI_BOOT_VOLUME_IO_ERROR);
		break;
	case IOMGR_NO_DRIVER:
		write_ui(DOS_UI_BOOT_VOLUME_NOT_FOUND);
		break;
	default:
		write_ui(DOS_UI_BOOT_VOLUME_INTERNAL_ERROR);
		console_write_u32((uint32_t)status);
		console_write_literal(".\n");
		break;
	}
}

static bool initialize_guest_space(void)
{
	struct x86_guest_space_config guest_config;
	kernel_object_handle_t address_space_identity;
	kernel_object_handle_t machine_identity;
	kernel_object_handle_t irq_router_identity;
	kernel_object_handle_t i8042_irq_producer_identity;
	kernel_object_handle_t backend_identity;
	kernel_object_handle_t backend_context;

	if (kernel_object_identity_source_initialize(&object_id_source) !=
		    KERNEL_OBJECT_IDENTITY_OK ||
	    kernel_object_identity_allocate(&object_id_source,
					    &address_space_identity) !=
		    KERNEL_OBJECT_IDENTITY_OK ||
	    kernel_object_identity_allocate(&object_id_source,
					    &machine_identity) !=
		    KERNEL_OBJECT_IDENTITY_OK ||
	    kernel_object_identity_allocate(&object_id_source,
					    &irq_router_identity) !=
		    KERNEL_OBJECT_IDENTITY_OK ||
	    kernel_object_identity_allocate(&object_id_source,
					    &i8042_irq_producer_identity) !=
		    KERNEL_OBJECT_IDENTITY_OK ||
	    kernel_object_identity_allocate(&object_id_source,
					    &backend_identity) !=
		    KERNEL_OBJECT_IDENTITY_OK ||
	    kernel_object_identity_allocate(&object_id_source,
					    &backend_context) !=
		    KERNEL_OBJECT_IDENTITY_OK)
		return false;
	guest_config = (struct x86_guest_space_config){
		.address_space_identity = address_space_identity,
		.machine_identity = machine_identity,
		.irq_router_identity = irq_router_identity,
		.i8042_irq_producer_identity = i8042_irq_producer_identity,
		.i8042 = {
			.command_byte = CONFIG_X86_GUEST_I8042_COMMAND_BYTE,
			.input_port = CONFIG_X86_GUEST_I8042_INPUT_PORT,
			.output_port = CONFIG_X86_GUEST_I8042_OUTPUT_PORT,
			.keyboard_present = 1u,
			.auxiliary_present = 0u,
			.keyboard_scanning_enabled = 1u,
			.keyboard_scan_set =
				CONFIG_X86_GUEST_I8042_KEYBOARD_SCAN_SET,
			.keyboard_unlocked = 1u,
			.keyboard_leds = 0u,
			.keyboard_typematic =
				CONFIG_X86_GUEST_I8042_KEYBOARD_TYPEMATIC,
			.keyboard_id_length = 2u,
			.keyboard_id_first =
				CONFIG_X86_GUEST_I8042_KEYBOARD_ID_FIRST,
			.keyboard_id_second =
				CONFIG_X86_GUEST_I8042_KEYBOARD_ID_SECOND,
			.auxiliary_id = 0u,
			.reserved = {0u},
		},
		.reserved = {0u},
	};
	return x86_guest_space_initialize(&guest_config) ==
			X86_GUEST_SPACE_OK &&
	       x86_vm86_backend_initialize(backend_identity, backend_context) ==
			X86_VM86_BACKEND_OK;
}

static bool initialize_guest_memory(const struct x86_boot_info *boot_info)
{
	kernel_object_handle_t manager_identity;
	enum x86_guest_memory_status status;

	if (kernel_object_identity_allocate(&object_id_source,
					    &manager_identity) !=
	    KERNEL_OBJECT_IDENTITY_OK)
		return false;
	status = x86_guest_memory_runtime_initialize(boot_info,
						     manager_identity);
	/* A legacy BIOS without a complete E820 map can still run base DOS.
	 * Extended-memory frontends remain unpublished until a pool exists. */
	return status == X86_GUEST_MEMORY_OK ||
	       status == X86_GUEST_MEMORY_INVALID_MAP;
}

static enum x86_native_irq_action_result native_pit_irq_action(
	kernel_object_handle_t context, const struct x86_native_irq_event *event)
{
	if (event == NULL || context != native_pit_binding.source_identity ||
	    event->hardware_irq != X86_LEGACY_TIMER_IRQ ||
	    native_pit_input_quantum == 0u)
		return X86_NATIVE_IRQ_ACTION_FAULT;
	return x86_guest_space_native_pit_submit(
		       &native_pit_binding, native_pit_input_quantum) ==
		       X86_GUEST_SPACE_OK
		       ? X86_NATIVE_IRQ_ACTION_HANDLED
		       : X86_NATIVE_IRQ_ACTION_FAULT;
}

static bool rollback_legacy_irq_delivery(
	kernel_object_handle_t source_identity, bool action_registered,
	bool guest_bound)
{
	bool action_released = !action_registered;
	bool complete = true;

	if (action_registered) {
		if (x86_legacy_irq_action_quiesce(
			    source_identity, &native_pit_action_binding) ==
			    X86_LEGACY_IRQ_OK &&
		    x86_legacy_irq_action_unregister(
			    source_identity, &native_pit_action_binding) ==
			    X86_LEGACY_IRQ_OK)
			action_released = true;
		else
			complete = false;
	}
	if (guest_bound) {
		if (x86_guest_space_native_pit_quiesce(&native_pit_binding) !=
			    X86_GUEST_SPACE_OK ||
		    x86_guest_space_native_pit_unbind(&native_pit_binding) !=
			    X86_GUEST_SPACE_OK)
			complete = false;
	}
	if (!action_released ||
	    x86_legacy_irq_abort(source_identity) != X86_LEGACY_IRQ_OK)
		complete = false;
	if (complete) {
		native_pit_binding = (struct x86_guest_space_pit_binding){0};
		native_pit_action_binding =
			(struct x86_native_irq_action_binding){0};
		native_pit_input_quantum = 0u;
	}
	return complete;
}

static bool prepare_legacy_irq_delivery(void)
{
	struct x86_native_irq_action_config action_config;
	struct x86_legacy_irq_source_info native_source;
	struct x86_legacy_irq_config irq_config;
	kernel_object_handle_t action_identity;
	kernel_object_handle_t controller_identity;
	kernel_object_handle_t dispatch_identity;
	kernel_object_handle_t source_identity;
	enum x86_guest_space_status guest_status;
	enum x86_legacy_irq_status irq_status;
	bool action_registered = false;
	bool guest_bound = false;

	if (kernel_object_identity_allocate(&object_id_source,
					    &source_identity) !=
		    KERNEL_OBJECT_IDENTITY_OK ||
	    kernel_object_identity_allocate(&object_id_source,
					    &controller_identity) !=
		    KERNEL_OBJECT_IDENTITY_OK ||
	    kernel_object_identity_allocate(&object_id_source,
					    &dispatch_identity) !=
		    KERNEL_OBJECT_IDENTITY_OK ||
	    kernel_object_identity_allocate(&object_id_source,
					    &action_identity) !=
		    KERNEL_OBJECT_IDENTITY_OK)
		return false;
	irq_config = (struct x86_legacy_irq_config){
		.source_identity = source_identity,
		.controller_identity = controller_identity,
		.dispatch_identity = dispatch_identity,
		.pit_input_quantum = CONFIG_X86_NATIVE_PIT_INPUT_QUANTUM,
		.vector_base = X86_LEGACY_IRQ_VECTOR_BASE,
		.present_irq_mask = X86_LEGACY_PIC_IRQ_MASK_ALL,
		/* Both enabled lines must have actions before publication.  The PIC
		 * remains fully masked throughout prepare, so IRQ1 cannot race the
		 * serio/input topology that is attached after this function returns. */
		.enabled_irq_mask =
			(uint16_t)((1u << X86_LEGACY_TIMER_IRQ) |
				   (1u << X86_LEGACY_KEYBOARD_IRQ)),
		.pit_reload = CONFIG_X86_NATIVE_PIT_RELOAD,
		.pit_rate_calibrated = CONFIG_X86_NATIVE_PIT_RATE_CALIBRATED,
		.present = 1u,
		.presence_evidence =
			X86_LEGACY_PIC_EVIDENCE_PLATFORM_ASSIGNED,
		.reserved = {0u},
	};
	irq_status = x86_legacy_irq_prepare(&irq_config);
	if (irq_status != X86_LEGACY_IRQ_OK)
		return false;
	irq_status = x86_legacy_irq_source_info(source_identity,
						&native_source);
	if (irq_status != X86_LEGACY_IRQ_OK ||
	    native_source.capabilities != X86_LEGACY_IRQ_SOURCE_PIT_CLOCK)
		goto rollback;
	guest_status = x86_guest_space_native_pit_bind(
		x86_guest_space_machine_identity(), source_identity,
		native_source.pit_input_quantum,
		native_source.pit_rate_calibrated != 0u, &native_pit_binding);
	if (guest_status != X86_GUEST_SPACE_OK)
		goto rollback;
	guest_bound = true;
	native_pit_input_quantum = native_source.pit_input_quantum;
	action_config = (struct x86_native_irq_action_config){
		.identity = action_identity,
		.context = source_identity,
		.hardware_irq = X86_LEGACY_TIMER_IRQ,
		.shared = 0u,
		.reserved = {0u},
		.handler = native_pit_irq_action,
	};
	irq_status = x86_legacy_irq_action_register(
		source_identity, &action_config, &native_pit_action_binding);
	if (irq_status != X86_LEGACY_IRQ_OK)
		goto rollback;
	action_registered = true;
	native_irq_source_identity = source_identity;
	return true;

rollback:
	{
		bool rollback_complete = rollback_legacy_irq_delivery(
			source_identity, action_registered, guest_bound);

		(void)rollback_complete;
	}
	return false;
}

static bool publish_legacy_irq_delivery(void)
{
	return native_irq_source_identity != KERNEL_OBJECT_HANDLE_INVALID &&
	       x86_legacy_irq_publish(native_irq_source_identity) ==
		       X86_LEGACY_IRQ_OK;
}

static bool legacy_input_read_port_is_valid(uint16_t port)
{
	return port == X86_I8042_DATA_PORT || port == X86_I8042_STATUS_PORT;
}

static bool legacy_input_write_port_is_valid(uint16_t port)
{
	return port == X86_I8042_DATA_PORT || port == X86_I8042_COMMAND_PORT;
}

static enum x86_native_i8042_io_status legacy_input_control_read8(
	kernel_object_handle_t context, uint16_t port, uint8_t *value)
{
	if (context != legacy_input_io_context_identity || value == NULL ||
	    !legacy_input_read_port_is_valid(port))
		return X86_NATIVE_I8042_IO_FAULT;
	*value = inb(port);
	return X86_NATIVE_I8042_IO_OK;
}

static enum x86_native_i8042_io_status legacy_input_control_write8(
	kernel_object_handle_t context, uint16_t port, uint8_t value)
{
	if (context != legacy_input_io_context_identity ||
	    !legacy_input_write_port_is_valid(port))
		return X86_NATIVE_I8042_IO_FAULT;
	outb(port, value);
	return X86_NATIVE_I8042_IO_OK;
}

static enum x86_native_input_status legacy_input_read8(
	kernel_object_handle_t context, uint16_t port, uint8_t *value)
{
	if (context != legacy_input_io_context_identity)
		return X86_NATIVE_INPUT_IDENTITY_MISMATCH;
	if (value == NULL || !legacy_input_read_port_is_valid(port))
		return X86_NATIVE_INPUT_INVALID_ARGUMENT;
	*value = inb(port);
	return X86_NATIVE_INPUT_OK;
}

static enum x86_native_input_status legacy_input_write8(
	kernel_object_handle_t context, uint16_t port, uint8_t value)
{
	if (context != legacy_input_io_context_identity)
		return X86_NATIVE_INPUT_IDENTITY_MISMATCH;
	if (!legacy_input_write_port_is_valid(port))
		return X86_NATIVE_INPUT_INVALID_ARGUMENT;
	outb(port, value);
	return X86_NATIVE_INPUT_OK;
}

static bool allocate_legacy_input_identities(
	struct x86_legacy_input_runtime_config *config)
{
	kernel_object_handle_t *const identities[] = {
		&config->identity,
		&config->serio_registry_identity,
		&config->input_core_identity,
		&config->controller_identity,
		&config->io_context_identity,
		&config->keyboard_port_identity,
		&config->atkbd_driver_identity,
		&config->input_device_identity,
		&config->console_handler_identity,
		&config->guest_handler_identity,
		&config->guest_context_identity,
		&config->guest_source_identity,
		&config->irq_action_identity,
	};
	size_t index;

	for (index = 0u; index < ARRAY_SIZE(identities); ++index) {
		if (kernel_object_identity_allocate(&object_id_source,
						    identities[index]) !=
		    KERNEL_OBJECT_IDENTITY_OK)
			return false;
	}
	config->guest_machine_identity = x86_guest_space_machine_identity();
	config->legacy_irq_source_identity = native_irq_source_identity;
	return config->guest_machine_identity != KERNEL_OBJECT_HANDLE_INVALID &&
	       config->legacy_irq_source_identity !=
		       KERNEL_OBJECT_HANDLE_INVALID;
}

static bool initialize_legacy_input(void)
{
	struct x86_legacy_input_runtime_config config = {0};
	enum x86_legacy_input_status status;

	if (!allocate_legacy_input_identities(&config))
		return false;
	legacy_input_io_context_identity = config.io_context_identity;
	config.control_read8 = legacy_input_control_read8;
	config.control_write8 = legacy_input_control_write8;
	config.input_read8 = legacy_input_read8;
	config.input_write8 = legacy_input_write8;
	config.console_wait = keyboard_console_x86_wait;
	config.console_wait_context = NULL;
	config.storage = (struct x86_legacy_input_storage){
		.serio_ports = legacy_input_serio_ports,
		.serio_drivers = legacy_input_serio_drivers,
		.input_devices = legacy_input_devices,
		.input_handlers = legacy_input_handlers,
		.keyboard_bytes = legacy_input_raw_events,
		.decoded_events = legacy_input_decoded_events,
		.console_keys = legacy_input_console_keys,
		.serio_port_capacity = ARRAY_SIZE(legacy_input_serio_ports),
		.serio_driver_capacity = ARRAY_SIZE(legacy_input_serio_drivers),
		.input_device_capacity = ARRAY_SIZE(legacy_input_devices),
		.input_handler_capacity = ARRAY_SIZE(legacy_input_handlers),
		.keyboard_byte_capacity = ARRAY_SIZE(legacy_input_raw_events),
		.decoded_event_capacity = ARRAY_SIZE(legacy_input_decoded_events),
		.console_key_capacity = ARRAY_SIZE(legacy_input_console_keys),
		.reserved = {0u},
	};
	config.controller_poll_limit = CONFIG_X86_NATIVE_I8042_POLL_LIMIT;
	config.input_write_poll_limit =
		CONFIG_X86_LEGACY_INPUT_WRITE_POLL_LIMIT;
	config.negotiation_step_limit =
		CONFIG_X86_LEGACY_INPUT_NEGOTIATION_STEP_LIMIT;
	config.controller_drain_limit = CONFIG_X86_NATIVE_I8042_DRAIN_LIMIT;
	config.controller_stability_attempts =
		CONFIG_X86_NATIVE_I8042_STABILITY_ATTEMPTS;
	config.atkbd_command_write_limit =
		CONFIG_X86_LEGACY_INPUT_COMMAND_WRITE_LIMIT;
	config.atkbd_command_nak_limit =
		CONFIG_X86_LEGACY_INPUT_COMMAND_NAK_LIMIT;
	/* The BIOS PC platform assigns an i8042 keyboard endpoint.  The control
	 * owner still proves accessibility by reading a stable command byte before
	 * any topology is published. */
	config.controller_present = 1u;
	config.keyboard_present = 1u;
	config.presence_evidence = X86_NATIVE_INPUT_EVIDENCE_PLATFORM_ASSIGNED;
	config.reserved[0] = 0u;

	x86_legacy_input_runtime_construct(&legacy_input_runtime);
	status = x86_legacy_input_runtime_prepare(&legacy_input_runtime, &config);
	if (status != X86_LEGACY_INPUT_OK)
		return false;
	status = x86_legacy_input_runtime_publish(&legacy_input_runtime,
						  config.identity);
	if (status != X86_LEGACY_INPUT_OK)
		return false;
	legacy_input_runtime_identity = config.identity;
	return true;
}

static void write_memory_summary(const struct x86_boot_info *boot_info)
{
	struct x86_guest_memory_snapshot managed;
	struct x86_memory_map_snapshot detected;

	console_write_literal("Memory: ");
	if (!x86_memory_map_query(boot_info, &detected)) {
		console_write_literal("E820 topology unavailable\n");
		return;
	}
	console_write_literal("detected ");
	console_write_u64(detected.usable_bytes >> 20u);
	console_write_literal(" MiB usable in ");
	console_write_u32(detected.usable_extent_count);
	console_write_literal(" E820 extents\nMemory: ");
	if (x86_guest_memory_runtime_query_snapshot(&managed) !=
	    X86_GUEST_MEMORY_OK) {
		console_write_literal("managed pool unavailable, identity map ");
		console_write_u32(x86_paging_identity_limit() /
				  (1024u * 1024u));
		console_write_literal(" MiB\n");
		return;
	}
	console_write_literal("managed ");
	console_write_u64((managed.total_free_pages * X86_GUEST_PAGE_BYTES) >>
			  20u);
	console_write_literal(" MiB, high ");
	console_write_hex64(managed.highest_address);
	console_write_literal(", identity map ");
	console_write_u32(x86_paging_identity_limit() / (1024u * 1024u));
	console_write_literal(" MiB\n");
}

static bool initialize_iomgr_exec_adapter(void)
{
	kernel_object_handle_t adapter_identity;
	kernel_object_handle_t adapter_context;

	return kernel_object_identity_allocate(&object_id_source,
					       &adapter_identity) ==
			KERNEL_OBJECT_IDENTITY_OK &&
	       kernel_object_identity_allocate(&object_id_source,
					       &adapter_context) ==
			KERNEL_OBJECT_IDENTITY_OK &&
	       iomgr_exec_adapter_initialize(adapter_identity, adapter_context,
					     system_volume) ==
			IOMGR_EXEC_ADAPTER_READY;
}

static bool initialize_dos_exec_adapters(
	const struct dos_int21_drive_config *drives)
{
	kernel_object_handle_t gate_identity;
	kernel_object_handle_t gate_context;
	kernel_object_handle_t sft_identity;
	kernel_object_handle_t sft_context;
	kernel_object_handle_t drive_identity;
	kernel_object_handle_t drive_context;

	if (!dos_int21_drive_config_is_valid(drives) ||
	    kernel_object_identity_allocate(&object_id_source, &gate_identity) !=
		KERNEL_OBJECT_IDENTITY_OK ||
	    kernel_object_identity_allocate(&object_id_source, &gate_context) !=
		KERNEL_OBJECT_IDENTITY_OK ||
	    kernel_object_identity_allocate(&object_id_source, &sft_identity) !=
		KERNEL_OBJECT_IDENTITY_OK ||
	    kernel_object_identity_allocate(&object_id_source, &sft_context) !=
		KERNEL_OBJECT_IDENTITY_OK ||
	    kernel_object_identity_allocate(&object_id_source, &drive_identity) !=
		KERNEL_OBJECT_IDENTITY_OK ||
	    kernel_object_identity_allocate(&object_id_source, &drive_context) !=
		KERNEL_OBJECT_IDENTITY_OK ||
	    dos_exec_gate_initialize(gate_identity, gate_context) !=
		DOS_EXEC_GATE_READY ||
	    dos_sft_registry_initialize(sft_identity, sft_context) !=
		DOS_SFT_REGISTRY_READY ||
	    dos_drive_visibility_initialize(
		drive_identity, drive_context, drives->current_drive,
		drives->available_drive_mask) !=
		DOS_DRIVE_VISIBILITY_READY)
		return false;

	/* JFT 0, 1 and 2 map to one CON SFT with three references. */
	if (dos_sft_registry_install(0u, 0u, 2u, 3u) !=
	    DOS_SFT_REGISTRY_READY)
		return false;
	return true;
}

static bool bind_dos_sft_runtime(void)
{
	struct dos_sft_backend_close_ops close_ops;
	kernel_object_handle_t close_identity;
	struct dos_personality *personality = dos_runtime_owner_personality();

	if (personality == NULL ||
	    kernel_object_identity_allocate(&object_id_source, &close_identity) !=
		    KERNEL_OBJECT_IDENTITY_OK)
		return false;
	close_ops = (struct dos_sft_backend_close_ops){
		.identity = close_identity,
		.context = runtime_bindings.machine_identity,
		.close = dos_sft_runtime_close,
	};
	return dos_sft_registry_bind_backend_close(
		       dos_sft_registry_context(), &close_ops) ==
		       DOS_SFT_REGISTRY_READY &&
	       dos_int21_bind_sft_services(
		       &personality->int21, dos_sft_registry_context()) ==
		       DOS_INT21_HANDLED;
}

static bool allocate_memory_lease_identity(
	dos_memory_lease_table_identity_t *identity)
{
	kernel_object_handle_t allocated;

	if (identity == NULL ||
	    kernel_object_identity_allocate(&object_id_source, &allocated) !=
		KERNEL_OBJECT_IDENTITY_OK ||
	    allocated > (kernel_object_handle_t)(~(uint32_t)0u) ||
	    allocated == DOS_MEMORY_LEASE_TABLE_IDENTITY_INVALID)
		return false;
	*identity = (dos_memory_lease_table_identity_t)allocated;
	return true;
}

static bool initialize_dos_runtime_owner(
	const struct dos_int21_drive_config *drives)
{
	struct dos_runtime_owner_config config = {0};
	const struct dos_machine *machine = x86_guest_space_machine();
	uint16_t conventional_end_segment;

	if (!dos_int21_drive_config_is_valid(drives) || machine == NULL ||
	    x86_guest_space_conventional_end_segment(
		&conventional_end_segment) != X86_GUEST_SPACE_OK ||
	    kernel_object_identity_allocate(
		&object_id_source, &config.coordinator_identity) !=
		KERNEL_OBJECT_IDENTITY_OK ||
	    kernel_object_identity_allocate(
		&object_id_source, &config.file_lease_table_identity) !=
		KERNEL_OBJECT_IDENTITY_OK ||
	    kernel_object_identity_allocate(
		&object_id_source, &config.memory_arena_identity) !=
		KERNEL_OBJECT_IDENTITY_OK ||
	    kernel_object_identity_allocate(
		&object_id_source, &config.backend_session_table_identity) !=
		KERNEL_OBJECT_IDENTITY_OK ||
	    kernel_object_identity_allocate(&object_id_source,
				      &config.runtime_identity) !=
		KERNEL_OBJECT_IDENTITY_OK ||
	    kernel_object_identity_allocate(&object_id_source,
				      &config.personality_identity) !=
		KERNEL_OBJECT_IDENTITY_OK ||
	    !allocate_memory_lease_identity(
		&config.memory_lease_table_identity))
		return false;
	config.arena_head_segment = DOS_RUNTIME_DEFAULT_ARENA_HEAD_SEGMENT;
	config.arena_end_segment = conventional_end_segment;
	config.drives = *drives;
	runtime_bindings = (struct dos_runtime_owner_bindings){
		.machine = machine,
		.file_ops = iomgr_exec_adapter_ops(),
		.observer_ops = dos_exec_gate_ops(),
		.sft_ops = dos_sft_registry_ops(),
		.drive_ops = dos_drive_visibility_ops(),
		.backend_ops = x86_vm86_backend_ops(),
		.machine_identity = x86_guest_space_machine_identity(),
		.file_adapter_context = iomgr_exec_adapter_context(),
		.observer_adapter_context = dos_exec_gate_context(),
		.sft_adapter_context = dos_sft_registry_context(),
		.drive_adapter_context = dos_drive_visibility_context(),
		.backend_adapter_context = x86_vm86_backend_context(),
	};
	return dos_runtime_owner_initialize(&config, &runtime_bindings) ==
	       DOS_RUNTIME_OWNER_READY;
}

static bool initialize_dos_ems(void)
{
	struct x86_ems_runtime_binding prepared_binding = {0};
	struct dos_personality *personality = dos_runtime_owner_personality();
	kernel_object_handle_t memory_owner;
	kernel_object_handle_t device_identity;
	kernel_object_handle_t device_context;
	enum x86_ems_runtime_config_status config_status;
	enum dos_ems_publication_status publication_status;

	if (personality == NULL)
		return false;
	/* No real 4x16 KiB page-frame mapper is bound yet.  A policy candidate
	 * alone must not publish INT 67h or EMMXXXX0. */
	config_status = x86_ems_runtime_config_resolve(
		NULL, KERNEL_OBJECT_HANDLE_INVALID, &prepared_binding);
	if (config_status == X86_EMS_RUNTIME_CONFIG_UNAVAILABLE ||
	    config_status == X86_EMS_RUNTIME_CONFIG_CONFLICT)
		return true;
	if (config_status != X86_EMS_RUNTIME_CONFIG_READY)
		return false;
	if (
	    kernel_object_identity_allocate(&object_id_source, &memory_owner) !=
		    KERNEL_OBJECT_IDENTITY_OK ||
	    kernel_object_identity_allocate(&object_id_source, &device_identity) !=
		    KERNEL_OBJECT_IDENTITY_OK ||
	    kernel_object_identity_allocate(&object_id_source, &device_context) !=
		    KERNEL_OBJECT_IDENTITY_OK)
		goto release_binding;
	/* No protected-execution backend is bound yet: keep VCPI undiscoverable. */
	publication_status = dos_ems_runtime_publish(
		personality, x86_ems_memory_runtime_operations(), memory_owner,
		&prepared_binding.page_frame, NULL,
		KERNEL_OBJECT_HANDLE_INVALID, &prepared_binding.config,
		device_identity, device_context, &ems_device_registration);
	if (publication_status == DOS_EMS_PUBLICATION_READY) {
		ems_runtime_binding = prepared_binding;
		return true;
	}
	if (x86_ems_runtime_config_release(&prepared_binding) !=
	    X86_EMS_RUNTIME_CONFIG_READY)
		return false;
	return publication_status == DOS_EMS_PUBLICATION_CONFLICT;

release_binding:
	if (x86_ems_runtime_config_release(&prepared_binding) !=
	    X86_EMS_RUNTIME_CONFIG_READY)
		return false;
	return false;
}

static bool execution_step_may_release_display(uint32_t status)
{
	return status == (uint32_t)DOS_EXECUTION_STEP_SERVICE_RESUMED ||
	       status == (uint32_t)DOS_EXECUTION_STEP_PORT_RESUMED ||
	       status == (uint32_t)DOS_EXECUTION_STEP_HALTED ||
	       status == (uint32_t)DOS_EXECUTION_STEP_CHAIN_RESUMED ||
	       status == (uint32_t)DOS_EXECUTION_STEP_PROCESS_EXITED ||
	       status == (uint32_t)DOS_EXECUTION_STEP_CHILD_STARTED;
}

struct external_command_frame {
	struct dos_exec_backend_session_handle parent_session;
	struct dos_process_runtime_snapshot parent_runtime;
	uint16_t child_psp;
};

/*
 * Release one generation-bound backend session regardless of whether its
 * adapter was already stopped by an earlier cleanup attempt.  The bounded
 * session table, rather than a second local nesting constant, is the owner of
 * the maximum simultaneously live EXEC depth.
 */
static bool stop_and_retire_backend_session(
	struct dos_exec_backend_session_table *sessions,
	struct dos_exec_backend_session_handle session)
{
	enum dos_exec_backend_session_state state;
	enum dos_exec_backend_session_status status;

	if (sessions == NULL)
		return false;
	status = dos_exec_backend_session_get_state(sessions, session, &state);
	if (status != DOS_EXEC_BACKEND_SESSION_OK)
		return false;
	if (state != DOS_EXEC_BACKEND_SESSION_STOPPED) {
		if (state != DOS_EXEC_BACKEND_SESSION_DORMANT &&
		    state != DOS_EXEC_BACKEND_SESSION_RUNNABLE &&
		    state != DOS_EXEC_BACKEND_SESSION_EXITED)
			return false;
		status = dos_exec_backend_session_stop(
			sessions, session, runtime_bindings.backend_ops,
			runtime_bindings.backend_adapter_context);
		if (status != DOS_EXEC_BACKEND_SESSION_OK)
			return false;
	}
	return dos_exec_backend_session_retire(sessions, session) ==
	       DOS_EXEC_BACKEND_SESSION_OK;
}

static enum shell_external_run_status run_external_dos_program(
	const uint8_t *path, size_t path_length, const uint8_t *command_tail,
	size_t command_tail_length)
{
	struct dos_exec_transaction_services services;
	struct dos_process_runtime_snapshot parent_runtime;
	struct dos_process_runtime_snapshot step_parent_runtime;
	struct dos_exec_native_request request = {
		.executable_name = path,
		.executable_name_length = path_length,
		.command_tail = command_tail,
		.command_tail_capacity = command_tail_length,
		.command_tail_length = command_tail_length,
	};
	struct dos_exec_native_result native_result;
	struct dos_termination_services termination_services;
	struct dos_termination_result termination_result;
	struct dos_execution_step_result step;
	struct dos_execution_exec_binding exec_binding;
	struct x86_guest_fault_snapshot fault_snapshot = {0};
	struct external_command_frame
		command_frames[DOS_EXEC_BACKEND_SESSION_SLOT_COUNT - 1u];
	size_t command_depth = 0u;
	size_t cleanup_depth;
	struct dos_personality *personality;
	struct dos_exec_backend_session_table *sessions;
	struct dos_exec_backend_session_handle session;
	struct dos_exec_backend_session_handle orphan_session = {0};
	enum dos_termination_status termination_status;
	enum shell_external_run_status run_status = SHELL_EXTERNAL_RUN_FAULT;
	bool cleanup_ok;
	bool display_foreground_owned = false;
	bool fault_ui_pending = false;
	bool guest_input_focused = false;
	bool orphan_session_owned = false;
	bool process_tree_recoverable = true;
	uint16_t child_psp;
	uint8_t exit_code = 0u;
	uint8_t exit_type = 0u;
	uint8_t last_chained_vector = 0u;
	x86_io_foreground_token_t display_token;

	if (dos_runtime_owner_borrow_exec_services(
		    &runtime_bindings, &services) != DOS_RUNTIME_OWNER_READY ||
	    dos_process_runtime_snapshot(services.runtime, &parent_runtime) !=
		    DOS_PROCESS_RUNTIME_OK)
		return SHELL_EXTERNAL_RUN_FAULT;
	if (dos_exec_native_execute(
		    dos_runtime_owner_transactions(), &services, &request,
		    (struct dos_process_far_address){0u, 0u},
		    &native_result) != DOS_EXEC_NATIVE_OK ||
	    native_result.executor.has_session != 1u) {
		console_write_literal("EXEC diagnostic: primary=");
		console_write_u32(native_result.executor.primary_status);
		console_write_literal(" cleanup=");
		console_write_u32(native_result.executor.cleanup_status);
		console_write_literal(" detail=");
		console_write_u32(native_result.executor.failure_detail);
		console_putc('\n');
		return SHELL_EXTERNAL_RUN_DOS_ERROR;
	}

	session = native_result.executor.session;
	child_psp = services.runtime->current_psp;
	sessions = dos_runtime_owner_sessions();
	personality = dos_runtime_owner_personality();
	termination_services = (struct dos_termination_services){
		.runtime = services.runtime,
		.memory_arena = services.memory_arena,
		.machine = services.machine,
		.sft_ops = services.sft_ops,
		.machine_identity = services.machine_identity,
		.sft_context = services.sft_adapter_context,
	};
	if (sessions == NULL || personality == NULL || child_psp == 0u) {
		process_tree_recoverable = false;
		if (dos_process_runtime_poison(services.runtime) !=
		    DOS_PROCESS_RUNTIME_OK)
			run_status = SHELL_EXTERNAL_RUN_FAULT;
		goto stop_session;
	}
	exec_binding = (struct dos_execution_exec_binding){
		.transactions = dos_runtime_owner_transactions(),
		.services = &services,
		.execute = dos_exec_int21_execute,
	};
	if (x86_legacy_input_runtime_focus_guest(
		    &legacy_input_runtime, legacy_input_runtime_identity) !=
	    X86_LEGACY_INPUT_OK)
		goto stop_session;
	guest_input_focused = true;

	for (;;) {
		struct x86_legacy_input_pump_result input_pump;
		enum x86_legacy_input_status input_status =
			x86_legacy_input_runtime_pump(
				&legacy_input_runtime, legacy_input_runtime_identity,
				CONFIG_X86_LEGACY_INPUT_PUMP_BUDGET, &input_pump);

		/* RETRY means the guest must run and drain its virtual controller before
		 * queued input can advance.  Other non-success values are ownership or
		 * lifecycle failures and cannot be hidden as ordinary backpressure. */
		if (input_status != X86_LEGACY_INPUT_OK &&
		    input_status != X86_LEGACY_INPUT_RETRY)
			goto stop_session;
		if (dos_process_runtime_snapshot(services.runtime,
					 &step_parent_runtime) !=
		    DOS_PROCESS_RUNTIME_OK)
			goto stop_session;
		display_token = X86_IO_FOREGROUND_TOKEN_INVALID;
		if (x86_guest_space_display_foreground_acquire(
			    runtime_bindings.machine_identity, session.value,
			    &display_token) != X86_GUEST_SPACE_OK)
			goto stop_session;
		display_foreground_owned = true;
		step = dos_execution_step_with_exec(
			sessions, session, runtime_bindings.backend_ops,
			runtime_bindings.backend_adapter_context,
			runtime_bindings.machine_identity,
			runtime_bindings.machine, personality, &exec_binding);
		if (execution_step_may_release_display(step.status)) {
			if (x86_guest_space_display_foreground_release(
				    session.value, display_token) !=
			    X86_GUEST_SPACE_OK) {
				if (x86_guest_space_display_foreground_revoke(
					    runtime_bindings.machine_identity) !=
				    X86_GUEST_SPACE_OK)
					run_status = SHELL_EXTERNAL_RUN_FAULT;
				else
					display_foreground_owned = false;
				goto stop_session;
			}
			display_foreground_owned = false;
		} else {
			if (x86_guest_space_display_foreground_revoke(
				    runtime_bindings.machine_identity) !=
			    X86_GUEST_SPACE_OK)
				goto stop_session;
			display_foreground_owned = false;
		}
		multiplex_call_diagnostic(&step);
		if (step.status ==
			    (uint32_t)DOS_EXECUTION_STEP_SERVICE_RESUMED ||
		    step.status ==
			    (uint32_t)DOS_EXECUTION_STEP_PORT_RESUMED)
			continue;
		if (step.status == (uint32_t)DOS_EXECUTION_STEP_HALTED) {
			/* Called with supervisor IF clear.  STI/HLT closes the check-to-
			 * sleep race and returns with IF clear; the backend alone decides
			 * whether a modeled, unmasked guest IRQ may retire guest HLT. */
			keyboard_console_x86_wait(NULL);
			continue;
		}
		if (step.status ==
		    (uint32_t)DOS_EXECUTION_STEP_CHAIN_RESUMED) {
			last_chained_vector = step.event.vector;
			continue;
		}
		if (step.status == (uint32_t)DOS_EXECUTION_STEP_PROCESS_EXITED) {
			exit_code = dos_register_high8(step.state.eax) == 0x4cu
					    ? dos_register_low8(step.state.eax)
					    : 0u;
			if (command_depth == 0u) {
				run_status = SHELL_EXTERNAL_RUN_OK;
				break;
			}
			if (!stop_and_retire_backend_session(sessions, session))
				goto stop_session;
			--command_depth;
			session =
				command_frames[command_depth].parent_session;
			termination_status = dos_termination_execute(
				&termination_services,
				&command_frames[command_depth].parent_runtime,
				command_frames[command_depth].child_psp,
				&termination_result);
			if (termination_status != DOS_TERMINATION_OK ||
			    dos_int21_publish_child_return(&personality->int21, 0u,
							   exit_code) !=
					    DOS_INT21_HANDLED)
				goto stop_session;
			continue;
		}
		if (step.status == (uint32_t)DOS_EXECUTION_STEP_CHILD_STARTED) {
			if (command_depth >= ARRAY_SIZE(command_frames) ||
			    services.runtime->current_psp == 0u) {
				/* CHILD_STARTED owns this handle even when the DOS
				 * process tree is inconsistent.  Quarantine the tree,
				 * but still retire every backend generation below. */
				orphan_session = step.child_session;
				orphan_session_owned = true;
				process_tree_recoverable = false;
				if (dos_process_runtime_poison(services.runtime) !=
				    DOS_PROCESS_RUNTIME_OK)
					run_status = SHELL_EXTERNAL_RUN_FAULT;
				goto stop_session;
			}
			exec_child_diagnostic(services.runtime->current_psp);
			command_frames[command_depth].parent_session = session;
			command_frames[command_depth].parent_runtime =
				step_parent_runtime;
			command_frames[command_depth].child_psp =
				services.runtime->current_psp;
			++command_depth;
			session = step.child_session;
			continue;
		}
#if CONFIG_X86_VM_ACCEPTANCE_DIAGNOSTICS
		console_write_literal("X86 execution diagnostic: step=");
		console_write_u32(step.status);
		console_write_literal(" session=");
		console_write_u32(step.session_status);
		console_write_literal(" event=");
		console_write_u32(step.event.kind);
		console_write_literal(" vector=");
		console_write_u32(step.event.vector);
		console_write_literal(" port=");
		console_write_u32(step.event.port);
		console_write_literal(" width=");
		console_write_u32(step.event.io_width);
		console_write_literal(" write=");
		console_write_u32(step.event.io_write);
		console_write_literal(" value=");
		console_write_u32(step.event.value);
		console_write_literal(" machine=");
		console_write_u32(step.interrupt.machine_status);
		console_write_literal(" last-chain=");
		console_write_u32(last_chained_vector);
		console_write_literal(" cs=");
		console_write_u32(step.state.cs);
		console_write_literal(" ip=");
		console_write_u32(step.state.eip);
		console_write_literal(" ax=");
		console_write_u32(dos_register_low16(step.state.eax));
		console_write_literal(" bx=");
		console_write_u32(dos_register_low16(step.state.ebx));
		console_write_literal(" cx=");
		console_write_u32(dos_register_low16(step.state.ecx));
		console_write_literal(" dx=");
		console_write_u32(dos_register_low16(step.state.edx));
		console_write_literal(" si=");
		console_write_u32(dos_register_low16(step.state.esi));
		console_write_literal(" di=");
		console_write_u32(dos_register_low16(step.state.edi));
		console_write_literal(" ds=");
		console_write_u32(step.state.ds);
		console_write_literal(" es=");
		console_write_u32(step.state.es);
		console_putc('\n');
#endif
		if (step.status != (uint32_t)DOS_EXECUTION_STEP_BLOCKED) {
			fault_snapshot = (struct x86_guest_fault_snapshot){
				.cpu = step.state,
				.step_status = step.status,
				.session_status = step.session_status,
				.event_kind = step.event.kind,
				.machine_status = step.interrupt.machine_status,
				.port = step.event.port,
				.vector = step.event.vector,
				.last_chained_vector = last_chained_vector,
				.io_width = step.event.io_width,
				.io_write = step.event.io_write != 0u,
			};

			fault_ui_pending = true;
			run_status = SHELL_EXTERNAL_RUN_FAULT_REPORTED;
		} else {
			run_status = SHELL_EXTERNAL_RUN_BLOCKED;
		}
		exit_type = 1u;
		exit_code = 0xffu;
		break;
	}
stop_session:
	cleanup_ok = true;
	if (display_foreground_owned) {
		if (x86_guest_space_display_foreground_revoke(
			    runtime_bindings.machine_identity) !=
		    X86_GUEST_SPACE_OK)
			cleanup_ok = false;
		else
			display_foreground_owned = false;
	}
	if (guest_input_focused) {
		if (x86_legacy_input_runtime_focus_console(
			    &legacy_input_runtime, legacy_input_runtime_identity) !=
		    X86_LEGACY_INPUT_OK)
			cleanup_ok = false;
		else
			guest_input_focused = false;
	}
	if (orphan_session_owned &&
	    !stop_and_retire_backend_session(sessions, orphan_session))
		cleanup_ok = false;
	if (!stop_and_retire_backend_session(sessions, session))
		cleanup_ok = false;
	for (cleanup_depth = command_depth; cleanup_depth != 0u;
	     --cleanup_depth) {
		if (!stop_and_retire_backend_session(
			    sessions,
			    command_frames[cleanup_depth - 1u].parent_session))
			cleanup_ok = false;
	}

	/* Runtime state is restored from the deepest child toward COMMAND. */
	while (process_tree_recoverable && command_depth != 0u) {
		--command_depth;
		termination_status = dos_termination_execute(
			&termination_services,
			&command_frames[command_depth].parent_runtime,
			command_frames[command_depth].child_psp,
			&termination_result);
		if (termination_status != DOS_TERMINATION_OK) {
			cleanup_ok = false;
			process_tree_recoverable = false;
		}
	}
	if (process_tree_recoverable) {
		termination_status = dos_termination_execute(
			&termination_services, &parent_runtime, child_psp,
			&termination_result);
		if (termination_status != DOS_TERMINATION_OK)
			cleanup_ok = false;
	} else {
		cleanup_ok = false;
	}
	/* Never draw a host-owned recovery screen or return to COMMAND without
	 * proving that both display and keyboard ownership were recovered. */
	if (display_foreground_owned || guest_input_focused)
		for (;;)
			__asm__ volatile("cli; hlt");
	if (fault_ui_pending && cleanup_ok) {
		x86_guest_fault_ui_show(&fault_snapshot);
		(void)keyboard_getchar();
		console_end_x86_guest_fault_screen();
	}
	if (!cleanup_ok || personality == NULL ||
	    dos_int21_publish_child_return(&personality->int21, exit_type,
					   exit_code) != DOS_INT21_HANDLED)
		return SHELL_EXTERNAL_RUN_FAULT;
	return run_status;
}

static bool native_command_console_write(kernel_object_handle_t context,
					 const char *text, size_t count)
{
	if (context != runtime_bindings.machine_identity ||
	    (text == NULL && count != 0u))
		return false;
	console_write(text, count);
	return true;
}

static size_t native_command_console_read_line(kernel_object_handle_t context,
					       char *text, size_t capacity)
{
	if (context != runtime_bindings.machine_identity)
		return 0u;
	return keyboard_readline(text, capacity);
}

static bool native_command_console_clear(kernel_object_handle_t context)
{
	if (context != runtime_bindings.machine_identity)
		return false;
	console_clear();
	return true;
}

static bool native_command_dos_exec(
	kernel_object_handle_t context, const uint8_t *path, size_t path_length,
	const uint8_t *command_tail, size_t command_tail_length)
{
	char canonical[DOS_PATH_CAPACITY];
	size_t canonical_length;

	if (context != runtime_bindings.machine_identity || path == NULL ||
	    dos_path_canonicalize(dos_current_directory_path,
				  sizeof(dos_current_directory_path),
				  (const char *)path, path_length,
				  canonical) != DOS_PATH_OK)
		return false;
	canonical_length = strnlen(canonical, sizeof(canonical));
	return canonical_length < sizeof(canonical) &&
	       run_external_dos_program((const uint8_t *)canonical,
					canonical_length, command_tail,
					command_tail_length) ==
		       SHELL_EXTERNAL_RUN_OK;
}

static bool native_command_dos_chdir(kernel_object_handle_t context,
				     const uint8_t *path, size_t path_length)
{
	uint8_t terminated[DOS_PATH_CAPACITY];

	if (context != runtime_bindings.machine_identity || path == NULL ||
	    path_length == 0u || path_length >= sizeof(terminated) ||
	    memcpy_s(terminated, sizeof(terminated), path, path_length,
		     path_length) != MEMORY_OK)
		return false;
	terminated[path_length] = 0u;
	return dos_change_directory(context, terminated, path_length + 1u) ==
	       DOS_SUCCESS;
}

static bool native_command_dos_getcwd(kernel_object_handle_t context,
				     char *path, size_t capacity,
				     size_t *path_length)
{
	size_t length;

	if (context != runtime_bindings.machine_identity || path == NULL ||
	    path_length == NULL)
		return false;
	length = strnlen(dos_current_directory_path,
			 sizeof(dos_current_directory_path));
	if (length == sizeof(dos_current_directory_path) || length >= capacity ||
	    memcpy_s(path, capacity, dos_current_directory_path, length + 1u,
		     length + 1u) != MEMORY_OK)
		return false;
	*path_length = length;
	return true;
}

static enum x86_user_file_open_status native_command_dos_file_open(
	kernel_object_handle_t context, const uint8_t *path, size_t path_length,
	kernel_object_handle_t *file)
{
	char canonical[DOS_PATH_CAPACITY];
	struct iomgr_path requested;
	struct iomgr_node_info info;
	iomgr_file_handle_t opened;
	enum iomgr_status status;

	if (context != runtime_bindings.machine_identity || path == NULL ||
	    file == NULL)
		return X86_USER_FILE_OPEN_ERROR;
	status = resolve_dos_iomgr_path(path, path_length, canonical, &requested);
	if (status == IOMGR_OK)
		status = iomgr_open_file(system_volume, &requested, &info, &opened);
	if (status == IOMGR_NOT_FOUND || status == IOMGR_NOT_DIRECTORY)
		return X86_USER_FILE_OPEN_NOT_FOUND;
	if (status != IOMGR_OK)
		return X86_USER_FILE_OPEN_ERROR;
	*file = opened;
	return X86_USER_FILE_OPEN_OK;
}

static bool native_command_dos_file_read(
	kernel_object_handle_t context, kernel_object_handle_t file,
	uint64_t offset, uint8_t *destination, size_t capacity,
	size_t *bytes_read)
{
	return context == runtime_bindings.machine_identity &&
	       iomgr_read_file(file, offset, destination, capacity, capacity,
			       bytes_read) == IOMGR_OK;
}

static bool native_command_dos_file_close(kernel_object_handle_t context,
					  kernel_object_handle_t file)
{
	return context == runtime_bindings.machine_identity &&
	       iomgr_close_file(file) == IOMGR_OK;
}

static enum x86_user_environment_status native_command_dos_environment_get(
	kernel_object_handle_t context, const uint8_t *name, size_t name_length,
	uint32_t value_offset, uint8_t *destination, size_t capacity,
	size_t *value_length, size_t *bytes_read)
{
	struct dos_process_runtime_snapshot runtime;
	struct dos_personality *personality;
	enum dos_environment_view_status status;

	personality = dos_runtime_owner_personality();
	if (context != runtime_bindings.machine_identity || name == NULL ||
	    value_length == NULL || bytes_read == NULL || personality == NULL ||
	    dos_process_runtime_snapshot(
		    &personality->int21.process_runtime, &runtime) !=
		    DOS_PROCESS_RUNTIME_OK)
		return X86_USER_ENVIRONMENT_ERROR;
	status = dos_environment_view_read_value(
		runtime_bindings.machine, runtime.current_psp, name, name_length,
		value_offset, destination, capacity, value_length, bytes_read);
	if (status == DOS_ENVIRONMENT_VIEW_OK)
		return X86_USER_ENVIRONMENT_OK;
	if (status == DOS_ENVIRONMENT_VIEW_NOT_FOUND)
		return X86_USER_ENVIRONMENT_NOT_FOUND;
	return X86_USER_ENVIRONMENT_ERROR;
}

#if CONFIG_BOOT_SELFTESTS
static bool initialize_self_test_identities(void)
{
	return kernel_object_identity_allocate(
		       &object_id_source, &vm86_session_table_identity) ==
			KERNEL_OBJECT_IDENTITY_OK &&
	       kernel_object_identity_allocate(
		       &object_id_source, &dos_personality_identity) ==
			KERNEL_OBJECT_IDENTITY_OK &&
	       kernel_object_identity_allocate(&object_id_source,
					       &dos_runtime_identity) ==
			KERNEL_OBJECT_IDENTITY_OK &&
	       kernel_object_identity_allocate(&object_id_source,
					       &dos_memory_arena_identity) ==
			KERNEL_OBJECT_IDENTITY_OK &&
	       kernel_object_identity_allocate(&object_id_source,
					       &guest_memory_test_identity) ==
			KERNEL_OBJECT_IDENTITY_OK;
}

static bool guest_memory_boot_self_test(void)
{
	struct x86_guest_memory_lease_info info;
	x86_guest_memory_lease_t lease;
	uint64_t highest_address;
	uint32_t physical_address;
	uint32_t free_before;
	uint32_t free_after;
	uint32_t largest_pages;

	return x86_guest_memory_runtime_query_free(&free_before) ==
			X86_GUEST_MEMORY_OK &&
	       free_before != 0u &&
	       x86_guest_memory_runtime_query_capacity(
		       &largest_pages, &free_after, &highest_address) ==
			X86_GUEST_MEMORY_OK &&
	       largest_pages != 0u && free_after == free_before &&
	       highest_address >= X86_BOOT_IDENTITY_FLOOR &&
	       highest_address < x86_paging_identity_limit() &&
	       x86_guest_memory_runtime_allocate(guest_memory_test_identity, 1u,
						 &lease,
						 &physical_address) ==
			X86_GUEST_MEMORY_OK &&
	       x86_guest_memory_runtime_inspect(lease, &info) ==
			X86_GUEST_MEMORY_OK &&
	       info.owner == guest_memory_test_identity &&
	       info.physical_address == physical_address &&
	       info.page_count == 1u &&
	       x86_guest_memory_runtime_release(guest_memory_test_identity,
					       lease) == X86_GUEST_MEMORY_OK &&
	       x86_guest_memory_runtime_query_free(&free_after) ==
			X86_GUEST_MEMORY_OK &&
	       free_after == free_before;
}
#endif

void kmain(uint32_t boot_drive, const struct x86_boot_info *boot_info)
{
	struct x86_legacy_bios_snapshot platform;
	struct iomgr_boot_volume_locator boot_volume;
	kernel_object_handle_t ata_block_identity;
	block_device_handle_t boot_device;
	enum iomgr_status status;
	const ata_write_policy_t boot_ata_write_policy =
		(ata_write_policy_t)CONFIG_X86_ATA_WRITE_POLICY;
	const struct x86_user_services command_services = {
		.console_write = native_command_console_write,
		.console_read_line = native_command_console_read_line,
		.console_clear = native_command_console_clear,
		.dos_exec = native_command_dos_exec,
		.dos_chdir = native_command_dos_chdir,
		.dos_getcwd = native_command_dos_getcwd,
		.dos_file_open = native_command_dos_file_open,
		.dos_file_read = native_command_dos_file_read,
		.dos_file_close = native_command_dos_file_close,
		.dos_environment_get = native_command_dos_environment_get,
	};

	x86_paging_initialize(boot_info);
	if (boot_drive > 0xffu ||
	    x86_legacy_bios_initialize((uint8_t)boot_drive, boot_info) !=
		    X86_LEGACY_BIOS_OK ||
	    !x86_legacy_bios_snapshot(&platform) ||
	    !initialize_system_drive_namespace(platform.boot_drive))
		for (;;)
			__asm__ volatile("cli; hlt");
	if (!initialize_guest_space())
		for (;;)
			__asm__ volatile("cli; hlt");
	if (!initialize_guest_memory(boot_info))
		for (;;)
			__asm__ volatile("cli; hlt");
	if (kernel_object_identity_allocate(&object_id_source,
					    &ata_block_identity) !=
		    KERNEL_OBJECT_IDENTITY_OK ||
	    ata_block_initialize(ata_block_identity,
				 CONFIG_X86_ATA_POLL_LIMIT,
				 boot_ata_write_policy) !=
		    ATA_BLOCK_OK)
		for (;;)
			__asm__ volatile("cli; hlt");
	console_init();
	x86_runtime_initialize();
	/* Native IRQ0 schedules the supervisor.  Its typed clock quantum advances
	 * the separately owned guest PIT/PIC; native vectors are never reflected. */
	if (!prepare_legacy_irq_delivery())
		for (;;)
			__asm__ volatile("cli; hlt");
#if CONFIG_BOOT_SELFTESTS
	if (!initialize_self_test_identities())
		for (;;)
			__asm__ volatile("cli; hlt");
	if (!guest_memory_boot_self_test())
		for (;;)
			__asm__ volatile("cli; hlt");
	if (!x86_runtime_breakpoint_self_test())
		for (;;)
			__asm__ volatile("cli; hlt");
#endif
	/* Build the complete i8042 -> serio -> atkbd -> input topology while the
	 * native PIC is masked.  Only after IRQ1 has a registered owner may the PIC
	 * publish IRQ0/IRQ1 together. */
	if (!initialize_legacy_input() || !publish_legacy_irq_delivery())
		for (;;)
			__asm__ volatile("cli; hlt");
#if CONFIG_BOOT_SELFTESTS
	/* The execution backend requires the complete interrupt domain to be
	 * active.  Exercise it only after every native IRQ owner is published. */
	if (!x86_vm86_boot_self_test(vm86_session_table_identity,
				     dos_personality_identity,
				     dos_runtime_identity,
				     dos_memory_arena_identity))
		for (;;)
			__asm__ volatile("cli; hlt");
#endif
	if (!initialize_dos_exec_adapters(&system_drive_config))
		for (;;)
			__asm__ volatile("cli; hlt");
	write_ui(DOS_UI_BOOT_BANNER);
	write_ui(DOS_UI_PLATFORM_BANNER);
	write_memory_summary(boot_info);

	boot_device = BLOCK_DEVICE_HANDLE_INVALID;
	if (platform.boot_storage_status == X86_BOOT_STORAGE_OK &&
	    ata_block_resolve_boot_locator(ata_block_identity,
					  &platform.boot_device,
					  &boot_device) != ATA_BLOCK_OK)
		boot_device = BLOCK_DEVICE_HANDLE_INVALID;
	if (boot_device == BLOCK_DEVICE_HANDLE_INVALID) {
		if (platform.boot_storage_status == X86_BOOT_STORAGE_CORRUPT) {
			write_boot_volume_error(IOMGR_CORRUPT);
		} else {
			write_ui(DOS_UI_BOOT_UNSUPPORTED_DRIVE);
			console_write_u32(platform.boot_drive);
			console_write_literal(".\n");
		}
	} else {
		boot_volume = (struct iomgr_boot_volume_locator){
			.first_lba =
				platform.boot_device.boot_volume_first_lba,
			.sector_count =
				platform.boot_device.boot_volume_sector_count,
			.logical_sector_bytes =
				platform.boot_device.logical_sector_bytes,
			.reserved = 0u,
		};
		status = iomgr_initialize();
		if (status == IOMGR_OK)
			status = iomgr_device_initialize();
		if (status == IOMGR_OK)
			status = fat_driver_register();
		if (status == IOMGR_OK)
			status = iomgr_mount_boot_volume(
				boot_device, &boot_volume, &system_volume);
		if (status != IOMGR_OK) {
			write_boot_volume_error(status);
			} else if (!initialize_iomgr_exec_adapter() ||
				   !initialize_dos_runtime_owner(
					   &system_drive_config) ||
				   !initialize_dos_ems() ||
				   !bind_dos_sft_runtime() ||
				   !initialize_dos_drive_parameter_block() ||
				   dos_int21_set_console_output(
				   &dos_runtime_owner_personality()->int21,
				   dos_console_output,
				   runtime_bindings.machine_identity) !=
				   DOS_INT21_HANDLED ||
			   dos_int21_set_console_input_status(
				   &dos_runtime_owner_personality()->int21,
				   dos_console_input_status,
				   runtime_bindings.machine_identity) !=
				   DOS_INT21_HANDLED ||
			   dos_int21_set_console_input(
				   &dos_runtime_owner_personality()->int21,
				   dos_console_input_character,
				   runtime_bindings.machine_identity) !=
				   DOS_INT21_HANDLED ||
			   dos_int21_set_console_input_flush(
				   &dos_runtime_owner_personality()->int21,
				   dos_console_input_flush,
				   runtime_bindings.machine_identity) !=
				   DOS_INT21_HANDLED ||
			   dos_int21_set_file_attributes_query(
				   &dos_runtime_owner_personality()->int21,
				   dos_file_attributes,
				   runtime_bindings.machine_identity) !=
				   DOS_INT21_HANDLED ||
			   dos_int21_set_file_services(
				   &dos_runtime_owner_personality()->int21,
				   &dos_iomgr_runtime_ops,
				   runtime_bindings.machine_identity) !=
				   DOS_INT21_HANDLED ||
			   dos_int21_set_find_services(
				   &dos_runtime_owner_personality()->int21,
				   &dos_iomgr_find_ops,
				   runtime_bindings.machine_identity) !=
				   DOS_INT21_HANDLED ||
			   dos_int21_set_directory_change(
				   &dos_runtime_owner_personality()->int21,
				   dos_change_directory,
				   runtime_bindings.machine_identity) !=
				   DOS_INT21_HANDLED ||
			   dos_int21_set_directory_create(
				   &dos_runtime_owner_personality()->int21,
				   dos_create_directory,
				   runtime_bindings.machine_identity) !=
				   DOS_INT21_HANDLED ||
			   dos_int21_set_current_directory_query(
				   &dos_runtime_owner_personality()->int21,
				   dos_get_current_directory,
				   runtime_bindings.machine_identity) !=
				   DOS_INT21_HANDLED ||
			   dos_int21_set_dpb_query(
				   &dos_runtime_owner_personality()->int21,
				   dos_get_drive_parameter_block,
				   runtime_bindings.machine_identity) !=
				   DOS_INT21_HANDLED ||
			   dos_int21_set_disk_space_query(
				   &dos_runtime_owner_personality()->int21,
				   dos_get_disk_space,
				   runtime_bindings.machine_identity) !=
				   DOS_INT21_HANDLED ||
			   dos_personality_set_absolute_disk_read(
				   dos_runtime_owner_personality(),
				   dos_absolute_disk_read,
				   runtime_bindings.machine_identity) !=
				   DOS_PERSONALITY_READY ||
			   dos_personality_set_xms(
				   dos_runtime_owner_personality(),
				   x86_xms_memory_runtime_operations(),
				   runtime_bindings.machine_identity) !=
				   DOS_PERSONALITY_READY)
			for (;;)
				__asm__ volatile("cli; hlt");
	}
	if (system_volume != IOMGR_VOLUME_HANDLE_INVALID)
		load_country_file();

	if (!shell_init(system_volume, system_drive_config.current_drive))
		for (;;)
			__asm__ volatile("cli; hlt");
	if (!shell_set_directory_commit(shell_commit_directory))
		for (;;)
			__asm__ volatile("cli; hlt");
	shell_set_external_runner(run_external_dos_program);
	if (system_volume != IOMGR_VOLUME_HANDLE_INVALID)
		shell_run_autoexec();
	for (;;) {
		struct c32_image_plan command_plan;
		struct x86_user_services active_services = command_services;
		enum c32_process_load_status load_status;
		enum x86_user_run_status run_status;
		uint32_t exit_code;

		load_status = c32_process_load(
			system_volume, command_interpreter_path + 2u,
			command_interpreter_path_length - 1u, &command_plan);
		if (load_status != C32_PROCESS_LOAD_OK) {
			console_write_literal(
				"DOS-C32: invalid native COMMAND.COM; system halted.\n");
			for (;;)
				__asm__ volatile("cli; hlt");
		}
		active_services.context = runtime_bindings.machine_identity;
		run_status = x86_user_run(command_plan.entry_point,
					       command_plan.stack_top,
					       &active_services, &exit_code);
		if (run_status == X86_USER_RUN_FAULT) {
			console_write_literal(
				"DOS-C32: protected COMMAND fault; restarting.\n");
			console_clear();
			continue;
		}
		if (run_status != X86_USER_RUN_OK) {
			console_write_literal(
				"DOS-C32: protected COMMAND failed; system halted.\n");
			for (;;)
				__asm__ volatile("cli; hlt");
		}
		(void)exit_code;
	}
}

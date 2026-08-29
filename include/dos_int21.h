/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Backend-independent DOS INT 21h register dispatcher.
 *
 * Compatibility contract: preserve the 16-bit register ABI, direct-IRET calls, and
 *                 SYS_RET_OK/ERR carry/error behavior while retaining 386
 *                 upper halves
 * Safety changes: typed context, no guest-pointer casts, explicit separation
 *                 between a completed DOS error and a machine-boundary fault
 */
#ifndef DOSC32_DOS_INT21_H
#define DOSC32_DOS_INT21_H

#include "compiler.h"
#include "dos_drive.h"
#include "dos_error.h"
#include "dos_find.h"
#include "dos_machine.h"
#include "dos_memory.h"
#include "dos_nls.h"
#include "dos_process_runtime.h"
#include "types.h"

/* Program-visible compatibility level selected for legacy DOS utilities. */
#define DOS_INT21_VERSION_MAJOR 6u
#define DOS_INT21_VERSION_MINOR 23u
#define DOS_INT21_DEFAULT_OEM_NUMBER 0xffu
#define DOS_INT21_USER_NUMBER_MAXIMUM 0x00ffffffu

enum dos_int21_status {
	/* The interrupt completed; inspect CF and AX for its DOS result. */
	DOS_INT21_HANDLED = 0,
	/* A valid MS-DOS function belongs to a dispatcher not implemented yet.
	 */
	DOS_INT21_UNIMPLEMENTED,
	/* Native caller supplied an invalid or uninitialized context. */
	DOS_INT21_INVALID_ARGUMENT,
	/* The configured guest-machine boundary cannot service the request. */
	DOS_INT21_MACHINE_FAULT,
	/* Rollback failed; the execution backend must stop this machine. */
	DOS_INT21_MACHINE_POISONED,
	/* AH=00h/4Ch never resumes this process; the owner performs teardown. */
	DOS_INT21_PROCESS_EXITED
};

typedef bool (*dos_int21_output_character_fn)(
	kernel_object_handle_t context, uint8_t character);
typedef bool (*dos_int21_input_status_fn)(kernel_object_handle_t context);
typedef bool (*dos_int21_input_character_fn)(kernel_object_handle_t context,
					      uint8_t *character);
typedef bool (*dos_int21_input_flush_fn)(kernel_object_handle_t context);
typedef enum dos_error (*dos_int21_file_attributes_fn)(
	kernel_object_handle_t context, const uint8_t *path, size_t path_length,
	uint16_t *attributes);
typedef enum dos_error (*dos_int21_file_open_fn)(
	kernel_object_handle_t context, const uint8_t *path, size_t path_length,
	kernel_object_handle_t *file, uint64_t *size);
typedef enum dos_error (*dos_int21_file_create_fn)(
	kernel_object_handle_t context, const uint8_t *path, size_t path_length,
	uint16_t attributes, kernel_object_handle_t *file, uint64_t *size);
typedef enum dos_error (*dos_int21_file_read_fn)(
	kernel_object_handle_t context, kernel_object_handle_t file,
	uint64_t offset, uint8_t *destination, size_t capacity, size_t count,
	size_t *bytes_read);
typedef enum dos_error (*dos_int21_file_write_fn)(
	kernel_object_handle_t context, kernel_object_handle_t file,
	uint64_t offset, const uint8_t *source, size_t source_capacity,
	size_t count, size_t *bytes_written);
typedef enum dos_error (*dos_int21_file_get_time_fn)(
	kernel_object_handle_t context, kernel_object_handle_t file,
	uint16_t *date, uint16_t *time);
typedef enum dos_error (*dos_int21_file_set_time_fn)(
	kernel_object_handle_t context, kernel_object_handle_t file,
	uint16_t date, uint16_t time);
typedef enum dos_error (*dos_int21_file_rename_fn)(
	kernel_object_handle_t context, const uint8_t *old_path,
	size_t old_path_length, const uint8_t *new_path,
	size_t new_path_length);
typedef enum dos_error (*dos_int21_file_close_fn)(
	kernel_object_handle_t context, kernel_object_handle_t file);
typedef enum dos_error (*dos_int21_change_directory_fn)(
	kernel_object_handle_t context, const uint8_t *path,
	size_t path_length);
typedef enum dos_error (*dos_int21_create_directory_fn)(
	kernel_object_handle_t context, const uint8_t *path,
	size_t path_length);
typedef enum dos_error (*dos_int21_get_current_directory_fn)(
	kernel_object_handle_t context, uint8_t drive, uint8_t *path,
	size_t capacity, size_t *path_length);
typedef enum dos_error (*dos_int21_get_dpb_fn)(
	kernel_object_handle_t context, uint8_t drive, uint16_t *segment,
	uint16_t *offset);
struct dos_int21_disk_space {
	uint64_t total_bytes;
	uint64_t free_bytes;
	uint32_t allocation_unit_bytes;
	uint32_t reserved;
} __aligned(8);
typedef enum dos_error (*dos_int21_get_disk_space_fn)(
	kernel_object_handle_t context, uint8_t drive,
	struct dos_int21_disk_space *space);
typedef enum dos_error (*dos_int21_find_first_fn)(
	kernel_object_handle_t context, const uint8_t *path, size_t path_length,
	uint8_t attributes, struct dos_find_record *record);
typedef enum dos_error (*dos_int21_find_next_fn)(
	kernel_object_handle_t context, const struct dos_find_record *previous,
	struct dos_find_record *record);

struct dos_int21_file_ops {
	dos_int21_file_open_fn open;
	dos_int21_file_create_fn create;
	dos_int21_file_read_fn read;
	dos_int21_file_write_fn write;
	dos_int21_file_get_time_fn get_time;
	dos_int21_file_set_time_fn set_time;
	dos_int21_file_rename_fn rename;
	dos_int21_file_close_fn close;
};

struct dos_int21_find_ops {
	dos_int21_find_first_fn first;
	dos_int21_find_next_fn next;
};

/* DOS letters use zero-based indices internally and one-based ABI values. */
struct dos_int21_drive_config {
	uint32_t available_drive_mask;
	uint8_t current_drive;
	uint8_t boot_drive;
	uint8_t last_drive;
	uint8_t reserved;
} __aligned(4);

bool dos_int21_drive_config_is_valid(
	const struct dos_int21_drive_config *config) __must_check;

/*
 * Operation namespace presented to an I/O Manager character-device control
 * callback by the DOS AH=44h adapter.  A control-read returns bytes through
 * the generic output buffer.  A control-write receives bytes through the
 * generic input buffer and reports the exact consumed count through
 * bytes_returned; output bytes themselves are ignored for that operation.
 */
#define DOS_INT21_DEVICE_CONTROL_READ 0x444f532000004402ull
#define DOS_INT21_DEVICE_CONTROL_WRITE 0x444f532000004403ull

/*
 * This is native kernel state, not a guest ABI structure.  It owns snapshots
 * of the machine boundary and arena state: neither is retained as an m32
 * native data pointer.  dos_machine.context remains the canonical 64-bit
 * backend handle; dos_machine.ops is an immutable architecture code table.
 */
struct dos_int21_context {
	struct dos_machine machine;
	struct dos_memory_arena memory_arena;
	/* Single owner for AH=50h/51h/62h and prepared EXEC snapshots. */
	struct dos_process_runtime process_runtime;
	struct dos_extended_error extended_error;
	uint32_t user_number;
	uint16_t extended_error_segment;
	uint16_t extended_error_offset;
	uint16_t child_return_code;
	dos_int21_output_character_fn output_character;
	kernel_object_handle_t output_context;
	dos_int21_input_status_fn input_status;
	kernel_object_handle_t input_context;
	dos_int21_input_character_fn input_character;
	kernel_object_handle_t input_character_context;
	dos_int21_input_flush_fn input_flush;
	kernel_object_handle_t input_flush_context;
	dos_int21_file_attributes_fn file_attributes;
	kernel_object_handle_t file_attributes_context;
	const struct dos_int21_file_ops *file_ops;
	kernel_object_handle_t file_context;
	dos_int21_change_directory_fn change_directory;
	kernel_object_handle_t directory_context;
	dos_int21_create_directory_fn create_directory;
	kernel_object_handle_t create_directory_context;
	dos_int21_get_current_directory_fn get_current_directory;
	kernel_object_handle_t current_directory_context;
	dos_int21_get_dpb_fn get_dpb;
	kernel_object_handle_t dpb_context;
	dos_int21_get_disk_space_fn get_disk_space;
	kernel_object_handle_t disk_space_context;
	const struct dos_int21_find_ops *find_ops;
	kernel_object_handle_t find_context;
	/* Authority for every JFT byte is the generation-checked DOS SFT. */
	kernel_object_handle_t sft_context;
	struct dos_nls_runtime nls;
	uint32_t available_drive_mask;
	uint8_t oem_number;
	uint8_t current_drive;
	uint8_t boot_drive;
	uint8_t last_drive;
	bool sft_services_bound;
	bool interim_console_mode;
	bool break_enabled;
	bool machine_poisoned;
	bool initialized;
};

/* runtime_identity names this native context lifetime; it is not a PSP. */
enum dos_int21_status dos_int21_context_initialize(
    struct dos_int21_context *context, const struct dos_machine *machine,
    const struct dos_memory_arena *memory_arena,
    kernel_object_handle_t runtime_identity, uint16_t current_psp,
    const struct dos_int21_drive_config *drive_config) __must_check;

enum dos_int21_status
dos_int21_set_current_psp(struct dos_int21_context *context,
			  uint16_t current_psp) __must_check;

/* Binds the native standard-output device used by AH=02h and AH=09h. */
enum dos_int21_status dos_int21_set_console_output(
	struct dos_int21_context *context,
	dos_int21_output_character_fn output_character,
	kernel_object_handle_t output_context) __must_check;

enum dos_int21_status dos_int21_set_console_input_status(
	struct dos_int21_context *context,
	dos_int21_input_status_fn input_status,
	kernel_object_handle_t input_context) __must_check;

enum dos_int21_status dos_int21_set_console_input(
	struct dos_int21_context *context,
	dos_int21_input_character_fn input_character,
	kernel_object_handle_t input_context) __must_check;

/* Binds the device-level input discard used by INT 21h/AH=0Ch. */
enum dos_int21_status dos_int21_set_console_input_flush(
	struct dos_int21_context *context,
	dos_int21_input_flush_fn input_flush,
	kernel_object_handle_t input_context) __must_check;

enum dos_int21_status dos_int21_set_file_attributes_query(
	struct dos_int21_context *context,
	dos_int21_file_attributes_fn file_attributes,
	kernel_object_handle_t file_attributes_context) __must_check;

enum dos_int21_status dos_int21_set_file_services(
	struct dos_int21_context *context,
	const struct dos_int21_file_ops *file_ops,
	kernel_object_handle_t file_context) __must_check;

/* Binds all DOS handles to the one process-global SFT authority. */
enum dos_int21_status dos_int21_bind_sft_services(
	struct dos_int21_context *context,
	kernel_object_handle_t sft_context) __must_check;

enum dos_int21_status dos_int21_set_directory_change(
	struct dos_int21_context *context,
	dos_int21_change_directory_fn change_directory,
	kernel_object_handle_t directory_context) __must_check;

enum dos_int21_status dos_int21_set_directory_create(
	struct dos_int21_context *context,
	dos_int21_create_directory_fn create_directory,
	kernel_object_handle_t directory_context) __must_check;

enum dos_int21_status dos_int21_set_current_directory_query(
	struct dos_int21_context *context,
	dos_int21_get_current_directory_fn get_current_directory,
	kernel_object_handle_t directory_context) __must_check;

enum dos_int21_status dos_int21_set_dpb_query(
	struct dos_int21_context *context, dos_int21_get_dpb_fn get_dpb,
	kernel_object_handle_t dpb_context) __must_check;

enum dos_int21_status dos_int21_set_disk_space_query(
	struct dos_int21_context *context,
	dos_int21_get_disk_space_fn get_disk_space,
	kernel_object_handle_t disk_space_context) __must_check;

enum dos_int21_status dos_int21_set_find_services(
	struct dos_int21_context *context,
	const struct dos_int21_find_ops *find_ops,
	kernel_object_handle_t find_context) __must_check;

enum dos_int21_status
dos_int21_set_version_identity(struct dos_int21_context *context,
			       uint8_t oem_number,
			       uint32_t user_number) __must_check;

void dos_int21_set_extended_error_pointer(struct dos_int21_context *context,
					  uint16_t segment, uint16_t offset);

/* Publishes the AX tuple consumed once by AH=4Dh after child teardown. */
enum dos_int21_status dos_int21_publish_child_return(
	struct dos_int21_context *context, uint8_t exit_type,
	uint8_t exit_code) __must_check;

/*
 * DOS failures are successful dispatcher completions: DOS_INT21_HANDLED is
 * returned and the guest observes CF=1 with mapped AX.  A valid but not-yet
 * unimplemented MS-DOS table entry returns DOS_INT21_UNIMPLEMENTED with both
 * registers and context unchanged so another dispatcher can handle it.
 * Native misuse and a structurally unusable machine boundary never
 * masquerade as DOS errors and leave the supplied register state unchanged.
 */
enum dos_int21_status
dos_int21_dispatch(struct dos_int21_context *context,
		   struct dos_cpu_state *registers) __must_check;

static_assert_expression(sizeof(struct dos_int21_drive_config) == 8u,
	"INT 21h drive configuration layout changed");

#endif

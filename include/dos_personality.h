/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Shared MS-DOS service personality
 *
 * Compatibility contract: one implementation of DOS interrupt behavior is shared by
 *                 native, VM86, and emulated execution backends
 */
#ifndef DOSC32_DOS_PERSONALITY_H
#define DOSC32_DOS_PERSONALITY_H

#include "compiler.h"
#include "dos_ems.h"
#include "dos_error.h"
#include "dos_int21.h"
#include "dos_machine.h"
#include "dos_xms.h"
#include "types.h"

enum dos_personality_status {
	DOS_PERSONALITY_READY = 0,
	DOS_PERSONALITY_INVALID_ARGUMENT,
	DOS_PERSONALITY_MACHINE_FAULT
};

enum dos_absolute_disk_status {
	DOS_ABSOLUTE_DISK_OK = 0,
	DOS_ABSOLUTE_DISK_BAD_DRIVE,
	DOS_ABSOLUTE_DISK_OUT_OF_RANGE,
	DOS_ABSOLUTE_DISK_IO_ERROR
};

typedef enum dos_absolute_disk_status (*dos_absolute_disk_read_fn)(
	kernel_object_handle_t context, uint8_t drive, uint32_t sector,
	uint8_t *destination, size_t capacity);

enum dos_bios_keyboard_status {
	DOS_BIOS_KEYBOARD_AVAILABLE = 0,
	DOS_BIOS_KEYBOARD_EMPTY,
	DOS_BIOS_KEYBOARD_FAULT
};

typedef enum dos_bios_keyboard_status (*dos_bios_keyboard_read_fn)(
	kernel_object_handle_t context, bool wait, uint16_t *key);
typedef uint16_t (*dos_bios_keyboard_shift_fn)(
	kernel_object_handle_t context);

/*
 * Native DOS-service state shared by every execution backend.  This object is
 * never guest-visible and retains no pointer into guest memory.  The machine
 * code table is borrowed for the constructed lifetime; its integer context
 * and explicit identity bind calls to the same guest boundary used by EXEC.
 */
struct dos_personality {
	struct dos_int21_context int21;
	kernel_object_handle_t identity;
	kernel_object_handle_t machine_identity;
	dos_absolute_disk_read_fn absolute_disk_read;
	kernel_object_handle_t absolute_disk_context;
	dos_bios_keyboard_read_fn bios_keyboard_read;
	dos_bios_keyboard_shift_fn bios_keyboard_shift;
	kernel_object_handle_t bios_keyboard_context;
	struct dos_xms_manager xms;
	struct dos_ems_manager ems;
	struct dos_ems_runtime_config ems_config;
	uint8_t initialized;
	uint8_t reserved[7];
} __aligned(8);

enum dos_interrupt_disposition {
	DOS_INTERRUPT_HANDLED = 0,
	DOS_INTERRUPT_CHAIN,
	DOS_INTERRUPT_BLOCKED,
	DOS_INTERRUPT_PROCESS_EXITED,
	DOS_INTERRUPT_MACHINE_FAULT,
	/* A protected-execution handoff consumed the old interrupt frame. */
	DOS_INTERRUPT_EXECUTION_TRANSFERRED
};

struct dos_interrupt_result {
	enum dos_interrupt_disposition disposition;
	enum dos_machine_status machine_status;
};

enum dos_personality_status dos_personality_initialize(
	struct dos_personality *personality,
	kernel_object_handle_t personality_identity,
	kernel_object_handle_t machine_identity,
	const struct dos_machine *machine,
	const struct dos_memory_arena *memory_arena,
	kernel_object_handle_t runtime_identity,
	uint16_t current_psp,
	const struct dos_int21_drive_config *drive_config) __must_check;

enum dos_personality_status dos_personality_set_absolute_disk_read(
	struct dos_personality *personality,
	dos_absolute_disk_read_fn absolute_disk_read,
	kernel_object_handle_t absolute_disk_context) __must_check;
enum dos_personality_status dos_personality_set_bios_keyboard(
	struct dos_personality *personality,
	dos_bios_keyboard_read_fn keyboard_read,
	dos_bios_keyboard_shift_fn keyboard_shift,
	kernel_object_handle_t keyboard_context) __must_check;
enum dos_personality_status dos_personality_set_xms(
	struct dos_personality *personality,
	const struct dos_xms_memory_ops *memory_ops,
	kernel_object_handle_t memory_context) __must_check;
enum dos_personality_status dos_personality_set_ems(
	struct dos_personality *personality,
	const struct dos_ems_page_ops *page_ops,
	kernel_object_handle_t page_context,
	const struct dos_ems_page_frame_binding *page_frame,
	const struct dos_vcpi_platform_ops *vcpi_ops,
	kernel_object_handle_t vcpi_context,
	const struct dos_ems_runtime_config *config) __must_check;
/* Publishes a fully initialized, allocation-free EMS manager atomically. */
enum dos_personality_status dos_personality_publish_ems(
	struct dos_personality *personality,
	const struct dos_ems_manager *prepared,
	const struct dos_ems_runtime_config *config) __must_check;
/* Output is unchanged unless a healthy initialized EMS configuration exists. */
bool dos_personality_ems_config_snapshot(
	const struct dos_personality *personality,
	struct dos_ems_runtime_config *config) __must_check;

/*
 * Dispatches a software interrupt from a guest.  A handled DOS failure is not
 * a C failure: the service places the primary DOS error in AX, sets CF, and
 * returns DOS_INTERRUPT_HANDLED.
 */
struct dos_interrupt_result dos_personality_interrupt(
	struct dos_personality *personality, const struct dos_machine *machine,
	kernel_object_handle_t machine_identity, uint8_t vector,
	struct dos_cpu_state *state) __must_check;

enum dos_error dos_personality_create_process(
	struct dos_personality *personality, struct dos_machine *machine,
	uint16_t parent_psp, uint16_t image_psp,
	uint16_t allocation_paragraphs) __must_check;
enum dos_error dos_personality_destroy_process(
	struct dos_personality *personality, struct dos_machine *machine,
	uint16_t process_psp, uint8_t exit_type, uint8_t exit_code) __must_check;

#endif

// SPDX-License-Identifier: GPL-2.0-only
/*
 * Backend-independent DOS software-interrupt personality.
 *
 * Compatibility contract: INT 21h is owned by the DOS dispatcher; an untranslated
 *                 valid function must stop explicitly rather than fabricate
 *                 BadCall or silently execute an IVT handler
 */
#include "dos_personality.h"

#ifndef CONFIG_DOS_XMS_HMA_MINIMUM_BYTES
#error "CONFIG_DOS_XMS_HMA_MINIMUM_BYTES must come from config/xms.mk"
#endif

#if CONFIG_DOS_XMS_HMA_MINIMUM_BYTES < 0 || \
	CONFIG_DOS_XMS_HMA_MINIMUM_BYTES > 65520u
#error "the HMA minimum request cannot exceed the physical HMA"
#endif

#define DOS_ABSOLUTE_READ_VECTOR 0x25u
#define DOS_ABSOLUTE_SECTOR_BYTES 512u
#define DOS_ABSOLUTE_EXTENDED_COUNT 0xffffu
#define DOS_ABSOLUTE_PACKET_BYTES 10u
#define DOS_ABSOLUTE_ERROR_BAD_DRIVE 0x0201u
#define DOS_ABSOLUTE_ERROR_GENERAL 0x020cu
#define DOS_ABSOLUTE_ERROR_BOUNDARY 0x0209u
#define BIOS_KEYBOARD_VECTOR 0x16u
#define BIOS_KEYBOARD_READ 0x00u
#define BIOS_KEYBOARD_STATUS 0x01u
#define BIOS_KEYBOARD_SHIFT 0x02u
#define BIOS_KEYBOARD_STORE 0x05u
#define BIOS_KEYBOARD_ENHANCED_READ 0x10u
#define BIOS_KEYBOARD_ENHANCED_STATUS 0x11u
#define BIOS_KEYBOARD_ENHANCED_SHIFT 0x12u
#define DOS_MULTIPLEX_VECTOR 0x2fu
#define DOS_EMS_VECTOR 0x67u


static bool valid_identity(kernel_object_handle_t identity)
{
	return identity != 0u && identity != KERNEL_OBJECT_HANDLE_INVALID;
}

static bool machine_binding_matches(const struct dos_machine *expected,
				    const struct dos_machine *actual)
{
	return expected != NULL && actual != NULL && expected->ops != NULL &&
	       expected->ops == actual->ops &&
	       expected->context == actual->context &&
	       expected->address_limit == actual->address_limit;
}

static struct dos_interrupt_result interrupt_result(
	enum dos_interrupt_disposition disposition,
	enum dos_machine_status machine_status)
{
	struct dos_interrupt_result result = {
		.disposition = disposition,
		.machine_status = machine_status,
	};

	return result;
}

static uint16_t read_le16(const uint8_t *bytes)
{
	return (uint16_t)bytes[0] | (uint16_t)((uint16_t)bytes[1] << 8u);
}

static uint32_t read_le32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8u) |
	       ((uint32_t)bytes[2] << 16u) | ((uint32_t)bytes[3] << 24u);
}

static struct dos_interrupt_result absolute_read_return(
	struct dos_personality *personality, struct dos_cpu_state *state,
	uint16_t result_code, bool failed)
{
	uint8_t flags[2];
	uint8_t rollback[2];
	uint16_t stack_pointer =
		(uint16_t)(dos_register_low16(state->esp) - 2u);
	uint16_t original_flags = dos_register_low16(state->eflags);
	enum dos_machine_status machine_status;

	flags[0] = (uint8_t)original_flags;
	flags[1] = (uint8_t)(original_flags >> 8u);
	machine_status = dos_machine_replace_far(
		&personality->int21.machine, state->ss, stack_pointer,
		flags, sizeof(flags), rollback, sizeof(rollback), sizeof(flags));
	if (machine_status == DOS_MACHINE_ROLLBACK_FAILED) {
		personality->int21.machine_poisoned = true;
		return interrupt_result(DOS_INTERRUPT_MACHINE_FAULT,
					DOS_MACHINE_STOPPED);
	}
	if (machine_status != DOS_MACHINE_OK)
		return interrupt_result(DOS_INTERRUPT_MACHINE_FAULT,
					machine_status);
	dos_register_set_low16(&state->esp, stack_pointer);
	dos_register_set_low16(&state->eax, result_code);
	if (failed)
		state->eflags |= DOS_EFLAGS_CF;
	else
		state->eflags &= ~DOS_EFLAGS_CF;
	return interrupt_result(DOS_INTERRUPT_HANDLED, DOS_MACHINE_OK);
}

static struct dos_interrupt_result dispatch_absolute_disk_read(
	struct dos_personality *personality, struct dos_cpu_state *state)
{
	uint8_t packet[DOS_ABSOLUTE_PACKET_BYTES];
	uint8_t sector[DOS_ABSOLUTE_SECTOR_BYTES];
	uint8_t rollback[DOS_ABSOLUTE_SECTOR_BYTES];
	uint8_t drive = dos_register_low8(state->eax) & 0x7fu;
	uint16_t count = dos_register_low16(state->ecx);
	uint16_t buffer_segment = state->ds;
	uint16_t buffer_offset = dos_register_low16(state->ebx);
	uint32_t first_sector = dos_register_low16(state->edx);
	uint32_t byte_count;
	uint32_t index;
	enum dos_machine_status machine_status;

	if (personality->absolute_disk_read == NULL)
		return interrupt_result(DOS_INTERRUPT_BLOCKED,
					DOS_MACHINE_UNSUPPORTED);
	if (count == DOS_ABSOLUTE_EXTENDED_COUNT) {
		machine_status = dos_machine_read_far(
			&personality->int21.machine, state->ds, buffer_offset,
			packet, sizeof(packet), sizeof(packet));
		if (machine_status != DOS_MACHINE_OK)
			return interrupt_result(DOS_INTERRUPT_MACHINE_FAULT,
						machine_status);
		first_sector = read_le32(packet);
		count = read_le16(packet + 4u);
		buffer_offset = read_le16(packet + 6u);
		buffer_segment = read_le16(packet + 8u);
	}
	byte_count = (uint32_t)count * DOS_ABSOLUTE_SECTOR_BYTES;
	if (byte_count > 0x10000u - (uint32_t)buffer_offset)
		return absolute_read_return(personality, state,
					    DOS_ABSOLUTE_ERROR_BOUNDARY, true);
	for (index = 0u; index < (uint32_t)count; ++index) {
		enum dos_absolute_disk_status disk_status;

		if (first_sector > 0xffffffffu - index)
			return absolute_read_return(personality, state,
						    DOS_ABSOLUTE_ERROR_GENERAL,
						    true);
		disk_status = personality->absolute_disk_read(
			personality->absolute_disk_context, drive,
			first_sector + index, sector, sizeof(sector));
		if (disk_status == DOS_ABSOLUTE_DISK_BAD_DRIVE)
			return absolute_read_return(personality, state,
						    DOS_ABSOLUTE_ERROR_BAD_DRIVE,
						    true);
		if (disk_status != DOS_ABSOLUTE_DISK_OK)
			return absolute_read_return(personality, state,
						    DOS_ABSOLUTE_ERROR_GENERAL,
						    true);
		machine_status = dos_machine_replace_far(
			&personality->int21.machine, buffer_segment,
			(uint16_t)(buffer_offset +
				   (uint16_t)(index * DOS_ABSOLUTE_SECTOR_BYTES)),
			sector, sizeof(sector), rollback, sizeof(rollback),
			sizeof(sector));
		if (machine_status == DOS_MACHINE_ROLLBACK_FAILED) {
			personality->int21.machine_poisoned = true;
			return interrupt_result(DOS_INTERRUPT_MACHINE_FAULT,
						DOS_MACHINE_STOPPED);
		}
		if (machine_status != DOS_MACHINE_OK)
			return interrupt_result(DOS_INTERRUPT_MACHINE_FAULT,
						machine_status);
	}
	return absolute_read_return(personality, state, 0u, false);
}

enum dos_personality_status dos_personality_initialize(
	struct dos_personality *personality,
	kernel_object_handle_t personality_identity,
	kernel_object_handle_t machine_identity,
	const struct dos_machine *machine,
	const struct dos_memory_arena *memory_arena,
	kernel_object_handle_t runtime_identity, uint16_t current_psp,
	const struct dos_int21_drive_config *drive_config)
{
	struct dos_personality prepared = {0};
	enum dos_int21_status status;
	enum dos_xms_status xms_status;

	if (personality == NULL || machine == NULL || memory_arena == NULL ||
	    !valid_identity(personality_identity) ||
	    !valid_identity(machine_identity) || !valid_identity(runtime_identity))
		return DOS_PERSONALITY_INVALID_ARGUMENT;
	status = dos_int21_context_initialize(&prepared.int21, machine,
					      memory_arena, runtime_identity,
					      current_psp, drive_config);
	if (status == DOS_INT21_MACHINE_FAULT ||
	    status == DOS_INT21_MACHINE_POISONED)
		return DOS_PERSONALITY_MACHINE_FAULT;
	if (status != DOS_INT21_HANDLED)
		return DOS_PERSONALITY_INVALID_ARGUMENT;
	prepared.identity = personality_identity;
	prepared.machine_identity = machine_identity;
	xms_status = dos_xms_construct(&prepared.xms, personality_identity);
	if (xms_status != DOS_XMS_READY)
		return DOS_PERSONALITY_INVALID_ARGUMENT;
	dos_ems_construct(&prepared.ems);
	prepared.initialized = 1u;
	*personality = prepared;
	return DOS_PERSONALITY_READY;
}

enum dos_personality_status dos_personality_set_absolute_disk_read(
	struct dos_personality *personality,
	dos_absolute_disk_read_fn absolute_disk_read,
	kernel_object_handle_t absolute_disk_context)
{
	if (personality == NULL || !personality->initialized ||
	    absolute_disk_read == NULL ||
	    !valid_identity(absolute_disk_context))
		return DOS_PERSONALITY_INVALID_ARGUMENT;
	personality->absolute_disk_read = absolute_disk_read;
	personality->absolute_disk_context = absolute_disk_context;
	return DOS_PERSONALITY_READY;
}

enum dos_personality_status dos_personality_set_bios_keyboard(
	struct dos_personality *personality,
	dos_bios_keyboard_read_fn keyboard_read,
	dos_bios_keyboard_shift_fn keyboard_shift,
	kernel_object_handle_t keyboard_context)
{
	if (personality == NULL || !personality->initialized ||
	    keyboard_read == NULL || keyboard_shift == NULL ||
	    !valid_identity(keyboard_context))
		return DOS_PERSONALITY_INVALID_ARGUMENT;
	personality->bios_keyboard_read = keyboard_read;
	personality->bios_keyboard_shift = keyboard_shift;
	personality->bios_keyboard_context = keyboard_context;
	return DOS_PERSONALITY_READY;
}

enum dos_personality_status dos_personality_set_xms(
	struct dos_personality *personality,
	const struct dos_xms_memory_ops *memory_ops,
	kernel_object_handle_t memory_context)
{
	static const struct dos_xms_config config = {
		.hma_minimum_bytes = CONFIG_DOS_XMS_HMA_MINIMUM_BYTES,
		.reserved = {0u},
	};
	enum dos_xms_status status;

	if (personality == NULL || !personality->initialized)
		return DOS_PERSONALITY_INVALID_ARGUMENT;
	status = dos_xms_initialize(&personality->xms,
				    &personality->int21.machine, memory_ops,
				    memory_context, &config);
	if (status == DOS_XMS_MACHINE_FAULT)
		return DOS_PERSONALITY_MACHINE_FAULT;
	return status == DOS_XMS_READY ? DOS_PERSONALITY_READY
				       : DOS_PERSONALITY_INVALID_ARGUMENT;
}

enum dos_personality_status dos_personality_set_ems(
	struct dos_personality *personality,
	const struct dos_ems_page_ops *page_ops,
	kernel_object_handle_t page_context,
	const struct dos_ems_page_frame_binding *page_frame,
	const struct dos_vcpi_platform_ops *vcpi_ops,
	kernel_object_handle_t vcpi_context,
	const struct dos_ems_runtime_config *config)
{
	struct dos_ems_manager prepared;
	enum dos_ems_status status;

	if (personality == NULL || !personality->initialized ||
	    !dos_ems_runtime_config_is_valid(config))
		return DOS_PERSONALITY_INVALID_ARGUMENT;
	dos_ems_construct(&prepared);
	status = dos_ems_initialize(&prepared, page_ops, page_context, page_frame,
				    vcpi_ops, vcpi_context, &config->service);
	if (status == DOS_EMS_MEMORY_FAULT || status == DOS_EMS_POISONED)
		return DOS_PERSONALITY_MACHINE_FAULT;
	if (status != DOS_EMS_READY)
		return DOS_PERSONALITY_INVALID_ARGUMENT;
	return dos_personality_publish_ems(personality, &prepared, config);
}

enum dos_personality_status dos_personality_publish_ems(
	struct dos_personality *personality,
	const struct dos_ems_manager *prepared,
	const struct dos_ems_runtime_config *config)
{
	if (personality == NULL || personality->initialized != 1u ||
	    prepared == NULL || prepared->constructed != 1u ||
	    prepared->initialized != 1u || prepared->poisoned != 0u ||
	    prepared->allocated_handles != 0u ||
	    personality->ems.initialized != 0u ||
	    !dos_ems_runtime_config_is_valid(config) ||
	    prepared->page_frame_segment != config->service.page_frame_segment)
		return DOS_PERSONALITY_INVALID_ARGUMENT;
	personality->ems = *prepared;
	personality->ems_config = *config;
	return DOS_PERSONALITY_READY;
}

bool dos_personality_ems_config_snapshot(
	const struct dos_personality *personality,
	struct dos_ems_runtime_config *config)
{
	if (personality == NULL || config == NULL ||
	    personality->initialized != 1u ||
	    personality->ems.initialized != 1u ||
	    personality->ems.poisoned != 0u)
		return false;
	*config = personality->ems_config;
	return true;
}

static struct dos_interrupt_result xms_result(enum dos_xms_status status)
{
	if (status == DOS_XMS_READY)
		return interrupt_result(DOS_INTERRUPT_HANDLED, DOS_MACHINE_OK);
	if (status == DOS_XMS_CHAIN)
		return interrupt_result(DOS_INTERRUPT_CHAIN, DOS_MACHINE_OK);
	return interrupt_result(DOS_INTERRUPT_MACHINE_FAULT,
				status == DOS_XMS_MACHINE_FAULT
					? DOS_MACHINE_IO_FAULT
					: DOS_MACHINE_INVALID_ARGUMENT);
}

static struct dos_interrupt_result ems_result(enum dos_ems_status status)
{
	switch (status) {
	case DOS_EMS_READY:
		return interrupt_result(DOS_INTERRUPT_HANDLED, DOS_MACHINE_OK);
	case DOS_EMS_EXECUTION_TRANSFERRED:
		return interrupt_result(DOS_INTERRUPT_EXECUTION_TRANSFERRED,
					DOS_MACHINE_OK);
	case DOS_EMS_MEMORY_FAULT:
		return interrupt_result(DOS_INTERRUPT_MACHINE_FAULT,
					DOS_MACHINE_IO_FAULT);
	case DOS_EMS_POISONED:
		return interrupt_result(DOS_INTERRUPT_MACHINE_FAULT,
					DOS_MACHINE_STOPPED);
	case DOS_EMS_INVALID_ARGUMENT:
		break;
	}
	return interrupt_result(DOS_INTERRUPT_MACHINE_FAULT,
				DOS_MACHINE_INVALID_ARGUMENT);
}

static struct dos_interrupt_result dispatch_bios_keyboard(
	struct dos_personality *personality, struct dos_cpu_state *state)
{
	uint8_t function = dos_register_high8(state->eax);
	uint16_t key = 0u;
	enum dos_bios_keyboard_status status;
	bool wait;

	if (personality->bios_keyboard_read == NULL ||
	    personality->bios_keyboard_shift == NULL)
		return interrupt_result(DOS_INTERRUPT_CHAIN, DOS_MACHINE_OK);
	if (function == BIOS_KEYBOARD_SHIFT ||
	    function == BIOS_KEYBOARD_ENHANCED_SHIFT) {
		uint16_t shift = personality->bios_keyboard_shift(
			personality->bios_keyboard_context);

		if (function == BIOS_KEYBOARD_SHIFT)
			dos_register_set_low8(&state->eax, (uint8_t)shift);
		else
			dos_register_set_low16(&state->eax, shift);
		return interrupt_result(DOS_INTERRUPT_HANDLED, DOS_MACHINE_OK);
	}
	if (function == BIOS_KEYBOARD_STORE) {
		/* The compact virtual keyboard has one host-owned pending slot. */
		dos_register_set_low8(&state->eax, 1u);
		return interrupt_result(DOS_INTERRUPT_HANDLED, DOS_MACHINE_OK);
	}
	if (function != BIOS_KEYBOARD_READ &&
	    function != BIOS_KEYBOARD_STATUS &&
	    function != BIOS_KEYBOARD_ENHANCED_READ &&
	    function != BIOS_KEYBOARD_ENHANCED_STATUS)
		return interrupt_result(DOS_INTERRUPT_CHAIN, DOS_MACHINE_OK);
	wait = function == BIOS_KEYBOARD_READ ||
	       function == BIOS_KEYBOARD_ENHANCED_READ;
	status = personality->bios_keyboard_read(
		personality->bios_keyboard_context, wait, &key);
	if (status == DOS_BIOS_KEYBOARD_FAULT)
		return interrupt_result(DOS_INTERRUPT_MACHINE_FAULT,
					DOS_MACHINE_IO_FAULT);
	if (status == DOS_BIOS_KEYBOARD_EMPTY) {
		state->eflags |= DOS_EFLAGS_ZF;
		return interrupt_result(DOS_INTERRUPT_HANDLED, DOS_MACHINE_OK);
	}
	dos_register_set_low16(&state->eax, key);
	state->eflags &= ~DOS_EFLAGS_ZF;
	return interrupt_result(DOS_INTERRUPT_HANDLED, DOS_MACHINE_OK);
}

struct dos_interrupt_result dos_personality_interrupt(
	struct dos_personality *personality, const struct dos_machine *machine,
	kernel_object_handle_t machine_identity, uint8_t vector,
	struct dos_cpu_state *state)
{
	enum dos_int21_status status;

	if (personality == NULL || machine == NULL || state == NULL ||
	    !personality->initialized ||
	    !valid_identity(personality->identity) ||
	    machine_identity != personality->machine_identity ||
	    !machine_binding_matches(&personality->int21.machine, machine))
		return interrupt_result(DOS_INTERRUPT_MACHINE_FAULT,
					DOS_MACHINE_INVALID_ARGUMENT);
	if (machine->poisoned || personality->int21.machine_poisoned)
		return interrupt_result(DOS_INTERRUPT_MACHINE_FAULT,
					DOS_MACHINE_STOPPED);

	/* Follow the current A20/poison value while retaining the proven binding. */
	personality->int21.machine.a20_enabled = machine->a20_enabled;
	personality->int21.machine.poisoned = machine->poisoned;
	if (vector == DOS_ABSOLUTE_READ_VECTOR)
		return dispatch_absolute_disk_read(personality, state);
	if (vector == BIOS_KEYBOARD_VECTOR)
		return dispatch_bios_keyboard(personality, state);
	if (vector == DOS_MULTIPLEX_VECTOR)
		return xms_result(dos_xms_multiplex(&personality->xms, state));
	if (vector == DOS_XMS_CONTROL_VECTOR)
		return xms_result(dos_xms_control(
			&personality->xms, &personality->int21.machine, state));
	if (vector == DOS_EMS_VECTOR) {
		if (personality->ems.initialized != 1u)
			return interrupt_result(DOS_INTERRUPT_CHAIN,
						DOS_MACHINE_OK);
		return ems_result(dos_ems_interrupt(&personality->ems, state));
	}
	if (vector != 0x21u)
		return interrupt_result(DOS_INTERRUPT_CHAIN, DOS_MACHINE_OK);

	status = dos_int21_dispatch(&personality->int21, state);
	switch (status) {
	case DOS_INT21_HANDLED:
		return interrupt_result(DOS_INTERRUPT_HANDLED, DOS_MACHINE_OK);
	case DOS_INT21_UNIMPLEMENTED:
		/* COMMAND owns INT 21h.  Do not turn missing C code into BadCall. */
		return interrupt_result(DOS_INTERRUPT_BLOCKED,
					DOS_MACHINE_UNSUPPORTED);
	case DOS_INT21_MACHINE_POISONED:
		return interrupt_result(DOS_INTERRUPT_MACHINE_FAULT,
					DOS_MACHINE_STOPPED);
	case DOS_INT21_MACHINE_FAULT:
		return interrupt_result(DOS_INTERRUPT_MACHINE_FAULT,
					DOS_MACHINE_IO_FAULT);
	case DOS_INT21_PROCESS_EXITED:
		return interrupt_result(DOS_INTERRUPT_PROCESS_EXITED,
					DOS_MACHINE_OK);
	case DOS_INT21_INVALID_ARGUMENT:
		break;
	}
	return interrupt_result(DOS_INTERRUPT_MACHINE_FAULT,
				DOS_MACHINE_INVALID_ARGUMENT);
}

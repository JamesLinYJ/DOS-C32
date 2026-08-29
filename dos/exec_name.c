// SPDX-License-Identifier: GPL-2.0-only
/* Safe bounded executable-name acquisition and owner-name encoding. */
#include "dos_exec_name.h"

#include "overflow.h"

#define DOS_EXEC_NAME_BACKSLASH ((uint8_t)0x5cu)
#define DOS_EXEC_NAME_COLON ((uint8_t)0x3au)
#define DOS_EXEC_NAME_DOT ((uint8_t)0x2eu)

bool dos_exec_name_plan_has_valid_encoding(
    const struct dos_exec_name_plan *plan)
{
	return plan != NULL && plan->bytes_including_nul != 0u &&
	       plan->reserved == 0u;
}

enum dos_exec_name_status dos_exec_name_read_guest(
    const struct dos_machine *machine, struct dos_far_pointer16 source,
    uint8_t *scratch, size_t scratch_capacity,
    struct dos_exec_name_plan *plan)
{
	struct dos_exec_name_plan prepared;
	uintptr_t scratch_end;
	size_t scan_capacity;
	size_t scanned;

	if (machine == NULL || scratch == NULL || scratch_capacity == 0u ||
	    plan == NULL)
		return DOS_EXEC_NAME_INVALID_ARGUMENT;
	scan_capacity = scratch_capacity < DOS_EXEC_NAME_SCAN_LIMIT
			    ? scratch_capacity
			    : DOS_EXEC_NAME_SCAN_LIMIT;
	if (check_add_overflow((uintptr_t)scratch,
			       (uintptr_t)scan_capacity, &scratch_end))
		return DOS_EXEC_NAME_INVALID_ARGUMENT;
	(void)scratch_end;

	for (scanned = 0u; scanned < scan_capacity; ++scanned) {
		uint8_t byte;
		uint16_t offset =
		    (uint16_t)((uint32_t)source.offset + (uint32_t)scanned);

		if (dos_machine_read_far(machine, source.segment, offset, &byte,
					 sizeof(byte), sizeof(byte)) !=
		    DOS_MACHINE_OK)
			return DOS_EXEC_NAME_GUEST_FAULT;
		scratch[scanned] = byte;
		if (byte != 0u)
			continue;

		prepared.source.offset = source.offset;
		prepared.source.segment = source.segment;
		prepared.bytes_including_nul = (uint16_t)(scanned + 1u);
		prepared.reserved = 0u;
		plan->source.offset = prepared.source.offset;
		plan->source.segment = prepared.source.segment;
		plan->bytes_including_nul = prepared.bytes_including_nul;
		plan->reserved = prepared.reserved;
		return DOS_EXEC_NAME_OK;
	}
	return scratch_capacity < DOS_EXEC_NAME_SCAN_LIMIT
		       ? DOS_EXEC_NAME_BUFFER_TOO_SMALL
		       : DOS_EXEC_NAME_INVALID_NAME;
}

static bool byte_is_separator(uint8_t byte)
{
	return byte == DOS_EXEC_NAME_COLON || byte == DOS_EXEC_NAME_BACKSLASH;
}

enum dos_exec_name_status
dos_exec_name_build_owner_patch(const uint8_t *name, size_t capacity,
				size_t length_including_nul,
				struct dos_memory_owner_name_patch *patch)
{
	struct dos_memory_owner_name_patch prepared = {0};
	size_t basename = 0u;
	size_t copied = 0u;
	size_t index;

	if (name == NULL || patch == NULL || length_including_nul == 0u ||
	    length_including_nul > capacity)
		return DOS_EXEC_NAME_INVALID_ARGUMENT;
	if (name[length_including_nul - 1u] != 0u)
		return DOS_EXEC_NAME_INVALID_NAME;

	for (index = 0u; index < length_including_nul - 1u; ++index) {
		if (name[index] == 0u)
			return DOS_EXEC_NAME_INVALID_NAME;
		if (byte_is_separator(name[index]))
			basename = index + 1u;
	}

	for (index = basename; index < length_including_nul &&
			       copied < DOS_MEMORY_OWNER_NAME_BYTES;
	     ++index) {
		uint8_t byte = name[index];

		if (byte == 0u || byte == DOS_EXEC_NAME_DOT)
			break;
		prepared.bytes[copied] = byte;
		++copied;
	}
	if (copied < DOS_MEMORY_OWNER_NAME_BYTES) {
		prepared.bytes[copied] = 0u;
		++copied;
	}
	prepared.count = (uint8_t)copied;

	for (index = 0u; index < ARRAY_SIZE(patch->bytes); ++index)
		patch->bytes[index] = prepared.bytes[index];
	patch->count = prepared.count;
	for (index = 0u; index < ARRAY_SIZE(patch->reserved); ++index)
		patch->reserved[index] = prepared.reserved[index];
	return DOS_EXEC_NAME_OK;
}

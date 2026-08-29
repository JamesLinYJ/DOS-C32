// SPDX-License-Identifier: GPL-2.0-only
/* Safe drive-visibility resolution for the two default FCBs. */
#include "dos_drive_visibility.h"

#define DOS_DRIVE_VALID_MASK ((1u << DOS_DRIVE_COUNT) - 1u)

struct dos_drive_visibility_owner {
	kernel_object_handle_t context;
	uint32_t visible_drives;
	uint8_t current_drive;
	uint8_t initialized;
	uint8_t reserved[2];
} __aligned(8);

static struct dos_drive_visibility_owner owner;

static enum dos_exec_drive_visibility_status
resolve_drive(kernel_object_handle_t context, uint8_t drive_designator);

static struct dos_exec_drive_visibility_ops operations = {
	.identity = KERNEL_OBJECT_HANDLE_INVALID,
	.resolve = resolve_drive,
};

static bool identity_is_valid(kernel_object_handle_t identity)
{
	return identity != 0u && identity != KERNEL_OBJECT_HANDLE_INVALID;
}

enum dos_drive_visibility_adapter_status dos_drive_visibility_initialize(
	kernel_object_handle_t adapter_identity,
	kernel_object_handle_t adapter_context, uint8_t current_drive,
	uint32_t visible_drives)
{
	if (!identity_is_valid(adapter_identity) ||
	    !identity_is_valid(adapter_context) || current_drive >= DOS_DRIVE_COUNT ||
	    (visible_drives & ~DOS_DRIVE_VALID_MASK) != 0u ||
	    (visible_drives & (1u << current_drive)) == 0u)
		return DOS_DRIVE_VISIBILITY_INVALID_ARGUMENT;
	if (owner.initialized != 0u)
		return DOS_DRIVE_VISIBILITY_INVALID_STATE;
	owner = (struct dos_drive_visibility_owner){
		.context = adapter_context,
		.visible_drives = visible_drives,
		.current_drive = current_drive,
		.initialized = 1u,
		.reserved = {0u},
	};
	operations.identity = adapter_identity;
	return DOS_DRIVE_VISIBILITY_READY;
}

const struct dos_exec_drive_visibility_ops *dos_drive_visibility_ops(void)
{
	return owner.initialized == 1u ? &operations : NULL;
}

kernel_object_handle_t dos_drive_visibility_context(void)
{
	return owner.initialized == 1u ? owner.context
				      : KERNEL_OBJECT_HANDLE_INVALID;
}

static enum dos_exec_drive_visibility_status
resolve_drive(kernel_object_handle_t context, uint8_t drive_designator)
{
	uint8_t drive;

	if (owner.initialized != 1u || context != owner.context)
		return DOS_EXEC_DRIVE_FAULT;
	if (drive_designator == 0u) {
		drive = owner.current_drive;
	} else if (drive_designator <= DOS_DRIVE_COUNT) {
		drive = (uint8_t)(drive_designator - 1u);
	} else {
		return DOS_EXEC_DRIVE_INVALID;
	}
	return (owner.visible_drives & (1u << drive)) != 0u
		       ? DOS_EXEC_DRIVE_VISIBLE
		       : DOS_EXEC_DRIVE_INVALID;
}

static_assert_expression(sizeof(struct dos_drive_visibility_owner) == 16u,
			 "drive visibility state must stay fixed width");

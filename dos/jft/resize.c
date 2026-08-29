// SPDX-License-Identifier: GPL-2.0-only
/*
 * DOS job file table resizing
 *
 * Compatibility contract: clamp BX to twenty, preserve live handles on shrink,
 *                 use PSP:18h at twenty and an MCB-owned external table above
 *                 twenty, with AX errors 1, 4 and 8 at the INT 21h boundary
 * Safety changes: bounded chunk copies, checked ownership, acquire-before-
 *                 publish, transactional PSP replacement and sticky poison
 */
#include "dos_jft.h"

#include "dos_abi.h"

#define JFT_COPY_CHUNK_BYTES 256u
#define PSP_JFT_OFFSET                                                       \
	((uint16_t)__builtin_offsetof(struct dos_psp_prefix40, jft))
#define PSP_JFT_LENGTH_OFFSET                                                \
	((uint16_t)__builtin_offsetof(struct dos_psp_prefix40, jft_length))
#define PSP_JFT_POINTER_OFFSET                                               \
	((uint16_t)__builtin_offsetof(struct dos_psp_prefix40, jft_pointer))
#define PSP_JFT_CONTROL_BYTES 6u
#define PSP_INLINE_PUBLICATION_BYTES                                         \
	((size_t)(PSP_JFT_POINTER_OFFSET + sizeof(struct dos_far_pointer16) -   \
		  PSP_JFT_OFFSET))
#define PSP_INLINE_CONTROL_INDEX                                             \
	((size_t)(PSP_JFT_LENGTH_OFFSET - PSP_JFT_OFFSET))

struct jft_control {
	struct dos_far_pointer16 pointer;
	uint16_t length;
};

static uint16_t read_le16(const uint8_t *bytes)
{
	return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8u);
}

static void write_le16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8u);
}

static size_t minimum_size(size_t left, size_t right)
{
	return left < right ? left : right;
}

static enum dos_jft_status poison_arena(struct dos_memory_arena *arena)
{
	if (arena != NULL &&
	    dos_memory_arena_poison(arena) != DOS_MEMORY_OK)
		arena->machine_poisoned = 1u;
	return DOS_JFT_MACHINE_POISONED;
}

static enum dos_jft_status
map_memory_status(enum dos_memory_status status, bool allocation)
{
	switch (status) {
	case DOS_MEMORY_OK:
		return DOS_JFT_OK;
	case DOS_MEMORY_NOT_ENOUGH_MEMORY:
		return allocation ? DOS_JFT_NOT_ENOUGH_MEMORY
				  : DOS_JFT_ARENA_FAULT;
	case DOS_MEMORY_MACHINE_FAULT:
		return DOS_JFT_MACHINE_FAULT;
	case DOS_MEMORY_MACHINE_POISONED:
		return DOS_JFT_MACHINE_POISONED;
	case DOS_MEMORY_INVALID_ARGUMENT:
	case DOS_MEMORY_OWNER_MISMATCH:
		return DOS_JFT_INVALID_STATE;
	case DOS_MEMORY_INVALID_BLOCK:
	case DOS_MEMORY_ARENA_DAMAGED:
	case DOS_MEMORY_IDENTITY_MISMATCH:
	case DOS_MEMORY_GENERATION_EXHAUSTED:
		return DOS_JFT_ARENA_FAULT;
	}
	return DOS_JFT_ARENA_FAULT;
}

static enum dos_jft_status
read_jft_control(const struct dos_machine *machine, uint16_t psp,
		 struct jft_control *control)
{
	uint8_t bytes[PSP_JFT_CONTROL_BYTES];

	if (dos_machine_read_far(machine, psp, PSP_JFT_LENGTH_OFFSET, bytes,
				 sizeof(bytes), sizeof(bytes)) != DOS_MACHINE_OK)
		return DOS_JFT_MACHINE_FAULT;
	control->length = read_le16(bytes);
	control->pointer.offset = read_le16(bytes + 2u);
	control->pointer.segment = read_le16(bytes + 4u);
	return DOS_JFT_OK;
}

static void encode_jft_control(uint8_t bytes[PSP_JFT_CONTROL_BYTES],
			       uint16_t length,
			       struct dos_far_pointer16 pointer)
{
	write_le16(bytes, length);
	write_le16(bytes + 2u, pointer.offset);
	write_le16(bytes + 4u, pointer.segment);
}

static enum dos_jft_status
validate_jft_range(const struct dos_machine *machine,
		   const struct jft_control *control)
{
	return dos_machine_validate_far(machine, control->pointer.segment,
					control->pointer.offset,
					(size_t)control->length) == DOS_MACHINE_OK
		       ? DOS_JFT_OK
		       : DOS_JFT_MACHINE_FAULT;
}

static enum dos_jft_status
scan_closed_tail(const struct dos_machine *machine,
		 const struct jft_control *control, uint16_t retained)
{
	uint8_t bytes[JFT_COPY_CHUNK_BYTES];
	size_t completed = 0u;
	size_t count = (size_t)control->length - (size_t)retained;

	while (completed < count) {
		size_t amount = minimum_size(sizeof(bytes), count - completed);
		uint16_t offset = (uint16_t)((uint32_t)control->pointer.offset +
					     (uint32_t)retained +
					     (uint32_t)completed);
		size_t index;

		if (dos_machine_read_far(machine, control->pointer.segment, offset,
					 bytes, sizeof(bytes), amount) != DOS_MACHINE_OK)
			return DOS_JFT_MACHINE_FAULT;
		for (index = 0u; index < amount; ++index) {
			if (bytes[index] != DOS_JFT_UNUSED)
				return DOS_JFT_TOO_MANY_OPEN_FILES;
		}
		completed += amount;
	}
	return DOS_JFT_OK;
}

static enum dos_jft_status preflight_old_external(
	const struct dos_memory_arena *arena, const struct dos_machine *machine,
	uint16_t current_psp, const struct jft_control *control)
{
	struct dos_memory_owner_rebind_value ignored;
	enum dos_memory_status status;

	if (control->pointer.offset != 0u)
		return DOS_JFT_OK;
	status = dos_memory_prepare_owner_rebind_checked(
		arena, machine, control->pointer.segment, current_psp,
		current_psp, &ignored);
	return map_memory_status(status, false);
}

static enum dos_jft_status release_owned_block(
	struct dos_memory_arena *arena, const struct dos_machine *machine,
	uint16_t segment, uint16_t owner)
{
	return map_memory_status(
		dos_memory_free_owned_checked(arena, machine, segment, owner), false);
}

static enum dos_jft_status cleanup_new_block(
	struct dos_memory_arena *arena, const struct dos_machine *machine,
	uint16_t segment, uint16_t owner, enum dos_jft_status original)
{
	enum dos_jft_status cleanup =
		release_owned_block(arena, machine, segment, owner);

	if (cleanup != DOS_JFT_OK)
		return poison_arena(arena);
	return original;
}

static enum dos_jft_status populate_external_table(
	const struct dos_machine *machine, const struct jft_control *old,
	uint16_t copy_count, uint16_t new_segment, uint16_t new_count)
{
	uint8_t bytes[JFT_COPY_CHUNK_BYTES];
	size_t completed = 0u;
	size_t index;

	while (completed < (size_t)copy_count) {
		size_t amount = minimum_size(sizeof(bytes),
					     (size_t)copy_count - completed);
		uint16_t source_offset =
			(uint16_t)((uint32_t)old->pointer.offset +
				   (uint32_t)completed);

		if (dos_machine_read_far(machine, old->pointer.segment,
					 source_offset, bytes, sizeof(bytes), amount) !=
			    DOS_MACHINE_OK ||
		    dos_machine_write_far(machine, new_segment,
					  (uint16_t)completed, bytes,
					  sizeof(bytes), amount) != DOS_MACHINE_OK)
			return DOS_JFT_MACHINE_FAULT;
		completed += amount;
	}
	for (index = 0u; index < sizeof(bytes); ++index)
		bytes[index] = DOS_JFT_UNUSED;
	while (completed < (size_t)new_count) {
		size_t amount = minimum_size(sizeof(bytes),
					     (size_t)new_count - completed);

		if (dos_machine_write_far(machine, new_segment,
					  (uint16_t)completed, bytes,
					  sizeof(bytes), amount) != DOS_MACHINE_OK)
			return DOS_JFT_MACHINE_FAULT;
		completed += amount;
	}
	return DOS_JFT_OK;
}

static enum dos_jft_status rollback_external_publication(
	struct dos_memory_arena *arena, const struct dos_machine *machine,
	uint16_t current_psp, uint8_t old_control[PSP_JFT_CONTROL_BYTES],
	uint8_t scratch[PSP_JFT_CONTROL_BYTES], uint16_t new_segment,
	enum dos_jft_status original)
{
	enum dos_machine_status machine_status = dos_machine_replace_far(
		machine, current_psp, PSP_JFT_LENGTH_OFFSET, old_control,
		PSP_JFT_CONTROL_BYTES, scratch, PSP_JFT_CONTROL_BYTES,
		PSP_JFT_CONTROL_BYTES);

	if (machine_status != DOS_MACHINE_OK)
		return poison_arena(arena);
	return cleanup_new_block(arena, machine, new_segment, current_psp,
				 original);
}

static enum dos_jft_status resize_to_inline(
	struct dos_memory_arena *arena, const struct dos_machine *machine,
	uint16_t current_psp, const struct jft_control *old)
{
	uint8_t retained[DOS_JFT_MINIMUM_HANDLES];
	uint8_t replacement[PSP_INLINE_PUBLICATION_BYTES];
	uint8_t rollback[PSP_INLINE_PUBLICATION_BYTES];
	struct dos_far_pointer16 inline_pointer = {
		.offset = PSP_JFT_OFFSET,
		.segment = current_psp,
	};
	enum dos_machine_status machine_status;
	enum dos_jft_status status;
	size_t index;

	if (dos_machine_read_far(machine, old->pointer.segment,
				 old->pointer.offset, retained, sizeof(retained),
				 sizeof(retained)) != DOS_MACHINE_OK ||
	    dos_machine_read_far(machine, current_psp, PSP_JFT_OFFSET,
				 replacement, sizeof(replacement),
				 sizeof(replacement)) != DOS_MACHINE_OK)
		return DOS_JFT_MACHINE_FAULT;
	for (index = 0u; index < sizeof(retained); ++index)
		replacement[index] = retained[index];
	encode_jft_control(replacement + PSP_INLINE_CONTROL_INDEX,
			   DOS_JFT_MINIMUM_HANDLES, inline_pointer);
	machine_status = dos_machine_replace_far(
		machine, current_psp, PSP_JFT_OFFSET, replacement,
		sizeof(replacement), rollback, sizeof(rollback),
		sizeof(replacement));
	if (machine_status == DOS_MACHINE_ROLLBACK_FAILED)
		return poison_arena(arena);
	if (machine_status != DOS_MACHINE_OK)
		return DOS_JFT_MACHINE_FAULT;
	if (old->pointer.offset != 0u)
		return DOS_JFT_OK;
	status = release_owned_block(arena, machine, old->pointer.segment,
				     current_psp);
	if (status == DOS_JFT_OK)
		return DOS_JFT_OK;
	if (status == DOS_JFT_MACHINE_POISONED)
		return poison_arena(arena);
	machine_status = dos_machine_replace_far(
		machine, current_psp, PSP_JFT_OFFSET, rollback,
		sizeof(rollback), replacement, sizeof(replacement),
		sizeof(rollback));
	if (machine_status != DOS_MACHINE_OK)
		return poison_arena(arena);
	return status;
}

static enum dos_jft_status resize_to_external(
	struct dos_memory_arena *arena, const struct dos_machine *machine,
	uint16_t current_psp, const struct jft_control *old,
	uint16_t requested, bool shrinking)
{
	struct dos_memory_allocation_result allocation;
	struct dos_far_pointer16 new_pointer;
	uint8_t replacement[PSP_JFT_CONTROL_BYTES];
	uint8_t rollback[PSP_JFT_CONTROL_BYTES];
	uint32_t rounded = (uint32_t)requested + 15u;
	uint16_t paragraphs = (uint16_t)(rounded >> 4u);
	uint16_t copy_count = shrinking ? requested : old->length;
	enum dos_memory_status memory_status;
	enum dos_machine_status machine_status;
	enum dos_jft_status status;

	memory_status = dos_memory_allocate_checked(
		arena, machine, current_psp, paragraphs, &allocation);
	status = map_memory_status(memory_status, true);
	if (status != DOS_JFT_OK)
		return status == DOS_JFT_MACHINE_POISONED ? poison_arena(arena)
							 : status;
	status = populate_external_table(machine, old, copy_count,
					 allocation.block_segment, requested);
	if (status != DOS_JFT_OK)
		return cleanup_new_block(arena, machine,
					 allocation.block_segment, current_psp,
					 status);
	new_pointer.offset = 0u;
	new_pointer.segment = allocation.block_segment;
	encode_jft_control(replacement, requested, new_pointer);
	machine_status = dos_machine_replace_far(
		machine, current_psp, PSP_JFT_LENGTH_OFFSET, replacement,
		sizeof(replacement), rollback, sizeof(rollback),
		sizeof(replacement));
	if (machine_status == DOS_MACHINE_ROLLBACK_FAILED)
		return poison_arena(arena);
	if (machine_status != DOS_MACHINE_OK)
		return cleanup_new_block(arena, machine,
					 allocation.block_segment, current_psp,
					 DOS_JFT_MACHINE_FAULT);
	if (old->pointer.offset != 0u)
		return DOS_JFT_OK;
	status = release_owned_block(arena, machine, old->pointer.segment,
				     current_psp);
	if (status == DOS_JFT_OK)
		return DOS_JFT_OK;
	if (status == DOS_JFT_MACHINE_POISONED)
		return poison_arena(arena);
	return rollback_external_publication(
		arena, machine, current_psp, rollback, replacement,
		allocation.block_segment, status);
}

enum dos_jft_status dos_jft_resize_checked(
	struct dos_memory_arena *arena, const struct dos_machine *machine,
	uint16_t current_psp, uint16_t requested_handles)
{
	struct jft_control old;
	uint16_t requested = requested_handles < DOS_JFT_MINIMUM_HANDLES
				 ? DOS_JFT_MINIMUM_HANDLES
				 : requested_handles;
	bool shrinking;
	enum dos_jft_status status;

	if (arena == NULL || machine == NULL || machine->ops == NULL ||
	    machine->ops->read_memory == NULL ||
	    machine->ops->write_memory == NULL)
		return DOS_JFT_INVALID_ARGUMENT;
	status = read_jft_control(machine, current_psp, &old);
	if (status != DOS_JFT_OK)
		return status;
	if (requested == old.length)
		return DOS_JFT_OK;
	shrinking = requested < old.length;
	if (!shrinking && requested > DOS_JFT_MAXIMUM_HANDLES)
		return DOS_JFT_INVALID_FUNCTION;
	status = validate_jft_range(machine, &old);
	if (status != DOS_JFT_OK)
		return status;
	if (shrinking) {
		status = scan_closed_tail(machine, &old, requested);
		if (status != DOS_JFT_OK)
			return status;
	}
	status = preflight_old_external(arena, machine, current_psp, &old);
	if (status != DOS_JFT_OK)
		return status == DOS_JFT_MACHINE_POISONED ? poison_arena(arena)
							 : status;
	if (shrinking && requested == DOS_JFT_MINIMUM_HANDLES)
		return resize_to_inline(arena, machine, current_psp, &old);
	return resize_to_external(arena, machine, current_psp, &old, requested,
				  shrinking);
}

static_assert_expression(DOS_JFT_MINIMUM_HANDLES == DOS_PSP_DEFAULT_HANDLES,
			 "JFT minimum must match the PSP inline table");
static_assert_expression(PSP_JFT_OFFSET == 0x18u,
			 "PSP inline JFT offset changed");
static_assert_expression(PSP_JFT_LENGTH_OFFSET == 0x32u,
			 "PSP JFT length offset changed");
static_assert_expression(PSP_JFT_POINTER_OFFSET == 0x34u,
			 "PSP JFT pointer offset changed");
static_assert_expression(PSP_INLINE_PUBLICATION_BYTES == 0x20u,
			 "inline JFT publication range changed");

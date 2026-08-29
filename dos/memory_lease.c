// SPDX-License-Identifier: GPL-2.0-only
/* Fixed-slot, generation-checked MCB ownership for EXEC transactions. */
#include "dos_memory_lease.h"

static bool bytes_are_zero(const uint8_t *bytes, size_t count)
{
	size_t index;

	for (index = 0u; index < count; ++index) {
		if (bytes[index] != 0u)
			return false;
	}
	return true;
}

static bool arena_has_valid_flag_encoding(
    const struct dos_memory_arena *arena)
{
	return arena != NULL && arena->initialized <= 1u &&
	       arena->machine_poisoned <= 1u && arena->constructed == 1u &&
	       bytes_are_zero(arena->reserved, ARRAY_SIZE(arena->reserved));
}

static bool table_identity_is_valid(
    dos_memory_lease_table_identity_t identity)
{
	return identity != 0u &&
	       identity != DOS_MEMORY_LEASE_TABLE_IDENTITY_INVALID;
}

static bool slot_has_valid_encoding(
    const struct dos_memory_lease_slot *slot,
    dos_memory_lease_table_identity_t lifetime_identity)
{
	return slot != NULL &&
	       slot->generation <= DOS_MEMORY_LEASE_GENERATION_MAX &&
	       slot->lifetime_identity == lifetime_identity &&
	       slot->state <= (uint8_t)DOS_MEMORY_LEASE_SLOT_PUBLISHED &&
	       bytes_are_zero(slot->reserved, ARRAY_SIZE(slot->reserved));
}

static bool table_has_valid_encoding(const struct dos_memory_lease_table *table)
{
	size_t index;

	if (table == NULL || table->constructed != 1u ||
	    table->initialized != 1u ||
	    !table_identity_is_valid(table->lifetime_identity) ||
	    !bytes_are_zero(table->reserved, ARRAY_SIZE(table->reserved)))
		return false;
	for (index = 0u; index < ARRAY_SIZE(table->slots); ++index) {
		if (!slot_has_valid_encoding(&table->slots[index],
					     table->lifetime_identity))
			return false;
	}
	return true;
}

static enum dos_memory_lease_status
memory_status_to_lease_status(enum dos_memory_status status)
{
	switch (status) {
	case DOS_MEMORY_OK:
		return DOS_MEMORY_LEASE_OK;
	case DOS_MEMORY_INVALID_ARGUMENT:
		return DOS_MEMORY_LEASE_INVALID_ARGUMENT;
	case DOS_MEMORY_INVALID_BLOCK:
		return DOS_MEMORY_LEASE_INVALID_BLOCK;
	case DOS_MEMORY_NOT_ENOUGH_MEMORY:
		return DOS_MEMORY_LEASE_NOT_ENOUGH_MEMORY;
	case DOS_MEMORY_ARENA_DAMAGED:
		return DOS_MEMORY_LEASE_ARENA_DAMAGED;
	case DOS_MEMORY_MACHINE_FAULT:
		return DOS_MEMORY_LEASE_MACHINE_FAULT;
	case DOS_MEMORY_MACHINE_POISONED:
		return DOS_MEMORY_LEASE_MACHINE_POISONED;
	case DOS_MEMORY_OWNER_MISMATCH:
		return DOS_MEMORY_LEASE_OWNER_MISMATCH;
	case DOS_MEMORY_IDENTITY_MISMATCH:
		return DOS_MEMORY_LEASE_IDENTITY_MISMATCH;
	case DOS_MEMORY_GENERATION_EXHAUSTED:
		return DOS_MEMORY_LEASE_GENERATION_EXHAUSTED;
	}
	return DOS_MEMORY_LEASE_ARENA_DAMAGED;
}

static bool slot_is_reusable(const struct dos_memory_lease_slot *slot)
{
	return slot->state == DOS_MEMORY_LEASE_SLOT_VACANT ||
	       slot->state == DOS_MEMORY_LEASE_SLOT_RELEASED ||
	       slot->state == DOS_MEMORY_LEASE_SLOT_ABORTED ||
	       slot->state == DOS_MEMORY_LEASE_SLOT_PUBLISHED;
}

static struct dos_memory_lease_handle make_handle(
    dos_memory_lease_table_identity_t lifetime_identity, uint32_t slot,
    uint64_t generation)
{
	struct dos_memory_lease_handle handle;

	handle.value =
	    ((uint64_t)lifetime_identity <<
	     DOS_MEMORY_LEASE_TABLE_IDENTITY_SHIFT) |
	    (generation << DOS_MEMORY_LEASE_GENERATION_SHIFT) | (uint64_t)slot;
	return handle;
}

static enum dos_memory_lease_status
decode_handle(struct dos_memory_lease_handle handle, uint32_t *slot,
	      uint64_t *generation,
	      dos_memory_lease_table_identity_t *lifetime_identity)
{
	uint64_t decoded_generation;
	dos_memory_lease_table_identity_t decoded_identity;

	if (slot == NULL || generation == NULL || lifetime_identity == NULL ||
	    handle.value == 0u)
		return DOS_MEMORY_LEASE_INVALID_ARGUMENT;
	decoded_identity = (dos_memory_lease_table_identity_t)(
	    handle.value >> DOS_MEMORY_LEASE_TABLE_IDENTITY_SHIFT);
	if (!table_identity_is_valid(decoded_identity))
		return DOS_MEMORY_LEASE_INVALID_ARGUMENT;
	decoded_generation =
	    (handle.value >> DOS_MEMORY_LEASE_GENERATION_SHIFT) &
	    DOS_MEMORY_LEASE_GENERATION_MAX;
	if (decoded_generation == 0u)
		return DOS_MEMORY_LEASE_STALE_HANDLE;
	*slot = (uint32_t)(handle.value & DOS_MEMORY_LEASE_SLOT_MASK);
	*generation = decoded_generation;
	*lifetime_identity = decoded_identity;
	return DOS_MEMORY_LEASE_OK;
}

static void clear_slot_identity(struct dos_memory_lease_slot *slot)
{
	size_t index;

	slot->machine_context = KERNEL_OBJECT_HANDLE_INVALID;
	slot->arena_identity = KERNEL_OBJECT_HANDLE_INVALID;
	slot->arena_generation = 0u;
	slot->arena_head_segment = 0u;
	slot->guest_segment = 0u;
	slot->owner = 0u;
	slot->paragraphs = 0u;
	for (index = 0u; index < ARRAY_SIZE(slot->reserved); ++index)
		slot->reserved[index] = 0u;
}

enum dos_memory_lease_status
dos_memory_lease_table_construct(
    struct dos_memory_lease_table *table,
    dos_memory_lease_table_identity_t lifetime_identity)
{
	size_t index;

	if (table == NULL || !table_identity_is_valid(lifetime_identity))
		return DOS_MEMORY_LEASE_INVALID_ARGUMENT;
	for (index = 0u; index < DOS_MEMORY_LEASE_SLOT_COUNT; ++index) {
		table->slots[index].generation = 0u;
		clear_slot_identity(&table->slots[index]);
		table->slots[index].lifetime_identity = lifetime_identity;
		table->slots[index].state = DOS_MEMORY_LEASE_SLOT_VACANT;
	}
	table->lifetime_identity = lifetime_identity;
	table->initialized = 0u;
	table->constructed = 1u;
	for (index = 0u; index < ARRAY_SIZE(table->reserved); ++index)
		table->reserved[index] = 0u;
	return DOS_MEMORY_LEASE_OK;
}

enum dos_memory_lease_status
dos_memory_lease_table_initialize(struct dos_memory_lease_table *table)
{
	size_t index;

	if (table == NULL || table->constructed != 1u ||
	    table->initialized > 1u ||
	    !table_identity_is_valid(table->lifetime_identity) ||
	    !bytes_are_zero(table->reserved, ARRAY_SIZE(table->reserved)))
		return DOS_MEMORY_LEASE_INVALID_ARGUMENT;
	if (table->initialized != 0u)
		return DOS_MEMORY_LEASE_INVALID_STATE;
	for (index = 0u; index < DOS_MEMORY_LEASE_SLOT_COUNT; ++index) {
		table->slots[index].generation = 0u;
		clear_slot_identity(&table->slots[index]);
		table->slots[index].lifetime_identity = table->lifetime_identity;
		table->slots[index].state = DOS_MEMORY_LEASE_SLOT_VACANT;
	}
	table->initialized = 1u;
	table->constructed = 1u;
	return DOS_MEMORY_LEASE_OK;
}

bool dos_memory_lease_table_is_drained(
    const struct dos_memory_lease_table *table)
{
	size_t index;

	if (!table_has_valid_encoding(table))
		return false;
	for (index = 0u; index < DOS_MEMORY_LEASE_SLOT_COUNT; ++index) {
		switch (table->slots[index].state) {
		case DOS_MEMORY_LEASE_SLOT_VACANT:
		case DOS_MEMORY_LEASE_SLOT_RELEASED:
		case DOS_MEMORY_LEASE_SLOT_ABORTED:
		case DOS_MEMORY_LEASE_SLOT_PUBLISHED:
			break;
		case DOS_MEMORY_LEASE_SLOT_ACQUIRING:
		case DOS_MEMORY_LEASE_SLOT_ACTIVE:
		case DOS_MEMORY_LEASE_SLOT_RELEASING:
		default:
			return false;
		}
	}
	return true;
}

static enum dos_memory_lease_status
reserve_slot(struct dos_memory_lease_table *table,
	     const struct dos_memory_arena *arena,
	     const struct dos_machine *machine, uint16_t owner,
	     uint16_t paragraphs, uint32_t *slot_index)
{
	bool every_generation_exhausted = true;
	uint32_t index;

	for (index = 0u; index < DOS_MEMORY_LEASE_SLOT_COUNT; ++index) {
		struct dos_memory_lease_slot *slot = &table->slots[index];

		if (slot->generation != DOS_MEMORY_LEASE_GENERATION_MAX)
			every_generation_exhausted = false;
		if (!slot_is_reusable(slot) ||
		    slot->generation == DOS_MEMORY_LEASE_GENERATION_MAX)
			continue;
		++slot->generation;
		slot->machine_context = machine->context;
		slot->arena_identity = arena->identity;
		slot->arena_generation = arena->generation;
		slot->arena_head_segment = arena->head_segment;
		slot->guest_segment = 0u;
		slot->owner = owner;
		slot->paragraphs = paragraphs;
		slot->state = DOS_MEMORY_LEASE_SLOT_ACQUIRING;
		*slot_index = index;
		return DOS_MEMORY_LEASE_OK;
	}
	return every_generation_exhausted
		   ? DOS_MEMORY_LEASE_GENERATION_EXHAUSTED
		   : DOS_MEMORY_LEASE_NO_SLOT;
}

static enum dos_memory_lease_status
acquire_common(struct dos_memory_lease_table *table,
	       struct dos_memory_arena *arena,
	       const struct dos_machine *machine, uint16_t owner,
	       const struct dos_memory_owner_identity *named_owner,
	       uint16_t paragraphs, struct dos_memory_lease_receipt *receipt)
{
	struct dos_memory_allocation_result allocation;
	struct dos_memory_lease_receipt prepared;
	struct dos_memory_lease_slot *slot;
	enum dos_memory_lease_status lease_status;
	enum dos_memory_status memory_status;
	uint32_t slot_index;

	if (!table_has_valid_encoding(table) ||
	    !arena_has_valid_flag_encoding(arena) ||
	    machine == NULL || receipt == NULL || owner == 0u ||
	    arena->initialized != 1u ||
	    arena->identity == 0u ||
	    arena->identity == KERNEL_OBJECT_HANDLE_INVALID ||
	    arena->generation == 0u ||
	    (named_owner != NULL && named_owner->psp_segment != owner))
		return DOS_MEMORY_LEASE_INVALID_ARGUMENT;
	if (arena->machine_poisoned == 1u)
		return DOS_MEMORY_LEASE_MACHINE_POISONED;
	lease_status =
	    reserve_slot(table, arena, machine, owner, paragraphs, &slot_index);
	if (lease_status != DOS_MEMORY_LEASE_OK)
		return lease_status;
	slot = &table->slots[slot_index];

	if (named_owner == NULL)
		memory_status = dos_memory_allocate_checked(
		    arena, machine, owner, paragraphs, &allocation);
	else
		memory_status = dos_memory_allocate_named_checked(
		    arena, machine, named_owner, paragraphs, &allocation);
	if (memory_status != DOS_MEMORY_OK) {
		clear_slot_identity(slot);
		slot->state = DOS_MEMORY_LEASE_SLOT_VACANT;
		return memory_status_to_lease_status(memory_status);
	}

	/* Publish the handle only after the guest MCB is fully committed. */
	slot->guest_segment = allocation.block_segment;
	slot->state = DOS_MEMORY_LEASE_SLOT_ACTIVE;
	prepared.handle = make_handle(table->lifetime_identity, slot_index,
				      slot->generation);
	prepared.guest_segment = allocation.block_segment;
	prepared.paragraphs = paragraphs;
	prepared.maximum_available = allocation.maximum_available;
	prepared.reserved = 0u;
	*receipt = prepared;
	return DOS_MEMORY_LEASE_OK;
}

enum dos_memory_lease_status dos_memory_lease_acquire(
    struct dos_memory_lease_table *table, struct dos_memory_arena *arena,
    const struct dos_machine *machine,
    const struct dos_memory_owner_identity *owner, uint16_t paragraphs,
    struct dos_memory_lease_receipt *receipt)
{
	if (owner == NULL)
		return DOS_MEMORY_LEASE_INVALID_ARGUMENT;
	return acquire_common(table, arena, machine, owner->psp_segment, owner,
			      paragraphs, receipt);
}

enum dos_memory_lease_status dos_memory_lease_acquire_unnamed(
    struct dos_memory_lease_table *table, struct dos_memory_arena *arena,
    const struct dos_machine *machine, uint16_t owner, uint16_t paragraphs,
    struct dos_memory_lease_receipt *receipt)
{
	return acquire_common(table, arena, machine, owner, NULL, paragraphs,
			      receipt);
}

static enum dos_memory_lease_status
find_slot(const struct dos_memory_lease_table *table,
	  const struct dos_memory_arena *arena,
	  const struct dos_machine *machine,
	  struct dos_memory_lease_handle handle, uint16_t expected_owner,
	  uint32_t *found_index)
{
	const struct dos_memory_lease_slot *slot;
	uint64_t generation;
	dos_memory_lease_table_identity_t lifetime_identity;
	uint32_t slot_index;
	enum dos_memory_lease_status status;

	if (!table_has_valid_encoding(table) ||
	    !arena_has_valid_flag_encoding(arena) ||
	    machine == NULL || found_index == NULL || machine->ops == NULL ||
	    arena->initialized != 1u ||
	    arena->identity == 0u ||
	    arena->identity == KERNEL_OBJECT_HANDLE_INVALID ||
	    arena->generation == 0u || expected_owner == 0u)
		return DOS_MEMORY_LEASE_INVALID_ARGUMENT;
	status = decode_handle(handle, &slot_index, &generation,
			       &lifetime_identity);
	if (status != DOS_MEMORY_LEASE_OK)
		return status;
	if (lifetime_identity != table->lifetime_identity)
		return DOS_MEMORY_LEASE_IDENTITY_MISMATCH;
	if (slot_index >= DOS_MEMORY_LEASE_SLOT_COUNT)
		return DOS_MEMORY_LEASE_STALE_HANDLE;
	slot = &table->slots[slot_index];
	if (slot->generation != generation ||
	    slot->state == DOS_MEMORY_LEASE_SLOT_VACANT)
		return DOS_MEMORY_LEASE_STALE_HANDLE;
	if (slot->machine_context != machine->context)
		return DOS_MEMORY_LEASE_CONTEXT_MISMATCH;
	if (slot->arena_identity != arena->identity)
		return DOS_MEMORY_LEASE_IDENTITY_MISMATCH;
	if (slot->arena_head_segment != arena->head_segment ||
	    slot->arena_generation != arena->generation)
		return DOS_MEMORY_LEASE_STALE_HANDLE;
	if (slot->owner != expected_owner)
		return DOS_MEMORY_LEASE_OWNER_MISMATCH;
	*found_index = slot_index;
	return DOS_MEMORY_LEASE_OK;
}

enum dos_memory_lease_status dos_memory_lease_resolve_active(
    const struct dos_memory_lease_table *table,
    const struct dos_memory_arena *arena, const struct dos_machine *machine,
    struct dos_memory_lease_handle handle, uint16_t expected_owner,
    struct dos_memory_lease_view *view)
{
	struct dos_memory_lease_view prepared;
	const struct dos_memory_lease_slot *slot;
	uint32_t slot_index;
	enum dos_memory_lease_status status;

	if (view == NULL)
		return DOS_MEMORY_LEASE_INVALID_ARGUMENT;
	status = find_slot(table, arena, machine, handle, expected_owner,
			   &slot_index);
	if (status != DOS_MEMORY_LEASE_OK)
		return status;
	slot = &table->slots[slot_index];
	if (slot->state != DOS_MEMORY_LEASE_SLOT_ACTIVE)
		return DOS_MEMORY_LEASE_INVALID_STATE;
	if (arena->machine_poisoned)
		return DOS_MEMORY_LEASE_MACHINE_POISONED;

	prepared.handle = handle;
	prepared.machine_context = slot->machine_context;
	prepared.arena_identity = slot->arena_identity;
	prepared.arena_generation = slot->arena_generation;
	prepared.guest_segment = slot->guest_segment;
	prepared.paragraphs = slot->paragraphs;
	prepared.owner = slot->owner;
	prepared.reserved = 0u;
	view->handle = prepared.handle;
	view->machine_context = prepared.machine_context;
	view->arena_identity = prepared.arena_identity;
	view->arena_generation = prepared.arena_generation;
	view->guest_segment = prepared.guest_segment;
	view->paragraphs = prepared.paragraphs;
	view->owner = prepared.owner;
	view->reserved = prepared.reserved;
	return DOS_MEMORY_LEASE_OK;
}

static bool
owner_name_patch_is_valid(const struct dos_memory_owner_name_patch *name_patch)
{
	size_t index;

	if (name_patch == NULL || name_patch->count == 0u ||
	    name_patch->count > DOS_MEMORY_OWNER_NAME_BYTES)
		return false;
	for (index = 0u; index < ARRAY_SIZE(name_patch->reserved); ++index) {
		if (name_patch->reserved[index] != 0u)
			return false;
	}
	return true;
}

static enum dos_memory_lease_status transfer_owner_common(
    struct dos_memory_lease_table *table, struct dos_memory_arena *arena,
    const struct dos_machine *machine, struct dos_memory_lease_handle handle,
    uint16_t expected_owner, uint16_t new_owner,
    const struct dos_memory_owner_identity *named_new_owner,
    const struct dos_memory_owner_name_patch *name_patch)
{
	struct dos_memory_lease_slot *slot;
	enum dos_memory_lease_status lease_status;
	enum dos_memory_status memory_status;
	uint32_t slot_index;

	if (new_owner == 0u ||
	    (named_new_owner != NULL && name_patch != NULL) ||
	    (named_new_owner != NULL &&
	     named_new_owner->psp_segment != new_owner))
		return DOS_MEMORY_LEASE_INVALID_ARGUMENT;
	lease_status = find_slot(table, arena, machine, handle, expected_owner,
				 &slot_index);
	if (lease_status != DOS_MEMORY_LEASE_OK)
		return lease_status;
	slot = &table->slots[slot_index];
	if (slot->state != DOS_MEMORY_LEASE_SLOT_ACTIVE)
		return DOS_MEMORY_LEASE_INVALID_STATE;
	if (arena->machine_poisoned)
		return DOS_MEMORY_LEASE_MACHINE_POISONED;
	if (name_patch != NULL)
		memory_status = dos_memory_transfer_owner_name_patch_checked(
		    arena, machine, slot->guest_segment, expected_owner,
		    new_owner, name_patch);
	else if (named_new_owner == NULL)
		memory_status = dos_memory_transfer_owner_checked(
		    arena, machine, slot->guest_segment, expected_owner,
		    new_owner);
	else
		memory_status = dos_memory_transfer_named_owner_checked(
		    arena, machine, slot->guest_segment, expected_owner,
		    named_new_owner);
	if (memory_status != DOS_MEMORY_OK)
		return memory_status_to_lease_status(memory_status);

	/* Guest ownership is committed before the native capability changes. */
	slot->owner = new_owner;
	return DOS_MEMORY_LEASE_OK;
}

enum dos_memory_lease_status dos_memory_lease_transfer_owner(
    struct dos_memory_lease_table *table, struct dos_memory_arena *arena,
    const struct dos_machine *machine, struct dos_memory_lease_handle handle,
    uint16_t expected_owner, uint16_t new_owner)
{
	return transfer_owner_common(table, arena, machine, handle,
				     expected_owner, new_owner, NULL, NULL);
}

enum dos_memory_lease_status dos_memory_lease_transfer_named_owner(
    struct dos_memory_lease_table *table, struct dos_memory_arena *arena,
    const struct dos_machine *machine, struct dos_memory_lease_handle handle,
    uint16_t expected_owner, const struct dos_memory_owner_identity *new_owner)
{
	struct dos_memory_owner_identity stable_owner;
	size_t index;

	if (new_owner == NULL || new_owner->psp_segment == 0u)
		return DOS_MEMORY_LEASE_INVALID_ARGUMENT;
	stable_owner.psp_segment = new_owner->psp_segment;
	for (index = 0u; index < DOS_MEMORY_OWNER_NAME_BYTES; ++index)
		stable_owner.name[index] = new_owner->name[index];
	return transfer_owner_common(table, arena, machine, handle,
				     expected_owner, stable_owner.psp_segment,
				     &stable_owner, NULL);
}

enum dos_memory_lease_status dos_memory_lease_transfer_owner_name_patch(
    struct dos_memory_lease_table *table, struct dos_memory_arena *arena,
    const struct dos_machine *machine, struct dos_memory_lease_handle handle,
    uint16_t expected_owner, uint16_t new_owner,
    const struct dos_memory_owner_name_patch *name_patch)
{
	struct dos_memory_owner_name_patch stable_patch;
	size_t index;

	if (!owner_name_patch_is_valid(name_patch))
		return DOS_MEMORY_LEASE_INVALID_ARGUMENT;
	for (index = 0u; index < DOS_MEMORY_OWNER_NAME_BYTES; ++index)
		stable_patch.bytes[index] = name_patch->bytes[index];
	stable_patch.count = name_patch->count;
	for (index = 0u; index < ARRAY_SIZE(stable_patch.reserved); ++index)
		stable_patch.reserved[index] = 0u;
	return transfer_owner_common(table, arena, machine, handle,
				     expected_owner, new_owner, NULL,
				     &stable_patch);
}

bool dos_memory_lease_rebind_plan_has_valid_encoding(
    const struct dos_memory_lease_rebind_plan *plan)
{
	if (plan == NULL || plan->handle.value == 0u ||
	    plan->machine_context == KERNEL_OBJECT_HANDLE_INVALID ||
	    plan->arena_identity == 0u ||
	    plan->arena_identity == KERNEL_OBJECT_HANDLE_INVALID ||
	    plan->arena_generation == 0u ||
	    !bytes_are_zero(plan->reserved, ARRAY_SIZE(plan->reserved)) ||
	    !dos_memory_owner_rebind_value_has_valid_encoding(&plan->value))
		return false;
	return plan->value.header_segment ==
	       (uint16_t)(plan->guest_segment - 1u);
}

static enum dos_memory_lease_status prepare_rebind_common(
    const struct dos_memory_lease_table *table,
    const struct dos_memory_arena *arena, const struct dos_machine *machine,
    struct dos_memory_lease_handle handle, uint16_t expected_owner,
    uint16_t new_owner, const struct dos_memory_owner_name_patch *name_patch,
    struct dos_memory_lease_rebind_plan *plan)
{
	struct dos_memory_lease_rebind_plan prepared = {0};
	const struct dos_memory_lease_slot *slot;
	enum dos_memory_lease_status lease_status;
	enum dos_memory_status memory_status;
	uint32_t slot_index;

	if (plan == NULL || new_owner == 0u)
		return DOS_MEMORY_LEASE_INVALID_ARGUMENT;
	lease_status = find_slot(table, arena, machine, handle, expected_owner,
				 &slot_index);
	if (lease_status != DOS_MEMORY_LEASE_OK)
		return lease_status;
	slot = &table->slots[slot_index];
	if (slot->state != DOS_MEMORY_LEASE_SLOT_ACTIVE)
		return DOS_MEMORY_LEASE_INVALID_STATE;
	if (arena->machine_poisoned)
		return DOS_MEMORY_LEASE_MACHINE_POISONED;
	if (name_patch == NULL)
		memory_status = dos_memory_prepare_owner_rebind_checked(
		    arena, machine, slot->guest_segment, expected_owner,
		    new_owner, &prepared.value);
	else
		memory_status =
		    dos_memory_prepare_owner_name_patch_rebind_checked(
			arena, machine, slot->guest_segment, expected_owner,
			new_owner, name_patch, &prepared.value);
	if (memory_status != DOS_MEMORY_OK)
		return memory_status_to_lease_status(memory_status);

	prepared.handle = handle;
	prepared.machine_context = slot->machine_context;
	prepared.arena_identity = slot->arena_identity;
	prepared.arena_generation = slot->arena_generation;
	prepared.guest_segment = slot->guest_segment;
	prepared.paragraphs = slot->paragraphs;
	prepared.arena_head_segment = slot->arena_head_segment;
	if (!dos_memory_lease_rebind_plan_has_valid_encoding(&prepared))
		return DOS_MEMORY_LEASE_ARENA_DAMAGED;
	*plan = prepared;
	return DOS_MEMORY_LEASE_OK;
}

enum dos_memory_lease_status dos_memory_lease_prepare_owner_rebind(
    const struct dos_memory_lease_table *table,
    const struct dos_memory_arena *arena, const struct dos_machine *machine,
    struct dos_memory_lease_handle handle, uint16_t expected_owner,
    uint16_t new_owner, struct dos_memory_lease_rebind_plan *plan)
{
	return prepare_rebind_common(table, arena, machine, handle,
				     expected_owner, new_owner, NULL, plan);
}

enum dos_memory_lease_status
dos_memory_lease_prepare_owner_name_patch_rebind(
    const struct dos_memory_lease_table *table,
    const struct dos_memory_arena *arena, const struct dos_machine *machine,
    struct dos_memory_lease_handle handle, uint16_t expected_owner,
    uint16_t new_owner, const struct dos_memory_owner_name_patch *name_patch,
    struct dos_memory_lease_rebind_plan *plan)
{
	if (!owner_name_patch_is_valid(name_patch))
		return DOS_MEMORY_LEASE_INVALID_ARGUMENT;
	return prepare_rebind_common(table, arena, machine, handle,
				     expected_owner, new_owner, name_patch, plan);
}

enum dos_memory_lease_status dos_memory_lease_preflight_rebind_publish(
    const struct dos_memory_lease_table *table,
    const struct dos_memory_arena *arena, const struct dos_machine *machine,
    const struct dos_memory_lease_rebind_plan *plan)
{
	const struct dos_memory_lease_slot *slot;
	uint32_t slot_index;
	enum dos_memory_lease_status status;

	if (!dos_memory_lease_rebind_plan_has_valid_encoding(plan))
		return DOS_MEMORY_LEASE_INVALID_ARGUMENT;
	status = find_slot(table, arena, machine, plan->handle,
			   plan->value.expected_owner, &slot_index);
	if (status != DOS_MEMORY_LEASE_OK)
		return status;
	slot = &table->slots[slot_index];
	if (arena->machine_poisoned)
		return DOS_MEMORY_LEASE_MACHINE_POISONED;
	if (slot->state != DOS_MEMORY_LEASE_SLOT_ACTIVE)
		return DOS_MEMORY_LEASE_INVALID_STATE;
	if (slot->machine_context != plan->machine_context)
		return DOS_MEMORY_LEASE_CONTEXT_MISMATCH;
	if (slot->arena_identity != plan->arena_identity)
		return DOS_MEMORY_LEASE_IDENTITY_MISMATCH;
	if (slot->arena_generation != plan->arena_generation ||
	    slot->arena_head_segment != plan->arena_head_segment ||
	    slot->guest_segment != plan->guest_segment ||
	    slot->paragraphs != plan->paragraphs)
		return DOS_MEMORY_LEASE_STALE_HANDLE;
	return DOS_MEMORY_LEASE_OK;
}

enum dos_memory_lease_status dos_memory_lease_rebind_publish(
    struct dos_memory_lease_table *table,
    const struct dos_memory_arena *arena, const struct dos_machine *machine,
    const struct dos_memory_lease_rebind_plan *plan)
{
	struct dos_memory_lease_slot *slot;
	uint32_t slot_index;
	enum dos_memory_lease_status status;

	status = dos_memory_lease_preflight_rebind_publish(table, arena, machine,
						   plan);
	if (status != DOS_MEMORY_LEASE_OK)
		return status;
	status = find_slot(table, arena, machine, plan->handle,
			   plan->value.expected_owner, &slot_index);
	if (status != DOS_MEMORY_LEASE_OK)
		return status;
	slot = &table->slots[slot_index];
	/* The caller's observation barrier makes this two-field local publish
	 * indivisible to DOS/IRQ consumers; no callback or failure point exists.
	 */
	slot->owner = plan->value.new_owner;
	slot->state = DOS_MEMORY_LEASE_SLOT_PUBLISHED;
	return DOS_MEMORY_LEASE_OK;
}

enum dos_memory_lease_status dos_memory_lease_preflight_publish(
    const struct dos_memory_lease_table *table,
    const struct dos_memory_arena *arena, const struct dos_machine *machine,
    struct dos_memory_lease_handle handle, uint16_t expected_owner)
{
	uint32_t slot_index;
	enum dos_memory_lease_status status;

	status = find_slot(table, arena, machine, handle, expected_owner,
			   &slot_index);
	if (status != DOS_MEMORY_LEASE_OK)
		return status;
	if (arena->machine_poisoned)
		return DOS_MEMORY_LEASE_MACHINE_POISONED;
	if (table->slots[slot_index].state != DOS_MEMORY_LEASE_SLOT_ACTIVE)
		return DOS_MEMORY_LEASE_INVALID_STATE;
	return DOS_MEMORY_LEASE_OK;
}

enum dos_memory_lease_status dos_memory_lease_publish(
    struct dos_memory_lease_table *table, const struct dos_memory_arena *arena,
    const struct dos_machine *machine, struct dos_memory_lease_handle handle,
    uint16_t expected_owner)
{
	struct dos_memory_lease_slot *slot;
	uint32_t slot_index;
	enum dos_memory_lease_status status;

	status = find_slot(table, arena, machine, handle, expected_owner,
			   &slot_index);
	if (status != DOS_MEMORY_LEASE_OK)
		return status;
	slot = &table->slots[slot_index];
	if (arena->machine_poisoned)
		return DOS_MEMORY_LEASE_MACHINE_POISONED;
	if (slot->state == DOS_MEMORY_LEASE_SLOT_PUBLISHED)
		return DOS_MEMORY_LEASE_OK;
	if (slot->state != DOS_MEMORY_LEASE_SLOT_ACTIVE)
		return DOS_MEMORY_LEASE_INVALID_STATE;
	slot->state = DOS_MEMORY_LEASE_SLOT_PUBLISHED;
	return DOS_MEMORY_LEASE_OK;
}

static enum dos_memory_lease_status finish_active_lease(
    struct dos_memory_lease_table *table, struct dos_memory_arena *arena,
    const struct dos_machine *machine, struct dos_memory_lease_handle handle,
    uint16_t expected_owner, bool aborting)
{
	struct dos_memory_lease_slot *slot;
	enum dos_memory_lease_status lease_status;
	enum dos_memory_status memory_status;
	uint32_t slot_index;

	lease_status = find_slot(table, arena, machine, handle, expected_owner,
				 &slot_index);
	if (lease_status != DOS_MEMORY_LEASE_OK)
		return lease_status;
	slot = &table->slots[slot_index];
	if (aborting && (slot->state == DOS_MEMORY_LEASE_SLOT_ABORTED ||
			 slot->state == DOS_MEMORY_LEASE_SLOT_RELEASED))
		return DOS_MEMORY_LEASE_OK;
	if (slot->state != DOS_MEMORY_LEASE_SLOT_ACTIVE)
		return DOS_MEMORY_LEASE_INVALID_STATE;

	slot->state = DOS_MEMORY_LEASE_SLOT_RELEASING;
	memory_status = dos_memory_free_owned_checked(
	    arena, machine, slot->guest_segment, expected_owner);
	if (memory_status != DOS_MEMORY_OK) {
		slot->state = DOS_MEMORY_LEASE_SLOT_ACTIVE;
		return memory_status_to_lease_status(memory_status);
	}
	slot->state = aborting ? DOS_MEMORY_LEASE_SLOT_ABORTED
			       : DOS_MEMORY_LEASE_SLOT_RELEASED;
	return DOS_MEMORY_LEASE_OK;
}

enum dos_memory_lease_status dos_memory_lease_release(
    struct dos_memory_lease_table *table, struct dos_memory_arena *arena,
    const struct dos_machine *machine, struct dos_memory_lease_handle handle,
    uint16_t expected_owner)
{
	return finish_active_lease(table, arena, machine, handle,
				   expected_owner, false);
}

enum dos_memory_lease_status dos_memory_lease_abort(
    struct dos_memory_lease_table *table, struct dos_memory_arena *arena,
    const struct dos_machine *machine, struct dos_memory_lease_handle handle,
    uint16_t expected_owner)
{
	return finish_active_lease(table, arena, machine, handle,
				   expected_owner, true);
}

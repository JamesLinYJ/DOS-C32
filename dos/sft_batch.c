// SPDX-License-Identifier: GPL-2.0-only
/*
 * Safe MS-DOS EXEC JFT/SFT inheritance.
 *
 * The operation filters a twenty-byte JFT, calls the logical device open only
 * for a local SFT, then increments the reference count once per inherited JFT
 * byte. Rollback applies the inverse order. Failures are typed internal
 * transaction faults.
 */
#include "dos_sft_batch.h"

#define DOS_SFT_BATCH_SLOT_BITS 4u
#define DOS_SFT_BATCH_SLOT_MASK 0x0fu
#define DOS_SFT_BATCH_GENERATION_MAXIMUM 0x0fffffffffffffffull

#define DOS_SFT_ENTRY_DEVICE_OPEN 0x01u
#define DOS_SFT_ENTRY_REFERENCE_ACQUIRED 0x02u

enum internal_batch_state {
	INTERNAL_BATCH_FREE = 0,
	INTERNAL_BATCH_PREPARING,
	INTERNAL_BATCH_PREPARED,
	INTERNAL_BATCH_COMMITTED,
	INTERNAL_BATCH_ABORTED,
	INTERNAL_BATCH_POISONED
};

struct batch_entry {
	dos_sft_reference_handle_t reference_handle;
	uint8_t sfn;
	uint8_t actions;
	uint8_t reserved[6];
};

struct batch_slot {
	struct batch_entry entries[DOS_SFT_BATCH_JFT_ENTRIES];
	struct dos_sft_jft20 child_jft;
	kernel_object_handle_t context;
	kernel_object_handle_t adapter_identity;
	uint64_t generation;
	uint8_t action_count;
	uint8_t inherited_count;
	uint8_t state;
	uint8_t reserved;
};

/* No native pointer is persistent in this registry. */
static struct batch_slot batch_slots[DOS_SFT_BATCH_MAXIMUM];

static bool ops_are_complete(const struct dos_sft_batch_ops *ops)
{
	return ops != NULL && ops->identity != KERNEL_OBJECT_HANDLE_INVALID &&
	       ops->lookup != NULL && ops->device_open != NULL &&
	       ops->reference_acquire != NULL &&
	       ops->reference_release != NULL && ops->device_close != NULL;
}

static dos_sft_batch_handle_t make_handle(uint32_t slot, uint64_t generation)
{
	return (dos_sft_batch_handle_t)(generation << DOS_SFT_BATCH_SLOT_BITS) |
	       (dos_sft_batch_handle_t)(slot + 1u);
}

static enum dos_sft_batch_status resolve_handle(dos_sft_batch_handle_t handle,
						struct batch_slot **slot_out)
{
	uint32_t encoded_slot = (uint32_t)(handle & DOS_SFT_BATCH_SLOT_MASK);
	uint64_t generation = handle >> DOS_SFT_BATCH_SLOT_BITS;
	uint32_t slot;

	if (slot_out == NULL || encoded_slot == 0u || generation == 0u)
		return DOS_SFT_BATCH_STALE_HANDLE;
	slot = encoded_slot - 1u;
	if (slot >= DOS_SFT_BATCH_MAXIMUM ||
	    batch_slots[slot].state == INTERNAL_BATCH_FREE ||
	    batch_slots[slot].generation != generation)
		return DOS_SFT_BATCH_STALE_HANDLE;
	*slot_out = &batch_slots[slot];
	return DOS_SFT_BATCH_OK;
}

static void clear_entry(struct batch_entry *entry)
{
	size_t index;

	entry->reference_handle = DOS_SFT_REFERENCE_HANDLE_INVALID;
	entry->sfn = DOS_JFT_ENTRY_UNUSED;
	entry->actions = 0u;
	for (index = 0u; index < ARRAY_SIZE(entry->reserved); ++index)
		entry->reserved[index] = 0u;
}

static void initialize_slot(struct batch_slot *slot,
			    kernel_object_handle_t context,
			    kernel_object_handle_t adapter_identity)
{
	size_t index;

	for (index = 0u; index < ARRAY_SIZE(slot->entries); ++index)
		clear_entry(&slot->entries[index]);
	for (index = 0u; index < ARRAY_SIZE(slot->child_jft.entries); ++index)
		slot->child_jft.entries[index] = DOS_JFT_ENTRY_UNUSED;
	slot->context = context;
	slot->adapter_identity = adapter_identity;
	slot->action_count = 0u;
	slot->inherited_count = 0u;
	slot->state = INTERNAL_BATCH_PREPARING;
	slot->reserved = 0u;
}

static void recycle_slot(struct batch_slot *slot)
{
	size_t index;

	for (index = 0u; index < ARRAY_SIZE(slot->entries); ++index)
		clear_entry(&slot->entries[index]);
	for (index = 0u; index < ARRAY_SIZE(slot->child_jft.entries); ++index)
		slot->child_jft.entries[index] = DOS_JFT_ENTRY_UNUSED;
	slot->context = KERNEL_OBJECT_HANDLE_INVALID;
	slot->adapter_identity = KERNEL_OBJECT_HANDLE_INVALID;
	slot->action_count = 0u;
	slot->inherited_count = 0u;
	slot->state = INTERNAL_BATCH_FREE;
	slot->reserved = 0u;
}

/* Continue all reverse releases after one failure; any failure is sticky. */
static bool unwind_entries(struct batch_slot *slot,
			   const struct dos_sft_batch_ops *ops,
			   kernel_object_handle_t context)
{
	bool undo_failed = false;
	uint32_t remaining = slot->action_count;

	while (remaining != 0u) {
		struct batch_entry *entry = &slot->entries[remaining - 1u];

		if ((entry->actions & DOS_SFT_ENTRY_REFERENCE_ACQUIRED) != 0u) {
			if (ops->reference_release(context,
						   entry->reference_handle) ==
			    DOS_SFT_ADAPTER_OK)
				entry->actions &=
				    (uint8_t)~DOS_SFT_ENTRY_REFERENCE_ACQUIRED;
			else
				undo_failed = true;
		}
		/* DEV_CLOSE depends on Free_SFT having completed for this
		 * entry. */
		if ((entry->actions & DOS_SFT_ENTRY_REFERENCE_ACQUIRED) == 0u &&
		    (entry->actions & DOS_SFT_ENTRY_DEVICE_OPEN) != 0u) {
			if (ops->device_close(context,
					      entry->reference_handle) ==
			    DOS_SFT_ADAPTER_OK)
				entry->actions &=
				    (uint8_t)~DOS_SFT_ENTRY_DEVICE_OPEN;
			else
				undo_failed = true;
		}
		--remaining;
	}
	return !undo_failed;
}

static bool sft_is_inheritable(const struct dos_sft_view *view)
{
	return (view->flags & DOS_SFT_FLAG_NO_INHERIT) == 0u &&
	       (view->mode & DOS_SFT_SHARING_MASK) !=
		   DOS_SFT_SHARING_NETWORK_FCB;
}

static enum dos_sft_batch_status
fail_prepare(struct batch_slot *slot, const struct dos_sft_batch_ops *ops,
	     kernel_object_handle_t context, dos_sft_batch_handle_t handle,
	     dos_sft_batch_handle_t *batch_handle)
{
	if (!unwind_entries(slot, ops, context)) {
		slot->state = INTERNAL_BATCH_POISONED;
		*batch_handle = handle;
		return DOS_SFT_BATCH_POISONED;
	}
	recycle_slot(slot);
	return DOS_SFT_BATCH_ADAPTER_FAULT;
}

enum dos_sft_batch_status
dos_sft_batch_prepare(const struct dos_sft_batch_ops *ops,
		      kernel_object_handle_t context,
		      const struct dos_sft_jft20 *parent_jft,
		      dos_sft_batch_handle_t *batch_handle)
{
	struct batch_slot *slot;
	dos_sft_batch_handle_t handle;
	uint32_t slot_index;
	size_t index;

	if (batch_handle != NULL)
		*batch_handle = DOS_SFT_BATCH_HANDLE_INVALID;
	if (!ops_are_complete(ops) || context == KERNEL_OBJECT_HANDLE_INVALID ||
	    parent_jft == NULL || batch_handle == NULL)
		return DOS_SFT_BATCH_INVALID_ARGUMENT;

	for (slot_index = 0u; slot_index < DOS_SFT_BATCH_MAXIMUM;
	     ++slot_index) {
		if (batch_slots[slot_index].state == INTERNAL_BATCH_FREE &&
		    batch_slots[slot_index].generation <
			DOS_SFT_BATCH_GENERATION_MAXIMUM)
			break;
	}
	if (slot_index == DOS_SFT_BATCH_MAXIMUM)
		return DOS_SFT_BATCH_NO_SLOT;

	slot = &batch_slots[slot_index];
	++slot->generation;
	initialize_slot(slot, context, ops->identity);
	handle = make_handle(slot_index, slot->generation);

	for (index = 0u; index < DOS_SFT_BATCH_JFT_ENTRIES; ++index) {
		struct dos_sft_view view = {
		    .reference_handle = DOS_SFT_REFERENCE_HANDLE_INVALID,
		    .flags = 0u,
		    .mode = 0u,
		};
		struct batch_entry *entry;
		enum dos_sft_adapter_status adapter_status;
		uint8_t sfn = parent_jft->entries[index];

		if (sfn == DOS_JFT_ENTRY_UNUSED)
			continue;
		adapter_status = ops->lookup(context, sfn, &view);
		if (adapter_status == DOS_SFT_ADAPTER_INVALID_SFT)
			continue;
		if (adapter_status != DOS_SFT_ADAPTER_OK ||
		    view.reference_handle == DOS_SFT_REFERENCE_HANDLE_INVALID)
			return fail_prepare(slot, ops, context, handle,
					    batch_handle);
		if (!sft_is_inheritable(&view))
			continue;

		entry = &slot->entries[slot->action_count];
		++slot->action_count;
		entry->reference_handle = view.reference_handle;
		entry->sfn = sfn;
		if ((view.flags & DOS_SFT_FLAG_IS_NETWORK) == 0u) {
			adapter_status =
			    ops->device_open(context, view.reference_handle);
			if (adapter_status != DOS_SFT_ADAPTER_OK)
				return fail_prepare(slot, ops, context, handle,
						    batch_handle);
			entry->actions |= DOS_SFT_ENTRY_DEVICE_OPEN;
		}
		adapter_status =
		    ops->reference_acquire(context, view.reference_handle);
		if (adapter_status != DOS_SFT_ADAPTER_OK)
			return fail_prepare(slot, ops, context, handle,
					    batch_handle);
		entry->actions |= DOS_SFT_ENTRY_REFERENCE_ACQUIRED;
		slot->child_jft.entries[index] = sfn;
		++slot->inherited_count;
	}

	slot->state = INTERNAL_BATCH_PREPARED;
	*batch_handle = handle;
	return DOS_SFT_BATCH_OK;
}

enum dos_sft_batch_status
dos_sft_batch_copy_child_jft(dos_sft_batch_handle_t batch_handle,
			     struct dos_sft_jft20 *child_jft)
{
	struct batch_slot *slot;
	enum dos_sft_batch_status status;
	size_t index;

	if (child_jft == NULL)
		return DOS_SFT_BATCH_INVALID_ARGUMENT;
	status = resolve_handle(batch_handle, &slot);
	if (status != DOS_SFT_BATCH_OK)
		return status;
	if (slot->state == INTERNAL_BATCH_POISONED)
		return DOS_SFT_BATCH_POISONED;
	if (slot->state != INTERNAL_BATCH_PREPARED &&
	    slot->state != INTERNAL_BATCH_COMMITTED)
		return DOS_SFT_BATCH_INVALID_STATE;
	for (index = 0u; index < ARRAY_SIZE(slot->child_jft.entries); ++index)
		child_jft->entries[index] = slot->child_jft.entries[index];
	return DOS_SFT_BATCH_OK;
}

enum dos_sft_batch_status
dos_sft_batch_preflight_commit(dos_sft_batch_handle_t batch_handle)
{
	struct batch_slot *slot;
	enum dos_sft_batch_status status = resolve_handle(batch_handle, &slot);

	if (status != DOS_SFT_BATCH_OK)
		return status;
	if (slot->state == INTERNAL_BATCH_POISONED)
		return DOS_SFT_BATCH_POISONED;
	if (slot->state != INTERNAL_BATCH_PREPARED)
		return DOS_SFT_BATCH_INVALID_STATE;
	return DOS_SFT_BATCH_OK;
}

enum dos_sft_batch_status
dos_sft_batch_commit(dos_sft_batch_handle_t batch_handle)
{
	struct batch_slot *slot;
	enum dos_sft_batch_status status = resolve_handle(batch_handle, &slot);

	if (status != DOS_SFT_BATCH_OK)
		return status;
	/* Preserve the established idempotent committed-state contract. */
	if (slot->state == INTERNAL_BATCH_COMMITTED)
		return DOS_SFT_BATCH_OK;
	status = dos_sft_batch_preflight_commit(batch_handle);
	if (status != DOS_SFT_BATCH_OK)
		return status;
	slot->state = INTERNAL_BATCH_COMMITTED;
	return DOS_SFT_BATCH_OK;
}

enum dos_sft_batch_status
dos_sft_batch_abort(dos_sft_batch_handle_t batch_handle,
		    const struct dos_sft_batch_ops *ops,
		    kernel_object_handle_t context)
{
	struct batch_slot *slot;
	enum dos_sft_batch_status status = resolve_handle(batch_handle, &slot);

	if (status != DOS_SFT_BATCH_OK)
		return status;
	if (slot->state == INTERNAL_BATCH_POISONED)
		return DOS_SFT_BATCH_POISONED;
	if (context != slot->context || ops == NULL ||
	    ops->identity != slot->adapter_identity)
		return DOS_SFT_BATCH_INVALID_ARGUMENT;
	if (slot->state == INTERNAL_BATCH_COMMITTED)
		return DOS_SFT_BATCH_ALREADY_COMMITTED;
	if (slot->state == INTERNAL_BATCH_ABORTED)
		return DOS_SFT_BATCH_OK;
	if (slot->state != INTERNAL_BATCH_PREPARED)
		return DOS_SFT_BATCH_INVALID_STATE;
	if (!ops_are_complete(ops))
		return DOS_SFT_BATCH_INVALID_ARGUMENT;
	if (!unwind_entries(slot, ops, context)) {
		slot->state = INTERNAL_BATCH_POISONED;
		return DOS_SFT_BATCH_POISONED;
	}
	slot->state = INTERNAL_BATCH_ABORTED;
	return DOS_SFT_BATCH_OK;
}

enum dos_sft_batch_status
dos_sft_batch_preflight_retire(dos_sft_batch_handle_t batch_handle)
{
	struct batch_slot *slot;
	enum dos_sft_batch_status status = resolve_handle(batch_handle, &slot);

	if (status != DOS_SFT_BATCH_OK)
		return status;
	if (slot->state == INTERNAL_BATCH_POISONED)
		return DOS_SFT_BATCH_POISONED;
	return slot->state == INTERNAL_BATCH_ABORTED ||
		       slot->state == INTERNAL_BATCH_COMMITTED
		   ? DOS_SFT_BATCH_OK
		   : DOS_SFT_BATCH_INVALID_STATE;
}

enum dos_sft_batch_status
dos_sft_batch_retire(dos_sft_batch_handle_t batch_handle)
{
	struct batch_slot *slot;
	enum dos_sft_batch_status status =
	    dos_sft_batch_preflight_retire(batch_handle);

	if (status != DOS_SFT_BATCH_OK)
		return status;
	status = resolve_handle(batch_handle, &slot);
	if (status != DOS_SFT_BATCH_OK)
		return status;
	recycle_slot(slot);
	return DOS_SFT_BATCH_OK;
}

enum dos_sft_batch_status
dos_sft_batch_get_state(dos_sft_batch_handle_t batch_handle,
			enum dos_sft_batch_state *state)
{
	struct batch_slot *slot;
	enum dos_sft_batch_status status;

	if (state == NULL)
		return DOS_SFT_BATCH_INVALID_ARGUMENT;
	status = resolve_handle(batch_handle, &slot);
	if (status != DOS_SFT_BATCH_OK)
		return status;
	switch (slot->state) {
	case INTERNAL_BATCH_PREPARED:
		*state = DOS_SFT_BATCH_STATE_PREPARED;
		return DOS_SFT_BATCH_OK;
	case INTERNAL_BATCH_COMMITTED:
		*state = DOS_SFT_BATCH_STATE_COMMITTED;
		return DOS_SFT_BATCH_OK;
	case INTERNAL_BATCH_ABORTED:
		*state = DOS_SFT_BATCH_STATE_ABORTED;
		return DOS_SFT_BATCH_OK;
	case INTERNAL_BATCH_POISONED:
		*state = DOS_SFT_BATCH_STATE_POISONED;
		return DOS_SFT_BATCH_OK;
	default:
		return DOS_SFT_BATCH_INVALID_STATE;
	}
}

static_assert_expression(
    sizeof(struct batch_entry) == 16,
    "batch entries must contain fixed-width integers only");

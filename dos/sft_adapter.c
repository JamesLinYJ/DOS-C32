// SPDX-License-Identifier: GPL-2.0-only
/*
 * Canonical SFT registry and EXEC lifetime adapter.
 *
 * Compatibility contract: JFT aliases share one SFT cursor and backend lifetime
 * Safety changes: fixed generations, reserve/publish, exact close rollback
 */
#include "dos_sft_adapter.h"

#define DOS_SFT_HANDLE_SLOT_BITS 9u
#define DOS_SFT_HANDLE_SLOT_MASK 0x1ffull
#define DOS_SFT_HANDLE_GENERATION_MAX 0x007fffffffffffffull
#define DOS_SFT_COUNTER_MAX (~(uint32_t)0u)

struct dos_sft_slot {
	uint64_t generation;
	kernel_object_handle_t backend_handle;
	uint64_t position;
	uint64_t size;
	uint32_t references;
	uint32_t device_opens;
	uint16_t flags;
	uint16_t mode;
	uint16_t information;
	uint8_t backend_kind;
	uint8_t state;
} __aligned(8);

struct dos_sft_registry_owner {
	struct dos_sft_slot slots[DOS_SFT_REGISTRY_SLOT_COUNT];
	struct dos_sft_backend_close_ops close_ops;
	kernel_object_handle_t context;
	uint8_t initialized;
	uint8_t close_bound;
	uint8_t reserved[6];
} __aligned(8);

static struct dos_sft_registry_owner owner;

static enum dos_sft_adapter_status registry_lookup(
	kernel_object_handle_t context, uint8_t sfn, struct dos_sft_view *view);
static enum dos_sft_adapter_status registry_device_open(
	kernel_object_handle_t context, dos_sft_reference_handle_t reference);
static enum dos_sft_adapter_status registry_reference_acquire(
	kernel_object_handle_t context, dos_sft_reference_handle_t reference);
static enum dos_sft_adapter_status registry_reference_release(
	kernel_object_handle_t context, dos_sft_reference_handle_t reference);
static enum dos_sft_adapter_status registry_device_close(
	kernel_object_handle_t context, dos_sft_reference_handle_t reference);

static struct dos_sft_batch_ops operations = {
	.identity = KERNEL_OBJECT_HANDLE_INVALID,
	.lookup = registry_lookup,
	.device_open = registry_device_open,
	.reference_acquire = registry_reference_acquire,
	.reference_release = registry_reference_release,
	.device_close = registry_device_close,
};

static bool identity_is_valid(kernel_object_handle_t identity)
{
	return identity != 0u && identity != KERNEL_OBJECT_HANDLE_INVALID;
}

static bool backend_kind_is_valid(uint8_t backend_kind)
{
	return backend_kind == (uint8_t)DOS_SFT_BACKEND_STANDARD ||
	       backend_kind == (uint8_t)DOS_SFT_BACKEND_FILE ||
	       backend_kind == (uint8_t)DOS_SFT_BACKEND_DEVICE;
}

static bool exact_error_is_valid(enum dos_error error)
{
	uint32_t value = (uint32_t)error;

	return value != (uint32_t)DOS_SUCCESS && value <= 0xffffu;
}

static bool slot_is_local(const struct dos_sft_slot *slot)
{
	return (slot->flags & DOS_SFT_FLAG_IS_NETWORK) == 0u;
}

static dos_sft_reference_handle_t make_reference(uint8_t sfn,
						 uint64_t generation)
{
	return (generation << DOS_SFT_HANDLE_SLOT_BITS) |
	       (dos_sft_reference_handle_t)((uint16_t)sfn + 1u);
}

static enum dos_sft_registry_status validate_context(
	kernel_object_handle_t context)
{
	if (owner.initialized != 1u)
		return DOS_SFT_REGISTRY_INVALID_STATE;
	return context == owner.context ? DOS_SFT_REGISTRY_READY
					: DOS_SFT_REGISTRY_CONTEXT_MISMATCH;
}

static enum dos_sft_registry_status resolve_reference(
	kernel_object_handle_t context, dos_sft_reference_handle_t reference,
	uint8_t *sfn, struct dos_sft_slot **slot_out)
{
	enum dos_sft_registry_status status = validate_context(context);
	uint32_t encoded_slot;
	uint32_t slot_index;
	uint64_t generation;
	struct dos_sft_slot *slot;

	if (status != DOS_SFT_REGISTRY_READY)
		return status;
	if (sfn == NULL || slot_out == NULL ||
	    reference == DOS_SFT_REFERENCE_HANDLE_INVALID)
		return DOS_SFT_REGISTRY_INVALID_ARGUMENT;
	encoded_slot = (uint32_t)(reference & DOS_SFT_HANDLE_SLOT_MASK);
	generation = reference >> DOS_SFT_HANDLE_SLOT_BITS;
	if (encoded_slot == 0u ||
	    encoded_slot > DOS_SFT_REGISTRY_SLOT_COUNT || generation == 0u ||
	    generation > DOS_SFT_HANDLE_GENERATION_MAX)
		return DOS_SFT_REGISTRY_STALE_REFERENCE;
	slot_index = encoded_slot - 1u;
	slot = &owner.slots[slot_index];
	if (slot->state == (uint8_t)DOS_SFT_SLOT_FREE ||
	    slot->generation != generation)
		return DOS_SFT_REGISTRY_STALE_REFERENCE;
	*sfn = (uint8_t)slot_index;
	*slot_out = slot;
	return DOS_SFT_REGISTRY_READY;
}

static void recycle_slot(struct dos_sft_slot *slot)
{
	uint64_t generation = slot->generation;

	*slot = (struct dos_sft_slot){
		.generation = generation,
		.backend_handle = KERNEL_OBJECT_HANDLE_INVALID,
		.state = (uint8_t)DOS_SFT_SLOT_FREE,
	};
}

static void poison_slot(struct dos_sft_slot *slot)
{
	slot->state = (uint8_t)DOS_SFT_SLOT_POISONED;
}

static void fill_view(uint8_t sfn, const struct dos_sft_slot *slot,
		      struct dos_sft_registry_view *view)
{
	dos_sft_reference_handle_t reference = DOS_SFT_REFERENCE_HANDLE_INVALID;

	if (slot->state != (uint8_t)DOS_SFT_SLOT_FREE)
		reference = make_reference(sfn, slot->generation);
	*view = (struct dos_sft_registry_view){
		.reference_handle = reference,
		.backend_handle = slot->backend_handle,
		.position = slot->position,
		.size = slot->size,
		.references = slot->references,
		.device_opens = slot->device_opens,
		.flags = slot->flags,
		.mode = slot->mode,
		.information = slot->information,
		.backend_kind = slot->backend_kind,
		.state = slot->state,
	};
}

static enum dos_sft_registry_status invoke_backend_close(
	struct dos_sft_slot *slot, enum dos_error *exact_error)
{
	enum dos_sft_backend_close_status close_status;
	enum dos_error callback_error = DOS_SUCCESS;

	if (owner.close_bound != 1u)
		return DOS_SFT_REGISTRY_INVALID_STATE;
	close_status = owner.close_ops.close(
		owner.close_ops.context,
		(enum dos_sft_backend_kind)slot->backend_kind,
		slot->backend_handle, &callback_error);
	switch (close_status) {
	case DOS_SFT_BACKEND_CLOSE_OK:
		if (callback_error == DOS_SUCCESS) {
			*exact_error = DOS_SUCCESS;
			return DOS_SFT_REGISTRY_READY;
		}
		break;
	case DOS_SFT_BACKEND_CLOSE_EXACT_FAILURE:
		if (exact_error_is_valid(callback_error)) {
			*exact_error = callback_error;
			return DOS_SFT_REGISTRY_BACKEND_FAILURE;
		}
		break;
	case DOS_SFT_BACKEND_CLOSE_UNCERTAIN:
		break;
	default:
		break;
	}
	poison_slot(slot);
	return DOS_SFT_REGISTRY_POISONED;
}

enum dos_sft_registry_status dos_sft_registry_initialize(
	kernel_object_handle_t adapter_identity,
	kernel_object_handle_t adapter_context)
{
	uint32_t index;

	if (!identity_is_valid(adapter_identity) ||
	    !identity_is_valid(adapter_context))
		return DOS_SFT_REGISTRY_INVALID_ARGUMENT;
	if (owner.initialized != 0u)
		return DOS_SFT_REGISTRY_INVALID_STATE;
	for (index = 0u; index < DOS_SFT_REGISTRY_SLOT_COUNT; ++index) {
		owner.slots[index] = (struct dos_sft_slot){
			.backend_handle = KERNEL_OBJECT_HANDLE_INVALID,
			.state = (uint8_t)DOS_SFT_SLOT_FREE,
		};
	}
	owner.close_ops = (struct dos_sft_backend_close_ops){
		.identity = KERNEL_OBJECT_HANDLE_INVALID,
		.context = KERNEL_OBJECT_HANDLE_INVALID,
		.close = NULL,
	};
	owner.context = adapter_context;
	owner.initialized = 1u;
	owner.close_bound = 0u;
	for (index = 0u; index < ARRAY_SIZE(owner.reserved); ++index)
		owner.reserved[index] = 0u;
	operations.identity = adapter_identity;
	return DOS_SFT_REGISTRY_READY;
}

enum dos_sft_registry_status dos_sft_registry_bind_backend_close(
	kernel_object_handle_t context,
	const struct dos_sft_backend_close_ops *ops)
{
	enum dos_sft_registry_status status = validate_context(context);

	if (status != DOS_SFT_REGISTRY_READY)
		return status;
	if (ops == NULL || !identity_is_valid(ops->identity) ||
	    !identity_is_valid(ops->context) || ops->close == NULL)
		return DOS_SFT_REGISTRY_INVALID_ARGUMENT;
	if (owner.close_bound != 0u)
		return DOS_SFT_REGISTRY_INVALID_STATE;
	owner.close_ops = *ops;
	owner.close_bound = 1u;
	return DOS_SFT_REGISTRY_READY;
}

enum dos_sft_registry_status dos_sft_registry_install(
	uint8_t sfn, uint16_t flags, uint16_t mode, uint32_t initial_references)
{
	struct dos_sft_slot *slot;

	if (owner.initialized != 1u || initial_references == 0u)
		return DOS_SFT_REGISTRY_INVALID_ARGUMENT;
	slot = &owner.slots[sfn];
	if (slot->state != (uint8_t)DOS_SFT_SLOT_FREE)
		return DOS_SFT_REGISTRY_INVALID_STATE;
	if (slot->generation >= DOS_SFT_HANDLE_GENERATION_MAX)
		return DOS_SFT_REGISTRY_GENERATION_EXHAUSTED;
	++slot->generation;
	slot->backend_handle = KERNEL_OBJECT_HANDLE_INVALID;
	slot->position = 0u;
	slot->size = 0u;
	slot->references = initial_references;
	slot->device_opens = (flags & DOS_SFT_FLAG_IS_NETWORK) == 0u
				     ? initial_references
				     : 0u;
	slot->flags = flags;
	slot->mode = mode;
	slot->information = 0u;
	slot->backend_kind = (uint8_t)DOS_SFT_BACKEND_STANDARD;
	slot->state = (uint8_t)DOS_SFT_SLOT_PRESENT;
	return DOS_SFT_REGISTRY_READY;
}

enum dos_sft_registry_status dos_sft_registry_reserve(
	kernel_object_handle_t context, uint8_t *sfn,
	dos_sft_reference_handle_t *reference_handle)
{
	enum dos_sft_registry_status status = validate_context(context);
	bool exhausted_slot_seen = false;
	uint32_t index;

	if (status != DOS_SFT_REGISTRY_READY)
		return status;
	if (sfn == NULL || reference_handle == NULL)
		return DOS_SFT_REGISTRY_INVALID_ARGUMENT;
	/* FFh is the JFT unused marker and cannot name a live SFT. */
	for (index = 0u; index < DOS_JFT_ENTRY_UNUSED; ++index) {
		struct dos_sft_slot *slot = &owner.slots[index];
		dos_sft_reference_handle_t prepared_reference;
		uint64_t generation;

		if (slot->state != (uint8_t)DOS_SFT_SLOT_FREE)
			continue;
		if (slot->generation >= DOS_SFT_HANDLE_GENERATION_MAX) {
			exhausted_slot_seen = true;
			continue;
		}
		++slot->generation;
		generation = slot->generation;
		prepared_reference = make_reference((uint8_t)index,
						    generation);
		*slot = (struct dos_sft_slot){
			.generation = generation,
			.backend_handle = KERNEL_OBJECT_HANDLE_INVALID,
			.state = (uint8_t)DOS_SFT_SLOT_RESERVED,
		};
		*sfn = (uint8_t)index;
		*reference_handle = prepared_reference;
		return DOS_SFT_REGISTRY_READY;
	}
	return exhausted_slot_seen ? DOS_SFT_REGISTRY_GENERATION_EXHAUSTED
				   : DOS_SFT_REGISTRY_NO_SLOT;
}

enum dos_sft_registry_status dos_sft_registry_publish(
	kernel_object_handle_t context,
	dos_sft_reference_handle_t reference_handle,
	const struct dos_sft_registry_publish_record *record)
{
	struct dos_sft_slot *slot;
	enum dos_sft_registry_status status;
	uint8_t sfn;

	if (record == NULL || record->reserved != 0u ||
	    !backend_kind_is_valid(record->backend_kind))
		return DOS_SFT_REGISTRY_INVALID_ARGUMENT;
	if (record->backend_kind != (uint8_t)DOS_SFT_BACKEND_STANDARD &&
	    record->backend_handle == KERNEL_OBJECT_HANDLE_INVALID)
		return DOS_SFT_REGISTRY_INVALID_ARGUMENT;
	status = resolve_reference(context, reference_handle, &sfn, &slot);
	if (status != DOS_SFT_REGISTRY_READY)
		return status;
	if (slot->state != (uint8_t)DOS_SFT_SLOT_RESERVED)
		return slot->state == (uint8_t)DOS_SFT_SLOT_POISONED
			       ? DOS_SFT_REGISTRY_POISONED
			       : DOS_SFT_REGISTRY_INVALID_STATE;
	if (owner.close_bound != 1u)
		return DOS_SFT_REGISTRY_INVALID_STATE;
	slot->backend_handle = record->backend_handle;
	slot->position = record->position;
	slot->size = record->size;
	slot->references = 1u;
	slot->device_opens =
		(record->flags & DOS_SFT_FLAG_IS_NETWORK) == 0u ? 1u : 0u;
	slot->flags = record->flags;
	slot->mode = record->mode;
	slot->information = record->information;
	slot->backend_kind = record->backend_kind;
	slot->state = (uint8_t)DOS_SFT_SLOT_PRESENT;
	(void)sfn;
	return DOS_SFT_REGISTRY_READY;
}

enum dos_sft_registry_status dos_sft_registry_cancel(
	kernel_object_handle_t context,
	dos_sft_reference_handle_t reference_handle)
{
	struct dos_sft_slot *slot;
	enum dos_sft_registry_status status;
	uint8_t sfn;

	status = resolve_reference(context, reference_handle, &sfn, &slot);
	if (status != DOS_SFT_REGISTRY_READY)
		return status;
	if (slot->state != (uint8_t)DOS_SFT_SLOT_RESERVED)
		return slot->state == (uint8_t)DOS_SFT_SLOT_POISONED
			       ? DOS_SFT_REGISTRY_POISONED
			       : DOS_SFT_REGISTRY_INVALID_STATE;
	recycle_slot(slot);
	(void)sfn;
	return DOS_SFT_REGISTRY_READY;
}

enum dos_sft_registry_status dos_sft_registry_resolve(
	kernel_object_handle_t context, uint8_t sfn,
	struct dos_sft_registry_view *view)
{
	const struct dos_sft_slot *slot;
	enum dos_sft_registry_status status = validate_context(context);
	struct dos_sft_registry_view prepared;

	if (status != DOS_SFT_REGISTRY_READY)
		return status;
	if (view == NULL)
		return DOS_SFT_REGISTRY_INVALID_ARGUMENT;
	slot = &owner.slots[sfn];
	if (slot->state == (uint8_t)DOS_SFT_SLOT_POISONED)
		return DOS_SFT_REGISTRY_POISONED;
	if (slot->state != (uint8_t)DOS_SFT_SLOT_PRESENT ||
	    slot->references == 0u)
		return DOS_SFT_REGISTRY_INVALID_STATE;
	fill_view(sfn, slot, &prepared);
	*view = prepared;
	return DOS_SFT_REGISTRY_READY;
}

enum dos_sft_registry_status dos_sft_registry_inspect_open(
	kernel_object_handle_t context,
	dos_sft_reference_handle_t reference_handle,
	struct dos_sft_registry_view *view)
{
	struct dos_sft_slot *slot;
	enum dos_sft_registry_status status;
	struct dos_sft_registry_view prepared;
	uint8_t sfn;

	if (view == NULL)
		return DOS_SFT_REGISTRY_INVALID_ARGUMENT;
	status = resolve_reference(context, reference_handle, &sfn, &slot);
	if (status != DOS_SFT_REGISTRY_READY)
		return status;
	if (slot->state == (uint8_t)DOS_SFT_SLOT_POISONED)
		return DOS_SFT_REGISTRY_POISONED;
	if (slot->state != (uint8_t)DOS_SFT_SLOT_PRESENT ||
	    slot->references == 0u)
		return DOS_SFT_REGISTRY_INVALID_STATE;
	fill_view(sfn, slot, &prepared);
	*view = prepared;
	return DOS_SFT_REGISTRY_READY;
}

enum dos_sft_registry_status dos_sft_registry_update_io(
	kernel_object_handle_t context,
	dos_sft_reference_handle_t reference_handle, uint64_t position,
	uint64_t size, uint16_t information)
{
	struct dos_sft_slot *slot;
	enum dos_sft_registry_status status;
	uint8_t sfn;

	status = resolve_reference(context, reference_handle, &sfn, &slot);
	if (status != DOS_SFT_REGISTRY_READY)
		return status;
	if (slot->state == (uint8_t)DOS_SFT_SLOT_POISONED)
		return DOS_SFT_REGISTRY_POISONED;
	if (slot->state != (uint8_t)DOS_SFT_SLOT_PRESENT ||
	    slot->references == 0u)
		return DOS_SFT_REGISTRY_INVALID_STATE;
	slot->position = position;
	slot->size = size;
	slot->information = information;
	(void)sfn;
	return DOS_SFT_REGISTRY_READY;
}

enum dos_sft_registry_status dos_sft_registry_close_reference(
	kernel_object_handle_t context,
	dos_sft_reference_handle_t reference_handle,
	enum dos_error *exact_error)
{
	struct dos_sft_slot *slot;
	enum dos_sft_registry_status status;
	uint8_t sfn;

	if (exact_error == NULL)
		return DOS_SFT_REGISTRY_INVALID_ARGUMENT;
	status = resolve_reference(context, reference_handle, &sfn, &slot);
	if (status != DOS_SFT_REGISTRY_READY)
		return status;
	if (slot->state == (uint8_t)DOS_SFT_SLOT_POISONED)
		return DOS_SFT_REGISTRY_POISONED;
	if (slot->state != (uint8_t)DOS_SFT_SLOT_PRESENT ||
	    slot->references == 0u)
		return DOS_SFT_REGISTRY_INVALID_STATE;
	if ((slot_is_local(slot) &&
	     slot->device_opens != slot->references) ||
	    (!slot_is_local(slot) && slot->device_opens != 0u)) {
		poison_slot(slot);
		return DOS_SFT_REGISTRY_POISONED;
	}
	if (slot->references > 1u) {
		--slot->references;
		if (slot_is_local(slot))
			--slot->device_opens;
		*exact_error = DOS_SUCCESS;
		return DOS_SFT_REGISTRY_READY;
	}
	status = invoke_backend_close(slot, exact_error);
	if (status == DOS_SFT_REGISTRY_READY)
		recycle_slot(slot);
	(void)sfn;
	return status;
}

enum dos_sft_registry_status dos_sft_registry_poison(
	kernel_object_handle_t context,
	dos_sft_reference_handle_t reference_handle)
{
	struct dos_sft_slot *slot;
	enum dos_sft_registry_status status;
	uint8_t sfn;

	status = resolve_reference(context, reference_handle, &sfn, &slot);
	if (status != DOS_SFT_REGISTRY_READY)
		return status;
	if (slot->state == (uint8_t)DOS_SFT_SLOT_POISONED)
		return DOS_SFT_REGISTRY_POISONED;
	if (slot->state != (uint8_t)DOS_SFT_SLOT_RESERVED &&
	    slot->state != (uint8_t)DOS_SFT_SLOT_PRESENT)
		return DOS_SFT_REGISTRY_INVALID_STATE;
	poison_slot(slot);
	(void)sfn;
	return DOS_SFT_REGISTRY_READY;
}

enum dos_sft_registry_status dos_sft_registry_inspect(
	uint8_t sfn, struct dos_sft_registry_view *view)
{
	struct dos_sft_registry_view prepared;

	if (owner.initialized != 1u || view == NULL)
		return DOS_SFT_REGISTRY_INVALID_ARGUMENT;
	fill_view(sfn, &owner.slots[sfn], &prepared);
	*view = prepared;
	return DOS_SFT_REGISTRY_READY;
}

const struct dos_sft_batch_ops *dos_sft_registry_ops(void)
{
	return owner.initialized == 1u ? &operations : NULL;
}

kernel_object_handle_t dos_sft_registry_context(void)
{
	return owner.initialized == 1u ? owner.context
				      : KERNEL_OBJECT_HANDLE_INVALID;
}

static enum dos_sft_adapter_status registry_lookup(
	kernel_object_handle_t context, uint8_t sfn, struct dos_sft_view *view)
{
	struct dos_sft_registry_view registry_view;
	enum dos_sft_registry_status status;
	struct dos_sft_view prepared;

	if (view == NULL)
		return DOS_SFT_ADAPTER_FAULT;
	status = dos_sft_registry_resolve(context, sfn, &registry_view);
	if (status == DOS_SFT_REGISTRY_INVALID_STATE)
		return DOS_SFT_ADAPTER_INVALID_SFT;
	if (status != DOS_SFT_REGISTRY_READY)
		return DOS_SFT_ADAPTER_FAULT;
	prepared = (struct dos_sft_view){
		.reference_handle = registry_view.reference_handle,
		.flags = registry_view.flags,
		.mode = registry_view.mode,
	};
	*view = prepared;
	return DOS_SFT_ADAPTER_OK;
}

static enum dos_sft_adapter_status registry_device_open(
	kernel_object_handle_t context, dos_sft_reference_handle_t reference)
{
	struct dos_sft_slot *slot;
	enum dos_sft_registry_status status;
	uint8_t sfn;

	status = resolve_reference(context, reference, &sfn, &slot);
	if (status != DOS_SFT_REGISTRY_READY ||
	    slot->state != (uint8_t)DOS_SFT_SLOT_PRESENT ||
	    slot->references == 0u || !slot_is_local(slot) ||
	    slot->device_opens != slot->references ||
	    slot->device_opens == DOS_SFT_COUNTER_MAX)
		return DOS_SFT_ADAPTER_FAULT;
	++slot->device_opens;
	(void)sfn;
	return DOS_SFT_ADAPTER_OK;
}

static enum dos_sft_adapter_status registry_reference_acquire(
	kernel_object_handle_t context, dos_sft_reference_handle_t reference)
{
	struct dos_sft_slot *slot;
	enum dos_sft_registry_status status;
	uint8_t sfn;

	status = resolve_reference(context, reference, &sfn, &slot);
	if (status != DOS_SFT_REGISTRY_READY ||
	    slot->state != (uint8_t)DOS_SFT_SLOT_PRESENT ||
	    slot->references == 0u || slot->references == DOS_SFT_COUNTER_MAX)
		return DOS_SFT_ADAPTER_FAULT;
	if ((slot_is_local(slot) &&
	     slot->device_opens != slot->references + 1u) ||
	    (!slot_is_local(slot) && slot->device_opens != 0u))
		return DOS_SFT_ADAPTER_FAULT;
	++slot->references;
	(void)sfn;
	return DOS_SFT_ADAPTER_OK;
}

static enum dos_sft_adapter_status registry_reference_release(
	kernel_object_handle_t context, dos_sft_reference_handle_t reference)
{
	struct dos_sft_slot *slot;
	enum dos_sft_registry_status status;
	enum dos_error exact_error = DOS_SUCCESS;
	uint8_t sfn;

	status = resolve_reference(context, reference, &sfn, &slot);
	if (status != DOS_SFT_REGISTRY_READY ||
	    slot->state != (uint8_t)DOS_SFT_SLOT_PRESENT ||
	    slot->references == 0u)
		return DOS_SFT_ADAPTER_FAULT;
	if ((slot_is_local(slot) &&
	     slot->device_opens != slot->references) ||
	    (!slot_is_local(slot) && slot->device_opens != 0u))
		return DOS_SFT_ADAPTER_FAULT;
	if (!slot_is_local(slot) && slot->references == 1u) {
		status = invoke_backend_close(slot, &exact_error);
		if (status == DOS_SFT_REGISTRY_READY) {
			recycle_slot(slot);
			return DOS_SFT_ADAPTER_OK;
		}
		poison_slot(slot);
		return DOS_SFT_ADAPTER_FAULT;
	}
	--slot->references;
	(void)sfn;
	return DOS_SFT_ADAPTER_OK;
}

static enum dos_sft_adapter_status registry_device_close(
	kernel_object_handle_t context, dos_sft_reference_handle_t reference)
{
	struct dos_sft_slot *slot;
	enum dos_sft_registry_status status;
	enum dos_error exact_error = DOS_SUCCESS;
	uint8_t sfn;

	status = resolve_reference(context, reference, &sfn, &slot);
	if (status != DOS_SFT_REGISTRY_READY ||
	    slot->state != (uint8_t)DOS_SFT_SLOT_PRESENT ||
	    !slot_is_local(slot) || slot->device_opens == 0u ||
	    slot->device_opens - 1u != slot->references)
		return DOS_SFT_ADAPTER_FAULT;
	if (slot->references != 0u) {
		--slot->device_opens;
		return DOS_SFT_ADAPTER_OK;
	}
	status = invoke_backend_close(slot, &exact_error);
	if (status == DOS_SFT_REGISTRY_READY) {
		recycle_slot(slot);
		return DOS_SFT_ADAPTER_OK;
	}
	poison_slot(slot);
	(void)sfn;
	return DOS_SFT_ADAPTER_FAULT;
}

static_assert_expression(sizeof(struct dos_sft_slot) == 48u,
	"SFT slots must stay data-model independent");
static_assert_expression(__alignof__(struct dos_sft_slot) == 8u,
	"SFT slot alignment changed");
static_assert_expression(
	__builtin_offsetof(struct dos_sft_slot, backend_handle) == 8u,
	"SFT slot backend-handle offset changed");
static_assert_expression(__builtin_offsetof(struct dos_sft_slot, position) ==
			 16u,
	"SFT slot position offset changed");
static_assert_expression(
	__builtin_offsetof(struct dos_sft_slot, references) == 32u,
	"SFT slot reference-count offset changed");
static_assert_expression(
	__builtin_offsetof(struct dos_sft_slot, information) == 44u,
	"SFT slot information offset changed");

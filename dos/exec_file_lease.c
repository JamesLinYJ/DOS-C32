// SPDX-License-Identifier: GPL-2.0-only
/*
 * Checked OPEN -> IOCTL device probe -> immutable read -> CLOSE ownership.
 * MS-DOS supplies the order and visible error policy; this file supplies
 * only bounded borrowing, fixed slots, generations, and explicit uncertainty.
 */
#include "dos_exec_file_lease.h"

static bool identity_is_valid(kernel_object_handle_t identity)
{
	return identity != 0u && identity != KERNEL_OBJECT_HANDLE_INVALID;
}

static bool ops_are_complete(const struct dos_exec_file_lease_ops *ops)
{
	return ops != NULL && identity_is_valid(ops->identity) &&
	       ops->open != NULL && ops->probe_device != NULL &&
	       ops->read != NULL && ops->close != NULL;
}

static bool bytes_are_zero(const uint8_t *bytes, size_t count)
{
	size_t index;

	for (index = 0u; index < count; ++index) {
		if (bytes[index] != 0u)
			return false;
	}
	return true;
}

static bool state_is_valid(uint8_t state)
{
	return state <= (uint8_t)DOS_EXEC_FILE_LEASE_STATE_POISONED;
}

static bool slot_has_valid_encoding(const struct dos_exec_file_lease_slot *slot)
{
	bool has_adapter;
	bool has_reader;

	if (slot == NULL ||
	    slot->generation > DOS_EXEC_FILE_LEASE_GENERATION_MAX ||
	    !state_is_valid(slot->state) || slot->is_device > 1u ||
	    !bytes_are_zero(slot->reserved, ARRAY_SIZE(slot->reserved)))
		return false;
	has_adapter = identity_is_valid(slot->adapter_identity) &&
		      slot->adapter_context != KERNEL_OBJECT_HANDLE_INVALID;
	has_reader = slot->reader_context != 0u &&
		     slot->reader_context != KERNEL_OBJECT_HANDLE_INVALID;
	switch (slot->state) {
	case DOS_EXEC_FILE_LEASE_STATE_VACANT:
		return slot->adapter_identity == KERNEL_OBJECT_HANDLE_INVALID &&
		       slot->adapter_context == KERNEL_OBJECT_HANDLE_INVALID &&
		       slot->reader_context == KERNEL_OBJECT_HANDLE_INVALID &&
		       slot->size == 0u && slot->is_device == 0u;
	case DOS_EXEC_FILE_LEASE_STATE_OPENING:
		return slot->generation != 0u && has_adapter &&
		       slot->reader_context == KERNEL_OBJECT_HANDLE_INVALID &&
		       slot->size == 0u && slot->is_device == 0u;
	case DOS_EXEC_FILE_LEASE_STATE_OPEN:
	case DOS_EXEC_FILE_LEASE_STATE_PROBING:
		return slot->generation != 0u && has_adapter && has_reader &&
		       slot->is_device == 0u;
	case DOS_EXEC_FILE_LEASE_STATE_PROBED:
	case DOS_EXEC_FILE_LEASE_STATE_CLOSING:
	case DOS_EXEC_FILE_LEASE_STATE_CLOSED:
		return slot->generation != 0u && has_adapter && has_reader;
	case DOS_EXEC_FILE_LEASE_STATE_POISONED:
		/* A malformed successful OPEN may have no usable reader handle.
		 */
		return slot->generation != 0u && has_adapter;
	default:
		return false;
	}
}

static void clear_slot_binding(struct dos_exec_file_lease_slot *slot)
{
	size_t index;

	slot->adapter_identity = KERNEL_OBJECT_HANDLE_INVALID;
	slot->adapter_context = KERNEL_OBJECT_HANDLE_INVALID;
	slot->reader_context = KERNEL_OBJECT_HANDLE_INVALID;
	slot->size = 0u;
	slot->is_device = 0u;
	for (index = 0u; index < ARRAY_SIZE(slot->reserved); ++index)
		slot->reserved[index] = 0u;
}

static void initialize_slot(struct dos_exec_file_lease_slot *slot)
{
	slot->generation = 0u;
	clear_slot_binding(slot);
	slot->state = (uint8_t)DOS_EXEC_FILE_LEASE_STATE_VACANT;
}

static void vacate_slot(struct dos_exec_file_lease_slot *slot)
{
	clear_slot_binding(slot);
	slot->state = (uint8_t)DOS_EXEC_FILE_LEASE_STATE_VACANT;
}

static enum dos_exec_file_lease_status
validate_table(const struct dos_exec_file_lease_table *table)
{
	size_t index;

	if (table == NULL)
		return DOS_EXEC_FILE_LEASE_INVALID_ARGUMENT;
	if (table->constructed != 1u || table->initialized > 1u ||
	    !bytes_are_zero(table->reserved, ARRAY_SIZE(table->reserved)))
		return DOS_EXEC_FILE_LEASE_INVALID_ARGUMENT;
	if (table->initialized == 0u)
		return DOS_EXEC_FILE_LEASE_NOT_INITIALIZED;
	if (!identity_is_valid(table->identity))
		return DOS_EXEC_FILE_LEASE_POISONED;
	for (index = 0u; index < ARRAY_SIZE(table->slots); ++index) {
		if (!slot_has_valid_encoding(&table->slots[index]))
			return DOS_EXEC_FILE_LEASE_POISONED;
	}
	return DOS_EXEC_FILE_LEASE_OK;
}

static struct dos_exec_file_lease_handle make_handle(uint32_t slot_index,
						     uint64_t generation)
{
	struct dos_exec_file_lease_handle handle;

	handle.value = (generation << DOS_EXEC_FILE_LEASE_SLOT_BITS) |
		       (uint64_t)(slot_index + 1u);
	return handle;
}

static enum dos_exec_file_lease_status
decode_handle(struct dos_exec_file_lease_handle handle, uint32_t *slot_index,
	      uint64_t *generation)
{
	uint32_t encoded_slot;
	uint64_t decoded_generation;

	if (slot_index == NULL || generation == NULL || handle.value == 0u ||
	    handle.value == KERNEL_OBJECT_HANDLE_INVALID)
		return DOS_EXEC_FILE_LEASE_STALE_HANDLE;
	encoded_slot = (uint32_t)(handle.value & DOS_EXEC_FILE_LEASE_SLOT_MASK);
	decoded_generation = handle.value >> DOS_EXEC_FILE_LEASE_SLOT_BITS;
	if (encoded_slot == 0u ||
	    encoded_slot > DOS_EXEC_FILE_LEASE_SLOT_COUNT ||
	    decoded_generation == 0u ||
	    decoded_generation > DOS_EXEC_FILE_LEASE_GENERATION_MAX)
		return DOS_EXEC_FILE_LEASE_STALE_HANDLE;
	*slot_index = encoded_slot - 1u;
	*generation = decoded_generation;
	return DOS_EXEC_FILE_LEASE_OK;
}

static enum dos_exec_file_lease_status
find_slot(const struct dos_exec_file_lease_table *table,
	  struct dos_exec_file_lease_handle handle, uint32_t *slot_index_out)
{
	const struct dos_exec_file_lease_slot *slot;
	enum dos_exec_file_lease_status status;
	uint64_t generation;
	uint32_t slot_index;

	if (slot_index_out == NULL)
		return DOS_EXEC_FILE_LEASE_INVALID_ARGUMENT;
	status = validate_table(table);
	if (status != DOS_EXEC_FILE_LEASE_OK)
		return status;
	status = decode_handle(handle, &slot_index, &generation);
	if (status != DOS_EXEC_FILE_LEASE_OK)
		return status;
	slot = &table->slots[slot_index];
	if (slot->generation != generation ||
	    slot->state == (uint8_t)DOS_EXEC_FILE_LEASE_STATE_VACANT)
		return DOS_EXEC_FILE_LEASE_STALE_HANDLE;
	*slot_index_out = slot_index;
	return DOS_EXEC_FILE_LEASE_OK;
}

static enum dos_exec_file_lease_status
find_bound_slot(const struct dos_exec_file_lease_table *table,
		struct dos_exec_file_lease_handle handle,
		const struct dos_exec_file_lease_ops *ops,
		kernel_object_handle_t context, uint32_t *slot_index_out)
{
	const struct dos_exec_file_lease_slot *slot;
	enum dos_exec_file_lease_status status;
	uint32_t slot_index;

	if (!ops_are_complete(ops) || context == KERNEL_OBJECT_HANDLE_INVALID)
		return DOS_EXEC_FILE_LEASE_INVALID_ARGUMENT;
	if (slot_index_out == NULL)
		return DOS_EXEC_FILE_LEASE_INVALID_ARGUMENT;
	status = find_slot(table, handle, &slot_index);
	if (status != DOS_EXEC_FILE_LEASE_OK)
		return status;
	slot = &table->slots[slot_index];
	if (slot->adapter_identity != ops->identity)
		return DOS_EXEC_FILE_LEASE_IDENTITY_MISMATCH;
	if (slot->adapter_context != context)
		return DOS_EXEC_FILE_LEASE_CONTEXT_MISMATCH;
	if (slot->state == (uint8_t)DOS_EXEC_FILE_LEASE_STATE_POISONED)
		return DOS_EXEC_FILE_LEASE_POISONED;
	*slot_index_out = slot_index;
	return DOS_EXEC_FILE_LEASE_OK;
}

enum dos_exec_file_lease_status
dos_exec_file_lease_table_construct(struct dos_exec_file_lease_table *table)
{
	size_t index;

	if (table == NULL)
		return DOS_EXEC_FILE_LEASE_INVALID_ARGUMENT;
	for (index = 0u; index < ARRAY_SIZE(table->slots); ++index)
		initialize_slot(&table->slots[index]);
	table->identity = KERNEL_OBJECT_HANDLE_INVALID;
	table->initialized = 0u;
	table->constructed = 1u;
	for (index = 0u; index < ARRAY_SIZE(table->reserved); ++index)
		table->reserved[index] = 0u;
	return DOS_EXEC_FILE_LEASE_OK;
}

enum dos_exec_file_lease_status
dos_exec_file_lease_table_initialize(struct dos_exec_file_lease_table *table,
				     kernel_object_handle_t identity)
{
	size_t index;

	if (table == NULL || table->constructed != 1u ||
	    table->initialized > 1u || !identity_is_valid(identity) ||
	    !bytes_are_zero(table->reserved, ARRAY_SIZE(table->reserved)))
		return DOS_EXEC_FILE_LEASE_INVALID_ARGUMENT;
	if (table->initialized != 0u)
		return DOS_EXEC_FILE_LEASE_INVALID_STATE;
	if (table->identity != KERNEL_OBJECT_HANDLE_INVALID)
		return DOS_EXEC_FILE_LEASE_INVALID_ARGUMENT;
	for (index = 0u; index < ARRAY_SIZE(table->slots); ++index)
		initialize_slot(&table->slots[index]);
	table->identity = identity;
	table->initialized = 1u;
	return DOS_EXEC_FILE_LEASE_OK;
}

bool dos_exec_file_lease_table_is_drained(
    const struct dos_exec_file_lease_table *table)
{
	size_t index;

	if (validate_table(table) != DOS_EXEC_FILE_LEASE_OK)
		return false;
	for (index = 0u; index < ARRAY_SIZE(table->slots); ++index) {
		const struct dos_exec_file_lease_slot *slot =
		    &table->slots[index];

		if (!state_is_valid(slot->state) ||
		    !bytes_are_zero(slot->reserved, ARRAY_SIZE(slot->reserved)))
			return false;
		if (slot->state != (uint8_t)DOS_EXEC_FILE_LEASE_STATE_VACANT &&
		    slot->state != (uint8_t)DOS_EXEC_FILE_LEASE_STATE_CLOSED)
			return false;
	}
	return true;
}

static enum dos_exec_file_lease_status
reserve_slot(struct dos_exec_file_lease_table *table,
	     const struct dos_exec_file_lease_ops *ops,
	     kernel_object_handle_t context, uint32_t *slot_index)
{
	bool every_generation_exhausted = true;
	uint32_t index;

	for (index = 0u; index < DOS_EXEC_FILE_LEASE_SLOT_COUNT; ++index) {
		struct dos_exec_file_lease_slot *slot = &table->slots[index];

		if (slot->generation < DOS_EXEC_FILE_LEASE_GENERATION_MAX)
			every_generation_exhausted = false;
		if (slot->state != (uint8_t)DOS_EXEC_FILE_LEASE_STATE_VACANT ||
		    slot->generation >= DOS_EXEC_FILE_LEASE_GENERATION_MAX ||
		    !bytes_are_zero(slot->reserved, ARRAY_SIZE(slot->reserved)))
			continue;
		++slot->generation;
		slot->adapter_identity = ops->identity;
		slot->adapter_context = context;
		slot->reader_context = KERNEL_OBJECT_HANDLE_INVALID;
		slot->size = 0u;
		slot->is_device = 0u;
		slot->state = (uint8_t)DOS_EXEC_FILE_LEASE_STATE_OPENING;
		*slot_index = index;
		return DOS_EXEC_FILE_LEASE_OK;
	}
	return every_generation_exhausted
		   ? DOS_EXEC_FILE_LEASE_GENERATION_EXHAUSTED
		   : DOS_EXEC_FILE_LEASE_NO_SLOT;
}

enum dos_exec_file_lease_status dos_exec_file_lease_acquire(
    struct dos_exec_file_lease_table *table,
    const struct dos_exec_file_lease_ops *ops, kernel_object_handle_t context,
    const uint8_t *path, size_t path_length,
    struct dos_exec_file_lease_handle *handle, uint32_t *open_failure_detail)
{
	struct dos_exec_file_open_result opened = {
	    .reader_context = KERNEL_OBJECT_HANDLE_INVALID,
	    .size = 0u,
	    .failure_detail = 0u,
	    .reserved = 0u,
	};
	struct dos_exec_file_lease_handle prepared_handle;
	struct dos_exec_file_lease_slot *slot;
	enum dos_exec_file_adapter_status adapter_status;
	enum dos_exec_file_lease_status status;
	uint32_t slot_index;

	if (!ops_are_complete(ops) || context == KERNEL_OBJECT_HANDLE_INVALID ||
	    path == NULL || handle == NULL || open_failure_detail == NULL)
		return DOS_EXEC_FILE_LEASE_INVALID_ARGUMENT;
	status = validate_table(table);
	if (status != DOS_EXEC_FILE_LEASE_OK)
		return status;
	status = reserve_slot(table, ops, context, &slot_index);
	if (status != DOS_EXEC_FILE_LEASE_OK)
		return status;
	slot = &table->slots[slot_index];
	prepared_handle = make_handle(slot_index, slot->generation);

	adapter_status = ops->open(context, path, path_length, &opened);
	if (adapter_status == DOS_EXEC_FILE_ADAPTER_FAULT) {
		uint32_t failure_detail = opened.failure_detail;

		vacate_slot(slot);
		*open_failure_detail = failure_detail;
		return DOS_EXEC_FILE_LEASE_OPEN_FAILED;
	}
	if (adapter_status != DOS_EXEC_FILE_ADAPTER_OK) {
		/* An enum outside the adapter contract cannot prove whether
		 * OPEN acquired ownership. */
		slot->reader_context = opened.reader_context;
		slot->size = opened.size;
		slot->state = (uint8_t)DOS_EXEC_FILE_LEASE_STATE_POISONED;
		return DOS_EXEC_FILE_LEASE_POISONED;
	}
	if (opened.reader_context == 0u ||
	    opened.reader_context == KERNEL_OBJECT_HANDLE_INVALID ||
	    opened.failure_detail != 0u || opened.reserved != 0u) {
		/* OPEN claimed ownership but returned a malformed capability
		 * value. */
		slot->reader_context = opened.reader_context;
		slot->size = opened.size;
		slot->state = (uint8_t)DOS_EXEC_FILE_LEASE_STATE_POISONED;
		*open_failure_detail = 0u;
		return DOS_EXEC_FILE_LEASE_POISONED;
	}

	slot->reader_context = opened.reader_context;
	slot->size = opened.size;
	slot->state = (uint8_t)DOS_EXEC_FILE_LEASE_STATE_OPEN;
	*open_failure_detail = 0u;
	*handle = prepared_handle;
	return DOS_EXEC_FILE_LEASE_OK;
}

enum dos_exec_file_lease_status dos_exec_file_lease_probe_device(
    struct dos_exec_file_lease_table *table,
    struct dos_exec_file_lease_handle handle,
    const struct dos_exec_file_lease_ops *ops, kernel_object_handle_t context,
    uint8_t *is_device, uint32_t *probe_failure_detail)
{
	struct dos_exec_file_probe_result probed = {
	    .failure_detail = 0u,
	    .is_device = 0xffu,
	    .reserved = {0u},
	};
	struct dos_exec_file_lease_slot *slot;
	enum dos_exec_file_adapter_status adapter_status;
	enum dos_exec_file_lease_status status;
	uint32_t slot_index;

	if (is_device == NULL || probe_failure_detail == NULL)
		return DOS_EXEC_FILE_LEASE_INVALID_ARGUMENT;
	status = find_bound_slot(table, handle, ops, context, &slot_index);
	if (status != DOS_EXEC_FILE_LEASE_OK)
		return status;
	slot = &table->slots[slot_index];
	if (slot->state == (uint8_t)DOS_EXEC_FILE_LEASE_STATE_PROBED) {
		*is_device = slot->is_device;
		*probe_failure_detail = 0u;
		return DOS_EXEC_FILE_LEASE_OK;
	}
	if (slot->state != (uint8_t)DOS_EXEC_FILE_LEASE_STATE_OPEN)
		return DOS_EXEC_FILE_LEASE_INVALID_STATE;

	slot->state = (uint8_t)DOS_EXEC_FILE_LEASE_STATE_PROBING;
	adapter_status =
	    ops->probe_device(context, slot->reader_context, &probed);
	if (adapter_status == DOS_EXEC_FILE_ADAPTER_FAULT) {
		if (!bytes_are_zero(probed.reserved, sizeof(probed.reserved))) {
			slot->state =
			    (uint8_t)DOS_EXEC_FILE_LEASE_STATE_POISONED;
			return DOS_EXEC_FILE_LEASE_POISONED;
		}
		slot->state = (uint8_t)DOS_EXEC_FILE_LEASE_STATE_OPEN;
		*probe_failure_detail = probed.failure_detail;
		return DOS_EXEC_FILE_LEASE_PROBE_FAILED;
	}
	if (adapter_status != DOS_EXEC_FILE_ADAPTER_OK) {
		slot->state = (uint8_t)DOS_EXEC_FILE_LEASE_STATE_POISONED;
		return DOS_EXEC_FILE_LEASE_POISONED;
	}
	if (probed.failure_detail != 0u || probed.is_device > 1u ||
	    !bytes_are_zero(probed.reserved, sizeof(probed.reserved))) {
		slot->state = (uint8_t)DOS_EXEC_FILE_LEASE_STATE_POISONED;
		return DOS_EXEC_FILE_LEASE_POISONED;
	}
	slot->is_device = probed.is_device;
	slot->state = (uint8_t)DOS_EXEC_FILE_LEASE_STATE_PROBED;
	*is_device = probed.is_device;
	*probe_failure_detail = 0u;
	return DOS_EXEC_FILE_LEASE_OK;
}

enum dos_exec_file_lease_status
dos_exec_file_lease_query_device(const struct dos_exec_file_lease_table *table,
				 struct dos_exec_file_lease_handle handle,
				 const struct dos_exec_file_lease_ops *ops,
				 kernel_object_handle_t context,
				 uint8_t *is_device)
{
	const struct dos_exec_file_lease_slot *slot;
	enum dos_exec_file_lease_status status;
	uint32_t slot_index;

	if (is_device == NULL)
		return DOS_EXEC_FILE_LEASE_INVALID_ARGUMENT;
	status = find_bound_slot(table, handle, ops, context, &slot_index);
	if (status != DOS_EXEC_FILE_LEASE_OK)
		return status;
	slot = &table->slots[slot_index];
	if (slot->state != (uint8_t)DOS_EXEC_FILE_LEASE_STATE_PROBED)
		return DOS_EXEC_FILE_LEASE_INVALID_STATE;
	*is_device = slot->is_device;
	return DOS_EXEC_FILE_LEASE_OK;
}

enum dos_exec_file_lease_status dos_exec_file_lease_resolve_reader(
    const struct dos_exec_file_lease_table *table,
    struct dos_exec_file_lease_handle handle,
    const struct dos_exec_file_lease_ops *ops, kernel_object_handle_t context,
    struct dos_image_reader *reader)
{
	struct dos_image_reader prepared;
	const struct dos_exec_file_lease_slot *slot;
	enum dos_exec_file_lease_status status;
	uint32_t slot_index;

	if (reader == NULL)
		return DOS_EXEC_FILE_LEASE_INVALID_ARGUMENT;
	status = find_bound_slot(table, handle, ops, context, &slot_index);
	if (status != DOS_EXEC_FILE_LEASE_OK)
		return status;
	slot = &table->slots[slot_index];
	if (slot->state != (uint8_t)DOS_EXEC_FILE_LEASE_STATE_PROBED)
		return DOS_EXEC_FILE_LEASE_INVALID_STATE;
	if (slot->is_device != 0u)
		return DOS_EXEC_FILE_LEASE_IS_DEVICE;

	prepared.context = slot->reader_context;
	prepared.size = slot->size;
	prepared.read = ops->read;
	reader->context = prepared.context;
	reader->size = prepared.size;
	reader->read = prepared.read;
	return DOS_EXEC_FILE_LEASE_OK;
}

static enum dos_exec_file_lease_status
finish_lease(struct dos_exec_file_lease_table *table,
	     struct dos_exec_file_lease_handle handle,
	     const struct dos_exec_file_lease_ops *ops,
	     kernel_object_handle_t context)
{
	struct dos_exec_file_lease_slot *slot;
	enum dos_exec_file_close_result close_result;
	enum dos_exec_file_lease_status status;
	uint32_t slot_index;
	uint8_t previous_state;

	status = find_bound_slot(table, handle, ops, context, &slot_index);
	if (status != DOS_EXEC_FILE_LEASE_OK)
		return status;
	slot = &table->slots[slot_index];
	if (slot->state == (uint8_t)DOS_EXEC_FILE_LEASE_STATE_CLOSED)
		return DOS_EXEC_FILE_LEASE_OK;
	if (slot->state != (uint8_t)DOS_EXEC_FILE_LEASE_STATE_OPEN &&
	    slot->state != (uint8_t)DOS_EXEC_FILE_LEASE_STATE_PROBED)
		return DOS_EXEC_FILE_LEASE_INVALID_STATE;

	previous_state = slot->state;
	slot->state = (uint8_t)DOS_EXEC_FILE_LEASE_STATE_CLOSING;
	close_result = ops->close(context, slot->reader_context);
	if (close_result == DOS_EXEC_FILE_CLOSE_CLOSED) {
		slot->state = (uint8_t)DOS_EXEC_FILE_LEASE_STATE_CLOSED;
		return DOS_EXEC_FILE_LEASE_OK;
	}
	if (close_result == DOS_EXEC_FILE_CLOSE_RETAINED) {
		slot->state = previous_state;
		return DOS_EXEC_FILE_LEASE_CLOSE_RETAINED;
	}
	/* UNCERTAIN and every invalid adapter encoding fail closed. */
	slot->state = (uint8_t)DOS_EXEC_FILE_LEASE_STATE_POISONED;
	return DOS_EXEC_FILE_LEASE_POISONED;
}

enum dos_exec_file_lease_status
dos_exec_file_lease_close(struct dos_exec_file_lease_table *table,
			  struct dos_exec_file_lease_handle handle,
			  const struct dos_exec_file_lease_ops *ops,
			  kernel_object_handle_t context)
{
	return finish_lease(table, handle, ops, context);
}

enum dos_exec_file_lease_status
dos_exec_file_lease_abort(struct dos_exec_file_lease_table *table,
			  struct dos_exec_file_lease_handle handle,
			  const struct dos_exec_file_lease_ops *ops,
			  kernel_object_handle_t context)
{
	return finish_lease(table, handle, ops, context);
}

enum dos_exec_file_lease_status
dos_exec_file_lease_preflight_retire(
    const struct dos_exec_file_lease_table *table,
    struct dos_exec_file_lease_handle handle)
{
	uint32_t slot_index;
	enum dos_exec_file_lease_status status =
	    find_slot(table, handle, &slot_index);

	if (status != DOS_EXEC_FILE_LEASE_OK)
		return status;
	if (table->slots[slot_index].state ==
	    (uint8_t)DOS_EXEC_FILE_LEASE_STATE_POISONED)
		return DOS_EXEC_FILE_LEASE_POISONED;
	return table->slots[slot_index].state ==
		       (uint8_t)DOS_EXEC_FILE_LEASE_STATE_CLOSED
		   ? DOS_EXEC_FILE_LEASE_OK
		   : DOS_EXEC_FILE_LEASE_INVALID_STATE;
}

enum dos_exec_file_lease_status
dos_exec_file_lease_retire(struct dos_exec_file_lease_table *table,
			   struct dos_exec_file_lease_handle handle)
{
	struct dos_exec_file_lease_slot *slot;
	enum dos_exec_file_lease_status status;
	uint32_t slot_index;

	status = dos_exec_file_lease_preflight_retire(table, handle);
	if (status != DOS_EXEC_FILE_LEASE_OK)
		return status;
	status = find_slot(table, handle, &slot_index);
	if (status != DOS_EXEC_FILE_LEASE_OK)
		return status;
	slot = &table->slots[slot_index];
	vacate_slot(slot);
	return DOS_EXEC_FILE_LEASE_OK;
}

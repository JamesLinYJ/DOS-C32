// SPDX-License-Identifier: GPL-2.0-only
/*
 * Fixed-lifetime x86 I/O-port ownership and dispatch.
 *
 * Safety design: one boot-lifetime owner, a bitmap as allocation truth,
 * non-wrapping slot generations, direction-specific policy, and explicit
 * foreground leases before any callback classified as native I/O.
 */
#include "x86_io_resource.h"

#define X86_IO_RESOURCE_SLOT_BITS 6u
#define X86_IO_RESOURCE_SLOT_MASK 0x3full
#define X86_IO_FOREGROUND_TOKEN_DOMAIN (1ull << 63)
#define X86_IO_RESOURCE_GENERATION_MAX                                   \
	((X86_IO_FOREGROUND_TOKEN_DOMAIN - 1u) >> X86_IO_RESOURCE_SLOT_BITS)

struct x86_io_resource_slot {
	struct x86_io_resource_descriptor descriptor;
	uint64_t generation;
	uint64_t foreground_generation;
	kernel_object_handle_t foreground_requester;
	x86_io_foreground_token_t foreground_token;
} __aligned(8);

struct x86_io_resource_owner {
	struct x86_io_resource_slot slots[X86_IO_RESOURCE_REGISTRY_CAPACITY];
	kernel_object_handle_t identity;
	uint32_t active_bitmap;
	uint8_t initialized;
	uint8_t dispatch_active;
	uint8_t reserved[2];
} __aligned(8);

static struct x86_io_resource_owner owner;

static bool identity_is_valid(kernel_object_handle_t identity)
{
	return identity != 0u && identity != KERNEL_OBJECT_HANDLE_INVALID;
}

static bool width_is_valid(enum dos_io_width width)
{
	return width == DOS_IO_WIDTH_8 || width == DOS_IO_WIDTH_16 ||
	       width == DOS_IO_WIDTH_32;
}

static uint8_t width_mask(enum dos_io_width width)
{
	switch (width) {
	case DOS_IO_WIDTH_8:
		return X86_IO_WIDTH_MASK_8;
	case DOS_IO_WIDTH_16:
		return X86_IO_WIDTH_MASK_16;
	case DOS_IO_WIDTH_32:
		return X86_IO_WIDTH_MASK_32;
	}
	return 0u;
}

static bool action_is_valid(uint8_t action)
{
	return action <= (uint8_t)X86_IO_RESOURCE_ACTION_NATIVE;
}

static bool reserved_is_zero(const uint8_t *bytes, size_t count)
{
	size_t index;

	for (index = 0u; index < count; ++index) {
		if (bytes[index] != 0u)
			return false;
	}
	return true;
}

static bool range_supports_width_mask(uint16_t first, uint16_t last,
				      uint8_t mask)
{
	uint32_t count = (uint32_t)last - (uint32_t)first + 1u;

	if ((mask & X86_IO_WIDTH_MASK_16) != 0u && count < DOS_IO_WIDTH_16)
		return false;
	if ((mask & X86_IO_WIDTH_MASK_32) != 0u && count < DOS_IO_WIDTH_32)
		return false;
	return true;
}

static bool direction_is_valid(uint8_t width_mask_value, uint8_t action,
			       bool callback_present, uint8_t flags)
{
	bool callback_action;

	if ((width_mask_value & (uint8_t)~X86_IO_WIDTH_MASK_ALL) != 0u ||
	    !action_is_valid(action))
		return false;
	callback_action = action == (uint8_t)X86_IO_RESOURCE_ACTION_EMULATE ||
			  action == (uint8_t)X86_IO_RESOURCE_ACTION_NATIVE;
	if (width_mask_value == 0u)
		return action == (uint8_t)X86_IO_RESOURCE_ACTION_DENY &&
		       !callback_present;
	if (callback_action != callback_present)
		return false;
	if (action == (uint8_t)X86_IO_RESOURCE_ACTION_NATIVE &&
	    (flags & X86_IO_RESOURCE_FLAG_FOREGROUND) == 0u)
		return false;
	return true;
}

static bool descriptor_is_valid(
	const struct x86_io_resource_descriptor *descriptor)
{
	if (descriptor == NULL ||
	    !identity_is_valid(descriptor->owner_identity) ||
	    descriptor->first_port > descriptor->last_port ||
	    (descriptor->flags & (uint8_t)~X86_IO_RESOURCE_FLAG_MASK) != 0u ||
	    !reserved_is_zero(descriptor->reserved,
			      ARRAY_SIZE(descriptor->reserved)) ||
	    !range_supports_width_mask(descriptor->first_port,
				       descriptor->last_port,
				       descriptor->read_width_mask) ||
	    !range_supports_width_mask(descriptor->first_port,
				       descriptor->last_port,
				       descriptor->write_width_mask) ||
	    !direction_is_valid(descriptor->read_width_mask,
				descriptor->read_action,
				descriptor->read != NULL,
				descriptor->flags) ||
	    !direction_is_valid(descriptor->write_width_mask,
				descriptor->write_action,
				descriptor->write != NULL,
				descriptor->flags))
		return false;
	if ((descriptor->read != NULL || descriptor->write != NULL) !=
	    identity_is_valid(descriptor->callback_context))
		return false;
	return true;
}

static bool slot_is_active(size_t index)
{
	return (owner.active_bitmap & ((uint32_t)1u << index)) != 0u;
}

static bool ranges_overlap(uint16_t first_a, uint16_t last_a,
			   uint16_t first_b, uint16_t last_b)
{
	return first_a <= last_b && first_b <= last_a;
}

static uint64_t make_generation_handle(size_t index, uint64_t generation,
				       uint64_t domain)
{
	return domain | (generation << X86_IO_RESOURCE_SLOT_BITS) |
	       (uint64_t)(index + 1u);
}

static bool decode_resource(x86_io_resource_handle_t resource,
			    size_t *slot_index)
{
	uint64_t generation;
	uint32_t encoded_slot;

	if (slot_index == NULL || resource == X86_IO_RESOURCE_HANDLE_INVALID ||
	    resource == KERNEL_OBJECT_HANDLE_INVALID ||
	    (resource & X86_IO_FOREGROUND_TOKEN_DOMAIN) != 0u)
		return false;
	encoded_slot = (uint32_t)(resource & X86_IO_RESOURCE_SLOT_MASK);
	generation = resource >> X86_IO_RESOURCE_SLOT_BITS;
	if (encoded_slot == 0u ||
	    encoded_slot > X86_IO_RESOURCE_REGISTRY_CAPACITY ||
	    generation == 0u || generation > X86_IO_RESOURCE_GENERATION_MAX ||
	    !slot_is_active((size_t)encoded_slot - 1u) ||
	    owner.slots[encoded_slot - 1u].generation != generation)
		return false;
	*slot_index = (size_t)encoded_slot - 1u;
	return true;
}

static bool active_range_overlaps(uint16_t first, uint16_t last)
{
	size_t index;

	for (index = 0u; index < X86_IO_RESOURCE_REGISTRY_CAPACITY; ++index) {
		const struct x86_io_resource_descriptor *descriptor;

		if (!slot_is_active(index))
			continue;
		descriptor = &owner.slots[index].descriptor;
		if (ranges_overlap(first, last, descriptor->first_port,
				   descriptor->last_port))
			return true;
	}
	return false;
}

static bool reserve_slot(uint32_t excluded_bitmap, size_t *slot_index,
			 uint64_t *generation)
{
	size_t index;

	for (index = 0u; index < X86_IO_RESOURCE_REGISTRY_CAPACITY; ++index) {
		struct x86_io_resource_slot *slot = &owner.slots[index];

		if (slot_is_active(index) ||
		    (excluded_bitmap & ((uint32_t)1u << index)) != 0u ||
		    slot->generation >= X86_IO_RESOURCE_GENERATION_MAX)
			continue;
		*slot_index = index;
		*generation = slot->generation + 1u;
		return true;
	}
	return false;
}

enum x86_io_resource_status x86_io_resource_registry_initialize(
	kernel_object_handle_t registry_identity)
{
	if (!identity_is_valid(registry_identity))
		return X86_IO_RESOURCE_INVALID_ARGUMENT;
	if (owner.initialized != 0u)
		return X86_IO_RESOURCE_INVALID_STATE;
	owner.identity = registry_identity;
	owner.active_bitmap = 0u;
	owner.dispatch_active = 0u;
	owner.initialized = 1u;
	return X86_IO_RESOURCE_OK;
}

kernel_object_handle_t x86_io_resource_registry_identity(void)
{
	return owner.initialized == 1u ? owner.identity
				      : KERNEL_OBJECT_HANDLE_INVALID;
}

enum x86_io_resource_status x86_io_resource_register(
	const struct x86_io_resource_descriptor *descriptor,
	x86_io_resource_handle_t *resource)
{
	return x86_io_resource_register_batch(descriptor, 1u, resource, 1u);
}

enum x86_io_resource_status x86_io_resource_register_batch(
	const struct x86_io_resource_descriptor *descriptors,
	size_t descriptor_count, x86_io_resource_handle_t *resources,
	size_t resource_capacity)
{
	x86_io_resource_handle_t
		prepared_handles[X86_IO_RESOURCE_REGISTRY_CAPACITY];
	uint64_t generations[X86_IO_RESOURCE_REGISTRY_CAPACITY];
	size_t slot_indices[X86_IO_RESOURCE_REGISTRY_CAPACITY];
	uint32_t reserved_bitmap = 0u;
	size_t index;
	size_t prior;

	if (descriptors == NULL || resources == NULL || descriptor_count == 0u ||
	    descriptor_count > X86_IO_RESOURCE_REGISTRY_CAPACITY ||
	    descriptor_count > resource_capacity)
		return X86_IO_RESOURCE_INVALID_ARGUMENT;
	if (owner.initialized != 1u || owner.dispatch_active != 0u)
		return X86_IO_RESOURCE_INVALID_STATE;

	for (index = 0u; index < descriptor_count; ++index) {
		if (!descriptor_is_valid(&descriptors[index]))
			return X86_IO_RESOURCE_INVALID_ARGUMENT;
		if (active_range_overlaps(descriptors[index].first_port,
					  descriptors[index].last_port))
			return X86_IO_RESOURCE_OVERLAP;
		for (prior = 0u; prior < index; ++prior) {
			if (ranges_overlap(descriptors[index].first_port,
					   descriptors[index].last_port,
					   descriptors[prior].first_port,
					   descriptors[prior].last_port))
				return X86_IO_RESOURCE_OVERLAP;
		}
		if (!reserve_slot(reserved_bitmap, &slot_indices[index],
				  &generations[index]))
			return X86_IO_RESOURCE_CAPACITY_EXHAUSTED;
		reserved_bitmap |= (uint32_t)1u << slot_indices[index];
		prepared_handles[index] = make_generation_handle(
			slot_indices[index], generations[index], 0u);
	}

	for (index = 0u; index < descriptor_count; ++index) {
		struct x86_io_resource_slot *slot =
			&owner.slots[slot_indices[index]];

		slot->descriptor = descriptors[index];
		slot->generation = generations[index];
		slot->foreground_requester = KERNEL_OBJECT_HANDLE_INVALID;
		slot->foreground_token = X86_IO_FOREGROUND_TOKEN_INVALID;
	}
	owner.active_bitmap |= reserved_bitmap;
	for (index = 0u; index < descriptor_count; ++index)
		resources[index] = prepared_handles[index];
	return X86_IO_RESOURCE_OK;
}

enum x86_io_resource_status x86_io_resource_unregister(
	x86_io_resource_handle_t resource,
	kernel_object_handle_t owner_identity)
{
	struct x86_io_resource_slot *slot;
	size_t slot_index;

	if (!identity_is_valid(owner_identity))
		return X86_IO_RESOURCE_INVALID_ARGUMENT;
	if (owner.initialized != 1u || owner.dispatch_active != 0u)
		return X86_IO_RESOURCE_INVALID_STATE;
	if (!decode_resource(resource, &slot_index))
		return X86_IO_RESOURCE_STALE_HANDLE;
	slot = &owner.slots[slot_index];
	if (slot->descriptor.owner_identity != owner_identity)
		return X86_IO_RESOURCE_OWNERSHIP_DENIED;

	owner.active_bitmap &= ~((uint32_t)1u << slot_index);
	slot->descriptor = (struct x86_io_resource_descriptor){0};
	slot->foreground_requester = KERNEL_OBJECT_HANDLE_INVALID;
	slot->foreground_token = X86_IO_FOREGROUND_TOKEN_INVALID;
	return X86_IO_RESOURCE_OK;
}

enum x86_io_resource_status x86_io_resource_query(
	x86_io_resource_handle_t resource, struct x86_io_resource_view *view)
{
	const struct x86_io_resource_slot *slot;
	struct x86_io_resource_view prepared;
	size_t slot_index;

	if (view == NULL)
		return X86_IO_RESOURCE_INVALID_ARGUMENT;
	if (owner.initialized != 1u)
		return X86_IO_RESOURCE_INVALID_STATE;
	if (!decode_resource(resource, &slot_index))
		return X86_IO_RESOURCE_STALE_HANDLE;
	slot = &owner.slots[slot_index];
	prepared = (struct x86_io_resource_view){
		.registry_identity = owner.identity,
		.owner_identity = slot->descriptor.owner_identity,
		.resource = resource,
		.foreground_requester = slot->foreground_requester,
		.first_port = slot->descriptor.first_port,
		.last_port = slot->descriptor.last_port,
		.read_width_mask = slot->descriptor.read_width_mask,
		.write_width_mask = slot->descriptor.write_width_mask,
		.read_action = slot->descriptor.read_action,
		.write_action = slot->descriptor.write_action,
		.flags = slot->descriptor.flags,
		.foreground_owned =
			(uint8_t)(slot->foreground_token !=
				  X86_IO_FOREGROUND_TOKEN_INVALID),
		.reserved = {0u},
	};
	*view = prepared;
	return X86_IO_RESOURCE_OK;
}

enum x86_io_resource_status x86_io_resource_foreground_acquire(
	x86_io_resource_handle_t resource,
	kernel_object_handle_t owner_identity,
	kernel_object_handle_t requester_identity,
	x86_io_foreground_token_t *token)
{
	struct x86_io_resource_slot *slot;
	x86_io_foreground_token_t prepared;
	size_t slot_index;

	if (!identity_is_valid(owner_identity) ||
	    !identity_is_valid(requester_identity) || token == NULL)
		return X86_IO_RESOURCE_INVALID_ARGUMENT;
	if (owner.initialized != 1u || owner.dispatch_active != 0u)
		return X86_IO_RESOURCE_INVALID_STATE;
	if (!decode_resource(resource, &slot_index))
		return X86_IO_RESOURCE_STALE_HANDLE;
	slot = &owner.slots[slot_index];
	if (slot->descriptor.owner_identity != owner_identity)
		return X86_IO_RESOURCE_OWNERSHIP_DENIED;
	if ((slot->descriptor.flags & X86_IO_RESOURCE_FLAG_FOREGROUND) == 0u ||
	    slot->foreground_token != X86_IO_FOREGROUND_TOKEN_INVALID)
		return X86_IO_RESOURCE_OWNERSHIP_DENIED;
	if (slot->foreground_generation >= X86_IO_RESOURCE_GENERATION_MAX)
		return X86_IO_RESOURCE_CAPACITY_EXHAUSTED;

	++slot->foreground_generation;
	prepared = make_generation_handle(slot_index,
					 slot->foreground_generation,
					 X86_IO_FOREGROUND_TOKEN_DOMAIN);
	slot->foreground_requester = requester_identity;
	slot->foreground_token = prepared;
	*token = prepared;
	return X86_IO_RESOURCE_OK;
}

enum x86_io_resource_status x86_io_resource_foreground_release(
	x86_io_foreground_token_t token,
	kernel_object_handle_t requester_identity)
{
	struct x86_io_resource_slot *slot;
	uint32_t encoded_slot;
	size_t slot_index;

	if (!identity_is_valid(requester_identity) ||
	    token == X86_IO_FOREGROUND_TOKEN_INVALID ||
	    token == KERNEL_OBJECT_HANDLE_INVALID ||
	    (token & X86_IO_FOREGROUND_TOKEN_DOMAIN) == 0u)
		return X86_IO_RESOURCE_INVALID_ARGUMENT;
	if (owner.initialized != 1u || owner.dispatch_active != 0u)
		return X86_IO_RESOURCE_INVALID_STATE;
	encoded_slot = (uint32_t)(token & X86_IO_RESOURCE_SLOT_MASK);
	if (encoded_slot == 0u ||
	    encoded_slot > X86_IO_RESOURCE_REGISTRY_CAPACITY)
		return X86_IO_RESOURCE_STALE_HANDLE;
	slot_index = (size_t)encoded_slot - 1u;
	if (!slot_is_active(slot_index))
		return X86_IO_RESOURCE_STALE_HANDLE;
	slot = &owner.slots[slot_index];
	if (slot->foreground_token != token)
		return X86_IO_RESOURCE_STALE_HANDLE;
	if (slot->foreground_requester != requester_identity)
		return X86_IO_RESOURCE_OWNERSHIP_DENIED;
	slot->foreground_requester = KERNEL_OBJECT_HANDLE_INVALID;
	slot->foreground_token = X86_IO_FOREGROUND_TOKEN_INVALID;
	return X86_IO_RESOURCE_OK;
}

enum x86_io_resource_status x86_io_resource_foreground_revoke(
	x86_io_resource_handle_t resource,
	kernel_object_handle_t owner_identity)
{
	struct x86_io_resource_slot *slot;
	size_t slot_index;

	if (!identity_is_valid(owner_identity))
		return X86_IO_RESOURCE_INVALID_ARGUMENT;
	if (owner.initialized != 1u || owner.dispatch_active != 0u)
		return X86_IO_RESOURCE_INVALID_STATE;
	if (!decode_resource(resource, &slot_index))
		return X86_IO_RESOURCE_STALE_HANDLE;
	slot = &owner.slots[slot_index];
	if (slot->descriptor.owner_identity != owner_identity)
		return X86_IO_RESOURCE_OWNERSHIP_DENIED;
	slot->foreground_requester = KERNEL_OBJECT_HANDLE_INVALID;
	slot->foreground_token = X86_IO_FOREGROUND_TOKEN_INVALID;
	return X86_IO_RESOURCE_OK;
}

static struct x86_io_resource_slot *find_containing_slot(uint16_t port)
{
	size_t index;

	for (index = 0u; index < X86_IO_RESOURCE_REGISTRY_CAPACITY; ++index) {
		struct x86_io_resource_slot *slot = &owner.slots[index];

		if (slot_is_active(index) &&
		    port >= slot->descriptor.first_port &&
		    port <= slot->descriptor.last_port)
			return slot;
	}
	return NULL;
}

static uint32_t absent_read_value(enum dos_io_width width)
{
	if (width == DOS_IO_WIDTH_8)
		return 0xffu;
	if (width == DOS_IO_WIDTH_16)
		return 0xffffu;
	return 0xffffffffu;
}

static bool value_fits_width(uint32_t value, enum dos_io_width width)
{
	if (width == DOS_IO_WIDTH_8)
		return value <= 0xffu;
	if (width == DOS_IO_WIDTH_16)
		return value <= 0xffffu;
	return true;
}

static bool foreground_access_is_valid(
	const struct x86_io_resource_slot *slot,
	kernel_object_handle_t requester_identity)
{
	if ((slot->descriptor.flags & X86_IO_RESOURCE_FLAG_FOREGROUND) == 0u)
		return true;
	return identity_is_valid(requester_identity) &&
	       slot->foreground_token != X86_IO_FOREGROUND_TOKEN_INVALID &&
	       slot->foreground_requester == requester_identity;
}

static enum x86_io_resource_status callback_status(
	enum x86_io_callback_status status)
{
	if (status == X86_IO_CALLBACK_OK)
		return X86_IO_RESOURCE_OK;
	if (status == X86_IO_CALLBACK_DENIED)
		return X86_IO_RESOURCE_ACCESS_DENIED;
	return X86_IO_RESOURCE_CALLBACK_FAULT;
}

static enum x86_io_resource_status resolve_access(
	uint16_t port, enum dos_io_width width,
	struct x86_io_resource_slot **slot)
{
	struct x86_io_resource_slot *containing;
	uint32_t last;

	if (!width_is_valid(width) || slot == NULL)
		return X86_IO_RESOURCE_INVALID_ARGUMENT;
	last = (uint32_t)port + (uint32_t)width - 1u;
	if (last > 0xffffu)
		return X86_IO_RESOURCE_ACCESS_DENIED;
	containing = find_containing_slot(port);
	if (containing != NULL) {
		if (last > containing->descriptor.last_port)
			return X86_IO_RESOURCE_ACCESS_DENIED;
		*slot = containing;
		return X86_IO_RESOURCE_OK;
	}
	if (active_range_overlaps(port, (uint16_t)last))
		return X86_IO_RESOURCE_ACCESS_DENIED;
	*slot = NULL;
	return X86_IO_RESOURCE_OK;
}

enum x86_io_resource_status x86_io_resource_read(
	kernel_object_handle_t requester_identity, uint16_t port,
	enum dos_io_width width, uint32_t *value)
{
	struct x86_io_resource_slot *slot;
	enum x86_io_resource_status status;
	enum x86_io_callback_status callback_result;
	uint32_t prepared;
	uint8_t requested_mask;
	uint8_t action;

	if (value == NULL)
		return X86_IO_RESOURCE_INVALID_ARGUMENT;
	if (owner.initialized != 1u || owner.dispatch_active != 0u)
		return X86_IO_RESOURCE_INVALID_STATE;
	status = resolve_access(port, width, &slot);
	if (status != X86_IO_RESOURCE_OK)
		return status;
	if (slot == NULL) {
		*value = absent_read_value(width);
		return X86_IO_RESOURCE_OK;
	}
	requested_mask = width_mask(width);
	if ((slot->descriptor.read_width_mask & requested_mask) == 0u ||
	    !foreground_access_is_valid(slot, requester_identity))
		return X86_IO_RESOURCE_ACCESS_DENIED;
	action = slot->descriptor.read_action;
	if (action == (uint8_t)X86_IO_RESOURCE_ACTION_ABSENT) {
		*value = absent_read_value(width);
		return X86_IO_RESOURCE_OK;
	}
	if (action == (uint8_t)X86_IO_RESOURCE_ACTION_DENY)
		return X86_IO_RESOURCE_ACCESS_DENIED;

	prepared = 0u;
	owner.dispatch_active = 1u;
	callback_result = slot->descriptor.read(
		slot->descriptor.callback_context, port, width, &prepared);
	owner.dispatch_active = 0u;
	status = callback_status(callback_result);
	if (status != X86_IO_RESOURCE_OK)
		return status;
	if (!value_fits_width(prepared, width))
		return X86_IO_RESOURCE_CALLBACK_FAULT;
	*value = prepared;
	return X86_IO_RESOURCE_OK;
}

enum x86_io_resource_status x86_io_resource_write(
	kernel_object_handle_t requester_identity, uint16_t port,
	enum dos_io_width width, uint32_t value)
{
	struct x86_io_resource_slot *slot;
	enum x86_io_resource_status status;
	enum x86_io_callback_status callback_result;
	uint8_t requested_mask;
	uint8_t action;

	if (!value_fits_width(value, width))
		return X86_IO_RESOURCE_INVALID_ARGUMENT;
	if (owner.initialized != 1u || owner.dispatch_active != 0u)
		return X86_IO_RESOURCE_INVALID_STATE;
	status = resolve_access(port, width, &slot);
	if (status != X86_IO_RESOURCE_OK)
		return status;
	if (slot == NULL)
		return X86_IO_RESOURCE_OK;
	requested_mask = width_mask(width);
	if ((slot->descriptor.write_width_mask & requested_mask) == 0u ||
	    !foreground_access_is_valid(slot, requester_identity))
		return X86_IO_RESOURCE_ACCESS_DENIED;
	action = slot->descriptor.write_action;
	if (action == (uint8_t)X86_IO_RESOURCE_ACTION_ABSENT)
		return X86_IO_RESOURCE_OK;
	if (action == (uint8_t)X86_IO_RESOURCE_ACTION_DENY)
		return X86_IO_RESOURCE_ACCESS_DENIED;

	owner.dispatch_active = 1u;
	callback_result = slot->descriptor.write(
		slot->descriptor.callback_context, port, width, value);
	owner.dispatch_active = 0u;
	return callback_status(callback_result);
}

static_assert_expression(X86_IO_RESOURCE_REGISTRY_CAPACITY <= 32u,
			 "I/O allocation bitmap no longer covers every slot");
static_assert_expression(X86_IO_RESOURCE_REGISTRY_CAPACITY <
			 X86_IO_RESOURCE_SLOT_MASK,
			 "I/O resource handle needs more slot bits");

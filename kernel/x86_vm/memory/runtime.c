// SPDX-License-Identifier: GPL-2.0-only
/* Single boot-lifetime owner for shared x86 guest physical pages. */
#include "x86_guest_memory_runtime.h"

#include "address.h"
#include "string.h"

struct x86_guest_memory_runtime_owner {
	struct x86_guest_memory_manager manager;
	kernel_object_handle_t identity;
	uint8_t initialized;
	uint8_t reserved[7];
} __aligned(8);

static struct x86_guest_memory_runtime_owner runtime_owner;

static bool valid_identity(kernel_object_handle_t identity)
{
	return identity != 0u && identity != KERNEL_OBJECT_HANDLE_INVALID;
}

static enum x86_guest_memory_zero_status zero_physical_range(
	kernel_object_handle_t context, uint32_t physical_address,
	uint32_t byte_count)
{
	struct native_mapping mapping;

	if (context != runtime_owner.identity || byte_count == 0u ||
	    runtime_owner.manager.initialized != 1u ||
	    physical_address < runtime_owner.manager.managed_base ||
	    physical_address >= runtime_owner.manager.managed_limit ||
	    (uint64_t)byte_count > runtime_owner.manager.managed_limit -
				      physical_address)
		return X86_GUEST_ZERO_FAILED;
	if (kernel_address_identity_map(physical_address, byte_count,
					&mapping) != ADDRESS_OK ||
	    mapping.length != byte_count ||
	    memset_s(mapping.pointer, mapping.length, 0, byte_count) !=
		    MEMORY_OK)
		return X86_GUEST_ZERO_FAILED;
	return X86_GUEST_ZERO_OK;
}

static const struct x86_guest_memory_ops runtime_ops = {
	.zero = zero_physical_range,
};

enum x86_guest_memory_status x86_guest_memory_runtime_initialize(
	const struct x86_boot_info *boot_info,
	kernel_object_handle_t manager_identity)
{
	enum x86_guest_memory_status status;

	if (boot_info == NULL || !valid_identity(manager_identity))
		return X86_GUEST_MEMORY_INVALID_ARGUMENT;
	if (runtime_owner.initialized != 0u)
		return X86_GUEST_MEMORY_INVALID_ARGUMENT;
	x86_guest_memory_construct(&runtime_owner.manager);
	runtime_owner.identity = manager_identity;
	status = x86_guest_memory_initialize(&runtime_owner.manager, boot_info,
					     &runtime_ops,
					     manager_identity);
	if (status != X86_GUEST_MEMORY_OK) {
		runtime_owner.identity = KERNEL_OBJECT_HANDLE_INVALID;
		x86_guest_memory_construct(&runtime_owner.manager);
		return status;
	}
	runtime_owner.initialized = 1u;
	return X86_GUEST_MEMORY_OK;
}

enum x86_guest_memory_status x86_guest_memory_runtime_allocate(
	kernel_object_handle_t owner, uint32_t page_count,
	x86_guest_memory_lease_t *lease, uint32_t *physical_address)
{
	if (runtime_owner.initialized != 1u)
		return X86_GUEST_MEMORY_NOT_INITIALIZED;
	return x86_guest_memory_allocate(&runtime_owner.manager, owner,
					 page_count, lease, physical_address);
}

enum x86_guest_memory_status x86_guest_memory_runtime_release(
	kernel_object_handle_t owner, x86_guest_memory_lease_t lease)
{
	if (runtime_owner.initialized != 1u)
		return X86_GUEST_MEMORY_NOT_INITIALIZED;
	return x86_guest_memory_release(&runtime_owner.manager, owner, lease);
}

enum x86_guest_memory_status x86_guest_memory_runtime_inspect(
	x86_guest_memory_lease_t lease,
	struct x86_guest_memory_lease_info *info)
{
	if (runtime_owner.initialized != 1u)
		return X86_GUEST_MEMORY_NOT_INITIALIZED;
	return x86_guest_memory_inspect(&runtime_owner.manager, lease, info);
}

enum x86_guest_memory_status x86_guest_memory_runtime_query_free(
	uint32_t *free_pages)
{
	if (runtime_owner.initialized != 1u)
		return X86_GUEST_MEMORY_NOT_INITIALIZED;
	return x86_guest_memory_query_free(&runtime_owner.manager, free_pages);
}

enum x86_guest_memory_status x86_guest_memory_runtime_query_snapshot(
	struct x86_guest_memory_snapshot *snapshot)
{
	if (runtime_owner.initialized != 1u)
		return X86_GUEST_MEMORY_NOT_INITIALIZED;
	return x86_guest_memory_query_snapshot(&runtime_owner.manager,
					       snapshot);
}

enum x86_guest_memory_status x86_guest_memory_runtime_query_capacity(
	uint32_t *largest_free_pages, uint32_t *total_free_pages,
	uint64_t *highest_address)
{
	if (runtime_owner.initialized != 1u)
		return X86_GUEST_MEMORY_NOT_INITIALIZED;
	return x86_guest_memory_query_capacity(&runtime_owner.manager,
					       largest_free_pages,
					       total_free_pages,
					       highest_address);
}

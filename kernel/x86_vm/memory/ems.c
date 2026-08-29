// SPDX-License-Identifier: GPL-2.0-only
/*
 * EMS/VCPI lease view of the shared x86 guest physical-page owner.
 *
 * Acquire-before-publish and reverse unwind protect every lease. The
 * E820-derived runtime owner remains the only capacity authority.
 */
#include "x86_ems_memory.h"

#include "x86_guest_memory_runtime.h"

static_assert_expression(DOS_EMS_NATIVE_PAGE_BYTES == X86_GUEST_PAGE_BYTES,
			 "EMS and shared owner page units diverged");

static bool valid_owner(kernel_object_handle_t owner)
{
	return owner != 0u && owner != KERNEL_OBJECT_HANDLE_INVALID;
}

static enum dos_ems_page_status map_status(
	enum x86_guest_memory_status status)
{
	switch (status) {
	case X86_GUEST_MEMORY_OK:
		return DOS_EMS_PAGE_OK;
	case X86_GUEST_MEMORY_NO_MEMORY:
	case X86_GUEST_MEMORY_NO_LEASE:
		return DOS_EMS_PAGE_NO_MEMORY;
	case X86_GUEST_MEMORY_STALE_LEASE:
	case X86_GUEST_MEMORY_OWNER_MISMATCH:
		return DOS_EMS_PAGE_INVALID_BLOCK;
	case X86_GUEST_MEMORY_POISONED:
		return DOS_EMS_PAGE_UNCERTAIN;
	default:
		return DOS_EMS_PAGE_FAULT;
	}
}

static bool snapshot_is_valid(
	const struct x86_guest_memory_snapshot *snapshot)
{
	return snapshot->managed_pages != 0u &&
	       snapshot->largest_free_pages <= snapshot->total_free_pages &&
	       snapshot->total_free_pages <= snapshot->managed_pages &&
	       snapshot->highest_address >= X86_GUEST_PAGE_BYTES - 1u &&
	       (snapshot->highest_address & (X86_GUEST_PAGE_BYTES - 1u)) ==
		       X86_GUEST_PAGE_BYTES - 1u &&
	       snapshot->managed_pages <=
		       (snapshot->highest_address >>
			X86_GUEST_PAGE_SHIFT) + 1u;
}

static enum dos_ems_page_status query_pages(
	kernel_object_handle_t owner, struct dos_ems_page_snapshot *snapshot)
{
	struct x86_guest_memory_snapshot runtime_snapshot;
	enum x86_guest_memory_status status;

	if (!valid_owner(owner) || snapshot == NULL)
		return DOS_EMS_PAGE_FAULT;
	status = x86_guest_memory_runtime_query_snapshot(&runtime_snapshot);
	if (status != X86_GUEST_MEMORY_OK)
		return map_status(status);
	if (!snapshot_is_valid(&runtime_snapshot))
		return DOS_EMS_PAGE_UNCERTAIN;
	*snapshot = (struct dos_ems_page_snapshot){
		.largest_free_pages = runtime_snapshot.largest_free_pages,
		.total_free_pages = runtime_snapshot.total_free_pages,
		.managed_pages = runtime_snapshot.managed_pages,
		.highest_address = runtime_snapshot.highest_address,
	};
	return DOS_EMS_PAGE_OK;
}

static enum dos_ems_page_status unwind_unpublished_lease(
	kernel_object_handle_t owner, x86_guest_memory_lease_t lease)
{
	return x86_guest_memory_runtime_release(owner, lease) ==
		       X86_GUEST_MEMORY_OK
		       ? DOS_EMS_PAGE_FAULT
		       : DOS_EMS_PAGE_UNCERTAIN;
}

static enum dos_ems_page_status allocate_pages(
	kernel_object_handle_t owner, uint64_t requested_pages,
	dos_ems_page_block_t *block, uint64_t *physical_address,
	uint64_t *capacity_pages)
{
	struct x86_guest_memory_lease_info info;
	x86_guest_memory_lease_t lease;
	uint32_t address;
	uint32_t count;
	enum x86_guest_memory_status status;

	if (!valid_owner(owner) || requested_pages == 0u || block == NULL ||
	    physical_address == NULL || capacity_pages == NULL)
		return DOS_EMS_PAGE_FAULT;
	if (requested_pages > 0xffffffffu)
		return DOS_EMS_PAGE_NO_MEMORY;
	count = (uint32_t)requested_pages;
	*block = DOS_EMS_PAGE_BLOCK_INVALID;
	*physical_address = 0u;
	*capacity_pages = 0u;
	status = x86_guest_memory_runtime_allocate(owner, count, &lease,
						   &address);
	if (status != X86_GUEST_MEMORY_OK)
		return map_status(status);
	status = x86_guest_memory_runtime_inspect(lease, &info);
	if (status != X86_GUEST_MEMORY_OK || info.owner != owner ||
	    info.physical_address != address || info.page_count != count)
		return unwind_unpublished_lease(owner, lease);
	*block = lease;
	*physical_address = address;
	*capacity_pages = info.page_count;
	return DOS_EMS_PAGE_OK;
}

static enum dos_ems_page_status release_pages(
	kernel_object_handle_t owner, dos_ems_page_block_t block)
{
	if (!valid_owner(owner) || block == DOS_EMS_PAGE_BLOCK_INVALID)
		return DOS_EMS_PAGE_INVALID_BLOCK;
	return map_status(x86_guest_memory_runtime_release(owner, block));
}

static const struct dos_ems_page_ops runtime_operations = {
	.query = query_pages,
	.allocate = allocate_pages,
	.release = release_pages,
};

const struct dos_ems_page_ops *
x86_ems_memory_runtime_operations(void)
{
	return &runtime_operations;
}

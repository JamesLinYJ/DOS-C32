// SPDX-License-Identifier: GPL-2.0-only
/* Lease-validating XMS view of the shared x86 guest physical-page owner. */
#include "x86_xms_memory.h"

#include "address.h"
#include "string.h"
#include "x86_guest_space.h"
#include "x86_guest_memory_runtime.h"
#include "x86_paging.h"

static enum dos_xms_memory_status map_status(
	enum x86_guest_memory_status status)
{
	if (status == X86_GUEST_MEMORY_OK)
		return DOS_XMS_MEMORY_OK;
	if (status == X86_GUEST_MEMORY_NO_MEMORY ||
	    status == X86_GUEST_MEMORY_NO_LEASE)
		return DOS_XMS_MEMORY_NO_MEMORY;
	if (status == X86_GUEST_MEMORY_POISONED)
		return DOS_XMS_MEMORY_UNCERTAIN;
	return DOS_XMS_MEMORY_FAULT;
}

static enum dos_xms_memory_status query_memory(
	kernel_object_handle_t context, uint64_t *largest_bytes,
	uint64_t *total_bytes, uint64_t *highest_address)
{
	uint32_t largest_pages;
	uint32_t total_pages;
	enum x86_guest_memory_status status;

	if (context == 0u || context == KERNEL_OBJECT_HANDLE_INVALID ||
	    largest_bytes == NULL || total_bytes == NULL ||
	    highest_address == NULL)
		return DOS_XMS_MEMORY_FAULT;
	status = x86_guest_memory_runtime_query_capacity(&largest_pages,
							 &total_pages,
							 highest_address);
	if (status != X86_GUEST_MEMORY_OK)
		return map_status(status);
	*largest_bytes = (uint64_t)largest_pages * X86_GUEST_PAGE_BYTES;
	*total_bytes = (uint64_t)total_pages * X86_GUEST_PAGE_BYTES;
	return DOS_XMS_MEMORY_OK;
}

static enum dos_xms_memory_status allocate_memory(
	kernel_object_handle_t context, uint64_t requested_bytes,
	dos_xms_block_t *block, uint64_t *physical_address,
	uint64_t *capacity_bytes)
{
	x86_guest_memory_lease_t lease;
	uint64_t rounded;
	uint32_t pages;
	uint32_t address;
	enum x86_guest_memory_status status;

	if (context == 0u || context == KERNEL_OBJECT_HANDLE_INVALID ||
	    requested_bytes == 0u || block == NULL || physical_address == NULL ||
	    capacity_bytes == NULL)
		return DOS_XMS_MEMORY_FAULT;
	if (requested_bytes >
	    (uint64_t)X86_GUEST_MEMORY_PAGE_COUNT * X86_GUEST_PAGE_BYTES)
		return DOS_XMS_MEMORY_NO_MEMORY;
	rounded = requested_bytes + (X86_GUEST_PAGE_BYTES - 1u);
	pages = (uint32_t)(rounded >> X86_GUEST_PAGE_SHIFT);
	*block = DOS_XMS_BLOCK_INVALID;
	*physical_address = 0u;
	*capacity_bytes = 0u;
	status = x86_guest_memory_runtime_allocate(context, pages, &lease,
						   &address);
	if (status != X86_GUEST_MEMORY_OK)
		return map_status(status);
	*block = lease;
	*physical_address = address;
	*capacity_bytes = (uint64_t)pages * X86_GUEST_PAGE_BYTES;
	return DOS_XMS_MEMORY_OK;
}

static enum dos_xms_memory_status release_memory(
	kernel_object_handle_t context, dos_xms_block_t block)
{
	return map_status(x86_guest_memory_runtime_release(context, block));
}

static enum dos_xms_memory_status resolve_mapping(
	kernel_object_handle_t context, dos_xms_block_t block, uint64_t offset,
	size_t count, struct native_mapping *mapping)
{
	struct x86_guest_memory_lease_info info;
	uint64_t allocation_bytes;
	uint64_t physical;
	enum x86_guest_memory_status status;

	if (context == 0u || context == KERNEL_OBJECT_HANDLE_INVALID ||
	    block == DOS_XMS_BLOCK_INVALID || mapping == NULL)
		return DOS_XMS_MEMORY_FAULT;
	status = x86_guest_memory_runtime_inspect(block, &info);
	if (status != X86_GUEST_MEMORY_OK)
		return map_status(status);
	allocation_bytes =
		(uint64_t)info.page_count * X86_GUEST_PAGE_BYTES;
	if (info.owner != context || offset > allocation_bytes ||
	    (uint64_t)count > allocation_bytes - offset)
		return DOS_XMS_MEMORY_FAULT;
	if (count == 0u) {
		*mapping = (struct native_mapping){0};
		return DOS_XMS_MEMORY_OK;
	}
	physical = (uint64_t)info.physical_address + offset;
	if (physical > 0xffffffffu ||
	    kernel_address_identity_map((uint32_t)physical, count, mapping) !=
		ADDRESS_OK ||
	    mapping->length != count)
		return DOS_XMS_MEMORY_FAULT;
	return DOS_XMS_MEMORY_OK;
}

static enum dos_xms_memory_status read_memory(
	kernel_object_handle_t context, dos_xms_block_t block, uint64_t offset,
	void *destination, size_t capacity, size_t count)
{
	struct native_mapping mapping;
	enum dos_xms_memory_status status;

	if ((destination == NULL && count != 0u) || count > capacity)
		return DOS_XMS_MEMORY_FAULT;
	status = resolve_mapping(context, block, offset, count, &mapping);
	if (status != DOS_XMS_MEMORY_OK || count == 0u)
		return status;
	return memcpy_s(destination, capacity, mapping.pointer, mapping.length,
			count) == MEMORY_OK
		       ? DOS_XMS_MEMORY_OK
		       : DOS_XMS_MEMORY_FAULT;
}

static enum dos_xms_memory_status write_memory(
	kernel_object_handle_t context, dos_xms_block_t block, uint64_t offset,
	const void *source, size_t capacity, size_t count)
{
	struct native_mapping mapping;
	enum dos_xms_memory_status status;

	if ((source == NULL && count != 0u) || count > capacity)
		return DOS_XMS_MEMORY_FAULT;
	status = resolve_mapping(context, block, offset, count, &mapping);
	if (status != DOS_XMS_MEMORY_OK || count == 0u)
		return status;
	return memcpy_s(mapping.pointer, mapping.length, source, capacity,
			count) == MEMORY_OK
		       ? DOS_XMS_MEMORY_OK
		       : DOS_XMS_MEMORY_FAULT;
}

static enum dos_xms_memory_status query_hma(
	kernel_object_handle_t context, const struct dos_machine *machine,
	struct dos_xms_hma_snapshot *snapshot)
{
	struct x86_guest_space_binding binding;
	struct dos_xms_hma_snapshot prepared;
	const struct dos_machine *active_machine;
	kernel_object_handle_t machine_identity;

	if (context == 0u || context == KERNEL_OBJECT_HANDLE_INVALID ||
	    machine == NULL || snapshot == NULL)
		return DOS_XMS_MEMORY_FAULT;
	if (machine->address_limit < DOS_XMS_HMA_LIMIT)
		return DOS_XMS_MEMORY_NO_MEMORY;
	active_machine = x86_guest_space_machine();
	machine_identity = x86_guest_space_machine_identity();
	if (active_machine == NULL ||
	    machine_identity == KERNEL_OBJECT_HANDLE_INVALID ||
	    machine->ops != active_machine->ops ||
	    machine->context != active_machine->context ||
	    machine->address_limit != active_machine->address_limit ||
	    machine->a20_enabled != active_machine->a20_enabled ||
	    machine->poisoned != 0u ||
	    x86_guest_space_pin(machine_identity, active_machine, &binding) !=
		    X86_GUEST_SPACE_OK)
		return DOS_XMS_MEMORY_FAULT;
	if (!x86_guest_space_binding_is_active(&binding, machine_identity,
					       active_machine) ||
	    !x86_paging_guest_range_is_accessible(
		    (uint32_t)DOS_XMS_HMA_BASE, (size_t)DOS_XMS_HMA_BYTES,
		    true))
		return DOS_XMS_MEMORY_UNCERTAIN;
	prepared = (struct dos_xms_hma_snapshot){
		.address_space_identity = binding.address_space_identity,
		.address_space_generation = binding.address_space_generation,
		.machine_context = binding.machine_context,
		.base_address = DOS_XMS_HMA_BASE,
		.byte_count = DOS_XMS_HMA_BYTES,
	};
	*snapshot = prepared;
	return DOS_XMS_MEMORY_OK;
}

static const struct dos_xms_memory_ops runtime_operations = {
	.query = query_memory,
	.allocate = allocate_memory,
	.release = release_memory,
	.read = read_memory,
	.write = write_memory,
	.query_hma = query_hma,
};

const struct dos_xms_memory_ops *
x86_xms_memory_runtime_operations(void)
{
	return &runtime_operations;
}

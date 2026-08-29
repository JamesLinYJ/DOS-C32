// SPDX-License-Identifier: GPL-2.0-only
/* Host tests for the production XMS/HMA active-guest mapping adapter. */
#include "test_entry.h"
#include "x86_guest_memory_runtime.h"
#include "x86_guest_space.h"
#include "x86_xms_memory.h"

#define TEST_MEMORY_CONTEXT ((kernel_object_handle_t)0x501u)
#define TEST_MACHINE_CONTEXT ((kernel_object_handle_t)0x502u)
#define TEST_MACHINE_IDENTITY ((kernel_object_handle_t)0x503u)
#define TEST_ADDRESS_SPACE_IDENTITY ((kernel_object_handle_t)0x504u)

static const struct dos_machine_ops machine_ops = {0};
static struct dos_machine active_machine = {
	.ops = &machine_ops,
	.context = TEST_MACHINE_CONTEXT,
	.address_limit = DOS_REAL_MODE_ADDRESS_LIMIT,
	.a20_enabled = false,
	.poisoned = 0u,
};
static enum x86_guest_space_status pin_status = X86_GUEST_SPACE_OK;
static bool binding_active = true;
static bool range_accessible = true;
static kernel_object_handle_t machine_identity = TEST_MACHINE_IDENTITY;
static uint32_t observed_range_address;
static size_t observed_range_count;
static bool observed_range_writable;

enum x86_guest_memory_status x86_guest_memory_runtime_query_capacity(
	uint32_t *largest_free_pages, uint32_t *total_free_pages,
	uint64_t *highest_address)
{
	if (largest_free_pages == NULL || total_free_pages == NULL ||
	    highest_address == NULL)
		return X86_GUEST_MEMORY_INVALID_ARGUMENT;
	*largest_free_pages = 16u;
	*total_free_pages = 16u;
	*highest_address = 0x01ffffffu;
	return X86_GUEST_MEMORY_OK;
}

enum x86_guest_memory_status x86_guest_memory_runtime_allocate(
	kernel_object_handle_t owner, uint32_t page_count,
	x86_guest_memory_lease_t *lease, uint32_t *physical_address)
{
	(void)owner;
	(void)page_count;
	(void)lease;
	(void)physical_address;
	return X86_GUEST_MEMORY_NO_MEMORY;
}

enum x86_guest_memory_status x86_guest_memory_runtime_release(
	kernel_object_handle_t owner, x86_guest_memory_lease_t lease)
{
	(void)owner;
	(void)lease;
	return X86_GUEST_MEMORY_NO_LEASE;
}

enum x86_guest_memory_status x86_guest_memory_runtime_inspect(
	x86_guest_memory_lease_t lease, struct x86_guest_memory_lease_info *info)
{
	(void)lease;
	(void)info;
	return X86_GUEST_MEMORY_NO_LEASE;
}

const struct dos_machine *x86_guest_space_machine(void)
{
	return &active_machine;
}

kernel_object_handle_t x86_guest_space_machine_identity(void)
{
	return machine_identity;
}

enum x86_guest_space_status x86_guest_space_pin(
	kernel_object_handle_t requested_identity,
	const struct dos_machine *machine, struct x86_guest_space_binding *binding)
{
	if (pin_status != X86_GUEST_SPACE_OK)
		return pin_status;
	if (requested_identity != TEST_MACHINE_IDENTITY ||
	    machine != &active_machine || binding == NULL)
		return X86_GUEST_SPACE_MACHINE_MISMATCH;
	*binding = (struct x86_guest_space_binding){
		.address_space_identity = TEST_ADDRESS_SPACE_IDENTITY,
		.address_space_generation = 7u,
		.machine_identity = TEST_MACHINE_IDENTITY,
		.machine_context = TEST_MACHINE_CONTEXT,
		.a20_enabled = 0u,
		.reserved = {0u},
	};
	return X86_GUEST_SPACE_OK;
}

bool x86_guest_space_binding_is_active(
	const struct x86_guest_space_binding *binding,
	kernel_object_handle_t requested_identity,
	const struct dos_machine *machine)
{
	return binding_active && binding != NULL &&
	       binding->address_space_identity == TEST_ADDRESS_SPACE_IDENTITY &&
	       binding->address_space_generation == 7u &&
	       requested_identity == TEST_MACHINE_IDENTITY &&
	       machine == &active_machine;
}

bool x86_paging_guest_range_is_accessible(uint32_t address, size_t count,
					  bool writable)
{
	observed_range_address = address;
	observed_range_count = count;
	observed_range_writable = writable;
	return range_accessible;
}

static bool snapshot_is_sentinel(const struct dos_xms_hma_snapshot *snapshot)
{
	return snapshot->address_space_identity == 0xaaaaaaaaaaaaaaaaull &&
	       snapshot->address_space_generation == 0xbbbbbbbbbbbbbbbbull &&
	       snapshot->machine_context == 0xccccccccccccccccull &&
	       snapshot->base_address == 0xddddddddddddddddull &&
	       snapshot->byte_count == 0xeeeeeeeeeeeeeeeeull;
}

static struct dos_xms_hma_snapshot sentinel_snapshot(void)
{
	return (struct dos_xms_hma_snapshot){
		.address_space_identity = 0xaaaaaaaaaaaaaaaaull,
		.address_space_generation = 0xbbbbbbbbbbbbbbbbull,
		.machine_context = 0xccccccccccccccccull,
		.base_address = 0xddddddddddddddddull,
		.byte_count = 0xeeeeeeeeeeeeeeeeull,
	};
}

static int run_tests(void)
{
	const struct dos_xms_memory_ops *ops =
		x86_xms_memory_runtime_operations();
	struct dos_xms_hma_snapshot snapshot = sentinel_snapshot();
	struct dos_machine borrowed;

	if (ops == NULL || ops->query_hma == NULL)
		return 1;
	if (ops->query_hma(TEST_MEMORY_CONTEXT, &active_machine, &snapshot) !=
		DOS_XMS_MEMORY_OK ||
	    snapshot.address_space_identity != TEST_ADDRESS_SPACE_IDENTITY ||
	    snapshot.address_space_generation != 7u ||
	    snapshot.machine_context != TEST_MACHINE_CONTEXT ||
	    snapshot.base_address != DOS_XMS_HMA_BASE ||
	    snapshot.byte_count != DOS_XMS_HMA_BYTES ||
	    observed_range_address != DOS_XMS_HMA_BASE ||
	    observed_range_count != DOS_XMS_HMA_BYTES ||
	    !observed_range_writable)
		return 2;

	borrowed = active_machine;
	borrowed.address_limit = DOS_A20_WRAP_ADDRESS;
	snapshot = sentinel_snapshot();
	if (ops->query_hma(TEST_MEMORY_CONTEXT, &borrowed, &snapshot) !=
		DOS_XMS_MEMORY_NO_MEMORY ||
	    !snapshot_is_sentinel(&snapshot))
		return 3;

	range_accessible = false;
	snapshot = sentinel_snapshot();
	if (ops->query_hma(TEST_MEMORY_CONTEXT, &active_machine, &snapshot) !=
		DOS_XMS_MEMORY_UNCERTAIN ||
	    !snapshot_is_sentinel(&snapshot))
		return 4;
	range_accessible = true;
	binding_active = false;
	snapshot = sentinel_snapshot();
	if (ops->query_hma(TEST_MEMORY_CONTEXT, &active_machine, &snapshot) !=
		DOS_XMS_MEMORY_UNCERTAIN ||
	    !snapshot_is_sentinel(&snapshot))
		return 5;
	binding_active = true;

	pin_status = X86_GUEST_SPACE_PAGING_MISMATCH;
	snapshot = sentinel_snapshot();
	if (ops->query_hma(TEST_MEMORY_CONTEXT, &active_machine, &snapshot) !=
		DOS_XMS_MEMORY_FAULT ||
	    !snapshot_is_sentinel(&snapshot))
		return 6;
	pin_status = X86_GUEST_SPACE_OK;

	borrowed = active_machine;
	borrowed.context = TEST_MACHINE_CONTEXT + 1u;
	snapshot = sentinel_snapshot();
	if (ops->query_hma(TEST_MEMORY_CONTEXT, &borrowed, &snapshot) !=
		DOS_XMS_MEMORY_FAULT ||
	    !snapshot_is_sentinel(&snapshot))
		return 7;

	machine_identity = KERNEL_OBJECT_HANDLE_INVALID;
	snapshot = sentinel_snapshot();
	if (ops->query_hma(TEST_MEMORY_CONTEXT, &active_machine, &snapshot) !=
		DOS_XMS_MEMORY_FAULT ||
	    !snapshot_is_sentinel(&snapshot))
		return 8;
	return 0;
}

DOSC32_TEST_ENTRY(run_tests)

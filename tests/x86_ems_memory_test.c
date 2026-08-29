// SPDX-License-Identifier: GPL-2.0-only
/* Host-safe production-adapter and VCPI capability-binding tests. */
#include "x86_ems_memory.h"
#include "x86_guest_memory_runtime.h"
#include "x86_vcpi_execution.h"
#if !defined(DOSC32_QEMU_SYSTEM_TEST)
#include "test_entry.h"
#endif

#define TEST_OWNER_A ((kernel_object_handle_t)0x401u)
#define TEST_OWNER_B ((kernel_object_handle_t)0x402u)
#define TEST_VCPI_CONTEXT ((kernel_object_handle_t)0x403u)
#define TEST_PHYSICAL_BASE 0x00800000u
#define TEST_MANAGED_PAGES 12u

static struct x86_guest_memory_snapshot runtime_snapshot;
static x86_guest_memory_lease_t live_lease;
static kernel_object_handle_t live_owner;
static uint32_t live_address;
static uint32_t live_pages;
static uint32_t next_generation;
static uint32_t allocate_calls;
static uint32_t release_calls;
static uint8_t force_inspect_mismatch;
static uint8_t force_release_failure;
static uint8_t force_poison;
static uint32_t platform_calls;

enum x86_guest_memory_status x86_guest_memory_runtime_query_snapshot(
	struct x86_guest_memory_snapshot *snapshot)
{
	if (snapshot == NULL)
		return X86_GUEST_MEMORY_INVALID_ARGUMENT;
	if (force_poison != 0u)
		return X86_GUEST_MEMORY_POISONED;
	*snapshot = runtime_snapshot;
	return X86_GUEST_MEMORY_OK;
}

enum x86_guest_memory_status x86_guest_memory_runtime_allocate(
	kernel_object_handle_t owner, uint32_t page_count,
	x86_guest_memory_lease_t *lease, uint32_t *physical_address)
{
	if (owner == 0u || owner == KERNEL_OBJECT_HANDLE_INVALID ||
	    page_count == 0u || lease == NULL || physical_address == NULL)
		return X86_GUEST_MEMORY_INVALID_ARGUMENT;
	if (force_poison != 0u)
		return X86_GUEST_MEMORY_POISONED;
	if (live_lease != X86_GUEST_MEMORY_LEASE_INVALID ||
	    page_count > runtime_snapshot.total_free_pages)
		return X86_GUEST_MEMORY_NO_MEMORY;
	++allocate_calls;
	++next_generation;
	live_lease = ((uint64_t)next_generation << 11u) | 1u;
	live_owner = owner;
	live_address = TEST_PHYSICAL_BASE;
	live_pages = page_count;
	runtime_snapshot.total_free_pages -= page_count;
	if (runtime_snapshot.largest_free_pages >
	    runtime_snapshot.total_free_pages)
		runtime_snapshot.largest_free_pages =
			runtime_snapshot.total_free_pages;
	*lease = live_lease;
	*physical_address = live_address;
	return X86_GUEST_MEMORY_OK;
}

enum x86_guest_memory_status x86_guest_memory_runtime_inspect(
	x86_guest_memory_lease_t lease,
	struct x86_guest_memory_lease_info *info)
{
	if (lease == X86_GUEST_MEMORY_LEASE_INVALID || info == NULL)
		return X86_GUEST_MEMORY_INVALID_ARGUMENT;
	if (force_poison != 0u)
		return X86_GUEST_MEMORY_POISONED;
	if (lease != live_lease)
		return X86_GUEST_MEMORY_STALE_LEASE;
	*info = (struct x86_guest_memory_lease_info){
		.owner = live_owner,
		.physical_address = live_address,
		.page_count = live_pages,
	};
	if (force_inspect_mismatch != 0u)
		++info->page_count;
	return X86_GUEST_MEMORY_OK;
}

enum x86_guest_memory_status x86_guest_memory_runtime_release(
	kernel_object_handle_t owner, x86_guest_memory_lease_t lease)
{
	if (lease == X86_GUEST_MEMORY_LEASE_INVALID || lease != live_lease)
		return X86_GUEST_MEMORY_STALE_LEASE;
	if (owner != live_owner)
		return X86_GUEST_MEMORY_OWNER_MISMATCH;
	if (force_poison != 0u)
		return X86_GUEST_MEMORY_POISONED;
	if (force_release_failure != 0u)
		return X86_GUEST_MEMORY_ZERO_FAILED;
	++release_calls;
	runtime_snapshot.total_free_pages += live_pages;
	runtime_snapshot.largest_free_pages =
		runtime_snapshot.total_free_pages;
	live_lease = X86_GUEST_MEMORY_LEASE_INVALID;
	live_owner = KERNEL_OBJECT_HANDLE_INVALID;
	live_address = 0u;
	live_pages = 0u;
	return X86_GUEST_MEMORY_OK;
}

static enum dos_vcpi_platform_status translate_low_page(
	kernel_object_handle_t context, uint16_t page,
	uint64_t *physical_address)
{
	++platform_calls;
	if (context != TEST_VCPI_CONTEXT || physical_address == NULL)
		return DOS_VCPI_PLATFORM_FAULT;
	*physical_address = (uint64_t)page * DOS_EMS_NATIVE_PAGE_BYTES;
	return DOS_VCPI_PLATFORM_OK;
}

static enum dos_vcpi_platform_status read_virtual_cr0(
	kernel_object_handle_t context, uint32_t *virtual_cr0)
{
	++platform_calls;
	if (context != TEST_VCPI_CONTEXT || virtual_cr0 == NULL)
		return DOS_VCPI_PLATFORM_FAULT;
	*virtual_cr0 = 0x11u;
	return DOS_VCPI_PLATFORM_OK;
}

static enum dos_vcpi_platform_status query_pic_mappings(
	kernel_object_handle_t context, uint8_t *master_base,
	uint8_t *slave_base)
{
	++platform_calls;
	if (context != TEST_VCPI_CONTEXT || master_base == NULL ||
	    slave_base == NULL)
		return DOS_VCPI_PLATFORM_FAULT;
	*master_base = 0x08u;
	*slave_base = 0x70u;
	return DOS_VCPI_PLATFORM_OK;
}

static enum dos_vcpi_platform_status set_pic_mappings(
	kernel_object_handle_t context, uint8_t master_base,
	uint8_t slave_base)
{
	++platform_calls;
	return context == TEST_VCPI_CONTEXT && master_base != slave_base
		       ? DOS_VCPI_PLATFORM_OK
		       : DOS_VCPI_PLATFORM_FAULT;
}

static enum dos_vcpi_handoff_status execution_handoff(
	kernel_object_handle_t context,
	const struct dos_vcpi_handoff_request *request,
	struct dos_cpu_state *state)
{
	++platform_calls;
	if (context != TEST_VCPI_CONTEXT || request == NULL || state == NULL)
		return DOS_VCPI_HANDOFF_FAULT;
	return DOS_VCPI_HANDOFF_UNSUPPORTED;
}

static void reset_runtime(void)
{
	runtime_snapshot = (struct x86_guest_memory_snapshot){
		.largest_free_pages = TEST_MANAGED_PAGES,
		.total_free_pages = TEST_MANAGED_PAGES,
		.managed_pages = TEST_MANAGED_PAGES,
		.highest_address = TEST_PHYSICAL_BASE +
				   TEST_MANAGED_PAGES *
					   X86_GUEST_PAGE_BYTES -
				   1u,
	};
	live_lease = X86_GUEST_MEMORY_LEASE_INVALID;
	live_owner = KERNEL_OBJECT_HANDLE_INVALID;
	live_address = 0u;
	live_pages = 0u;
	allocate_calls = 0u;
	release_calls = 0u;
	force_inspect_mismatch = 0u;
	force_release_failure = 0u;
	force_poison = 0u;
}

static int test_runtime_adapter(void)
{
	const struct dos_ems_page_ops *ops =
		x86_ems_memory_runtime_operations();
	struct dos_ems_page_snapshot snapshot;
	dos_ems_page_block_t block;
	uint64_t physical_address;
	uint64_t capacity_pages;

	reset_runtime();
	if (ops == NULL || ops->query == NULL || ops->allocate == NULL ||
	    ops->release == NULL)
		return 1;
	if (ops->query(TEST_OWNER_A, &snapshot) != DOS_EMS_PAGE_OK ||
	    snapshot.largest_free_pages != TEST_MANAGED_PAGES ||
	    snapshot.total_free_pages != TEST_MANAGED_PAGES ||
	    snapshot.managed_pages != TEST_MANAGED_PAGES ||
	    snapshot.highest_address != runtime_snapshot.highest_address)
		return 2;
	if (ops->allocate(TEST_OWNER_A, 4u, &block, &physical_address,
			  &capacity_pages) != DOS_EMS_PAGE_OK ||
	    block == DOS_EMS_PAGE_BLOCK_INVALID ||
	    physical_address != TEST_PHYSICAL_BASE || capacity_pages != 4u ||
	    allocate_calls != 1u)
		return 3;
	if (ops->query(TEST_OWNER_A, &snapshot) != DOS_EMS_PAGE_OK ||
	    snapshot.total_free_pages != TEST_MANAGED_PAGES - 4u ||
	    snapshot.managed_pages != TEST_MANAGED_PAGES)
		return 4;
	if (ops->release(TEST_OWNER_B, block) !=
			DOS_EMS_PAGE_INVALID_BLOCK ||
	    live_lease == X86_GUEST_MEMORY_LEASE_INVALID)
		return 5;
	if (ops->release(TEST_OWNER_A, block) != DOS_EMS_PAGE_OK ||
	    release_calls != 1u ||
	    ops->release(TEST_OWNER_A, block) !=
			DOS_EMS_PAGE_INVALID_BLOCK)
		return 6;
	if (ops->allocate(TEST_OWNER_A, 0x100000000ull, &block,
			  &physical_address, &capacity_pages) !=
			DOS_EMS_PAGE_NO_MEMORY ||
	    allocate_calls != 1u)
		return 7;
	force_poison = 1u;
	if (ops->query(TEST_OWNER_A, &snapshot) !=
			DOS_EMS_PAGE_UNCERTAIN)
		return 8;
	return 0;
}

static int test_unpublished_allocation_unwind(void)
{
	const struct dos_ems_page_ops *ops =
		x86_ems_memory_runtime_operations();
	dos_ems_page_block_t block;
	uint64_t physical_address;
	uint64_t capacity_pages;

	reset_runtime();
	force_inspect_mismatch = 1u;
	if (ops->allocate(TEST_OWNER_A, 2u, &block, &physical_address,
			  &capacity_pages) != DOS_EMS_PAGE_FAULT ||
	    block != DOS_EMS_PAGE_BLOCK_INVALID || physical_address != 0u ||
	    capacity_pages != 0u ||
	    live_lease != X86_GUEST_MEMORY_LEASE_INVALID ||
	    release_calls != 1u)
		return 1;
	reset_runtime();
	force_inspect_mismatch = 1u;
	force_release_failure = 1u;
	if (ops->allocate(TEST_OWNER_A, 2u, &block, &physical_address,
			  &capacity_pages) != DOS_EMS_PAGE_UNCERTAIN ||
	    block != DOS_EMS_PAGE_BLOCK_INVALID || physical_address != 0u ||
	    capacity_pages != 0u ||
	    live_lease == X86_GUEST_MEMORY_LEASE_INVALID)
		return 2;
	return 0;
}

static int test_execution_capability_gate(void)
{
	static const struct dos_vcpi_platform_ops complete_ops = {
		.translate_low_page = translate_low_page,
		.read_virtual_cr0 = read_virtual_cr0,
		.query_pic_mappings = query_pic_mappings,
		.set_pic_mappings = set_pic_mappings,
		.handoff = execution_handoff,
	};
	static const struct dos_vcpi_platform_ops incomplete_ops = {
		.translate_low_page = translate_low_page,
		.read_virtual_cr0 = read_virtual_cr0,
	};
	struct x86_vcpi_execution_binding binding;
	const struct dos_vcpi_platform_ops *resolved_ops = NULL;
	kernel_object_handle_t context = KERNEL_OBJECT_HANDLE_INVALID;

	platform_calls = 0u;
	x86_vcpi_execution_construct(&binding);
	if (x86_vcpi_execution_resolve(&binding, &resolved_ops, &context) !=
			X86_VCPI_EXECUTION_UNAVAILABLE ||
	    resolved_ops != NULL || context != KERNEL_OBJECT_HANDLE_INVALID)
		return 1;
	if (x86_vcpi_execution_bind(&binding, &incomplete_ops,
				    TEST_VCPI_CONTEXT) !=
			X86_VCPI_EXECUTION_INVALID_ARGUMENT)
		return 2;
	if (x86_vcpi_execution_bind(&binding, &complete_ops,
				    TEST_VCPI_CONTEXT) !=
			X86_VCPI_EXECUTION_OK ||
	    platform_calls != 0u)
		return 3;
	if (x86_vcpi_execution_resolve(&binding, &resolved_ops, &context) !=
			X86_VCPI_EXECUTION_OK ||
	    resolved_ops != &complete_ops || context != TEST_VCPI_CONTEXT ||
	    platform_calls != 0u)
		return 4;
	if (x86_vcpi_execution_bind(&binding, &complete_ops,
				    TEST_VCPI_CONTEXT) !=
			X86_VCPI_EXECUTION_INVALID_STATE)
		return 5;
	return 0;
}

int x86_ems_memory_test_run(void);

int x86_ems_memory_test_run(void)
{
	int status;

	status = test_runtime_adapter();
	if (status != 0)
		return 10 + status;
	status = test_unpublished_allocation_unwind();
	if (status != 0)
		return 20 + status;
	status = test_execution_capability_gate();
	if (status != 0)
		return 30 + status;
	return 0;
}

#if !defined(DOSC32_QEMU_SYSTEM_TEST)
DOSC32_TEST_ENTRY(x86_ems_memory_test_run)
#endif

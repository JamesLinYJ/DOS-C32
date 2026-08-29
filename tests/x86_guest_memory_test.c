// SPDX-License-Identifier: GPL-2.0-only
/* Host-safe tests for the shared x86 guest physical-page owner. */
#include "test_entry.h"
#include "x86_guest_memory.h"
#include "x86_memory_map.h"

#define TEST_OWNER_A ((kernel_object_handle_t)0x101u)
#define TEST_OWNER_B ((kernel_object_handle_t)0x102u)

static struct x86_guest_memory_manager manager;
static struct x86_guest_memory_manager failure_manager;
static uint32_t zero_calls;
static uint32_t last_zero_address;
static uint32_t last_zero_bytes;
static enum x86_guest_memory_zero_status zero_result;

static enum x86_guest_memory_zero_status test_zero(
	kernel_object_handle_t context, uint32_t physical_address,
	uint32_t byte_count)
{
	if (context != (kernel_object_handle_t)0x200u)
		return X86_GUEST_ZERO_UNCERTAIN;
	++zero_calls;
	last_zero_address = physical_address;
	last_zero_bytes = byte_count;
	return zero_result;
}

static const struct x86_guest_memory_ops memory_ops = {
	.zero = test_zero,
};

static struct x86_boot_info valid_map(void)
{
	struct x86_boot_info info = {0};

	info.signature = X86_BOOT_INFO_SIGNATURE;
	info.version = X86_BOOT_INFO_VERSION;
	info.header_bytes = X86_BOOT_INFO_HEADER_BYTES;
	info.range_bytes = X86_BOOT_MEMORY_RANGE_BYTES;
	info.range_count = 2u;
	info.flags = X86_BOOT_INFO_FLAG_MASK;
	info.ranges[0] = (struct x86_boot_memory_range){
		.base = X86_GUEST_MEMORY_BASE,
		.length = 8u * X86_GUEST_PAGE_BYTES,
		.type = X86_BOOT_MEMORY_USABLE,
		.attributes = X86_BOOT_MEMORY_ENABLED,
	};
	/* An unaligned reserved overlap must remove both touched pages. */
	info.ranges[1] = (struct x86_boot_memory_range){
		.base = X86_GUEST_MEMORY_BASE +
			2u * X86_GUEST_PAGE_BYTES + 512u,
		.length = X86_GUEST_PAGE_BYTES,
		.type = 2u,
		.attributes = X86_BOOT_MEMORY_ENABLED,
	};
	return info;
}

static int test_map_validation(void)
{
	struct x86_boot_info info = valid_map();
	struct x86_memory_map_snapshot snapshot;
	uint64_t usable_limit;

	if (!x86_memory_map_is_valid(&info) ||
	    !x86_memory_map_query(&info, &snapshot) ||
	    snapshot.usable_bytes != 7u * X86_GUEST_PAGE_BYTES ||
	    snapshot.highest_usable_address !=
		    X86_GUEST_MEMORY_BASE + 8u * X86_GUEST_PAGE_BYTES - 1u ||
	    snapshot.physical_address_limit !=
		    X86_GUEST_MEMORY_BASE + 8u * X86_GUEST_PAGE_BYTES ||
	    snapshot.usable_extent_count != 2u ||
	    snapshot.firmware_range_count != 2u ||
	    !x86_memory_map_usable_limit(&info, &usable_limit) ||
	    usable_limit != X86_GUEST_MEMORY_BASE +
			    8u * X86_GUEST_PAGE_BYTES ||
	    !x86_memory_map_range_is_usable(
		    &info, X86_GUEST_MEMORY_BASE,
		    2u * X86_GUEST_PAGE_BYTES) ||
	    x86_memory_map_range_is_usable(
		    &info, X86_GUEST_MEMORY_BASE +
			   2u * X86_GUEST_PAGE_BYTES,
		    X86_GUEST_PAGE_BYTES) ||
	    !x86_memory_map_range_is_usable(
		    &info, X86_GUEST_MEMORY_BASE +
			   4u * X86_GUEST_PAGE_BYTES,
		    4u * X86_GUEST_PAGE_BYTES) ||
	    x86_memory_map_range_is_usable(
		    &info, X86_GUEST_MEMORY_BASE, 0u))
		return 1;

	x86_guest_memory_construct(&failure_manager);
	info.flags = X86_BOOT_INFO_MEMORY_MAP_PRESENT;
	if (x86_guest_memory_initialize(&failure_manager, &info, &memory_ops,
					(kernel_object_handle_t)0x200u) !=
	    X86_GUEST_MEMORY_INVALID_MAP)
		return 2;
	x86_guest_memory_construct(&failure_manager);
	info = valid_map();
	info.ranges[0].base = ~(uint64_t)0u - 1u;
	info.ranges[0].length = 4u;
	if (x86_guest_memory_initialize(&failure_manager, &info, &memory_ops,
					(kernel_object_handle_t)0x200u) !=
	    X86_GUEST_MEMORY_INVALID_MAP)
		return 3;
	x86_guest_memory_construct(&failure_manager);
	info = valid_map();
	info.range_count = X86_BOOT_MEMORY_RANGE_COUNT + 1u;
	if (x86_guest_memory_initialize(&failure_manager, &info, &memory_ops,
					(kernel_object_handle_t)0x200u) !=
	    X86_GUEST_MEMORY_INVALID_MAP)
		return 4;
	x86_guest_memory_construct(&failure_manager);
	info = valid_map();
	info.ranges[0].attributes = 0u;
	if (x86_guest_memory_initialize(&failure_manager, &info, &memory_ops,
					(kernel_object_handle_t)0x200u) !=
	    X86_GUEST_MEMORY_INVALID_MAP)
		return 5;
	return 0;
}

static int test_topology_snapshot_is_not_allocator_capacity(void)
{
	struct x86_boot_info info = {0};
	struct x86_memory_map_snapshot snapshot;
	const uint64_t mebibyte = 1024u * 1024u;

	info.signature = X86_BOOT_INFO_SIGNATURE;
	info.version = X86_BOOT_INFO_VERSION;
	info.header_bytes = X86_BOOT_INFO_HEADER_BYTES;
	info.range_bytes = X86_BOOT_MEMORY_RANGE_BYTES;
	info.range_count = 5u;
	info.flags = X86_BOOT_INFO_FLAG_MASK;
	info.ranges[0] = (struct x86_boot_memory_range){
		.base = mebibyte,
		.length = 4u * mebibyte,
		.type = X86_BOOT_MEMORY_USABLE,
		.attributes = X86_BOOT_MEMORY_ENABLED,
	};
	info.ranges[1] = (struct x86_boot_memory_range){
		.base = 3u * mebibyte,
		.length = 4u * mebibyte,
		.type = X86_BOOT_MEMORY_USABLE,
		.attributes = X86_BOOT_MEMORY_ENABLED,
	};
	info.ranges[2] = (struct x86_boot_memory_range){
		.base = 2u * mebibyte,
		.length = mebibyte,
		.type = 2u,
		.attributes = X86_BOOT_MEMORY_ENABLED,
	};
	info.ranges[3] = (struct x86_boot_memory_range){
		.base = 8u * mebibyte,
		.length = mebibyte,
		.type = 2u,
		.attributes = X86_BOOT_MEMORY_ENABLED,
	};
	info.ranges[4] = (struct x86_boot_memory_range){
		.base = 0x100000000ull,
		.length = 2u * mebibyte,
		.type = X86_BOOT_MEMORY_USABLE,
		.attributes = X86_BOOT_MEMORY_ENABLED,
	};
	if (!x86_memory_map_query(&info, &snapshot) ||
	    snapshot.usable_bytes != 7u * mebibyte ||
	    snapshot.highest_usable_address != 0x1001fffffull ||
	    snapshot.physical_address_limit != 0x100200000ull ||
	    snapshot.usable_extent_count != 3u ||
	    snapshot.firmware_range_count != 5u)
		return 1;
	return 0;
}

static int test_allocate_release(void)
{
	struct x86_boot_info info = valid_map();
	struct x86_guest_memory_lease_info lease_info;
	struct x86_guest_memory_snapshot snapshot;
	x86_guest_memory_lease_t first;
	x86_guest_memory_lease_t second;
	uint32_t address;
	uint32_t free_pages;
	uint32_t largest_pages;
	uint64_t highest_address;

	zero_result = X86_GUEST_ZERO_OK;
	zero_calls = 0u;
	x86_guest_memory_construct(&manager);
	if (x86_guest_memory_initialize(&manager, &info, &memory_ops,
					(kernel_object_handle_t)0x200u) !=
		X86_GUEST_MEMORY_OK ||
	    x86_guest_memory_query_free(&manager, &free_pages) !=
		X86_GUEST_MEMORY_OK ||
	    free_pages != 6u ||
	    x86_guest_memory_query_capacity(&manager, &largest_pages,
					      &free_pages,
					      &highest_address) !=
		X86_GUEST_MEMORY_OK ||
	    largest_pages != 4u || free_pages != 6u ||
	    x86_guest_memory_query_snapshot(&manager, &snapshot) !=
		X86_GUEST_MEMORY_OK ||
	    snapshot.largest_free_pages != 4u ||
	    snapshot.total_free_pages != 6u || snapshot.managed_pages != 6u ||
	    snapshot.highest_address != highest_address ||
	    highest_address != X86_GUEST_MEMORY_BASE +
				       8u * X86_GUEST_PAGE_BYTES - 1u ||
	    manager.managed_base != X86_GUEST_MEMORY_BASE ||
	    manager.managed_limit != X86_GUEST_MEMORY_BASE +
				     8u * X86_GUEST_PAGE_BYTES)
		return 1;
	if (x86_guest_memory_allocate(&manager, TEST_OWNER_A, 2u, &first,
				      &address) != X86_GUEST_MEMORY_OK ||
	    address != X86_GUEST_MEMORY_BASE || zero_calls != 1u ||
	    last_zero_address != address ||
	    last_zero_bytes != 2u * X86_GUEST_PAGE_BYTES)
		return 2;
	if (x86_guest_memory_allocate(&manager, TEST_OWNER_B, 2u, &second,
				      &address) != X86_GUEST_MEMORY_OK ||
	    address != X86_GUEST_MEMORY_BASE + 4u * X86_GUEST_PAGE_BYTES)
		return 3;
	if (x86_guest_memory_inspect(&manager, second, &lease_info) !=
		X86_GUEST_MEMORY_OK ||
	    lease_info.owner != TEST_OWNER_B || lease_info.page_count != 2u ||
	    lease_info.physical_address != address ||
	    x86_guest_memory_query_capacity(&manager, &largest_pages,
					      &free_pages,
					      &highest_address) !=
		X86_GUEST_MEMORY_OK ||
	    largest_pages != 2u || free_pages != 2u ||
	    x86_guest_memory_query_snapshot(&manager, &snapshot) !=
		X86_GUEST_MEMORY_OK ||
	    snapshot.largest_free_pages != 2u ||
	    snapshot.total_free_pages != 2u || snapshot.managed_pages != 6u)
		return 4;
	if (x86_guest_memory_release(&manager, TEST_OWNER_A, second) !=
		X86_GUEST_MEMORY_OWNER_MISMATCH ||
	    x86_guest_memory_release(&manager, TEST_OWNER_B, second) !=
		X86_GUEST_MEMORY_OK ||
	    x86_guest_memory_inspect(&manager, second, &lease_info) !=
		X86_GUEST_MEMORY_STALE_LEASE)
		return 5;
	if (x86_guest_memory_release(&manager, TEST_OWNER_A, first) !=
		X86_GUEST_MEMORY_OK ||
	    x86_guest_memory_query_free(&manager, &free_pages) !=
		X86_GUEST_MEMORY_OK ||
	    free_pages != 6u ||
	    x86_guest_memory_query_capacity(&manager, &largest_pages,
					      &free_pages,
					      &highest_address) !=
		X86_GUEST_MEMORY_OK ||
	    largest_pages != 4u || free_pages != 6u || zero_calls != 4u)
		return 6;
	return 0;
}

static int test_configured_aperture_is_only_a_capacity(void)
{
	struct x86_boot_info info = {0};
	struct x86_guest_memory_snapshot snapshot;
	uint64_t highest_address;
	uint32_t largest_pages;
	uint32_t total_pages;

	info.signature = X86_BOOT_INFO_SIGNATURE;
	info.version = X86_BOOT_INFO_VERSION;
	info.header_bytes = X86_BOOT_INFO_HEADER_BYTES;
	info.range_bytes = X86_BOOT_MEMORY_RANGE_BYTES;
	info.range_count = 1u;
	info.flags = X86_BOOT_INFO_FLAG_MASK;
	info.ranges[0] = (struct x86_boot_memory_range){
		.base = X86_GUEST_MEMORY_BASE,
		.length = X86_GUEST_MEMORY_APERTURE_LIMIT -
			  X86_GUEST_MEMORY_BASE,
		.type = X86_BOOT_MEMORY_USABLE,
		.attributes = X86_BOOT_MEMORY_ENABLED,
	};
	x86_guest_memory_construct(&failure_manager);
	if (x86_guest_memory_initialize(&failure_manager, &info, &memory_ops,
					(kernel_object_handle_t)0x200u) !=
			X86_GUEST_MEMORY_OK ||
	    x86_guest_memory_query_capacity(&failure_manager, &largest_pages,
					      &total_pages,
					      &highest_address) !=
			X86_GUEST_MEMORY_OK ||
	    largest_pages != X86_GUEST_MEMORY_PAGE_COUNT ||
	    total_pages != X86_GUEST_MEMORY_PAGE_COUNT ||
	    highest_address != X86_GUEST_MEMORY_APERTURE_LIMIT - 1u ||
	    x86_guest_memory_query_snapshot(&failure_manager, &snapshot) !=
		X86_GUEST_MEMORY_OK ||
	    snapshot.largest_free_pages != X86_GUEST_MEMORY_PAGE_COUNT ||
	    snapshot.total_free_pages != X86_GUEST_MEMORY_PAGE_COUNT ||
	    snapshot.managed_pages != X86_GUEST_MEMORY_PAGE_COUNT ||
	    snapshot.highest_address != highest_address ||
	    failure_manager.managed_base != X86_GUEST_MEMORY_BASE ||
	    failure_manager.managed_limit !=
			X86_GUEST_MEMORY_APERTURE_LIMIT)
		return 1;
	return 0;
}

static int test_zero_failures(void)
{
	struct x86_boot_info info = valid_map();
	x86_guest_memory_lease_t lease;
	uint32_t address;
	uint32_t free_pages;

	zero_result = X86_GUEST_ZERO_FAILED;
	x86_guest_memory_construct(&failure_manager);
	if (x86_guest_memory_initialize(&failure_manager, &info, &memory_ops,
					(kernel_object_handle_t)0x200u) !=
		X86_GUEST_MEMORY_OK ||
	    x86_guest_memory_allocate(&failure_manager, TEST_OWNER_A, 1u,
				      &lease, &address) !=
		X86_GUEST_MEMORY_ZERO_FAILED ||
	    x86_guest_memory_query_free(&failure_manager, &free_pages) !=
		X86_GUEST_MEMORY_OK ||
	    free_pages != 6u)
		return 1;
	zero_result = X86_GUEST_ZERO_UNCERTAIN;
	if (x86_guest_memory_allocate(&failure_manager, TEST_OWNER_A, 1u,
				      &lease, &address) !=
		X86_GUEST_MEMORY_POISONED ||
	    x86_guest_memory_query_free(&failure_manager, &free_pages) !=
		X86_GUEST_MEMORY_POISONED)
		return 2;
	return 0;
}

static int test_release_failure_retains_lease(void)
{
	struct x86_boot_info info = valid_map();
	struct x86_guest_memory_lease_info lease_info;
	x86_guest_memory_lease_t lease;
	uint32_t address;

	zero_result = X86_GUEST_ZERO_OK;
	x86_guest_memory_construct(&failure_manager);
	if (x86_guest_memory_initialize(&failure_manager, &info, &memory_ops,
					(kernel_object_handle_t)0x200u) !=
		X86_GUEST_MEMORY_OK ||
	    x86_guest_memory_allocate(&failure_manager, TEST_OWNER_A, 1u,
				      &lease, &address) !=
		X86_GUEST_MEMORY_OK)
		return 1;
	zero_result = X86_GUEST_ZERO_FAILED;
	if (x86_guest_memory_release(&failure_manager, TEST_OWNER_A, lease) !=
		X86_GUEST_MEMORY_ZERO_FAILED ||
	    x86_guest_memory_inspect(&failure_manager, lease, &lease_info) !=
		X86_GUEST_MEMORY_OK ||
	    lease_info.physical_address != address)
		return 2;
	return 0;
}

static int run_tests(void)
{
	int status;

	status = test_map_validation();
	if (status != 0)
		return 10 + status;
	status = test_topology_snapshot_is_not_allocator_capacity();
	if (status != 0)
		return 20 + status;
	status = test_allocate_release();
	if (status != 0)
		return 30 + status;
	status = test_zero_failures();
	if (status != 0)
		return 40 + status;
	status = test_release_failure_retains_lease();
	if (status != 0)
		return 50 + status;
	status = test_configured_aperture_is_only_a_capacity();
	if (status != 0)
		return 60 + status;
	return 0;
}

DOSC32_TEST_ENTRY(run_tests)

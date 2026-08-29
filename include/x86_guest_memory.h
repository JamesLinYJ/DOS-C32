/* SPDX-License-Identifier: GPL-2.0-only */
/* Generation-bound physical pages shared by XMS, EMS and x86 VM backends. */
#ifndef DOSC32_X86_GUEST_MEMORY_H
#define DOSC32_X86_GUEST_MEMORY_H

#include "address.h"
#include "compiler.h"
#include "types.h"
#include "x86_boot_info.h"
#include "x86_paging.h"

#define X86_GUEST_PAGE_SHIFT 12u
#define X86_GUEST_PAGE_BYTES (1u << X86_GUEST_PAGE_SHIFT)
#define X86_GUEST_MEMORY_BASE X86_BOOT_IDENTITY_FLOOR
#define X86_GUEST_MEMORY_APERTURE_LIMIT X86_BOOT_IDENTITY_LIMIT
#define X86_GUEST_MEMORY_PAGE_COUNT                                      \
	((X86_GUEST_MEMORY_APERTURE_LIMIT - X86_GUEST_MEMORY_BASE) /        \
	 X86_GUEST_PAGE_BYTES)
#define X86_GUEST_MEMORY_BITMAP_WORDS                                   \
	((X86_GUEST_MEMORY_PAGE_COUNT + 31u) / 32u)
#define X86_GUEST_MEMORY_LEASE_COUNT 1024u

typedef uint64_t x86_guest_memory_lease_t;

#define X86_GUEST_MEMORY_LEASE_INVALID ((x86_guest_memory_lease_t)0u)

enum x86_guest_memory_zero_status {
	X86_GUEST_ZERO_OK = 0,
	X86_GUEST_ZERO_FAILED,
	X86_GUEST_ZERO_UNCERTAIN
};

typedef enum x86_guest_memory_zero_status (*x86_guest_memory_zero_fn)(
	kernel_object_handle_t context, uint32_t physical_address,
	uint32_t byte_count);

struct x86_guest_memory_ops {
	x86_guest_memory_zero_fn zero;
};

enum x86_guest_memory_status {
	X86_GUEST_MEMORY_OK = 0,
	X86_GUEST_MEMORY_INVALID_ARGUMENT,
	X86_GUEST_MEMORY_INVALID_MAP,
	X86_GUEST_MEMORY_NOT_INITIALIZED,
	X86_GUEST_MEMORY_NO_MEMORY,
	X86_GUEST_MEMORY_NO_LEASE,
	X86_GUEST_MEMORY_STALE_LEASE,
	X86_GUEST_MEMORY_OWNER_MISMATCH,
	X86_GUEST_MEMORY_ZERO_FAILED,
	X86_GUEST_MEMORY_POISONED
};

enum x86_guest_memory_lease_state {
	X86_GUEST_MEMORY_LEASE_FREE = 0,
	X86_GUEST_MEMORY_LEASE_LIVE,
	X86_GUEST_MEMORY_LEASE_RETIRED
};

struct x86_guest_memory_lease_slot {
	uint64_t generation;
	kernel_object_handle_t owner;
	uint32_t first_page;
	uint32_t page_count;
	uint8_t state;
	uint8_t reserved[7];
} __aligned(8);

struct x86_guest_memory_manager {
	uint32_t available[X86_GUEST_MEMORY_BITMAP_WORDS];
	uint32_t allocated[X86_GUEST_MEMORY_BITMAP_WORDS];
	struct x86_guest_memory_lease_slot
		leases[X86_GUEST_MEMORY_LEASE_COUNT];
	const struct x86_guest_memory_ops *ops;
	kernel_object_handle_t zero_context;
	uint64_t managed_base;
	uint64_t managed_limit;
	uint32_t available_pages;
	uint32_t allocated_pages;
	uint32_t next_free_hint;
	uint8_t initialized;
	uint8_t constructed;
	uint8_t poisoned;
	uint8_t reserved[5];
} __aligned(8);

struct x86_guest_memory_lease_info {
	kernel_object_handle_t owner;
	uint32_t physical_address;
	uint32_t page_count;
} __aligned(8);

/* Runtime truth from the E820-derived owner; all counts use 4 KiB pages. */
struct x86_guest_memory_snapshot {
	uint64_t largest_free_pages;
	uint64_t total_free_pages;
	uint64_t managed_pages;
	uint64_t highest_address;
} __aligned(8);

void x86_guest_memory_construct(
	struct x86_guest_memory_manager *manager);
enum x86_guest_memory_status x86_guest_memory_initialize(
	struct x86_guest_memory_manager *manager,
	const struct x86_boot_info *boot_info,
	const struct x86_guest_memory_ops *ops,
	kernel_object_handle_t zero_context) __must_check;
enum x86_guest_memory_status x86_guest_memory_allocate(
	struct x86_guest_memory_manager *manager,
	kernel_object_handle_t owner, uint32_t page_count,
	x86_guest_memory_lease_t *lease,
	uint32_t *physical_address) __must_check;
enum x86_guest_memory_status x86_guest_memory_release(
	struct x86_guest_memory_manager *manager,
	kernel_object_handle_t owner,
	x86_guest_memory_lease_t lease) __must_check;
enum x86_guest_memory_status x86_guest_memory_inspect(
	const struct x86_guest_memory_manager *manager,
	x86_guest_memory_lease_t lease,
	struct x86_guest_memory_lease_info *info) __must_check;
enum x86_guest_memory_status x86_guest_memory_query_free(
	const struct x86_guest_memory_manager *manager,
	uint32_t *free_pages) __must_check;
enum x86_guest_memory_status x86_guest_memory_query_snapshot(
	const struct x86_guest_memory_manager *manager,
	struct x86_guest_memory_snapshot *snapshot) __must_check;
/* Compatibility view for existing consumers with compiled 32-bit counts. */
enum x86_guest_memory_status x86_guest_memory_query_capacity(
	const struct x86_guest_memory_manager *manager,
	uint32_t *largest_free_pages,
	uint32_t *total_free_pages,
	uint64_t *highest_address) __must_check;

static_assert_expression(sizeof(struct x86_guest_memory_lease_slot) == 32u,
			 "guest-memory lease layout changed");
static_assert_expression(sizeof(struct x86_guest_memory_lease_info) == 16u,
			 "guest-memory lease snapshot layout changed");
static_assert_expression(sizeof(struct x86_guest_memory_snapshot) == 32u,
			 "guest-memory capacity snapshot layout changed");

#endif

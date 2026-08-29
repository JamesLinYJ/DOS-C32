// SPDX-License-Identifier: GPL-2.0-only
/*
 * Fixed-capacity x86 guest physical-page owner.
 *
 * Bitmap allocation, range validation and acquire-before-publish ownership
 * keep physical-page state bounded and auditable.
 */
#include "x86_guest_memory.h"

#include "x86_memory_map.h"

#define X86_GUEST_MEMORY_LEASE_SLOT_BITS 11u
#define X86_GUEST_MEMORY_LEASE_SLOT_MASK 0x7ffull
#define X86_GUEST_MEMORY_GENERATION_MAX 0x001fffffffffffffull

static_assert_expression(X86_GUEST_MEMORY_APERTURE_LIMIT <=
			 X86_BOOT_IDENTITY_LIMIT,
			 "guest pages must remain in the boot identity map");

static bool valid_identity(kernel_object_handle_t identity)
{
	return identity != 0u && identity != KERNEL_OBJECT_HANDLE_INVALID;
}

static bool bit_is_set(const uint32_t *bitmap, uint32_t page)
{
	return (bitmap[page >> 5u] & (1u << (page & 31u))) != 0u;
}

static void bit_set(uint32_t *bitmap, uint32_t page)
{
	bitmap[page >> 5u] |= 1u << (page & 31u);
}

static void bit_clear(uint32_t *bitmap, uint32_t page)
{
	bitmap[page >> 5u] &= ~(1u << (page & 31u));
}

static uint64_t align_up_page(uint64_t value)
{
	uint64_t mask = (uint64_t)X86_GUEST_PAGE_BYTES - 1u;

	if (value > ~(uint64_t)0u - mask)
		return ~(uint64_t)0u;
	return (value + mask) & ~mask;
}

static uint64_t align_down_page(uint64_t value)
{
	return value & ~((uint64_t)X86_GUEST_PAGE_BYTES - 1u);
}

static void change_range_availability(
	struct x86_guest_memory_manager *manager,
	const struct x86_boot_memory_range *range, bool available)
{
	uint64_t range_end = range->base + range->length;
	uint64_t first;
	uint64_t end;
	uint32_t page;
	uint32_t last;

	if (available) {
		first = align_up_page(range->base);
		end = align_down_page(range_end);
	} else {
		first = align_down_page(range->base);
		end = align_up_page(range_end);
	}
	if (first < X86_GUEST_MEMORY_BASE)
		first = X86_GUEST_MEMORY_BASE;
	if (end > X86_GUEST_MEMORY_APERTURE_LIMIT)
		end = X86_GUEST_MEMORY_APERTURE_LIMIT;
	if (first >= end || first >= X86_GUEST_MEMORY_APERTURE_LIMIT ||
	    end <= X86_GUEST_MEMORY_BASE)
		return;
	page = (uint32_t)((first - X86_GUEST_MEMORY_BASE) /
			  X86_GUEST_PAGE_BYTES);
	last = (uint32_t)((end - X86_GUEST_MEMORY_BASE) /
			  X86_GUEST_PAGE_BYTES);
	for (; page < last; ++page) {
		if (available)
			bit_set(manager->available, page);
		else
			bit_clear(manager->available, page);
	}
}

static uint32_t count_available(
	const struct x86_guest_memory_manager *manager)
{
	uint32_t count = 0u;
	uint32_t page;

	for (page = 0u; page < X86_GUEST_MEMORY_PAGE_COUNT; ++page) {
		if (bit_is_set(manager->available, page))
			++count;
	}
	return count;
}

void x86_guest_memory_construct(
	struct x86_guest_memory_manager *manager)
{
	uint32_t index;

	if (manager == NULL)
		return;
	for (index = 0u; index < X86_GUEST_MEMORY_BITMAP_WORDS; ++index) {
		manager->available[index] = 0u;
		manager->allocated[index] = 0u;
	}
	for (index = 0u; index < X86_GUEST_MEMORY_LEASE_COUNT; ++index)
		manager->leases[index] =
			(struct x86_guest_memory_lease_slot){0};
	manager->ops = NULL;
	manager->zero_context = KERNEL_OBJECT_HANDLE_INVALID;
	manager->managed_base = 0u;
	manager->managed_limit = 0u;
	manager->available_pages = 0u;
	manager->allocated_pages = 0u;
	manager->next_free_hint = (uint32_t)(
		(manager->managed_base - X86_GUEST_MEMORY_BASE) /
		X86_GUEST_PAGE_BYTES);
	manager->initialized = 0u;
	manager->constructed = 1u;
	manager->poisoned = 0u;
	for (index = 0u; index < ARRAY_SIZE(manager->reserved); ++index)
		manager->reserved[index] = 0u;
}

enum x86_guest_memory_status x86_guest_memory_initialize(
	struct x86_guest_memory_manager *manager,
	const struct x86_boot_info *boot_info,
	const struct x86_guest_memory_ops *ops,
	kernel_object_handle_t zero_context)
{
	uint16_t index;
	uint32_t word;

	if (manager == NULL || ops == NULL || ops->zero == NULL ||
	    !valid_identity(zero_context))
		return X86_GUEST_MEMORY_INVALID_ARGUMENT;
	if (manager->constructed != 1u || manager->initialized != 0u)
		return X86_GUEST_MEMORY_INVALID_ARGUMENT;
	if (!x86_memory_map_is_valid(boot_info))
		return X86_GUEST_MEMORY_INVALID_MAP;
	for (word = 0u; word < X86_GUEST_MEMORY_BITMAP_WORDS; ++word) {
		manager->available[word] = 0u;
		manager->allocated[word] = 0u;
	}
	for (index = 0u; index < X86_GUEST_MEMORY_LEASE_COUNT; ++index)
		manager->leases[index] =
			(struct x86_guest_memory_lease_slot){0};
	for (index = 0u; index < boot_info->range_count; ++index) {
		if (boot_info->ranges[index].type == X86_BOOT_MEMORY_USABLE)
			change_range_availability(manager,
				&boot_info->ranges[index], true);
	}
	/* Reserved overlaps win regardless of firmware entry order. */
	for (index = 0u; index < boot_info->range_count; ++index) {
		if (boot_info->ranges[index].type != X86_BOOT_MEMORY_USABLE)
			change_range_availability(manager,
				&boot_info->ranges[index], false);
	}
	manager->ops = ops;
	manager->zero_context = zero_context;
	manager->available_pages = count_available(manager);
	if (manager->available_pages == 0u) {
		manager->ops = NULL;
		manager->zero_context = KERNEL_OBJECT_HANDLE_INVALID;
		return X86_GUEST_MEMORY_INVALID_MAP;
	}
	manager->managed_base = 0u;
	manager->managed_limit = 0u;
	for (word = 0u; word < X86_GUEST_MEMORY_PAGE_COUNT; ++word) {
		if (!bit_is_set(manager->available, word))
			continue;
		if (manager->managed_base == 0u)
			manager->managed_base =
				(uint64_t)X86_GUEST_MEMORY_BASE +
				(uint64_t)word * X86_GUEST_PAGE_BYTES;
		manager->managed_limit =
			(uint64_t)X86_GUEST_MEMORY_BASE +
			(uint64_t)(word + 1u) * X86_GUEST_PAGE_BYTES;
	}
	if (manager->managed_base == 0u ||
	    manager->managed_limit <= manager->managed_base) {
		manager->ops = NULL;
		manager->zero_context = KERNEL_OBJECT_HANDLE_INVALID;
		manager->available_pages = 0u;
		return X86_GUEST_MEMORY_INVALID_MAP;
	}
	manager->allocated_pages = 0u;
	manager->next_free_hint = 0u;
	manager->poisoned = 0u;
	manager->initialized = 1u;
	for (index = 0u; index < ARRAY_SIZE(manager->reserved); ++index)
		manager->reserved[index] = 0u;
	return X86_GUEST_MEMORY_OK;
}

static uint32_t find_lease_slot(
	struct x86_guest_memory_manager *manager)
{
	uint32_t index;

	for (index = 0u; index < X86_GUEST_MEMORY_LEASE_COUNT; ++index) {
		struct x86_guest_memory_lease_slot *slot =
			&manager->leases[index];

		if (slot->state != X86_GUEST_MEMORY_LEASE_FREE)
			continue;
		if (slot->generation >= X86_GUEST_MEMORY_GENERATION_MAX) {
			slot->state = X86_GUEST_MEMORY_LEASE_RETIRED;
			continue;
		}
		return index;
	}
	return X86_GUEST_MEMORY_LEASE_COUNT;
}

static bool page_is_free(const struct x86_guest_memory_manager *manager,
			 uint32_t page)
{
	return bit_is_set(manager->available, page) &&
	       !bit_is_set(manager->allocated, page);
}

static uint32_t managed_first_page(
	const struct x86_guest_memory_manager *manager)
{
	return (uint32_t)((manager->managed_base - X86_GUEST_MEMORY_BASE) /
			  X86_GUEST_PAGE_BYTES);
}

static uint32_t managed_page_limit(
	const struct x86_guest_memory_manager *manager)
{
	return (uint32_t)((manager->managed_limit - X86_GUEST_MEMORY_BASE) /
			  X86_GUEST_PAGE_BYTES);
}

static bool run_is_free(const struct x86_guest_memory_manager *manager,
			uint32_t first, uint32_t count)
{
	uint32_t index;

	if (count == 0u || count > managed_page_limit(manager) ||
	    first > managed_page_limit(manager) - count)
		return false;
	for (index = 0u; index < count; ++index) {
		if (!page_is_free(manager, first + index))
			return false;
	}
	return true;
}

static uint32_t find_free_run(
	const struct x86_guest_memory_manager *manager, uint32_t count)
{
	uint32_t end;
	uint32_t first;
	uint32_t page;
	uint32_t limit;
	uint32_t start;

	if (manager->allocated_pages > manager->available_pages ||
	    count == 0u || count > X86_GUEST_MEMORY_PAGE_COUNT ||
	    count > manager->available_pages - manager->allocated_pages)
		return X86_GUEST_MEMORY_PAGE_COUNT;
	first = managed_first_page(manager);
	end = managed_page_limit(manager);
	if (count > end - first)
		return X86_GUEST_MEMORY_PAGE_COUNT;
	limit = end - count;
	start = manager->next_free_hint <= limit
			? manager->next_free_hint
			: first;
	if (start < first)
		start = first;
	for (page = start; page <= limit; ++page) {
		if (run_is_free(manager, page, count))
			return page;
	}
	for (page = first; page < start && page <= limit; ++page) {
		if (run_is_free(manager, page, count))
			return page;
	}
	return X86_GUEST_MEMORY_PAGE_COUNT;
}

static x86_guest_memory_lease_t make_lease(uint32_t slot,
					    uint64_t generation)
{
	return (generation << X86_GUEST_MEMORY_LEASE_SLOT_BITS) |
	       (x86_guest_memory_lease_t)(slot + 1u);
}

static enum x86_guest_memory_status resolve_lease(
	const struct x86_guest_memory_manager *manager,
	x86_guest_memory_lease_t lease, uint32_t *slot_index)
{
	uint64_t generation;
	uint32_t encoded_slot;
	const struct x86_guest_memory_lease_slot *slot;

	if (lease == X86_GUEST_MEMORY_LEASE_INVALID ||
	    lease == KERNEL_OBJECT_HANDLE_INVALID || slot_index == NULL)
		return X86_GUEST_MEMORY_STALE_LEASE;
	encoded_slot = (uint32_t)(lease &
				  X86_GUEST_MEMORY_LEASE_SLOT_MASK);
	generation = lease >> X86_GUEST_MEMORY_LEASE_SLOT_BITS;
	if (encoded_slot == 0u ||
	    encoded_slot > X86_GUEST_MEMORY_LEASE_COUNT || generation == 0u ||
	    generation > X86_GUEST_MEMORY_GENERATION_MAX)
		return X86_GUEST_MEMORY_STALE_LEASE;
	slot = &manager->leases[encoded_slot - 1u];
	if (slot->state != X86_GUEST_MEMORY_LEASE_LIVE ||
	    slot->generation != generation)
		return X86_GUEST_MEMORY_STALE_LEASE;
	*slot_index = encoded_slot - 1u;
	return X86_GUEST_MEMORY_OK;
}

enum x86_guest_memory_status x86_guest_memory_allocate(
	struct x86_guest_memory_manager *manager,
	kernel_object_handle_t owner, uint32_t page_count,
	x86_guest_memory_lease_t *lease, uint32_t *physical_address)
{
	struct x86_guest_memory_lease_slot *slot;
	enum x86_guest_memory_zero_status zero_status;
	uint32_t slot_index;
	uint32_t first_page;
	uint32_t index;
	uint32_t address;
	uint32_t byte_count;

	if (manager == NULL || lease == NULL || physical_address == NULL ||
	    !valid_identity(owner) || page_count == 0u)
		return X86_GUEST_MEMORY_INVALID_ARGUMENT;
	if (manager->initialized != 1u)
		return X86_GUEST_MEMORY_NOT_INITIALIZED;
	if (manager->poisoned != 0u)
		return X86_GUEST_MEMORY_POISONED;
	if (manager->allocated_pages > manager->available_pages) {
		manager->poisoned = 1u;
		return X86_GUEST_MEMORY_POISONED;
	}
	if (page_count > X86_GUEST_MEMORY_PAGE_COUNT ||
	    page_count > manager->available_pages - manager->allocated_pages)
		return X86_GUEST_MEMORY_NO_MEMORY;
	slot_index = find_lease_slot(manager);
	if (slot_index == X86_GUEST_MEMORY_LEASE_COUNT)
		return X86_GUEST_MEMORY_NO_LEASE;
	first_page = find_free_run(manager, page_count);
	if (first_page == X86_GUEST_MEMORY_PAGE_COUNT)
		return X86_GUEST_MEMORY_NO_MEMORY;
	address = X86_GUEST_MEMORY_BASE +
		  first_page * X86_GUEST_PAGE_BYTES;
	byte_count = page_count * X86_GUEST_PAGE_BYTES;
	zero_status = manager->ops->zero(manager->zero_context, address,
					 byte_count);
	if (zero_status == X86_GUEST_ZERO_FAILED)
		return X86_GUEST_MEMORY_ZERO_FAILED;
	if (zero_status != X86_GUEST_ZERO_OK) {
		manager->poisoned = 1u;
		return X86_GUEST_MEMORY_POISONED;
	}
	for (index = 0u; index < page_count; ++index)
		bit_set(manager->allocated, first_page + index);
	slot = &manager->leases[slot_index];
	++slot->generation;
	slot->owner = owner;
	slot->first_page = first_page;
	slot->page_count = page_count;
	slot->state = X86_GUEST_MEMORY_LEASE_LIVE;
	for (index = 0u; index < ARRAY_SIZE(slot->reserved); ++index)
		slot->reserved[index] = 0u;
	manager->allocated_pages += page_count;
	manager->next_free_hint = first_page + page_count;
	if (manager->next_free_hint >= managed_page_limit(manager))
		manager->next_free_hint = managed_first_page(manager);
	*lease = make_lease(slot_index, slot->generation);
	*physical_address = address;
	return X86_GUEST_MEMORY_OK;
}

enum x86_guest_memory_status x86_guest_memory_release(
	struct x86_guest_memory_manager *manager,
	kernel_object_handle_t owner, x86_guest_memory_lease_t lease)
{
	struct x86_guest_memory_lease_slot *slot;
	enum x86_guest_memory_zero_status zero_status;
	enum x86_guest_memory_status status;
	uint32_t slot_index;
	uint32_t index;
	uint32_t address;
	uint32_t byte_count;

	if (manager == NULL || !valid_identity(owner))
		return X86_GUEST_MEMORY_INVALID_ARGUMENT;
	if (manager->initialized != 1u)
		return X86_GUEST_MEMORY_NOT_INITIALIZED;
	if (manager->poisoned != 0u)
		return X86_GUEST_MEMORY_POISONED;
	status = resolve_lease(manager, lease, &slot_index);
	if (status != X86_GUEST_MEMORY_OK)
		return status;
	slot = &manager->leases[slot_index];
	if (slot->owner != owner)
		return X86_GUEST_MEMORY_OWNER_MISMATCH;
	if (slot->page_count == 0u ||
	    slot->first_page > X86_GUEST_MEMORY_PAGE_COUNT -
				      slot->page_count ||
	    slot->page_count > manager->allocated_pages) {
		manager->poisoned = 1u;
		return X86_GUEST_MEMORY_POISONED;
	}
	for (index = 0u; index < slot->page_count; ++index) {
		if (!bit_is_set(manager->allocated,
				slot->first_page + index)) {
			manager->poisoned = 1u;
			return X86_GUEST_MEMORY_POISONED;
		}
	}
	address = X86_GUEST_MEMORY_BASE +
		  slot->first_page * X86_GUEST_PAGE_BYTES;
	byte_count = slot->page_count * X86_GUEST_PAGE_BYTES;
	zero_status = manager->ops->zero(manager->zero_context, address,
					 byte_count);
	if (zero_status == X86_GUEST_ZERO_FAILED)
		return X86_GUEST_MEMORY_ZERO_FAILED;
	if (zero_status != X86_GUEST_ZERO_OK) {
		manager->poisoned = 1u;
		return X86_GUEST_MEMORY_POISONED;
	}
	for (index = 0u; index < slot->page_count; ++index)
		bit_clear(manager->allocated, slot->first_page + index);
	manager->allocated_pages -= slot->page_count;
	if (slot->first_page < manager->next_free_hint)
		manager->next_free_hint = slot->first_page;
	slot->owner = KERNEL_OBJECT_HANDLE_INVALID;
	slot->first_page = 0u;
	slot->page_count = 0u;
	slot->state = X86_GUEST_MEMORY_LEASE_FREE;
	return X86_GUEST_MEMORY_OK;
}

enum x86_guest_memory_status x86_guest_memory_inspect(
	const struct x86_guest_memory_manager *manager,
	x86_guest_memory_lease_t lease,
	struct x86_guest_memory_lease_info *info)
{
	enum x86_guest_memory_status status;
	uint32_t slot_index;

	if (manager == NULL || info == NULL)
		return X86_GUEST_MEMORY_INVALID_ARGUMENT;
	if (manager->initialized != 1u)
		return X86_GUEST_MEMORY_NOT_INITIALIZED;
	if (manager->poisoned != 0u)
		return X86_GUEST_MEMORY_POISONED;
	status = resolve_lease(manager, lease, &slot_index);
	if (status != X86_GUEST_MEMORY_OK)
		return status;
	*info = (struct x86_guest_memory_lease_info){
		.owner = manager->leases[slot_index].owner,
		.physical_address =
			X86_GUEST_MEMORY_BASE +
			manager->leases[slot_index].first_page *
				X86_GUEST_PAGE_BYTES,
		.page_count = manager->leases[slot_index].page_count,
	};
	return X86_GUEST_MEMORY_OK;
}

enum x86_guest_memory_status x86_guest_memory_query_free(
	const struct x86_guest_memory_manager *manager,
	uint32_t *free_pages)
{
	if (manager == NULL || free_pages == NULL)
		return X86_GUEST_MEMORY_INVALID_ARGUMENT;
	if (manager->initialized != 1u)
		return X86_GUEST_MEMORY_NOT_INITIALIZED;
	if (manager->poisoned != 0u)
		return X86_GUEST_MEMORY_POISONED;
	if (manager->allocated_pages > manager->available_pages)
		return X86_GUEST_MEMORY_POISONED;
	*free_pages = manager->available_pages - manager->allocated_pages;
	return X86_GUEST_MEMORY_OK;
}

enum x86_guest_memory_status x86_guest_memory_query_snapshot(
	const struct x86_guest_memory_manager *manager,
	struct x86_guest_memory_snapshot *snapshot)
{
	uint32_t current = 0u;
	uint32_t first;
	uint32_t largest = 0u;
	uint32_t limit;
	uint32_t page;

	if (manager == NULL || snapshot == NULL)
		return X86_GUEST_MEMORY_INVALID_ARGUMENT;
	if (manager->initialized != 1u)
		return X86_GUEST_MEMORY_NOT_INITIALIZED;
	if (manager->poisoned != 0u ||
	    manager->allocated_pages > manager->available_pages)
		return X86_GUEST_MEMORY_POISONED;
	if (manager->managed_base < X86_GUEST_MEMORY_BASE ||
	    manager->managed_limit <= manager->managed_base ||
	    manager->managed_limit > X86_GUEST_MEMORY_APERTURE_LIMIT)
		return X86_GUEST_MEMORY_POISONED;
	first = managed_first_page(manager);
	limit = managed_page_limit(manager);
	for (page = first; page < limit; ++page) {
		if (!page_is_free(manager, page)) {
			current = 0u;
			continue;
		}
		++current;
		if (current > largest)
			largest = current;
	}
	*snapshot = (struct x86_guest_memory_snapshot){
		.largest_free_pages = largest,
		.total_free_pages =
			manager->available_pages - manager->allocated_pages,
		.managed_pages = manager->available_pages,
		.highest_address = manager->managed_limit - 1u,
	};
	return X86_GUEST_MEMORY_OK;
}

enum x86_guest_memory_status x86_guest_memory_query_capacity(
	const struct x86_guest_memory_manager *manager,
	uint32_t *largest_free_pages, uint32_t *total_free_pages,
	uint64_t *highest_address)
{
	struct x86_guest_memory_snapshot snapshot;
	enum x86_guest_memory_status status;

	if (largest_free_pages == NULL || total_free_pages == NULL ||
	    highest_address == NULL)
		return X86_GUEST_MEMORY_INVALID_ARGUMENT;
	status = x86_guest_memory_query_snapshot(manager, &snapshot);
	if (status != X86_GUEST_MEMORY_OK)
		return status;
	if (snapshot.largest_free_pages > 0xffffffffu ||
	    snapshot.total_free_pages > 0xffffffffu)
		return X86_GUEST_MEMORY_POISONED;
	*largest_free_pages = (uint32_t)snapshot.largest_free_pages;
	*total_free_pages = (uint32_t)snapshot.total_free_pages;
	*highest_address = snapshot.highest_address;
	return X86_GUEST_MEMORY_OK;
}

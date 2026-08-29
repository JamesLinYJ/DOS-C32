// SPDX-License-Identifier: GPL-2.0-only
/*
 * Page-level private COW for legacy BIOS runtime storage.
 *
 * PC firmware may keep mutable interrupt-frame state in
 * the otherwise reserved C0000h-FFFFFh aperture. The base mapping remains
 * guest-readable and read-only. A precise user write-protection fault may
 * replace only its faulting page with a guest-owned RAM page; the real ROM or
 * MMIO backing is never written and the RAM page's identity mapping remains
 * supervisor-only.
 */
#include "firmware_shadow.h"

#include "../../../config/x86-guest-space.h"
#include "address.h"
#include "string.h"
#include "x86_guest_memory_runtime.h"

#define X86_PAGE_FAULT_PRESENT (1u << 0)
#define X86_PAGE_FAULT_WRITE (1u << 1)
#define X86_PAGE_FAULT_USER (1u << 2)
#define X86_FIRMWARE_WRITE_FAULT                                      \
	(X86_PAGE_FAULT_PRESENT | X86_PAGE_FAULT_WRITE | X86_PAGE_FAULT_USER)
#define X86_FIRMWARE_FIRST_GENERATION 1u
#define X86_FIRMWARE_GENERATION_MAX ((uint64_t)-2)

enum x86_firmware_page_state {
	X86_FIRMWARE_PAGE_EMPTY = 0,
	X86_FIRMWARE_PAGE_LEASED,
	X86_FIRMWARE_PAGE_ACTIVE,
	X86_FIRMWARE_PAGE_RESTORED
};

struct x86_firmware_client_slot {
	uint64_t generation;
	uint64_t execution_generation;
	uint8_t active;
	uint8_t reserved[7];
} __aligned(8);

struct x86_firmware_shadow_page {
	struct x86_paging_guest_shadow_snapshot snapshot;
	x86_guest_memory_lease_t lease;
	uint32_t linear_page;
	uint32_t physical_page;
	uint8_t state;
	uint8_t reserved[7];
} __aligned(8);

struct x86_firmware_shadow_owner {
	struct x86_paging_binding paging;
	struct x86_firmware_client_slot
		clients[CONFIG_X86_GUEST_FIRMWARE_CLIENT_CAPACITY];
	struct x86_firmware_shadow_page
		pages[CONFIG_X86_GUEST_FIRMWARE_SHADOW_PAGE_CAPACITY];
	kernel_object_handle_t address_space_identity;
	kernel_object_handle_t machine_identity;
	uint64_t address_space_generation;
	uint64_t execution_generation;
	uint32_t client_count;
	uint32_t page_count;
	uint8_t initialized;
	uint8_t poisoned;
	uint8_t cleanup_corrupted;
	uint8_t reserved[5];
} __aligned(8);

static struct x86_firmware_shadow_owner shadow_owner;

static_assert_expression(CONFIG_X86_GUEST_FIRMWARE_CLIENT_CAPACITY > 0u,
	"firmware shadow needs at least one execution client");
static_assert_expression(CONFIG_X86_GUEST_FIRMWARE_SHADOW_PAGE_CAPACITY > 0u,
	"firmware shadow needs at least one private page slot");
static_assert_expression(
	CONFIG_X86_GUEST_FIRMWARE_SHADOW_PAGE_CAPACITY <=
		(X86_LEGACY_ROM_LIMIT - X86_DOS_VIDEO_LIMIT) / X86_PAGE_BYTES,
	"firmware shadow capacity cannot exceed the firmware aperture");
static_assert_expression(sizeof(struct x86_firmware_client_slot) == 24u,
	"firmware client slots must remain fixed width");
static_assert_expression(sizeof(struct x86_firmware_shadow_page) == 56u,
	"firmware page records must remain fixed width");

static bool identity_is_valid(kernel_object_handle_t identity)
{
	return identity != 0u && identity != KERNEL_OBJECT_HANDLE_INVALID;
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

static bool owner_is_active(void)
{
	return shadow_owner.initialized == 1u && shadow_owner.poisoned == 0u &&
	       x86_paging_binding_is_active(&shadow_owner.paging);
}

#if defined(DOSC32_HOST_TEST)
bool x86_firmware_shadow_test_physical_map(
	uint32_t physical_address, size_t count, bool writable,
	struct native_mapping *mapping);
#endif

static bool map_physical_page(uint32_t physical_address, bool writable,
			      struct native_mapping *mapping)
{
#if defined(DOSC32_HOST_TEST)
	return x86_firmware_shadow_test_physical_map(
		physical_address, X86_PAGE_BYTES, writable, mapping);
#else
	(void)writable;
	return kernel_address_identity_map(physical_address, X86_PAGE_BYTES,
					   mapping) == ADDRESS_OK &&
	       mapping->length == X86_PAGE_BYTES;
#endif
}

static bool binding_header_matches(
	const struct x86_guest_space_firmware_binding *binding)
{
	return binding != NULL &&
	       binding->address_space_identity ==
		       shadow_owner.address_space_identity &&
	       binding->address_space_generation ==
		       shadow_owner.address_space_generation &&
	       binding->machine_identity == shadow_owner.machine_identity &&
	       binding->execution_generation ==
		       shadow_owner.execution_generation &&
	       binding->client_slot <
		       CONFIG_X86_GUEST_FIRMWARE_CLIENT_CAPACITY &&
	       bytes_are_zero(binding->reserved,
			      ARRAY_SIZE(binding->reserved));
}

static struct x86_firmware_client_slot *resolve_client(
	const struct x86_guest_space_firmware_binding *binding)
{
	struct x86_firmware_client_slot *slot;

	if (!binding_header_matches(binding))
		return NULL;
	slot = &shadow_owner.clients[binding->client_slot];
	if (slot->generation != binding->client_generation ||
	    slot->execution_generation != binding->execution_generation)
		return NULL;
	return slot;
}

static struct x86_firmware_shadow_page *find_page(uint32_t linear_page)
{
	size_t index;

	for (index = 0u;
	     index < CONFIG_X86_GUEST_FIRMWARE_SHADOW_PAGE_CAPACITY; ++index) {
		struct x86_firmware_shadow_page *page =
			&shadow_owner.pages[index];

		if (page->state != (uint8_t)X86_FIRMWARE_PAGE_EMPTY &&
		    page->linear_page == linear_page)
			return page;
	}
	return NULL;
}

static struct x86_firmware_shadow_page *find_empty_page(void)
{
	size_t index;

	for (index = 0u;
	     index < CONFIG_X86_GUEST_FIRMWARE_SHADOW_PAGE_CAPACITY; ++index) {
		if (shadow_owner.pages[index].state ==
		    (uint8_t)X86_FIRMWARE_PAGE_EMPTY)
			return &shadow_owner.pages[index];
	}
	return NULL;
}

static void clear_page(struct x86_firmware_shadow_page *page)
{
	*page = (struct x86_firmware_shadow_page){0};
	if (shadow_owner.page_count > 0u)
		--shadow_owner.page_count;
}

static enum x86_firmware_shadow_status release_private_page(
	struct x86_firmware_shadow_page *page, bool *mapping_corrupted)
{
	enum x86_guest_memory_status memory_status;
	enum x86_paging_guest_shadow_status paging_status;

	if (page->state == (uint8_t)X86_FIRMWARE_PAGE_ACTIVE) {
		paging_status = x86_paging_guest_shadow_restore(
			&page->snapshot, page->physical_page);
		if (paging_status == X86_PAGING_GUEST_SHADOW_OK) {
			page->state = (uint8_t)X86_FIRMWARE_PAGE_RESTORED;
		} else if (paging_status ==
			   X86_PAGING_GUEST_SHADOW_MAPPING_MISMATCH) {
			/* The implementation forced the saved safe mapping. */
			page->state = (uint8_t)X86_FIRMWARE_PAGE_RESTORED;
			shadow_owner.cleanup_corrupted = 1u;
			*mapping_corrupted = true;
		} else {
			return X86_FIRMWARE_SHADOW_POISONED;
		}
	}
	if (page->state != (uint8_t)X86_FIRMWARE_PAGE_LEASED &&
	    page->state != (uint8_t)X86_FIRMWARE_PAGE_RESTORED)
		return X86_FIRMWARE_SHADOW_POISONED;
	memory_status = x86_guest_memory_runtime_release(
		shadow_owner.address_space_identity, page->lease);
	if (memory_status == X86_GUEST_MEMORY_ZERO_FAILED)
		return X86_FIRMWARE_SHADOW_RETRY;
	if (memory_status != X86_GUEST_MEMORY_OK)
		return X86_FIRMWARE_SHADOW_POISONED;
	clear_page(page);
	return X86_FIRMWARE_SHADOW_OK;
}

static enum x86_firmware_shadow_status rollback_private_page(
	struct x86_firmware_shadow_page *page,
	enum x86_firmware_shadow_status original_status)
{
	bool mapping_corrupted = false;
	enum x86_firmware_shadow_status release_status =
		release_private_page(page, &mapping_corrupted);

	if (release_status == X86_FIRMWARE_SHADOW_OK && !mapping_corrupted)
		return original_status;
	shadow_owner.poisoned = 1u;
	return X86_FIRMWARE_SHADOW_POISONED;
}

enum x86_firmware_shadow_status x86_firmware_shadow_initialize(
	kernel_object_handle_t address_space_identity,
	kernel_object_handle_t machine_identity, uint64_t address_space_generation,
	const struct x86_paging_binding *paging)
{
	if (!identity_is_valid(address_space_identity) ||
	    !identity_is_valid(machine_identity) ||
	    address_space_identity == machine_identity ||
	    address_space_generation == 0u || paging == NULL)
		return X86_FIRMWARE_SHADOW_INVALID_ARGUMENT;
	if (shadow_owner.initialized != 0u)
		return X86_FIRMWARE_SHADOW_INVALID_STATE;
	if (!x86_paging_binding_is_active(paging))
		return X86_FIRMWARE_SHADOW_PAGING_MISMATCH;
	shadow_owner = (struct x86_firmware_shadow_owner){0};
	shadow_owner.paging = *paging;
	shadow_owner.address_space_identity = address_space_identity;
	shadow_owner.machine_identity = machine_identity;
	shadow_owner.address_space_generation = address_space_generation;
	shadow_owner.initialized = 1u;
	return X86_FIRMWARE_SHADOW_OK;
}

enum x86_firmware_shadow_status x86_firmware_shadow_execution_acquire(
	kernel_object_handle_t machine_identity,
	struct x86_guest_space_firmware_binding *binding)
{
	struct x86_guest_space_firmware_binding prepared;
	size_t index;

	if (!identity_is_valid(machine_identity) || binding == NULL)
		return X86_FIRMWARE_SHADOW_INVALID_ARGUMENT;
	if (!owner_is_active())
		return shadow_owner.poisoned != 0u
			       ? X86_FIRMWARE_SHADOW_POISONED
			       : X86_FIRMWARE_SHADOW_INVALID_STATE;
	if (machine_identity != shadow_owner.machine_identity)
		return X86_FIRMWARE_SHADOW_MACHINE_MISMATCH;
	if (shadow_owner.client_count == 0u) {
		if (shadow_owner.page_count != 0u ||
		    shadow_owner.execution_generation >=
			    X86_FIRMWARE_GENERATION_MAX) {
			shadow_owner.poisoned = 1u;
			return X86_FIRMWARE_SHADOW_POISONED;
		}
		++shadow_owner.execution_generation;
		if (shadow_owner.execution_generation <
		    X86_FIRMWARE_FIRST_GENERATION)
			shadow_owner.execution_generation =
				X86_FIRMWARE_FIRST_GENERATION;
	}
	for (index = 0u; index < CONFIG_X86_GUEST_FIRMWARE_CLIENT_CAPACITY;
	     ++index) {
		struct x86_firmware_client_slot *slot =
			&shadow_owner.clients[index];

		if (slot->active != 0u ||
		    slot->generation >= X86_FIRMWARE_GENERATION_MAX)
			continue;
		++slot->generation;
		slot->execution_generation = shadow_owner.execution_generation;
		slot->active = 1u;
		++shadow_owner.client_count;
		prepared = (struct x86_guest_space_firmware_binding){
			.address_space_identity =
				shadow_owner.address_space_identity,
			.address_space_generation =
				shadow_owner.address_space_generation,
			.machine_identity = shadow_owner.machine_identity,
			.execution_generation =
				shadow_owner.execution_generation,
			.client_generation = slot->generation,
			.client_slot = (uint32_t)index,
			.reserved = {0u},
		};
		*binding = prepared;
		return X86_FIRMWARE_SHADOW_OK;
	}
	return X86_FIRMWARE_SHADOW_CAPACITY_EXHAUSTED;
}

enum x86_firmware_shadow_status x86_firmware_shadow_execution_release(
	kernel_object_handle_t machine_identity,
	const struct x86_guest_space_firmware_binding *binding)
{
	struct x86_firmware_client_slot *client;
	bool mapping_corrupted = false;
	size_t index;

	if (!identity_is_valid(machine_identity) || binding == NULL)
		return X86_FIRMWARE_SHADOW_INVALID_ARGUMENT;
	if (shadow_owner.initialized != 1u)
		return X86_FIRMWARE_SHADOW_INVALID_STATE;
	if (machine_identity != shadow_owner.machine_identity)
		return X86_FIRMWARE_SHADOW_MACHINE_MISMATCH;
	client = resolve_client(binding);
	if (client == NULL)
		return X86_FIRMWARE_SHADOW_STALE_BINDING;
	/* Exact duplicate release is harmless until this slot is reused. */
	if (client->active == 0u)
		return X86_FIRMWARE_SHADOW_OK;
	if (shadow_owner.poisoned != 0u)
		return X86_FIRMWARE_SHADOW_POISONED;
	if (shadow_owner.client_count == 0u) {
		shadow_owner.poisoned = 1u;
		return X86_FIRMWARE_SHADOW_POISONED;
	}
	if (shadow_owner.client_count > 1u) {
		client->active = 0u;
		--shadow_owner.client_count;
		return X86_FIRMWARE_SHADOW_OK;
	}
	for (index = CONFIG_X86_GUEST_FIRMWARE_SHADOW_PAGE_CAPACITY;
	     index > 0u; --index) {
		struct x86_firmware_shadow_page *page =
			&shadow_owner.pages[index - 1u];
		enum x86_firmware_shadow_status status;

		if (page->state == (uint8_t)X86_FIRMWARE_PAGE_EMPTY)
			continue;
		status = release_private_page(page, &mapping_corrupted);
		if (status == X86_FIRMWARE_SHADOW_RETRY)
			return status;
		if (status != X86_FIRMWARE_SHADOW_OK) {
			shadow_owner.poisoned = 1u;
			return X86_FIRMWARE_SHADOW_POISONED;
		}
	}
	client->active = 0u;
	shadow_owner.client_count = 0u;
	if (shadow_owner.page_count != 0u || mapping_corrupted ||
	    shadow_owner.cleanup_corrupted != 0u) {
		shadow_owner.poisoned = 1u;
		return X86_FIRMWARE_SHADOW_POISONED;
	}
	return X86_FIRMWARE_SHADOW_OK;
}

enum x86_firmware_shadow_status x86_firmware_shadow_execution_quarantine(
	kernel_object_handle_t machine_identity,
	const struct x86_guest_space_firmware_binding *binding)
{
	struct x86_firmware_client_slot *client;
	size_t index;

	if (!identity_is_valid(machine_identity) || binding == NULL)
		return X86_FIRMWARE_SHADOW_INVALID_ARGUMENT;
	if (shadow_owner.initialized != 1u)
		return X86_FIRMWARE_SHADOW_INVALID_STATE;
	if (machine_identity != shadow_owner.machine_identity)
		return X86_FIRMWARE_SHADOW_MACHINE_MISMATCH;
	client = resolve_client(binding);
	if (client == NULL)
		return X86_FIRMWARE_SHADOW_STALE_BINDING;
	(void)client;
	/*
	 * Do not return backing whose zero-before-free contract has failed.  Strip
	 * every still-active low alias, then retain all remaining leases until
	 * reboot under a sticky poisoned owner.
	 */
	for (index = 0u;
	     index < CONFIG_X86_GUEST_FIRMWARE_SHADOW_PAGE_CAPACITY; ++index) {
		struct x86_firmware_shadow_page *page =
			&shadow_owner.pages[index];
		enum x86_paging_guest_shadow_status paging_status;

		if (page->state != (uint8_t)X86_FIRMWARE_PAGE_ACTIVE)
			continue;
		paging_status = x86_paging_guest_shadow_restore(
			&page->snapshot, page->physical_page);
		if (paging_status == X86_PAGING_GUEST_SHADOW_OK ||
		    paging_status == X86_PAGING_GUEST_SHADOW_MAPPING_MISMATCH)
			page->state = (uint8_t)X86_FIRMWARE_PAGE_RESTORED;
	}
	shadow_owner.poisoned = 1u;
	return X86_FIRMWARE_SHADOW_POISONED;
}

static enum x86_firmware_shadow_status memory_allocation_status(
	enum x86_guest_memory_status status)
{
	if (status == X86_GUEST_MEMORY_NO_MEMORY ||
	    status == X86_GUEST_MEMORY_NO_LEASE ||
	    status == X86_GUEST_MEMORY_ZERO_FAILED)
		return X86_FIRMWARE_SHADOW_NO_MEMORY;
	if (status == X86_GUEST_MEMORY_POISONED)
		return X86_FIRMWARE_SHADOW_POISONED;
	return X86_FIRMWARE_SHADOW_INVALID_STATE;
}

static bool copy_firmware_page(uint32_t source_page,
			       uint32_t destination_page)
{
	struct native_mapping destination;
	struct native_mapping source;

	if (!map_physical_page(source_page, false, &source) ||
	    !map_physical_page(destination_page, true, &destination) ||
	    source.length != X86_PAGE_BYTES ||
	    destination.length != X86_PAGE_BYTES)
		return false;
	return memcpy_s(destination.pointer, destination.length, source.pointer,
			source.length, X86_PAGE_BYTES) == MEMORY_OK;
}

enum x86_firmware_shadow_status x86_firmware_shadow_write_fault(
	kernel_object_handle_t machine_identity,
	const struct x86_guest_space_firmware_binding *binding,
	uint32_t page_fault_error, uint32_t fault_address)
{
	struct x86_guest_memory_lease_info lease_info;
	struct x86_firmware_client_slot *client;
	struct x86_firmware_shadow_page *page;
	enum x86_guest_memory_status memory_status;
	enum x86_paging_guest_shadow_status paging_status;
	enum x86_firmware_shadow_status status;
	x86_guest_memory_lease_t lease;
	uint32_t linear_page;
	uint32_t physical_page;

	if (!identity_is_valid(machine_identity) || binding == NULL)
		return X86_FIRMWARE_SHADOW_INVALID_ARGUMENT;
	if (!owner_is_active())
		return shadow_owner.poisoned != 0u
			       ? X86_FIRMWARE_SHADOW_POISONED
			       : X86_FIRMWARE_SHADOW_INVALID_STATE;
	if (machine_identity != shadow_owner.machine_identity)
		return X86_FIRMWARE_SHADOW_MACHINE_MISMATCH;
	client = resolve_client(binding);
	if (client == NULL || client->active != 1u)
		return X86_FIRMWARE_SHADOW_STALE_BINDING;
	if (page_fault_error != X86_FIRMWARE_WRITE_FAULT ||
	    fault_address < X86_DOS_VIDEO_LIMIT ||
	    fault_address >= X86_LEGACY_ROM_LIMIT)
		return X86_FIRMWARE_SHADOW_NOT_APPLICABLE;
	linear_page = fault_address & ~(X86_PAGE_BYTES - 1u);
	page = find_page(linear_page);
	if (page != NULL) {
		struct x86_paging_guest_translation translation;

		if (page->state == (uint8_t)X86_FIRMWARE_PAGE_ACTIVE &&
		    x86_paging_guest_shadow_translate(
			    &page->snapshot, page->physical_page, fault_address,
			    true, &translation))
			return X86_FIRMWARE_SHADOW_OK;
		if (page->state == (uint8_t)X86_FIRMWARE_PAGE_ACTIVE) {
			paging_status = x86_paging_guest_shadow_restore(
				&page->snapshot, page->physical_page);
			if (paging_status == X86_PAGING_GUEST_SHADOW_OK ||
			    paging_status ==
				    X86_PAGING_GUEST_SHADOW_MAPPING_MISMATCH)
				page->state =
					(uint8_t)X86_FIRMWARE_PAGE_RESTORED;
		}
		shadow_owner.poisoned = 1u;
		return X86_FIRMWARE_SHADOW_POISONED;
	}
	if (shadow_owner.page_count >=
	    CONFIG_X86_GUEST_FIRMWARE_SHADOW_PAGE_CAPACITY)
		return X86_FIRMWARE_SHADOW_CAPACITY_EXHAUSTED;
	page = find_empty_page();
	if (page == NULL) {
		shadow_owner.poisoned = 1u;
		return X86_FIRMWARE_SHADOW_POISONED;
	}
	memory_status = x86_guest_memory_runtime_allocate(
		shadow_owner.address_space_identity, 1u, &lease, &physical_page);
	if (memory_status != X86_GUEST_MEMORY_OK) {
		status = memory_allocation_status(memory_status);
		if (status == X86_FIRMWARE_SHADOW_POISONED)
			shadow_owner.poisoned = 1u;
		return status;
	}
	*page = (struct x86_firmware_shadow_page){
		.lease = lease,
		.linear_page = linear_page,
		.physical_page = physical_page,
		.state = (uint8_t)X86_FIRMWARE_PAGE_LEASED,
		.reserved = {0u},
	};
	++shadow_owner.page_count;
	memory_status = x86_guest_memory_runtime_inspect(lease, &lease_info);
	if (memory_status != X86_GUEST_MEMORY_OK ||
	    lease_info.owner != shadow_owner.address_space_identity ||
	    lease_info.physical_address != physical_page ||
	    lease_info.page_count != 1u)
		return rollback_private_page(
			page, X86_FIRMWARE_SHADOW_INVALID_STATE);
	paging_status = x86_paging_guest_shadow_snapshot(
		&shadow_owner.paging, linear_page, &page->snapshot);
	if (paging_status != X86_PAGING_GUEST_SHADOW_OK)
		return rollback_private_page(
			page, X86_FIRMWARE_SHADOW_PAGING_MISMATCH);
	if (!copy_firmware_page(linear_page, physical_page))
		return rollback_private_page(
			page, X86_FIRMWARE_SHADOW_INVALID_STATE);
	paging_status = x86_paging_guest_shadow_publish(
		&page->snapshot, physical_page);
	if (paging_status != X86_PAGING_GUEST_SHADOW_OK)
		return rollback_private_page(
			page, X86_FIRMWARE_SHADOW_PAGING_MISMATCH);
	/* No fallible work follows the PTE commit. */
	page->state = (uint8_t)X86_FIRMWARE_PAGE_ACTIVE;
	return X86_FIRMWARE_SHADOW_OK;
}

bool x86_firmware_shadow_translate(
	uint32_t address, bool writable,
	struct x86_paging_guest_translation *translation)
{
	struct x86_firmware_shadow_page *page;
	uint32_t linear_page;

	if (!owner_is_active() || translation == NULL ||
	    address >= shadow_owner.paging.guest_linear_limit)
		return false;
	linear_page = address & ~(X86_PAGE_BYTES - 1u);
	page = find_page(linear_page);
	if (page == NULL)
		return x86_paging_guest_identity_translate(
			&shadow_owner.paging, address, writable, translation);
	if (page->state != (uint8_t)X86_FIRMWARE_PAGE_ACTIVE)
		return false;
	return x86_paging_guest_shadow_translate(
		&page->snapshot, page->physical_page, address, writable,
		translation);
}

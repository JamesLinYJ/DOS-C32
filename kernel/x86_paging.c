// SPDX-License-Identifier: GPL-2.0-only
/*
 * Initial i386 page tables for the relocated DOS-C32 kernel.
 *
 * Low DOS/video/BIOS addresses are consumed by isolated legacy x86 guests;
 * the kernel, page tables, and stack at 4..8 MiB remain supervisor-only.
 * A later per-guest address-space manager will replace identity low mappings
 * with private frames while retaining this access policy.
 */
#include "x86_paging.h"

#include "compiler.h"
#include "x86_memory_map.h"

#define X86_PAGE_SHIFT 12u
#define X86_PAGE_TABLE_ENTRIES 1024u
#define X86_PAGE_TABLE_SPAN \
	(X86_PAGE_BYTES * X86_PAGE_TABLE_ENTRIES)
#define X86_BOOT_PAGE_TABLE_CAPACITY \
	(X86_BOOT_IDENTITY_LIMIT / (X86_PAGE_BYTES * X86_PAGE_TABLE_ENTRIES))

#define X86_PAGE_PRESENT (1u << 0)
#define X86_PAGE_WRITABLE (1u << 1)
#define X86_PAGE_USER (1u << 2)
#define X86_PAGE_ACCESSED (1u << 5)
#define X86_PAGE_DIRTY (1u << 6)
#define X86_PAGE_ADDRESS_MASK (~(X86_PAGE_BYTES - 1u))
#define X86_PAGE_HARDWARE_MUTABLE (X86_PAGE_ACCESSED | X86_PAGE_DIRTY)
#define X86_CR0_PAGING (1u << 31)
#define X86_BOOT_PAGING_GENERATION 1u

static uint32_t page_directory[X86_PAGE_TABLE_ENTRIES]
	__attribute__((aligned(X86_PAGE_BYTES)));
static uint32_t identity_page_tables[X86_BOOT_PAGE_TABLE_CAPACITY]
				    [X86_PAGE_TABLE_ENTRIES]
	__attribute__((aligned(X86_PAGE_BYTES)));
static bool paging_enabled;
static uint32_t paging_identity_limit;

#if defined(X86_PAGING_HOST_TEST)
static uint32_t host_active_page_directory;
static uint32_t host_tlb_flush_count;
#endif

static uint32_t active_page_directory(void)
{
#if defined(X86_PAGING_HOST_TEST)
	return host_active_page_directory;
#else
	uint32_t cr3;

	__asm__ volatile("movl %%cr3, %0" : "=r"(cr3));
	return cr3 & ~(X86_PAGE_BYTES - 1u);
#endif
}

static_assert_expression(X86_BOOT_IDENTITY_FLOOR >= X86_REAL_MODE_LINEAR_LIMIT,
			 "identity-map floor must cover the legacy guest window");
static_assert_expression((X86_BOOT_IDENTITY_FLOOR &
			  (X86_PAGE_BYTES - 1u)) == 0u,
			 "identity-map floor must be page aligned");
static_assert_expression(X86_BOOT_IDENTITY_LIMIT > X86_BOOT_IDENTITY_FLOOR,
			 "identity-map ceiling must exceed its floor");
static_assert_expression((X86_BOOT_IDENTITY_LIMIT %
			  X86_PAGE_TABLE_SPAN) == 0u,
			 "identity-map ceiling must be page-table aligned");
static_assert_expression(X86_BOOT_PAGE_TABLE_CAPACITY > 0u &&
			 X86_BOOT_PAGE_TABLE_CAPACITY <=
				 X86_PAGE_TABLE_ENTRIES,
			 "identity-map capacity must fit one page directory");
static_assert_expression(sizeof(page_directory) == X86_PAGE_BYTES,
			 "page directory must occupy one page");
static_assert_expression(sizeof(identity_page_tables[0]) == X86_PAGE_BYTES,
			 "each page table must occupy one page");
static_assert_expression((X86_LEGACY_VIDEO_BASE &
			  (X86_PAGE_BYTES - 1u)) == 0u,
			 "legacy video base must be page aligned");
static_assert_expression((X86_DOS_VIDEO_LIMIT &
			  (X86_PAGE_BYTES - 1u)) == 0u,
			 "legacy video limit must be page aligned");
static_assert_expression(X86_LEGACY_VIDEO_BASE < X86_DOS_VIDEO_LIMIT,
			 "legacy video aperture must not be empty");

static uint32_t access_flags(enum x86_page_access access)
{
	switch (access) {
	case X86_PAGE_GUEST_READ_WRITE:
		return X86_PAGE_WRITABLE | X86_PAGE_USER;
	case X86_PAGE_GUEST_READ_ONLY:
		return X86_PAGE_USER;
	case X86_PAGE_SUPERVISOR_READ_WRITE:
	default:
		return X86_PAGE_WRITABLE;
	}
}

static void flush_translation_lookaside_buffer(void)
{
#if defined(X86_PAGING_HOST_TEST)
	++host_tlb_flush_count;
#else
	uint32_t cr3 = active_page_directory();

	__asm__ volatile("movl %0, %%cr3" : : "r"(cr3) : "memory");
#endif
}

static uint32_t detected_table_count(const struct x86_boot_info *boot_info)
{
	uint64_t detected_limit;
	uint64_t mapped_limit = X86_BOOT_IDENTITY_FLOOR;

	if (x86_memory_map_usable_limit(boot_info, &detected_limit) &&
	    detected_limit > mapped_limit)
		mapped_limit = detected_limit;
	if (mapped_limit > X86_BOOT_IDENTITY_LIMIT)
		mapped_limit = X86_BOOT_IDENTITY_LIMIT;
	mapped_limit = (mapped_limit + X86_PAGE_TABLE_SPAN - 1u) &
		       ~((uint64_t)X86_PAGE_TABLE_SPAN - 1u);
	if (mapped_limit > X86_BOOT_IDENTITY_LIMIT)
		mapped_limit = X86_BOOT_IDENTITY_LIMIT;
	return (uint32_t)(mapped_limit / X86_PAGE_TABLE_SPAN);
}

void x86_paging_initialize(const struct x86_boot_info *boot_info)
{
	uint32_t table_index;
	uint32_t table_count;
	uint32_t page_index;
#if !defined(X86_PAGING_HOST_TEST)
	uint32_t cr0;
#endif

	table_count = detected_table_count(boot_info);
	for (page_index = 0u; page_index < X86_PAGE_TABLE_ENTRIES; ++page_index)
		page_directory[page_index] = 0u;
	for (table_index = 0u; table_index < table_count;
	     ++table_index) {
		uint32_t directory_flags = X86_PAGE_PRESENT | X86_PAGE_WRITABLE;

		/* Only the first 4 MiB contains pages visible to legacy guests. */
		if (table_index == 0u)
			directory_flags |= X86_PAGE_USER;
		page_directory[table_index] =
			(uint32_t)(uintptr_t)&identity_page_tables[table_index]
								  [0] |
			directory_flags;
		for (page_index = 0u; page_index < X86_PAGE_TABLE_ENTRIES;
		     ++page_index) {
			uint32_t linear =
				(table_index * X86_PAGE_TABLE_ENTRIES +
				 page_index)
				<< X86_PAGE_SHIFT;

			identity_page_tables[table_index][page_index] =
				x86_boot_page_is_present(boot_info, linear)
					? linear | X86_PAGE_PRESENT |
						access_flags(
							x86_boot_page_access(linear))
					: 0u;
		}
	}

#if defined(X86_PAGING_HOST_TEST)
	host_active_page_directory =
		(uint32_t)(uintptr_t)&page_directory[0];
	host_tlb_flush_count = 0u;
#else
	__asm__ volatile("movl %0, %%cr3"
			 :
			 : "r"((uint32_t)(uintptr_t)&page_directory[0])
			 : "memory");
	__asm__ volatile("movl %%cr0, %0" : "=r"(cr0));
	cr0 |= X86_CR0_PAGING;
	__asm__ volatile("movl %0, %%cr0" : : "r"(cr0) : "memory");
	__asm__ volatile("jmp 1f\n1:" : : : "memory");
#endif
	paging_identity_limit = table_count * X86_PAGE_TABLE_SPAN;
	paging_enabled = true;
}

bool x86_paging_is_enabled(void)
{
	return paging_enabled;
}

uint32_t x86_paging_identity_limit(void)
{
	return paging_enabled ? paging_identity_limit : 0u;
}

bool x86_paging_snapshot(struct x86_paging_binding *binding)
{
	struct x86_paging_binding prepared;
	uint32_t expected = (uint32_t)(uintptr_t)&page_directory[0];

	if (binding == NULL || !paging_enabled ||
	    active_page_directory() != expected)
		return false;
	prepared = (struct x86_paging_binding){
		.generation = X86_BOOT_PAGING_GENERATION,
		.page_directory = expected,
		.guest_linear_limit = X86_REAL_MODE_LINEAR_LIMIT,
	};
	*binding = prepared;
	return true;
}

bool x86_paging_binding_is_active(const struct x86_paging_binding *binding)
{
	return binding != NULL && paging_enabled &&
	       binding->generation == X86_BOOT_PAGING_GENERATION &&
	       binding->page_directory ==
		       (uint32_t)(uintptr_t)&page_directory[0] &&
	       binding->guest_linear_limit == X86_REAL_MODE_LINEAR_LIMIT &&
	       active_page_directory() == binding->page_directory;
}

static bool active_boot_page_directory(void)
{
	return paging_enabled &&
	       paging_identity_limit >= X86_DOS_VIDEO_LIMIT &&
	       active_page_directory() ==
		       (uint32_t)(uintptr_t)&page_directory[0];
}

static uint32_t *boot_page_entry(uint32_t linear_address)
{
	uint32_t table;
	uint32_t page;

	if (!active_boot_page_directory() ||
	    linear_address >= paging_identity_limit)
		return NULL;
	table = linear_address / X86_PAGE_TABLE_SPAN;
	page = (linear_address >> X86_PAGE_SHIFT) &
	       (X86_PAGE_TABLE_ENTRIES - 1u);
	if (table >= X86_BOOT_PAGE_TABLE_CAPACITY ||
	    (page_directory[table] & X86_PAGE_PRESENT) == 0u)
		return NULL;
	return &identity_page_tables[table][page];
}

static bool guest_directory_is_user_accessible(uint32_t linear_address)
{
	uint32_t table = linear_address / X86_PAGE_TABLE_SPAN;

	return table < X86_BOOT_PAGE_TABLE_CAPACITY &&
	       (page_directory[table] & (X86_PAGE_PRESENT | X86_PAGE_USER)) ==
		       (X86_PAGE_PRESENT | X86_PAGE_USER);
}

static bool entries_match_ignoring_cpu_state(uint32_t left, uint32_t right)
{
	return (left & ~X86_PAGE_HARDWARE_MUTABLE) ==
	       (right & ~X86_PAGE_HARDWARE_MUTABLE);
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

static bool private_shadow_target_is_safe(uint32_t physical_page)
{
	uint32_t *entry;

	if ((physical_page & (X86_PAGE_BYTES - 1u)) != 0u ||
	    physical_page < X86_BOOT_IDENTITY_FLOOR ||
	    physical_page >= paging_identity_limit)
		return false;
	entry = boot_page_entry(physical_page);
	return entry != NULL &&
	       (*entry & X86_PAGE_ADDRESS_MASK) == physical_page &&
	       (*entry & (X86_PAGE_PRESENT | X86_PAGE_WRITABLE)) ==
		       (X86_PAGE_PRESENT | X86_PAGE_WRITABLE) &&
	       (*entry & X86_PAGE_USER) == 0u;
}

static bool guest_shadow_snapshot_is_valid(
	const struct x86_paging_guest_shadow_snapshot *snapshot)
{
	return snapshot != NULL &&
	       x86_paging_binding_is_active(&snapshot->paging) &&
	       snapshot->linear_page >= X86_DOS_VIDEO_LIMIT &&
	       snapshot->linear_page < X86_LEGACY_ROM_LIMIT &&
	       (snapshot->linear_page & (X86_PAGE_BYTES - 1u)) == 0u &&
	       (snapshot->original_entry & X86_PAGE_ADDRESS_MASK) ==
		       snapshot->linear_page &&
	       (snapshot->original_entry & (X86_PAGE_PRESENT | X86_PAGE_USER |
					    X86_PAGE_WRITABLE)) ==
		       (X86_PAGE_PRESENT | X86_PAGE_USER) &&
	       bytes_are_zero(snapshot->reserved,
			      ARRAY_SIZE(snapshot->reserved));
}

static uint32_t guest_shadow_entry(
	const struct x86_paging_guest_shadow_snapshot *snapshot,
	uint32_t private_physical_page)
{
	return private_physical_page |
	       (snapshot->original_entry & ~X86_PAGE_ADDRESS_MASK) |
	       X86_PAGE_WRITABLE;
}

bool x86_paging_guest_identity_translate(
	const struct x86_paging_binding *binding, uint32_t address,
	bool writable, struct x86_paging_guest_translation *translation)
{
	struct x86_paging_guest_translation prepared;
	uint32_t required = X86_PAGE_PRESENT | X86_PAGE_USER;
	uint32_t page = address & X86_PAGE_ADDRESS_MASK;
	uint32_t *entry;

	if (translation == NULL ||
	    !x86_paging_binding_is_active(binding) ||
	    address >= binding->guest_linear_limit ||
	    !guest_directory_is_user_accessible(address))
		return false;
	entry = boot_page_entry(page);
	if (entry == NULL || (*entry & X86_PAGE_ADDRESS_MASK) != page)
		return false;
	if (writable)
		required |= X86_PAGE_WRITABLE;
	if ((*entry & required) != required)
		return false;
	prepared = (struct x86_paging_guest_translation){
		.physical_address = address,
		.contiguous_bytes = X86_PAGE_BYTES -
				    (address & (X86_PAGE_BYTES - 1u)),
	};
	*translation = prepared;
	return true;
}

enum x86_paging_guest_shadow_status x86_paging_guest_shadow_snapshot(
	const struct x86_paging_binding *binding, uint32_t linear_page,
	struct x86_paging_guest_shadow_snapshot *snapshot)
{
	struct x86_paging_guest_shadow_snapshot prepared;
	uint32_t *entry;

	if (binding == NULL || snapshot == NULL ||
	    (linear_page & (X86_PAGE_BYTES - 1u)) != 0u ||
	    linear_page < X86_DOS_VIDEO_LIMIT ||
	    linear_page >= X86_LEGACY_ROM_LIMIT)
		return X86_PAGING_GUEST_SHADOW_INVALID_ARGUMENT;
	if (!x86_paging_binding_is_active(binding))
		return X86_PAGING_GUEST_SHADOW_STALE_BINDING;
	if (!guest_directory_is_user_accessible(linear_page))
		return X86_PAGING_GUEST_SHADOW_SOURCE_MISMATCH;
	entry = boot_page_entry(linear_page);
	if (entry == NULL || (*entry & X86_PAGE_ADDRESS_MASK) != linear_page ||
	    (*entry & (X86_PAGE_PRESENT | X86_PAGE_USER |
		       X86_PAGE_WRITABLE)) !=
		    (X86_PAGE_PRESENT | X86_PAGE_USER))
		return X86_PAGING_GUEST_SHADOW_SOURCE_MISMATCH;
	prepared = (struct x86_paging_guest_shadow_snapshot){
		.paging = *binding,
		.linear_page = linear_page,
		.original_entry = *entry,
		.reserved = {0u},
	};
	*snapshot = prepared;
	return X86_PAGING_GUEST_SHADOW_OK;
}

enum x86_paging_guest_shadow_status x86_paging_guest_shadow_publish(
	const struct x86_paging_guest_shadow_snapshot *snapshot,
	uint32_t private_physical_page)
{
	uint32_t *entry;

	if (snapshot == NULL ||
	    (private_physical_page & (X86_PAGE_BYTES - 1u)) != 0u)
		return X86_PAGING_GUEST_SHADOW_INVALID_ARGUMENT;
	if (!guest_shadow_snapshot_is_valid(snapshot))
		return X86_PAGING_GUEST_SHADOW_STALE_BINDING;
	if (!private_shadow_target_is_safe(private_physical_page))
		return X86_PAGING_GUEST_SHADOW_TARGET_DENIED;
	if (!guest_directory_is_user_accessible(snapshot->linear_page))
		return X86_PAGING_GUEST_SHADOW_SOURCE_MISMATCH;
	entry = boot_page_entry(snapshot->linear_page);
	if (entry == NULL ||
	    !entries_match_ignoring_cpu_state(*entry,
					      snapshot->original_entry))
		return X86_PAGING_GUEST_SHADOW_SOURCE_MISMATCH;
	*entry = guest_shadow_entry(snapshot, private_physical_page);
	flush_translation_lookaside_buffer();
	return X86_PAGING_GUEST_SHADOW_OK;
}

enum x86_paging_guest_shadow_status x86_paging_guest_shadow_restore(
	const struct x86_paging_guest_shadow_snapshot *snapshot,
	uint32_t private_physical_page)
{
	bool directory_valid;
	uint32_t expected;
	uint32_t *entry;

	if (snapshot == NULL ||
	    (private_physical_page & (X86_PAGE_BYTES - 1u)) != 0u)
		return X86_PAGING_GUEST_SHADOW_INVALID_ARGUMENT;
	if (!guest_shadow_snapshot_is_valid(snapshot))
		return X86_PAGING_GUEST_SHADOW_STALE_BINDING;
	entry = boot_page_entry(snapshot->linear_page);
	if (entry == NULL)
		return X86_PAGING_GUEST_SHADOW_STALE_BINDING;
	directory_valid = guest_directory_is_user_accessible(
		snapshot->linear_page);
	expected = guest_shadow_entry(snapshot, private_physical_page);
	if (entries_match_ignoring_cpu_state(*entry,
					     snapshot->original_entry))
		return directory_valid
			       ? X86_PAGING_GUEST_SHADOW_OK
			       : X86_PAGING_GUEST_SHADOW_MAPPING_MISMATCH;
	if (entries_match_ignoring_cpu_state(*entry, expected)) {
		*entry = snapshot->original_entry;
		flush_translation_lookaside_buffer();
		return directory_valid
			       ? X86_PAGING_GUEST_SHADOW_OK
			       : X86_PAGING_GUEST_SHADOW_MAPPING_MISMATCH;
	}
	/* A valid snapshot always permits fail-closed restoration. */
	*entry = snapshot->original_entry;
	flush_translation_lookaside_buffer();
	return X86_PAGING_GUEST_SHADOW_MAPPING_MISMATCH;
}

bool x86_paging_guest_shadow_translate(
	const struct x86_paging_guest_shadow_snapshot *snapshot,
	uint32_t private_physical_page, uint32_t address, bool writable,
	struct x86_paging_guest_translation *translation)
{
	struct x86_paging_guest_translation prepared;
	uint32_t expected;
	uint32_t offset;
	uint32_t *entry;

	if (translation == NULL ||
	    !guest_shadow_snapshot_is_valid(snapshot) ||
	    !private_shadow_target_is_safe(private_physical_page) ||
	    !guest_directory_is_user_accessible(snapshot->linear_page) ||
	    address < snapshot->linear_page ||
	    address >= snapshot->linear_page + X86_PAGE_BYTES)
		return false;
	entry = boot_page_entry(snapshot->linear_page);
	expected = guest_shadow_entry(snapshot, private_physical_page);
	if (entry == NULL ||
	    !entries_match_ignoring_cpu_state(*entry, expected) ||
	    (*entry & (X86_PAGE_PRESENT | X86_PAGE_USER)) !=
		    (X86_PAGE_PRESENT | X86_PAGE_USER) ||
	    (writable && (*entry & X86_PAGE_WRITABLE) == 0u))
		return false;
	offset = address - snapshot->linear_page;
	prepared = (struct x86_paging_guest_translation){
		.physical_address = private_physical_page + offset,
		.contiguous_bytes = X86_PAGE_BYTES - offset,
	};
	*translation = prepared;
	return true;
}

bool x86_paging_guest_range_is_accessible(uint32_t address, size_t count,
					  bool writable)
{
	uint32_t end;
	uint32_t page;
	uint32_t last_page;
	uint32_t required = X86_PAGE_PRESENT | X86_PAGE_USER;

	if (!active_boot_page_directory() ||
	    address > X86_REAL_MODE_LINEAR_LIMIT ||
	    count > (size_t)(X86_REAL_MODE_LINEAR_LIMIT - address))
		return false;
	if (count == 0u)
		return true;
	end = address + (uint32_t)count;
	if (end <= address ||
	    (page_directory[0] & (X86_PAGE_PRESENT | X86_PAGE_USER)) !=
		    (X86_PAGE_PRESENT | X86_PAGE_USER))
		return false;
	if (writable)
		required |= X86_PAGE_WRITABLE;
	page = address >> X86_PAGE_SHIFT;
	last_page = (end - 1u) >> X86_PAGE_SHIFT;
	for (; page <= last_page; ++page) {
		uint32_t linear = page << X86_PAGE_SHIFT;
		uint32_t entry = identity_page_tables[0][page];

		if ((entry & X86_PAGE_ADDRESS_MASK) != linear ||
		    (entry & required) != required)
			return false;
	}
	return true;
}

static bool legacy_video_range(uint32_t address, size_t count,
			       uint32_t *limit)
{
	if (limit == NULL || count == 0u ||
	    (address & (X86_PAGE_BYTES - 1u)) != 0u ||
	    (count & (X86_PAGE_BYTES - 1u)) != 0u ||
	    address < X86_LEGACY_VIDEO_BASE || address >= X86_DOS_VIDEO_LIMIT ||
	    count > (size_t)(X86_DOS_VIDEO_LIMIT - address))
		return false;
	*limit = address + (uint32_t)count;
	return *limit > address;
}

static bool legacy_video_identity_mappings_are_valid(
	uint32_t address, size_t count, bool require_user, bool forbid_user)
{
	uint32_t limit;
	uint32_t linear;

	if (!legacy_video_range(address, count, &limit))
		return false;
	for (linear = address; linear < limit; linear += X86_PAGE_BYTES) {
		uint32_t entry =
			identity_page_tables[0][linear >> X86_PAGE_SHIFT];

		if ((entry & X86_PAGE_ADDRESS_MASK) != linear ||
		    (entry & (X86_PAGE_PRESENT | X86_PAGE_WRITABLE)) !=
			    (X86_PAGE_PRESENT | X86_PAGE_WRITABLE) ||
		    (require_user && (entry & X86_PAGE_USER) == 0u) ||
		    (forbid_user && (entry & X86_PAGE_USER) != 0u))
			return false;
	}
	return true;
}

static bool force_legacy_video_supervisor(uint32_t address, size_t count)
{
	bool active = active_boot_page_directory();
	bool changed = false;
	bool mappings_valid = true;
	uint32_t limit;
	uint32_t linear;

	if (!legacy_video_range(address, count, &limit))
		return false;
	for (linear = address; linear < limit; linear += X86_PAGE_BYTES) {
		uint32_t *entry =
			&identity_page_tables[0][linear >> X86_PAGE_SHIFT];

		if ((*entry & X86_PAGE_ADDRESS_MASK) != linear ||
		    (*entry & (X86_PAGE_PRESENT | X86_PAGE_WRITABLE)) !=
			    (X86_PAGE_PRESENT | X86_PAGE_WRITABLE))
			mappings_valid = false;
		if ((*entry & X86_PAGE_USER) != 0u) {
			*entry &= ~X86_PAGE_USER;
			changed = true;
		}
	}
	if (changed && active)
		flush_translation_lookaside_buffer();
	return active && mappings_valid;
}

bool x86_paging_grant_legacy_video_range(uint32_t address, size_t count)
{
	uint32_t limit;
	uint32_t linear;

	if (!active_boot_page_directory() ||
	    !legacy_video_identity_mappings_are_valid(address, count, false,
						 true) ||
	    !legacy_video_range(address, count, &limit)) {
		(void)force_legacy_video_supervisor(address, count);
		return false;
	}
	for (linear = address; linear < limit; linear += X86_PAGE_BYTES)
		identity_page_tables[0][linear >> X86_PAGE_SHIFT] |=
			X86_PAGE_USER;
	flush_translation_lookaside_buffer();
	return true;
}

bool x86_paging_revoke_legacy_video_range(uint32_t address, size_t count)
{
	return force_legacy_video_supervisor(address, count);
}

bool x86_paging_legacy_video_range_is_user_accessible(uint32_t address,
						       size_t count)
{
	return active_boot_page_directory() &&
	       legacy_video_identity_mappings_are_valid(address, count, true,
						   false);
}

static bool protected_user_range(uint32_t address, size_t count,
				 uint32_t *first_page, uint32_t *page_limit)
{
	uint32_t end;

	if (first_page == NULL || page_limit == NULL || !paging_enabled ||
	    count == 0u ||
	    address < X86_PROTECTED_USER_BASE ||
	    address >= X86_PROTECTED_USER_STACK_TOP ||
	    count > (size_t)(X86_PROTECTED_USER_STACK_TOP - address))
		return false;
	end = address + (uint32_t)count;
	if (end > X86_PROTECTED_USER_STACK_TOP || end < address)
		return false;
	*first_page = address >> X86_PAGE_SHIFT;
	end = (end + X86_PAGE_BYTES - 1u) & ~(X86_PAGE_BYTES - 1u);
	*page_limit = end >> X86_PAGE_SHIFT;
	return true;
}

bool x86_paging_supervisor_range_is_writable(uint32_t address, size_t count)
{
	uint32_t first_page;
	uint32_t page_limit;
	uint32_t page;

	if (!protected_user_range(address, count, &first_page, &page_limit) ||
	    !active_boot_page_directory())
		return false;
	for (page = first_page; page < page_limit; ++page) {
		uint32_t entry = identity_page_tables[0][page];

		if ((entry & (X86_PAGE_PRESENT | X86_PAGE_WRITABLE)) !=
			    (X86_PAGE_PRESENT | X86_PAGE_WRITABLE) ||
		    (entry & X86_PAGE_USER) != 0u)
			return false;
	}
	return true;
}

bool x86_paging_grant_user_range(uint32_t address, size_t count)
{
	uint32_t first_page;
	uint32_t page_limit;
	uint32_t page;

	if (!protected_user_range(address, count, &first_page, &page_limit) ||
	    !active_boot_page_directory())
		return false;
	for (page = first_page; page < page_limit; ++page) {
		if ((identity_page_tables[0][page] &
		     (X86_PAGE_PRESENT | X86_PAGE_WRITABLE)) !=
		    (X86_PAGE_PRESENT | X86_PAGE_WRITABLE))
			return false;
	}
	for (page = first_page; page < page_limit; ++page)
		identity_page_tables[0][page] |=
			X86_PAGE_USER | X86_PAGE_WRITABLE;
	flush_translation_lookaside_buffer();
	return true;
}

bool x86_paging_revoke_user_range(uint32_t address, size_t count)
{
	uint32_t first_page;
	uint32_t page_limit;
	uint32_t page;
	bool valid = true;
	bool changed = false;

	if (!protected_user_range(address, count, &first_page, &page_limit) ||
	    !active_boot_page_directory())
		return false;
	for (page = first_page; page < page_limit; ++page) {
		uint32_t *entry = &identity_page_tables[0][page];

		if ((*entry & (X86_PAGE_PRESENT | X86_PAGE_WRITABLE)) !=
		    (X86_PAGE_PRESENT | X86_PAGE_WRITABLE))
			valid = false;
		if ((*entry & X86_PAGE_USER) != 0u) {
			*entry &= ~X86_PAGE_USER;
			changed = true;
		}
	}
	if (changed)
		flush_translation_lookaside_buffer();
	return valid;
}

bool x86_paging_user_range_is_accessible(uint32_t address, size_t count)
{
	uint32_t end;
	uint32_t page;
	uint32_t last_page;

	if (!paging_enabled || address < X86_PROTECTED_USER_BASE ||
	    count > (size_t)(X86_PROTECTED_USER_STACK_TOP - address))
		return false;
	if (count == 0u)
		return address <= X86_PROTECTED_USER_STACK_TOP;
	end = address + (uint32_t)count;
	if (end <= address || end > X86_PROTECTED_USER_STACK_TOP)
		return false;
	page = address >> X86_PAGE_SHIFT;
	last_page = (end - 1u) >> X86_PAGE_SHIFT;
	for (; page <= last_page; ++page) {
		uint32_t entry = identity_page_tables[0][page];

		if ((entry & (X86_PAGE_PRESENT | X86_PAGE_USER)) !=
		    (X86_PAGE_PRESENT | X86_PAGE_USER))
			return false;
	}
	return true;
}

#if defined(X86_PAGING_HOST_TEST)
uint32_t x86_paging_test_page_entry(uint32_t linear_address)
{
	uint32_t table = linear_address / X86_PAGE_TABLE_SPAN;
	uint32_t page = (linear_address >> X86_PAGE_SHIFT) &
			(X86_PAGE_TABLE_ENTRIES - 1u);

	if (table >= X86_BOOT_PAGE_TABLE_CAPACITY)
		return 0u;
	return identity_page_tables[table][page];
}

bool x86_paging_test_replace_page_entry(uint32_t linear_address,
					uint32_t entry)
{
	uint32_t table = linear_address / X86_PAGE_TABLE_SPAN;
	uint32_t page = (linear_address >> X86_PAGE_SHIFT) &
			(X86_PAGE_TABLE_ENTRIES - 1u);

	if ((linear_address & (X86_PAGE_BYTES - 1u)) != 0u ||
	    table >= X86_BOOT_PAGE_TABLE_CAPACITY)
		return false;
	identity_page_tables[table][page] = entry;
	return true;
}

uint32_t x86_paging_test_tlb_flush_count(void)
{
	return host_tlb_flush_count;
}
#endif

// SPDX-License-Identifier: GPL-2.0-only
/* Host-safe tests for the legacy-x86-guest-facing early paging policy. */
#include "dos_xms.h"
#include "x86_paging.h"
#include "x86_boot_info.h"
#include "test_entry.h"

#define TEST_PAGE_PRESENT (1u << 0)
#define TEST_PAGE_WRITABLE (1u << 1)
#define TEST_PAGE_USER (1u << 2)

static struct x86_boot_info test_map(void)
{
	struct x86_boot_info info = {0};

	info.signature = X86_BOOT_INFO_SIGNATURE;
	info.version = X86_BOOT_INFO_VERSION;
	info.header_bytes = X86_BOOT_INFO_HEADER_BYTES;
	info.range_bytes = X86_BOOT_MEMORY_RANGE_BYTES;
	info.range_count = 4u;
	info.flags = X86_BOOT_INFO_FLAG_MASK;
	info.ranges[0] = (struct x86_boot_memory_range){
		.base = X86_LEGACY_ROM_LIMIT,
		.length = 0x00500000u,
		.type = X86_BOOT_MEMORY_USABLE,
		.attributes = X86_BOOT_MEMORY_ENABLED,
	};
	info.ranges[1] = (struct x86_boot_memory_range){
		.base = 0x00300000u,
		.length = X86_PAGE_BYTES,
		.type = 2u,
		.attributes = X86_BOOT_MEMORY_ENABLED,
	};
	info.ranges[2] = (struct x86_boot_memory_range){
		.base = 0x00700000u,
		.length = X86_PAGE_BYTES,
		.type = X86_BOOT_MEMORY_USABLE,
		.attributes = X86_BOOT_MEMORY_ENABLED,
	};
	info.ranges[3] = (struct x86_boot_memory_range){
		.base = X86_BOOT_IDENTITY_FLOOR,
		.length = X86_PAGE_BYTES,
		.type = X86_BOOT_MEMORY_USABLE,
		.attributes = X86_BOOT_MEMORY_ENABLED,
	};
	return info;
}

static bool video_user_bits_match(uint32_t base, uint32_t bytes,
				  bool accessible)
{
	uint32_t linear;

	for (linear = base; linear < base + bytes; linear += X86_PAGE_BYTES) {
		uint32_t entry = x86_paging_test_page_entry(linear);

		if ((entry & (TEST_PAGE_PRESENT | TEST_PAGE_WRITABLE)) !=
		    (TEST_PAGE_PRESENT | TEST_PAGE_WRITABLE) ||
		    ((entry & TEST_PAGE_USER) != 0u) != accessible)
			return false;
	}
	return true;
}

static int run_tests(void)
{
	struct x86_boot_info info = test_map();
	struct x86_boot_info missing_hma = test_map();
	uint32_t damaged_entry;
	uint32_t first_entry;
	uint32_t flushes;
	uint32_t hma_entry;
	uint32_t rom_entry;
	uint32_t user_entry;
	uint32_t user_second_entry;
	struct x86_paging_binding paging;
	struct x86_paging_guest_shadow_snapshot shadow;
	struct x86_paging_guest_translation translation;
	uint32_t private_entry;

	if (x86_boot_page_access(0u) != X86_PAGE_GUEST_READ_WRITE ||
	    x86_boot_page_access(X86_DOS_CONVENTIONAL_LIMIT - 1u) !=
		    X86_PAGE_GUEST_READ_WRITE)
		return 1;
	if (x86_boot_page_access(X86_LEGACY_VIDEO_BASE) !=
		    X86_PAGE_SUPERVISOR_READ_WRITE ||
	    x86_boot_page_access(X86_DOS_VIDEO_LIMIT - 1u) !=
		    X86_PAGE_SUPERVISOR_READ_WRITE)
		return 2;
	/* Real firmware remains readable/executable; guest writes need a separate
	 * shadow capability and are never inferred from the architectural range. */
	if (x86_boot_page_access(X86_DOS_VIDEO_LIMIT) !=
		    X86_PAGE_GUEST_READ_ONLY ||
	    x86_boot_page_access(X86_LEGACY_ROM_LIMIT - 1u) !=
		    X86_PAGE_GUEST_READ_ONLY)
		return 3;
	if (x86_boot_page_access(X86_LEGACY_ROM_LIMIT) !=
		    X86_PAGE_GUEST_READ_WRITE ||
	    x86_boot_page_access(X86_REAL_MODE_LINEAR_LIMIT - 1u) !=
		    X86_PAGE_GUEST_READ_WRITE ||
	    x86_boot_page_access(X86_REAL_MODE_LINEAR_LIMIT) !=
		    X86_PAGE_SUPERVISOR_READ_WRITE ||
	    x86_boot_page_access(X86_BOOT_IDENTITY_LIMIT - 1u) !=
		    X86_PAGE_SUPERVISOR_READ_WRITE)
		return 4;
	missing_hma.ranges[0].base = X86_REAL_MODE_LINEAR_LIMIT;
	missing_hma.ranges[0].length = 0x004f0000u;
	if (!x86_boot_page_is_present(&info, 0u) ||
	    !x86_boot_page_is_present(&info, X86_LEGACY_ROM_LIMIT) ||
	    x86_boot_page_is_present(&missing_hma,
				     X86_LEGACY_ROM_LIMIT) ||
	    !x86_boot_page_is_present(&info, 0x00200000u) ||
	    x86_boot_page_is_present(&info, 0x00300000u) ||
	    !x86_boot_page_is_present(&info, 0x00500000u) ||
	    x86_boot_page_is_present(&info, 0x00600000u) ||
	    !x86_boot_page_is_present(&info, 0x00700000u) ||
	    x86_boot_page_is_present(&info, X86_BOOT_IDENTITY_LIMIT))
		return 5;

	x86_paging_initialize(&info);
	if (!x86_paging_guest_range_is_accessible(
		    X86_DOS_VIDEO_LIMIT, X86_PAGE_BYTES, false) ||
	    x86_paging_guest_range_is_accessible(
		    X86_DOS_VIDEO_LIMIT, X86_PAGE_BYTES, true))
		return 16;
	hma_entry = x86_paging_test_page_entry((uint32_t)DOS_XMS_HMA_BASE);
	if (!x86_paging_guest_range_is_accessible(
		    (uint32_t)DOS_XMS_HMA_BASE, (size_t)DOS_XMS_HMA_BYTES,
		    true) ||
	    x86_paging_guest_range_is_accessible(
		    (uint32_t)DOS_XMS_HMA_BASE,
		    (size_t)DOS_XMS_HMA_BYTES + 17u, true) ||
	    !x86_paging_test_replace_page_entry(
		    (uint32_t)DOS_XMS_HMA_BASE,
		    hma_entry & ~TEST_PAGE_WRITABLE) ||
	    x86_paging_guest_range_is_accessible(
		    (uint32_t)DOS_XMS_HMA_BASE, (size_t)DOS_XMS_HMA_BYTES,
		    true) ||
	    !x86_paging_guest_range_is_accessible(
		    (uint32_t)DOS_XMS_HMA_BASE, (size_t)DOS_XMS_HMA_BYTES,
		    false) ||
	    !x86_paging_test_replace_page_entry(
		    (uint32_t)DOS_XMS_HMA_BASE, hma_entry))
		return 6;
	rom_entry = x86_paging_test_page_entry(X86_DOS_VIDEO_LIMIT);
	if (x86_paging_legacy_video_range_is_user_accessible(
		    X86_LEGACY_VIDEO_BASE, X86_LEGACY_VIDEO_BYTES) ||
	    !video_user_bits_match(X86_LEGACY_VIDEO_BASE,
				   X86_LEGACY_VIDEO_BYTES, false) ||
	    (rom_entry & (TEST_PAGE_PRESENT | TEST_PAGE_WRITABLE |
			  TEST_PAGE_USER)) !=
		    (TEST_PAGE_PRESENT | TEST_PAGE_USER) ||
	    x86_paging_test_tlb_flush_count() != 0u)
		return 7;
	if (!x86_paging_grant_legacy_video_range(0x000b8000u, 0x00008000u) ||
	    !x86_paging_legacy_video_range_is_user_accessible(
		    0x000b8000u, 0x00008000u) ||
	    x86_paging_legacy_video_range_is_user_accessible(
		    X86_LEGACY_VIDEO_BASE, 0x00018000u) ||
	    !video_user_bits_match(0x000b8000u, 0x00008000u, true) ||
	    !video_user_bits_match(X86_LEGACY_VIDEO_BASE, 0x00018000u, false) ||
	    x86_paging_test_page_entry(X86_DOS_VIDEO_LIMIT) != rom_entry ||
	    x86_paging_test_tlb_flush_count() != 1u)
		return 8;
	if (!x86_paging_revoke_legacy_video_range(0x000b8000u, 0x00008000u) ||
	    x86_paging_legacy_video_range_is_user_accessible(
		    0x000b8000u, 0x00008000u) ||
	    !video_user_bits_match(X86_LEGACY_VIDEO_BASE,
				   X86_LEGACY_VIDEO_BYTES, false) ||
	    x86_paging_test_page_entry(X86_DOS_VIDEO_LIMIT) != rom_entry ||
	    x86_paging_test_tlb_flush_count() != 2u)
		return 9;
	if (!x86_paging_revoke_legacy_video_range(0x000b8000u,
						 0x00008000u) ||
	    x86_paging_test_tlb_flush_count() != 2u)
		return 10;
	flushes = x86_paging_test_tlb_flush_count();
	if (x86_paging_grant_legacy_video_range(0x000b8001u, 0x00008000u) ||
	    x86_paging_grant_legacy_video_range(0x000b8000u, 0x00007fffu) ||
	    x86_paging_grant_legacy_video_range(0x0009f000u, X86_PAGE_BYTES) ||
	    x86_paging_revoke_legacy_video_range(0x000c0000u,
					       X86_PAGE_BYTES) ||
	    x86_paging_legacy_video_range_is_user_accessible(
		    0x000b8001u, 0x00008000u) ||
	    x86_paging_test_tlb_flush_count() != flushes)
		return 17;

	first_entry = x86_paging_test_page_entry(X86_LEGACY_VIDEO_BASE);
	damaged_entry = x86_paging_test_page_entry(
		X86_LEGACY_VIDEO_BASE + X86_PAGE_BYTES);
	flushes = x86_paging_test_tlb_flush_count();
	if (!x86_paging_test_replace_page_entry(X86_LEGACY_VIDEO_BASE,
						first_entry | TEST_PAGE_USER) ||
	    !x86_paging_test_replace_page_entry(
		    X86_LEGACY_VIDEO_BASE + X86_PAGE_BYTES,
		    damaged_entry & ~TEST_PAGE_PRESENT))
		return 11;
	if (x86_paging_grant_legacy_video_range(X86_LEGACY_VIDEO_BASE,
						X86_LEGACY_VIDEO_BYTES) ||
	    x86_paging_legacy_video_range_is_user_accessible(
		    X86_LEGACY_VIDEO_BASE, X86_LEGACY_VIDEO_BYTES) ||
	    (x86_paging_test_page_entry(X86_LEGACY_VIDEO_BASE) &
	     TEST_PAGE_USER) != 0u ||
	    x86_paging_test_tlb_flush_count() != flushes + 1u ||
	    x86_paging_test_page_entry(X86_DOS_VIDEO_LIMIT) != rom_entry)
		return 12;
	if (!x86_paging_test_replace_page_entry(X86_LEGACY_VIDEO_BASE,
						first_entry) ||
	    !x86_paging_test_replace_page_entry(
		    X86_LEGACY_VIDEO_BASE + X86_PAGE_BYTES, damaged_entry) ||
	    !x86_paging_grant_legacy_video_range(X86_LEGACY_VIDEO_BASE,
						 X86_LEGACY_VIDEO_BYTES) ||
	    !x86_paging_revoke_legacy_video_range(X86_LEGACY_VIDEO_BASE,
						  X86_LEGACY_VIDEO_BYTES) ||
	    x86_paging_legacy_video_range_is_user_accessible(
		    X86_LEGACY_VIDEO_BASE, X86_LEGACY_VIDEO_BYTES) ||
	    x86_paging_test_page_entry(X86_DOS_VIDEO_LIMIT) != rom_entry)
		return 13;

	user_entry = x86_paging_test_page_entry(X86_PROTECTED_USER_BASE);
	user_second_entry = x86_paging_test_page_entry(
		X86_PROTECTED_USER_BASE + X86_PAGE_BYTES);
	flushes = x86_paging_test_tlb_flush_count();
	if (!x86_paging_supervisor_range_is_writable(
		    X86_PROTECTED_USER_BASE + 123u, X86_PAGE_BYTES) ||
	    !x86_paging_grant_user_range(X86_PROTECTED_USER_BASE + 123u,
					 X86_PAGE_BYTES) ||
	    !x86_paging_user_range_is_accessible(
		    X86_PROTECTED_USER_BASE + 123u, X86_PAGE_BYTES) ||
	    x86_paging_supervisor_range_is_writable(
		    X86_PROTECTED_USER_BASE + 123u, X86_PAGE_BYTES) ||
	    x86_paging_test_tlb_flush_count() != flushes + 1u ||
	    !x86_paging_revoke_user_range(X86_PROTECTED_USER_BASE + 123u,
					  X86_PAGE_BYTES) ||
	    x86_paging_user_range_is_accessible(
		    X86_PROTECTED_USER_BASE + 123u, X86_PAGE_BYTES) ||
	    !x86_paging_supervisor_range_is_writable(
		    X86_PROTECTED_USER_BASE + 123u, X86_PAGE_BYTES) ||
	    x86_paging_test_tlb_flush_count() != flushes + 2u)
		return 14;
	if (!x86_paging_test_replace_page_entry(
		    X86_PROTECTED_USER_BASE + X86_PAGE_BYTES,
		    user_second_entry & ~TEST_PAGE_PRESENT) ||
	    x86_paging_supervisor_range_is_writable(
		    X86_PROTECTED_USER_BASE + 123u, X86_PAGE_BYTES) ||
	    x86_paging_grant_user_range(X86_PROTECTED_USER_BASE + 123u,
					 X86_PAGE_BYTES) ||
	    !x86_paging_test_replace_page_entry(
		    X86_PROTECTED_USER_BASE + X86_PAGE_BYTES,
		    user_second_entry) ||
	    x86_paging_test_page_entry(X86_PROTECTED_USER_BASE) != user_entry ||
	    x86_paging_supervisor_range_is_writable(
		    X86_PROTECTED_USER_STACK_TOP, 1u))
		return 15;

	/* A ROM write can target only a private high page whose own identity PTE
	 * remains supervisor-only. The exact snapshot is the translation proof. */
	if (!x86_paging_snapshot(&paging))
		return 18;
	if (!x86_paging_guest_identity_translate(
		    &paging, X86_LEGACY_VIDEO_BASE - 1u, false, &translation) ||
	    translation.physical_address != X86_LEGACY_VIDEO_BASE - 1u ||
	    translation.contiguous_bytes != 1u ||
	    x86_paging_guest_shadow_snapshot(
		    &paging, X86_DOS_VIDEO_LIMIT + 1u, &shadow) !=
		    X86_PAGING_GUEST_SHADOW_INVALID_ARGUMENT ||
	    x86_paging_guest_shadow_snapshot(
		    &paging, X86_DOS_VIDEO_LIMIT, &shadow) !=
		    X86_PAGING_GUEST_SHADOW_OK)
		return 19;
	private_entry = x86_paging_test_page_entry(X86_BOOT_IDENTITY_FLOOR);
	if ((private_entry &
	     (TEST_PAGE_PRESENT | TEST_PAGE_WRITABLE | TEST_PAGE_USER)) !=
		    (TEST_PAGE_PRESENT | TEST_PAGE_WRITABLE) ||
	    !x86_paging_test_replace_page_entry(
		    X86_BOOT_IDENTITY_FLOOR, private_entry | TEST_PAGE_USER) ||
	    x86_paging_guest_shadow_publish(
		    &shadow, X86_BOOT_IDENTITY_FLOOR) !=
		    X86_PAGING_GUEST_SHADOW_TARGET_DENIED ||
	    x86_paging_test_page_entry(X86_DOS_VIDEO_LIMIT) != rom_entry ||
	    !x86_paging_test_replace_page_entry(X86_BOOT_IDENTITY_FLOOR,
						private_entry))
		return 20;
	flushes = x86_paging_test_tlb_flush_count();
	if (x86_paging_guest_shadow_publish(
		    &shadow, X86_BOOT_IDENTITY_FLOOR) !=
		    X86_PAGING_GUEST_SHADOW_OK ||
	    (x86_paging_test_page_entry(X86_DOS_VIDEO_LIMIT) &
	     (TEST_PAGE_PRESENT | TEST_PAGE_WRITABLE | TEST_PAGE_USER)) !=
		    (TEST_PAGE_PRESENT | TEST_PAGE_WRITABLE | TEST_PAGE_USER) ||
	    (x86_paging_test_page_entry(X86_DOS_VIDEO_LIMIT) &
	     ~(X86_PAGE_BYTES - 1u)) != X86_BOOT_IDENTITY_FLOOR ||
	    x86_paging_test_page_entry(X86_BOOT_IDENTITY_FLOOR) !=
		    private_entry ||
	    x86_paging_test_tlb_flush_count() != flushes + 1u ||
	    x86_paging_guest_identity_translate(
		    &paging, X86_DOS_VIDEO_LIMIT + 37u, true, &translation) ||
	    !x86_paging_guest_shadow_translate(
		    &shadow, X86_BOOT_IDENTITY_FLOOR,
		    X86_DOS_VIDEO_LIMIT + 37u, true, &translation) ||
	    translation.physical_address != X86_BOOT_IDENTITY_FLOOR + 37u ||
	    translation.contiguous_bytes != X86_PAGE_BYTES - 37u ||
	    x86_paging_guest_range_is_accessible(
		    X86_DOS_VIDEO_LIMIT, X86_PAGE_BYTES, false))
		return 20;
	if (x86_paging_guest_shadow_restore(
		    &shadow, X86_BOOT_IDENTITY_FLOOR) !=
		    X86_PAGING_GUEST_SHADOW_OK ||
	    x86_paging_test_page_entry(X86_DOS_VIDEO_LIMIT) != rom_entry ||
	    x86_paging_test_tlb_flush_count() != flushes + 2u ||
	    x86_paging_guest_shadow_restore(
		    &shadow, X86_BOOT_IDENTITY_FLOOR) !=
		    X86_PAGING_GUEST_SHADOW_OK ||
	    x86_paging_test_tlb_flush_count() != flushes + 2u)
		return 21;
	/* A corrupted alias is never trusted: restore forces the saved PTE and
	 * reports the mismatch so the execution owner can quarantine itself. */
	if (x86_paging_guest_shadow_publish(
		    &shadow, X86_BOOT_IDENTITY_FLOOR) !=
		    X86_PAGING_GUEST_SHADOW_OK ||
	    !x86_paging_test_replace_page_entry(
		    X86_DOS_VIDEO_LIMIT, rom_entry | TEST_PAGE_WRITABLE) ||
	    x86_paging_guest_shadow_restore(
		    &shadow, X86_BOOT_IDENTITY_FLOOR) !=
		    X86_PAGING_GUEST_SHADOW_MAPPING_MISMATCH ||
	    x86_paging_test_page_entry(X86_DOS_VIDEO_LIMIT) != rom_entry ||
	    x86_paging_test_page_entry(X86_BOOT_IDENTITY_FLOOR) != private_entry)
		return 22;
	return 0;
}

DOSC32_TEST_ENTRY(run_tests)

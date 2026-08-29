/* SPDX-License-Identifier: GPL-2.0-only */
/* Early i386 paging policy used before the x86 guest address-space manager. */
#ifndef DOSC32_X86_PAGING_H
#define DOSC32_X86_PAGING_H

#include "compiler.h"
#include "types.h"

struct x86_boot_info;

#define X86_PAGE_BYTES 4096u
#ifndef CONFIG_X86_BOOT_IDENTITY_FLOOR
#error "legacy BIOS configuration must define the identity-map floor"
#endif
#ifndef CONFIG_X86_BOOT_IDENTITY_CEILING
#error "legacy BIOS configuration must define the identity-map ceiling"
#endif
#define X86_BOOT_IDENTITY_FLOOR ((uint32_t)CONFIG_X86_BOOT_IDENTITY_FLOOR)
#define X86_BOOT_IDENTITY_LIMIT ((uint32_t)CONFIG_X86_BOOT_IDENTITY_CEILING)
#define X86_DOS_CONVENTIONAL_LIMIT 0x000a0000u
#define X86_LEGACY_VIDEO_BASE X86_DOS_CONVENTIONAL_LIMIT
#define X86_DOS_VIDEO_LIMIT 0x000c0000u
#define X86_LEGACY_VIDEO_BYTES \
	(X86_DOS_VIDEO_LIMIT - X86_LEGACY_VIDEO_BASE)
#define X86_LEGACY_ROM_LIMIT 0x00100000u
/* The 16-bit segment:offset ceiling with A20 enabled includes the HMA. */
#define X86_REAL_MODE_LINEAR_LIMIT 0x00110000u
/* Supervisor-only XMS pool; reserve the HMA and avoid the Ring-3 aperture. */
/* One fixed native-process aperture below the relocated 4 MiB kernel. */
#define X86_PROTECTED_USER_BASE 0x00200000u
#define X86_PROTECTED_USER_IMAGE_LIMIT 0x003e0000u
#define X86_PROTECTED_USER_STACK_FLOOR 0x003e0000u
#define X86_PROTECTED_USER_STACK_TOP 0x00400000u

enum x86_page_access {
	X86_PAGE_SUPERVISOR_READ_WRITE = 0,
	X86_PAGE_GUEST_READ_WRITE,
	X86_PAGE_GUEST_READ_ONLY
};

/* Fixed-width proof that a caller observed the active boot page directory. */
struct x86_paging_binding {
	uint64_t generation;
	uint32_t page_directory;
	uint32_t guest_linear_limit;
} __aligned(8);

/*
 * Proof that one legacy firmware page was observed in its safe identity,
 * guest-readable, read-only state.  It authorizes only a private RAM alias
 * for that same linear page; it is not a general page-table capability.
 */
struct x86_paging_guest_shadow_snapshot {
	struct x86_paging_binding paging;
	uint32_t linear_page;
	uint32_t original_entry;
	uint8_t reserved[8];
} __aligned(8);

struct x86_paging_guest_translation {
	uint32_t physical_address;
	uint32_t contiguous_bytes;
} __aligned(8);

enum x86_paging_guest_shadow_status {
	X86_PAGING_GUEST_SHADOW_OK = 0,
	X86_PAGING_GUEST_SHADOW_INVALID_ARGUMENT,
	X86_PAGING_GUEST_SHADOW_STALE_BINDING,
	X86_PAGING_GUEST_SHADOW_SOURCE_MISMATCH,
	X86_PAGING_GUEST_SHADOW_TARGET_DENIED,
	/* Restore forced the saved safe PTE after detecting corruption. */
	X86_PAGING_GUEST_SHADOW_MAPPING_MISMATCH
};

/* Pure boundary policy, shared by runtime construction and host tests. */
enum x86_page_access x86_boot_page_access(uint32_t linear_address);
bool x86_boot_page_is_present(const struct x86_boot_info *boot_info,
			      uint32_t linear_address);

/* Called once with interrupts disabled after relocation and BSS clearing. */
void x86_paging_initialize(const struct x86_boot_info *boot_info);
bool x86_paging_is_enabled(void);
uint32_t x86_paging_identity_limit(void);
bool x86_paging_snapshot(struct x86_paging_binding *binding);
bool x86_paging_binding_is_active(
	const struct x86_paging_binding *binding);

/*
 * The architectural A0000h-BFFFFh window is supervisor-only by default.  A
 * foreground display owner may publish only its discovered, page-aligned
 * subranges. Revoke is idempotent and strips user access even when it reports
 * a mapping mismatch.
 */
bool x86_paging_grant_legacy_video_range(uint32_t address,
					 size_t count) __must_check;
bool x86_paging_revoke_legacy_video_range(uint32_t address,
					  size_t count) __must_check;
bool x86_paging_legacy_video_range_is_user_accessible(
	uint32_t address, size_t count) __must_check;

/* Read-only proof over the active low guest identity mappings. */
bool x86_paging_guest_range_is_accessible(uint32_t address, size_t count,
					  bool writable) __must_check;

/*
 * Identity translations cover ordinary low memory.  Firmware aliases require
 * the exact snapshot returned below, so a corrupted low PTE cannot expose an
 * arbitrary supervisor page through this API.
 */
bool x86_paging_guest_identity_translate(
	const struct x86_paging_binding *binding, uint32_t address,
	bool writable,
	struct x86_paging_guest_translation *translation) __must_check;
enum x86_paging_guest_shadow_status x86_paging_guest_shadow_snapshot(
	const struct x86_paging_binding *binding, uint32_t linear_page,
	struct x86_paging_guest_shadow_snapshot *snapshot) __must_check;
enum x86_paging_guest_shadow_status x86_paging_guest_shadow_publish(
	const struct x86_paging_guest_shadow_snapshot *snapshot,
	uint32_t private_physical_page) __must_check;
enum x86_paging_guest_shadow_status x86_paging_guest_shadow_restore(
	const struct x86_paging_guest_shadow_snapshot *snapshot,
	uint32_t private_physical_page) __must_check;
bool x86_paging_guest_shadow_translate(
	const struct x86_paging_guest_shadow_snapshot *snapshot,
	uint32_t private_physical_page, uint32_t address, bool writable,
	struct x86_paging_guest_translation *translation) __must_check;

/* Publishes only complete pages in the protected-process aperture as CPL3. */
bool x86_paging_supervisor_range_is_writable(uint32_t address,
					     size_t count);
bool x86_paging_grant_user_range(uint32_t address, size_t count);
bool x86_paging_revoke_user_range(uint32_t address, size_t count);
bool x86_paging_user_range_is_accessible(uint32_t address, size_t count);

#if defined(X86_PAGING_HOST_TEST)
uint32_t x86_paging_test_page_entry(uint32_t linear_address);
bool x86_paging_test_replace_page_entry(uint32_t linear_address,
					uint32_t entry) __must_check;
uint32_t x86_paging_test_tlb_flush_count(void);
#endif

static_assert_expression(sizeof(struct x86_paging_binding) == 16u,
			 "paging bindings must remain fixed width");
static_assert_expression(
	sizeof(struct x86_paging_guest_shadow_snapshot) == 32u,
	"guest shadow snapshots must remain fixed width");
static_assert_expression(sizeof(struct x86_paging_guest_translation) == 8u,
			 "guest translations must remain fixed width");

#endif

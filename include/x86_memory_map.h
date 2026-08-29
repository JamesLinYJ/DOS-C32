/* SPDX-License-Identifier: GPL-2.0-only */
/* Validated, policy-free queries over the legacy BIOS E820 handoff. */
#ifndef DOSC32_X86_MEMORY_MAP_H
#define DOSC32_X86_MEMORY_MAP_H

#include "types.h"
#include "x86_boot_info.h"

/*
 * Policy-free firmware topology. This describes all usable physical memory
 * reported by E820 after reserved overlaps win; it is deliberately separate
 * from the smaller aperture that the current page allocator can manage.
 */
struct x86_memory_map_snapshot {
	uint64_t usable_bytes;
	uint64_t highest_usable_address;
	uint64_t physical_address_limit;
	uint32_t usable_extent_count;
	uint32_t firmware_range_count;
} __aligned(8);

bool x86_memory_map_is_valid(const struct x86_boot_info *boot_info);
bool x86_memory_map_query(const struct x86_boot_info *boot_info,
			  struct x86_memory_map_snapshot *snapshot);
bool x86_memory_map_usable_limit(const struct x86_boot_info *boot_info,
				 uint64_t *exclusive_limit);
/* Reserved/non-usable overlap wins over usable coverage. */
bool x86_memory_map_range_is_usable(const struct x86_boot_info *boot_info,
				    uint64_t base, uint64_t length);

static_assert_expression(sizeof(struct x86_memory_map_snapshot) == 32u,
			 "memory-map snapshot layout changed");

#endif

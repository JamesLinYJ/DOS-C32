// SPDX-License-Identifier: GPL-2.0-only
/* Shared validation for consumers of the BIOS E820 boot handoff. */
#include "x86_memory_map.h"

#define X86_MEMORY_MAP_BOUNDARY_COUNT \
	(2u * X86_BOOT_MEMORY_RANGE_COUNT)

static bool add_overflow_u64(uint64_t left, uint64_t right,
			     uint64_t *result)
{
	*result = left + right;
	return *result < left;
}

static bool boot_platform_header_is_well_formed(
	const struct x86_boot_platform_handoff *platform)
{
	size_t index;

	if ((platform->flags & (uint32_t)~X86_BOOT_PLATFORM_FLAG_MASK) != 0u)
		return false;
	for (index = 0u; index < ARRAY_SIZE(platform->reserved); ++index) {
		if (platform->reserved[index] != 0u)
			return false;
	}
	return true;
}

bool x86_memory_map_is_valid(const struct x86_boot_info *boot_info)
{
	uint64_t end;
	uint16_t index;

	if (boot_info == NULL ||
	    boot_info->signature != X86_BOOT_INFO_SIGNATURE ||
	    boot_info->version != X86_BOOT_INFO_VERSION ||
	    boot_info->header_bytes != X86_BOOT_INFO_HEADER_BYTES ||
	    boot_info->range_bytes != X86_BOOT_MEMORY_RANGE_BYTES ||
	    boot_info->range_count == 0u ||
	    boot_info->range_count > X86_BOOT_MEMORY_RANGE_COUNT ||
	    boot_info->flags != X86_BOOT_INFO_FLAG_MASK ||
	    !boot_platform_header_is_well_formed(&boot_info->platform))
		return false;
	for (index = 0u; index < boot_info->range_count; ++index) {
		const struct x86_boot_memory_range *range =
			&boot_info->ranges[index];

		if (range->length == 0u || range->type == 0u ||
		    (range->attributes & X86_BOOT_MEMORY_ENABLED) == 0u ||
		    add_overflow_u64(range->base, range->length, &end))
			return false;
	}
	return true;
}

static void sort_boundaries(uint64_t *boundaries, uint32_t count)
{
	uint32_t index;

	for (index = 1u; index < count; ++index) {
		uint64_t value = boundaries[index];
		uint32_t position = index;

		while (position != 0u &&
		       boundaries[position - 1u] > value) {
			boundaries[position] = boundaries[position - 1u];
			--position;
		}
		boundaries[position] = value;
	}
}

static uint32_t compact_boundaries(uint64_t *boundaries, uint32_t count)
{
	uint32_t input;
	uint32_t output = 0u;

	for (input = 0u; input < count; ++input) {
		if (output != 0u &&
		    boundaries[output - 1u] == boundaries[input])
			continue;
		boundaries[output++] = boundaries[input];
	}
	return output;
}

static bool interval_is_usable(const struct x86_boot_info *boot_info,
			       uint64_t base, uint64_t limit)
{
	bool covered_by_usable = false;
	uint16_t index;

	for (index = 0u; index < boot_info->range_count; ++index) {
		const struct x86_boot_memory_range *range =
			&boot_info->ranges[index];
		uint64_t range_limit = range->base + range->length;

		if (range->base > base || range_limit < limit)
			continue;
		if (range->type != X86_BOOT_MEMORY_USABLE)
			return false;
		covered_by_usable = true;
	}
	return covered_by_usable;
}

bool x86_memory_map_query(const struct x86_boot_info *boot_info,
			  struct x86_memory_map_snapshot *snapshot)
{
	uint64_t boundaries[X86_MEMORY_MAP_BOUNDARY_COUNT];
	struct x86_memory_map_snapshot prepared = {0};
	uint32_t boundary_count;
	uint32_t index;
	bool previous_usable = false;

	if (snapshot == NULL || !x86_memory_map_is_valid(boot_info))
		return false;
	boundary_count = (uint32_t)boot_info->range_count * 2u;
	for (index = 0u; index < boot_info->range_count; ++index) {
		uint64_t limit = boot_info->ranges[index].base +
				 boot_info->ranges[index].length;

		boundaries[index * 2u] = boot_info->ranges[index].base;
		boundaries[index * 2u + 1u] = limit;
		if (limit > prepared.physical_address_limit)
			prepared.physical_address_limit = limit;
	}
	sort_boundaries(boundaries, boundary_count);
	boundary_count = compact_boundaries(boundaries, boundary_count);
	for (index = 0u; index + 1u < boundary_count; ++index) {
		uint64_t base = boundaries[index];
		uint64_t limit = boundaries[index + 1u];
		uint64_t length;
		bool usable;

		if (base == limit)
			continue;
		usable = interval_is_usable(boot_info, base, limit);
		if (!usable) {
			previous_usable = false;
			continue;
		}
		length = limit - base;
		if (prepared.usable_bytes > ~(uint64_t)0u - length)
			return false;
		prepared.usable_bytes += length;
		prepared.highest_usable_address = limit - 1u;
		if (!previous_usable)
			++prepared.usable_extent_count;
		previous_usable = true;
	}
	if (prepared.usable_bytes == 0u)
		return false;
	prepared.firmware_range_count = boot_info->range_count;
	*snapshot = prepared;
	return true;
}

bool x86_memory_map_usable_limit(const struct x86_boot_info *boot_info,
				 uint64_t *exclusive_limit)
{
	uint64_t limit = 0u;
	uint16_t index;

	if (exclusive_limit == NULL || !x86_memory_map_is_valid(boot_info))
		return false;
	for (index = 0u; index < boot_info->range_count; ++index) {
		const struct x86_boot_memory_range *range =
			&boot_info->ranges[index];
		uint64_t end;

		if (range->type != X86_BOOT_MEMORY_USABLE)
			continue;
		end = range->base + range->length;
		if (end > limit)
			limit = end;
	}
	if (limit == 0u)
		return false;
	*exclusive_limit = limit;
	return true;
}

bool x86_memory_map_range_is_usable(const struct x86_boot_info *boot_info,
				    uint64_t base, uint64_t length)
{
	uint64_t cursor = base;
	uint64_t end;
	uint16_t index;

	if (length == 0u || add_overflow_u64(base, length, &end) ||
	    !x86_memory_map_is_valid(boot_info))
		return false;
	/* E820 can contain overlapping entries.  Match the allocator rule:
	 * any reserved/non-usable overlap overrides a usable declaration. */
	for (index = 0u; index < boot_info->range_count; ++index) {
		const struct x86_boot_memory_range *range =
			&boot_info->ranges[index];
		uint64_t range_end = range->base + range->length;

		if (range->type != X86_BOOT_MEMORY_USABLE &&
		    range->base < end && base < range_end)
			return false;
	}
	/* Usable entries may be adjacent or overlap.  Advance only over ranges
	 * that cover the current byte, proving complete coverage without sorting
	 * or rewriting the firmware handoff. */
	while (cursor < end) {
		uint64_t covered_until = cursor;

		for (index = 0u; index < boot_info->range_count; ++index) {
			const struct x86_boot_memory_range *range =
				&boot_info->ranges[index];
			uint64_t range_end;

			if (range->type != X86_BOOT_MEMORY_USABLE ||
			    range->base > cursor)
				continue;
			range_end = range->base + range->length;
			if (range_end > covered_until)
				covered_until = range_end;
		}
		if (covered_until == cursor)
			return false;
		cursor = covered_until < end ? covered_until : end;
	}
	return true;
}

// SPDX-License-Identifier: GPL-2.0-only
/* 64-bit canonical-address bump arena for libc32-core bootstrap allocation. */
#include "arena.h"

#include "overflow.h"

enum arena_status address_arena_initialize(struct address_arena *arena,
					   kernel_address_t base,
					   uint64_t capacity)
{
	kernel_address_t end;

	if (arena == NULL)
		return ARENA_INVALID_ARGUMENT;
	if (check_add_overflow(base, capacity, &end))
		return ARENA_RANGE_OVERFLOW;
	(void)end;
	arena->base = base;
	arena->capacity = capacity;
	arena->used = 0u;
	return ARENA_OK;
}

enum arena_status address_arena_allocate(struct address_arena *arena,
					 uint64_t size, uint64_t alignment,
					 kernel_address_t *address)
{
	kernel_address_t current;
	kernel_address_t aligned;
	kernel_address_t end;
	kernel_address_t arena_end;
	uint64_t mask;

	if (address != NULL)
		*address = KERNEL_ADDRESS_INVALID;
	if (arena == NULL || address == NULL || size == 0u || alignment == 0u ||
	    (alignment & (alignment - 1u)) != 0u)
		return ARENA_INVALID_ARGUMENT;
	mask = alignment - 1u;
	if (check_add_overflow(arena->base, arena->used, &current) ||
	    check_add_overflow(current, mask, &aligned))
		return ARENA_RANGE_OVERFLOW;
	aligned &= ~mask;
	if (check_add_overflow(aligned, size, &end) ||
	    check_add_overflow(arena->base, arena->capacity, &arena_end))
		return ARENA_RANGE_OVERFLOW;
	if (aligned < arena->base || end > arena_end)
		return ARENA_OUT_OF_SPACE;
	arena->used = end - arena->base;
	*address = aligned;
	return ARENA_OK;
}

uint64_t address_arena_checkpoint(const struct address_arena *arena)
{
	return arena == NULL ? (uint64_t)-1 : arena->used;
}

enum arena_status address_arena_rewind(struct address_arena *arena,
				       uint64_t checkpoint)
{
	if (arena == NULL)
		return ARENA_INVALID_ARGUMENT;
	if (checkpoint > arena->used)
		return ARENA_INVALID_CHECKPOINT;
	arena->used = checkpoint;
	return ARENA_OK;
}

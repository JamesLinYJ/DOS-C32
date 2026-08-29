/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DOSC32_ARENA_H
#define DOSC32_ARENA_H

#include "address.h"
#include "compiler.h"

enum arena_status {
	ARENA_OK = 0,
	ARENA_INVALID_ARGUMENT,
	ARENA_RANGE_OVERFLOW,
	ARENA_OUT_OF_SPACE,
	ARENA_INVALID_CHECKPOINT
};

struct address_arena {
	kernel_address_t base;
	uint64_t capacity;
	uint64_t used;
};

enum arena_status address_arena_initialize(struct address_arena *arena,
					   kernel_address_t base,
					   uint64_t capacity) __must_check;
enum arena_status address_arena_allocate(struct address_arena *arena,
					 uint64_t size, uint64_t alignment,
					 kernel_address_t *address) __must_check;
uint64_t address_arena_checkpoint(const struct address_arena *arena);
enum arena_status address_arena_rewind(struct address_arena *arena,
				       uint64_t checkpoint) __must_check;

#endif

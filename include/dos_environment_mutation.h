/* SPDX-License-Identifier: GPL-2.0-only */
/* Transactional mutation of a PSP-owned DOS environment block. */
#ifndef DOSC32_DOS_ENVIRONMENT_MUTATION_H
#define DOSC32_DOS_ENVIRONMENT_MUTATION_H

#include "compiler.h"
#include "dos_machine.h"
#include "dos_memory.h"
#include "types.h"

#define DOS_ENVIRONMENT_MUTATION_MAX_NAME_BYTES 126u
#define DOS_ENVIRONMENT_MUTATION_MAX_BLOCK_BYTES 0x8000u

enum dos_environment_mutation_action {
	DOS_ENVIRONMENT_MUTATION_SET = 0,
	DOS_ENVIRONMENT_MUTATION_DELETE = 1
};

enum dos_environment_mutation_status {
	DOS_ENVIRONMENT_MUTATION_OK = 0,
	DOS_ENVIRONMENT_MUTATION_INVALID_ARGUMENT,
	DOS_ENVIRONMENT_MUTATION_NOT_FOUND,
	DOS_ENVIRONMENT_MUTATION_BAD_PSP,
	DOS_ENVIRONMENT_MUTATION_BAD_BLOCK,
	DOS_ENVIRONMENT_MUTATION_BAD_LAYOUT,
	DOS_ENVIRONMENT_MUTATION_RANGE_OVERFLOW,
	DOS_ENVIRONMENT_MUTATION_NOT_ENOUGH_MEMORY,
	DOS_ENVIRONMENT_MUTATION_MACHINE_FAULT,
	DOS_ENVIRONMENT_MUTATION_ARENA_DAMAGED,
	DOS_ENVIRONMENT_MUTATION_OWNER_MISMATCH,
	DOS_ENVIRONMENT_MUTATION_STALE_PSP,
	DOS_ENVIRONMENT_MUTATION_POISONED
};

/*
 * Replace, add, or explicitly delete NAME in PSP:2ch's environment.
 * NAME matching is ASCII case-insensitive.  SET with value_length zero creates
 * the valid empty entry "NAME="; only DELETE removes an entry.
 *
 * The caller owns name/value for the complete call and must already exclude
 * guest execution and interrupt observers.  This function allocates and
 * validates an isolated replacement before changing PSP:2ch.  The old block
 * is released only after publication.  Failure preserves the old PSP and
 * environment whenever that can be proved; otherwise the arena is poisoned
 * and the affected allocations remain quarantined.
 */
enum dos_environment_mutation_status dos_environment_mutate_psp(
	struct dos_memory_arena *arena, const struct dos_machine *machine,
	uint16_t psp_segment, enum dos_environment_mutation_action action,
	const uint8_t *name, size_t name_capacity, size_t name_length,
	const uint8_t *value, size_t value_capacity,
	size_t value_length) __must_check;

#endif

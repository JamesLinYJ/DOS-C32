/* SPDX-License-Identifier: GPL-2.0-only */
/* Safe C interface for the MS-DOS MCB arena. */
#ifndef DOSC32_DOS_MEMORY_H
#define DOSC32_DOS_MEMORY_H

#include "compiler.h"
#include "dos_error.h"
#include "dos_machine.h"
#include "types.h"

enum dos_allocation_strategy {
	DOS_ALLOC_FIRST_FIT = 0,
	DOS_ALLOC_BEST_FIT = 1,
	DOS_ALLOC_LAST_FIT = 2
};

#define DOS_MEMORY_OWNER_NAME_BYTES 8u
#define DOS_MEMORY_MCB_BYTES 16u
#define DOS_MEMORY_GENERATION_MAX (~(uint64_t)0u)

/*
 * Internal results keep machine failures separate from DOS-visible errors.
 * INT 21h wrappers below deliberately translate these to DOS errors.
 */
enum dos_memory_status {
	DOS_MEMORY_OK = 0,
	DOS_MEMORY_INVALID_ARGUMENT,
	DOS_MEMORY_INVALID_BLOCK,
	DOS_MEMORY_NOT_ENOUGH_MEMORY,
	DOS_MEMORY_ARENA_DAMAGED,
	DOS_MEMORY_MACHINE_FAULT,
	DOS_MEMORY_MACHINE_POISONED,
	DOS_MEMORY_OWNER_MISMATCH,
	DOS_MEMORY_IDENTITY_MISMATCH,
	DOS_MEMORY_GENERATION_EXHAUSTED
};

struct dos_memory_owner_identity {
	uint16_t psp_segment;
	uint8_t name[DOS_MEMORY_OWNER_NAME_BYTES];
};

/*
 * Scan_Execname parsing belongs to the EXEC-name layer.  This value carries
 * only its already-bounded MCB prefix update: count is 1..8 and every reserved
 * byte must be zero.
 */
struct dos_memory_owner_name_patch {
	uint8_t bytes[DOS_MEMORY_OWNER_NAME_BYTES];
	uint8_t count;
	uint8_t reserved[7];
};

/*
 * Exact, value-only replacement for one reachable MCB.  EXEC prepares this
 * while the allocation is still parent-owned, journals replacement_bytes,
 * and changes the native lease owner only at its callback-free seal.  Keeping
 * the complete 16-byte record preserves signature, size and untouched name
 * bytes according to MS-DOS semantics, without retaining a guest/native
 * pointer or requiring a second MCB read at publication time.
 */
struct dos_memory_owner_rebind_value {
	uint16_t header_segment;
	uint16_t expected_owner;
	uint16_t new_owner;
	uint8_t reserved[2];
	uint8_t replacement_bytes[DOS_MEMORY_MCB_BYTES];
};

static_assert_expression(sizeof(struct dos_memory_owner_name_patch) == 16,
			 "MCB name patches must have a fixed cross-ABI size");
static_assert_expression(__builtin_offsetof(struct dos_memory_owner_name_patch,
					    count) == 8,
			 "MCB name patch count offset changed");
static_assert_expression(sizeof(struct dos_memory_owner_rebind_value) == 24,
			 "MCB rebind values must have a fixed cross-ABI size");
static_assert_expression(
    __builtin_offsetof(struct dos_memory_owner_rebind_value,
			       replacement_bytes) == 8,
    "MCB rebind replacement offset changed");

struct dos_memory_allocation_result {
	uint16_t block_segment;
	uint16_t maximum_available;
};

struct dos_memory_arena {
	uint64_t generation;
	kernel_object_handle_t identity;
	uint16_t head_segment;
	uint8_t strategy;
	uint8_t initialized;
	/* A failed rollback makes every later arena operation fail closed. */
	uint8_t machine_poisoned;
	uint8_t constructed;
	uint8_t reserved[2];
} __aligned(8);

#define DOS_MEMORY_ARENA_INITIALIZER(identity_value)                           \
	{.generation = 0u,                                                     \
	 .identity = (identity_value),                                         \
	 .head_segment = 0u,                                                   \
	 .strategy = DOS_ALLOC_FIRST_FIT,                                      \
	 .initialized = 0u,                                                    \
	 .machine_poisoned = 0u,                                               \
	 .constructed = 1u,                                                    \
	 .reserved = {0u}}

static_assert_expression(sizeof(struct dos_memory_arena) == 24,
			 "memory arenas must be data-model independent");
static_assert_expression(__alignof__(struct dos_memory_arena) == 8,
			 "memory arena alignment changed");
static_assert_expression(__builtin_offsetof(struct dos_memory_arena,
					    initialized) == 19,
			 "memory arena flag offset changed");
static_assert_expression(__builtin_offsetof(struct dos_memory_arena,
					    reserved) == 22,
			 "memory arena reserved offset changed");

/*
 * These checked interfaces are for kernel/EXEC transactions.  Construct (or
 * DOS_MEMORY_ARENA_INITIALIZER) must establish the C object lifetime before
 * initialize; identity must be neither zero nor KERNEL_OBJECT_HANDLE_INVALID,
 * and initialize increments its nonzero generation on success.
 * Every other operation rejects a poisoned arena.  Callers must serialize one
 * arena through an already-exclusive execution context, not a lock held over
 * this call: none of these functions retains a native pointer or holds an
 * arena lock while invoking the guest-memory backend.
 */
enum dos_memory_status
dos_memory_arena_construct(struct dos_memory_arena *arena,
			   kernel_object_handle_t identity) __must_check;
enum dos_memory_status dos_memory_arena_initialize_checked(
    struct dos_memory_arena *arena, const struct dos_machine *machine,
    uint16_t head_segment, uint16_t end_segment) __must_check;
enum dos_memory_status dos_memory_arena_validate_checked(
    const struct dos_memory_arena *arena,
    const struct dos_machine *machine) __must_check;
/* Sticky no-callback quarantine for an uncertain enclosing transaction. */
enum dos_memory_status
dos_memory_arena_poison(struct dos_memory_arena *arena) __must_check;
enum dos_memory_status dos_memory_allocate_checked(
    struct dos_memory_arena *arena, const struct dos_machine *machine,
    uint16_t owner_psp, uint16_t paragraphs,
    struct dos_memory_allocation_result *result) __must_check;
enum dos_memory_status dos_memory_allocate_named_checked(
    struct dos_memory_arena *arena, const struct dos_machine *machine,
    const struct dos_memory_owner_identity *owner, uint16_t paragraphs,
    struct dos_memory_allocation_result *result) __must_check;
/*
 * EXEC probes allocation with BX=ffffh after reserving the environment and
 * consumes the returned largest free block.  This typed form preserves the
 * same bounded walk and forward coalescing without manufacturing a DOS error
 * as native control flow.  maximum_available is unchanged on error.
 */
enum dos_memory_status
dos_memory_query_maximum_checked(struct dos_memory_arena *arena,
				 const struct dos_machine *machine,
				 uint16_t *maximum_available) __must_check;
enum dos_memory_status
dos_memory_free_checked(struct dos_memory_arena *arena,
			const struct dos_machine *machine,
			uint16_t block_segment) __must_check;
enum dos_memory_status dos_memory_free_owned_checked(
    struct dos_memory_arena *arena, const struct dos_machine *machine,
    uint16_t block_segment, uint16_t expected_owner) __must_check;
/*
 * Native EXEC ownership transfer is stricter than $SETBLOCK: the target MCB
 * must be reachable from arena_head and must still have expected_owner.
 * Both owner values are nonzero.  The unnamed form changes only arena_owner
 * and preserves all eight arena_name bytes; the named form replaces them with
 * the fixed-width identity name.  The patch form changes owner and the bounded
 * name prefix in the same 16-byte MCB replacement while retaining its tail.
 * Replacement is transactional and a failed rollback poisons the arena.
 */
enum dos_memory_status dos_memory_transfer_owner_checked(
    struct dos_memory_arena *arena, const struct dos_machine *machine,
    uint16_t block_segment, uint16_t expected_owner,
    uint16_t new_owner) __must_check;
enum dos_memory_status dos_memory_transfer_named_owner_checked(
    struct dos_memory_arena *arena, const struct dos_machine *machine,
    uint16_t block_segment, uint16_t expected_owner,
    const struct dos_memory_owner_identity *new_owner) __must_check;
/* Replace only name[0..count); the remaining arena_name bytes are preserved. */
enum dos_memory_status dos_memory_transfer_owner_name_patch_checked(
    struct dos_memory_arena *arena, const struct dos_machine *machine,
    uint16_t block_segment, uint16_t expected_owner, uint16_t new_owner,
    const struct dos_memory_owner_name_patch *name_patch) __must_check;
/*
 * Prepare-only forms perform the same bounded reachable-MCB and owner checks
 * as transfer, but do not write guest memory or mutate the arena.  Output is
 * unchanged on every error.  The value validator is pure and suitable for
 * persistent transaction reconstruction checks.
 */
bool dos_memory_owner_rebind_value_has_valid_encoding(
    const struct dos_memory_owner_rebind_value *value) __must_check;
enum dos_memory_status dos_memory_prepare_owner_rebind_checked(
    const struct dos_memory_arena *arena, const struct dos_machine *machine,
    uint16_t block_segment, uint16_t expected_owner,
    uint16_t new_owner,
    struct dos_memory_owner_rebind_value *value) __must_check;
enum dos_memory_status dos_memory_prepare_owner_name_patch_rebind_checked(
    const struct dos_memory_arena *arena, const struct dos_machine *machine,
    uint16_t block_segment, uint16_t expected_owner, uint16_t new_owner,
    const struct dos_memory_owner_name_patch *name_patch,
    struct dos_memory_owner_rebind_value *value) __must_check;
enum dos_memory_status dos_memory_resize_checked(
    struct dos_memory_arena *arena, const struct dos_machine *machine,
    uint16_t block_segment, uint16_t current_psp, uint16_t paragraphs,
    uint16_t *maximum_available) __must_check;
enum dos_memory_status
dos_memory_free_process_checked(struct dos_memory_arena *arena,
				const struct dos_machine *machine,
				uint16_t owner_psp) __must_check;
enum dos_memory_status
dos_memory_get_strategy_checked(const struct dos_memory_arena *arena,
				uint8_t *strategy) __must_check;
enum dos_memory_status
dos_memory_set_strategy_checked(struct dos_memory_arena *arena,
				uint8_t strategy) __must_check;

/* MS-DOS-compatible allocation entry points retained for INT 21h. */
enum dos_error dos_memory_arena_initialize(struct dos_memory_arena *arena,
					   const struct dos_machine *machine,
					   uint16_t head_segment,
					   uint16_t end_segment) __must_check;
enum dos_error
dos_memory_arena_validate(const struct dos_memory_arena *arena,
			  const struct dos_machine *machine) __must_check;
/*
 * Allocation copies CurrentPDB verbatim, including zero, into arena_owner.
 * Native named/lease allocation rejects zero because it denotes a free MCB,
 * but this DOS entry point preserves the required unusual visible behaviour.
 * block_segment is a simulated 16-bit DOS segment, so zero can also be a
 * successful result after 16-bit wrap; callers must use the returned status
 * rather than treating zero as NULL.
 */
enum dos_error dos_memory_allocate(struct dos_memory_arena *arena,
				   const struct dos_machine *machine,
				   uint16_t owner_psp, uint16_t paragraphs,
				   uint16_t *block_segment,
				   uint16_t *maximum_available) __must_check;
enum dos_error dos_memory_free(struct dos_memory_arena *arena,
			       const struct dos_machine *machine,
			       uint16_t block_segment) __must_check;
enum dos_error dos_memory_resize(struct dos_memory_arena *arena,
				 const struct dos_machine *machine,
				 uint16_t block_segment, uint16_t current_psp,
				 uint16_t paragraphs,
				 uint16_t *maximum_available) __must_check;
enum dos_error dos_memory_free_process(struct dos_memory_arena *arena,
				       const struct dos_machine *machine,
				       uint16_t owner_psp) __must_check;
enum dos_error dos_memory_get_strategy(const struct dos_memory_arena *arena,
				       uint8_t *strategy) __must_check;
enum dos_error dos_memory_set_strategy(struct dos_memory_arena *arena,
				       uint8_t strategy) __must_check;

#endif

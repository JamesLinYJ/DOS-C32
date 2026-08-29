/* SPDX-License-Identifier: GPL-2.0-only */
/* Generation-checked EXEC ownership for DOS MCB allocations. */
#ifndef DOSC32_DOS_MEMORY_LEASE_H
#define DOSC32_DOS_MEMORY_LEASE_H

#include "compiler.h"
#include "dos_memory.h"
#include "types.h"

#define DOS_MEMORY_LEASE_SLOT_COUNT 16u
#define DOS_MEMORY_LEASE_SLOT_BITS 4u
#define DOS_MEMORY_LEASE_SLOT_MASK 0x0full
#define DOS_MEMORY_LEASE_GENERATION_SHIFT DOS_MEMORY_LEASE_SLOT_BITS
#define DOS_MEMORY_LEASE_TABLE_IDENTITY_SHIFT 32u
#define DOS_MEMORY_LEASE_HANDLE_LOCAL_MASK 0xffffffffull
/* A slot at this generation is permanently retired; it never wraps to one. */
#define DOS_MEMORY_LEASE_GENERATION_MAX 0x0fffffffull

typedef uint32_t dos_memory_lease_table_identity_t;

#define DOS_MEMORY_LEASE_TABLE_IDENTITY_INVALID                                \
	((dos_memory_lease_table_identity_t)-1)

enum dos_memory_lease_status {
	DOS_MEMORY_LEASE_OK = 0,
	DOS_MEMORY_LEASE_INVALID_ARGUMENT,
	DOS_MEMORY_LEASE_NO_SLOT,
	DOS_MEMORY_LEASE_NOT_ENOUGH_MEMORY,
	DOS_MEMORY_LEASE_INVALID_BLOCK,
	DOS_MEMORY_LEASE_ARENA_DAMAGED,
	DOS_MEMORY_LEASE_MACHINE_FAULT,
	DOS_MEMORY_LEASE_MACHINE_POISONED,
	DOS_MEMORY_LEASE_STALE_HANDLE,
	DOS_MEMORY_LEASE_OWNER_MISMATCH,
	DOS_MEMORY_LEASE_CONTEXT_MISMATCH,
	DOS_MEMORY_LEASE_IDENTITY_MISMATCH,
	DOS_MEMORY_LEASE_GENERATION_EXHAUSTED,
	DOS_MEMORY_LEASE_INVALID_STATE
};

enum dos_memory_lease_slot_state {
	DOS_MEMORY_LEASE_SLOT_VACANT = 0,
	DOS_MEMORY_LEASE_SLOT_ACQUIRING,
	DOS_MEMORY_LEASE_SLOT_ACTIVE,
	DOS_MEMORY_LEASE_SLOT_RELEASING,
	DOS_MEMORY_LEASE_SLOT_RELEASED,
	DOS_MEMORY_LEASE_SLOT_ABORTED,
	DOS_MEMORY_LEASE_SLOT_PUBLISHED
};

struct dos_memory_lease_handle {
	uint64_t value;
} __aligned(8);

/* No field is a native pointer; every guest reference remains an integer. */
struct dos_memory_lease_slot {
	uint64_t generation;
	kernel_object_handle_t machine_context;
	kernel_object_handle_t arena_identity;
	uint64_t arena_generation;
	uint16_t arena_head_segment;
	uint16_t guest_segment;
	uint16_t owner;
	uint16_t paragraphs;
	dos_memory_lease_table_identity_t lifetime_identity;
	uint8_t state;
	uint8_t reserved[3];
} __aligned(8);

struct dos_memory_lease_table {
	struct dos_memory_lease_slot slots[DOS_MEMORY_LEASE_SLOT_COUNT];
	dos_memory_lease_table_identity_t lifetime_identity;
	uint8_t initialized;
	uint8_t constructed;
	uint8_t reserved[2];
} __aligned(8);

#define DOS_MEMORY_LEASE_TABLE_INITIALIZER(_identity)                          \
	{.slots = {{0}},                                                       \
	 .lifetime_identity = (_identity),                                     \
	 .initialized = 0u,                                                    \
	 .constructed = 1u,                                                    \
	 .reserved = {0u}}

struct dos_memory_lease_receipt {
	struct dos_memory_lease_handle handle;
	uint16_t guest_segment;
	uint16_t paragraphs;
	uint16_t maximum_available;
	uint16_t reserved;
} __aligned(8);

/* Fixed-width, read-only description of one currently active lease. */
struct dos_memory_lease_view {
	struct dos_memory_lease_handle handle;
	kernel_object_handle_t machine_context;
	kernel_object_handle_t arena_identity;
	uint64_t arena_generation;
	uint16_t guest_segment;
	uint16_t paragraphs;
	uint16_t owner;
	uint16_t reserved;
} __aligned(8);

/*
 * Generation-bound plan for EXEC's final MCB ownership publication.  The
 * exact guest MCB replacement is staged through dos_exec_journal while the
 * native lease remains parent-owned.  Final rebind+publish then consumes only
 * these fixed values and invokes no guest callback.
 */
struct dos_memory_lease_rebind_plan {
	struct dos_memory_lease_handle handle;
	kernel_object_handle_t machine_context;
	kernel_object_handle_t arena_identity;
	uint64_t arena_generation;
	struct dos_memory_owner_rebind_value value;
	uint16_t guest_segment;
	uint16_t paragraphs;
	uint16_t arena_head_segment;
	uint8_t reserved[2];
} __aligned(8);

bool dos_memory_lease_rebind_plan_has_valid_encoding(
    const struct dos_memory_lease_rebind_plan *plan) __must_check;

static_assert_expression(sizeof(dos_memory_lease_table_identity_t) == 4,
			 "memory lease table identities must remain 32-bit");
static_assert_expression(
    sizeof(struct dos_memory_lease_handle) == 8,
    "memory lease handles must remain one explicit 64-bit value");
static_assert_expression(__alignof__(struct dos_memory_lease_handle) == 8,
			 "memory lease-handle alignment changed");
static_assert_expression(sizeof(struct dos_memory_lease_slot) == 48,
			 "memory lease slots must be data-model independent");
static_assert_expression(__alignof__(struct dos_memory_lease_slot) == 8,
			 "memory lease-slot alignment changed");
static_assert_expression(__builtin_offsetof(struct dos_memory_lease_slot,
					    lifetime_identity) == 40,
			 "memory lease slot identity offset changed");
static_assert_expression(__builtin_offsetof(struct dos_memory_lease_slot,
					    state) == 44,
			 "memory lease state offset changed");
static_assert_expression(sizeof(struct dos_memory_lease_table) == 776,
			 "memory lease tables must be data-model independent");
static_assert_expression(__alignof__(struct dos_memory_lease_table) == 8,
			 "memory lease-table alignment changed");
static_assert_expression(__builtin_offsetof(struct dos_memory_lease_table,
					    lifetime_identity) == 768,
			 "memory lease table identity offset changed");
static_assert_expression(__builtin_offsetof(struct dos_memory_lease_table,
					    initialized) == 772,
			 "memory lease table flag offset changed");
static_assert_expression(
    sizeof(struct dos_memory_lease_receipt) == 16,
    "memory lease receipts must have a fixed cross-ABI size");
static_assert_expression(__alignof__(struct dos_memory_lease_receipt) == 8,
			 "memory lease-receipt alignment changed");
static_assert_expression(sizeof(struct dos_memory_lease_view) == 40,
			 "memory lease views must have a fixed cross-ABI size");
static_assert_expression(__alignof__(struct dos_memory_lease_view) == 8,
			 "memory lease-view alignment changed");
static_assert_expression(sizeof(struct dos_memory_lease_rebind_plan) == 64,
			 "memory rebind plans must have a fixed cross-ABI size");
static_assert_expression(
    __builtin_offsetof(struct dos_memory_lease_rebind_plan, value) == 32,
    "memory rebind value offset changed");
static_assert_expression(
    __builtin_offsetof(struct dos_memory_lease_rebind_plan,
			       guest_segment) == 56,
    "memory rebind guest segment offset changed");
static_assert_expression(
    DOS_MEMORY_LEASE_SLOT_COUNT == (1u << DOS_MEMORY_LEASE_SLOT_BITS),
    "lease handle slot encoding no longer covers every fixed slot");
static_assert_expression(
    DOS_MEMORY_LEASE_TABLE_IDENTITY_SHIFT + 32u == 64u,
    "memory lease identity no longer occupies the high handle word");
static_assert_expression(
    ((DOS_MEMORY_LEASE_GENERATION_MAX <<
      DOS_MEMORY_LEASE_GENERATION_SHIFT) |
     DOS_MEMORY_LEASE_SLOT_MASK) == DOS_MEMORY_LEASE_HANDLE_LOCAL_MASK,
    "memory lease generation and slot no longer fill the low handle word");

/*
 * Construct establishes a new C object lifetime and is not a reset operation.
 * The caller supplies a nonzero, non-invalid 32-bit identity which must never
 * be reused while an old handle could be submitted.  The fixed 64-bit handle
 * stores that identity in bits 63:32, a non-wrapping 28-bit slot generation in
 * bits 31:4, and the slot in bits 3:0.  A handle from an ended lifetime
 * therefore cannot name a lease in replacement storage even when its slot and
 * generation repeat.  Callers may reuse the storage only after every active
 * lease has been drained.  Initialize succeeds exactly once in that lifetime.
 *
 * An already-exclusive executor context must serialize the arena and table;
 * no arena lock may be held across entry because guest-memory I/O occurs
 * inside acquire/transfer/release/abort.  Publish is the final callback-free
 * state transition.  The table never retains machine or arena pointers and
 * therefore cannot extend a native mapping lifetime.
 */
enum dos_memory_lease_status dos_memory_lease_table_construct(
    struct dos_memory_lease_table *table,
    dos_memory_lease_table_identity_t lifetime_identity) __must_check;
enum dos_memory_lease_status dos_memory_lease_table_initialize(
    struct dos_memory_lease_table *table) __must_check;
/*
 * Pure reconstruction preflight.  A valid table is drained exactly when no
 * slot still owns an acquiring, active, or releasing rollback obligation.
 */
bool dos_memory_lease_table_is_drained(
    const struct dos_memory_lease_table *table) __must_check;
/* Named program allocation replaces the fixed eight-byte MCB name. */
enum dos_memory_lease_status dos_memory_lease_acquire(
    struct dos_memory_lease_table *table, struct dos_memory_arena *arena,
    const struct dos_machine *machine,
    const struct dos_memory_owner_identity *owner, uint16_t paragraphs,
    struct dos_memory_lease_receipt *receipt) __must_check;
/* MS-DOS-compatible environment allocation leaves the MCB name untouched. */
enum dos_memory_lease_status dos_memory_lease_acquire_unnamed(
    struct dos_memory_lease_table *table, struct dos_memory_arena *arena,
    const struct dos_machine *machine, uint16_t owner, uint16_t paragraphs,
    struct dos_memory_lease_receipt *receipt) __must_check;
/*
 * Resolve a value-only view after validating the complete active ownership
 * tuple.  The view is usable only inside the same exclusive EXEC/IRQ
 * observation interval as this call.  It is not a persistent capability and
 * must be resolved again after dropping that serialization boundary.
 * Output is unchanged on every error.
 */
enum dos_memory_lease_status dos_memory_lease_resolve_active(
    const struct dos_memory_lease_table *table,
    const struct dos_memory_arena *arena, const struct dos_machine *machine,
    struct dos_memory_lease_handle handle, uint16_t expected_owner,
    struct dos_memory_lease_view *view) __must_check;
/*
 * Transfer first validates the complete ACTIVE tuple and canonical guest MCB.
 * The slot owner changes only after the transactional MCB update succeeds.
 */
enum dos_memory_lease_status dos_memory_lease_transfer_owner(
    struct dos_memory_lease_table *table, struct dos_memory_arena *arena,
    const struct dos_machine *machine, struct dos_memory_lease_handle handle,
    uint16_t expected_owner, uint16_t new_owner) __must_check;
enum dos_memory_lease_status dos_memory_lease_transfer_named_owner(
    struct dos_memory_lease_table *table, struct dos_memory_arena *arena,
    const struct dos_machine *machine, struct dos_memory_lease_handle handle,
    uint16_t expected_owner,
    const struct dos_memory_owner_identity *new_owner) __must_check;
/* Scan_Execname prefix update; path parsing remains outside the lease layer. */
enum dos_memory_lease_status dos_memory_lease_transfer_owner_name_patch(
    struct dos_memory_lease_table *table, struct dos_memory_arena *arena,
    const struct dos_machine *machine, struct dos_memory_lease_handle handle,
    uint16_t expected_owner, uint16_t new_owner,
    const struct dos_memory_owner_name_patch *name_patch) __must_check;
/*
 * Prepare one exact MCB replacement without changing guest memory or the
 * native slot. Output is unchanged on every error. The patch form preserves
 * bytes after the bounded executable-name prefix.
 */
enum dos_memory_lease_status dos_memory_lease_prepare_owner_rebind(
    const struct dos_memory_lease_table *table,
    const struct dos_memory_arena *arena, const struct dos_machine *machine,
    struct dos_memory_lease_handle handle, uint16_t expected_owner,
    uint16_t new_owner,
    struct dos_memory_lease_rebind_plan *plan) __must_check;
enum dos_memory_lease_status
dos_memory_lease_prepare_owner_name_patch_rebind(
    const struct dos_memory_lease_table *table,
    const struct dos_memory_arena *arena, const struct dos_machine *machine,
    struct dos_memory_lease_handle handle, uint16_t expected_owner,
    uint16_t new_owner, const struct dos_memory_owner_name_patch *name_patch,
    struct dos_memory_lease_rebind_plan *plan) __must_check;
/* Pure preflight followed by one callback-free ACTIVE -> PUBLISHED commit. */
enum dos_memory_lease_status dos_memory_lease_preflight_rebind_publish(
    const struct dos_memory_lease_table *table,
    const struct dos_memory_arena *arena, const struct dos_machine *machine,
    const struct dos_memory_lease_rebind_plan *plan) __must_check;
enum dos_memory_lease_status dos_memory_lease_rebind_publish(
    struct dos_memory_lease_table *table,
    const struct dos_memory_arena *arena, const struct dos_machine *machine,
    const struct dos_memory_lease_rebind_plan *plan) __must_check;
/*
 * Final no-callback commit.  ACTIVE becomes PUBLISHED; repeating publication
 * of the same generation is successful.  Publication transfers the rollback
 * right away, so release/abort no longer free that MCB.  A published slot is
 * reusable at the next generation, which makes the old handle stale.
 */
enum dos_memory_lease_status
dos_memory_lease_preflight_publish(const struct dos_memory_lease_table *table,
				   const struct dos_memory_arena *arena,
				   const struct dos_machine *machine,
				   struct dos_memory_lease_handle handle,
				   uint16_t expected_owner) __must_check;
enum dos_memory_lease_status dos_memory_lease_publish(
    struct dos_memory_lease_table *table, const struct dos_memory_arena *arena,
    const struct dos_machine *machine, struct dos_memory_lease_handle handle,
    uint16_t expected_owner) __must_check;
enum dos_memory_lease_status dos_memory_lease_release(
    struct dos_memory_lease_table *table, struct dos_memory_arena *arena,
    const struct dos_machine *machine, struct dos_memory_lease_handle handle,
    uint16_t expected_owner) __must_check;
/* Repeating abort on the same terminal generation is explicitly successful. */
enum dos_memory_lease_status dos_memory_lease_abort(
    struct dos_memory_lease_table *table, struct dos_memory_arena *arena,
    const struct dos_machine *machine, struct dos_memory_lease_handle handle,
    uint16_t expected_owner) __must_check;

#endif

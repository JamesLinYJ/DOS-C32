/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Serialized DOS process-global state.
 *
 * CurrentPDB and DMAADD are MS-DOS process globals. The
 * identity and generation are native transaction metadata only.  Identity
 * prevents a snapshot from crossing a reconstructed runtime lifetime;
 * generation prevents publication over changed DOS state.  Neither is
 * guest-visible.
 */
#ifndef DOSC32_DOS_PROCESS_RUNTIME_H
#define DOSC32_DOS_PROCESS_RUNTIME_H

#include "address.h"
#include "compiler.h"
#include "dos_abi.h"
#include "types.h"

#define DOS_PROCESS_RUNTIME_GENERATION_MAX (~(uint64_t)0u)

enum dos_process_runtime_status {
	DOS_PROCESS_RUNTIME_OK = 0,
	DOS_PROCESS_RUNTIME_INVALID_ARGUMENT,
	DOS_PROCESS_RUNTIME_NOT_INITIALIZED,
	DOS_PROCESS_RUNTIME_INVALID_STATE,
	DOS_PROCESS_RUNTIME_STALE_SNAPSHOT,
	DOS_PROCESS_RUNTIME_GENERATION_EXHAUSTED,
	DOS_PROCESS_RUNTIME_POISONED
};

/* Native state only.  It contains guest values, never native pointers. */
struct dos_process_runtime {
	uint64_t generation;
	kernel_object_handle_t identity;
	struct dos_far_pointer16 dta;
	uint16_t current_psp;
	uint8_t initialized;
	uint8_t poisoned;
	uint8_t constructed;
	uint8_t reserved[7];
} __aligned(8);

#define DOS_PROCESS_RUNTIME_INITIALIZER                                        \
	{.generation = 0u,                                                     \
	 .identity = KERNEL_OBJECT_HANDLE_INVALID,                             \
	 .dta = {.offset = 0u, .segment = 0u},                                 \
	 .current_psp = 0u,                                                    \
	 .initialized = 0u,                                                    \
	 .poisoned = 0u,                                                       \
	 .constructed = 1u,                                                    \
	 .reserved = {0u}}

/* Fixed-width value object retained by an EXEC preparation transaction. */
struct dos_process_runtime_snapshot {
	uint64_t generation;
	kernel_object_handle_t runtime_identity;
	struct dos_far_pointer16 dta;
	uint16_t current_psp;
	uint16_t reserved;
} __aligned(8);

static_assert_expression(
    sizeof(struct dos_process_runtime) == 32,
    "process runtime state must be data-model independent");
static_assert_expression(__alignof__(struct dos_process_runtime) == 8,
			 "process runtime-state alignment changed");
static_assert_expression(__builtin_offsetof(struct dos_process_runtime,
					    identity) == 8,
			 "process runtime identity offset changed");
static_assert_expression(__builtin_offsetof(struct dos_process_runtime,
					    current_psp) == 20,
			 "process runtime CurrentPDB offset changed");
static_assert_expression(__builtin_offsetof(struct dos_process_runtime,
					    initialized) == 22,
			 "process runtime flag offset changed");
static_assert_expression(
    sizeof(struct dos_process_runtime_snapshot) == 24,
    "process runtime snapshots must be data-model independent");
static_assert_expression(__alignof__(struct dos_process_runtime_snapshot) == 8,
			 "process runtime-snapshot alignment changed");
static_assert_expression(__builtin_offsetof(struct dos_process_runtime_snapshot,
					    runtime_identity) == 8,
			 "process runtime-snapshot identity offset changed");
static_assert_expression(__builtin_offsetof(struct dos_process_runtime_snapshot,
					    current_psp) == 20,
			 "process runtime-snapshot PSP offset changed");

/*
 * Construct establishes the C object without inspecting prior bytes.
 * Initialize may then run exactly once in that lifetime with a unique,
 * generation-pinned identity; it cannot clear poison or reset a generation.
 * Reusing storage requires a deliberate new construct and a new identity only
 * after every snapshot from the old lifetime has been drained under the
 * caller's DOS-state lock.
 */
enum dos_process_runtime_status
dos_process_runtime_construct(struct dos_process_runtime *runtime) __must_check;
enum dos_process_runtime_status dos_process_runtime_initialize(
    struct dos_process_runtime *runtime, kernel_object_handle_t identity,
    uint16_t current_psp, struct dos_far_pointer16 dta) __must_check;

/* Output is unchanged on error. */
enum dos_process_runtime_status dos_process_runtime_snapshot(
    const struct dos_process_runtime *runtime,
    struct dos_process_runtime_snapshot *snapshot) __must_check;

/*
 * MS-DOS accepts every 16-bit PSP value, including zero. Updating this
 * value invalidates prepared EXEC snapshots but deliberately leaves DMAADD
 * unchanged, exactly as INT 21h/AH=50h does.
 */
enum dos_process_runtime_status
dos_process_runtime_set_current_psp(struct dos_process_runtime *runtime,
				    uint16_t current_psp) __must_check;

/* DMA address changes do not modify CurrentPDB. */
enum dos_process_runtime_status
dos_process_runtime_set_dta(struct dos_process_runtime *runtime,
			    struct dos_far_pointer16 dta) __must_check;

/*
 * Pure final-seal preflight.  It verifies the expected parent snapshot and
 * generation capacity without changing CurrentPDB or DMAADD.  Its success is
 * meaningful only while the caller retains the same exclusive DOS-state and
 * EXEC observation ownership through dos_process_runtime_publish_exec().
 */
enum dos_process_runtime_status dos_process_runtime_preflight_exec(
    const struct dos_process_runtime *runtime,
    const struct dos_process_runtime_snapshot *expected) __must_check;

/*
 * Final no-callback EXEC publication.  expected must still describe the
 * current parent state.  DTA becomes child:0080h first; CurrentPDB is the last
 * DOS-visible field written.  The caller supplies the serialization and
 * release barrier required by all DOS-state readers.
 */
enum dos_process_runtime_status dos_process_runtime_publish_exec(
    struct dos_process_runtime *runtime,
    const struct dos_process_runtime_snapshot *expected,
    uint16_t child_psp) __must_check;

/*
 * MS-DOS-ordered termination restores the exact pre-EXEC parent globals.
 * expected_child must describe the current child generation; parent may be an
 * older snapshot but must belong to the same constructed runtime lifetime.
 */
enum dos_process_runtime_status dos_process_runtime_restore_parent(
	struct dos_process_runtime *runtime,
	const struct dos_process_runtime_snapshot *expected_child,
	const struct dos_process_runtime_snapshot *parent) __must_check;

/* Sticky quarantine after an uncertain enclosing transaction rollback. */
enum dos_process_runtime_status
dos_process_runtime_poison(struct dos_process_runtime *runtime) __must_check;

#endif

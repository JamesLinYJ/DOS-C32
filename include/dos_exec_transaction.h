/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Typed ownership coordinator for the common DOS EXEC prefix.
 *
 * This layer implements DStrLen-style guest-name acquisition, OPEN, IOCTL
 * device probing, the ordered EXEC0/1 environment selection, and the
 * ordered environment scan/allocation/build lifetime, private header
 * classification, process load-block selection/allocation, resident bytes,
 * MZ relocation, SFT/JFT inheritance, child PSP staging, late-bound default
 * AX finalization, journaled PSP/stack/MCB/INT22/EXEC1 writes, the EXEC1
 * publication seal, and EXEC0's backend-neutral entry value.  EXEC0 backend
 * binding/activation, EXEC3 resident composition and termination still remain;
 * this does not claim that full EXEC is complete.
 *
 * The C coordinator acquires the observation owner after validating AL and
 * before OPEN.  That is an invisible safety barrier around callback-driven C,
 * not a change to the DOS-visible OPEN -> IOCTL order.  No low-level lock is
 * held across those callbacks.
 */
#ifndef DOSC32_DOS_EXEC_TRANSACTION_H
#define DOSC32_DOS_EXEC_TRANSACTION_H

#include "compiler.h"
#include "dos_abi.h"
#include "dos_environment.h"
#include "dos_exec_file_lease.h"
#include "dos_exec_backend_session.h"
#include "dos_exec_handoff.h"
#include "dos_exec_journal.h"
#include "dos_exec_name.h"
#include "dos_exec_observer.h"
#include "dos_exec_parameter.h"
#include "dos_image_load.h"
#include "dos_machine.h"
#include "dos_memory_lease.h"
#include "dos_process_runtime.h"
#include "dos_relocator.h"
#include "types.h"

#define DOS_EXEC_TRANSACTION_SLOT_COUNT 4u
#define DOS_EXEC_TRANSACTION_SLOT_BITS 3u
#define DOS_EXEC_TRANSACTION_SLOT_MASK 0x07ull
#define DOS_EXEC_TRANSACTION_GENERATION_MAX 0x1fffffffffffffffull

enum dos_exec_transaction_state {
	DOS_EXEC_TRANSACTION_STATE_VACANT = 0,
	DOS_EXEC_TRANSACTION_STATE_OBSERVED = 1,
	DOS_EXEC_TRANSACTION_STATE_FILE_OPEN = 2,
	DOS_EXEC_TRANSACTION_STATE_FILE_PROBED = 3,
	DOS_EXEC_TRANSACTION_STATE_ENV_READY = 4,
	/* Private COM/MZ header classification is implemented at IMAGE_READY. */
	DOS_EXEC_TRANSACTION_STATE_IMAGE_READY = 5,
	/* MS-DOS-ordered publication states; implemented as noted below. */
	DOS_EXEC_TRANSACTION_STATE_TARGET_READY = 6,
	/* Resident bytes are private and MZ relocations have completed. */
	DOS_EXEC_TRANSACTION_STATE_IMAGE_PRIVATE = 7,
	DOS_EXEC_TRANSACTION_STATE_SFT_RESERVED = 8,
	DOS_EXEC_TRANSACTION_STATE_PSP_PREPARED = 9,
	DOS_EXEC_TRANSACTION_STATE_GLOBAL_STAGED = 10,
	DOS_EXEC_TRANSACTION_STATE_FILE_CLOSED = 11,
	DOS_EXEC_TRANSACTION_STATE_COMMITTING = 12,
	DOS_EXEC_TRANSACTION_STATE_PUBLISHED = 13,
	DOS_EXEC_TRANSACTION_STATE_FAILED = 14,
	DOS_EXEC_TRANSACTION_STATE_ABORTING = 15,
	DOS_EXEC_TRANSACTION_STATE_ABORTED = 16,
	DOS_EXEC_TRANSACTION_STATE_POISONED = 17,
	/* Callback-in-flight states reject every reentrant public operation. */
	DOS_EXEC_TRANSACTION_STATE_OBSERVER_ACQUIRING = 18,
	DOS_EXEC_TRANSACTION_STATE_FILE_OPENING = 19,
	DOS_EXEC_TRANSACTION_STATE_FILE_PROBING = 20,
	DOS_EXEC_TRANSACTION_STATE_FILE_CLOSING = 21,
	DOS_EXEC_TRANSACTION_STATE_ABORT_FILE_CLOSING = 22,
	DOS_EXEC_TRANSACTION_STATE_OBSERVER_RELEASING = 23,
	DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_READING = 24,
	DOS_EXEC_TRANSACTION_STATE_NAME_READING = 25,
	DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_SCANNING = 26,
	DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_PLANNED = 27,
	DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_ALLOCATING = 28,
	DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_LEASED = 29,
	DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_BUILDING = 30,
	DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_BLOCK_READY = 31,
	DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_LEASE_ABORTING = 32,
	DOS_EXEC_TRANSACTION_STATE_IMAGE_READING = 33,
	DOS_EXEC_TRANSACTION_STATE_LOAD_QUERYING = 34,
	DOS_EXEC_TRANSACTION_STATE_LOAD_PLANNED = 35,
	DOS_EXEC_TRANSACTION_STATE_LOAD_ALLOCATING = 36,
	DOS_EXEC_TRANSACTION_STATE_LOAD_LEASE_ABORTING = 37,
	DOS_EXEC_TRANSACTION_STATE_LOAD_LEASED = 38,
	/* Appended without renumbering the published common-prefix states. */
	DOS_EXEC_TRANSACTION_STATE_PROCESS_PLANNED = 39,
	DOS_EXEC_TRANSACTION_STATE_RESIDENT_LOADING = 40,
	DOS_EXEC_TRANSACTION_STATE_RESIDENT_READY = 41,
	DOS_EXEC_TRANSACTION_STATE_RELOCATION_PLANNED = 42,
	DOS_EXEC_TRANSACTION_STATE_RELOCATING = 43,
	DOS_EXEC_TRANSACTION_STATE_PROCESS_FILE_CLOSING = 44,
	DOS_EXEC_TRANSACTION_STATE_PROCESS_FILE_CLOSED = 45,
	DOS_EXEC_TRANSACTION_STATE_PARENT_SNAPSHOTTING = 46,
	DOS_EXEC_TRANSACTION_STATE_PARENT_SNAPSHOT_READY = 47,
	DOS_EXEC_TRANSACTION_STATE_SFT_PREPARING = 48,
	DOS_EXEC_TRANSACTION_STATE_SFT_ABORTING = 49,
	DOS_EXEC_TRANSACTION_STATE_PROCESS_PARAMETER_READING = 50,
	DOS_EXEC_TRANSACTION_STATE_PSP_PREPARING = 51,
	DOS_EXEC_TRANSACTION_STATE_DRIVE_RESOLVING = 52,
	DOS_EXEC_TRANSACTION_STATE_INITIAL_STATE_READY = 53,
	DOS_EXEC_TRANSACTION_STATE_PROCESS_MEMORY_STAGING = 54,
	DOS_EXEC_TRANSACTION_STATE_JOURNAL_ABORTING = 55,
	DOS_EXEC_TRANSACTION_STATE_GLOBAL_MEMORY_STAGING = 56,
	DOS_EXEC_TRANSACTION_STATE_GLOBAL_MEMORY_READY = 57,
	DOS_EXEC_TRANSACTION_STATE_HANDOFF_STAGING = 58,
	DOS_EXEC_TRANSACTION_STATE_HANDOFF_READY = 59,
	DOS_EXEC_TRANSACTION_STATE_BACKEND_PREPARING = 60,
	DOS_EXEC_TRANSACTION_STATE_BACKEND_DORMANT = 61,
	DOS_EXEC_TRANSACTION_STATE_BACKEND_RELEASING = 62
};

enum dos_exec_transaction_status {
	DOS_EXEC_TRANSACTION_OK = 0,
	DOS_EXEC_TRANSACTION_INVALID_ARGUMENT,
	DOS_EXEC_TRANSACTION_NOT_INITIALIZED,
	DOS_EXEC_TRANSACTION_INVALID_STATE,
	DOS_EXEC_TRANSACTION_BUSY,
	DOS_EXEC_TRANSACTION_NO_SLOT,
	DOS_EXEC_TRANSACTION_GENERATION_EXHAUSTED,
	DOS_EXEC_TRANSACTION_STALE_HANDLE,
	DOS_EXEC_TRANSACTION_BINDING_MISMATCH,
	DOS_EXEC_TRANSACTION_RUNTIME_NOT_READY,
	DOS_EXEC_TRANSACTION_OBSERVER_BUSY,
	DOS_EXEC_TRANSACTION_OBSERVER_FAULT,
	DOS_EXEC_TRANSACTION_FILE_LEASE_UNAVAILABLE,
	DOS_EXEC_TRANSACTION_FILE_LEASE_FAULT,
	DOS_EXEC_TRANSACTION_OPEN_FAILED,
	DOS_EXEC_TRANSACTION_PROBE_FAILED,
	DOS_EXEC_TRANSACTION_IS_DEVICE,
	DOS_EXEC_TRANSACTION_ENVIRONMENT_FAULT,
	DOS_EXEC_TRANSACTION_CLOSE_RETAINED,
	DOS_EXEC_TRANSACTION_POISONED,
	/* Appended to preserve all previously published numeric statuses. */
	DOS_EXEC_TRANSACTION_NAME_FAULT,
	DOS_EXEC_TRANSACTION_BAD_ENVIRONMENT,
	DOS_EXEC_TRANSACTION_ENVIRONMENT_RANGE_OVERFLOW,
	DOS_EXEC_TRANSACTION_MEMORY_LEASE_UNAVAILABLE,
	DOS_EXEC_TRANSACTION_NOT_ENOUGH_MEMORY,
	DOS_EXEC_TRANSACTION_MEMORY_FAULT,
	DOS_EXEC_TRANSACTION_MEMORY_LEASE_RETAINED,
	DOS_EXEC_TRANSACTION_BAD_IMAGE,
	DOS_EXEC_TRANSACTION_IMAGE_TOO_LARGE,
	DOS_EXEC_TRANSACTION_SFT_UNAVAILABLE,
	DOS_EXEC_TRANSACTION_SFT_FAULT,
	DOS_EXEC_TRANSACTION_DRIVE_FAULT,
	DOS_EXEC_TRANSACTION_PUBLICATION_NOT_READY,
	DOS_EXEC_TRANSACTION_BACKEND_UNAVAILABLE,
	DOS_EXEC_TRANSACTION_BACKEND_RETAINED,
	DOS_EXEC_TRANSACTION_BACKEND_POISONED
};

/*
 * Read-only GetVisDrv boundary.  A drive designator is the FCB byte exactly as
 * supplied by DOS (zero means the current drive, one means A:).  INVALID is a
 * normal DOS result used to form FFh in the child's default AX; FAULT reports
 * an implementation/backend failure and is never confused with bad media.
 */
enum dos_exec_drive_visibility_status {
	DOS_EXEC_DRIVE_VISIBLE = 0,
	DOS_EXEC_DRIVE_INVALID,
	DOS_EXEC_DRIVE_FAULT
};

struct dos_exec_drive_visibility_ops {
	kernel_object_handle_t identity;
	enum dos_exec_drive_visibility_status (*resolve)(
	    kernel_object_handle_t context, uint8_t drive_designator);
};

struct dos_exec_transaction_handle {
	uint64_t value;
} __aligned(8);

/* Decoded guest request values.  Segment zero remains a valid DOS value. */
struct dos_exec_transaction_request {
	struct dos_far_pointer16 executable_name;
	struct dos_far_pointer16 parameter_block;
	uint8_t subfunction;
	uint8_t reserved[7];
} __aligned(8);

/*
 * Fixed-width result of the environment preparation interval. A
 * nonzero has_block owns one ACTIVE, unpublished memory lease.  An all-zero
 * value represents the MS-DOS no-environment path. No native pointer,
 * size_t, enum or bool crosses this boundary.
 */
struct dos_exec_transaction_environment {
	struct dos_environment_plan plan;
	struct dos_memory_lease_receipt lease;
	uint8_t has_block;
	uint8_t reserved[7];
} __aligned(8);

/* Fixed-width publication wrapper for the private COM/MZ header result. */
struct dos_exec_transaction_image {
	struct dos_load_plan plan;
	uint8_t has_plan;
	uint8_t reserved[7];
} __aligned(8);

/*
 * Process-only result of target allocation. The MCB
 * remains parent-owned and ACTIVE until the later PSP publication seal.  The
 * separate flag means guest segment zero is never treated as a native NULL.
 */
struct dos_exec_transaction_target {
	struct dos_process_allocation_plan allocation;
	struct dos_memory_lease_receipt lease;
	uint8_t has_load_block;
	uint8_t reserved[7];
} __aligned(8);

/*
 * A process plan and its resident-load receipt remain private to the
 * parent-owned MCB.  The union is tagged explicitly; neither C enum values nor
 * native pointers cross the persistent transaction boundary.  Initial AX is
 * zero through PSP_PREPARED, then becomes the late-bound GetVisDrv result in
 * INITIAL_STATE_READY.
 */
union dos_exec_transaction_process_plan {
	struct dos_com_process_plan com;
	struct dos_mz_process_plan mz;
	uint8_t bytes[104];
};

struct dos_exec_transaction_resident {
	union dos_exec_transaction_process_plan process;
	struct dos_image_load_result load;
	uint8_t format;
	uint8_t has_process_plan;
	uint8_t has_resident;
	uint8_t reserved[5];
} __aligned(8);

/* MZ-only relocation receipt.  COM completes this phase with all zeros. */
struct dos_exec_transaction_relocation {
	struct dos_relocator_request request;
	struct dos_relocator_result result;
	uint8_t applicable;
	uint8_t has_request;
	uint8_t applied;
	uint8_t reserved[5];
} __aligned(8);

/* Immutable `$Dup_PDB` parent snapshot captured after executable CLOSE. */
struct dos_exec_transaction_parent {
	struct dos_process_parent_snapshot snapshot;
	uint8_t has_snapshot;
	uint8_t reserved[7];
} __aligned(8);

struct dos_exec_transaction_inheritance {
	dos_sft_batch_handle_t batch;
	struct dos_sft_jft20 child_jft;
	uint8_t has_batch;
	uint8_t has_child_jft;
	uint8_t reserved[6];
} __aligned(8);

struct dos_exec_transaction_psp {
	struct dos_process_psp_image image;
	uint8_t has_image;
	uint8_t reserved[5];
} __aligned(8);

/*
 * Captured values needed after OPEN and at the final EXEC seal.  The
 * owner-name patch is captured with the executable name so later mutable
 * guest bytes cannot redirect MCB naming.  Rebind plans remain absent until
 * both complete MCB replacements have been journaled successfully.
 */
struct dos_exec_transaction_publication {
	struct dos_memory_owner_name_patch owner_name;
	struct dos_memory_lease_rebind_plan environment_rebind;
	struct dos_memory_lease_rebind_plan load_rebind;
	struct dos_exec_handoff_plan handoff;
	struct dos_exec_backend_session_handle backend_session;
	kernel_object_handle_t backend_session_table_identity;
	kernel_object_handle_t backend_adapter_identity;
	kernel_object_handle_t backend_adapter_context;
	uint8_t has_owner_name;
	uint8_t has_environment_rebind;
	uint8_t has_load_rebind;
	uint8_t has_handoff;
	uint8_t has_backend_session;
	uint8_t reserved[3];
} __aligned(8);

/*
 * Persistent slot: fixed-width values and generation handles only.  It never
 * owns an ops pointer, native pointer, size_t, C enum or bool.  The adapter
 * identities name generation-pinned lifetimes; contexts are opaque integers,
 * not addresses.  observer is itself a fixed-width, pointer-free value object.
 */
struct dos_exec_transaction_slot {
	uint64_t generation;
	kernel_object_handle_t coordinator_identity;
	kernel_object_handle_t machine_identity;
	kernel_object_handle_t machine_context;
	uint64_t machine_address_limit;
	kernel_object_handle_t file_adapter_identity;
	kernel_object_handle_t file_adapter_context;
	kernel_object_handle_t file_lease_table_identity;
	kernel_object_handle_t runtime_identity;
	kernel_object_handle_t sft_adapter_identity;
	kernel_object_handle_t sft_adapter_context;
	kernel_object_handle_t observer_adapter_identity;
	kernel_object_handle_t observer_adapter_context;
	struct dos_process_runtime_snapshot parent_runtime;
	struct dos_exec_environment_source_plan environment_source;
	struct dos_exec_name_plan executable_name;
	struct dos_exec_file_lease_handle file_lease;
	struct dos_exec_transaction_request request;
	struct dos_exec_observer observer;
	uint8_t state;
	uint8_t has_file_lease;
	uint8_t machine_a20_enabled;
	uint8_t reserved[5];
	kernel_object_handle_t memory_arena_identity;
	uint64_t memory_arena_generation;
	struct dos_exec_transaction_environment environment;
	dos_memory_lease_table_identity_t memory_lease_table_identity;
	uint16_t memory_arena_head_segment;
	uint8_t reserved_extension[2];
	struct dos_exec_transaction_image image;
	struct dos_exec_transaction_target target;
	struct dos_exec_transaction_resident resident;
	struct dos_exec_transaction_relocation relocation;
	struct dos_exec_transaction_parent parent;
	struct dos_exec_transaction_inheritance inheritance;
	struct dos_exec_transaction_psp psp;
	kernel_object_handle_t drive_adapter_identity;
	kernel_object_handle_t drive_adapter_context;
	struct dos_exec_transaction_publication publication;
	struct dos_exec_journal journal;
} __aligned(8);

struct dos_exec_transaction_table {
	struct dos_exec_transaction_slot slots[DOS_EXEC_TRANSACTION_SLOT_COUNT];
	kernel_object_handle_t coordinator_identity;
	uint8_t initialized;
	uint8_t constructed;
	uint8_t poisoned;
	uint8_t reserved[5];
} __aligned(8);

#define DOS_EXEC_TRANSACTION_TABLE_INITIALIZER                                 \
	{.slots = {{0}},                                                       \
	 .coordinator_identity = KERNEL_OBJECT_HANDLE_INVALID,                 \
	 .initialized = 0u,                                                    \
	 .constructed = 1u,                                                    \
	 .poisoned = 0u,                                                       \
	 .reserved = {0u}}

/*
 * Borrowed for one call; no pointer below is copied into a transaction slot.
 * runtime, sft_* and drive_* are process-only (AL=0/1).  They may be absent
 * for AL=3; file_* still pins the caller's DOS JFN/SFT namespace for that
 * overlay.
 */
struct dos_exec_transaction_services {
	struct dos_exec_file_lease_table *file_leases;
	const struct dos_exec_file_lease_ops *file_ops;
	const struct dos_exec_observer_ops *observer_ops;
	const struct dos_sft_batch_ops *sft_ops;
	const struct dos_exec_drive_visibility_ops *drive_ops;
	struct dos_process_runtime *runtime;
	const struct dos_machine *machine;
	struct dos_memory_lease_table *memory_leases;
	struct dos_memory_arena *memory_arena;
	struct dos_exec_backend_session_table *backend_sessions;
	const struct dos_exec_backend_ops *backend_ops;
	kernel_object_handle_t coordinator_identity;
	kernel_object_handle_t machine_identity;
	kernel_object_handle_t file_lease_table_identity;
	kernel_object_handle_t file_adapter_context;
	kernel_object_handle_t sft_adapter_identity;
	kernel_object_handle_t sft_adapter_context;
	kernel_object_handle_t drive_adapter_identity;
	kernel_object_handle_t drive_adapter_context;
	kernel_object_handle_t observer_adapter_context;
	kernel_object_handle_t backend_session_table_identity;
	kernel_object_handle_t backend_adapter_context;
	dos_memory_lease_table_identity_t memory_lease_table_identity;
};

static_assert_expression(sizeof(struct dos_exec_transaction_handle) == 8,
			 "EXEC transaction handles must remain 64-bit");
static_assert_expression(__alignof__(struct dos_exec_transaction_handle) == 8,
			 "EXEC transaction handle alignment changed");
static_assert_expression(
    sizeof(struct dos_exec_transaction_request) == 16,
    "EXEC transaction requests must be data-model independent");
static_assert_expression(__alignof__(struct dos_exec_transaction_request) == 8,
			 "EXEC transaction request alignment changed");
static_assert_expression(__builtin_offsetof(struct dos_exec_transaction_request,
					    subfunction) == 8,
			 "EXEC transaction subfunction offset changed");
static_assert_expression(
    sizeof(struct dos_exec_transaction_environment) == 56,
    "EXEC transaction environments must be data-model independent");
static_assert_expression(
    __alignof__(struct dos_exec_transaction_environment) == 8,
    "EXEC transaction environment alignment changed");
static_assert_expression(
    __builtin_offsetof(struct dos_exec_transaction_environment, lease) == 32,
    "EXEC transaction environment lease offset changed");
static_assert_expression(
    __builtin_offsetof(struct dos_exec_transaction_environment, has_block) ==
	48,
    "EXEC transaction environment flag offset changed");
static_assert_expression(sizeof(struct dos_exec_transaction_image) == 64,
			 "EXEC transaction image must be fixed width");
static_assert_expression(__alignof__(struct dos_exec_transaction_image) == 8,
			 "EXEC transaction image alignment changed");
static_assert_expression(
    __builtin_offsetof(struct dos_exec_transaction_image, has_plan) == 56,
    "EXEC transaction image flag offset changed");
static_assert_expression(sizeof(struct dos_exec_transaction_target) == 32,
			 "EXEC transaction target must be fixed width");
static_assert_expression(__alignof__(struct dos_exec_transaction_target) == 8,
			 "EXEC transaction target alignment changed");
static_assert_expression(
    __builtin_offsetof(struct dos_exec_transaction_target, lease) == 8,
    "EXEC transaction target lease offset changed");
static_assert_expression(
    __builtin_offsetof(struct dos_exec_transaction_target, has_load_block) ==
	24,
    "EXEC transaction target flag offset changed");
static_assert_expression(
    sizeof(union dos_exec_transaction_process_plan) == 104,
    "EXEC transaction process union must be data-model independent");
static_assert_expression(sizeof(struct dos_exec_transaction_resident) == 136,
			 "EXEC transaction resident must be fixed width");
static_assert_expression(
    __alignof__(struct dos_exec_transaction_resident) == 8,
    "EXEC transaction resident alignment changed");
static_assert_expression(
    __builtin_offsetof(struct dos_exec_transaction_resident, load) == 104,
    "EXEC transaction resident receipt offset changed");
static_assert_expression(
    __builtin_offsetof(struct dos_exec_transaction_resident, format) == 128,
    "EXEC transaction resident tag offset changed");
static_assert_expression(
    sizeof(struct dos_exec_transaction_relocation) == 40,
    "EXEC transaction relocation must be fixed width");
static_assert_expression(
    __alignof__(struct dos_exec_transaction_relocation) == 8,
    "EXEC transaction relocation alignment changed");
static_assert_expression(
    __builtin_offsetof(struct dos_exec_transaction_relocation, applicable) ==
	28,
    "EXEC transaction relocation flags moved");
static_assert_expression(sizeof(struct dos_exec_transaction_parent) == 320,
			 "EXEC transaction parent snapshot must be fixed width");
static_assert_expression(
    __builtin_offsetof(struct dos_exec_transaction_parent, has_snapshot) ==
	312,
    "EXEC transaction parent snapshot flag moved");
static_assert_expression(
    sizeof(struct dos_exec_transaction_inheritance) == 40,
    "EXEC transaction inheritance must be fixed width");
static_assert_expression(
    __builtin_offsetof(struct dos_exec_transaction_inheritance, child_jft) ==
	8,
    "EXEC transaction child JFT moved");
static_assert_expression(sizeof(struct dos_exec_transaction_psp) == 264,
			 "EXEC transaction PSP must be fixed width");
static_assert_expression(
    __builtin_offsetof(struct dos_exec_transaction_psp, has_image) == 258,
    "EXEC transaction PSP flag moved");
static_assert_expression(sizeof(struct dos_exec_transaction_publication) ==
			     256,
			 "EXEC publication values must be fixed width");
static_assert_expression(
    __builtin_offsetof(struct dos_exec_transaction_publication,
			       environment_rebind) == 16,
    "EXEC environment rebind offset changed");
static_assert_expression(
    __builtin_offsetof(struct dos_exec_transaction_publication, load_rebind) ==
	80,
    "EXEC load rebind offset changed");
static_assert_expression(
    __builtin_offsetof(struct dos_exec_transaction_publication, handoff) ==
	144,
    "EXEC handoff offset changed");
static_assert_expression(
	    sizeof(struct dos_exec_transaction_slot) == 2640,
	    "EXEC transaction slots must be data-model independent");
static_assert_expression(__alignof__(struct dos_exec_transaction_slot) == 8,
			 "EXEC transaction slot alignment changed");
static_assert_expression(__builtin_offsetof(struct dos_exec_transaction_slot,
					    file_adapter_identity) == 40,
			 "EXEC transaction file binding offset changed");
static_assert_expression(__builtin_offsetof(struct dos_exec_transaction_slot,
					    file_lease_table_identity) == 56,
			 "EXEC transaction file-table binding offset changed");
static_assert_expression(__builtin_offsetof(struct dos_exec_transaction_slot,
					    runtime_identity) == 64,
			 "EXEC transaction runtime binding offset changed");
static_assert_expression(__builtin_offsetof(struct dos_exec_transaction_slot,
					    parent_runtime) == 104,
			 "EXEC transaction runtime-snapshot offset changed");
static_assert_expression(__builtin_offsetof(struct dos_exec_transaction_slot,
					    environment_source) == 128,
			 "EXEC transaction environment-source offset changed");
static_assert_expression(__builtin_offsetof(struct dos_exec_transaction_slot,
					    executable_name) == 136,
			 "EXEC transaction name-plan offset changed");
static_assert_expression(__builtin_offsetof(struct dos_exec_transaction_slot,
					    file_lease) == 144,
			 "EXEC transaction file-lease offset changed");
static_assert_expression(__builtin_offsetof(struct dos_exec_transaction_slot,
					    request) == 152,
			 "EXEC transaction request offset changed");
static_assert_expression(__builtin_offsetof(struct dos_exec_transaction_slot,
					    observer) == 168,
			 "EXEC transaction observer offset changed");
static_assert_expression(__builtin_offsetof(struct dos_exec_transaction_slot,
					    state) == 200,
			 "EXEC transaction state offset changed");
static_assert_expression(
    __builtin_offsetof(struct dos_exec_transaction_slot,
			       memory_arena_identity) == 208,
    "EXEC transaction arena binding offset changed");
static_assert_expression(
    __builtin_offsetof(struct dos_exec_transaction_slot, environment) == 224,
    "EXEC transaction environment offset changed");
static_assert_expression(
    __builtin_offsetof(struct dos_exec_transaction_slot,
			       memory_lease_table_identity) == 280,
    "EXEC transaction memory-table binding offset changed");
static_assert_expression(
	__builtin_offsetof(struct dos_exec_transaction_slot, image) == 288,
	"EXEC transaction image offset changed");
static_assert_expression(
	__builtin_offsetof(struct dos_exec_transaction_slot, target) == 352,
	"EXEC transaction target offset changed");
static_assert_expression(
	__builtin_offsetof(struct dos_exec_transaction_slot, resident) == 384,
	"EXEC transaction resident offset changed");
static_assert_expression(
	__builtin_offsetof(struct dos_exec_transaction_slot, relocation) == 520,
	"EXEC transaction relocation offset changed");
static_assert_expression(
	__builtin_offsetof(struct dos_exec_transaction_slot, parent) == 560,
	"EXEC transaction parent snapshot offset changed");
static_assert_expression(
	__builtin_offsetof(struct dos_exec_transaction_slot, inheritance) == 880,
	"EXEC transaction inheritance offset changed");
static_assert_expression(
	__builtin_offsetof(struct dos_exec_transaction_slot, psp) == 920,
	"EXEC transaction PSP offset changed");
static_assert_expression(
	__builtin_offsetof(struct dos_exec_transaction_slot,
			     drive_adapter_identity) == 1184,
	"EXEC transaction drive binding offset changed");
static_assert_expression(
	__builtin_offsetof(struct dos_exec_transaction_slot, publication) == 1200,
	"EXEC transaction publication offset changed");
static_assert_expression(
	__builtin_offsetof(struct dos_exec_transaction_slot, journal) == 1456,
	"EXEC transaction journal offset changed");
static_assert_expression(
	    sizeof(struct dos_exec_transaction_table) == 10576,
	    "EXEC transaction tables must be data-model independent");
static_assert_expression(__alignof__(struct dos_exec_transaction_table) == 8,
			 "EXEC transaction table alignment changed");
static_assert_expression(__builtin_offsetof(struct dos_exec_transaction_table,
					    coordinator_identity) == 10560,
			 "EXEC transaction table binding offset changed");
static_assert_expression(
    DOS_EXEC_TRANSACTION_SLOT_COUNT < (1u << DOS_EXEC_TRANSACTION_SLOT_BITS),
    "slot-plus-one handle encoding needs one reserved zero value");

/*
 * Construct starts a new C object lifetime; it is not a reset operation for
 * live handles and cannot be used to clear poison.  Initialize succeeds once.
 * The table has no internal lock.  One outer EXEC executor serializes the
 * table, file lease table, runtime, observer adapter and represented DOS state.
 */
enum dos_exec_transaction_status dos_exec_transaction_table_construct(
    struct dos_exec_transaction_table *table) __must_check;
enum dos_exec_transaction_status dos_exec_transaction_table_initialize(
    struct dos_exec_transaction_table *table,
    kernel_object_handle_t coordinator_identity) __must_check;
bool dos_exec_transaction_table_is_drained(
    const struct dos_exec_transaction_table *table) __must_check;

/*
 * Validate AL, reserve one non-wrapping generation and acquire observation.
 * AL=0/1 then capture CurrentPDB/DTA and bind the child-JFT/SFT inheritance
 * backend. The AL=3 overlay path skips both process-only dependencies;
 * its OPEN adapter still pins the caller's DOS file namespace.  Handle is
 * unchanged on every non-OK result.
 */
enum dos_exec_transaction_status dos_exec_transaction_begin(
    struct dos_exec_transaction_table *table,
    const struct dos_exec_transaction_services *services,
    const struct dos_exec_transaction_request *request,
    struct dos_exec_transaction_handle *handle) __must_check;

/*
 * Preserve the MS-DOS name-length then OPEN order.
 * scratch is caller-owned writable work space; no pointer or size_t survives
 * this call.  The guest name is fetched bytewise before OPEN and the exact
 * including-NUL span is passed to the adapter.  Only a successful OPEN
 * publishes both the fixed-width name plan and the file lease, with FILE_OPEN
 * published last.  Guest faults, unterminated names and insufficient scratch
 * enter FAILED without calling OPEN or retaining a name plan.
 *
 * On OPEN_FAILED, failure_detail receives the adapter's exact value and the
 * transaction enters FAILED.  Every pre-OPEN error and every uncertain lease
 * failure leaves failure_detail unchanged.
 */
enum dos_exec_transaction_status
dos_exec_transaction_open(struct dos_exec_transaction_table *table,
			  struct dos_exec_transaction_handle handle,
			  const struct dos_exec_transaction_services *services,
			  uint8_t *executable_name_scratch,
			  size_t executable_name_scratch_capacity,
			  uint32_t *failure_detail) __must_check;

/*
 * Preserve the separate IOCTL phase. Normal probe failure publishes
 * only failure_detail.  A device publishes is_device=1 and returns IS_DEVICE;
 * both outcomes enter FAILED and require abort.
 */
enum dos_exec_transaction_status
dos_exec_transaction_probe(struct dos_exec_transaction_table *table,
			   struct dos_exec_transaction_handle handle,
			   const struct dos_exec_transaction_services *services,
			   uint8_t *is_device,
			   uint32_t *failure_detail) __must_check;

/*
 * Process-only environment selection after FILE_PROBED.
 * A pure runtime-snapshot preflight runs before either guest read.  The exact
 * parameter-first, parent-on-zero decoder then publishes one fixed-width plan
 * both to the slot and to environment_source.  AL=3 is rejected without a
 * guest read.  environment_source is unchanged on every error.
 */
enum dos_exec_transaction_status dos_exec_transaction_select_environment(
    struct dos_exec_transaction_table *table,
    struct dos_exec_transaction_handle handle,
    const struct dos_exec_transaction_services *services,
    struct dos_exec_environment_source_plan *environment_source) __must_check;

/*
 * Preserve the observable MS-DOS order and block layout: scan the
 * selected environment, calculate its paragraph count, acquire an unnamed
 * parent-owned MCB lease, copy the environment, write little-endian word 1,
 * then re-read/copy argv[0].  Unsafe internal wrap, ambiguous ownership and
 * stale-buffer behavior are rejected rather than reproduced.  The lease
 * remains ACTIVE and unpublished for later image/PSP sealing.  A NONE selection
 * performs no guest read and publishes an all-zero result.  AL=3 is rejected;
 * its next transition is image-header inspection.
 *
 * Runtime and service bindings are preflighted before the first guest callback.
 * result is unchanged on every error.  A failure after allocation retains the
 * lease for dos_exec_transaction_abort(), whose reverse unwind frees it before
 * closing the executable file and releasing observation.
 */
enum dos_exec_transaction_status dos_exec_transaction_prepare_environment(
    struct dos_exec_transaction_table *table,
    struct dos_exec_transaction_handle handle,
    const struct dos_exec_transaction_services *services,
    struct dos_exec_transaction_environment *result) __must_check;

/*
 * Inspect the executable header. EXEC0/1 require a completed
 * environment transition (including NONE); EXEC3 starts directly from
 * FILE_PROBED.  Resolve the transient immutable reader, read at most the
 * private 26-byte header, and publish one pointer-free COM/MZ plan.  result is
 * unchanged on every error.  Header I/O/empty/short failures map to BAD_IMAGE;
 * a COM image at the 16-bit equality-failure limit maps to IMAGE_TOO_LARGE.
 */
enum dos_exec_transaction_status dos_exec_transaction_inspect_image(
    struct dos_exec_transaction_table *table,
    struct dos_exec_transaction_handle handle,
    const struct dos_exec_transaction_services *services,
    struct dos_load_plan *result) __must_check;

/*
 * Process-only target allocation. Query the largest arena block only after environment
 * allocation and image classification, select the exact COM/MZ paragraph
 * count, then acquire that exact parent-owned MCB as an unpublished lease.
 * The allocation plan is retained on an ordinary allocation failure; result
 * remains unchanged on every error.  EXEC3 uses caller-supplied overlay
 * geometry and is rejected here without touching the arena.
 */
enum dos_exec_transaction_status dos_exec_transaction_prepare_target(
    struct dos_exec_transaction_table *table,
    struct dos_exec_transaction_handle handle,
    const struct dos_exec_transaction_services *services,
    struct dos_exec_transaction_target *result) __must_check;

/*
 * Continue from TARGET_READY through the resident read. This builds
 * exact COM/MZ process geometry with a placeholder AX, resolves the active
 * unpublished load-block lease, and copies the file bytes into that private
 * block.  It does not relocate MZ words, build the PSP, prepare the final
 * stack, close the file, or publish the MCB.  Every non-OK result leaves
 * result unchanged and requires abort to discard the whole load block.
 */
enum dos_exec_transaction_status dos_exec_transaction_load_resident(
    struct dos_exec_transaction_table *table,
    struct dos_exec_transaction_handle handle,
    const struct dos_exec_transaction_services *services,
    struct dos_exec_transaction_resident *result) __must_check;

/*
 * Apply the MZ relocation table in two bounded passes. COM performs no
 * file or guest callback and reaches the same IMAGE_PRIVATE state with an
 * all-zero relocation receipt.  MZ targets are revalidated against the exact
 * resident span on both passes.  A failure retains the entire private lease
 * for abort; result is unchanged.
 */
enum dos_exec_transaction_status dos_exec_transaction_relocate_resident(
    struct dos_exec_transaction_table *table,
    struct dos_exec_transaction_handle handle,
    const struct dos_exec_transaction_services *services,
    struct dos_exec_transaction_relocation *result) __must_check;

/*
 * Capture the complete parent PSP and its first twenty effective JFT bytes
 * after the MS-DOS-ordered executable CLOSE. The fixed snapshot is the sole
 * parent input for later `$Dup_PDB` and SFT inheritance; result is unchanged
 * on failure and no native pointer is retained.
 */
enum dos_exec_transaction_status dos_exec_transaction_capture_parent(
    struct dos_exec_transaction_table *table,
    struct dos_exec_transaction_handle handle,
    const struct dos_exec_transaction_services *services,
    struct dos_exec_transaction_parent *result) __must_check;

enum dos_exec_transaction_status dos_exec_transaction_prepare_inheritance(
    struct dos_exec_transaction_table *table,
    struct dos_exec_transaction_handle handle,
    const struct dos_exec_transaction_services *services,
    struct dos_exec_transaction_inheritance *result) __must_check;

enum dos_exec_transaction_status dos_exec_transaction_prepare_psp(
    struct dos_exec_transaction_table *table,
    struct dos_exec_transaction_handle handle,
    const struct dos_exec_transaction_services *services,
    struct dos_process_far_address terminate_vector,
    struct dos_exec_transaction_psp *result) __must_check;

/*
 * Resolve FCB2 and then FCB1 after both FCBs and the raw
 * command tail are in the child PSP.  Resolve those exact staged drive bytes,
 * form BH:BL with 00h/FFh validity bytes, and atomically bind that value into
 * the retained COM/MZ process plan.  No stack or PSP guest write occurs here;
 * result is unchanged on error.
 */
enum dos_exec_transaction_status dos_exec_transaction_finalize_initial_state(
    struct dos_exec_transaction_table *table,
    struct dos_exec_transaction_handle handle,
    const struct dos_exec_transaction_services *services,
    struct dos_exec_transaction_resident *result) __must_check;

/*
 * Journal the staged 256-byte PSP in four cache-line-sized far writes, then
 * perform
 * only the stack-memory writes that EXEC itself performs before publication:
 * COM's zero sentinel and, for EXEC1, the default AX word.  MZ EXEC0 leaves
 * its PUSH/PUSH for the backend handoff.  Every ordinary failure restores all
 * prior records before returning; no lease/SFT/runtime publication occurs.
 */
enum dos_exec_transaction_status dos_exec_transaction_stage_process_memory(
    struct dos_exec_transaction_table *table,
    struct dos_exec_transaction_handle handle,
    const struct dos_exec_transaction_services *services) __must_check;

/*
 * Prepare and journal the environment MCB owner replacement (when present),
 * then the load MCB owner/name replacement, and finally the EXEC1 return
 * tuple.  Native lease owners remain the parent until the no-callback seal;
 * any failure restores the complete PSP/stack/MCB/result journal.
 */
enum dos_exec_transaction_status dos_exec_transaction_stage_global_memory(
    struct dos_exec_transaction_table *table,
    struct dos_exec_transaction_handle handle,
    const struct dos_exec_transaction_services *services) __must_check;

/*
 * EXEC0 only: derive the fixed-width entry value from the retained COM/MZ
 * process plan and atomically journal exec_go's PUSH CS/PUSH IP footprint.
 * No backend callback or guest instruction runs here.  Result is unchanged on
 * failure; a successful value can be handed to any guest execution backend.
 */
enum dos_exec_transaction_status dos_exec_transaction_prepare_handoff(
    struct dos_exec_transaction_table *table,
    struct dos_exec_transaction_handle handle,
    const struct dos_exec_transaction_services *services,
    struct dos_exec_handoff_plan *result) __must_check;

/*
 * Fallibly prepare one dormant guest execution session.  The backend cannot
 * execute guest code in this phase.  A normal adapter rejection returns its
 * opaque detail and leaves the transaction at HANDOFF_READY; success stores a
 * generation-bound session and reaches BACKEND_DORMANT.
 */
enum dos_exec_transaction_status dos_exec_transaction_prepare_backend(
    struct dos_exec_transaction_table *table,
    struct dos_exec_transaction_handle handle,
    const struct dos_exec_transaction_services *services,
    uint32_t *failure_detail) __must_check;

/*
 * Pure-preflight the dormant backend and every DOS publication object, commit
 * the DOS seal, then make the session RUNNABLE with no callback.  No guest
 * instruction executes in this call.  session is unchanged on failure.
 */
enum dos_exec_transaction_status dos_exec_transaction_seal_execute(
    struct dos_exec_transaction_table *table,
    struct dos_exec_transaction_handle handle,
    const struct dos_exec_transaction_services *services,
    struct dos_exec_backend_session_handle *session) __must_check;

/* Complete the callback-free EXEC1 cross-object seal and release observation. */
enum dos_exec_transaction_status dos_exec_transaction_seal_load_only(
    struct dos_exec_transaction_table *table,
    struct dos_exec_transaction_handle handle,
    const struct dos_exec_transaction_services *services) __must_check;

/*
 * Preflight and recycle only coordinator-local CLOSED/COMMITTED handles after
 * successful publication.  Published MCB/SFT ownership remains with the
 * child; no guest, file, device, or observer callback occurs.
 */
enum dos_exec_transaction_status dos_exec_transaction_retire_published(
    struct dos_exec_transaction_table *table,
    struct dos_exec_transaction_handle handle,
    const struct dos_exec_transaction_services *services) __must_check;

/*
 * EXEC3 may close from FILE_PROBED and reaches FILE_CLOSED.  EXEC0/1 preserve
 * post-relocation order by closing from IMAGE_PRIVATE and reaching
 * PROCESS_FILE_CLOSED before any PSP/JFT work.  RETAINED enters FAILED for
 * abort retry.  The closed generation handle remains owned until abort or the
 * eventual successful publication path retires it; no second adapter CLOSE is
 * issued for an already closed lease.
 */
enum dos_exec_transaction_status dos_exec_transaction_close(
    struct dos_exec_transaction_table *table,
    struct dos_exec_transaction_handle handle,
    const struct dos_exec_transaction_services *services) __must_check;

/*
 * Idempotent reverse unwind: discard the unpublished load lease, then the
 * environment lease, close/retire the file lease, and finally release the
 * observation owner.  A retained memory or file lease leaves ABORTING so a
 * later abort retries.
 */
enum dos_exec_transaction_status dos_exec_transaction_abort(
    struct dos_exec_transaction_table *table,
    struct dos_exec_transaction_handle handle,
    const struct dos_exec_transaction_services *services) __must_check;

/* Recycle only ABORTED.  The next begin increments the slot generation. */
enum dos_exec_transaction_status dos_exec_transaction_retire(
    struct dos_exec_transaction_table *table,
    kernel_object_handle_t coordinator_identity,
    struct dos_exec_transaction_handle handle) __must_check;

#endif

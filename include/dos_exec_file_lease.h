/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Generation-checked executable-file ownership for DOS EXEC.
 *
 * The MS-DOS operation order is OPEN, IOCTL device classification, immutable
 * reads, and
 * CLOSE.  DOS-visible errors remain the responsibility of the enclosing EXEC
 * transaction.  This boundary adds only bounded native spans, explicit
 * ownership, non-wrapping generations, and fail-closed uncertainty; it does
 * does not add unrelated errno, namespace, path, or process semantics.
 */
#ifndef DOSC32_DOS_EXEC_FILE_LEASE_H
#define DOSC32_DOS_EXEC_FILE_LEASE_H

#include "compiler.h"
#include "dos_loader.h"
#include "types.h"

#define DOS_EXEC_FILE_LEASE_SLOT_COUNT 16u
#define DOS_EXEC_FILE_LEASE_SLOT_BITS 5u
#define DOS_EXEC_FILE_LEASE_SLOT_MASK 0x1full
#define DOS_EXEC_FILE_LEASE_GENERATION_MAX 0x07ffffffffffffffull

enum dos_exec_file_adapter_status {
	DOS_EXEC_FILE_ADAPTER_OK = 0,
	/* Operation did not complete and acquired no new ownership. */
	DOS_EXEC_FILE_ADAPTER_FAULT = 1
};

/* The result describes resource ownership, not the DOS CLOSE return value. */
enum dos_exec_file_close_result {
	/* Ownership is certainly released, including an ordinary DOS close
	   error. */
	DOS_EXEC_FILE_CLOSE_CLOSED = 0,
	/* Ownership is certainly retained and close may be retried. */
	DOS_EXEC_FILE_CLOSE_RETAINED = 1,
	/* Ownership cannot be proved either retained or released. */
	DOS_EXEC_FILE_CLOSE_UNCERTAIN = 2
};

struct dos_exec_file_open_result {
	kernel_object_handle_t reader_context;
	file_offset_t size;
	/* Opaque to the lease; meaningful when open returns non-OK. */
	uint32_t failure_detail;
	uint32_t reserved;
} __aligned(8);

/* Fixed-width IOCTL classification result; failure_detail is adapter-opaque. */
struct dos_exec_file_probe_result {
	uint32_t failure_detail;
	uint8_t is_device;
	uint8_t reserved[3];
} __aligned(8);

/*
 * ops and every native pointer below are borrowed only for one public call and
 * are never retained in a lease slot.  identity is a generation-pinned
 * registry identity and must be neither zero nor KERNEL_OBJECT_HANDLE_INVALID.
 *
 * open receives an exact byte span; no terminator or host path convention is
 * implied.  Exact DOS_EXEC_FILE_ADAPTER_FAULT guarantees that OPEN acquired no
 * resource and returns one fixed-width opaque failure_detail.  Any other
 * non-OK encoding violates the contract and cannot prove whether ownership
 * was acquired.  OK returns a reader_context that is neither zero nor invalid
 * and one immutable 64-bit size; its detail and reserved fields are zero.  The
 * lease never interprets failure_detail.  A future DOS $OPEN adapter defines
 * its low 16 bits from the adapter's DOS error result, while the transaction owns
 * the actual DOS-visible mapping.
 *
 * probe_device runs only after OPEN.  Its successful result has zero detail,
 * zero reserved bytes, and is_device exactly zero or one.  Exact
 * DOS_EXEC_FILE_ADAPTER_FAULT retains the open resource, leaves all reserved
 * bytes zero, and carries one opaque failure_detail.  Any other non-OK
 * encoding violates the contract and poisons ownership.  A future DOS $IOCTL
 * adapter places its DOS error in the low 16 bits.
 * read has precisely the dos_image_reader callback contract and is positional
 * rather than dependent on shared seek state.  From successful OPEN through
 * certain CLOSE, the adapter pins the same size and complete byte snapshot:
 * same-sized file replacement must not change later loader or two-pass
 * relocator reads.  Each successful OPEN owns one independently closeable
 * reference.  The 64-bit reader_context is itself generation-pinned and never
 * ABA-reused: after a certain close an old resolved read must fail, and a later
 * OPEN must not make that old integer refer to a new file.
 */
struct dos_exec_file_lease_ops {
	kernel_object_handle_t identity;
	enum dos_exec_file_adapter_status (*open)(
	    kernel_object_handle_t context, const uint8_t *path,
	    size_t path_length, struct dos_exec_file_open_result *result);
	enum dos_exec_file_adapter_status (*probe_device)(
	    kernel_object_handle_t context,
	    kernel_object_handle_t reader_context,
	    struct dos_exec_file_probe_result *result);
	enum dos_image_read_status (*read)(
	    kernel_object_handle_t reader_context, file_offset_t offset,
	    void *destination, size_t destination_capacity, size_t count,
	    size_t *bytes_read);
	enum dos_exec_file_close_result (*close)(
	    kernel_object_handle_t context,
	    kernel_object_handle_t reader_context);
};

enum dos_exec_file_lease_state {
	DOS_EXEC_FILE_LEASE_STATE_VACANT = 0,
	DOS_EXEC_FILE_LEASE_STATE_OPENING = 1,
	DOS_EXEC_FILE_LEASE_STATE_OPEN = 2,
	DOS_EXEC_FILE_LEASE_STATE_PROBING = 3,
	DOS_EXEC_FILE_LEASE_STATE_PROBED = 4,
	DOS_EXEC_FILE_LEASE_STATE_CLOSING = 5,
	DOS_EXEC_FILE_LEASE_STATE_CLOSED = 6,
	DOS_EXEC_FILE_LEASE_STATE_POISONED = 7
};

enum dos_exec_file_lease_status {
	DOS_EXEC_FILE_LEASE_OK = 0,
	DOS_EXEC_FILE_LEASE_INVALID_ARGUMENT = 1,
	DOS_EXEC_FILE_LEASE_NOT_INITIALIZED = 2,
	DOS_EXEC_FILE_LEASE_INVALID_STATE = 3,
	DOS_EXEC_FILE_LEASE_NO_SLOT = 4,
	DOS_EXEC_FILE_LEASE_GENERATION_EXHAUSTED = 5,
	DOS_EXEC_FILE_LEASE_STALE_HANDLE = 6,
	DOS_EXEC_FILE_LEASE_IDENTITY_MISMATCH = 7,
	DOS_EXEC_FILE_LEASE_CONTEXT_MISMATCH = 8,
	DOS_EXEC_FILE_LEASE_OPEN_FAILED = 9,
	DOS_EXEC_FILE_LEASE_PROBE_FAILED = 10,
	DOS_EXEC_FILE_LEASE_IS_DEVICE = 11,
	DOS_EXEC_FILE_LEASE_CLOSE_RETAINED = 12,
	DOS_EXEC_FILE_LEASE_POISONED = 13
};

struct dos_exec_file_lease_handle {
	uint64_t value;
} __aligned(8);

/*
 * Persistent slots contain fixed-width integers only.  state stores one
 * dos_exec_file_lease_state value as uint8_t; is_device is meaningful only in
 * PROBED state.  It remains meaningful during CLOSING/CLOSED only when that
 * transition began from PROBED; an OPEN closed after failed probing is still
 * unclassified.  No enum, bool, size_t, or pointer is retained.
 */
struct dos_exec_file_lease_slot {
	uint64_t generation;
	kernel_object_handle_t adapter_identity;
	kernel_object_handle_t adapter_context;
	kernel_object_handle_t reader_context;
	file_offset_t size;
	uint8_t state;
	uint8_t is_device;
	uint8_t reserved[6];
} __aligned(8);

struct dos_exec_file_lease_table {
	struct dos_exec_file_lease_slot slots[DOS_EXEC_FILE_LEASE_SLOT_COUNT];
	kernel_object_handle_t identity;
	uint8_t initialized;
	uint8_t constructed;
	uint8_t reserved[6];
} __aligned(8);

#define DOS_EXEC_FILE_LEASE_TABLE_INITIALIZER                                  \
	{.slots = {{0}},                                                       \
	 .identity = KERNEL_OBJECT_HANDLE_INVALID,                             \
	 .initialized = 0u,                                                    \
	 .constructed = 1u,                                                    \
	 .reserved = {0}}

static_assert_expression(
    sizeof(struct dos_exec_file_open_result) == 24,
    "EXEC file open results must be data-model independent");
static_assert_expression(__alignof__(struct dos_exec_file_open_result) == 8,
			 "EXEC file open-result alignment changed");
static_assert_expression(__builtin_offsetof(struct dos_exec_file_open_result,
					    failure_detail) == 16,
			 "EXEC file open failure-detail offset changed");
static_assert_expression(
    sizeof(struct dos_exec_file_probe_result) == 8,
    "EXEC file probe results must be data-model independent");
static_assert_expression(__alignof__(struct dos_exec_file_probe_result) == 8,
			 "EXEC file probe-result alignment changed");
static_assert_expression(__builtin_offsetof(struct dos_exec_file_probe_result,
					    is_device) == 4,
			 "EXEC file probe device offset changed");
static_assert_expression(
    sizeof(struct dos_exec_file_lease_handle) == 8,
    "EXEC file lease handles must remain one explicit 64-bit value");
static_assert_expression(__alignof__(struct dos_exec_file_lease_handle) == 8,
			 "EXEC file lease-handle alignment changed");
static_assert_expression(
    sizeof(struct dos_exec_file_lease_slot) == 48,
    "EXEC file lease slots must be data-model independent");
static_assert_expression(__alignof__(struct dos_exec_file_lease_slot) == 8,
			 "EXEC file lease-slot alignment changed");
static_assert_expression(__builtin_offsetof(struct dos_exec_file_lease_slot,
					    adapter_identity) == 8,
			 "EXEC file lease adapter-identity offset changed");
static_assert_expression(__builtin_offsetof(struct dos_exec_file_lease_slot,
					    reader_context) == 24,
			 "EXEC file lease reader-context offset changed");
static_assert_expression(__builtin_offsetof(struct dos_exec_file_lease_slot,
					    state) == 40,
			 "EXEC file lease state offset changed");
static_assert_expression(
    sizeof(struct dos_exec_file_lease_table) == 784,
    "EXEC file lease tables must be data-model independent");
static_assert_expression(__alignof__(struct dos_exec_file_lease_table) == 8,
			 "EXEC file lease-table alignment changed");
static_assert_expression(__builtin_offsetof(struct dos_exec_file_lease_table,
					    identity) == 768,
			 "EXEC file lease table identity offset changed");
static_assert_expression(__builtin_offsetof(struct dos_exec_file_lease_table,
					    initialized) == 776,
			 "EXEC file lease table flags offset changed");
static_assert_expression(
    DOS_EXEC_FILE_LEASE_SLOT_COUNT < (1u << DOS_EXEC_FILE_LEASE_SLOT_BITS),
    "slot-plus-one handle encoding needs one reserved zero value");

/*
 * Construct begins a new C object lifetime; it is not a reset operation.
 * Initialize succeeds exactly once in that lifetime.  Before reconstructing,
 * the owner must stop acquisitions, drain every open or transitioning slot,
 * and ensure no old handle can be submitted.  POISONED cannot be normally
 * drained or retired: only after adapter-wide quarantine has explicitly
 * abandoned the uncertain represented resource may the owner end that object
 * lifetime and construct replacement storage.  Initialize never clears it.
 *
 * The table contains no internal lock.  One exclusive EXEC coordinator must
 * serialize every construct/initialize/acquire/probe/read-resolution/close/
 * retire operation and the represented adapter generation.  This ownership
 * may span callbacks, but it is not an arena spinlock and no low-level lock is
 * held across filesystem or device calls.
 */
enum dos_exec_file_lease_status dos_exec_file_lease_table_construct(
    struct dos_exec_file_lease_table *table) __must_check;
enum dos_exec_file_lease_status dos_exec_file_lease_table_initialize(
    struct dos_exec_file_lease_table *table,
    kernel_object_handle_t identity) __must_check;
bool dos_exec_file_lease_table_is_drained(
    const struct dos_exec_file_lease_table *table) __must_check;

/*
 * Reserve one generation and invoke OPEN.  path is borrowed as exactly
 * path_length bytes and is never stored or scanned.  handle and
 * open_failure_detail are unchanged on every error before OPEN is invoked.
 * OPEN_FAILED leaves handle unchanged and returns the adapter's exact opaque
 * detail; success owns one OPEN resource and writes detail zero.
 * If an adapter reports success with a malformed result, or returns an enum
 * outside the contract, POISONED is returned without publishing a handle: the
 * caller already knows ops->identity and must quarantine that complete adapter
 * generation before any later open.
 */
enum dos_exec_file_lease_status
dos_exec_file_lease_acquire(struct dos_exec_file_lease_table *table,
			    const struct dos_exec_file_lease_ops *ops,
			    kernel_object_handle_t context, const uint8_t *path,
			    size_t path_length,
			    struct dos_exec_file_lease_handle *handle,
			    uint32_t *open_failure_detail) __must_check;

/*
 * OPEN -> PROBED; repeated probing of PROBED is callback-free and idempotent.
 * is_device and probe_failure_detail are unchanged on every error before the
 * adapter is called.  PROBE_FAILED returns its opaque detail and leaves
 * is_device unchanged; success writes the classification and detail zero.
 * Exact ADAPTER_FAULT also requires zero reserved bytes; it returns the opaque
 * detail and retains OPEN ownership.  A malformed or otherwise unrecognized
 * adapter result poisons the slot and changes neither output, because it
 * cannot be mapped to a fabricated DOS error.
 */
enum dos_exec_file_lease_status dos_exec_file_lease_probe_device(
    struct dos_exec_file_lease_table *table,
    struct dos_exec_file_lease_handle handle,
    const struct dos_exec_file_lease_ops *ops, kernel_object_handle_t context,
    uint8_t *is_device, uint32_t *probe_failure_detail) __must_check;

/* Pure PROBED-state query.  is_device is unchanged on every error. */
enum dos_exec_file_lease_status
dos_exec_file_lease_query_device(const struct dos_exec_file_lease_table *table,
				 struct dos_exec_file_lease_handle handle,
				 const struct dos_exec_file_lease_ops *ops,
				 kernel_object_handle_t context,
				 uint8_t *is_device) __must_check;

/*
 * Resolve only a PROBED non-device resource.  reader is unchanged on error.
 * The returned dos_image_reader is transient: its function pointer is usable
 * only while the caller retains the same borrowed adapter generation and EXEC
 * serialization interval.  It must never be placed in persistent state.
 */
enum dos_exec_file_lease_status dos_exec_file_lease_resolve_reader(
    const struct dos_exec_file_lease_table *table,
    struct dos_exec_file_lease_handle handle,
    const struct dos_exec_file_lease_ops *ops, kernel_object_handle_t context,
    struct dos_image_reader *reader) __must_check;

/*
 * close and abort have the same ownership transition.  CLOSED is idempotent
 * and callback-free; RETAINED keeps the prior active state for retry;
 * UNCERTAIN permanently poisons the slot.  The EXEC coordinator must then
 * quarantine the complete adapter identity rather than only this slot.
 */
enum dos_exec_file_lease_status
dos_exec_file_lease_close(struct dos_exec_file_lease_table *table,
			  struct dos_exec_file_lease_handle handle,
			  const struct dos_exec_file_lease_ops *ops,
			  kernel_object_handle_t context) __must_check;
enum dos_exec_file_lease_status
dos_exec_file_lease_abort(struct dos_exec_file_lease_table *table,
			  struct dos_exec_file_lease_handle handle,
			  const struct dos_exec_file_lease_ops *ops,
			  kernel_object_handle_t context) __must_check;

/* Pure no-state-change validation for the CLOSED retirement below. */
enum dos_exec_file_lease_status dos_exec_file_lease_preflight_retire(
    const struct dos_exec_file_lease_table *table,
    struct dos_exec_file_lease_handle handle) __must_check;
/* Recycle only CLOSED; the next generation makes this handle stale. */
enum dos_exec_file_lease_status dos_exec_file_lease_retire(
    struct dos_exec_file_lease_table *table,
    struct dos_exec_file_lease_handle handle) __must_check;

#endif

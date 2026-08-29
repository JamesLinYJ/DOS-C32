/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Fixed-capacity guest-write undo journal for one serialized DOS EXEC.
 *
 * EXEC performs several DOS-global writes before the child transition. This
 * journal is only the bounded safety envelope for those writes: targets
 * retain DOS segment:offset and A20 behavior through dos_machine.  It is not a
 * general virtual-memory transaction and does not invent page, mapping, or
 * process semantics.
 */
#ifndef DOSC32_DOS_EXEC_JOURNAL_H
#define DOSC32_DOS_EXEC_JOURNAL_H

#include "address.h"
#include "compiler.h"
#include "dos_machine.h"
#include "types.h"

/*
 * One cache-line-sized payload keeps PSP staging to four backend round trips.
 * Sixteen records still cover PSP, stack, both MCBs, EXEC1 outputs and saved
 * global words without allocation or an unbounded journal.
 */
#define DOS_EXEC_JOURNAL_RECORD_CAPACITY 16u
#define DOS_EXEC_JOURNAL_RECORD_BYTES 64u

enum dos_exec_journal_state {
	DOS_EXEC_JOURNAL_STATE_UNINITIALIZED = 0,
	DOS_EXEC_JOURNAL_STATE_STAGING,
	DOS_EXEC_JOURNAL_STATE_SEALED,
	DOS_EXEC_JOURNAL_STATE_ABORTED,
	DOS_EXEC_JOURNAL_STATE_POISONED
};

enum dos_exec_journal_status {
	DOS_EXEC_JOURNAL_OK = 0,
	DOS_EXEC_JOURNAL_INVALID_ARGUMENT,
	DOS_EXEC_JOURNAL_INVALID_STATE,
	DOS_EXEC_JOURNAL_RECORD_TOO_LARGE,
	DOS_EXEC_JOURNAL_FULL,
	DOS_EXEC_JOURNAL_IDENTITY_MISMATCH,
	DOS_EXEC_JOURNAL_CONTEXT_MISMATCH,
	DOS_EXEC_JOURNAL_MACHINE_MISMATCH,
	DOS_EXEC_JOURNAL_MACHINE_FAULT,
	DOS_EXEC_JOURNAL_POISONED
};

/* One far write and the exact bytes visible before that write. */
struct dos_exec_journal_record {
	uint16_t segment;
	uint16_t offset;
	uint8_t count;
	uint8_t reserved[3];
	uint8_t old_bytes[DOS_EXEC_JOURNAL_RECORD_BYTES];
};

/*
 * No field is a native pointer, size_t, C enum, or bool.  machine_identity is
 * a generation-pinned registry identity supplied by the EXEC coordinator;
 * machine_context remains a borrowed callback key, not an address.
 */
struct dos_exec_journal {
	kernel_object_handle_t machine_identity;
	kernel_object_handle_t machine_context;
	uint64_t machine_address_limit;
	uint32_t record_count;
	uint8_t state;
	uint8_t constructed;
	uint8_t a20_enabled;
	uint8_t reserved;
	struct dos_exec_journal_record
	    records[DOS_EXEC_JOURNAL_RECORD_CAPACITY];
};

#define DOS_EXEC_JOURNAL_INITIALIZER                                           \
	{                                                                      \
		.machine_identity = KERNEL_OBJECT_HANDLE_INVALID,              \
		.machine_context = KERNEL_OBJECT_HANDLE_INVALID,               \
		.machine_address_limit = 0u, .record_count = 0u,               \
		.state = DOS_EXEC_JOURNAL_STATE_UNINITIALIZED,                 \
		.constructed = 1u, .a20_enabled = 0u, .reserved = 0u,          \
		.records = {                                                   \
			{0}                                                    \
		}                                                              \
	}

static_assert_expression(sizeof(struct dos_exec_journal_record) == 72,
			 "EXEC journal records must be data-model independent");
static_assert_expression(__builtin_offsetof(struct dos_exec_journal_record,
					    old_bytes) == 8,
			 "EXEC journal old-byte offset changed");
static_assert_expression(sizeof(struct dos_exec_journal) == 1184,
			 "EXEC journal must be data-model independent");
static_assert_expression(__builtin_offsetof(struct dos_exec_journal,
					    record_count) == 24,
			 "EXEC journal count offset changed");
static_assert_expression(__builtin_offsetof(struct dos_exec_journal, state) ==
			     28,
			 "EXEC journal state offset changed");
static_assert_expression(__builtin_offsetof(struct dos_exec_journal, records) ==
			     32,
			 "EXEC journal record-array offset changed");

/*
 * Construct starts a new C object lifetime; it is not a reset operation for a
 * live transaction.  Initialize binds one stable machine identity and the
 * current far-address model without invoking guest callbacks.
 */
enum dos_exec_journal_status
dos_exec_journal_construct(struct dos_exec_journal *journal) __must_check;
bool dos_exec_journal_has_valid_encoding(
    const struct dos_exec_journal *journal) __must_check;
enum dos_exec_journal_status
dos_exec_journal_initialize(struct dos_exec_journal *journal,
			    kernel_object_handle_t machine_identity,
			    const struct dos_machine *machine) __must_check;

/*
 * The caller must hold the same exclusive dos_exec_observer interval from
 * initialize through seal or abort.  The source is borrowed for this call.
 * Segment zero is a valid DOS segment.  A successful replacement publishes
 * exactly one undo record; every ordinary failure leaves the journal
 * unchanged, while a failed single-write rollback makes it sticky POISONED.
 */
enum dos_exec_journal_status dos_exec_journal_stage_replace_far(
    struct dos_exec_journal *journal, kernel_object_handle_t machine_identity,
    const struct dos_machine *machine, uint16_t segment, uint16_t offset,
    const void *source, size_t source_capacity, size_t count) __must_check;

/*
 * Atomically stage a larger contiguous far span as 64-byte records.  Offset
 * progression is explicit 16-bit wrap, matching REP MOVSB guest semantics;
 * no allocation occurs.  Capacity is checked before the first callback, and
 * a later failure restores only records created by this call.
 */
enum dos_exec_journal_status dos_exec_journal_stage_replace_far_span(
    struct dos_exec_journal *journal, kernel_object_handle_t machine_identity,
    const struct dos_machine *machine, uint16_t segment, uint16_t offset,
    const void *source, size_t source_capacity, size_t count) __must_check;

/*
 * Abort restores successful records in strict reverse order.  It is
 * idempotent after success.  Any failed restore poisons the journal and the
 * enclosing EXEC transaction must quarantine the guest backend.
 */
enum dos_exec_journal_status
dos_exec_journal_abort(struct dos_exec_journal *journal,
		       kernel_object_handle_t machine_identity,
		       const struct dos_machine *machine) __must_check;

/*
 * Seal commits the already-written guest bytes by discarding undo records.
 * It invokes no guest callback and is idempotent after success.
 */
enum dos_exec_journal_status
dos_exec_journal_preflight_seal(const struct dos_exec_journal *journal,
				kernel_object_handle_t machine_identity,
				const struct dos_machine *machine) __must_check;
/* Sticky no-callback quarantine after a cross-object seal invariant fails. */
enum dos_exec_journal_status
dos_exec_journal_poison(struct dos_exec_journal *journal,
			kernel_object_handle_t machine_identity,
			const struct dos_machine *machine) __must_check;
enum dos_exec_journal_status
dos_exec_journal_seal(struct dos_exec_journal *journal,
		      kernel_object_handle_t machine_identity,
		      const struct dos_machine *machine) __must_check;

#endif

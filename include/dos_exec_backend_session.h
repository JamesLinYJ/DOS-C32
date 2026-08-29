/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Generation-bound lifecycle for dormant and runnable DOS execution backends.
 *
 * EXEC publication and instruction execution are deliberately separate.  A
 * fallible adapter callback may prepare a dormant backend; after all DOS
 * objects pass their publication preflights, making the session runnable is a
 * callback-free state transition.  This is the common owner for every guest
 * execution backend, not a second implementation of DOS process semantics.
 */
#ifndef DOSC32_DOS_EXEC_BACKEND_SESSION_H
#define DOSC32_DOS_EXEC_BACKEND_SESSION_H

#include "compiler.h"
#include "exec_backend.h"
#include "types.h"

#define DOS_EXEC_BACKEND_SESSION_SLOT_COUNT 4u
#define DOS_EXEC_BACKEND_SESSION_SLOT_BITS 3u
#define DOS_EXEC_BACKEND_SESSION_SLOT_MASK 0x07ull
#define DOS_EXEC_BACKEND_SESSION_GENERATION_MAX 0x1fffffffffffffffull

enum dos_exec_backend_session_state {
	DOS_EXEC_BACKEND_SESSION_VACANT = 0,
	DOS_EXEC_BACKEND_SESSION_PREPARING,
	DOS_EXEC_BACKEND_SESSION_DORMANT,
	DOS_EXEC_BACKEND_SESSION_RUNNABLE,
	DOS_EXEC_BACKEND_SESSION_RUNNING,
	DOS_EXEC_BACKEND_SESSION_EXITED,
	DOS_EXEC_BACKEND_SESSION_RELEASING,
	DOS_EXEC_BACKEND_SESSION_STOPPED,
	DOS_EXEC_BACKEND_SESSION_STATE_POISONED
};

enum dos_exec_backend_session_status {
	DOS_EXEC_BACKEND_SESSION_OK = 0,
	DOS_EXEC_BACKEND_SESSION_INVALID_ARGUMENT,
	DOS_EXEC_BACKEND_SESSION_NOT_INITIALIZED,
	DOS_EXEC_BACKEND_SESSION_INVALID_STATE,
	DOS_EXEC_BACKEND_SESSION_NO_SLOT,
	DOS_EXEC_BACKEND_SESSION_GENERATION_EXHAUSTED,
	DOS_EXEC_BACKEND_SESSION_STALE_HANDLE,
	DOS_EXEC_BACKEND_SESSION_IDENTITY_MISMATCH,
	DOS_EXEC_BACKEND_SESSION_CONTEXT_MISMATCH,
	DOS_EXEC_BACKEND_SESSION_MACHINE_MISMATCH,
	DOS_EXEC_BACKEND_SESSION_PREPARE_REJECTED,
	DOS_EXEC_BACKEND_SESSION_RELEASE_RETAINED,
	DOS_EXEC_BACKEND_SESSION_POISONED,
	/* Appended: a precise event snapshot no longer names current state. */
	DOS_EXEC_BACKEND_SESSION_STATE_MISMATCH
};

struct dos_exec_backend_session_handle {
	uint64_t value;
} __aligned(8);

/* Fixed-width persistent value; all pointers remain borrowed by public calls. */
struct dos_exec_backend_session_slot {
	uint64_t generation;
	kernel_object_handle_t adapter_identity;
	kernel_object_handle_t adapter_context;
	kernel_object_handle_t backend_context;
	kernel_object_handle_t machine_identity;
	kernel_object_handle_t machine_context;
	uint64_t machine_address_limit;
	struct dos_exec_handoff_plan handoff;
	struct dos_cpu_state current_state;
	uint32_t capabilities;
	uint8_t state;
	uint8_t a20_enabled;
	uint8_t has_current_state;
	uint8_t reserved[9];
} __aligned(8);

struct dos_exec_backend_session_table {
	struct dos_exec_backend_session_slot
	    slots[DOS_EXEC_BACKEND_SESSION_SLOT_COUNT];
	kernel_object_handle_t identity;
	uint8_t initialized;
	uint8_t constructed;
	uint8_t poisoned;
	uint8_t reserved[5];
} __aligned(8);

#define DOS_EXEC_BACKEND_SESSION_TABLE_INITIALIZER                            \
	{                                                                      \
		.slots = {{0}}, .identity = KERNEL_OBJECT_HANDLE_INVALID,        \
		.initialized = 0u, .constructed = 1u, .poisoned = 0u,           \
		.reserved = {0u}                                                \
	}

static_assert_expression(sizeof(struct dos_exec_backend_session_handle) == 8,
			 "backend session handles must remain 64-bit");
static_assert_expression(sizeof(struct dos_exec_backend_session_slot) == 200,
			 "backend session slots must be data-model independent");
static_assert_expression(sizeof(struct dos_exec_backend_session_table) == 816,
			 "backend session tables must be data-model independent");
static_assert_expression(
    DOS_EXEC_BACKEND_SESSION_SLOT_COUNT <
	(1u << DOS_EXEC_BACKEND_SESSION_SLOT_BITS),
    "slot-plus-one handle encoding requires one reserved zero value");

enum dos_exec_backend_session_status dos_exec_backend_session_table_construct(
    struct dos_exec_backend_session_table *table) __must_check;
enum dos_exec_backend_session_status dos_exec_backend_session_table_initialize(
    struct dos_exec_backend_session_table *table,
    kernel_object_handle_t identity) __must_check;
bool dos_exec_backend_session_table_is_drained(
    const struct dos_exec_backend_session_table *table) __must_check;

/*
 * Invoke the fallible adapter prepare operation and acquire a DORMANT session.
 * handle and failure_detail are unchanged before the callback.  An exact
 * adapter rejection changes only failure_detail.  Malformed or uncertain
 * ownership poisons the table and publishes no handle.
 */
enum dos_exec_backend_session_status dos_exec_backend_session_prepare(
    struct dos_exec_backend_session_table *table,
    const struct dos_exec_backend_ops *ops,
    kernel_object_handle_t adapter_context,
    kernel_object_handle_t machine_identity,
    const struct dos_machine *machine,
    const struct dos_exec_handoff_plan *handoff,
    struct dos_exec_backend_session_handle *handle,
    uint32_t *failure_detail) __must_check;

/* Pure validation followed by a no-callback DORMANT -> RUNNABLE transition. */
enum dos_exec_backend_session_status dos_exec_backend_session_preflight_publish(
    const struct dos_exec_backend_session_table *table,
    struct dos_exec_backend_session_handle handle,
    const struct dos_exec_backend_ops *ops,
    kernel_object_handle_t adapter_context,
    kernel_object_handle_t machine_identity,
    const struct dos_machine *machine,
    const struct dos_exec_handoff_plan *expected_handoff) __must_check;
enum dos_exec_backend_session_status dos_exec_backend_session_publish(
    struct dos_exec_backend_session_table *table,
    struct dos_exec_backend_session_handle handle,
    const struct dos_exec_backend_ops *ops,
    kernel_object_handle_t adapter_context,
    kernel_object_handle_t machine_identity,
    const struct dos_machine *machine,
    const struct dos_exec_handoff_plan *expected_handoff) __must_check;

/*
 * Execute only a RUNNABLE session until one precise typed event.  CPU/event
 * outputs are unchanged on error.  An imprecise adapter return poisons the
 * complete backend table because instruction side effects cannot be undone.
 */
enum dos_exec_backend_session_status dos_exec_backend_session_run_until_event(
    struct dos_exec_backend_session_table *table,
    struct dos_exec_backend_session_handle handle,
    const struct dos_exec_backend_ops *ops,
    kernel_object_handle_t adapter_context,
    kernel_object_handle_t machine_identity,
    const struct dos_machine *machine,
    struct dos_cpu_state *state,
    struct dos_execution_event *event) __must_check;

/*
 * Publish DOS/BIOS service results back into a RUNNABLE session.  The expected
 * precise-event state prevents an old dispatcher result from overwriting a
 * later stop.  This is an O(1), callback-free compare-and-replace operation.
 */
enum dos_exec_backend_session_status dos_exec_backend_session_replace_state(
    struct dos_exec_backend_session_table *table,
    struct dos_exec_backend_session_handle handle,
    const struct dos_exec_backend_ops *ops,
    kernel_object_handle_t adapter_context,
    kernel_object_handle_t machine_identity,
    const struct dos_machine *machine,
    const struct dos_cpu_state *expected_state,
    const struct dos_cpu_state *replacement_state) __must_check;

/*
 * Release a DORMANT, RUNNABLE, or EXITED backend.  RETAINED is retryable;
 * uncertainty poisons the complete adapter table.  STOPPED retirement is
 * callback-free and makes the generation stale.
 */
enum dos_exec_backend_session_status dos_exec_backend_session_stop(
    struct dos_exec_backend_session_table *table,
    struct dos_exec_backend_session_handle handle,
    const struct dos_exec_backend_ops *ops,
    kernel_object_handle_t adapter_context) __must_check;
enum dos_exec_backend_session_status dos_exec_backend_session_retire(
    struct dos_exec_backend_session_table *table,
    struct dos_exec_backend_session_handle handle) __must_check;

enum dos_exec_backend_session_status dos_exec_backend_session_get_state(
    const struct dos_exec_backend_session_table *table,
    struct dos_exec_backend_session_handle handle,
    enum dos_exec_backend_session_state *state) __must_check;

#endif

/* SPDX-License-Identifier: GPL-2.0-only */
/* Exclusive observation ownership for a multi-step DOS EXEC transaction. */
#ifndef DOSC32_DOS_EXEC_OBSERVER_H
#define DOSC32_DOS_EXEC_OBSERVER_H

#include "address.h"
#include "compiler.h"
#include "types.h"

enum dos_exec_observer_adapter_status {
	DOS_EXEC_OBSERVER_ADAPTER_OK = 0,
	DOS_EXEC_OBSERVER_ADAPTER_BUSY,
	DOS_EXEC_OBSERVER_ADAPTER_FAULT
};

/*
 * The adapter stops guest execution and IRQ/interrupt observers without
 * holding an arena spinlock over filesystem, device, or guest-memory calls.
 * acquire publishes a nonzero generation only after exclusion is effective.
 * release removes exactly that generation.  quarantine is the fail-closed
 * path when the coordinator can no longer prove a safe release.
 * Exact BUSY and FAULT acquire results guarantee that generation remains zero
 * and no exclusion ownership was acquired.  A nonzero generation with either
 * result, or any result outside this enum, is ownership-uncertain and is
 * quarantined before POISONED is returned.
 * Every ownership-uncertain path publishes the observer's sticky POISONED
 * state before invoking quarantine, so a callback that reenters an enclosing
 * coordinator cannot observe a reusable intermediate state.
 *
 * The operations table is borrowed for one call and is never retained.
 * identity is a generation-pinned registry identity, not a native pointer.
 */
struct dos_exec_observer_ops {
	kernel_object_handle_t identity;
	enum dos_exec_observer_adapter_status (*acquire)(
	    kernel_object_handle_t context, uint64_t *generation);
	enum dos_exec_observer_adapter_status (*release)(
	    kernel_object_handle_t context, uint64_t generation);
	enum dos_exec_observer_adapter_status (*quarantine)(
	    kernel_object_handle_t context, uint64_t generation);
};

enum dos_exec_observer_state {
	DOS_EXEC_OBSERVER_STATE_IDLE = 0,
	DOS_EXEC_OBSERVER_STATE_ACQUIRING,
	DOS_EXEC_OBSERVER_STATE_HELD,
	DOS_EXEC_OBSERVER_STATE_RELEASING,
	DOS_EXEC_OBSERVER_STATE_RELEASED,
	DOS_EXEC_OBSERVER_STATE_POISONED
};

enum dos_exec_observer_status {
	DOS_EXEC_OBSERVER_OK = 0,
	DOS_EXEC_OBSERVER_INVALID_ARGUMENT,
	DOS_EXEC_OBSERVER_INVALID_STATE,
	DOS_EXEC_OBSERVER_BUSY,
	DOS_EXEC_OBSERVER_BACKEND_FAULT,
	DOS_EXEC_OBSERVER_IDENTITY_MISMATCH,
	DOS_EXEC_OBSERVER_CONTEXT_MISMATCH,
	DOS_EXEC_OBSERVER_POISONED
};

/* Fixed-width native ownership state; no field is a native pointer. */
struct dos_exec_observer {
	kernel_object_handle_t adapter_identity;
	kernel_object_handle_t context;
	uint64_t generation;
	uint8_t state;
	uint8_t constructed;
	uint8_t reserved[6];
} __aligned(8);

#define DOS_EXEC_OBSERVER_INITIALIZER                                          \
	{.adapter_identity = KERNEL_OBJECT_HANDLE_INVALID,                     \
	 .context = KERNEL_OBJECT_HANDLE_INVALID,                              \
	 .generation = 0u,                                                     \
	 .state = DOS_EXEC_OBSERVER_STATE_IDLE,                                \
	 .constructed = true,                                                  \
	 .reserved = {0u}}

static_assert_expression(sizeof(struct dos_exec_observer) == 32,
			 "EXEC observer state must be data-model independent");
static_assert_expression(__alignof__(struct dos_exec_observer) == 8,
			 "EXEC observer-state alignment changed");
static_assert_expression(__builtin_offsetof(struct dos_exec_observer, state) ==
			     24,
			 "EXEC observer state offset changed");

enum dos_exec_observer_status
dos_exec_observer_construct(struct dos_exec_observer *observer) __must_check;

/* Acquire exactly once before the transaction's first private guest write. */
enum dos_exec_observer_status
dos_exec_observer_acquire(struct dos_exec_observer *observer,
			  const struct dos_exec_observer_ops *ops,
			  kernel_object_handle_t context) __must_check;

/* Pure seal preflight; valid only while the executor remains serialized. */
enum dos_exec_observer_status
dos_exec_observer_validate_held(const struct dos_exec_observer *observer,
				const struct dos_exec_observer_ops *ops,
				kernel_object_handle_t context) __must_check;

/*
 * Idempotent after a successful release; callbacks run at most once.  A HELD
 * encoding with generation zero is rejected without invoking the adapter.
 */
enum dos_exec_observer_status
dos_exec_observer_release(struct dos_exec_observer *observer,
			  const struct dos_exec_observer_ops *ops,
			  kernel_object_handle_t context) __must_check;

/*
 * Sticky quarantine for rollback failure or uncertain post-handoff state.
 * A HELD encoding with generation zero is rejected without invoking the
 * adapter.
 */
enum dos_exec_observer_status
dos_exec_observer_poison(struct dos_exec_observer *observer,
			 const struct dos_exec_observer_ops *ops,
			 kernel_object_handle_t context) __must_check;

#endif

/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Final no-filesystem publication seal for DOS EXEC1.
 *
 * Preflight and acquire-before-publish keep the callback-driven C
 * implementation fail closed; MS-DOS remains the authority for
 * the published PSP, MCB, JFT/SFT, DTA, CurrentPDB and EXEC1 values.
 */
#ifndef DOSC32_DOS_EXEC_SEAL_H
#define DOSC32_DOS_EXEC_SEAL_H

#include "compiler.h"
#include "dos_exec_journal.h"
#include "dos_exec_observer.h"
#include "dos_memory_lease.h"
#include "dos_process_runtime.h"
#include "dos_sft_batch.h"
#include "types.h"

enum dos_exec_seal_status {
	DOS_EXEC_SEAL_OK = 0,
	DOS_EXEC_SEAL_INVALID_ARGUMENT,
	DOS_EXEC_SEAL_OBSERVER_NOT_READY,
	DOS_EXEC_SEAL_JOURNAL_NOT_READY,
	DOS_EXEC_SEAL_ENVIRONMENT_NOT_READY,
	DOS_EXEC_SEAL_LOAD_NOT_READY,
	DOS_EXEC_SEAL_SFT_NOT_READY,
	DOS_EXEC_SEAL_RUNTIME_NOT_READY,
	/* A post-preflight invariant or observation-release failure. */
	DOS_EXEC_SEAL_POISONED
};

/* Fixed-width transaction values; no zero-segment sentinel is used. */
struct dos_exec_load_only_seal_plan {
	struct dos_process_runtime_snapshot expected_parent;
	struct dos_memory_lease_rebind_plan environment_rebind;
	struct dos_memory_lease_rebind_plan load_rebind;
	dos_sft_batch_handle_t sft_batch;
	uint16_t child_psp;
	uint8_t has_environment;
	uint8_t reserved[5];
} __aligned(8);

/* Borrowed only for one call and never stored in a transaction slot. */
struct dos_exec_seal_services {
	struct dos_exec_observer *observer;
	const struct dos_exec_observer_ops *observer_ops;
	kernel_object_handle_t observer_context;
	struct dos_exec_journal *journal;
	kernel_object_handle_t machine_identity;
	struct dos_memory_lease_table *memory_leases;
	struct dos_memory_arena *arena;
	const struct dos_machine *machine;
	struct dos_process_runtime *runtime;
};

static_assert_expression(sizeof(struct dos_exec_load_only_seal_plan) == 168,
			 "EXEC1 seal plan must be data-model independent");
static_assert_expression(__alignof__(struct dos_exec_load_only_seal_plan) == 8,
			 "EXEC1 seal-plan alignment changed");
static_assert_expression(__builtin_offsetof(struct dos_exec_load_only_seal_plan,
					    environment_rebind) == 24,
			 "EXEC1 environment-lease offset changed");
static_assert_expression(__builtin_offsetof(struct dos_exec_load_only_seal_plan,
					    load_rebind) == 88,
			 "EXEC1 load-rebind offset changed");
static_assert_expression(__builtin_offsetof(struct dos_exec_load_only_seal_plan,
					    child_psp) == 160,
			 "EXEC1 child PSP offset changed");

/* Pure, no-callback and no-state-change final validation. */
enum dos_exec_seal_status dos_exec_seal_preflight_load_only(
    const struct dos_exec_seal_services *services,
    const struct dos_exec_load_only_seal_plan *plan) __must_check;

/*
 * Preflight is repeated internally.  The seal publishes environment/load
 * leases and the SFT batch, discards the guest undo journal, publishes DTA and
 * CurrentPDB last, then releases observation ownership.  No filesystem,
 * reader, network, device, or guest-memory callback occurs in the local seal;
 * only the final observer release callback is allowed.
 */
enum dos_exec_seal_status dos_exec_seal_commit_load_only(
    const struct dos_exec_seal_services *services,
    const struct dos_exec_load_only_seal_plan *plan) __must_check;

#endif

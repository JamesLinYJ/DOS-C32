/* SPDX-License-Identifier: GPL-2.0-only */
/* One production owner for the process-global DOS namespace. */
#ifndef DOSC32_DOS_RUNTIME_OWNER_H
#define DOSC32_DOS_RUNTIME_OWNER_H

#include "compiler.h"
#include "dos_exec_transaction.h"
#include "dos_personality.h"
#include "types.h"

enum dos_runtime_owner_status {
	DOS_RUNTIME_OWNER_READY = 0,
	DOS_RUNTIME_OWNER_INVALID_ARGUMENT,
	DOS_RUNTIME_OWNER_INVALID_STATE,
	DOS_RUNTIME_OWNER_MACHINE_FAULT,
	DOS_RUNTIME_OWNER_MEMORY_FAULT,
	DOS_RUNTIME_OWNER_PROCESS_FAULT
};

/* Low 64 KiB retains the IVT, BDA and BIOS/boot compatibility structures. */
#define DOS_RUNTIME_DEFAULT_ARENA_HEAD_SEGMENT 0x1000u

/* Fixed identities and guest layout; no native pointer crosses this value. */
struct dos_runtime_owner_config {
	kernel_object_handle_t coordinator_identity;
	kernel_object_handle_t file_lease_table_identity;
	kernel_object_handle_t memory_arena_identity;
	kernel_object_handle_t backend_session_table_identity;
	kernel_object_handle_t runtime_identity;
	kernel_object_handle_t personality_identity;
	dos_memory_lease_table_identity_t memory_lease_table_identity;
	uint16_t arena_head_segment;
	uint16_t arena_end_segment;
	struct dos_int21_drive_config drives;
} __aligned(8);

/*
 * Borrowed code/data bindings.  The owner persists only their identities and
 * integer contexts; this native structure is validated again whenever an
 * EXEC services view is borrowed.
 */
struct dos_runtime_owner_bindings {
	const struct dos_machine *machine;
	const struct dos_exec_file_lease_ops *file_ops;
	const struct dos_exec_observer_ops *observer_ops;
	const struct dos_sft_batch_ops *sft_ops;
	const struct dos_exec_drive_visibility_ops *drive_ops;
	const struct dos_exec_backend_ops *backend_ops;
	kernel_object_handle_t machine_identity;
	kernel_object_handle_t file_adapter_context;
	kernel_object_handle_t observer_adapter_context;
	kernel_object_handle_t sft_adapter_context;
	kernel_object_handle_t drive_adapter_context;
	kernel_object_handle_t backend_adapter_context;
};

enum dos_runtime_owner_status dos_runtime_owner_initialize(
	const struct dos_runtime_owner_config *config,
	const struct dos_runtime_owner_bindings *bindings) __must_check;

/* Output is unchanged unless the complete current binding is proven. */
enum dos_runtime_owner_status dos_runtime_owner_borrow_exec_services(
	const struct dos_runtime_owner_bindings *bindings,
	struct dos_exec_transaction_services *services) __must_check;

struct dos_exec_transaction_table *dos_runtime_owner_transactions(void)
	__must_check;
struct dos_exec_backend_session_table *dos_runtime_owner_sessions(void)
	__must_check;
struct dos_personality *dos_runtime_owner_personality(void) __must_check;
uint16_t dos_runtime_owner_initial_psp(void) __must_check;

static_assert_expression(sizeof(struct dos_runtime_owner_config) == 64u,
			 "runtime owner configuration must stay fixed width");

#endif

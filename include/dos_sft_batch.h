/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Transactional JFT/SFT inheritance for DOS EXEC.
 *
 * MS-DOS walks exactly the first twenty parent JFT bytes. Invalid SFTs,
 * sf_no_inherit entries, and sharing_net_fcb entries become ffh.  Every other
 * entry performs one logical device open for a non-network SFT and one SFT
 * reference increment, even when several JFT bytes name the same SFN.
 *
 * This boundary deliberately does not implement an SFT table.  The adapter
 * owns SFT lookup, generation pinning, reference arithmetic, and device calls.
 * Checked handles and reverse unwind make those operations safely composable
 * without introducing unrelated errno or namespace semantics.
 */
#ifndef DOSC32_DOS_SFT_BATCH_H
#define DOSC32_DOS_SFT_BATCH_H

#include "address.h"
#include "compiler.h"
#include "types.h"

#define DOS_SFT_BATCH_JFT_ENTRIES 20u
#define DOS_SFT_BATCH_MAXIMUM 8u
#define DOS_SFT_BATCH_HANDLE_INVALID ((dos_sft_batch_handle_t)0u)
#define DOS_SFT_REFERENCE_HANDLE_INVALID KERNEL_OBJECT_HANDLE_INVALID

#define DOS_SFT_FLAG_IS_NETWORK 0x8000u
#define DOS_SFT_FLAG_NO_INHERIT 0x1000u
#define DOS_SFT_SHARING_MASK 0x00f0u
#define DOS_SFT_SHARING_NETWORK_FCB 0x0070u
#define DOS_JFT_ENTRY_UNUSED 0xffu

typedef kernel_object_handle_t dos_sft_batch_handle_t;
typedef kernel_object_handle_t dos_sft_reference_handle_t;

enum dos_sft_adapter_status {
	DOS_SFT_ADAPTER_OK = 0,
	/* Normal SFFromHandle failure: EXEC writes ffh and continues. */
	DOS_SFT_ADAPTER_INVALID_SFT,
	/* Internal adapter/device/table failure; never a DOS errno value. */
	DOS_SFT_ADAPTER_FAULT
};

struct dos_sft_view {
	dos_sft_reference_handle_t reference_handle;
	uint16_t flags;
	uint16_t mode;
};

/*
 * All callbacks either complete exactly one operation or leave adapter state
 * unchanged.  lookup must return a generation-pinned 64-bit reference handle;
 * the batch retains that integer, never a native SFT pointer.  A successful
 * reference_release and device_close undo precisely one corresponding acquire
 * and open.  The future real-SFT adapter is responsible for validating the
 * refcount (including overflow/busy values) and for preserving DOS ordering.
 *
 * ops is borrowed only during a public call and is never stored.  The same
 * generation-pinned identity and context must be supplied to abort; this
 * binds the integer lease without retaining native function pointers.
 * Callers must externally serialize the fixed registry and the represented
 * SFT/device state against EXEC, CLOSE, and guest execution.
 */
struct dos_sft_batch_ops {
	/* Registry identity, never a native function-table pointer. */
	kernel_object_handle_t identity;
	enum dos_sft_adapter_status (*lookup)(kernel_object_handle_t context,
					      uint8_t sfn,
					      struct dos_sft_view *view);
	enum dos_sft_adapter_status (*device_open)(
	    kernel_object_handle_t context,
	    dos_sft_reference_handle_t reference_handle);
	enum dos_sft_adapter_status (*reference_acquire)(
	    kernel_object_handle_t context,
	    dos_sft_reference_handle_t reference_handle);
	enum dos_sft_adapter_status (*reference_release)(
	    kernel_object_handle_t context,
	    dos_sft_reference_handle_t reference_handle);
	enum dos_sft_adapter_status (*device_close)(
	    kernel_object_handle_t context,
	    dos_sft_reference_handle_t reference_handle);
};

struct dos_sft_jft20 {
	uint8_t entries[DOS_SFT_BATCH_JFT_ENTRIES];
};

enum dos_sft_batch_state {
	DOS_SFT_BATCH_STATE_PREPARED = 1,
	DOS_SFT_BATCH_STATE_COMMITTED,
	DOS_SFT_BATCH_STATE_ABORTED,
	DOS_SFT_BATCH_STATE_POISONED
};

enum dos_sft_batch_status {
	DOS_SFT_BATCH_OK = 0,
	DOS_SFT_BATCH_INVALID_ARGUMENT,
	DOS_SFT_BATCH_NO_SLOT,
	DOS_SFT_BATCH_STALE_HANDLE,
	DOS_SFT_BATCH_INVALID_STATE,
	DOS_SFT_BATCH_ALREADY_COMMITTED,
	DOS_SFT_BATCH_ADAPTER_FAULT,
	/*
	 * At least one acquired operation could not be undone.  The caller must
	 * quarantine the complete adapter identity, not merely this batch slot.
	 */
	DOS_SFT_BATCH_POISONED
};

/*
 * Exactly twenty entries are inspected, matching FilPerProc and
 * the MS-DOS inheritance contract; this is deliberately not a caller-selected
 * prefix.  Unused output positions are ffh.  On ordinary failure batch_handle
 * remains INVALID and all acquired operations have been unwound.  If unwind
 * fails, POISONED is returned and the poisoned handle is published so the
 * enclosing EXEC transaction can quarantine the adapter and machine.
 */
enum dos_sft_batch_status
dos_sft_batch_prepare(const struct dos_sft_batch_ops *ops,
		      kernel_object_handle_t context,
		      const struct dos_sft_jft20 *parent_jft,
		      dos_sft_batch_handle_t *batch_handle) __must_check;

/* Copy the fully prepared child table; output is unchanged on every error. */
enum dos_sft_batch_status
dos_sft_batch_copy_child_jft(dos_sft_batch_handle_t batch_handle,
			     struct dos_sft_jft20 *child_jft) __must_check;

/*
 * Pure commit preflight.  Only a live PREPARED generation succeeds; poisoned,
 * terminal and stale records are rejected.  This call invokes no adapter and
 * changes neither the batch nor its prepared child JFT.
 */
enum dos_sft_batch_status dos_sft_batch_preflight_commit(
    dos_sft_batch_handle_t batch_handle) __must_check;

/*
 * Commit only transfers ownership in the batch state; it calls no adapter.
 * Repeating commit after COMMITTED remains successful, while pure preflight
 * intentionally accepts PREPARED only.
 */
enum dos_sft_batch_status
dos_sft_batch_commit(dos_sft_batch_handle_t batch_handle) __must_check;

/*
 * Abort PREPARED batches in exact reverse order.  Repeated abort of ABORTED is
 * successful and performs no callbacks.  A COMMITTED batch owns no rollback
 * right: abort returns ALREADY_COMMITTED and never releases child references.
 */
enum dos_sft_batch_status
dos_sft_batch_abort(dos_sft_batch_handle_t batch_handle,
		    const struct dos_sft_batch_ops *ops,
		    kernel_object_handle_t context) __must_check;

/* Pure validation for retirement of an ABORTED or COMMITTED generation. */
enum dos_sft_batch_status dos_sft_batch_preflight_retire(
    dos_sft_batch_handle_t batch_handle) __must_check;
/* Recycle only an ABORTED or COMMITTED record; generation rejects old ABA. */
enum dos_sft_batch_status
dos_sft_batch_retire(dos_sft_batch_handle_t batch_handle) __must_check;

enum dos_sft_batch_status
dos_sft_batch_get_state(dos_sft_batch_handle_t batch_handle,
			enum dos_sft_batch_state *state) __must_check;

static_assert_expression(sizeof(dos_sft_batch_handle_t) == 8,
			 "batch handles must remain canonical 64-bit");
static_assert_expression(sizeof(dos_sft_reference_handle_t) == 8,
			 "SFT reference handles must remain canonical 64-bit");
static_assert_expression(
    sizeof(struct dos_sft_jft20) == 20,
    "source EXEC inheritance table must remain twenty bytes");

#endif

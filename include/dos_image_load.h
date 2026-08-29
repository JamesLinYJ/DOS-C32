/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Safe DOS EXEC resident-image loading.
 *
 * Compatibility contract: COM reads stop short of the available capacity; MZ reads
 *                 use wrapped exec_res_len_para geometry and accept only a
 *                 per-read deficit below 512, without clearing absent bytes.
 * Safety changes: immutable 64-bit reader handles, checked lease geometry,
 *                 fixed-size staging buffers and rollback-protected writes.
 */
#ifndef DOSC32_DOS_IMAGE_LOAD_H
#define DOSC32_DOS_IMAGE_LOAD_H

#include "address.h"
#include "compiler.h"
#include "dos_memory_lease.h"
#include "dos_process.h"
#include "types.h"

enum dos_image_load_status {
	DOS_IMAGE_LOAD_OK = 0,
	DOS_IMAGE_LOAD_INVALID_ARGUMENT,
	DOS_IMAGE_LOAD_WRONG_IMAGE_FORMAT,
	DOS_IMAGE_LOAD_STALE_PLAN,
	DOS_IMAGE_LOAD_FILE_RANGE_OVERFLOW,
	DOS_IMAGE_LOAD_BAD_FILE_RANGE,
	DOS_IMAGE_LOAD_BAD_LEASE,
	DOS_IMAGE_LOAD_BAD_RESIDENT_RANGE,
	DOS_IMAGE_LOAD_IMAGE_IO_ERROR,
	DOS_IMAGE_LOAD_IMAGE_SHORT_READ,
	DOS_IMAGE_LOAD_MACHINE_FAULT,
	/* A failed rollback leaves guest memory indeterminate. */
	DOS_IMAGE_LOAD_MACHINE_POISONED
};

/* Published only to the enclosing transaction after a completely good load. */
struct dos_image_load_result {
	kernel_object_handle_t lease_handle;
	uint32_t file_bytes_written;
	uint32_t resident_bytes;
	uint32_t untouched_bytes;
	uint32_t reserved;
};

static_assert_expression(sizeof(struct dos_memory_lease_view) == 40,
			 "active lease view must be data-model independent");
static_assert_expression(__builtin_offsetof(struct dos_memory_lease_view,
					    handle) == 0,
			 "active lease handle offset changed");
static_assert_expression(__builtin_offsetof(struct dos_memory_lease_view,
					    machine_context) == 8,
			 "active lease machine-context offset changed");
static_assert_expression(__builtin_offsetof(struct dos_memory_lease_view,
					    arena_identity) == 16,
			 "active lease arena-identity offset changed");
static_assert_expression(__builtin_offsetof(struct dos_memory_lease_view,
					    arena_generation) == 24,
			 "active lease arena-generation offset changed");
static_assert_expression(__builtin_offsetof(struct dos_memory_lease_view,
					    guest_segment) == 32,
			 "active lease guest-geometry offset changed");
static_assert_expression(__builtin_offsetof(struct dos_memory_lease_view,
					    owner) == 36,
			 "active lease owner offset changed");
static_assert_expression(__builtin_offsetof(struct dos_memory_lease_view,
					    reserved) == 38,
			 "active lease reserved offset changed");
static_assert_expression(sizeof(struct dos_image_load_result) == 24,
			 "image-load result must be data-model independent");
static_assert_expression(__builtin_offsetof(struct dos_image_load_result,
					    lease_handle) == 0,
			 "image-load result handle offset changed");
static_assert_expression(__builtin_offsetof(struct dos_image_load_result,
					    resident_bytes) == 12,
			 "image-load result resident-size offset changed");

/*
 * reader->context must identify the same immutable snapshot inspected into
 * image_plan.  lease_view must have just been produced by
 * dos_memory_lease_resolve_active() inside the same exclusive EXEC/IRQ
 * observation interval as this call.  It is a transient observation, not a
 * persistent capability: do not retain it after dropping that serialization
 * boundary.  These calls only change the caller-owned active lease; they never
 * publish it or retain a native pointer.  result is unchanged on error.
 *
 * Every non-OK return requires the enclosing EXEC transaction to discard the
 * complete lease: an earlier fixed-size chunk may already contain file data.
 * MACHINE_POISONED additionally requires stopping the guest backend because
 * even the failing chunk could not be restored.
 *
 * For MZ, image_plan->image_size is the resident byte request, not a logical
 * length reconstructed from exe_len_mod_512.  file_bytes_written is the part
 * physically present after image_file_offset, capped by that request.
 * untouched_bytes are deliberately not zero-filled, including a missing final
 * fraction accepted by MS-DOS's strict-less-than-512 rule.
 */
enum dos_image_load_status
dos_image_load_com_resident(const struct dos_image_reader *reader,
			    const struct dos_machine *machine,
			    const struct dos_load_plan *image_plan,
			    const struct dos_com_process_plan *process_plan,
			    const struct dos_memory_lease_view *lease_view,
			    struct dos_image_load_result *result) __must_check;

enum dos_image_load_status
dos_image_load_mz_resident(const struct dos_image_reader *reader,
			   const struct dos_machine *machine,
			   const struct dos_load_plan *image_plan,
			   const struct dos_mz_process_plan *process_plan,
			   const struct dos_memory_lease_view *lease_view,
			   struct dos_image_load_result *result) __must_check;

/*
 * Stack writes are a distinct prepare step after successful resident loading.
 * COM always receives the zero return sentinel written by exec_com_file.
 * EXEC1 additionally stores the default AX at SS:SP-2.  MZ EXEC1 stores only
 * that default AX.  EXEC0's two PUSH instructions occur atomically at backend
 * launch and are only revalidated here; this layer does not manufacture them
 * early.
 *
 * As with resident loading, a non-OK result discards the whole lease.
 */
enum dos_image_load_status dos_image_load_prepare_com_stack(
    const struct dos_machine *machine,
    const struct dos_com_process_plan *process_plan,
    const struct dos_memory_lease_view *lease_view) __must_check;

enum dos_image_load_status dos_image_load_prepare_mz_stack(
    const struct dos_machine *machine,
    const struct dos_mz_process_plan *process_plan,
    const struct dos_memory_lease_view *lease_view) __must_check;

#endif

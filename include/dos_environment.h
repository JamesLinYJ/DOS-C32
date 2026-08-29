/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Safe DOS EXEC environment-block planning and construction.
 *
 * An environment source is always a simulated 16:16 address.  EXEC parameter
 * decoding first resolves explicit/inherited/none/overlay cases through
 * dos_exec_parameter_decode_environment_source; this module is invoked only
 * for a selected nonzero segment.  The earlier DStrLen result remains a
 * fixed-width dos_exec_name_plan: its simulated pointer and exact ASCIZ length
 * are bound into this plan, never converted to or retained as a native
 * pointer. Checked arithmetic and explicit poisoned-state reporting do not
 * change guest-visible bytes.
 */
#ifndef DOSC32_DOS_ENVIRONMENT_H
#define DOSC32_DOS_ENVIRONMENT_H

#include "compiler.h"
#include "dos_abi.h"
#include "dos_exec_name.h"
#include "dos_machine.h"
#include "types.h"

#define DOS_ENVIRONMENT_SCAN_LIMIT 0x7fffu
#define DOS_ENVIRONMENT_PARAGRAPH_BYTES 16u
#define DOS_ENVIRONMENT_TRAILER_VALUE 1u

enum dos_environment_status {
	DOS_ENVIRONMENT_OK = 0,
	DOS_ENVIRONMENT_INVALID_ARGUMENT,
	DOS_ENVIRONMENT_BAD_SOURCE,
	DOS_ENVIRONMENT_SOURCE_FAULT,
	DOS_ENVIRONMENT_NAME_NOT_TERMINATED,
	DOS_ENVIRONMENT_RANGE_OVERFLOW,
	DOS_ENVIRONMENT_STALE_PLAN,
	/*
	 * Target validation/I/O failed without a rollback failure.  The whole
	 * unpublished block is still discarded because earlier chunks may have
	 * completed; this status says the machine itself is not poisoned.
	 */
	DOS_ENVIRONMENT_TARGET_FAULT,
	/*
	 * A failed target write could not be rolled back.  The unpublished
	 * allocation must be discarded and its bytes treated as indeterminate.
	 */
	DOS_ENVIRONMENT_TARGET_POISONED
};

/*
 * A fixed-width value object suitable for crossing allocation boundaries.
 * It deliberately contains no native pointer or compiler-sized length.
 */
struct dos_environment_plan {
	struct dos_far_pointer16 source;
	uint32_t environment_bytes;
	struct dos_exec_name_plan executable_name;
	uint32_t payload_bytes;
	uint32_t allocation_bytes;
	uint16_t paragraphs;
	uint16_t reserved;
} __aligned(8);

static_assert_expression(sizeof(struct dos_environment_plan) == 32,
			 "environment plan must be data-model independent");
static_assert_expression(__alignof__(struct dos_environment_plan) == 8,
			 "environment plan alignment changed");
static_assert_expression(__builtin_offsetof(struct dos_environment_plan,
					    executable_name) == 8,
			 "environment name-plan offset changed");

/* Pure fixed-width validation; it performs no guest-memory access. */
bool dos_environment_plan_has_valid_encoding(
    const struct dos_environment_plan *plan) __must_check;

/*
 * Validate the fixed DStrLen result and scan source without modifying guest
 * memory. This stage deliberately does not re-read executable_name: argv[0]
 * is read only after environment allocation and copying.
 * plan is unchanged on every error.
 *
 * The EXEC transaction must serialize the parent guest while planning and
 * building.  Revalidation detects changed length/geometry, but a same-length
 * source mutation is intentionally not treated as an immutable snapshot.
 */
enum dos_environment_status dos_environment_plan_create(
    const struct dos_machine *machine, struct dos_far_pointer16 source,
    const struct dos_exec_name_plan *executable_name,
    struct dos_environment_plan *plan) __must_check;

/*
 * Construct exactly:
 *
 *   inherited strings, NUL, NUL, little-endian word 1, argv[0], NUL
 *
 * EXEC allocates the destination at offset zero, so nonzero offsets are
 * rejected.  The caller owns an isolated, unpublished, non-aliasing block of
 * plan->paragraphs paragraphs and must publish it only after success.  Bytes
 * between payload_bytes and allocation_bytes are intentionally untouched to
 * preserve MS-DOS behavior. Construction uses independently rollback-protected,
 * bounded chunks instead of a large kernel-stack buffer.  The publication
 * boundary is the whole block: any error still requires discarding the
 * allocation, while TARGET_POISONED specifically identifies failed rollback.
 * After copying the environment and little-endian word 1, construction
 * re-reads exactly executable_name.bytes_including_nul guest bytes at the
 * defined late-copy point. It stops on an early NUL, never fetches past
 * it, and rejects a missing final NUL as a stale plan.  A late guest-name fault
 * is TARGET_FAULT because the isolated target has already been modified.  The
 * source environment and executable-name bytes must remain stable for the
 * call; the enclosing EXEC transaction provides that lifetime guarantee.
 */
enum dos_environment_status dos_environment_build(
    const struct dos_machine *machine, const struct dos_environment_plan *plan,
    struct dos_far_pointer16 destination) __must_check;

#endif

/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Bounded EXEC-name acquisition and DOS MCB owner-name translation.
 *
 * DOS semantics remain byte-oriented: only ':' and '\\' delimit path
 * components, '/' is an ordinary byte, case is preserved, and the first dot
 * in the final component ends the MCB name.  This boundary emits at most the
 * exact eight-byte field and never writes beyond arena_name.
 */
#ifndef DOSC32_DOS_EXEC_NAME_H
#define DOSC32_DOS_EXEC_NAME_H

#include "compiler.h"
#include "dos_abi.h"
#include "dos_machine.h"
#include "dos_memory.h"
#include "types.h"

#define DOS_EXEC_NAME_SCAN_LIMIT 0xffffu
/* The contiguous internal open and command path buffers are 128 bytes. */
#define DOS_EXEC_PATH_CAPACITY 128u

enum dos_exec_name_status {
	DOS_EXEC_NAME_OK = 0,
	DOS_EXEC_NAME_INVALID_ARGUMENT = 1,
	DOS_EXEC_NAME_INVALID_NAME = 2,
	DOS_EXEC_NAME_GUEST_FAULT = 3,
	DOS_EXEC_NAME_BUFFER_TOO_SMALL = 4
};

/*
 * Persistent DStrLen result.  source is always a simulated 16:16 pointer;
 * bytes_including_nul is 1..0xffff and reserved is zero.  The value contains
 * no native pointer, size_t, enum or bool and is safe to retain across EXEC
 * stages.
 */
struct dos_exec_name_plan {
	struct dos_far_pointer16 source;
	uint16_t bytes_including_nul;
	uint16_t reserved;
} __aligned(8);

static_assert_expression(sizeof(struct dos_exec_name_plan) == 8,
			 "EXEC name plans must remain fixed-width");
static_assert_expression(__alignof__(struct dos_exec_name_plan) == 8,
			 "EXEC name plan alignment changed");
static_assert_expression(__builtin_offsetof(struct dos_exec_name_plan,
					    bytes_including_nul) == 4,
			 "EXEC name plan length offset changed");

bool dos_exec_name_plan_has_valid_encoding(
    const struct dos_exec_name_plan *plan) __must_check;

/*
 * Translate DStrLen without treating a guest integer as a native pointer.
 * At most DOS_EXEC_NAME_SCAN_LIMIT bytes are fetched, one byte at a time, so
 * an inaccessible byte after the first NUL is never observed.  DI-style
 * 16-bit offset wrap and A20 policy are provided by dos_machine_read_far().
 *
 * scratch is caller-owned work space.  It contains the exact ASCIZ span on
 * success and may contain a partial prefix on error.  plan is unchanged on
 * every error.  A capacity shorter than the guest name reports
 * BUFFER_TOO_SMALL without reading beyond that capacity; a full 0xffff-byte
 * unterminated source reports INVALID_NAME.
 */
enum dos_exec_name_status dos_exec_name_read_guest(
    const struct dos_machine *machine, struct dos_far_pointer16 source,
    uint8_t *scratch, size_t scratch_capacity,
    struct dos_exec_name_plan *plan) __must_check;

/*
 * length_including_nul must describe one canonical byte string inside
 * capacity: the last byte is NUL and no earlier byte is NUL.  On success,
 * patch->count is 1..8 and all reserved bytes are zero.  A short basename
 * includes one terminating NUL in the patch; an eight-byte prefix does not.
 * Bytes after patch->count are canonical zeroes, but the memory layer uses
 * count and therefore preserves the untouched tail of arena_name.
 *
 * patch is unchanged on every error.  No native or guest pointer is retained.
 */
enum dos_exec_name_status dos_exec_name_build_owner_patch(
    const uint8_t *name, size_t capacity, size_t length_including_nul,
    struct dos_memory_owner_name_patch *patch) __must_check;

#endif

/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Bounded MZ relocation boundary.
 *
 * Compatibility contract: relocation entries are little-endian offset:segment pairs;
 *                 the relocation factor selects the target segment and is
 *                 added modulo 16 bits to each target word.
 * Safety changes: validate the complete table before changing guest memory,
 *                 use a fixed-size read buffer and transactional word writes,
 *                 and never retain a native data pointer.  CPU registers are
 *                 deliberately outside this interface.
 */
#ifndef DOSC32_DOS_RELOCATOR_H
#define DOSC32_DOS_RELOCATOR_H

#include "compiler.h"
#include "dos_loader.h"
#include "dos_machine.h"
#include "types.h"

enum dos_relocator_status {
	DOS_RELOCATOR_OK = 0,
	DOS_RELOCATOR_INVALID_ARGUMENT,
	DOS_RELOCATOR_FILE_RANGE_OVERFLOW,
	DOS_RELOCATOR_BAD_FILE_RANGE,
	DOS_RELOCATOR_IMAGE_IO_ERROR,
	DOS_RELOCATOR_IMAGE_SHORT_READ,
	DOS_RELOCATOR_BAD_RESIDENT_RANGE,
	DOS_RELOCATOR_BAD_TARGET_OFFSET,
	DOS_RELOCATOR_TARGET_OUTSIDE_RESIDENT,
	DOS_RELOCATOR_MACHINE_FAULT,
	/* A failed word rollback leaves the machine indeterminate. */
	DOS_RELOCATOR_MACHINE_POISONED
};

/*
 * relocation_table_offset is an absolute seek from the start of the image,
 * exactly like EXEC's CX:DX=0:exec_rle_table call to $LSEEK.  The private
 * 26-byte header read does not constrain that later seek: the table may start
 * before, at, or after the private-header end.  Checked table arithmetic and
 * reader.size bound every nonempty table in the immutable file snapshot
 * instead.  A zero-entry table may retain its decoded 16-bit seek beyond EOF,
 * matching local $LSEEK without naming any bytes to read.
 *
 * The table segment plus relocation factor follows EXEC's 16-bit ADD and
 * therefore wraps modulo 10000h.  resident_linear_address/resident_size
 * identify an isolated, not-yet-published load block.  On an application
 * fault, the caller must discard
 * the complete block rather than expose its partially relocated contents.
 * Overlay callers must pass their explicit relocation factor; this module
 * does not infer overlay layout or synthesize any additional semantics.
 *
 * reader->context is the existing canonical 64-bit object handle.  Its
 * backend contract MUST pin an immutable file snapshot for this call; the
 * relocator never retains the reader or a native pointer after returning.
 */
struct dos_relocator_request {
	file_offset_t relocation_table_offset;
	uint64_t resident_size;
	dos_linear_address_t resident_linear_address;
	uint16_t relocation_count;
	uint16_t relocation_factor;
} __aligned(8);

static_assert_expression(sizeof(struct dos_relocator_request) == 24,
			 "relocator request must be data-model independent");
static_assert_expression(__alignof__(struct dos_relocator_request) == 8,
			 "relocator request must remain explicitly aligned");
static_assert_expression(__builtin_offsetof(struct dos_relocator_request,
					    resident_size) == 8,
			 "relocator resident-size offset changed");
static_assert_expression(__builtin_offsetof(struct dos_relocator_request,
					    relocation_count) == 20,
			 "relocator count offset changed");

struct dos_relocator_result {
	uint16_t validated_entries;
	uint16_t applied_entries;
};

/*
 * result is published only after complete success and is unchanged on every
 * error.  Validation errors leave the resident block byte-for-byte unchanged.
 * Once the application pass starts, an I/O or machine fault may leave earlier
 * words relocated in the isolated block, so every non-OK result discards that
 * block.  An ordinary write fault restores its failing word.  MACHINE_POISONED
 * means even that word could not be restored and the backend must stop.
 * Second-pass entries are range-checked again, so even a reader-backend
 * contract violation cannot direct a write outside resident.  This range
 * check is defense in depth, not a replacement for the immutable-snapshot
 * requirement above: a changed but still valid entry cannot be identified as
 * the same entry without a pinned reader generation.
 */
enum dos_relocator_status
dos_relocator_apply(const struct dos_image_reader *reader,
		    const struct dos_machine *machine,
		    const struct dos_relocator_request *request,
		    struct dos_relocator_result *result) __must_check;

#endif

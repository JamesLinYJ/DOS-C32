/* SPDX-License-Identifier: GPL-2.0-only */
/* Bounded read-only view of the current PSP environment. */
#ifndef DOSC32_DOS_ENVIRONMENT_VIEW_H
#define DOSC32_DOS_ENVIRONMENT_VIEW_H

#include "compiler.h"
#include "dos_machine.h"
#include "types.h"

#define DOS_ENVIRONMENT_VIEW_READ_BYTES 128u

enum dos_environment_view_status {
	DOS_ENVIRONMENT_VIEW_OK = 0,
	DOS_ENVIRONMENT_VIEW_INVALID_ARGUMENT,
	DOS_ENVIRONMENT_VIEW_NOT_FOUND,
	DOS_ENVIRONMENT_VIEW_BAD_PSP,
	DOS_ENVIRONMENT_VIEW_BAD_BLOCK,
	DOS_ENVIRONMENT_VIEW_MACHINE_FAULT
};

/*
 * Find NAME case-insensitively in PSP:2Ch's environment and read one value
 * range.  value_length is the complete value length; bytes_read is the
 * amount copied at value_offset.  Both outputs and destination remain
 * unpublished on failure.  A zero-capacity read is a length-only query.
 * Each read is bounded to DOS_ENVIRONMENT_VIEW_READ_BYTES so callers can
 * stream an arbitrarily sized DOS environment value without a large stack.
 */
enum dos_environment_view_status dos_environment_view_read_value(
	const struct dos_machine *machine, uint16_t psp_segment,
	const uint8_t *name, size_t name_length, uint32_t value_offset,
	uint8_t *destination, size_t destination_capacity,
	size_t *value_length, size_t *bytes_read) __must_check;

#endif

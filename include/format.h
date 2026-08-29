/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DOSC32_FORMAT_H
#define DOSC32_FORMAT_H

#include <stdarg.h>

#include "compiler.h"
#include "types.h"

enum format_status {
	FORMAT_OK = 0,
	FORMAT_TRUNCATED,
	FORMAT_INVALID_ARGUMENT,
	FORMAT_INVALID_FORMAT,
	FORMAT_RANGE_ERROR
};

/*
 * Format a complete, terminated string into destination.
 *
 * destination_capacity includes the terminating NUL.  format_capacity is
 * the readable size of format and must include a NUL.  On FORMAT_OK,
 * *required_length is the number of output bytes excluding the NUL.  On
 * FORMAT_TRUNCATED it is the capacity that would have been required,
 * excluding the NUL.  Other failures set it to zero.  A non-success result
 * leaves destination as an empty string.
 *
 * Supported conversions are %c, %.Ns/%.*s, %u, %d, %x, %X, %p, and %%.
 * %p follows the native compiler ABI width while DOS guest pointers remain
 * explicit 16:16 values in dos_abi.h.
 * An unqualified %s is rejected because a variadic pointer does not carry a
 * readable capacity.  N or the value supplied to * is that explicit bound.
 */
enum format_status vsnprintf_s(char *destination,
			       size_t destination_capacity,
			       size_t *required_length,
			       const char *format, size_t format_capacity,
			       va_list arguments) __must_check;
enum format_status snprintf_s(char *destination,
			      size_t destination_capacity,
			      size_t *required_length,
			      const char *format, size_t format_capacity,
			      ...) __must_check;

#endif

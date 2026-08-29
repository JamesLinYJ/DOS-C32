/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DOSC32_CONVERT_H
#define DOSC32_CONVERT_H

#include "compiler.h"
#include "types.h"

enum convert_status {
	CONVERT_OK = 0,
	CONVERT_INVALID_ARGUMENT,
	CONVERT_EMPTY,
	CONVERT_INVALID_DIGIT,
	CONVERT_OVERFLOW,
	CONVERT_TRUNCATED
};

/* Input is an exact bounded string view; no terminating NUL is required. */
enum convert_status parse_u64_s(const char *text, size_t length,
				uint32_t base, uint64_t *value,
				size_t *consumed) __must_check;
enum convert_status parse_i64_s(const char *text, size_t length,
				uint32_t base, int64_t *value,
				size_t *consumed) __must_check;
enum convert_status format_u64_s(char *destination,
				 size_t destination_capacity,
				 uint64_t value, uint32_t base,
				 bool uppercase,
				 size_t *required_length) __must_check;
enum convert_status format_i64_s(char *destination,
				 size_t destination_capacity,
				 int64_t value, uint32_t base,
				 size_t *required_length) __must_check;

#endif

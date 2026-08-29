/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DOSC32_MATH64_H
#define DOSC32_MATH64_H

#include "compiler.h"
#include "types.h"

enum math64_status {
	MATH64_OK = 0,
	MATH64_INVALID_ARGUMENT,
	MATH64_DIVIDE_BY_ZERO,
	MATH64_OVERFLOW
};

/* Outputs are cleared before validation and published together on success. */
enum math64_status math64_div_u64(uint64_t dividend, uint64_t divisor,
				  uint64_t *quotient,
				  uint64_t *remainder) __must_check;
enum math64_status math64_div_u64_u32(uint64_t dividend, uint32_t divisor,
				      uint64_t *quotient,
				      uint32_t *remainder) __must_check;
enum math64_status math64_div_i64(int64_t dividend, int64_t divisor,
				  int64_t *quotient,
				  int64_t *remainder) __must_check;

#endif

/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Checked arithmetic interface. The helpers are internal implementation
 * machinery and define no DOS ABI.
 * Modified by DOS-C32 contributors, 2026.
 */
#ifndef DOSC32_OVERFLOW_H
#define DOSC32_OVERFLOW_H

#include "types.h"

#define check_add_overflow(left, right, result) \
	__builtin_add_overflow((left), (right), (result))
#define check_sub_overflow(left, right, result) \
	__builtin_sub_overflow((left), (right), (result))
#define check_mul_overflow(left, right, result) \
	__builtin_mul_overflow((left), (right), (result))

static inline bool range_overflows_u32(uint32_t start, uint32_t size,
				       uint32_t exclusive_limit)
{
	return start >= exclusive_limit || size > exclusive_limit - start;
}

static inline bool range_overflows_u64(uint64_t start, uint64_t size,
				       uint64_t exclusive_limit)
{
	return start >= exclusive_limit || size > exclusive_limit - start;
}

static inline bool range_overflows_size(size_t start, size_t size,
					size_t exclusive_limit)
{
	return start >= exclusive_limit || size > exclusive_limit - start;
}

#endif

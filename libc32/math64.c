// SPDX-License-Identifier: GPL-2.0-only
/*
 * Freestanding 64-bit division for 32-bit DOS-C32 targets.
 *
 * Restoring division deliberately uses only shifts, comparisons, addition,
 * and subtraction so GCC cannot
 * recursively lower it to the compiler helpers defined at the end.
 */
#include "math64.h"

static uint64_t divide_unsigned(uint64_t dividend, uint64_t divisor,
				uint64_t *remainder)
{
	uint64_t quotient = 0u;
	uint64_t current_remainder = 0u;
	uint32_t bit = 64u;

	while (bit != 0u) {
		bool carry;

		--bit;
		carry = (current_remainder >> 63) != 0u;
		current_remainder =
			(current_remainder << 1) | ((dividend >> bit) & 1u);
		if (carry || current_remainder >= divisor) {
			current_remainder -= divisor;
			quotient |= (uint64_t)1u << bit;
		}
	}
	*remainder = current_remainder;
	return quotient;
}

enum math64_status math64_div_u64(uint64_t dividend, uint64_t divisor,
				  uint64_t *quotient, uint64_t *remainder)
{
	uint64_t result;
	uint64_t residual;

	if (quotient != NULL)
		*quotient = 0u;
	if (remainder != NULL)
		*remainder = 0u;
	if (quotient == NULL || remainder == NULL)
		return MATH64_INVALID_ARGUMENT;
	if (divisor == 0u)
		return MATH64_DIVIDE_BY_ZERO;
	result = divide_unsigned(dividend, divisor, &residual);
	*quotient = result;
	*remainder = residual;
	return MATH64_OK;
}

enum math64_status math64_div_u64_u32(uint64_t dividend, uint32_t divisor,
				      uint64_t *quotient,
				      uint32_t *remainder)
{
	uint64_t residual;
	uint64_t result;
	enum math64_status status;

	if (quotient != NULL)
		*quotient = 0u;
	if (remainder != NULL)
		*remainder = 0u;
	if (quotient == NULL || remainder == NULL)
		return MATH64_INVALID_ARGUMENT;
	status = math64_div_u64(dividend, divisor, &result, &residual);
	if (status != MATH64_OK)
		return status;
	*quotient = result;
	*remainder = (uint32_t)residual;
	return MATH64_OK;
}

enum math64_status math64_div_i64(int64_t dividend, int64_t divisor,
				  int64_t *quotient, int64_t *remainder)
{
	const int64_t minimum = (-9223372036854775807ll - 1ll);
	uint64_t unsigned_dividend;
	uint64_t unsigned_divisor;
	uint64_t unsigned_quotient;
	uint64_t unsigned_remainder;
	bool quotient_negative;
	bool remainder_negative;

	if (quotient != NULL)
		*quotient = 0;
	if (remainder != NULL)
		*remainder = 0;
	if (quotient == NULL || remainder == NULL)
		return MATH64_INVALID_ARGUMENT;
	if (divisor == 0)
		return MATH64_DIVIDE_BY_ZERO;
	if (dividend == minimum && divisor == -1)
		return MATH64_OVERFLOW;
	quotient_negative = (dividend < 0) != (divisor < 0);
	remainder_negative = dividend < 0;
	unsigned_dividend = (uint64_t)dividend;
	if (remainder_negative)
		unsigned_dividend = 0u - unsigned_dividend;
	unsigned_divisor = (uint64_t)divisor;
	if (divisor < 0)
		unsigned_divisor = 0u - unsigned_divisor;
	unsigned_quotient = divide_unsigned(unsigned_dividend,
					    unsigned_divisor,
					    &unsigned_remainder);
	if (quotient_negative)
		unsigned_quotient = 0u - unsigned_quotient;
	if (remainder_negative)
		unsigned_remainder = 0u - unsigned_remainder;
	*quotient = (int64_t)unsigned_quotient;
	*remainder = (int64_t)unsigned_remainder;
	return MATH64_OK;
}

/* GCC i386 compiler-runtime ABI.  Checked code should use math64_div_*(). */
uint64_t __udivmoddi4(uint64_t dividend, uint64_t divisor,
			      uint64_t *remainder);
uint64_t __udivdi3(uint64_t dividend, uint64_t divisor);
uint64_t __umoddi3(uint64_t dividend, uint64_t divisor);
int64_t __divdi3(int64_t dividend, int64_t divisor);
int64_t __moddi3(int64_t dividend, int64_t divisor);

uint64_t __udivmoddi4(uint64_t dividend, uint64_t divisor,
			      uint64_t *remainder)
{
	uint64_t ignored_remainder;

	if (divisor == 0u)
		__builtin_trap();
	if (remainder == NULL)
		remainder = &ignored_remainder;
	return divide_unsigned(dividend, divisor, remainder);
}

uint64_t __udivdi3(uint64_t dividend, uint64_t divisor)
{
	uint64_t remainder;

	return __udivmoddi4(dividend, divisor, &remainder);
}

uint64_t __umoddi3(uint64_t dividend, uint64_t divisor)
{
	uint64_t remainder;

	(void)__udivmoddi4(dividend, divisor, &remainder);
	return remainder;
}

int64_t __divdi3(int64_t dividend, int64_t divisor)
{
	int64_t quotient;
	int64_t remainder;

	if (math64_div_i64(dividend, divisor, &quotient, &remainder) !=
	    MATH64_OK)
		__builtin_trap();
	return quotient;
}

int64_t __moddi3(int64_t dividend, int64_t divisor)
{
	int64_t quotient;
	int64_t remainder;

	if (math64_div_i64(dividend, divisor, &quotient, &remainder) !=
	    MATH64_OK)
		__builtin_trap();
	return remainder;
}

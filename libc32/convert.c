// SPDX-License-Identifier: GPL-2.0-only
/*
 * Bounded integer conversion for libc32-core.
 *
 * This replaces atoi-style interfaces.  The implementation avoids compiler
 * 64-bit division helpers so the same implementation links in the i386 kernel
 * and the future x86_64 runtime.
 */
#include "convert.h"

#include "math64.h"
#include "overflow.h"

static bool decode_digit(char character, uint32_t *digit)
{
	if (character >= '0' && character <= '9') {
		*digit = (uint32_t)(character - '0');
		return true;
	}
	if (character >= 'A' && character <= 'Z') {
		*digit = (uint32_t)(character - 'A') + 10u;
		return true;
	}
	if (character >= 'a' && character <= 'z') {
		*digit = (uint32_t)(character - 'a') + 10u;
		return true;
	}
	return false;
}

static bool multiply_u64_small(uint64_t value, uint32_t multiplier,
			       uint64_t *result)
{
	uint64_t product = 0u;
	uint64_t addend = value;
	uint32_t remaining = multiplier;

	while (remaining != 0u) {
		if ((remaining & 1u) != 0u &&
		    check_add_overflow(product, addend, &product))
			return false;
		remaining >>= 1;
		if (remaining != 0u &&
		    check_add_overflow(addend, addend, &addend))
			return false;
	}
	*result = product;
	return true;
}

static enum convert_status parse_magnitude(const char *text, size_t length,
					   uint32_t base,
					   uint64_t limit,
					   uint64_t *value,
					   size_t *consumed)
{
	uint64_t result = 0u;
	size_t index;

	for (index = 0u; index < length; ++index) {
		uint64_t multiplied;
		uint32_t digit;

		if (!decode_digit(text[index], &digit) || digit >= base) {
			*consumed = index;
			return CONVERT_INVALID_DIGIT;
		}
		if (!multiply_u64_small(result, base, &multiplied) ||
		    check_add_overflow(multiplied, (uint64_t)digit, &result) ||
		    result > limit) {
			*consumed = index;
			return CONVERT_OVERFLOW;
		}
	}
	*value = result;
	*consumed = length;
	return CONVERT_OK;
}

enum convert_status parse_u64_s(const char *text, size_t length,
				uint32_t base, uint64_t *value,
				size_t *consumed)
{
	if (value != NULL)
		*value = 0u;
	if (consumed != NULL)
		*consumed = 0u;
	if (text == NULL || value == NULL || consumed == NULL || base < 2u ||
	    base > 36u)
		return CONVERT_INVALID_ARGUMENT;
	if (length == 0u)
		return CONVERT_EMPTY;
	return parse_magnitude(text, length, base, (uint64_t)-1, value,
			       consumed);
}

enum convert_status parse_i64_s(const char *text, size_t length,
				uint32_t base, int64_t *value,
				size_t *consumed)
{
	const uint64_t negative_limit = (uint64_t)1u << 63;
	const uint64_t positive_limit = negative_limit - 1u;
	uint64_t magnitude;
	size_t digits_consumed;
	size_t start = 0u;
	bool negative = false;
	enum convert_status status;

	if (value != NULL)
		*value = 0;
	if (consumed != NULL)
		*consumed = 0u;
	if (text == NULL || value == NULL || consumed == NULL || base < 2u ||
	    base > 36u)
		return CONVERT_INVALID_ARGUMENT;
	if (length == 0u)
		return CONVERT_EMPTY;
	if (text[0] == '-' || text[0] == '+') {
		negative = text[0] == '-';
		start = 1u;
	}
	if (start == length) {
		*consumed = start;
		return CONVERT_EMPTY;
	}
	status = parse_magnitude(text + start, length - start, base,
				 negative ? negative_limit : positive_limit,
				 &magnitude, &digits_consumed);
	*consumed = start + digits_consumed;
	if (status != CONVERT_OK)
		return status;
	if (negative) {
		if (magnitude == negative_limit)
			*value = (-9223372036854775807ll - 1ll);
		else
			*value = -(int64_t)magnitude;
	} else {
		*value = (int64_t)magnitude;
	}
	return CONVERT_OK;
}

static enum convert_status format_magnitude(char *destination,
					    size_t destination_capacity,
					    uint64_t value, uint32_t base,
					    bool uppercase,
					    bool negative,
					    size_t *required_length)
{
	const char *digits = uppercase ? "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ" :
					  "0123456789abcdefghijklmnopqrstuvwxyz";
	char reversed[65];
	size_t count = 0u;
	size_t index;

	do {
		uint64_t quotient;
		uint32_t remainder;

		if (math64_div_u64_u32(value, base, &quotient, &remainder) !=
		    MATH64_OK)
			return CONVERT_INVALID_ARGUMENT;

		reversed[count++] = digits[remainder];
		value = quotient;
	} while (value != 0u);
	*required_length = count + (negative ? 1u : 0u);
	if (*required_length >= destination_capacity)
		return CONVERT_TRUNCATED;
	index = 0u;
	if (negative)
		destination[index++] = '-';
	while (count != 0u)
		destination[index++] = reversed[--count];
	destination[index] = '\0';
	return CONVERT_OK;
}

enum convert_status format_u64_s(char *destination,
				 size_t destination_capacity,
				 uint64_t value, uint32_t base,
				 bool uppercase, size_t *required_length)
{
	if (required_length != NULL)
		*required_length = 0u;
	if (destination != NULL && destination_capacity != 0u)
		destination[0] = '\0';
	if (destination == NULL || destination_capacity == 0u ||
	    required_length == NULL || base < 2u || base > 36u)
		return CONVERT_INVALID_ARGUMENT;
	return format_magnitude(destination, destination_capacity, value, base,
				uppercase, false, required_length);
}

enum convert_status format_i64_s(char *destination,
				 size_t destination_capacity,
				 int64_t value, uint32_t base,
				 size_t *required_length)
{
	bool negative = value < 0;
	uint64_t magnitude = (uint64_t)value;

	if (negative)
		magnitude = 0u - magnitude;
	if (required_length != NULL)
		*required_length = 0u;
	if (destination != NULL && destination_capacity != 0u)
		destination[0] = '\0';
	if (destination == NULL || destination_capacity == 0u ||
	    required_length == NULL || base < 2u || base > 36u)
		return CONVERT_INVALID_ARGUMENT;
	return format_magnitude(destination, destination_capacity, magnitude,
				base, false, negative, required_length);
}

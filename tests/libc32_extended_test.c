// SPDX-License-Identifier: GPL-2.0-only
/* Freestanding tests for 64-bit conversion and canonical-address arenas. */
#include "address.h"
#include "test_entry.h"
#include "arena.h"
#include "convert.h"
#include "dosc32_assert.h"
#include "math64.h"

static bool text_equals(const char *left, const char *right, size_t length)
{
	size_t index;

	for (index = 0u; index < length; ++index) {
		if (left[index] != right[index])
			return false;
	}
	return left[length] == '\0';
}

static int test_conversion(void)
{
	static const char maximum[] = "18446744073709551615";
	static const char overflow[] = "18446744073709551616";
	static const char minimum[] = "-9223372036854775808";
	char output[32];
	char small[4] = { 'x', 'x', 'x', 'x' };
	uint64_t unsigned_value;
	int64_t signed_value;
	size_t count;

	if (parse_u64_s(maximum, ARRAY_SIZE(maximum) - 1u, 10u,
			&unsigned_value, &count) != CONVERT_OK ||
	    unsigned_value != (uint64_t)-1 || count != 20u)
		return 1;
	if (parse_u64_s(overflow, ARRAY_SIZE(overflow) - 1u, 10u,
			&unsigned_value, &count) != CONVERT_OVERFLOW ||
	    unsigned_value != 0u)
		return 2;
	if (parse_u64_s("12x", 3u, 10u, &unsigned_value, &count) !=
	    CONVERT_INVALID_DIGIT || count != 2u)
		return 3;
	if (parse_i64_s(minimum, ARRAY_SIZE(minimum) - 1u, 10u,
			&signed_value, &count) != CONVERT_OK ||
	    signed_value != (-9223372036854775807ll - 1ll))
		return 4;
	if (parse_i64_s("9223372036854775808", 19u, 10u, &signed_value,
			&count) != CONVERT_OVERFLOW)
		return 5;
	if (format_u64_s(output, sizeof(output), (uint64_t)-1, 10u, false,
			 &count) != CONVERT_OK || count != 20u ||
	    !text_equals(output, maximum, 20u))
		return 6;
	if (format_i64_s(output, sizeof(output),
			 (-9223372036854775807ll - 1ll), 10u,
			 &count) != CONVERT_OK || count != 20u ||
	    !text_equals(output, minimum, 20u))
		return 7;
	if (format_u64_s(output, sizeof(output), 0xabcdefu, 16u, true,
			 &count) != CONVERT_OK ||
	    !text_equals(output, "ABCDEF", 6u))
		return 8;
	if (format_u64_s(small, sizeof(small), 12345u, 10u, false,
			 &count) != CONVERT_TRUNCATED || count != 5u ||
	    small[0] != '\0')
		return 9;
	return 0;
}

static int test_arena(void)
{
	struct address_arena arena;
	kernel_address_t address;
	uint64_t checkpoint;

	if (address_arena_initialize(&arena, 0x1003u, 0x100u) != ARENA_OK)
		return 20;
	if (address_arena_allocate(&arena, 0x10u, 0x10u, &address) !=
	    ARENA_OK || address != 0x1010u)
		return 21;
	checkpoint = address_arena_checkpoint(&arena);
	if (address_arena_allocate(&arena, 0x20u, 0x20u, &address) !=
	    ARENA_OK || address != 0x1020u)
		return 22;
	if (address_arena_rewind(&arena, checkpoint) != ARENA_OK)
		return 23;
	if (address_arena_allocate(&arena, 0x100u, 1u, &address) !=
	    ARENA_OUT_OF_SPACE || address != KERNEL_ADDRESS_INVALID)
		return 24;
	if (address_arena_allocate(&arena, 1u, 3u, &address) !=
	    ARENA_INVALID_ARGUMENT)
		return 25;
	if (address_arena_initialize(&arena, (kernel_address_t)-8, 16u) !=
	    ARENA_RANGE_OVERFLOW)
		return 26;
	return 0;
}

static int test_math64(void)
{
	uint64_t quotient;
	uint64_t remainder;
	uint32_t remainder32;
	int64_t signed_quotient;
	int64_t signed_remainder;

	if (math64_div_u64((uint64_t)-1, 3u, &quotient, &remainder) !=
	    MATH64_OK || quotient != 6148914691236517205ull ||
	    remainder != 0u)
		return 40;
	if (math64_div_u64((uint64_t)-1, ((uint64_t)1u << 63) + 1u,
			   &quotient, &remainder) != MATH64_OK || quotient != 1u ||
	    remainder != (((uint64_t)1u << 63) - 2u))
		return 41;
	if (math64_div_u64_u32(0x100000001ull, 512u, &quotient,
			       &remainder32) != MATH64_OK ||
	    quotient != 0x800000u || remainder32 != 1u)
		return 42;
	if (math64_div_i64(-9223372036854775807ll, 97,
			   &signed_quotient, &signed_remainder) != MATH64_OK ||
	    signed_quotient != -95086309658296657ll ||
	    signed_remainder != -78)
		return 43;
	quotient = 9u;
	remainder = 9u;
	if (math64_div_u64(1u, 0u, &quotient, &remainder) !=
	    MATH64_DIVIDE_BY_ZERO || quotient != 0u || remainder != 0u)
		return 44;
	if (math64_div_i64((-9223372036854775807ll - 1ll), -1,
			   &signed_quotient, &signed_remainder) !=
	    MATH64_OVERFLOW || signed_quotient != 0 || signed_remainder != 0)
		return 45;
	return 0;
}

static int run_tests(void)
{
	int status = test_conversion();
	unsigned int evaluated = 0u;

	DOSC32_ASSERT(++evaluated == 1u);
	if (evaluated != 1u)
		return 30;

	if (status != 0)
		return status;
	status = test_arena();
	return status != 0 ? status : test_math64();
}

DOSC32_TEST_ENTRY(run_tests)

// SPDX-License-Identifier: GPL-2.0-only
/*
 * Freestanding libc32-core regression tests.
 *
 * The test body deliberately has no dependency on a hosted C library.  A
 * platform entry wrapper returns the first failed check through either the
 * macOS process runtime or the freestanding x86 exit boundary.  The same
 * test body is built for DOS-C32's i386 and native 64-bit data models.
 */
#include "overflow.h"
#include "test_entry.h"
#include "string.h"

#define UINT32_MAX_VALUE ((uint32_t)0xffffffffu)

static int test_memcpy_s(void);
static int test_memmove_s(void);
static int test_memset_s(void);
static int test_memzero_explicit_s(void);
static int test_memcpy_and_pad_s(void);
static int test_strtomem_pad_s(void);
static int test_strscpy_s(void);
static int test_strnlen(void);
static int test_memcmp_s(void);
static int test_checked_arithmetic(void);
static int run_tests(void);
static bool bytes_equal(const uint8_t *left, const uint8_t *right,
			 size_t count)
{
	size_t index;

	for (index = 0; index < count; ++index) {
		if (left[index] != right[index])
			return false;
	}
	return true;
}

static int test_memcpy_s(void)
{
	uint8_t source[8] = { 0x10, 0x21, 0x32, 0x43,
			      0x54, 0x65, 0x76, 0x87 };
	uint8_t destination[8] = { 0xa0, 0xa1, 0xa2, 0xa3,
				   0xa4, 0xa5, 0xa6, 0xa7 };
	uint8_t original[8] = { 0xa0, 0xa1, 0xa2, 0xa3,
				0xa4, 0xa5, 0xa6, 0xa7 };
	uint8_t overlap[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
	uint8_t overlap_original[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };

	if (memcpy_s(NULL, 0, NULL, 0, 0) != MEMORY_OK)
		return 10;
	if (memcpy_s(destination, ARRAY_SIZE(destination), source,
		     ARRAY_SIZE(source), ARRAY_SIZE(source)) != MEMORY_OK)
		return 11;
	if (!bytes_equal(destination, source, ARRAY_SIZE(source)))
		return 12;

	if (memcpy_s(overlap + 1, 7, overlap, 8, 4) != MEMORY_OVERLAP)
		return 13;
	if (!bytes_equal(overlap, overlap_original, ARRAY_SIZE(overlap)))
		return 14;
	if (memcpy_s(overlap, 8, overlap + 1, 7, 4) != MEMORY_OVERLAP)
		return 15;
	if (memcpy_s(overlap, 8, overlap, 8, 1) != MEMORY_OVERLAP)
		return 16;

	if (memcpy_s(original, 3, source, 8, 4) != MEMORY_OUT_OF_BOUNDS)
		return 17;
	if (original[0] != 0xa0 || original[1] != 0xa1 ||
	    original[2] != 0xa2 || original[3] != 0xa3)
		return 18;
	if (memcpy_s(original, 8, source, 3, 4) != MEMORY_OUT_OF_BOUNDS)
		return 19;
	if (memcpy_s(NULL, 8, source, 8, 1) != MEMORY_INVALID_ARGUMENT)
		return 20;
	if (memcpy_s(original, 8, NULL, 8, 1) != MEMORY_INVALID_ARGUMENT)
		return 21;

	/* Address-range overflow must be rejected before either address is used. */
	if (memcpy_s((void *)(uintptr_t)-4, 8, source, 8, 8) !=
	    MEMORY_OUT_OF_BOUNDS)
		return 22;
	if (memcpy_s(original, 8,
		     (const void *)(uintptr_t)-4, 8, 8) !=
	    MEMORY_OUT_OF_BOUNDS)
		return 23;

	return 0;
}

static int test_memmove_s(void)
{
	uint8_t forward[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
	uint8_t forward_expected[8] = { 1, 2, 3, 4, 5, 6, 6, 7 };
	uint8_t backward[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
	uint8_t backward_expected[8] = { 0, 0, 1, 2, 3, 4, 5, 7 };
	uint8_t same[4] = { 9, 8, 7, 6 };
	uint8_t same_expected[4] = { 9, 8, 7, 6 };
	uint8_t bounded[4] = { 0xa0, 0xa1, 0xa2, 0xa3 };
	uint8_t bounded_expected[4] = { 0xa0, 0xa1, 0xa2, 0xa3 };
	uint8_t source[4] = { 1, 2, 3, 4 };

	/* Destination below source exercises memmove's forward-copy path. */
	if (memmove_s(forward, ARRAY_SIZE(forward), forward + 1, 7, 6) !=
	    MEMORY_OK)
		return 100;
	if (!bytes_equal(forward, forward_expected, ARRAY_SIZE(forward)))
		return 101;

	/* Destination above source must copy backward to preserve overlap. */
	if (memmove_s(backward + 1, 7, backward, ARRAY_SIZE(backward), 6) !=
	    MEMORY_OK)
		return 102;
	if (!bytes_equal(backward, backward_expected, ARRAY_SIZE(backward)))
		return 103;

	if (memmove_s(same, ARRAY_SIZE(same), same, ARRAY_SIZE(same),
		      ARRAY_SIZE(same)) != MEMORY_OK)
		return 104;
	if (!bytes_equal(same, same_expected, ARRAY_SIZE(same)))
		return 105;

	if (memmove_s(bounded, 3, source, ARRAY_SIZE(source), 4) !=
	    MEMORY_OUT_OF_BOUNDS)
		return 106;
	if (!bytes_equal(bounded, bounded_expected, ARRAY_SIZE(bounded)))
		return 107;
	if (memmove_s(bounded, ARRAY_SIZE(bounded), source, 3, 4) !=
	    MEMORY_OUT_OF_BOUNDS)
		return 108;
	if (!bytes_equal(bounded, bounded_expected, ARRAY_SIZE(bounded)))
		return 109;

	if (memmove_s(NULL, 0, NULL, 0, 0) != MEMORY_OK)
		return 110;
	if (memmove_s(NULL, 4, source, ARRAY_SIZE(source), 1) !=
	    MEMORY_INVALID_ARGUMENT)
		return 111;
	if (memmove_s(bounded, ARRAY_SIZE(bounded), NULL, 4, 1) !=
	    MEMORY_INVALID_ARGUMENT)
		return 112;

	/* Both endpoint additions are checked before memmove dereferences them. */
	if (memmove_s((void *)(uintptr_t)-4, 8, source,
		      ARRAY_SIZE(source), 4) != MEMORY_OUT_OF_BOUNDS)
		return 113;
	if (memmove_s(bounded, ARRAY_SIZE(bounded),
		      (const void *)(uintptr_t)-4, 8, 4) !=
	    MEMORY_OUT_OF_BOUNDS)
		return 114;

	return 0;
}

static int test_memset_s(void)
{
	uint8_t bytes[4] = { 1u, 2u, 3u, 4u };
	uint8_t unchanged[3] = { 5u, 6u, 7u };

	if (memset_s(bytes, ARRAY_SIZE(bytes), 0xa5,
		     ARRAY_SIZE(bytes) - 1u) != MEMORY_OK)
		return 115;
	if (bytes[0] != 0xa5u || bytes[1] != 0xa5u || bytes[2] != 0xa5u ||
	    bytes[3] != 4u)
		return 116;
	if (memset_s(unchanged, 2u, 0, 3u) != MEMORY_OUT_OF_BOUNDS ||
	    unchanged[0] != 5u || unchanged[1] != 6u || unchanged[2] != 7u)
		return 117;
	if (memset_s(NULL, 0u, 0, 0u) != MEMORY_OK)
		return 118;
	if (memset_s(NULL, 1u, 0, 1u) != MEMORY_INVALID_ARGUMENT)
		return 119;
	if (memset_s((void *)(uintptr_t)-1, 1u, 0, 1u) !=
	    MEMORY_OUT_OF_BOUNDS)
		return 130;
	return 0;
}

static int test_memzero_explicit_s(void)
{
	uint8_t partial[5] = { 0x41, 0x42, 0x43, 0x44, 0x45 };
	uint8_t complete[4] = { 1, 2, 3, 4 };
	uint8_t unchanged[3] = { 7, 8, 9 };
	uint8_t zero[4] = { 0, 0, 0, 0 };

	if (memzero_explicit_s(partial, ARRAY_SIZE(partial), 3) != MEMORY_OK)
		return 120;
	if (partial[0] != 0 || partial[1] != 0 || partial[2] != 0 ||
	    partial[3] != 0x44 || partial[4] != 0x45)
		return 121;
	if (memzero_explicit_s(complete, ARRAY_SIZE(complete),
			       ARRAY_SIZE(complete)) != MEMORY_OK)
		return 122;
	if (!bytes_equal(complete, zero, ARRAY_SIZE(complete)))
		return 123;

	if (memzero_explicit_s(NULL, 0, 0) != MEMORY_OK)
		return 124;
	if (memzero_explicit_s(NULL, 1, 1) != MEMORY_INVALID_ARGUMENT)
		return 125;
	if (memzero_explicit_s(unchanged, 2, 3) != MEMORY_OUT_OF_BOUNDS)
		return 128;
	if (unchanged[0] != 7 || unchanged[1] != 8 || unchanged[2] != 9)
		return 129;

	return 0;
}

static int test_memcpy_and_pad_s(void)
{
	uint8_t source[5] = { 'A', 'B', 'C', 'D', 'E' };
	uint8_t destination[8];
	uint8_t expected[8] = { 'A', 'B', 'C', '.', '.', '.', '.', '.' };
	uint8_t unchanged[4] = { 1, 2, 3, 4 };
	uint8_t overlap[4] = { 'A', 'B', 'C', 'D' };
	uint8_t overlap_original[4] = { 'A', 'B', 'C', 'D' };

	if (memcpy_and_pad_s(destination, ARRAY_SIZE(destination), source,
			     ARRAY_SIZE(source), 3, '.') != MEMORY_OK)
		return 30;
	if (!bytes_equal(destination, expected, ARRAY_SIZE(expected)))
		return 31;

	if (memcpy_and_pad_s(destination, 3, source, ARRAY_SIZE(source), 5,
			     0) != MEMORY_TRUNCATED)
		return 32;
	if (destination[0] != 'A' || destination[1] != 'B' ||
	    destination[2] != 'C')
		return 33;

	if (memcpy_and_pad_s(unchanged, ARRAY_SIZE(unchanged), source, 2, 3,
			     0) != MEMORY_OUT_OF_BOUNDS)
		return 34;
	if (unchanged[0] != 1 || unchanged[1] != 2 ||
	    unchanged[2] != 3 || unchanged[3] != 4)
		return 35;
	if (memcpy_and_pad_s(NULL, 0, source, ARRAY_SIZE(source), 3, 0) !=
	    MEMORY_TRUNCATED)
		return 36;
	if (memcpy_and_pad_s(destination, ARRAY_SIZE(destination), NULL, 0, 1,
			     0) != MEMORY_INVALID_ARGUMENT)
		return 37;
	if (memcpy_and_pad_s(overlap + 1u, 3u, overlap, 4u, 2u, '.') !=
		    MEMORY_OVERLAP ||
	    !bytes_equal(overlap, overlap_original, ARRAY_SIZE(overlap)))
		return 38;
	if (memcpy_and_pad_s((void *)(uintptr_t)-1, 2u, source,
			     ARRAY_SIZE(source), 1u, '.') !=
	    MEMORY_OUT_OF_BOUNDS)
		return 39;
	if (memcpy_and_pad_s(unchanged, ARRAY_SIZE(unchanged),
			     (const void *)(uintptr_t)-1, 2u, 2u, '.') !=
		    MEMORY_OUT_OF_BOUNDS ||
	    unchanged[0] != 1u || unchanged[1] != 2u ||
	    unchanged[2] != 3u || unchanged[3] != 4u)
		return 131;

	return 0;
}

static int test_strtomem_pad_s(void)
{
	char terminated[3] = { 'A', 'B', '\0' };
	char exact[4] = { 'A', 'B', 'C', '\0' };
	char unterminated[3] = { 'X', 'Y', 'Z' };
	char long_source[6] = { '1', '2', '3', '4', '5', '\0' };
	uint8_t destination[5];
	uint8_t padded[5] = { 'A', 'B', '_', '_', '_' };

	if (strtomem_pad_s(destination, ARRAY_SIZE(destination), terminated,
			   ARRAY_SIZE(terminated), '_') != MEMORY_OK)
		return 40;
	if (!bytes_equal(destination, padded, ARRAY_SIZE(padded)))
		return 41;

	if (strtomem_pad_s(destination, 3, exact, ARRAY_SIZE(exact), 0) !=
	    MEMORY_OK)
		return 42;
	if (destination[0] != 'A' || destination[1] != 'B' ||
	    destination[2] != 'C')
		return 43;

	if (strtomem_pad_s(destination, 3, unterminated,
			   ARRAY_SIZE(unterminated), 0) != MEMORY_TRUNCATED)
		return 44;
	if (destination[0] != 'X' || destination[1] != 'Y' ||
	    destination[2] != 'Z')
		return 45;

	if (strtomem_pad_s(destination, 3, long_source,
			   ARRAY_SIZE(long_source), 0) != MEMORY_TRUNCATED)
		return 46;
	if (destination[0] != '1' || destination[1] != '2' ||
	    destination[2] != '3')
		return 47;
	if (strtomem_pad_s(destination, 3, NULL, 0, 0) !=
	    MEMORY_INVALID_ARGUMENT)
		return 48;
	if (strtomem_pad_s(NULL, 1, terminated, ARRAY_SIZE(terminated), 0) !=
	    MEMORY_INVALID_ARGUMENT)
		return 49;

	/* A zero readable extent is padded but cannot prove termination. */
	if (strtomem_pad_s(destination, 3, terminated, 0, '#') !=
	    MEMORY_TRUNCATED)
		return 50;
	if (destination[0] != '#' || destination[1] != '#' ||
	    destination[2] != '#')
		return 51;

	/*
	 * The later one-byte destination range must reject a UINTPTR_MAX start,
	 * so this boundary case never dereferences the synthetic address under
	 * either native data model.
	 */
	if (strtomem_pad_s((void *)(uintptr_t)-1,
			   (size_t)UINT32_MAX_VALUE, "A", 2, 0) !=
	    MEMORY_OUT_OF_BOUNDS)
		return 52;
	destination[0] = '!';
	if (strtomem_pad_s(destination, ARRAY_SIZE(destination),
			   (const char *)(uintptr_t)-1, 1u, 0) !=
		    MEMORY_OUT_OF_BOUNDS ||
	    destination[0] != '!')
		return 53;

	return 0;
}

static int test_strscpy_s(void)
{
	char destination[6] = { '!', '!', '!', '!', '!', '!' };
	char small[3] = { '!', '!', '!' };
	char single[1] = { '!' };
	char untouched[1] = { '!' };
	char unterminated[3] = { 'D', 'O', 'S' };
	char overlap[6] = { 'D', 'O', 'S', '\0', '!', '!' };
	ssize_t copied;

	copied = strscpy_s(destination, ARRAY_SIZE(destination), "DOS",
			   sizeof("DOS"));
	if (copied != 3 || destination[0] != 'D' || destination[1] != 'O' ||
	    destination[2] != 'S' || destination[3] != '\0')
		return 60;

	copied = strscpy_s(small, ARRAY_SIZE(small), "DOS", sizeof("DOS"));
	if (copied != STRSCPY_TRUNCATED || small[0] != 'D' ||
	    small[1] != 'O' || small[2] != '\0')
		return 61;

	copied = strscpy_s(single, ARRAY_SIZE(single), "", sizeof(""));
	if (copied != 0 || single[0] != '\0')
		return 62;
	if (strscpy_s(untouched, 0u, "X", sizeof("X")) !=
		STRSCPY_TRUNCATED ||
	    untouched[0] != '!')
		return 63;
	if (strscpy_s(NULL, 1u, "X", sizeof("X")) != STRSCPY_TRUNCATED)
		return 64;
	destination[0] = '!';
	if (strscpy_s(destination, ARRAY_SIZE(destination), NULL, 1u) !=
		STRSCPY_TRUNCATED ||
	    destination[0] != '!')
		return 65;

	copied = strscpy_s(destination, ARRAY_SIZE(destination), unterminated,
			   ARRAY_SIZE(unterminated));
	if (copied != STRSCPY_TRUNCATED || destination[0] != 'D' ||
	    destination[1] != 'O' || destination[2] != 'S' ||
	    destination[3] != '\0')
		return 66;
	destination[0] = '!';
	destination[1] = '!';
	if (strscpy_s(destination, ARRAY_SIZE(destination), unterminated, 0u) !=
		STRSCPY_TRUNCATED ||
	    destination[0] != '\0' || destination[1] != '!')
		return 67;
	if (strscpy_s(overlap + 1u, ARRAY_SIZE(overlap) - 1u, overlap, 4u) !=
		STRSCPY_TRUNCATED ||
	    overlap[0] != 'D' || overlap[1] != 'O' || overlap[2] != 'S' ||
	    overlap[3] != '\0' || overlap[4] != '!' || overlap[5] != '!')
		return 68;
	if (strscpy_s((char *)(uintptr_t)-1, 2u, "X", sizeof("X")) !=
		STRSCPY_TRUNCATED)
		return 69;
	if (strscpy_s((char *)(uintptr_t)-1, 2u, "X", 0u) !=
		STRSCPY_TRUNCATED)
		return 70;

	return 0;
}

static int test_strnlen(void)
{
	char text[6] = { 'D', 'O', 'S', '\0', 'X', 'Y' };
	char unterminated[4] = { 'A', 'B', 'C', 'D' };

	if (strnlen(text, ARRAY_SIZE(text)) != 3)
		return 71;
	if (strnlen(text, 2) != 2)
		return 72;
	if (strnlen(unterminated, ARRAY_SIZE(unterminated)) != 4)
		return 73;
	if (strnlen(NULL, 0) != 0)
		return 73;

	return 0;
}

static int test_memcmp_s(void)
{
	uint8_t lower[3] = { 1, 2, 3 };
	uint8_t higher[3] = { 1, 2, 4 };
	int comparison = 99;

	if (memcmp_s(lower, 3, lower, 3, 3, &comparison) != MEMORY_OK ||
	    comparison != 0)
		return 80;
	if (memcmp_s(lower, 3, higher, 3, 3, &comparison) != MEMORY_OK ||
	    comparison >= 0)
		return 81;
	if (memcmp_s(higher, 3, lower, 3, 3, &comparison) != MEMORY_OK ||
	    comparison <= 0)
		return 82;

	comparison = 99;
	if (memcmp_s(NULL, 0, NULL, 0, 0, &comparison) != MEMORY_OK ||
	    comparison != 0)
		return 83;
	comparison = 99;
	if (memcmp_s(lower, 2, higher, 3, 3, &comparison) !=
	    MEMORY_OUT_OF_BOUNDS || comparison != 0)
		return 84;
	comparison = 99;
	if (memcmp_s(NULL, 3, higher, 3, 1, &comparison) !=
	    MEMORY_INVALID_ARGUMENT || comparison != 0)
		return 85;
	if (memcmp_s(lower, 3, higher, 3, 1, NULL) !=
	    MEMORY_INVALID_ARGUMENT)
		return 86;
	comparison = 99;
	if (memcmp_s((const void *)(uintptr_t)-1, 1u, higher, 3u, 1u,
		     &comparison) != MEMORY_OUT_OF_BOUNDS || comparison != 0)
		return 87;
	comparison = 99;
	if (memcmp_s(lower, 3u, (const void *)(uintptr_t)-1, 1u, 1u,
		     &comparison) != MEMORY_OUT_OF_BOUNDS || comparison != 0)
		return 88;

	return 0;
}

static int test_checked_arithmetic(void)
{
	uint32_t result;

	if (!check_add_overflow(UINT32_MAX_VALUE, (uint32_t)1, &result) ||
	    result != 0)
		return 90;
	if (check_add_overflow((uint32_t)0xfffffffeu, (uint32_t)1,
			       &result) || result != UINT32_MAX_VALUE)
		return 91;
	if (!check_sub_overflow((uint32_t)0, (uint32_t)1, &result) ||
	    result != UINT32_MAX_VALUE)
		return 92;
	if (check_sub_overflow((uint32_t)9, (uint32_t)4, &result) ||
	    result != 5)
		return 93;
	if (!check_mul_overflow(UINT32_MAX_VALUE, (uint32_t)2, &result) ||
	    result != (uint32_t)0xfffffffeu)
		return 94;
	if (check_mul_overflow((uint32_t)0x10000u, (uint32_t)0x1000u,
			       &result) || result != (uint32_t)0x10000000u)
		return 95;

	if (range_overflows_u32(10, 5, 15))
		return 96;
	if (!range_overflows_u32(10, 6, 15))
		return 97;
	if (!range_overflows_u32(15, 0, 15))
		return 98;
	if (range_overflows_u32(UINT32_MAX_VALUE - 1u, 1,
				UINT32_MAX_VALUE))
		return 99;

	return 0;
}

static int run_tests(void)
{
	int status;

	status = test_memcpy_s();
	if (status != 0)
		return status;
	status = test_memmove_s();
	if (status != 0)
		return status;
	status = test_memset_s();
	if (status != 0)
		return status;
	status = test_memzero_explicit_s();
	if (status != 0)
		return status;
	status = test_memcpy_and_pad_s();
	if (status != 0)
		return status;
	status = test_strtomem_pad_s();
	if (status != 0)
		return status;
	status = test_strscpy_s();
	if (status != 0)
		return status;
	status = test_strnlen();
	if (status != 0)
		return status;
	status = test_memcmp_s();
	if (status != 0)
		return status;
	return test_checked_arithmetic();
}

DOSC32_TEST_ENTRY(run_tests)

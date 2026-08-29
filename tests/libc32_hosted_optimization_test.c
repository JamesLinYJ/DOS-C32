// SPDX-License-Identifier: GPL-2.0-only
/*
 * Hosted optimization regression for the raw memory primitives.
 *
 * This translation unit is deliberately built at -O2 without -ffreestanding
 * or -fno-builtin. Volatile function pointers force calls to the exported
 * definitions so a compiler-generated self-call cannot hide behind a builtin.
 */
#include "string.h"

void *memcpy(void *destination, const void *source, size_t count);
void *memmove(void *destination, const void *source, size_t count);
void *memset(void *destination, int value, size_t count);
int memcmp(const void *left, const void *right, size_t count);

typedef void *(*memory_copy_function)(void *, const void *, size_t);
typedef void *(*memory_move_function)(void *, const void *, size_t);
typedef void *(*memory_fill_function)(void *, int, size_t);
typedef int (*memory_compare_function)(const void *, const void *, size_t);

static memory_copy_function volatile call_memcpy = memcpy;
static memory_move_function volatile call_memmove = memmove;
static memory_fill_function volatile call_memset = memset;
static memory_compare_function volatile call_memcmp = memcmp;

static bool bytes_match(const uint8_t *left, const uint8_t *right,
			size_t count)
{
	size_t index;

	for (index = 0u; index < count; ++index) {
		if (left[index] != right[index])
			return false;
	}
	return true;
}

int main(void)
{
	const uint8_t source[8] = { 0x10u, 0x21u, 0x32u, 0x43u,
				    0x54u, 0x65u, 0x76u, 0x87u };
	const uint8_t filled_expected[8] = {
		0xa5u, 0xa5u, 0xa5u, 0xa5u, 0xa5u, 0xa5u, 0xa5u, 0xa5u
	};
	const uint8_t moved_backward_expected[8] = {
		0u, 0u, 1u, 2u, 3u, 4u, 5u, 7u
	};
	const uint8_t moved_forward_expected[8] = {
		1u, 2u, 3u, 4u, 5u, 6u, 6u, 7u
	};
	uint8_t destination[8] = { 0u };
	uint8_t moved_backward[8] = { 0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u };
	uint8_t moved_forward[8] = { 0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u };
	int comparison = 0;

	if (call_memset(destination, 0xa5, ARRAY_SIZE(destination)) !=
	    destination)
		return 1;
	if (!bytes_match(destination, filled_expected,
			 ARRAY_SIZE(destination)))
		return 2;

	if (call_memcpy(destination, source, ARRAY_SIZE(source)) != destination)
		return 3;
	if (!bytes_match(destination, source, ARRAY_SIZE(source)))
		return 4;

	if (call_memmove(moved_backward + 1u, moved_backward, 6u) !=
	    moved_backward + 1u)
		return 5;
	if (!bytes_match(moved_backward, moved_backward_expected,
			 ARRAY_SIZE(moved_backward)))
		return 6;
	if (call_memmove(moved_forward, moved_forward + 1u, 6u) !=
	    moved_forward)
		return 7;
	if (!bytes_match(moved_forward, moved_forward_expected,
			 ARRAY_SIZE(moved_forward)))
		return 8;

	if (call_memcmp(source, source, ARRAY_SIZE(source)) != 0)
		return 9;
	if (call_memcmp(source, filled_expected, ARRAY_SIZE(source)) >= 0)
		return 10;
	if (call_memcmp(filled_expected, source, ARRAY_SIZE(source)) <= 0)
		return 11;

	if (memset_s(destination, ARRAY_SIZE(destination), 0x3c,
		     ARRAY_SIZE(destination)) != MEMORY_OK)
		return 12;
	if (destination[0] != 0x3cu || destination[7] != 0x3cu)
		return 13;
	if (memcpy_s(destination, ARRAY_SIZE(destination), source,
		     ARRAY_SIZE(source), ARRAY_SIZE(source)) != MEMORY_OK)
		return 14;
	if (memmove_s(destination + 1u, 7u, destination, 8u, 6u) !=
	    MEMORY_OK)
		return 15;
	if (memcmp_s(destination + 1u, 7u, source, ARRAY_SIZE(source), 6u,
		     &comparison) != MEMORY_OK || comparison != 0)
		return 16;

	return 0;
}

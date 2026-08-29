// SPDX-License-Identifier: GPL-2.0-only
/* Freestanding, always-on assertion failure endpoint. */
#include "dosc32_assert.h"

void dosc32_assert_fail(const char *expression, const char *file,
			uint32_t line)
{
	/* Keep call-site data visible to a debugger without needing hosted I/O. */
	__asm__ volatile("" : : "r"(expression), "r"(file), "r"(line) : "memory");
	__builtin_trap();
	__builtin_unreachable();
}

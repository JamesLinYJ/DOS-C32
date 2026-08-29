/* SPDX-License-Identifier: GPL-2.0-only */
/* Always-on freestanding invariant checks for libc32 and the DOS core. */
#ifndef DOSC32_ASSERT_H
#define DOSC32_ASSERT_H

#include "compiler.h"
#include "types.h"

void dosc32_assert_fail(const char *expression, const char *file,
			uint32_t line) __noreturn __cold;

#define DOSC32_ASSERT(condition)                                             \
	do {                                                                  \
		if (unlikely(!(condition)))                                     \
			dosc32_assert_fail(#condition, __FILE__,                  \
					   (uint32_t)__LINE__);                     \
	} while (0)

#endif

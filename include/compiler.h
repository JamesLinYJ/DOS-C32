/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DOSC32_COMPILER_H
#define DOSC32_COMPILER_H

#define __must_check __attribute__((warn_unused_result))
#define __packed __attribute__((packed))
#define __aligned(value) __attribute__((aligned(value)))
#define __noreturn __attribute__((noreturn))
#ifndef __cold
#define __cold __attribute__((cold))
#endif

#define likely(condition) __builtin_expect(!!(condition), 1)
#define unlikely(condition) __builtin_expect(!!(condition), 0)

#define static_assert_expression(condition, message)                           \
	_Static_assert((condition), message)

#endif

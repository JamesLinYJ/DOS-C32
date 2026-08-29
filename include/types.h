/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DOSC32_TYPES_H
#define DOSC32_TYPES_H

#ifdef DOSC32_HOSTED_TYPES
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#else
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
typedef signed char int8_t;
typedef signed short int16_t;
typedef signed int int32_t;
typedef signed long long int64_t;

/* Follow the selected GCC ABI: 32-bit now, naturally 64-bit under -m64. */
typedef __SIZE_TYPE__ size_t;
typedef __PTRDIFF_TYPE__ ptrdiff_t;
typedef __PTRDIFF_TYPE__ ssize_t;
typedef __UINTPTR_TYPE__ uintptr_t;
typedef __INTPTR_TYPE__ intptr_t;

typedef enum {
    false = 0,
    true = 1
} bool;

#define NULL ((void *)0)
#endif

/* Reject pointer arguments to ARRAY_SIZE at compile time. */
#define __same_type(left, right) \
	__builtin_types_compatible_p(typeof(left), typeof(right))
#define __must_be_array(array) \
	(0 * sizeof(struct { int : -!!__same_type((array), &(array)[0]); }))
#define ARRAY_SIZE(array) \
	(sizeof(array) / sizeof((array)[0]) + __must_be_array(array))

_Static_assert(sizeof(uint8_t) == 1, "uint8_t width changed");
_Static_assert(sizeof(uint16_t) == 2, "uint16_t width changed");
_Static_assert(sizeof(uint32_t) == 4, "uint32_t width changed");
_Static_assert(sizeof(uint64_t) == 8, "uint64_t width changed");
_Static_assert(sizeof(size_t) == sizeof(void *),
	"size_t must follow the selected compiler ABI");
_Static_assert(sizeof(uintptr_t) == sizeof(void *),
	"uintptr_t must represent a native pointer");
_Static_assert(sizeof(void *) == 4 || sizeof(void *) == 8,
	"DOS-C32 supports only 32-bit and 64-bit native ABIs");

#endif

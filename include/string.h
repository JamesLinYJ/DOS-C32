/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DOSC32_STRING_H
#define DOSC32_STRING_H

#include "compiler.h"
#include "types.h"

#define STRSCPY_TRUNCATED ((ssize_t) - 1)

enum memory_status {
	MEMORY_OK = 0,
	MEMORY_INVALID_ARGUMENT,
	MEMORY_OUT_OF_BOUNDS,
	MEMORY_OVERLAP,
	MEMORY_TRUNCATED
};

enum memory_status memcpy_s(void *destination, size_t destination_capacity,
			    const void *source, size_t source_capacity,
			    size_t count) __must_check;
enum memory_status memmove_s(void *destination, size_t destination_capacity,
			     const void *source, size_t source_capacity,
			     size_t count) __must_check;
enum memory_status memset_s(void *destination, size_t destination_capacity,
			    int value, size_t count) __must_check;
enum memory_status memzero_explicit_s(void *destination,
				      size_t destination_capacity,
				      size_t count) __must_check;
enum memory_status memcmp_s(const void *left, size_t left_capacity,
			    const void *right, size_t right_capacity,
			    size_t count, int *comparison) __must_check;
enum memory_status memcpy_and_pad_s(void *destination,
				    size_t destination_capacity,
				    const void *source, size_t source_capacity,
				    size_t count, int padding) __must_check;
enum memory_status strtomem_pad_s(void *destination,
				  size_t destination_capacity,
				  const char *source, size_t source_capacity,
				  int padding) __must_check;
size_t strnlen(const char *text, size_t limit);
int strcmp(const char *left, const char *right);
int strncmp(const char *left, const char *right, size_t count);
int strcasecmp(const char *left, const char *right);
/*
 * Bounded string copy with an explicit readable source extent.
 * A valid non-empty destination is NUL-terminated on truncation.  Overlap and
 * native range overflow are rejected before either object is modified.
 */
ssize_t strscpy_s(char *destination, size_t destination_capacity,
		  const char *source, size_t source_capacity) __must_check;
char *strchr(const char *text, int character);
char ascii_toupper(char character);
char ascii_tolower(char character);
bool ascii_isspace(char character);

#endif

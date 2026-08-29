// SPDX-License-Identifier: GPL-2.0-only
/*
 * DOS-C32 freestanding byte and string operations.
 * Raw primitives exist for compiler-generated calls. Kernel code uses the
 * capacity-carrying interfaces, which validate every native interval before
 * changing the destination.
 */
#include "string.h"

#include "overflow.h"

void *memcpy(void *destination, const void *source, size_t count);
void *memmove(void *destination, const void *source, size_t count);
void *memset(void *destination, int value, size_t count);
int memcmp(const void *left, const void *right, size_t count);

struct byte_span {
	uintptr_t begin;
	uintptr_t end;
};

static bool byte_span_create(const void *address, size_t length,
			     struct byte_span *span)
{
	uintptr_t begin = (uintptr_t)address;
	uintptr_t end = begin;

	if (length != 0u &&
	    check_add_overflow(begin, (uintptr_t)length, &end))
		return false;
	span->begin = begin;
	span->end = end;
	return true;
}

static bool byte_spans_intersect(const struct byte_span *left,
				 const struct byte_span *right)
{
	if (left->begin == left->end || right->begin == right->end)
		return false;
	return left->begin < right->end && right->begin < left->end;
}

/*
 * Volatile byte accesses prevent an optimizing hosted compiler from replacing
 * these defining loops with calls to the very raw primitives being defined.
 * This is a correctness boundary, not an MMIO interface.
 */
static void bytes_copy_forward(volatile uint8_t *destination,
			       const volatile uint8_t *source, size_t count)
{
	while (count != 0u) {
		*destination = *source;
		++destination;
		++source;
		--count;
	}
}

static void bytes_copy_backward(volatile uint8_t *destination,
				const volatile uint8_t *source, size_t count)
{
	while (count != 0u) {
		--count;
		destination[count] = source[count];
	}
}

static void bytes_fill(volatile uint8_t *destination, uint8_t value,
		       size_t count)
{
	while (count != 0u) {
		*destination = value;
		++destination;
		--count;
	}
}

void *memcpy(void *destination, const void *source, size_t count)
{
	bytes_copy_forward((uint8_t *)destination, (const uint8_t *)source,
			   count);
	return destination;
}

void *memmove(void *destination, const void *source, size_t count)
{
	uintptr_t destination_address = (uintptr_t)destination;
	uintptr_t source_address = (uintptr_t)source;

	if (destination_address == source_address || count == 0u)
		return destination;

	if (destination_address > source_address &&
	    destination_address - source_address < (uintptr_t)count) {
		bytes_copy_backward((uint8_t *)destination,
				    (const uint8_t *)source, count);
	} else {
		bytes_copy_forward((uint8_t *)destination,
				   (const uint8_t *)source, count);
	}
	return destination;
}

void *memset(void *destination, int value, size_t count)
{
	bytes_fill((uint8_t *)destination, (uint8_t)value, count);
	return destination;
}

int memcmp(const void *left, const void *right, size_t count)
{
	const volatile uint8_t *left_bytes = (const uint8_t *)left;
	const volatile uint8_t *right_bytes = (const uint8_t *)right;

	while (count != 0u) {
		if (*left_bytes != *right_bytes)
			return (int)*left_bytes - (int)*right_bytes;
		++left_bytes;
		++right_bytes;
		--count;
	}
	return 0;
}

enum memory_status memcpy_s(void *destination, size_t destination_capacity,
			    const void *source, size_t source_capacity,
			    size_t count)
{
	struct byte_span destination_span;
	struct byte_span source_span;

	if (count == 0u)
		return MEMORY_OK;
	if (destination == NULL || source == NULL)
		return MEMORY_INVALID_ARGUMENT;
	if (count > destination_capacity || count > source_capacity)
		return MEMORY_OUT_OF_BOUNDS;
	if (!byte_span_create(destination, count, &destination_span) ||
	    !byte_span_create(source, count, &source_span))
		return MEMORY_OUT_OF_BOUNDS;
	if (byte_spans_intersect(&destination_span, &source_span))
		return MEMORY_OVERLAP;

	bytes_copy_forward((uint8_t *)destination, (const uint8_t *)source,
			   count);
	return MEMORY_OK;
}

enum memory_status memmove_s(void *destination, size_t destination_capacity,
			     const void *source, size_t source_capacity,
			     size_t count)
{
	struct byte_span destination_span;
	struct byte_span source_span;

	if (count == 0u)
		return MEMORY_OK;
	if (destination == NULL || source == NULL)
		return MEMORY_INVALID_ARGUMENT;
	if (count > destination_capacity || count > source_capacity)
		return MEMORY_OUT_OF_BOUNDS;
	if (!byte_span_create(destination, count, &destination_span) ||
	    !byte_span_create(source, count, &source_span))
		return MEMORY_OUT_OF_BOUNDS;

	(void)destination_span;
	(void)source_span;
	(void)memmove(destination, source, count);
	return MEMORY_OK;
}

enum memory_status memset_s(void *destination, size_t destination_capacity,
			    int value, size_t count)
{
	struct byte_span destination_span;

	if (count == 0u)
		return MEMORY_OK;
	if (destination == NULL)
		return MEMORY_INVALID_ARGUMENT;
	if (count > destination_capacity)
		return MEMORY_OUT_OF_BOUNDS;
	if (!byte_span_create(destination, count, &destination_span))
		return MEMORY_OUT_OF_BOUNDS;

	(void)destination_span;
	bytes_fill((uint8_t *)destination, (uint8_t)value, count);
	return MEMORY_OK;
}

enum memory_status memzero_explicit_s(void *destination,
				      size_t destination_capacity,
				      size_t count)
{
	enum memory_status status =
		memset_s(destination, destination_capacity, 0, count);

	if (status != MEMORY_OK)
		return status;
	__asm__ volatile("" : : "r"(destination) : "memory");
	return MEMORY_OK;
}

enum memory_status memcmp_s(const void *left, size_t left_capacity,
			    const void *right, size_t right_capacity,
			    size_t count, int *comparison)
{
	struct byte_span left_span;
	struct byte_span right_span;

	if (comparison == NULL)
		return MEMORY_INVALID_ARGUMENT;
	*comparison = 0;
	if (count == 0u)
		return MEMORY_OK;
	if (left == NULL || right == NULL)
		return MEMORY_INVALID_ARGUMENT;
	if (count > left_capacity || count > right_capacity)
		return MEMORY_OUT_OF_BOUNDS;
	if (!byte_span_create(left, count, &left_span) ||
	    !byte_span_create(right, count, &right_span))
		return MEMORY_OUT_OF_BOUNDS;

	(void)left_span;
	(void)right_span;
	*comparison = memcmp(left, right, count);
	return MEMORY_OK;
}

enum memory_status memcpy_and_pad_s(void *destination,
				    size_t destination_capacity,
				    const void *source,
				    size_t source_capacity,
				    size_t count, int padding)
{
	struct byte_span destination_span;
	struct byte_span source_span;
	struct byte_span copied_destination_span;
	struct byte_span copied_source_span;
	size_t copied;

	if (destination_capacity != 0u && destination == NULL)
		return MEMORY_INVALID_ARGUMENT;
	if (count != 0u && source == NULL)
		return MEMORY_INVALID_ARGUMENT;
	if (count > source_capacity)
		return MEMORY_OUT_OF_BOUNDS;
	if (!byte_span_create(destination, destination_capacity,
			      &destination_span) ||
	    !byte_span_create(source, count, &source_span))
		return MEMORY_OUT_OF_BOUNDS;

	copied = count < destination_capacity ? count : destination_capacity;
	if (!byte_span_create(destination, copied,
			      &copied_destination_span) ||
	    !byte_span_create(source, copied, &copied_source_span))
		return MEMORY_OUT_OF_BOUNDS;
	if (byte_spans_intersect(&copied_destination_span,
				 &copied_source_span))
		return MEMORY_OVERLAP;

	(void)destination_span;
	(void)source_span;
	if (copied != 0u) {
		bytes_copy_forward((uint8_t *)destination,
				   (const uint8_t *)source, copied);
	}
	if (destination_capacity > copied) {
		bytes_fill((uint8_t *)destination + copied, (uint8_t)padding,
			   destination_capacity - copied);
	}
	return count > destination_capacity ? MEMORY_TRUNCATED : MEMORY_OK;
}

enum memory_status strtomem_pad_s(void *destination,
				  size_t destination_capacity,
				  const char *source,
				  size_t source_capacity, int padding)
{
	struct byte_span destination_span;
	struct byte_span source_probe_span;
	size_t probe_capacity;
	size_t probe_limit;
	size_t source_length = 0u;
	size_t copied;
	bool terminated = false;
	enum memory_status status;

	if (source == NULL)
		return MEMORY_INVALID_ARGUMENT;
	if (destination_capacity != 0u && destination == NULL)
		return MEMORY_INVALID_ARGUMENT;
	if (!byte_span_create(destination, destination_capacity,
			      &destination_span))
		return MEMORY_OUT_OF_BOUNDS;
	if (check_add_overflow(destination_capacity, (size_t)1u,
			       &probe_limit))
		probe_limit = destination_capacity;
	probe_capacity = source_capacity < probe_limit ? source_capacity
						       : probe_limit;
	if (!byte_span_create(source, probe_capacity, &source_probe_span))
		return MEMORY_OUT_OF_BOUNDS;

	(void)destination_span;
	(void)source_probe_span;
	while (source_length < probe_capacity) {
		if (source[source_length] == '\0') {
			terminated = true;
			break;
		}
		++source_length;
	}
	copied = source_length < destination_capacity ? source_length
						      : destination_capacity;
	status = memcpy_and_pad_s(destination, destination_capacity, source,
				  source_capacity, copied, padding);
	if (status != MEMORY_OK)
		return status;
	return terminated && source_length <= destination_capacity
		       ? MEMORY_OK
		       : MEMORY_TRUNCATED;
}

size_t strnlen(const char *text, size_t limit)
{
	size_t length = 0u;

	while (length < limit && text[length] != '\0')
		++length;
	return length;
}

int strcmp(const char *left, const char *right)
{
	while (*left == *right) {
		if (*left == '\0')
			return 0;
		++left;
		++right;
	}
	return (int)(uint8_t)*left - (int)(uint8_t)*right;
}

int strncmp(const char *left, const char *right, size_t count)
{
	while (count != 0u) {
		if (*left != *right)
			return (int)(uint8_t)*left - (int)(uint8_t)*right;
		if (*left == '\0')
			return 0;
		++left;
		++right;
		--count;
	}
	return 0;
}

int strcasecmp(const char *left, const char *right)
{
	for (;;) {
		uint8_t left_folded = (uint8_t)ascii_toupper(*left);
		uint8_t right_folded = (uint8_t)ascii_toupper(*right);

		if (left_folded != right_folded)
			return (int)left_folded - (int)right_folded;
		if (*left == '\0')
			return 0;
		++left;
		++right;
	}
}

ssize_t strscpy_s(char *destination, size_t destination_capacity,
		  const char *source, size_t source_capacity)
{
	struct byte_span destination_span;
	struct byte_span source_span;
	size_t payload_capacity;
	size_t copied = 0u;

	if (destination_capacity == 0u || destination == NULL || source == NULL)
		return STRSCPY_TRUNCATED;
	if (!byte_span_create(destination, destination_capacity,
			      &destination_span) ||
	    !byte_span_create(source, source_capacity, &source_span))
		return STRSCPY_TRUNCATED;
	if (byte_spans_intersect(&destination_span, &source_span))
		return STRSCPY_TRUNCATED;

	payload_capacity = destination_capacity - 1u;
	while (copied < payload_capacity && copied < source_capacity) {
		char character = source[copied];

		if (character == '\0') {
			destination[copied] = '\0';
			return copied <= (size_t)__PTRDIFF_MAX__
				       ? (ssize_t)copied
				       : STRSCPY_TRUNCATED;
		}
		destination[copied] = character;
		++copied;
	}

	destination[copied] = '\0';
	if (copied < source_capacity && source[copied] == '\0' &&
	    copied <= (size_t)__PTRDIFF_MAX__)
		return (ssize_t)copied;
	return STRSCPY_TRUNCATED;
}

char *strchr(const char *text, int character)
{
	char target = (char)character;

	for (;;) {
		if (*text == target)
			return (char *)text;
		if (*text == '\0')
			return NULL;
		++text;
	}
}

char ascii_toupper(char character)
{
	if (character >= 'a' && character <= 'z')
		return (char)(character - ('a' - 'A'));
	return character;
}

char ascii_tolower(char character)
{
	if (character >= 'A' && character <= 'Z')
		return (char)(character + ('a' - 'A'));
	return character;
}

bool ascii_isspace(char character)
{
	return character == ' ' || character == '\t' || character == '\r' ||
	       character == '\n' || character == '\f' || character == '\v';
}

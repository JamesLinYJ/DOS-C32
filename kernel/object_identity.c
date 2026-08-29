// SPDX-License-Identifier: GPL-2.0-only
/* Monotonic boot-lifetime kernel object identities; zero is never published. */
#include "object_identity.h"

#define KERNEL_OBJECT_IDENTITY_FIRST ((kernel_object_handle_t)1u)
#define KERNEL_OBJECT_IDENTITY_LAST (KERNEL_OBJECT_HANDLE_INVALID - 1u)

static bool reserved_is_zero(const uint8_t *reserved, size_t count)
{
	size_t index;

	for (index = 0u; index < count; ++index) {
		if (reserved[index] != 0u)
			return false;
	}
	return true;
}

enum kernel_object_identity_status kernel_object_identity_source_initialize(
	struct kernel_object_identity_source *source)
{
	if (source == NULL || source->initialized > 1u ||
	    source->exhausted != 0u ||
	    !reserved_is_zero(source->reserved, ARRAY_SIZE(source->reserved)))
		return KERNEL_OBJECT_IDENTITY_INVALID_ARGUMENT;
	if (source->initialized != 0u || source->next != 0u)
		return KERNEL_OBJECT_IDENTITY_INVALID_STATE;
	source->next = KERNEL_OBJECT_IDENTITY_FIRST;
	source->initialized = 1u;
	return KERNEL_OBJECT_IDENTITY_OK;
}

enum kernel_object_identity_status kernel_object_identity_allocate(
	struct kernel_object_identity_source *source,
	kernel_object_handle_t *identity)
{
	kernel_object_handle_t allocated;

	if (source == NULL || identity == NULL || source->initialized != 1u ||
	    source->exhausted > 1u ||
	    !reserved_is_zero(source->reserved, ARRAY_SIZE(source->reserved)) ||
	    source->next == 0u ||
	    source->next == KERNEL_OBJECT_HANDLE_INVALID)
		return KERNEL_OBJECT_IDENTITY_INVALID_ARGUMENT;
	if (source->exhausted != 0u)
		return KERNEL_OBJECT_IDENTITY_EXHAUSTED;
	allocated = source->next;
	if (allocated == KERNEL_OBJECT_IDENTITY_LAST)
		source->exhausted = 1u;
	else
		++source->next;
	*identity = allocated;
	return KERNEL_OBJECT_IDENTITY_OK;
}

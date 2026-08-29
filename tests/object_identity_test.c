// SPDX-License-Identifier: GPL-2.0-only
#include "object_identity.h"
#include "test_entry.h"

static int run_tests(void)
{
	struct kernel_object_identity_source source =
		KERNEL_OBJECT_IDENTITY_SOURCE_INITIALIZER;
	kernel_object_handle_t first = 0xa5a5u;
	kernel_object_handle_t second = 0x5a5au;

	if (kernel_object_identity_allocate(&source, &first) !=
			KERNEL_OBJECT_IDENTITY_INVALID_ARGUMENT ||
	    first != 0xa5a5u)
		return 1;
	if (kernel_object_identity_source_initialize(&source) !=
			KERNEL_OBJECT_IDENTITY_OK ||
	    kernel_object_identity_source_initialize(&source) !=
			KERNEL_OBJECT_IDENTITY_INVALID_STATE)
		return 2;
	if (kernel_object_identity_allocate(&source, &first) !=
			KERNEL_OBJECT_IDENTITY_OK ||
	    kernel_object_identity_allocate(&source, &second) !=
			KERNEL_OBJECT_IDENTITY_OK ||
	    first == 0u || second != first + 1u ||
	    first == KERNEL_OBJECT_HANDLE_INVALID ||
	    second == KERNEL_OBJECT_HANDLE_INVALID)
		return 3;
	source.next = KERNEL_OBJECT_HANDLE_INVALID - 1u;
	first = 0u;
	second = 0x5a5au;
	if (kernel_object_identity_allocate(&source, &first) !=
			KERNEL_OBJECT_IDENTITY_OK ||
	    first != KERNEL_OBJECT_HANDLE_INVALID - 1u ||
	    kernel_object_identity_allocate(&source, &second) !=
			KERNEL_OBJECT_IDENTITY_EXHAUSTED ||
	    second != 0x5a5au)
		return 4;
	return 0;
}

DOSC32_TEST_ENTRY(run_tests)

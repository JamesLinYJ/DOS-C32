/* SPDX-License-Identifier: GPL-2.0-only */
/* Serialized, non-reusing identities for long-lived kernel objects. */
#ifndef DOSC32_OBJECT_IDENTITY_H
#define DOSC32_OBJECT_IDENTITY_H

#include "address.h"
#include "compiler.h"
#include "types.h"

enum kernel_object_identity_status {
	KERNEL_OBJECT_IDENTITY_OK = 0,
	KERNEL_OBJECT_IDENTITY_INVALID_ARGUMENT,
	KERNEL_OBJECT_IDENTITY_INVALID_STATE,
	KERNEL_OBJECT_IDENTITY_EXHAUSTED
};

struct kernel_object_identity_source {
	uint64_t next;
	uint8_t initialized;
	uint8_t exhausted;
	uint8_t reserved[6];
} __aligned(8);

#define KERNEL_OBJECT_IDENTITY_SOURCE_INITIALIZER                            \
	{                                                                      \
		.next = 0u, .initialized = 0u, .exhausted = 0u, .reserved = {0u} \
	}

enum kernel_object_identity_status kernel_object_identity_source_initialize(
	struct kernel_object_identity_source *source) __must_check;
enum kernel_object_identity_status kernel_object_identity_allocate(
	struct kernel_object_identity_source *source,
	kernel_object_handle_t *identity) __must_check;

static_assert_expression(sizeof(struct kernel_object_identity_source) == 16u,
			 "object identity source must stay fixed width");

#endif

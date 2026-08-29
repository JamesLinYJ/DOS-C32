/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Stable address model shared by the i386 and future x86_64 kernels.
 * Canonical addresses are always 64-bit. Native pointers are temporary maps.
 */
#ifndef DOSC32_ADDRESS_H
#define DOSC32_ADDRESS_H

#include "compiler.h"
#include "overflow.h"
#include "types.h"

typedef uint64_t kernel_address_t;
typedef uint64_t physical_address_t;
typedef uint64_t block_lba_t;
typedef uint64_t file_offset_t;
typedef uint64_t kernel_object_handle_t;
typedef uint32_t dos_linear_address_t;

#define KERNEL_ADDRESS_INVALID ((kernel_address_t)-1)
#define PHYSICAL_ADDRESS_INVALID ((physical_address_t)-1)
#define BLOCK_LBA_INVALID ((block_lba_t)-1)
#define KERNEL_OBJECT_HANDLE_INVALID ((kernel_object_handle_t)-1)

enum address_status {
	ADDRESS_OK = 0,
	ADDRESS_INVALID_ARGUMENT,
	ADDRESS_RANGE_OVERFLOW,
	ADDRESS_NOT_NATIVE_REPRESENTABLE,
	ADDRESS_NOT_MAPPED,
	ADDRESS_ACCESS_DENIED
};

enum mapping_access {
	MAPPING_READ = 1u << 0,
	MAPPING_WRITE = 1u << 1,
	MAPPING_EXECUTE = 1u << 2,
	MAPPING_DEVICE = 1u << 3
};

/* Valid only until the matching address-space unmap operation. */
struct native_mapping {
	void *pointer;
	size_t length;
};

struct kernel_address_space_ops {
	enum address_status (*map)(kernel_object_handle_t context,
		kernel_address_t address, uint64_t length, uint32_t access,
		struct native_mapping *mapping);
	void (*unmap)(kernel_object_handle_t context,
		      struct native_mapping *mapping);
};

struct kernel_address_space {
	const struct kernel_address_space_ops *ops;
	kernel_object_handle_t context;
};

/* Bootstrap identity-map helper; general code goes through address-space ops. */
static inline enum address_status
kernel_address_identity_map(kernel_address_t address, uint64_t length,
			    struct native_mapping *mapping)
{
	uint64_t last;

	if (mapping == NULL)
		return ADDRESS_INVALID_ARGUMENT;
	mapping->pointer = NULL;
	mapping->length = 0;
	if (length > (uint64_t)(size_t)-1)
		return ADDRESS_NOT_NATIVE_REPRESENTABLE;
	if (length != 0u) {
		if (check_add_overflow(address, length - 1u, &last))
			return ADDRESS_RANGE_OVERFLOW;
		if (last > (uint64_t)(uintptr_t)-1)
			return ADDRESS_NOT_NATIVE_REPRESENTABLE;
	} else if (address > (uint64_t)(uintptr_t)-1) {
		return ADDRESS_NOT_NATIVE_REPRESENTABLE;
	}
	mapping->pointer = (void *)(uintptr_t)address;
	mapping->length = (size_t)length;
	return ADDRESS_OK;
}

static_assert_expression(sizeof(kernel_address_t) == 8,
	"canonical kernel addresses must remain 64-bit");
static_assert_expression(sizeof(physical_address_t) == 8,
	"canonical physical addresses must remain 64-bit");
static_assert_expression(sizeof(block_lba_t) == 8,
	"block LBAs must remain 64-bit");
static_assert_expression(sizeof(dos_linear_address_t) == 4,
	"DOS guest linear addresses must remain 32-bit");

#endif

/* SPDX-License-Identifier: GPL-2.0-only */
/* Fixed-sector block-device boundary for DOS filesystems and drivers. */
#ifndef DOSC32_BLOCK_DEVICE_H
#define DOSC32_BLOCK_DEVICE_H

#include "address.h"
#include "compiler.h"
#include "types.h"

#define BLOCK_DEVICE_SECTOR_BYTES 512u
#define BLOCK_DEVICE_HANDLE_INVALID KERNEL_OBJECT_HANDLE_INVALID
#define BLOCK_DEVICE_GENERATION_MAX 0xffffffffu

typedef kernel_object_handle_t block_device_handle_t;

union block_device_sector {
	uint8_t bytes[BLOCK_DEVICE_SECTOR_BYTES];
	uint16_t words[BLOCK_DEVICE_SECTOR_BYTES / sizeof(uint16_t)];
};

enum block_device_status {
	BLOCK_DEVICE_OK = 0,
	BLOCK_DEVICE_INVALID_ARGUMENT,
	BLOCK_DEVICE_NO_SLOT,
	BLOCK_DEVICE_STALE_HANDLE,
	BLOCK_DEVICE_NOT_READY,
	BLOCK_DEVICE_NO_MEDIA,
	BLOCK_DEVICE_UNSUPPORTED,
	BLOCK_DEVICE_OUT_OF_RANGE,
	BLOCK_DEVICE_READ_ONLY,
	BLOCK_DEVICE_TIMEOUT,
	BLOCK_DEVICE_IO_ERROR,
	BLOCK_DEVICE_GENERATION_EXHAUSTED
};

struct block_device_geometry {
	block_lba_t sector_count;
	uint32_t logical_sector_bytes;
	uint8_t writable;
	uint8_t reserved[3];
} __aligned(8);

struct block_device_ops {
	enum block_device_status (*probe)(
	    kernel_object_handle_t context,
	    struct block_device_geometry *geometry);
	enum block_device_status (*read_sector)(
	    kernel_object_handle_t context, block_lba_t lba,
	    union block_device_sector *sector);
	enum block_device_status (*write_sector)(
	    kernel_object_handle_t context, block_lba_t lba,
	    const union block_device_sector *sector);
	enum block_device_status (*flush)(kernel_object_handle_t context);
};

/*
 * Persistent consumers store only the 64-bit handle.  Backend-native function
 * pointers remain private to this architecture-selected registry.  Adapter
 * callbacks return this private status domain, never hosted errno values. An
 * unrecognized callback result is converted to BLOCK_DEVICE_IO_ERROR;
 * NO_SLOT, STALE_HANDLE, and GENERATION_EXHAUSTED are registry-only results and
 * are likewise invalid when returned by an adapter.
 *
 * Registration and geometry/read outputs are published only on success and
 * remain unchanged on every error.  writable is canonical 0 or 1; reserved
 * bytes must remain zero.  A slot which has issued GENERATION_MAX is retired
 * after unregister and can never wrap to a handle that revives an old value.
 */
enum block_device_status
block_device_register(const struct block_device_ops *ops,
		      kernel_object_handle_t context,
		      block_device_handle_t *handle) __must_check;
enum block_device_status
block_device_unregister(block_device_handle_t handle) __must_check;
enum block_device_status
block_device_get_geometry(block_device_handle_t handle,
			  struct block_device_geometry *geometry) __must_check;
enum block_device_status
block_device_read_sector(block_device_handle_t handle, block_lba_t lba,
			 union block_device_sector *sector) __must_check;
enum block_device_status
block_device_write_sector(block_device_handle_t handle, block_lba_t lba,
			  const union block_device_sector *sector) __must_check;
enum block_device_status
block_device_flush(block_device_handle_t handle) __must_check;
const char *block_device_status_string(enum block_device_status status);

static_assert_expression(
    sizeof(union block_device_sector) == BLOCK_DEVICE_SECTOR_BYTES,
    "block-device transfer object must be exactly one 512-byte sector");
static_assert_expression(sizeof(block_device_handle_t) == 8,
				 "persistent block-device handles must remain 64-bit");
static_assert_expression(sizeof(struct block_device_geometry) == 16,
				 "block-device geometry must be data-model independent");
static_assert_expression(__alignof__(struct block_device_geometry) == 8,
				 "block-device geometry alignment changed");
static_assert_expression(__builtin_offsetof(struct block_device_geometry,
						    writable) == 12,
				 "block-device writable offset changed");

#endif

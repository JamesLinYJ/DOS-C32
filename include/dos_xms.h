/* SPDX-License-Identifier: GPL-2.0-only */
/* XMS 3.0-compatible extended-memory service owned by the DOS personality. */
#ifndef DOSC32_DOS_XMS_H
#define DOSC32_DOS_XMS_H

#include "compiler.h"
#include "dos_machine.h"
#include "types.h"

#define DOS_XMS_CONTROL_VECTOR 0xfeu
#define DOS_XMS_CONTROL_SEGMENT 0x0f00u
#define DOS_XMS_CONTROL_OFFSET 0x0100u
#define DOS_XMS_HANDLE_COUNT 32u
#define DOS_XMS_HMA_BASE 0x00100000ull
#define DOS_XMS_HMA_BYTES 0x0000fff0ull
#define DOS_XMS_HMA_LIMIT (DOS_XMS_HMA_BASE + DOS_XMS_HMA_BYTES)
#define DOS_XMS_MANAGER_GENERATION_MAX (~(uint64_t)0u)

enum dos_xms_memory_status {
	DOS_XMS_MEMORY_OK = 0,
	DOS_XMS_MEMORY_NO_MEMORY,
	DOS_XMS_MEMORY_FAULT,
	DOS_XMS_MEMORY_UNCERTAIN
};

typedef uint64_t dos_xms_block_t;

#define DOS_XMS_BLOCK_INVALID ((dos_xms_block_t)0u)

typedef enum dos_xms_memory_status (*dos_xms_memory_query_fn)(
	kernel_object_handle_t context, uint64_t *largest_bytes,
	uint64_t *total_bytes, uint64_t *highest_address);
typedef enum dos_xms_memory_status (*dos_xms_memory_allocate_fn)(
	kernel_object_handle_t context, uint64_t requested_bytes,
	dos_xms_block_t *block, uint64_t *physical_address,
	uint64_t *capacity_bytes);
typedef enum dos_xms_memory_status (*dos_xms_memory_release_fn)(
	kernel_object_handle_t context, dos_xms_block_t block);

typedef enum dos_xms_memory_status (*dos_xms_memory_read_fn)(
	kernel_object_handle_t context, dos_xms_block_t block, uint64_t offset,
	void *destination, size_t capacity, size_t count);
typedef enum dos_xms_memory_status (*dos_xms_memory_write_fn)(
	kernel_object_handle_t context, dos_xms_block_t block, uint64_t offset,
	const void *source, size_t capacity, size_t count);

/* Immutable proof that the exact HMA belongs to the active guest mapping. */
struct dos_xms_hma_snapshot {
	kernel_object_handle_t address_space_identity;
	uint64_t address_space_generation;
	kernel_object_handle_t machine_context;
	uint64_t base_address;
	uint64_t byte_count;
} __aligned(8);

typedef enum dos_xms_memory_status (*dos_xms_hma_query_fn)(
	kernel_object_handle_t context, const struct dos_machine *machine,
	struct dos_xms_hma_snapshot *snapshot);

struct dos_xms_memory_ops {
	dos_xms_memory_query_fn query;
	dos_xms_memory_allocate_fn allocate;
	dos_xms_memory_release_fn release;
	dos_xms_memory_read_fn read;
	dos_xms_memory_write_fn write;
	dos_xms_hma_query_fn query_hma;
};

struct dos_xms_config {
	uint16_t hma_minimum_bytes;
	uint8_t reserved[6];
} __aligned(8);

struct dos_xms_hma_lease {
	kernel_object_handle_t manager_identity;
	uint64_t manager_generation;
	uint64_t lease_generation;
	struct dos_xms_hma_snapshot mapping;
	uint8_t active;
	uint8_t reserved[7];
} __aligned(8);

struct dos_xms_handle {
	dos_xms_block_t block;
	uint64_t size_bytes;
	uint64_t capacity_bytes;
	uint64_t physical_address;
	uint8_t lock_count;
	uint8_t reserved[7];
};

struct dos_xms_manager {
	const struct dos_xms_memory_ops *ops;
	kernel_object_handle_t memory_context;
	kernel_object_handle_t identity;
	uint64_t generation;
	uint64_t hma_generation;
	struct dos_xms_hma_lease hma;
	uint32_t allocated_bitmap;
	struct dos_xms_handle handles[DOS_XMS_HANDLE_COUNT];
	uint16_t local_a20_locks;
	uint16_t hma_minimum_bytes;
	uint8_t initialized;
	uint8_t poisoned;
	uint8_t constructed;
	uint8_t reserved[3];
} __aligned(8);

static_assert_expression(sizeof(struct dos_xms_hma_snapshot) == 40u,
			 "XMS HMA snapshots must remain fixed width");
static_assert_expression(sizeof(struct dos_xms_hma_lease) == 72u,
			 "XMS HMA leases must remain fixed width");
static_assert_expression(__alignof__(struct dos_xms_hma_lease) == 8u,
			 "XMS HMA lease alignment changed");

enum dos_xms_status {
	DOS_XMS_READY = 0,
	DOS_XMS_CHAIN,
	DOS_XMS_INVALID_ARGUMENT,
	DOS_XMS_MACHINE_FAULT
};

enum dos_xms_status dos_xms_construct(
	struct dos_xms_manager *manager,
	kernel_object_handle_t manager_identity) __must_check;

enum dos_xms_status dos_xms_initialize(
	struct dos_xms_manager *manager, const struct dos_machine *machine,
	const struct dos_xms_memory_ops *ops,
	kernel_object_handle_t memory_context,
	const struct dos_xms_config *config) __must_check;

/* INT 2Fh installation/address calls and the private three-byte call gate. */
enum dos_xms_status dos_xms_multiplex(
	const struct dos_xms_manager *manager,
	struct dos_cpu_state *state) __must_check;
enum dos_xms_status dos_xms_control(
	struct dos_xms_manager *manager, struct dos_machine *machine,
	struct dos_cpu_state *state) __must_check;

#endif

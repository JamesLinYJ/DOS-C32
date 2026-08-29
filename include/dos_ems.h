/* SPDX-License-Identifier: GPL-2.0-only */
/* EMS 4.0 state owner and VCPI 1.0 real/protected-mode service core. */
#ifndef DOSC32_DOS_EMS_H
#define DOSC32_DOS_EMS_H

#include "compiler.h"
#include "dos_machine.h"
#include "dos_vcpi.h"
#include "types.h"

/* EMS/VCPI ABI units, not detected platform capacity. */
#define DOS_EMS_VERSION 0x40u
#define DOS_EMS_PAGE_SHIFT 14u
#define DOS_EMS_PAGE_BYTES (1u << DOS_EMS_PAGE_SHIFT)
#define DOS_EMS_NATIVE_PAGE_SHIFT 12u
#define DOS_EMS_NATIVE_PAGE_BYTES (1u << DOS_EMS_NATIVE_PAGE_SHIFT)
#define DOS_EMS_PAGE_PARAGRAPHS (DOS_EMS_PAGE_BYTES / 16u)
#define DOS_EMS_PAGE_FRAME_SLOTS 4u
#define DOS_EMS_NATIVE_PAGES_PER_PAGE                                  \
	(DOS_EMS_PAGE_BYTES / DOS_EMS_NATIVE_PAGE_BYTES)
#define DOS_EMS_DEVICE_NAME_BYTES 8u
/* Auditable bookkeeping ceilings; neither value is reported as RAM. */
#define DOS_EMS_HANDLE_COUNT 32u
#define DOS_VCPI_ALLOCATION_COUNT 256u
#define DOS_VCPI_ALLOCATION_BITMAP_WORDS                               \
	((DOS_VCPI_ALLOCATION_COUNT + 31u) / 32u)

typedef uint64_t dos_ems_page_block_t;

#define DOS_EMS_PAGE_BLOCK_INVALID ((dos_ems_page_block_t)0u)

enum dos_ems_page_status {
	DOS_EMS_PAGE_OK = 0,
	DOS_EMS_PAGE_NO_MEMORY,
	DOS_EMS_PAGE_INVALID_BLOCK,
	DOS_EMS_PAGE_FAULT,
	DOS_EMS_PAGE_UNCERTAIN
};

struct dos_ems_page_snapshot {
	uint64_t largest_free_pages;
	uint64_t total_free_pages;
	uint64_t managed_pages;
	uint64_t highest_address;
} __aligned(8);

typedef enum dos_ems_page_status (*dos_ems_page_query_fn)(
	kernel_object_handle_t context,
	struct dos_ems_page_snapshot *snapshot);
typedef enum dos_ems_page_status (*dos_ems_page_allocate_fn)(
	kernel_object_handle_t context, uint64_t requested_pages,
	dos_ems_page_block_t *block, uint64_t *physical_address,
	uint64_t *capacity_pages);
typedef enum dos_ems_page_status (*dos_ems_page_release_fn)(
	kernel_object_handle_t context, dos_ems_page_block_t block);

struct dos_ems_page_ops {
	dos_ems_page_query_fn query;
	dos_ems_page_allocate_fn allocate;
	dos_ems_page_release_fn release;
};

typedef uint64_t dos_ems_page_frame_lease_t;

#define DOS_EMS_PAGE_FRAME_LEASE_INVALID ((dos_ems_page_frame_lease_t)0u)

enum dos_ems_page_frame_status {
	DOS_EMS_PAGE_FRAME_OK = 0,
	DOS_EMS_PAGE_FRAME_UNAVAILABLE,
	DOS_EMS_PAGE_FRAME_CONFLICT,
	DOS_EMS_PAGE_FRAME_FAULT,
	DOS_EMS_PAGE_FRAME_UNCERTAIN
};

/*
 * The platform owns discovery, conflict checks, PTE updates and rollback for
 * the complete 64 KiB page-frame window.  A successful map/unmap is atomic
 * for one 16 KiB EMS slot.  UNCERTAIN quarantines the EMS manager.
 */
typedef enum dos_ems_page_frame_status (*dos_ems_page_frame_acquire_fn)(
	kernel_object_handle_t context, uint64_t linear_address,
	uint64_t byte_count, dos_ems_page_frame_lease_t *lease);
typedef enum dos_ems_page_frame_status (*dos_ems_page_frame_release_fn)(
	kernel_object_handle_t context, dos_ems_page_frame_lease_t lease);
typedef enum dos_ems_page_frame_status (*dos_ems_page_frame_map_fn)(
	kernel_object_handle_t context, dos_ems_page_frame_lease_t lease,
	uint8_t physical_page, uint64_t source_physical_address);
typedef enum dos_ems_page_frame_status (*dos_ems_page_frame_unmap_fn)(
	kernel_object_handle_t context, dos_ems_page_frame_lease_t lease,
	uint8_t physical_page);

struct dos_ems_page_frame_ops {
	dos_ems_page_frame_acquire_fn acquire;
	dos_ems_page_frame_release_fn release;
	dos_ems_page_frame_map_fn map;
	dos_ems_page_frame_unmap_fn unmap;
};

struct dos_ems_page_frame_binding {
	const struct dos_ems_page_frame_ops *ops;
	kernel_object_handle_t context;
	dos_ems_page_frame_lease_t lease;
	uint64_t linear_address;
	uint64_t byte_count;
} __aligned(8);

struct dos_ems_config {
	uint16_t page_frame_segment;
	uint16_t reserved;
	uint32_t reserved2;
};

/*
 * One immutable boot configuration for the DOS-visible service and its
 * named-device publication.  The device name is the exact padded eight-byte
 * DOS name; the I/O Manager copies it when the service is registered.
 */
struct dos_ems_runtime_config {
	struct dos_ems_config service;
	uint8_t device_name[DOS_EMS_DEVICE_NAME_BYTES];
	uint8_t reserved[8];
} __aligned(8);

struct dos_ems_handle {
	dos_ems_page_block_t block;
	uint64_t generation;
	uint64_t physical_address;
	uint64_t page_count;
	uint64_t capacity_pages;
} __aligned(8);

struct dos_vcpi_allocation {
	dos_ems_page_block_t block;
	uint64_t generation;
	uint64_t physical_address;
	uint64_t capacity_pages;
} __aligned(8);

struct dos_ems_page_frame_slot {
	uint64_t physical_address;
	uint16_t handle;
	uint16_t logical_page;
	uint8_t mapped;
	uint8_t reserved[3];
} __aligned(8);

struct dos_ems_manager {
	const struct dos_ems_page_ops *page_ops;
	kernel_object_handle_t page_context;
	struct dos_ems_page_frame_binding page_frame;
	const struct dos_vcpi_platform_ops *vcpi_ops;
	kernel_object_handle_t vcpi_context;
	uint32_t allocated_handles;
	uint32_t vcpi_allocated[DOS_VCPI_ALLOCATION_BITMAP_WORDS];
	struct dos_ems_handle handles[DOS_EMS_HANDLE_COUNT];
	struct dos_vcpi_allocation
		vcpi_allocations[DOS_VCPI_ALLOCATION_COUNT];
	struct dos_ems_page_frame_slot
		frame_slots[DOS_EMS_PAGE_FRAME_SLOTS];
	uint16_t page_frame_segment;
	uint8_t initialized;
	uint8_t constructed;
	uint8_t poisoned;
	uint8_t vcpi_available;
	uint8_t reserved[4];
} __aligned(8);

enum dos_ems_status {
	DOS_EMS_READY = 0,
	DOS_EMS_EXECUTION_TRANSFERRED,
	DOS_EMS_INVALID_ARGUMENT,
	DOS_EMS_MEMORY_FAULT,
	DOS_EMS_POISONED
};

void dos_ems_construct(struct dos_ems_manager *manager);
bool dos_ems_runtime_config_is_valid(
	const struct dos_ems_runtime_config *config) __must_check;
enum dos_ems_status dos_ems_initialize(
	struct dos_ems_manager *manager,
	const struct dos_ems_page_ops *page_ops,
	kernel_object_handle_t page_context,
	const struct dos_ems_page_frame_binding *page_frame,
	const struct dos_vcpi_platform_ops *vcpi_ops,
	kernel_object_handle_t vcpi_context,
	const struct dos_ems_config *config) __must_check;

/* Dispatch one INT 67h register frame. Unknown functions return AH=84h. */
enum dos_ems_status dos_ems_interrupt(
	struct dos_ems_manager *manager,
	struct dos_cpu_state *state) __must_check;

static_assert_expression(sizeof(struct dos_ems_page_snapshot) == 32u,
			 "EMS page snapshot layout changed");
static_assert_expression(sizeof(struct dos_ems_runtime_config) == 24u,
			 "EMS runtime configuration layout changed");
static_assert_expression(sizeof(struct dos_ems_handle) == 40u,
			 "EMS handle layout changed");
static_assert_expression(sizeof(struct dos_vcpi_allocation) == 32u,
			 "VCPI allocation layout changed");
static_assert_expression(sizeof(struct dos_ems_page_frame_slot) == 16u,
			 "EMS page-frame slot layout changed");
static_assert_expression(
	__alignof__(struct dos_ems_page_frame_binding) == 8u,
	"EMS page-frame binding alignment changed");

#endif

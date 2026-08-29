/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DOSC32_DOS_EMS_PRIVATE_H
#define DOSC32_DOS_EMS_PRIVATE_H

#include "dos_ems.h"

#define DOS_EMS_STATUS_OK 0x00u
#define DOS_EMS_STATUS_SOFTWARE_ERROR 0x80u
#define DOS_EMS_STATUS_HARDWARE_ERROR 0x81u
#define DOS_EMS_STATUS_INVALID_HANDLE 0x83u
#define DOS_EMS_STATUS_INVALID_FUNCTION 0x84u
#define DOS_EMS_STATUS_NO_MORE_HANDLES 0x85u
#define DOS_EMS_STATUS_OUT_OF_PAGES 0x87u
#define DOS_EMS_STATUS_OUT_OF_FREE_PAGES 0x88u
#define DOS_EMS_STATUS_ZERO_PAGES 0x89u
#define DOS_EMS_STATUS_LOGICAL_PAGE_INVALID 0x8au
#define DOS_EMS_STATUS_PHYSICAL_PAGE_INVALID 0x8bu

#define DOS_EMS_GENERATION_MAX 0xfffffffffffffffeull

bool dos_ems_identity_is_valid(kernel_object_handle_t identity);
bool dos_ems_page_snapshot_is_valid(
	const struct dos_ems_page_snapshot *snapshot);
enum dos_ems_status dos_ems_query_pages(
	struct dos_ems_manager *manager,
	struct dos_ems_page_snapshot *snapshot);
enum dos_ems_status dos_ems_page_fault(
	struct dos_ems_manager *manager, enum dos_ems_page_status status);
bool dos_ems_allocation_result_is_valid(
	dos_ems_page_block_t block, uint64_t physical_address,
	uint64_t requested_pages, uint64_t capacity_pages);
enum dos_ems_status dos_ems_reject_invalid_allocation(
	struct dos_ems_manager *manager, dos_ems_page_block_t block);
void dos_ems_return_status(struct dos_cpu_state *state, uint8_t status);

enum dos_ems_status dos_vcpi_dispatch(
	struct dos_ems_manager *manager,
	struct dos_cpu_state *state) __must_check;

#endif

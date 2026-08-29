/* SPDX-License-Identifier: GPL-2.0-only */
/* Boot-lifetime owner for physical pages shared by x86 guest services. */
#ifndef DOSC32_X86_GUEST_MEMORY_RUNTIME_H
#define DOSC32_X86_GUEST_MEMORY_RUNTIME_H

#include "x86_guest_memory.h"

enum x86_guest_memory_status x86_guest_memory_runtime_initialize(
	const struct x86_boot_info *boot_info,
	kernel_object_handle_t manager_identity) __must_check;
enum x86_guest_memory_status x86_guest_memory_runtime_allocate(
	kernel_object_handle_t owner, uint32_t page_count,
	x86_guest_memory_lease_t *lease,
	uint32_t *physical_address) __must_check;
enum x86_guest_memory_status x86_guest_memory_runtime_release(
	kernel_object_handle_t owner,
	x86_guest_memory_lease_t lease) __must_check;
enum x86_guest_memory_status x86_guest_memory_runtime_inspect(
	x86_guest_memory_lease_t lease,
	struct x86_guest_memory_lease_info *info) __must_check;
enum x86_guest_memory_status x86_guest_memory_runtime_query_free(
	uint32_t *free_pages) __must_check;
enum x86_guest_memory_status x86_guest_memory_runtime_query_snapshot(
	struct x86_guest_memory_snapshot *snapshot) __must_check;
enum x86_guest_memory_status x86_guest_memory_runtime_query_capacity(
	uint32_t *largest_free_pages,
	uint32_t *total_free_pages,
	uint64_t *highest_address) __must_check;

#endif

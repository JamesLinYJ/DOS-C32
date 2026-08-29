/* SPDX-License-Identifier: GPL-2.0-only */
/* Bounded sector transaction interface for mounted writable volumes. */
#ifndef DOSC32_IOMGR_TRANSACTION_H
#define DOSC32_IOMGR_TRANSACTION_H

#include "block_device.h"
#include "iomgr.h"

#define IOMGR_TRANSACTION_HANDLE_INVALID KERNEL_OBJECT_HANDLE_INVALID
#define IOMGR_TRANSACTION_MAX_SECTORS 16u

typedef kernel_object_handle_t iomgr_transaction_handle_t;

enum iomgr_status
iomgr_transaction_begin(iomgr_volume_handle_t volume,
			 iomgr_transaction_handle_t *transaction) __must_check;
enum iomgr_status
iomgr_transaction_stage(
	iomgr_transaction_handle_t transaction, block_lba_t volume_relative_lba,
	const union block_device_sector *after) __must_check;
enum iomgr_status
iomgr_transaction_commit(iomgr_transaction_handle_t transaction) __must_check;
enum iomgr_status
iomgr_transaction_abort(iomgr_transaction_handle_t transaction) __must_check;

static_assert_expression(sizeof(iomgr_transaction_handle_t) == 8u,
			 "I/O Manager transaction handles must remain 64-bit");

#endif

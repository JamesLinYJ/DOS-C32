// SPDX-License-Identifier: GPL-2.0-only
/*
 * I/O Manager bounded sector transaction pool
 *
 * DOS contract:   none; filesystem drivers choose their metadata order
 * Safety changes: before-images, reverse rollback, flush, volume quarantine
 *
 * Acquire-before-publish and reverse unwind protect in-memory ownership. This
 * is not a journal and does not claim atomic recovery across sudden power loss.
 */
#include "iomgr_transaction.h"

#include "iomgr_driver.h"
#include "overflow.h"

#define IOMGR_TRANSACTION_SLOTS 2u
#define IOMGR_TRANSACTION_GENERATION_MAX 0xffffffffu

enum transaction_state {
	TRANSACTION_FREE = 0,
	TRANSACTION_LIVE,
	TRANSACTION_QUARANTINED,
	TRANSACTION_RETIRED
};

struct transaction_entry {
	block_lba_t relative_lba;
	union block_device_sector before;
	union block_device_sector after;
};

struct transaction_slot {
	enum transaction_state state;
	uint32_t generation;
	iomgr_volume_handle_t volume;
	struct iomgr_volume_info binding;
	size_t entry_count;
	struct transaction_entry entries[IOMGR_TRANSACTION_MAX_SECTORS];
};

static struct transaction_slot transactions[IOMGR_TRANSACTION_SLOTS];

static iomgr_transaction_handle_t make_handle(size_t index,
					       uint32_t generation)
{
	return ((uint64_t)generation << 32) | (uint64_t)(index + 1u);
}

static enum iomgr_status resolve_transaction(
	iomgr_transaction_handle_t handle, struct transaction_slot **transaction)
{
	uint32_t encoded_slot = (uint32_t)handle;
	uint32_t generation = (uint32_t)(handle >> 32);
	struct transaction_slot *slot;

	if (handle == 0u || handle == IOMGR_TRANSACTION_HANDLE_INVALID ||
	    encoded_slot == 0u || encoded_slot > IOMGR_TRANSACTION_SLOTS ||
	    generation == 0u)
		return IOMGR_STALE_HANDLE;
	slot = &transactions[encoded_slot - 1u];
	if (slot->generation != generation)
		return IOMGR_STALE_HANDLE;
	if (slot->state == TRANSACTION_QUARANTINED)
		return IOMGR_POISONED;
	if (slot->state != TRANSACTION_LIVE)
		return IOMGR_STALE_HANDLE;
	*transaction = slot;
	return IOMGR_OK;
}

static size_t reserve_transaction(void)
{
	size_t index;

	for (index = 0u; index < IOMGR_TRANSACTION_SLOTS; ++index) {
		struct transaction_slot *slot = &transactions[index];

		if (slot->state != TRANSACTION_FREE)
			continue;
		if (slot->generation == IOMGR_TRANSACTION_GENERATION_MAX) {
			slot->state = TRANSACTION_RETIRED;
			continue;
		}
		++slot->generation;
		slot->state = TRANSACTION_LIVE;
		return index;
	}
	return IOMGR_TRANSACTION_SLOTS;
}

enum iomgr_status
iomgr_transaction_begin(iomgr_volume_handle_t volume,
			 iomgr_transaction_handle_t *transaction)
{
	struct iomgr_volume_info info;
	struct transaction_slot *slot;
	enum iomgr_status status;
	size_t index;

	if (transaction == NULL)
		return IOMGR_INVALID_ARGUMENT;
	status = iomgr_get_volume_info(volume, &info);
	if (status != IOMGR_OK)
		return status;
	if ((info.capabilities & IOMGR_VOLUME_CAP_WRITE) == 0u)
		return IOMGR_READ_ONLY;
	index = reserve_transaction();
	if (index == IOMGR_TRANSACTION_SLOTS)
		return IOMGR_NO_SLOT;
	slot = &transactions[index];
	slot->volume = volume;
	slot->binding = info;
	slot->entry_count = 0u;
	*transaction = make_handle(index, slot->generation);
	return IOMGR_OK;
}

static bool bindings_match(const struct iomgr_volume_info *left,
			   const struct iomgr_volume_info *right)
{
	return left->driver_identity == right->driver_identity &&
	       left->device == right->device && left->first_lba == right->first_lba &&
	       left->sector_count == right->sector_count &&
	       left->capabilities == right->capabilities &&
	       left->maximum_name_units == right->maximum_name_units &&
	       left->reserved == 0u && right->reserved == 0u;
}

enum iomgr_status
iomgr_transaction_stage(iomgr_transaction_handle_t transaction,
			block_lba_t volume_relative_lba,
			const union block_device_sector *after)
{
	struct transaction_slot *slot;
	struct transaction_entry *entry;
	block_lba_t physical_lba;
	enum iomgr_status status;
	size_t index;

	if (after == NULL)
		return IOMGR_INVALID_ARGUMENT;
	status = resolve_transaction(transaction, &slot);
	if (status != IOMGR_OK)
		return status;
	if (volume_relative_lba >= slot->binding.sector_count ||
	    check_add_overflow(slot->binding.first_lba, volume_relative_lba,
			       &physical_lba))
		return IOMGR_INVALID_ARGUMENT;
	for (index = 0u; index < slot->entry_count; ++index) {
		if (slot->entries[index].relative_lba == volume_relative_lba) {
			slot->entries[index].after = *after;
			return IOMGR_OK;
		}
	}
	if (slot->entry_count == IOMGR_TRANSACTION_MAX_SECTORS)
		return IOMGR_NO_SLOT;
	entry = &slot->entries[slot->entry_count];
	if (block_device_read_sector(slot->binding.device, physical_lba,
				     &entry->before) != BLOCK_DEVICE_OK)
		return IOMGR_IO_ERROR;
	entry->relative_lba = volume_relative_lba;
	entry->after = *after;
	++slot->entry_count;
	return IOMGR_OK;
}

static bool rollback(struct transaction_slot *slot, size_t written)
{
	bool restored = true;

	while (written != 0u) {
		struct transaction_entry *entry;
		block_lba_t physical_lba;

		--written;
		entry = &slot->entries[written];
		if (check_add_overflow(slot->binding.first_lba,
				       entry->relative_lba, &physical_lba) ||
		    block_device_write_sector(slot->binding.device, physical_lba,
					      &entry->before) != BLOCK_DEVICE_OK)
			restored = false;
	}
	if (block_device_flush(slot->binding.device) != BLOCK_DEVICE_OK)
		restored = false;
	return restored;
}

enum iomgr_status
iomgr_transaction_commit(iomgr_transaction_handle_t transaction)
{
	struct transaction_slot *slot;
	struct iomgr_volume_info current;
	block_lba_t physical_lba;
	enum iomgr_status status;
	size_t written = 0u;

	status = resolve_transaction(transaction, &slot);
	if (status != IOMGR_OK)
		return status;
	status = iomgr_get_volume_info(slot->volume, &current);
	if (status != IOMGR_OK || !bindings_match(&slot->binding, &current)) {
		slot->state = TRANSACTION_FREE;
		return status == IOMGR_OK ? IOMGR_STALE_HANDLE : status;
	}
	while (written < slot->entry_count) {
		struct transaction_entry *entry = &slot->entries[written];

		if (check_add_overflow(slot->binding.first_lba,
				       entry->relative_lba, &physical_lba) ||
		    block_device_write_sector(slot->binding.device, physical_lba,
					      &entry->after) != BLOCK_DEVICE_OK)
			break;
		++written;
	}
	if (written == slot->entry_count &&
	    block_device_flush(slot->binding.device) == BLOCK_DEVICE_OK) {
		slot->state = TRANSACTION_FREE;
		return IOMGR_OK;
	}
	if (rollback(slot, written)) {
		slot->state = TRANSACTION_FREE;
		return IOMGR_IO_ERROR;
	}
	slot->state = TRANSACTION_QUARANTINED;
	if (iomgr_quarantine_volume(slot->volume) != IOMGR_OK)
		return IOMGR_POISONED;
	return IOMGR_UNCERTAIN;
}

enum iomgr_status
iomgr_transaction_abort(iomgr_transaction_handle_t transaction)
{
	struct transaction_slot *slot;
	enum iomgr_status status;

	status = resolve_transaction(transaction, &slot);
	if (status != IOMGR_OK)
		return status;
	slot->state = TRANSACTION_FREE;
	return IOMGR_OK;
}

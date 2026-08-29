// SPDX-License-Identifier: GPL-2.0-only
/*
 * FAT12/16/32 bounded metadata mutations behind the I/O Manager ABI.
 *
 * Allocation and publication validate every affected sector before mutation,
 * then use an explicit transaction for rollback and quarantine.
 */
#include "internal.h"

#include "fat_table.h"
#include "iomgr_transaction.h"
#include "overflow.h"

#define FAT_DIRENT_BYTES 32u
#define FAT_DIRENTS_PER_SECTOR \
	(BLOCK_DEVICE_SECTOR_BYTES / FAT_DIRENT_BYTES)
#define FAT_ATTRIBUTE_DIRECTORY 0x10u
#define FAT_ATTRIBUTE_READ_ONLY 0x01u
#define FAT_ATTRIBUTE_ARCHIVE 0x20u
#define FAT_ATTRIBUTE_LFN 0x0fu
#define FAT_MUTATION_SECTORS IOMGR_TRANSACTION_MAX_SECTORS

struct fat_directory_scan {
	bool existing;
	bool free_slot;
	block_lba_t free_lba;
	uint16_t free_offset;
	uint32_t last_cluster;
	block_lba_t existing_lba;
	uint16_t existing_offset;
	uint8_t existing_raw[FAT_DIRENT_BYTES];
};

struct fat_mutation_sector {
	block_lba_t lba;
	union block_device_sector data;
};

struct fat_mutation_cache {
	size_t count;
	struct fat_mutation_sector sectors[FAT_MUTATION_SECTORS];
};

static uint16_t read_le16(const uint8_t *bytes)
{
	return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static void write_le16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
}

static void write_le32(uint8_t *bytes, uint32_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
	bytes[2] = (uint8_t)(value >> 16);
	bytes[3] = (uint8_t)(value >> 24);
}

static void clear_sector(union block_device_sector *sector)
{
	size_t index;

	for (index = 0u; index < ARRAY_SIZE(sector->bytes); ++index)
		sector->bytes[index] = 0u;
}

static bool path_separator(uint8_t character)
{
	return character == '/' || character == '\\';
}

static bool cluster_valid(const struct fat_driver_volume_snapshot *volume,
			  uint32_t cluster)
{
	return cluster >= 2u && cluster < volume->layout.cluster_limit;
}

static struct fat_table_layout table_layout(
	const struct fat_driver_volume_snapshot *volume)
{
	return (struct fat_table_layout){
		.fat_start = volume->layout.reserved_sectors,
		.sectors_per_fat = volume->layout.sectors_per_fat,
		.entry_limit = volume->layout.cluster_limit,
		.sector_bytes = volume->layout.sector_bytes,
		.fat_count = volume->layout.fat_count,
		.fat_bits = volume->layout.fat_bits,
	};
}

static enum iomgr_status read_relative(
	const struct fat_driver_volume_snapshot *volume, block_lba_t relative,
	union block_device_sector *sector)
{
	block_lba_t physical;

	if (relative >= volume->layout.total_sectors ||
	    check_add_overflow(volume->first_lba, relative, &physical))
		return IOMGR_CORRUPT;
	return block_device_read_sector(volume->device, physical, sector) ==
		       BLOCK_DEVICE_OK
		       ? IOMGR_OK
		       : IOMGR_IO_ERROR;
}

static enum iomgr_status write_relative(
	const struct fat_driver_volume_snapshot *volume, block_lba_t relative,
	const union block_device_sector *sector)
{
	block_lba_t physical;

	if (relative >= volume->layout.total_sectors ||
	    check_add_overflow(volume->first_lba, relative, &physical))
		return IOMGR_CORRUPT;
	return block_device_write_sector(volume->device, physical, sector) ==
		       BLOCK_DEVICE_OK
		       ? IOMGR_OK
		       : IOMGR_IO_ERROR;
}

static enum iomgr_status cluster_sector_lba(
	const struct fat_driver_volume_snapshot *volume, uint32_t cluster,
	uint32_t sector_index, block_lba_t *lba)
{
	block_lba_t offset;
	block_lba_t candidate;

	if (!cluster_valid(volume, cluster) || lba == NULL ||
	    sector_index >= volume->layout.sectors_per_cluster ||
	    check_mul_overflow((block_lba_t)(cluster - 2u),
			       (block_lba_t)volume->layout.sectors_per_cluster,
			       &offset) ||
	    check_add_overflow((block_lba_t)volume->layout.data_start, offset,
			       &candidate) ||
	    check_add_overflow(candidate, (block_lba_t)sector_index,
			       &candidate) ||
	    candidate >= volume->layout.total_sectors)
		return IOMGR_CORRUPT;
	*lba = candidate;
	return IOMGR_OK;
}

static enum iomgr_status read_fat_entry(
	const struct fat_driver_volume_snapshot *volume, uint32_t cluster,
	uint32_t *value, enum fat_table_entry_kind *kind)
{
	struct fat_table_layout table = table_layout(volume);
	struct fat_table_position position;
	union block_device_sector first;
	union block_device_sector second;
	block_lba_t lba;
	block_lba_t second_lba;
	uint32_t reference_value = 0u;
	enum fat_table_entry_kind reference_kind = FAT_TABLE_ENTRY_INVALID;
	uint8_t copy;

	if (value == NULL || kind == NULL || !cluster_valid(volume, cluster) ||
	    fat_table_locate(&table, cluster, &position) != FAT_TABLE_OK)
		return IOMGR_CORRUPT;
	for (copy = 0u; copy < table.fat_count; ++copy) {
		uint32_t current_value;
		enum fat_table_entry_kind current_kind;

		if (fat_table_copy_lba(&table, copy, position.sector_index,
				       &lba) != FAT_TABLE_OK ||
		    read_relative(volume, lba, &first) != IOMGR_OK)
			return IOMGR_IO_ERROR;
		if (position.sector_count == 2u) {
			if (fat_table_copy_lba(&table, copy,
					       position.sector_index + 1u,
					       &second_lba) != FAT_TABLE_OK)
				return IOMGR_CORRUPT;
			if (read_relative(volume, second_lba, &second) != IOMGR_OK)
				return IOMGR_IO_ERROR;
		}
		if (fat_table_read(&table, cluster, &first,
				   position.sector_count == 2u ? &second : NULL,
				   &current_value, &current_kind) != FAT_TABLE_OK)
			return IOMGR_CORRUPT;
		if (copy == 0u) {
			reference_value = current_value;
			reference_kind = current_kind;
		} else if (current_value != reference_value ||
			   current_kind != reference_kind) {
			return IOMGR_CORRUPT;
		}
	}
	*value = reference_value;
	*kind = reference_kind;
	return IOMGR_OK;
}

static enum iomgr_status next_cluster(
	const struct fat_driver_volume_snapshot *volume, uint32_t cluster,
	uint32_t *next, bool *at_end)
{
	uint32_t value;
	enum fat_table_entry_kind kind;
	enum iomgr_status status =
		read_fat_entry(volume, cluster, &value, &kind);

	if (status != IOMGR_OK)
		return status;
	if (kind == FAT_TABLE_ENTRY_EOC) {
		*next = 0u;
		*at_end = true;
		return IOMGR_OK;
	}
	if (kind != FAT_TABLE_ENTRY_DATA)
		return IOMGR_CORRUPT;
	*next = value;
	*at_end = false;
	return IOMGR_OK;
}

static uint32_t directory_entry_cluster(
	const struct fat_driver_volume_snapshot *volume, const uint8_t *raw)
{
	uint32_t cluster = read_le16(raw + 26u);

	if (volume->layout.fat_bits == FAT_TABLE_32)
		cluster |= (uint32_t)read_le16(raw + 20u) << 16;
	return cluster;
}

static bool names_equal(const uint8_t name83[11], const uint8_t *raw)
{
	size_t index;

	for (index = 0u; index < 11u; ++index) {
		uint8_t value = raw[index];

		if (index == 0u && value == 0x05u)
			value = 0xe5u;
		if (name83[index] != value)
			return false;
	}
	return true;
}

static void remember_free(struct fat_directory_scan *scan, block_lba_t lba,
			  uint16_t offset)
{
	if (scan->free_slot)
		return;
	scan->free_slot = true;
	scan->free_lba = lba;
	scan->free_offset = offset;
}

static bool scan_sector(const union block_device_sector *sector,
			block_lba_t lba, const uint8_t name83[11],
			struct fat_directory_scan *scan)
{
	size_t entry;

	for (entry = 0u; entry < FAT_DIRENTS_PER_SECTOR; ++entry) {
		uint16_t offset = (uint16_t)(entry * FAT_DIRENT_BYTES);
		const uint8_t *raw = sector->bytes + offset;
		size_t index;

		if (raw[0] == 0u) {
			remember_free(scan, lba, offset);
			return true;
		}
		if (raw[0] == 0xe5u) {
			remember_free(scan, lba, offset);
			continue;
		}
		if (raw[11] == FAT_ATTRIBUTE_LFN ||
		    (raw[11] & IOMGR_NODE_VOLUME_LABEL) != 0u ||
		    !names_equal(name83, raw))
			continue;
		for (index = 0u; index < FAT_DIRENT_BYTES; ++index)
			scan->existing_raw[index] = raw[index];
		scan->existing = true;
		scan->existing_lba = lba;
		scan->existing_offset = offset;
		return true;
	}
	return false;
}

static enum iomgr_status scan_directory(
	const struct fat_driver_volume_snapshot *volume, uint32_t directory,
	const uint8_t name83[11], struct fat_directory_scan *scan)
{
	union block_device_sector sector;
	uint32_t current;
	uint32_t steps = 0u;
	bool fixed_root;

	*scan = (struct fat_directory_scan){ 0 };
	fixed_root = directory == 0u &&
		     volume->layout.fat_bits != FAT_TABLE_32;
	if (fixed_root) {
		uint32_t sector_index;

		for (sector_index = 0u;
		     sector_index < volume->layout.root_sectors; ++sector_index) {
			block_lba_t lba;

			if (check_add_overflow(
				    (block_lba_t)volume->layout.root_start,
				    (block_lba_t)sector_index, &lba) ||
			    read_relative(volume, lba, &sector) != IOMGR_OK)
				return IOMGR_IO_ERROR;
			if (scan_sector(&sector, lba, name83, scan))
				return IOMGR_OK;
		}
		return IOMGR_OK;
	}
	current = directory == 0u ? volume->layout.root_cluster : directory;
	if (!cluster_valid(volume, current))
		return IOMGR_CORRUPT;
	for (;;) {
		uint32_t sector_index;
		uint32_t following;
		bool at_end;
		enum iomgr_status status;

		if (++steps > volume->layout.data_clusters)
			return IOMGR_CORRUPT;
		scan->last_cluster = current;
		for (sector_index = 0u;
		     sector_index < volume->layout.sectors_per_cluster;
		     ++sector_index) {
			block_lba_t lba;

			status = cluster_sector_lba(volume, current, sector_index,
						    &lba);
			if (status != IOMGR_OK)
				return status;
			status = read_relative(volume, lba, &sector);
			if (status != IOMGR_OK)
				return status;
			if (scan_sector(&sector, lba, name83, scan))
				return IOMGR_OK;
		}
		status = next_cluster(volume, current, &following, &at_end);
		if (status != IOMGR_OK)
			return status;
		if (at_end)
			return IOMGR_OK;
		current = following;
	}
}

static enum iomgr_status resolve_parent(
	const struct fat_driver_volume_snapshot *volume,
	const struct iomgr_path *path, uint32_t *parent,
	uint8_t child_name83[11])
{
	size_t end;
	size_t child_start;
	size_t cursor;
	uint32_t directory = 0u;

	if (path == NULL || parent == NULL || child_name83 == NULL ||
	    path->length == 0u)
		return IOMGR_INVALID_NAME;
	end = path->length;
	if (path_separator(path->bytes[end - 1u]))
		return IOMGR_INVALID_NAME;
	child_start = end;
	while (child_start != 0u &&
	       !path_separator(path->bytes[child_start - 1u]))
		--child_start;
	if (fat_short_name_encode(path->bytes + child_start,
				  end - child_start, child_name83) != IOMGR_OK)
		return IOMGR_INVALID_NAME;
	cursor = 0u;
	while (cursor < child_start) {
		struct fat_directory_scan scan;
		uint8_t name83[11];
		size_t start;
		size_t length;
		uint32_t cluster;
		enum iomgr_status status;

		while (cursor < child_start &&
		       path_separator(path->bytes[cursor]))
			++cursor;
		if (cursor == child_start)
			break;
		start = cursor;
		while (cursor < child_start &&
		       !path_separator(path->bytes[cursor]))
			++cursor;
		length = cursor - start;
		status = fat_short_name_encode(path->bytes + start, length,
					       name83);
		if (status != IOMGR_OK)
			return status;
		status = scan_directory(volume, directory, name83, &scan);
		if (status != IOMGR_OK)
			return status;
		if (!scan.existing)
			return IOMGR_NOT_FOUND;
		if ((scan.existing_raw[11] & FAT_ATTRIBUTE_DIRECTORY) == 0u)
			return IOMGR_NOT_DIRECTORY;
		cluster = directory_entry_cluster(volume, scan.existing_raw);
		if (!cluster_valid(volume, cluster))
			return IOMGR_CORRUPT;
		directory = cluster;
	}
	*parent = directory;
	return IOMGR_OK;
}

static enum iomgr_status find_free_clusters(
	kernel_object_handle_t volume_context,
	const struct fat_driver_volume_snapshot *volume, size_t needed,
	uint32_t *clusters)
{
	uint32_t cluster;
	uint32_t scanned;
	uint32_t start;
	size_t found = 0u;
	enum iomgr_status status;

	if (needed == 0u || needed > volume->layout.data_clusters)
		return IOMGR_INVALID_ARGUMENT;
	status = fat_driver_get_allocation_hint(volume_context, &start);
	if (status != IOMGR_OK)
		return status;
	if (!cluster_valid(volume, start))
		start = 2u;
	cluster = start;
	for (scanned = 0u; scanned < volume->layout.data_clusters; ++scanned) {
		uint32_t value;
		enum fat_table_entry_kind kind;

		status = read_fat_entry(volume, cluster, &value, &kind);

		(void)value;
		if (status != IOMGR_OK)
			return status;
		if (kind == FAT_TABLE_ENTRY_FREE) {
			clusters[found++] = cluster;
			if (found == needed) {
				uint32_t next = cluster + 1u;

				if (next >= volume->layout.cluster_limit)
					next = 2u;
				status = fat_driver_set_allocation_hint(
					volume_context, next);
				if (status != IOMGR_OK)
					return status;
			return IOMGR_OK;
			}
		}
		++cluster;
		if (cluster >= volume->layout.cluster_limit)
			cluster = 2u;
	}
	return IOMGR_NO_SPACE;
}

static enum iomgr_status zero_cluster(
	const struct fat_driver_volume_snapshot *volume, uint32_t cluster)
{
	union block_device_sector zero;
	uint32_t sector_index;

	clear_sector(&zero);
	for (sector_index = 0u;
	     sector_index < volume->layout.sectors_per_cluster;
	     ++sector_index) {
		block_lba_t lba;
		enum iomgr_status status = cluster_sector_lba(
			volume, cluster, sector_index, &lba);

		if (status != IOMGR_OK)
			return status;
		status = write_relative(volume, lba, &zero);
		if (status != IOMGR_OK)
			return status;
	}
	return IOMGR_OK;
}

static enum iomgr_status cache_sector(
	struct fat_mutation_cache *cache,
	const struct fat_driver_volume_snapshot *volume, block_lba_t lba,
	union block_device_sector **sector)
{
	size_t index;

	for (index = 0u; index < cache->count; ++index) {
		if (cache->sectors[index].lba == lba) {
			*sector = &cache->sectors[index].data;
			return IOMGR_OK;
		}
	}
	if (cache->count == FAT_MUTATION_SECTORS)
		return IOMGR_NO_SLOT;
	if (read_relative(volume, lba,
			  &cache->sectors[cache->count].data) != IOMGR_OK)
		return IOMGR_IO_ERROR;
	cache->sectors[cache->count].lba = lba;
	*sector = &cache->sectors[cache->count].data;
	++cache->count;
	return IOMGR_OK;
}

static enum iomgr_status cache_fat_entry(
	struct fat_mutation_cache *cache,
	const struct fat_driver_volume_snapshot *volume, uint32_t cluster,
	uint32_t value)
{
	struct fat_table_layout table = table_layout(volume);
	struct fat_table_position position;
	uint8_t copy;

	if (fat_table_locate(&table, cluster, &position) != FAT_TABLE_OK)
		return IOMGR_CORRUPT;
	for (copy = 0u; copy < table.fat_count; ++copy) {
		union block_device_sector *first;
		union block_device_sector *second = NULL;
		block_lba_t lba;
		block_lba_t second_lba;
		enum iomgr_status status;

		if (fat_table_copy_lba(&table, copy, position.sector_index,
				       &lba) != FAT_TABLE_OK)
			return IOMGR_CORRUPT;
		status = cache_sector(cache, volume, lba, &first);
		if (status != IOMGR_OK)
			return status;
		if (position.sector_count == 2u) {
			if (fat_table_copy_lba(&table, copy,
					       position.sector_index + 1u,
					       &second_lba) != FAT_TABLE_OK)
				return IOMGR_CORRUPT;
			status = cache_sector(cache, volume, second_lba, &second);
			if (status != IOMGR_OK)
				return status;
		}
		if (fat_table_write(&table, cluster, value, first, second) !=
		    FAT_TABLE_OK)
			return IOMGR_CORRUPT;
	}
	return IOMGR_OK;
}

static enum iomgr_status cache_free_chain(
	struct fat_mutation_cache *cache,
	const struct fat_driver_volume_snapshot *volume, uint32_t first_cluster,
	uint32_t *freed_clusters)
{
	uint32_t current = first_cluster;
	uint32_t steps = 0u;

	if (freed_clusters == NULL)
		return IOMGR_INVALID_ARGUMENT;
	*freed_clusters = 0u;
	if (current == 0u)
		return IOMGR_OK;
	while (cluster_valid(volume, current)) {
		uint32_t next;
		enum fat_table_entry_kind kind;
		enum iomgr_status status;

		if (++steps > volume->layout.data_clusters)
			return IOMGR_CORRUPT;
		status = read_fat_entry(volume, current, &next, &kind);
		if (status != IOMGR_OK)
			return status;
		if (kind != FAT_TABLE_ENTRY_DATA && kind != FAT_TABLE_ENTRY_EOC)
			return IOMGR_CORRUPT;
		status = cache_fat_entry(cache, volume, current, 0u);
		if (status != IOMGR_OK)
			return status;
		++*freed_clusters;
		if (kind == FAT_TABLE_ENTRY_EOC)
			return IOMGR_OK;
		current = next;
	}
	return IOMGR_CORRUPT;
}

static void encode_directory_entry(
	const struct fat_driver_volume_snapshot *volume, uint8_t *raw,
	const uint8_t name83[11], uint32_t cluster)
{
	size_t index;

	for (index = 0u; index < FAT_DIRENT_BYTES; ++index)
		raw[index] = 0u;
	for (index = 0u; index < 11u; ++index)
		raw[index] = name83[index];
	raw[11] = FAT_ATTRIBUTE_DIRECTORY;
	if (volume->layout.fat_bits == FAT_TABLE_32)
		write_le16(raw + 20u, (uint16_t)(cluster >> 16));
	write_le16(raw + 26u, (uint16_t)cluster);
	write_le32(raw + 28u, 0u);
}

static void encode_file_entry(uint8_t *raw, const uint8_t name83[11],
			      uint32_t attributes)
{
	size_t index;

	for (index = 0u; index < FAT_DIRENT_BYTES; ++index)
		raw[index] = 0u;
	for (index = 0u; index < 11u; ++index)
		raw[index] = name83[index];
	raw[11] = (uint8_t)(attributes | FAT_ATTRIBUTE_ARCHIVE);
}

static enum iomgr_status prepare_directory_sectors(
	struct fat_mutation_cache *cache,
	const struct fat_driver_volume_snapshot *volume,
	const struct fat_directory_scan *parent_scan, uint32_t parent,
	uint32_t child, uint32_t extension, const uint8_t child_name83[11])
{
	static const uint8_t dot[11] = {
		'.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '
	};
	static const uint8_t dotdot[11] = {
		'.', '.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '
	};
	union block_device_sector *sector;
	block_lba_t lba;
	enum iomgr_status status;

	status = cluster_sector_lba(volume, child, 0u, &lba);
	if (status != IOMGR_OK)
		return status;
	status = cache_sector(cache, volume, lba, &sector);
	if (status != IOMGR_OK)
		return status;
	encode_directory_entry(volume, sector->bytes, dot, child);
	encode_directory_entry(volume, sector->bytes + FAT_DIRENT_BYTES, dotdot,
			       parent);

	if (parent_scan->free_slot) {
		lba = parent_scan->free_lba;
	} else {
		status = cluster_sector_lba(volume, extension, 0u, &lba);
		if (status != IOMGR_OK)
			return status;
	}
	status = cache_sector(cache, volume, lba, &sector);
	if (status != IOMGR_OK)
		return status;
	encode_directory_entry(
		volume,
		sector->bytes +
			(parent_scan->free_slot ? parent_scan->free_offset : 0u),
		child_name83, child);
	return IOMGR_OK;
}

static enum iomgr_status abort_with_status(
	iomgr_transaction_handle_t transaction, enum iomgr_status failure)
{
	enum iomgr_status abort_status = iomgr_transaction_abort(transaction);

	return abort_status == IOMGR_OK ? failure : IOMGR_POISONED;
}

static enum iomgr_status commit_cache(
	iomgr_volume_handle_t volume, const struct fat_mutation_cache *cache)
{
	iomgr_transaction_handle_t transaction;
	enum iomgr_status status;
	size_t index;

	status = iomgr_transaction_begin(volume, &transaction);
	if (status != IOMGR_OK)
		return status;
	for (index = 0u; index < cache->count; ++index) {
		status = iomgr_transaction_stage(
			transaction, cache->sectors[index].lba,
			&cache->sectors[index].data);
		if (status != IOMGR_OK)
			return abort_with_status(transaction, status);
	}
	return iomgr_transaction_commit(transaction);
}

enum iomgr_status fat_create_directory(
	kernel_object_handle_t volume_context, const struct iomgr_path *path,
	iomgr_volume_handle_t volume_handle)
{
	struct fat_driver_volume_snapshot volume;
	struct fat_directory_scan parent_scan;
	struct fat_mutation_cache cache = { 0 };
	uint8_t child_name83[11];
	uint32_t free_clusters[2];
	uint32_t parent;
	uint32_t child;
	uint32_t extension = 0u;
	uint32_t eoc;
	size_t needed;
	enum iomgr_status status;

	status = fat_driver_snapshot_from_context(volume_context, &volume);
	if (status != IOMGR_OK)
		return status;
	status = resolve_parent(&volume, path, &parent, child_name83);
	if (status != IOMGR_OK)
		return status;
	status = scan_directory(&volume, parent, child_name83, &parent_scan);
	if (status != IOMGR_OK)
		return status;
	if (parent_scan.existing)
		return IOMGR_ALREADY_EXISTS;
	if (!parent_scan.free_slot && parent == 0u &&
	    volume.layout.fat_bits != FAT_TABLE_32)
		return IOMGR_NO_SPACE;
	needed = parent_scan.free_slot ? 1u : 2u;
	status = find_free_clusters(volume_context, &volume, needed,
				    free_clusters);
	if (status != IOMGR_OK)
		return status;
	child = free_clusters[0];
	if (needed == 2u)
		extension = free_clusters[1];
	status = zero_cluster(&volume, child);
	if (status == IOMGR_OK && extension != 0u)
		status = zero_cluster(&volume, extension);
	if (status != IOMGR_OK)
		return status;
	if (block_device_flush(volume.device) != BLOCK_DEVICE_OK)
		return IOMGR_IO_ERROR;
	eoc = fat_table_eoc_value(&(struct fat_table_layout){
		.fat_start = volume.layout.reserved_sectors,
		.sectors_per_fat = volume.layout.sectors_per_fat,
		.entry_limit = volume.layout.cluster_limit,
		.sector_bytes = volume.layout.sector_bytes,
		.fat_count = volume.layout.fat_count,
		.fat_bits = volume.layout.fat_bits,
	});
	if (eoc == 0u)
		return IOMGR_CORRUPT;
	status = cache_fat_entry(&cache, &volume, child, eoc);
	if (status == IOMGR_OK && extension != 0u)
		status = cache_fat_entry(&cache, &volume, extension, eoc);
	if (status == IOMGR_OK && extension != 0u)
		status = cache_fat_entry(&cache, &volume,
					 parent_scan.last_cluster, extension);
	if (status != IOMGR_OK)
		return status;
	status = prepare_directory_sectors(&cache, &volume, &parent_scan,
					   parent, child, extension,
					   child_name83);
	if (status != IOMGR_OK)
		return status;
	status = commit_cache(volume_handle, &cache);
	if (status != IOMGR_OK)
		return status;
	return fat_driver_adjust_free_clusters(volume_context, -(int32_t)needed);
}

enum iomgr_status fat_rename(kernel_object_handle_t volume_context,
			     const struct iomgr_path *old_path,
			     const struct iomgr_path *new_path,
			     iomgr_volume_handle_t volume_handle)
{
	struct fat_driver_volume_snapshot volume;
	struct fat_directory_scan source;
	struct fat_directory_scan target;
	struct fat_mutation_cache cache = {0};
	union block_device_sector *source_sector;
	union block_device_sector *target_sector;
	uint8_t old_name83[11];
	uint8_t new_name83[11];
	uint32_t old_parent;
	uint32_t new_parent;
	size_t index;
	enum iomgr_status status;

	status = fat_driver_snapshot_from_context(volume_context, &volume);
	if (status != IOMGR_OK)
		return status;
	status = resolve_parent(&volume, old_path, &old_parent, old_name83);
	if (status == IOMGR_OK)
		status = resolve_parent(&volume, new_path, &new_parent,
					new_name83);
	if (status != IOMGR_OK)
		return status;
	status = scan_directory(&volume, old_parent, old_name83, &source);
	if (status != IOMGR_OK)
		return status;
	if (!source.existing)
		return IOMGR_NOT_FOUND;
	status = scan_directory(&volume, new_parent, new_name83, &target);
	if (status != IOMGR_OK)
		return status;
	if (target.existing) {
		if (old_parent == new_parent &&
		    target.existing_lba == source.existing_lba &&
		    target.existing_offset == source.existing_offset)
			return IOMGR_OK;
		return IOMGR_ALREADY_EXISTS;
	}
	status = cache_sector(&cache, &volume, source.existing_lba,
			      &source_sector);
	if (status != IOMGR_OK)
		return status;
	if (old_parent == new_parent) {
		uint8_t *raw = source_sector->bytes + source.existing_offset;

		for (index = 0u; index < 11u; ++index)
			raw[index] = new_name83[index];
		return commit_cache(volume_handle, &cache);
	}
	/* Moving a directory across parents must also update its '..' entry.
	 * Refuse that operation until it can be included in the same transaction. */
	if ((source.existing_raw[11] & FAT_ATTRIBUTE_DIRECTORY) != 0u)
		return IOMGR_UNSUPPORTED;
	if (!target.free_slot)
		return IOMGR_NO_SPACE;
	status = cache_sector(&cache, &volume, target.free_lba,
			      &target_sector);
	if (status != IOMGR_OK)
		return status;
	for (index = 0u; index < FAT_DIRENT_BYTES; ++index)
		target_sector->bytes[target.free_offset + index] =
			source.existing_raw[index];
	for (index = 0u; index < 11u; ++index)
		target_sector->bytes[target.free_offset + index] = new_name83[index];
	source_sector->bytes[source.existing_offset] = 0xe5u;
	return commit_cache(volume_handle, &cache);
}

enum iomgr_status fat_create_file(
	kernel_object_handle_t volume_context, const struct iomgr_path *path,
	uint32_t attributes, iomgr_volume_handle_t volume_handle,
	struct fat_created_file *created)
{
	struct fat_driver_volume_snapshot volume;
	struct fat_directory_scan scan;
	struct fat_mutation_cache cache = {0};
	uint8_t name83[11];
	union block_device_sector *directory_sector;
	block_lba_t directory_lba;
	uint16_t directory_offset;
	uint32_t parent;
	uint32_t freed_clusters = 0u;
	bool allocated_extension = false;
	enum iomgr_status status;

	if (created == NULL || (attributes & ~0x27u) != 0u)
		return IOMGR_INVALID_ARGUMENT;
	status = fat_driver_snapshot_from_context(volume_context, &volume);
	if (status != IOMGR_OK)
		return status;
	status = resolve_parent(&volume, path, &parent, name83);
	if (status != IOMGR_OK)
		return status;
	status = scan_directory(&volume, parent, name83, &scan);
	if (status != IOMGR_OK)
		return status;
	if (scan.existing) {
		uint32_t first_cluster;

		if ((scan.existing_raw[11] & FAT_ATTRIBUTE_DIRECTORY) != 0u)
			return IOMGR_IS_DIRECTORY;
		if ((scan.existing_raw[11] & FAT_ATTRIBUTE_READ_ONLY) != 0u)
			return IOMGR_READ_ONLY;
		first_cluster = directory_entry_cluster(&volume,
						  scan.existing_raw);
		status = cache_free_chain(&cache, &volume, first_cluster,
					  &freed_clusters);
		if (status != IOMGR_OK)
			return status;
		directory_lba = scan.existing_lba;
		directory_offset = scan.existing_offset;
	} else if (scan.free_slot) {
		directory_lba = scan.free_lba;
		directory_offset = scan.free_offset;
	} else {
		struct fat_table_layout table = table_layout(&volume);
		uint32_t extension;
		uint32_t eoc;

		if (parent == 0u && volume.layout.fat_bits != FAT_TABLE_32)
			return IOMGR_NO_SPACE;
		status = find_free_clusters(volume_context, &volume, 1u,
					    &extension);
		if (status != IOMGR_OK)
			return status;
		allocated_extension = true;
		status = zero_cluster(&volume, extension);
		if (status != IOMGR_OK ||
		    block_device_flush(volume.device) != BLOCK_DEVICE_OK)
			return IOMGR_IO_ERROR;
		eoc = fat_table_eoc_value(&table);
		if (eoc == 0u)
			return IOMGR_CORRUPT;
		status = cache_fat_entry(&cache, &volume, extension, eoc);
		if (status == IOMGR_OK)
			status = cache_fat_entry(&cache, &volume,
						 scan.last_cluster, extension);
		if (status != IOMGR_OK)
			return status;
		status = cluster_sector_lba(&volume, extension, 0u,
					    &directory_lba);
		if (status != IOMGR_OK)
			return status;
		directory_offset = 0u;
	}
	status = cache_sector(&cache, &volume, directory_lba,
			      &directory_sector);
	if (status != IOMGR_OK)
		return status;
	encode_file_entry(directory_sector->bytes + directory_offset, name83,
			  attributes);
	status = commit_cache(volume_handle, &cache);
	if (status != IOMGR_OK)
		return status;
	status = fat_driver_adjust_free_clusters(
		volume_context, (int32_t)freed_clusters -
				(allocated_extension ? 1 : 0));
	if (status != IOMGR_OK)
		return IOMGR_UNCERTAIN;
	*created = (struct fat_created_file){
	    .directory_lba = directory_lba,
	    .first_cluster = 0u,
	    .size = 0u,
	    .attributes = attributes | FAT_ATTRIBUTE_ARCHIVE,
	    .directory_offset = directory_offset,
	};
	return IOMGR_OK;
}

enum iomgr_status fat_write_file_sector(
	kernel_object_handle_t volume_context, iomgr_volume_handle_t volume_handle,
	uint32_t first_cluster, uint32_t size, block_lba_t directory_lba,
	uint16_t directory_offset, uint32_t cursor_cluster,
	uint32_t cursor_cluster_index, bool cursor_valid, uint64_t offset,
	const uint8_t *source, size_t source_capacity, size_t count,
	struct fat_write_result *result)
{
	struct fat_driver_volume_snapshot volume;
	struct fat_mutation_cache cache = {0};
	union block_device_sector *data_sector;
	union block_device_sector *directory_sector;
	uint64_t cluster_bytes;
	uint64_t cluster_index;
	uint64_t in_cluster;
	uint64_t end;
	uint32_t current;
	uint64_t step = 0u;
	uint32_t previous = 0u;
	uint32_t allocated = 0u;
	uint32_t new_size;
	block_lba_t data_lba;
	size_t in_sector;
	size_t index;
	enum iomgr_status status;

	if (source == NULL || result == NULL || count == 0u ||
	    count > source_capacity || directory_offset >
		BLOCK_DEVICE_SECTOR_BYTES - FAT_DIRENT_BYTES || offset > size ||
	    check_add_overflow(offset, (uint64_t)count, &end) ||
	    end > 0xffffffffu)
		return IOMGR_INVALID_ARGUMENT;
	status = fat_driver_snapshot_from_context(volume_context, &volume);
	if (status != IOMGR_OK)
		return status;
	cluster_bytes = (uint64_t)volume.layout.sectors_per_cluster *
			BLOCK_DEVICE_SECTOR_BYTES;
	if (cluster_bytes == 0u ||
	    count > BLOCK_DEVICE_SECTOR_BYTES - (size_t)(offset & 511u))
		return IOMGR_INVALID_ARGUMENT;
	cluster_index = offset / cluster_bytes;
	in_cluster = offset % cluster_bytes;
	current = first_cluster;
	if (current == 0u) {
		if (size != 0u || cluster_index != 0u)
			return IOMGR_CORRUPT;
		status = find_free_clusters(volume_context, &volume, 1u,
					    &allocated);
		if (status != IOMGR_OK)
			return status;
		status = zero_cluster(&volume, allocated);
		if (status != IOMGR_OK ||
		    block_device_flush(volume.device) != BLOCK_DEVICE_OK)
			return IOMGR_IO_ERROR;
		current = allocated;
	} else {
		if (cursor_valid && cursor_cluster_index <= cluster_index) {
			current = cursor_cluster;
			step = cursor_cluster_index;
		}
		if (!cluster_valid(&volume, current) || step > cluster_index)
			return IOMGR_CORRUPT;
		for (; step < cluster_index; ++step) {
			uint32_t next;
			bool at_end;

			status = next_cluster(&volume, current, &next, &at_end);
			if (status != IOMGR_OK)
				return status;
			if (!at_end) {
				current = next;
				continue;
			}
			if (step + 1u != cluster_index || offset != size)
				return IOMGR_CORRUPT;
			previous = current;
			status = find_free_clusters(volume_context, &volume, 1u,
						    &allocated);
			if (status != IOMGR_OK)
				return status;
			status = zero_cluster(&volume, allocated);
			if (status != IOMGR_OK ||
			    block_device_flush(volume.device) != BLOCK_DEVICE_OK)
				return IOMGR_IO_ERROR;
			current = allocated;
		}
	}
	status = cluster_sector_lba(&volume, current,
				    (uint32_t)(in_cluster / 512u), &data_lba);
	if (status != IOMGR_OK)
		return status;
	status = cache_sector(&cache, &volume, data_lba, &data_sector);
	if (status != IOMGR_OK)
		return status;
	in_sector = (size_t)(offset & 511u);
	for (index = 0u; index < count; ++index)
		data_sector->bytes[in_sector + index] = source[index];
	if (allocated != 0u) {
		struct fat_table_layout table = table_layout(&volume);
		uint32_t eoc = fat_table_eoc_value(&table);

		if (eoc == 0u)
			return IOMGR_CORRUPT;
		status = cache_fat_entry(&cache, &volume, allocated, eoc);
		if (status == IOMGR_OK && previous != 0u)
			status = cache_fat_entry(&cache, &volume, previous, allocated);
		if (status != IOMGR_OK)
			return status;
		if (first_cluster == 0u)
			first_cluster = allocated;
	}
	status = cache_sector(&cache, &volume, directory_lba,
			      &directory_sector);
	if (status != IOMGR_OK)
		return status;
	new_size = end > size ? (uint32_t)end : size;
	if (volume.layout.fat_bits == FAT_TABLE_32)
		write_le16(directory_sector->bytes + directory_offset + 20u,
			   (uint16_t)(first_cluster >> 16u));
	write_le16(directory_sector->bytes + directory_offset + 26u,
		   (uint16_t)first_cluster);
	write_le32(directory_sector->bytes + directory_offset + 28u, new_size);
	directory_sector->bytes[directory_offset + 11u] |= FAT_ATTRIBUTE_ARCHIVE;
	status = commit_cache(volume_handle, &cache);
	if (status != IOMGR_OK)
		return status;
	if (allocated != 0u &&
	    fat_driver_adjust_free_clusters(volume_context, -1) != IOMGR_OK)
		return IOMGR_UNCERTAIN;
	*result = (struct fat_write_result){
	    .first_cluster = first_cluster,
	    .size = new_size,
	    .cursor_cluster = current,
	    .cursor_cluster_index = (uint32_t)cluster_index,
	};
	return IOMGR_OK;
}

enum iomgr_status fat_set_file_modified(
	kernel_object_handle_t volume_context, iomgr_volume_handle_t volume_handle,
	block_lba_t directory_lba, uint16_t directory_offset,
	uint32_t expected_first_cluster, uint32_t expected_size,
	const struct iomgr_timestamp *modified)
{
	struct fat_driver_volume_snapshot volume;
	struct fat_mutation_cache cache = {0};
	union block_device_sector *directory_sector;
	uint8_t *raw;
	uint16_t date;
	uint16_t time;
	enum iomgr_status status;

	if (modified == NULL || modified->year < 1980u ||
	    modified->year > 2107u || directory_offset % FAT_DIRENT_BYTES != 0u ||
	    directory_offset > BLOCK_DEVICE_SECTOR_BYTES - FAT_DIRENT_BYTES)
		return IOMGR_INVALID_ARGUMENT;
	status = fat_driver_snapshot_from_context(volume_context, &volume);
	if (status != IOMGR_OK)
		return status;
	status = cache_sector(&cache, &volume, directory_lba,
			      &directory_sector);
	if (status != IOMGR_OK)
		return status;
	raw = directory_sector->bytes + directory_offset;
	if (raw[0] == 0u || raw[0] == 0xe5u || raw[11] == FAT_ATTRIBUTE_LFN ||
	    (raw[11] & FAT_ATTRIBUTE_DIRECTORY) != 0u ||
	    directory_entry_cluster(&volume, raw) != expected_first_cluster ||
	    ((uint32_t)raw[28] | ((uint32_t)raw[29] << 8u) |
	     ((uint32_t)raw[30] << 16u) | ((uint32_t)raw[31] << 24u)) !=
		expected_size)
		return IOMGR_STALE_HANDLE;
	date = (uint16_t)(((modified->year - 1980u) << 9u) |
			  ((uint16_t)modified->month << 5u) |
			  modified->day);
	time = (uint16_t)(((uint16_t)modified->hour << 11u) |
			  ((uint16_t)modified->minute << 5u) |
			  (modified->second >> 1u));
	write_le16(raw + 22u, time);
	write_le16(raw + 24u, date);
	return commit_cache(volume_handle, &cache);
}

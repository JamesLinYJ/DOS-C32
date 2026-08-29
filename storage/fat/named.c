// SPDX-License-Identifier: GPL-2.0-only
/*
 * FAT12/16/32 named-I/O implementation
 *
 * Compatibility contract: 8.3 lookup, directory attributes, FAT chain failures
 * Safety changes: counted UTF-8 input, typed sectors, bounded walks,
 *                 generation-owned file/search objects
 */
#include "internal.h"

#include "fat_table.h"
#include "overflow.h"

#define FAT_DIRENT_BYTES 32u
#define FAT_ENTRIES_PER_SECTOR (BLOCK_DEVICE_SECTOR_BYTES / FAT_DIRENT_BYTES)
#define FAT_ATTRIBUTE_LFN 0x0fu
#define FAT_NAMED_FILE_SLOTS 32u
#define FAT_NAMED_SEARCH_SLOTS 16u

struct fat_disk_entry {
	uint8_t name83[11];
	uint8_t attributes;
	uint16_t modified_time;
	uint16_t modified_date;
	uint32_t first_cluster;
	uint32_t size;
	block_lba_t directory_lba;
	uint16_t directory_offset;
};

struct fat_directory_iterator {
	struct fat_driver_volume_snapshot volume;
	union block_device_sector sector;
	uint32_t current_cluster;
	uint32_t cluster_steps;
	uint32_t sector_index;
	uint8_t entry_index;
	bool fixed_root;
	bool sector_loaded;
	bool ended;
	block_lba_t sector_lba;
	block_lba_t last_lba;
	uint16_t last_offset;
};

enum fat_named_slot_state {
	FAT_NAMED_SLOT_FREE = 0,
	FAT_NAMED_SLOT_LIVE,
	FAT_NAMED_SLOT_RETIRED
};

struct fat_file_slot {
	enum fat_named_slot_state state;
	uint32_t generation;
	kernel_object_handle_t volume_context;
	uint32_t first_cluster;
	uint32_t size;
	block_lba_t directory_lba;
	uint16_t directory_offset;
	uint16_t modified_time;
	uint16_t modified_date;
	uint8_t attributes;
	bool writable;
	bool cursor_valid;
	uint32_t cursor_cluster;
	uint32_t cursor_cluster_index;
};

struct fat_search_slot {
	enum fat_named_slot_state state;
	uint32_t generation;
	kernel_object_handle_t volume_context;
	uint8_t pattern[11];
	uint32_t attributes;
	struct fat_directory_iterator iterator;
};

static struct fat_file_slot file_slots[FAT_NAMED_FILE_SLOTS];
static struct fat_search_slot search_slots[FAT_NAMED_SEARCH_SLOTS];

static uint16_t read_le16(const uint8_t *bytes)
{
	return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static uint32_t read_le32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
	       ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
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

static bool cluster_is_valid(const struct fat_driver_volume_snapshot *volume,
			     uint32_t cluster)
{
	return cluster >= 2u && cluster < volume->layout.cluster_limit;
}

static enum iomgr_status fat_entry_read(
	const struct fat_driver_volume_snapshot *volume, uint32_t cluster,
	uint32_t *value, enum fat_table_entry_kind *kind)
{
	struct fat_table_layout table = table_layout(volume);
	struct fat_table_position position;
	union block_device_sector first;
	union block_device_sector second;
	union block_device_sector mirror_first;
	union block_device_sector mirror_second;
	block_lba_t lba;
	block_lba_t second_lba;
	uint32_t mirror_value;
	enum fat_table_entry_kind mirror_kind;
	uint8_t copy;
	enum iomgr_status status;

	if (value == NULL || kind == NULL || !cluster_is_valid(volume, cluster) ||
	    fat_table_locate(&table, cluster, &position) != FAT_TABLE_OK)
		return IOMGR_CORRUPT;
	if (fat_table_copy_lba(&table, 0u, position.sector_index, &lba) !=
		    FAT_TABLE_OK)
		return IOMGR_CORRUPT;
	status = read_relative(volume, lba, &first);
	if (status != IOMGR_OK)
		return status;
	if (position.sector_count == 2u) {
		if (fat_table_copy_lba(&table, 0u,
				       position.sector_index + 1u,
				       &second_lba) != FAT_TABLE_OK)
			return IOMGR_CORRUPT;
		status = read_relative(volume, second_lba, &second);
		if (status != IOMGR_OK)
			return status;
	}
	if (fat_table_read(&table, cluster, &first,
			   position.sector_count == 2u ? &second : NULL,
			   value, kind) != FAT_TABLE_OK)
		return IOMGR_CORRUPT;
	for (copy = 1u; copy < table.fat_count; ++copy) {
		if (fat_table_copy_lba(&table, copy, position.sector_index, &lba) !=
			    FAT_TABLE_OK)
			return IOMGR_CORRUPT;
		status = read_relative(volume, lba, &mirror_first);
		if (status != IOMGR_OK)
			return status;
		if (position.sector_count == 2u) {
			if (fat_table_copy_lba(&table, copy,
					       position.sector_index + 1u,
					       &second_lba) != FAT_TABLE_OK)
				return IOMGR_CORRUPT;
			status = read_relative(volume, second_lba,
					       &mirror_second);
			if (status != IOMGR_OK)
				return status;
		}
		if (fat_table_read(&table, cluster, &mirror_first,
				   position.sector_count == 2u
					   ? &mirror_second
					   : NULL,
				   &mirror_value, &mirror_kind) != FAT_TABLE_OK ||
		    mirror_value != *value || mirror_kind != *kind)
			return IOMGR_CORRUPT;
	}
	return IOMGR_OK;
}

static bool sectors_equal(const union block_device_sector *left,
			  const union block_device_sector *right)
{
	size_t index;

	for (index = 0u; index < ARRAY_SIZE(left->bytes); ++index)
		if (left->bytes[index] != right->bytes[index])
			return false;
	return true;
}

static enum iomgr_status load_validated_fat_sector(
	const struct fat_driver_volume_snapshot *volume,
	const struct fat_table_layout *table, uint32_t sector_index,
	union block_device_sector *sector)
{
	union block_device_sector mirror;
	block_lba_t lba;
	uint8_t copy;
	enum iomgr_status status;

	if (fat_table_copy_lba(table, 0u, sector_index, &lba) != FAT_TABLE_OK)
		return IOMGR_CORRUPT;
	status = read_relative(volume, lba, sector);
	if (status != IOMGR_OK)
		return status;
	for (copy = 1u; copy < table->fat_count; ++copy) {
		if (fat_table_copy_lba(table, copy, sector_index, &lba) !=
			    FAT_TABLE_OK)
			return IOMGR_CORRUPT;
		status = read_relative(volume, lba, &mirror);
		if (status != IOMGR_OK)
			return status;
		if (!sectors_equal(sector, &mirror))
			return IOMGR_CORRUPT;
	}
	return IOMGR_OK;
}

static enum iomgr_status count_free_clusters(
	const struct fat_driver_volume_snapshot *volume, uint32_t *free_clusters)
{
	struct fat_table_layout table = table_layout(volume);
	struct fat_table_position position;
	union block_device_sector first;
	union block_device_sector second;
	uint32_t cached_index = 0xffffffffu;
	uint32_t cluster;
	uint32_t count = 0u;
	bool second_valid = false;
	enum iomgr_status status;

	if (free_clusters == NULL)
		return IOMGR_INVALID_ARGUMENT;
	for (cluster = 2u; cluster < volume->layout.cluster_limit; ++cluster) {
		uint32_t value;
		enum fat_table_entry_kind kind;

		if (fat_table_locate(&table, cluster, &position) != FAT_TABLE_OK)
			return IOMGR_CORRUPT;
		if (position.sector_index != cached_index) {
			if (second_valid &&
			    position.sector_index == cached_index + 1u) {
				first = second;
				cached_index = position.sector_index;
				second_valid = false;
			} else {
				status = load_validated_fat_sector(
					volume, &table, position.sector_index, &first);
				if (status != IOMGR_OK)
					return status;
				cached_index = position.sector_index;
				second_valid = false;
			}
		}
		if (position.sector_count == 2u && !second_valid) {
			status = load_validated_fat_sector(
				volume, &table, cached_index + 1u, &second);
			if (status != IOMGR_OK)
				return status;
			second_valid = true;
		}
		if (fat_table_read(&table, cluster, &first,
				   position.sector_count == 2u ? &second : NULL,
				   &value, &kind) != FAT_TABLE_OK)
			return IOMGR_CORRUPT;
		(void)value;
		if (kind == FAT_TABLE_ENTRY_FREE)
			++count;
	}
	*free_clusters = count;
	return IOMGR_OK;
}

static enum iomgr_status next_cluster(
	const struct fat_driver_volume_snapshot *volume, uint32_t cluster,
	uint32_t *next, bool *at_end)
{
	uint32_t value;
	enum fat_table_entry_kind kind;
	enum iomgr_status status;

	status = fat_entry_read(volume, cluster, &value, &kind);
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

static enum iomgr_status cluster_sector_lba(
	const struct fat_driver_volume_snapshot *volume, uint32_t cluster,
	uint32_t sector_index, block_lba_t *lba)
{
	block_lba_t cluster_offset;
	block_lba_t candidate;

	if (!cluster_is_valid(volume, cluster) ||
	    sector_index >= volume->layout.sectors_per_cluster || lba == NULL)
		return IOMGR_CORRUPT;
	if (check_mul_overflow((block_lba_t)(cluster - 2u),
			       (block_lba_t)volume->layout.sectors_per_cluster,
			       &cluster_offset) ||
	    check_add_overflow((block_lba_t)volume->layout.data_start,
			       cluster_offset, &candidate) ||
	    check_add_overflow(candidate, (block_lba_t)sector_index,
			       &candidate) ||
	    candidate >= volume->layout.total_sectors)
		return IOMGR_CORRUPT;
	*lba = candidate;
	return IOMGR_OK;
}

static enum iomgr_status iterator_initialize(
	struct fat_directory_iterator *iterator,
	const struct fat_driver_volume_snapshot *volume, uint32_t directory)
{
	if (iterator == NULL || volume == NULL)
		return IOMGR_INVALID_ARGUMENT;
	*iterator = (struct fat_directory_iterator){ 0 };
	iterator->volume = *volume;
	if (directory == 0u && volume->layout.fat_bits != FAT_TABLE_32) {
		iterator->fixed_root = true;
		return IOMGR_OK;
	}
	iterator->current_cluster =
		directory == 0u ? volume->layout.root_cluster : directory;
	if (!cluster_is_valid(volume, iterator->current_cluster))
		return IOMGR_CORRUPT;
	return IOMGR_OK;
}

static enum iomgr_status iterator_advance_sector(
	struct fat_directory_iterator *iterator)
{
	uint32_t following;
	bool at_end;
	enum iomgr_status status;

	iterator->sector_loaded = false;
	iterator->entry_index = 0u;
	++iterator->sector_index;
	if (iterator->fixed_root) {
		if (iterator->sector_index >= iterator->volume.layout.root_sectors)
			iterator->ended = true;
		return IOMGR_OK;
	}
	if (iterator->sector_index <
	    iterator->volume.layout.sectors_per_cluster)
		return IOMGR_OK;
	iterator->sector_index = 0u;
	if (++iterator->cluster_steps > iterator->volume.layout.data_clusters)
		return IOMGR_CORRUPT;
	status = next_cluster(&iterator->volume, iterator->current_cluster,
			      &following, &at_end);
	if (status != IOMGR_OK)
		return status;
	if (at_end) {
		iterator->ended = true;
		return IOMGR_OK;
	}
	iterator->current_cluster = following;
	return IOMGR_OK;
}

static enum iomgr_status iterator_next_raw(
	struct fat_directory_iterator *iterator, uint8_t raw[FAT_DIRENT_BYTES])
{
	block_lba_t lba;
	size_t offset;
	size_t index;
	enum iomgr_status status;

	if (iterator == NULL || raw == NULL || iterator->ended)
		return IOMGR_END_OF_SEARCH;
	if (!iterator->sector_loaded) {
		if (iterator->fixed_root) {
			if (check_add_overflow(
				    (block_lba_t)iterator->volume.layout.root_start,
				    (block_lba_t)iterator->sector_index, &lba))
				return IOMGR_CORRUPT;
		} else {
			status = cluster_sector_lba(&iterator->volume,
						    iterator->current_cluster,
						    iterator->sector_index, &lba);
			if (status != IOMGR_OK)
				return status;
		}
		status = read_relative(&iterator->volume, lba,
				       &iterator->sector);
		if (status != IOMGR_OK)
			return status;
		iterator->sector_loaded = true;
		iterator->sector_lba = lba;
	}
	offset = (size_t)iterator->entry_index * FAT_DIRENT_BYTES;
	iterator->last_lba = iterator->sector_lba;
	iterator->last_offset = (uint16_t)offset;
	for (index = 0u; index < FAT_DIRENT_BYTES; ++index)
		raw[index] = iterator->sector.bytes[offset + index];
	++iterator->entry_index;
	if (iterator->entry_index == FAT_ENTRIES_PER_SECTOR) {
		status = iterator_advance_sector(iterator);
		if (status != IOMGR_OK)
			return status;
	}
	if (raw[0] == 0u) {
		iterator->ended = true;
		return IOMGR_END_OF_SEARCH;
	}
	return IOMGR_OK;
}

static void parse_entry(const uint8_t raw[FAT_DIRENT_BYTES], bool fat32,
			struct fat_disk_entry *entry)
{
	size_t index;

	for (index = 0u; index < 11u; ++index)
		entry->name83[index] = raw[index];
	entry->attributes = raw[11];
	entry->modified_time = read_le16(raw + 22u);
	entry->modified_date = read_le16(raw + 24u);
	entry->first_cluster = (uint32_t)read_le16(raw + 26u);
	if (fat32)
		entry->first_cluster |= (uint32_t)read_le16(raw + 20u) << 16;
	entry->size = read_le32(raw + 28u);
}

static bool short_character_is_legal(uint8_t character)
{
	static const char forbidden[] = "\"*+,/:;<=>?[\\]|";
	size_t index;

	if (character < 0x21u || character >= 0x7fu)
		return false;
	for (index = 0u; forbidden[index] != '\0'; ++index) {
		if (character == (uint8_t)forbidden[index])
			return false;
	}
	return true;
}

static uint8_t upper_ascii(uint8_t character)
{
	return character >= 'a' && character <= 'z'
		       ? (uint8_t)(character - ('a' - 'A'))
		       : character;
}

enum iomgr_status fat_short_name_encode(const uint8_t *component,
					size_t length, uint8_t output[11])
{
	size_t base = 0u;
	size_t extension = 0u;
	size_t index;
	bool in_extension = false;

	if (component == NULL || length == 0u)
		return IOMGR_INVALID_NAME;
	for (index = 0u; index < 11u; ++index)
		output[index] = ' ';
	for (index = 0u; index < length; ++index) {
		uint8_t character = component[index];

		if (character == '.') {
			if (in_extension || base == 0u)
				return IOMGR_INVALID_NAME;
			in_extension = true;
			continue;
		}
		if (!short_character_is_legal(character))
			return IOMGR_INVALID_NAME;
		if (!in_extension) {
			if (base == 8u)
				return IOMGR_INVALID_NAME;
			output[base++] = upper_ascii(character);
		} else {
			if (extension == 3u)
				return IOMGR_INVALID_NAME;
			output[8u + extension++] = upper_ascii(character);
		}
	}
	if (base == 0u || (in_extension && extension == 0u))
		return IOMGR_INVALID_NAME;
	return IOMGR_OK;
}

static bool names_equal(const uint8_t left[11], const uint8_t right[11])
{
	size_t index;

	for (index = 0u; index < 11u; ++index) {
		uint8_t right_value = right[index];

		if (index == 0u && right_value == 0x05u)
			right_value = 0xe5u;
		if (left[index] != right_value)
			return false;
	}
	return true;
}

static enum iomgr_status find_in_directory(
	const struct fat_driver_volume_snapshot *volume, uint32_t directory,
	const uint8_t name83[11], struct fat_disk_entry *entry)
{
	struct fat_directory_iterator iterator;
	uint8_t raw[FAT_DIRENT_BYTES];
	enum iomgr_status status;

	status = iterator_initialize(&iterator, volume, directory);
	if (status != IOMGR_OK)
		return status;
	for (;;) {
		status = iterator_next_raw(&iterator, raw);
		if (status == IOMGR_END_OF_SEARCH)
			return IOMGR_NOT_FOUND;
		if (status != IOMGR_OK)
			return status;
		if (raw[0] == 0xe5u || raw[11] == FAT_ATTRIBUTE_LFN ||
		    (raw[11] & IOMGR_NODE_VOLUME_LABEL) != 0u ||
		    !names_equal(name83, raw))
			continue;
		parse_entry(raw, volume->layout.fat_bits == FAT_TABLE_32, entry);
		entry->directory_lba = iterator.last_lba;
		entry->directory_offset = iterator.last_offset;
		return IOMGR_OK;
	}
}

static bool path_separator(uint8_t character)
{
	return character == '/' || character == '\\';
}

static enum iomgr_status resolve_path(
	const struct fat_driver_volume_snapshot *volume,
	const struct iomgr_path *path, struct fat_disk_entry *entry)
{
	uint32_t directory = 0u;
	struct fat_disk_entry current = { 0 };
	size_t cursor = 0u;
	bool saw_component = false;

	current.attributes = IOMGR_NODE_DIRECTORY;
	while (cursor < path->length && path_separator(path->bytes[cursor]))
		++cursor;
	while (cursor < path->length) {
		uint8_t name83[11];
		size_t start = cursor;
		size_t length;
		enum iomgr_status status;

		while (cursor < path->length &&
		       !path_separator(path->bytes[cursor]))
			++cursor;
		length = cursor - start;
		while (cursor < path->length &&
		       path_separator(path->bytes[cursor]))
			++cursor;
		if (length == 0u)
			continue;
		saw_component = true;
		if (length == 1u && path->bytes[start] == '.')
			continue;
		status = fat_short_name_encode(path->bytes + start, length, name83);
		if (status != IOMGR_OK)
			return status;
		status = find_in_directory(volume, directory, name83, &current);
		if (status != IOMGR_OK)
			return status;
		if (cursor < path->length) {
			if ((current.attributes & IOMGR_NODE_DIRECTORY) == 0u)
				return IOMGR_NOT_DIRECTORY;
			if (!cluster_is_valid(volume, current.first_cluster))
				return IOMGR_CORRUPT;
			directory = current.first_cluster;
		}
	}
	if (!saw_component)
		current.attributes = IOMGR_NODE_DIRECTORY;
	*entry = current;
	return IOMGR_OK;
}

static struct iomgr_timestamp decode_timestamp(uint16_t date, uint16_t time)
{
	return (struct iomgr_timestamp){
		.year = (uint16_t)(1980u + ((date >> 9) & 0x7fu)),
		.month = (uint8_t)((date >> 5) & 0x0fu),
		.day = (uint8_t)(date & 0x1fu),
		.hour = (uint8_t)((time >> 11) & 0x1fu),
		.minute = (uint8_t)((time >> 5) & 0x3fu),
		.second = (uint8_t)((time & 0x1fu) * 2u),
		.centiseconds = 0u,
	};
}

static struct iomgr_node_info node_info(const struct fat_disk_entry *entry)
{
	return (struct iomgr_node_info){
		.size = (entry->attributes & IOMGR_NODE_DIRECTORY) != 0u
				? 0u
				: entry->size,
		.attributes = entry->attributes & 0x3fu,
		.modified = decode_timestamp(entry->modified_date,
					     entry->modified_time),
	};
}

static size_t reserve_file_slot(void)
{
	size_t index;

	for (index = 0u; index < FAT_NAMED_FILE_SLOTS; ++index) {
		if (file_slots[index].state != FAT_NAMED_SLOT_FREE)
			continue;
		if (file_slots[index].generation == 0xffffffffu) {
			file_slots[index].state = FAT_NAMED_SLOT_RETIRED;
			continue;
		}
		++file_slots[index].generation;
		return index;
	}
	return FAT_NAMED_FILE_SLOTS;
}

static kernel_object_handle_t make_context(size_t index, uint32_t generation)
{
	return ((uint64_t)generation << 32) | (uint64_t)(index + 1u);
}

static enum iomgr_status resolve_file_context(
	kernel_object_handle_t context, struct fat_file_slot **slot)
{
	uint32_t encoded = (uint32_t)context;
	uint32_t generation = (uint32_t)(context >> 32);

	if (encoded == 0u || encoded > FAT_NAMED_FILE_SLOTS || generation == 0u)
		return IOMGR_STALE_HANDLE;
	*slot = &file_slots[encoded - 1u];
	if ((*slot)->state != FAT_NAMED_SLOT_LIVE ||
	    (*slot)->generation != generation)
		return IOMGR_STALE_HANDLE;
	return IOMGR_OK;
}

static enum iomgr_status fat_named_stat(
	kernel_object_handle_t volume_context, const struct iomgr_path *path,
	struct iomgr_node_info *info)
{
	struct fat_driver_volume_snapshot volume;
	struct fat_disk_entry entry;
	enum iomgr_status status;

	status = fat_driver_snapshot_from_context(volume_context, &volume);
	if (status != IOMGR_OK)
		return status;
	status = resolve_path(&volume, path, &entry);
	if (status != IOMGR_OK)
		return status;
	*info = node_info(&entry);
	return IOMGR_OK;
}

static enum iomgr_status fat_named_open_file(
	kernel_object_handle_t volume_context, const struct iomgr_path *path,
	kernel_object_handle_t *file_context, struct iomgr_node_info *info)
{
	struct fat_driver_volume_snapshot volume;
	struct fat_disk_entry entry;
	size_t index;
	enum iomgr_status status;

	status = fat_driver_snapshot_from_context(volume_context, &volume);
	if (status != IOMGR_OK)
		return status;
	status = resolve_path(&volume, path, &entry);
	if (status != IOMGR_OK)
		return status;
	if ((entry.attributes & IOMGR_NODE_DIRECTORY) != 0u)
		return IOMGR_IS_DIRECTORY;
	if (entry.size != 0u && !cluster_is_valid(&volume, entry.first_cluster))
		return IOMGR_CORRUPT;
	index = reserve_file_slot();
	if (index == FAT_NAMED_FILE_SLOTS)
		return IOMGR_NO_SLOT;
	file_slots[index].volume_context = volume_context;
	file_slots[index].first_cluster = entry.first_cluster;
	file_slots[index].size = entry.size;
	file_slots[index].directory_lba = entry.directory_lba;
	file_slots[index].directory_offset = entry.directory_offset;
	file_slots[index].modified_time = entry.modified_time;
	file_slots[index].modified_date = entry.modified_date;
	file_slots[index].attributes = entry.attributes;
	file_slots[index].writable = false;
	file_slots[index].cursor_valid = entry.first_cluster != 0u;
	file_slots[index].cursor_cluster = entry.first_cluster;
	file_slots[index].cursor_cluster_index = 0u;
	file_slots[index].state = FAT_NAMED_SLOT_LIVE;
	*file_context = make_context(index, file_slots[index].generation);
	*info = node_info(&entry);
	return IOMGR_OK;
}

static enum iomgr_status fat_named_create_file(
	kernel_object_handle_t volume_context, const struct iomgr_path *path,
	uint32_t attributes, iomgr_volume_handle_t volume_handle,
	kernel_object_handle_t *file_context, struct iomgr_node_info *info)
{
	struct fat_created_file created;
	size_t index;
	enum iomgr_status status;

	if (file_context == NULL || info == NULL)
		return IOMGR_INVALID_ARGUMENT;
	index = reserve_file_slot();
	if (index == FAT_NAMED_FILE_SLOTS)
		return IOMGR_NO_SLOT;
	status = fat_create_file(volume_context, path, attributes, volume_handle,
				 &created);
	if (status != IOMGR_OK)
		return status;
	file_slots[index].volume_context = volume_context;
	file_slots[index].first_cluster = created.first_cluster;
	file_slots[index].size = created.size;
	file_slots[index].directory_lba = created.directory_lba;
	file_slots[index].directory_offset = created.directory_offset;
	file_slots[index].modified_time = 0u;
	file_slots[index].modified_date = 0u;
	file_slots[index].attributes = (uint8_t)created.attributes;
	file_slots[index].writable = true;
	file_slots[index].cursor_valid = false;
	file_slots[index].cursor_cluster = 0u;
	file_slots[index].cursor_cluster_index = 0u;
	file_slots[index].state = FAT_NAMED_SLOT_LIVE;
	*file_context = make_context(index, file_slots[index].generation);
	*info = (struct iomgr_node_info){
	    .size = created.size,
	    .attributes = created.attributes,
	};
	return IOMGR_OK;
}

static enum iomgr_status fat_named_read_file(
	kernel_object_handle_t volume_context,
	kernel_object_handle_t file_context, uint64_t offset,
	uint8_t *destination, size_t capacity, size_t count, size_t *bytes_read)
{
	struct fat_driver_volume_snapshot volume;
	struct fat_file_slot *slot;
	union block_device_sector sector;
	uint64_t cluster_bytes;
	uint64_t cluster_mask;
	uint8_t cluster_shift = 9u;
	uint8_t sectors_per_cluster;
	uint64_t cluster_index;
	uint32_t cluster;
	uint64_t cursor_index;
	uint64_t step;
	size_t completed = 0u;
	enum iomgr_status status;

	status = resolve_file_context(file_context, &slot);
	if (status != IOMGR_OK || slot->volume_context != volume_context)
		return IOMGR_STALE_HANDLE;
	if (destination == NULL || bytes_read == NULL || count > capacity ||
	    offset > slot->size)
		return IOMGR_INVALID_ARGUMENT;
	if ((uint64_t)count > (uint64_t)slot->size - offset)
		count = (size_t)((uint64_t)slot->size - offset);
	if (count == 0u) {
		*bytes_read = 0u;
		return IOMGR_OK;
	}
	status = fat_driver_snapshot_from_context(volume_context, &volume);
	if (status != IOMGR_OK)
		return status;
	sectors_per_cluster = volume.layout.sectors_per_cluster;
	while (sectors_per_cluster > 1u) {
		sectors_per_cluster >>= 1;
		++cluster_shift;
	}
	cluster_bytes = (uint64_t)1u << cluster_shift;
	cluster_mask = cluster_bytes - 1u;
	cluster_index = offset >> cluster_shift;
	if (cluster_index >= volume.layout.data_clusters)
		return IOMGR_CORRUPT;
	if (slot->cursor_valid && slot->cursor_cluster_index <= cluster_index) {
		cluster = slot->cursor_cluster;
		cursor_index = slot->cursor_cluster_index;
	} else {
		cluster = slot->first_cluster;
		cursor_index = 0u;
	}
	if (!cluster_is_valid(&volume, cluster))
		return IOMGR_CORRUPT;
	for (step = cursor_index; step < cluster_index; ++step) {
		uint32_t following;
		bool at_end;

		status = next_cluster(&volume, cluster, &following, &at_end);
		if (status != IOMGR_OK || at_end)
			return IOMGR_CORRUPT;
		cluster = following;
	}
	cursor_index = cluster_index;
	while (completed < count) {
		uint64_t in_cluster = offset & cluster_mask;
		uint32_t sector_index = (uint32_t)(in_cluster >> 9);
		size_t in_sector = (size_t)(in_cluster & 511u);
		size_t available = BLOCK_DEVICE_SECTOR_BYTES - in_sector;
		size_t remaining = count - completed;
		block_lba_t lba;
		size_t copy_count;
		size_t index;

		status = cluster_sector_lba(&volume, cluster, sector_index, &lba);
		if (status != IOMGR_OK)
			return status;
		status = read_relative(&volume, lba, &sector);
		if (status != IOMGR_OK)
			return status;
		copy_count = remaining < available ? remaining : available;
		for (index = 0u; index < copy_count; ++index)
			destination[completed + index] =
				sector.bytes[in_sector + index];
		completed += copy_count;
		offset += copy_count;
		if (completed < count && (offset & cluster_mask) == 0u) {
			uint32_t following;
			bool at_end;

			status = next_cluster(&volume, cluster, &following, &at_end);
			if (status != IOMGR_OK || at_end)
				return IOMGR_CORRUPT;
			cluster = following;
			++cursor_index;
		}
	}
	slot->cursor_valid = true;
	slot->cursor_cluster = cluster;
	slot->cursor_cluster_index = (uint32_t)cursor_index;
	*bytes_read = completed;
	return IOMGR_OK;
}

static enum iomgr_status fat_named_write_file(
	kernel_object_handle_t volume_context,
	kernel_object_handle_t file_context, iomgr_volume_handle_t volume_handle,
	uint64_t offset, const uint8_t *source, size_t source_capacity,
	size_t count, size_t *bytes_written)
{
	struct fat_file_slot *slot;
	size_t completed = 0u;
	enum iomgr_status status;

	if (source == NULL || bytes_written == NULL || count > source_capacity)
		return IOMGR_INVALID_ARGUMENT;
	status = resolve_file_context(file_context, &slot);
	if (status != IOMGR_OK || slot->volume_context != volume_context)
		return IOMGR_STALE_HANDLE;
	if (!slot->writable)
		return IOMGR_READ_ONLY;
	if (offset > slot->size)
		return IOMGR_INVALID_ARGUMENT;
	while (completed < count) {
		struct fat_write_result result;
		size_t amount = BLOCK_DEVICE_SECTOR_BYTES -
				(size_t)((offset + completed) & 511u);

		if (amount > count - completed)
			amount = count - completed;
		status = fat_write_file_sector(
			volume_context, volume_handle, slot->first_cluster,
			slot->size, slot->directory_lba, slot->directory_offset,
			slot->cursor_cluster, slot->cursor_cluster_index,
			slot->cursor_valid, offset + completed, source + completed,
			source_capacity - completed, amount, &result);
		if (status != IOMGR_OK)
			return status;
		slot->first_cluster = result.first_cluster;
		slot->size = result.size;
		slot->cursor_valid = true;
		slot->cursor_cluster = result.cursor_cluster;
		slot->cursor_cluster_index = result.cursor_cluster_index;
		completed += amount;
	}
	*bytes_written = completed;
	return IOMGR_OK;
}

static enum iomgr_status fat_named_get_file_info(
	kernel_object_handle_t volume_context,
	kernel_object_handle_t file_context, struct iomgr_node_info *info)
{
	struct fat_file_slot *slot;
	enum iomgr_status status;

	if (info == NULL)
		return IOMGR_INVALID_ARGUMENT;
	status = resolve_file_context(file_context, &slot);
	if (status != IOMGR_OK || slot->volume_context != volume_context)
		return IOMGR_STALE_HANDLE;
	*info = (struct iomgr_node_info){
	    .size = slot->size,
	    .attributes = slot->attributes,
	    .modified = decode_timestamp(slot->modified_date,
					 slot->modified_time),
	};
	return IOMGR_OK;
}

static enum iomgr_status fat_named_set_file_info(
	kernel_object_handle_t volume_context,
	kernel_object_handle_t file_context, iomgr_volume_handle_t volume,
	const struct iomgr_file_update *update)
{
	struct fat_file_slot *slot;
	enum iomgr_status status;

	if (update == NULL || update->valid != IOMGR_FILE_UPDATE_MODIFIED)
		return IOMGR_INVALID_ARGUMENT;
	status = resolve_file_context(file_context, &slot);
	if (status != IOMGR_OK || slot->volume_context != volume_context)
		return IOMGR_STALE_HANDLE;
	if (!slot->writable)
		return IOMGR_READ_ONLY;
	status = fat_set_file_modified(
		volume_context, volume, slot->directory_lba,
		slot->directory_offset, slot->first_cluster, slot->size,
		&update->modified);
	if (status != IOMGR_OK)
		return status;
	slot->modified_date =
		(uint16_t)(((update->modified.year - 1980u) << 9u) |
			   ((uint16_t)update->modified.month << 5u) |
			   update->modified.day);
	slot->modified_time =
		(uint16_t)(((uint16_t)update->modified.hour << 11u) |
			   ((uint16_t)update->modified.minute << 5u) |
			   (update->modified.second >> 1u));
	return IOMGR_OK;
}

static enum iomgr_status fat_named_close_file(
	kernel_object_handle_t volume_context,
	kernel_object_handle_t file_context)
{
	struct fat_file_slot *slot;
	enum iomgr_status status = resolve_file_context(file_context, &slot);

	if (status != IOMGR_OK || slot->volume_context != volume_context)
		return IOMGR_STALE_HANDLE;
	slot->state = FAT_NAMED_SLOT_FREE;
	return IOMGR_OK;
}

static enum iomgr_status wildcard_to_name83(const uint8_t *pattern,
					     size_t length,
					     uint8_t output[11])
{
	size_t position = 0u;
	size_t limit = 8u;
	size_t index;
	bool extension = false;

	if (pattern == NULL || length == 0u)
		return IOMGR_INVALID_NAME;
	for (index = 0u; index < 11u; ++index)
		output[index] = ' ';
	for (index = 0u; index < length; ++index) {
		uint8_t character = pattern[index];

		if (character == '.') {
			if (extension)
				return IOMGR_INVALID_NAME;
			extension = true;
			position = 8u;
			limit = 11u;
			continue;
		}
		if (character == '*') {
			while (position < limit)
				output[position++] = '?';
			while (index + 1u < length && pattern[index + 1u] != '.')
				++index;
			continue;
		}
		if (position >= limit)
			return IOMGR_INVALID_NAME;
		if (character == '?')
			output[position++] = '?';
		else if (short_character_is_legal(character))
			output[position++] = upper_ascii(character);
		else
			return IOMGR_INVALID_NAME;
	}
	return IOMGR_OK;
}

static size_t reserve_search_slot(void)
{
	size_t index;

	for (index = 0u; index < FAT_NAMED_SEARCH_SLOTS; ++index) {
		if (search_slots[index].state != FAT_NAMED_SLOT_FREE)
			continue;
		if (search_slots[index].generation == 0xffffffffu) {
			search_slots[index].state = FAT_NAMED_SLOT_RETIRED;
			continue;
		}
		++search_slots[index].generation;
		return index;
	}
	return FAT_NAMED_SEARCH_SLOTS;
}

static enum iomgr_status resolve_search_context(
	kernel_object_handle_t context, struct fat_search_slot **slot)
{
	uint32_t encoded = (uint32_t)context;
	uint32_t generation = (uint32_t)(context >> 32);

	if (encoded == 0u || encoded > FAT_NAMED_SEARCH_SLOTS ||
	    generation == 0u)
		return IOMGR_STALE_HANDLE;
	*slot = &search_slots[encoded - 1u];
	if ((*slot)->state != FAT_NAMED_SLOT_LIVE ||
	    (*slot)->generation != generation)
		return IOMGR_STALE_HANDLE;
	return IOMGR_OK;
}

static enum iomgr_status search_parent_directory(
	const struct fat_driver_volume_snapshot *volume,
	const struct iomgr_path *pattern, uint32_t *directory,
	uint8_t name83[11])
{
	static const uint8_t root_byte = '/';
	struct iomgr_path parent;
	struct fat_disk_entry entry;
	size_t last_separator = (size_t)-1;
	size_t index;
	size_t pattern_start;
	enum iomgr_status status;

	for (index = 0u; index < pattern->length; ++index) {
		if (path_separator(pattern->bytes[index]))
			last_separator = index;
	}
	pattern_start = last_separator == (size_t)-1 ? 0u : last_separator + 1u;
	if (pattern_start == pattern->length)
		return IOMGR_INVALID_NAME;
	status = wildcard_to_name83(pattern->bytes + pattern_start,
				    pattern->length - pattern_start, name83);
	if (status != IOMGR_OK)
		return status;
	if (last_separator == (size_t)-1 || last_separator == 0u) {
		parent.bytes = &root_byte;
		parent.length = 1u;
	} else {
		parent.bytes = pattern->bytes;
		parent.length = last_separator;
	}
	status = resolve_path(volume, &parent, &entry);
	if (status != IOMGR_OK)
		return status;
	if ((entry.attributes & IOMGR_NODE_DIRECTORY) == 0u)
		return IOMGR_NOT_DIRECTORY;
	if (parent.length == 1u && parent.bytes[0] == '/') {
		*directory = 0u;
		return IOMGR_OK;
	}
	if (!cluster_is_valid(volume, entry.first_cluster))
		return IOMGR_CORRUPT;
	*directory = entry.first_cluster;
	return IOMGR_OK;
}

static enum iomgr_status fat_named_open_search(
	kernel_object_handle_t volume_context, const struct iomgr_path *pattern,
	uint32_t attributes, kernel_object_handle_t *search_context)
{
	struct fat_driver_volume_snapshot volume;
	struct fat_directory_iterator iterator;
	uint8_t name83[11];
	uint32_t directory;
	size_t index;
	enum iomgr_status status;

	status = fat_driver_snapshot_from_context(volume_context, &volume);
	if (status != IOMGR_OK)
		return status;
	status = search_parent_directory(&volume, pattern, &directory, name83);
	if (status != IOMGR_OK)
		return status;
	status = iterator_initialize(&iterator, &volume, directory);
	if (status != IOMGR_OK)
		return status;
	index = reserve_search_slot();
	if (index == FAT_NAMED_SEARCH_SLOTS)
		return IOMGR_NO_SLOT;
	search_slots[index].volume_context = volume_context;
	search_slots[index].attributes = attributes;
	search_slots[index].iterator = iterator;
	for (directory = 0u; directory < 11u; ++directory)
		search_slots[index].pattern[directory] = name83[directory];
	search_slots[index].state = FAT_NAMED_SLOT_LIVE;
	*search_context = make_context(index, search_slots[index].generation);
	return IOMGR_OK;
}

static bool wildcard_matches(const uint8_t pattern[11],
			     const uint8_t name83[11])
{
	size_t index;

	for (index = 0u; index < 11u; ++index) {
		uint8_t value = name83[index];

		if (index == 0u && value == 0x05u)
			value = 0xe5u;
		if (pattern[index] != '?' && pattern[index] != value)
			return false;
	}
	return true;
}

static bool attributes_match(uint32_t requested, uint8_t found)
{
	uint32_t special = requested &
		~(IOMGR_NODE_READ_ONLY | IOMGR_NODE_ARCHIVE);
	uint32_t filtered = IOMGR_NODE_HIDDEN | IOMGR_NODE_SYSTEM |
			    IOMGR_NODE_VOLUME_LABEL | IOMGR_NODE_DIRECTORY;

	if (special == IOMGR_NODE_VOLUME_LABEL)
		return (found & IOMGR_NODE_VOLUME_LABEL) != 0u;
	return ((uint32_t)found & filtered & ~special) == 0u;
}

static size_t short_name_to_bytes(const uint8_t name83[11],
				  uint8_t output[IOMGR_NAME_MAX_BYTES + 1u])
{
	size_t base = 8u;
	size_t extension = 3u;
	size_t length = 0u;
	size_t index;

	while (base != 0u && name83[base - 1u] == ' ')
		--base;
	while (extension != 0u && name83[8u + extension - 1u] == ' ')
		--extension;
	for (index = 0u; index < base; ++index)
		output[length++] = index == 0u && name83[index] == 0x05u
					   ? 0xe5u
					   : name83[index];
	if (extension != 0u) {
		output[length++] = '.';
		for (index = 0u; index < extension; ++index)
			output[length++] = name83[8u + index];
	}
	output[length] = 0u;
	return length;
}

static enum iomgr_status fat_named_search_next(
	kernel_object_handle_t volume_context,
	kernel_object_handle_t search_context,
	struct iomgr_directory_entry *entry)
{
	struct fat_search_slot *slot;
	struct fat_disk_entry disk_entry;
	uint8_t raw[FAT_DIRENT_BYTES];
	enum iomgr_status status;

	status = resolve_search_context(search_context, &slot);
	if (status != IOMGR_OK || slot->volume_context != volume_context)
		return IOMGR_STALE_HANDLE;
	for (;;) {
		status = iterator_next_raw(&slot->iterator, raw);
		if (status != IOMGR_OK)
			return status;
		if (raw[0] == 0xe5u || raw[11] == FAT_ATTRIBUTE_LFN ||
		    !wildcard_matches(slot->pattern, raw) ||
		    !attributes_match(slot->attributes, raw[11]))
			continue;
		parse_entry(raw, slot->iterator.volume.layout.fat_bits ==
					 FAT_TABLE_32,
			    &disk_entry);
		entry->info = node_info(&disk_entry);
		entry->name_length = (uint16_t)short_name_to_bytes(
			disk_entry.name83, entry->name);
		return IOMGR_OK;
	}
}

static enum iomgr_status fat_named_close_search(
	kernel_object_handle_t volume_context,
	kernel_object_handle_t search_context)
{
	struct fat_search_slot *slot;
	enum iomgr_status status =
		resolve_search_context(search_context, &slot);

	if (status != IOMGR_OK || slot->volume_context != volume_context)
		return IOMGR_STALE_HANDLE;
	slot->state = FAT_NAMED_SLOT_FREE;
	return IOMGR_OK;
}

static enum iomgr_status fat_named_query_space(
	kernel_object_handle_t volume_context, bool count_free,
	struct iomgr_space_info *info)
{
	struct fat_driver_volume_snapshot volume;
	uint64_t allocation_unit;
	uint64_t total_bytes;
	uint64_t free_clusters = 0u;
	uint32_t cached_free_clusters;
	bool free_known;
	enum iomgr_status status;

	status = fat_driver_snapshot_from_context(volume_context, &volume);
	if (status != IOMGR_OK)
		return status;
	allocation_unit =
		(uint64_t)volume.layout.sectors_per_cluster << 9;
	if (check_mul_overflow((uint64_t)volume.layout.data_clusters,
			       allocation_unit, &total_bytes) ||
	    allocation_unit > 0xffffffffu)
		return IOMGR_CORRUPT;
	status = fat_driver_get_free_clusters(volume_context, &free_known,
					      &cached_free_clusters);
	if (status != IOMGR_OK)
		return status;
	if (count_free && free_known) {
		free_clusters = cached_free_clusters;
	} else if (count_free) {
		status = count_free_clusters(&volume, &cached_free_clusters);
		if (status != IOMGR_OK)
			return status;
		free_clusters = cached_free_clusters;
		status = fat_driver_publish_free_clusters(
			volume_context, cached_free_clusters);
		if (status != IOMGR_OK)
			return status;
	}
	if (check_mul_overflow(free_clusters, allocation_unit,
			       &info->free_bytes))
		return IOMGR_CORRUPT;
	info->total_bytes = total_bytes;
	info->allocation_unit_bytes = (uint32_t)allocation_unit;
	info->reserved = 0u;
	return IOMGR_OK;
}

const struct iomgr_driver_named_ops fat_named_ops = {
	.stat = fat_named_stat,
	.open_file = fat_named_open_file,
	.create_file = fat_named_create_file,
	.read_file = fat_named_read_file,
	.write_file = fat_named_write_file,
	.get_file_info = fat_named_get_file_info,
	.set_file_info = fat_named_set_file_info,
	.close_file = fat_named_close_file,
	.open_search = fat_named_open_search,
	.search_next = fat_named_search_next,
	.close_search = fat_named_close_search,
	.query_space = fat_named_query_space,
	.create_directory = fat_create_directory,
	.rename = fat_rename,
};

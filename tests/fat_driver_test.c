// SPDX-License-Identifier: GPL-2.0-only
/* Freestanding I/O Manager FAT driver probe and mount tests. */
#include "fat_driver.h"
#include "test_entry.h"

#define TEST_DEVICE ((block_device_handle_t)0x464154u)
#define TEST_FIRST_LBA 100u

enum test_volume_kind {
	TEST_FAT12 = 0,
	TEST_FAT16,
	TEST_FAT32,
	TEST_CORRUPT_FAT32,
	TEST_NTFS
};

static enum test_volume_kind selected_volume;
static uint32_t sector_reads;

#define TEST_WRITTEN_SECTORS 32u
struct written_sector {
	bool valid;
	block_lba_t lba;
	union block_device_sector sector;
};

static struct written_sector written_sectors[TEST_WRITTEN_SECTORS];
static uint32_t sector_writes;
static uint32_t flushes;

static void reset_writes(void)
{
	size_t index;

	for (index = 0u; index < ARRAY_SIZE(written_sectors); ++index)
		written_sectors[index].valid = false;
	sector_writes = 0u;
	flushes = 0u;
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

static void fill_common(union block_device_sector *sector, uint8_t spc,
			uint16_t reserved, uint8_t fats,
			uint16_t root_entries)
{
	clear_sector(sector);
	sector->bytes[0] = 0xebu;
	sector->bytes[1] = 0x3cu;
	sector->bytes[2] = 0x90u;
	write_le16(sector->bytes + 11u, BLOCK_DEVICE_SECTOR_BYTES);
	sector->bytes[13] = spc;
	write_le16(sector->bytes + 14u, reserved);
	sector->bytes[16] = fats;
	write_le16(sector->bytes + 17u, root_entries);
	sector->bytes[21] = 0xf8u;
}

static void fill_selected_boot(union block_device_sector *sector)
{
	if (selected_volume == TEST_NTFS) {
		clear_sector(sector);
		sector->bytes[3] = 'N';
		sector->bytes[4] = 'T';
		sector->bytes[5] = 'F';
		sector->bytes[6] = 'S';
		sector->bytes[7] = ' ';
		sector->bytes[8] = ' ';
		sector->bytes[9] = ' ';
		sector->bytes[10] = ' ';
		return;
	}
	if (selected_volume == TEST_FAT12) {
		fill_common(sector, 1u, 1u, 2u, 224u);
		write_le16(sector->bytes + 19u, 2880u);
		write_le16(sector->bytes + 22u, 9u);
		return;
	}
	if (selected_volume == TEST_FAT16) {
		fill_common(sector, 1u, 1u, 2u, 512u);
		write_le16(sector->bytes + 19u, 32768u);
		write_le16(sector->bytes + 22u, 128u);
		return;
	}
	fill_common(sector, 1u, 32u, 2u, 0u);
	write_le32(sector->bytes + 32u, 70000u);
	write_le32(sector->bytes + 36u, 600u);
	write_le32(sector->bytes + 44u, 2u);
	write_le16(sector->bytes + 48u,
		   selected_volume == TEST_CORRUPT_FAT32 ? 32u : 1u);
	write_le16(sector->bytes + 50u, 6u);
}

static void fill_fat16_table(union block_device_sector *sector)
{
	clear_sector(sector);
	write_le16(sector->bytes, 0xfff8u);
	write_le16(sector->bytes + 2u, 0xffffu);
	write_le16(sector->bytes + 4u, 3u);
	write_le16(sector->bytes + 6u, 0xffffu);
}

static void fill_fat16_root(union block_device_sector *sector)
{
	static const uint8_t name[11] = {'F', 'I', 'L', 'E', ' ', ' ',
					 ' ', ' ', 'T', 'X', 'T'};
	size_t index;

	clear_sector(sector);
	for (index = 0u; index < sizeof(name); ++index)
		sector->bytes[index] = name[index];
	sector->bytes[11] = IOMGR_NODE_ARCHIVE;
	write_le16(sector->bytes + 22u, (uint16_t)(12u << 11));
	write_le16(sector->bytes + 24u,
		   (uint16_t)(((2026u - 1980u) << 9) | (8u << 5) | 28u));
	write_le16(sector->bytes + 26u, 2u);
	write_le32(sector->bytes + 28u, 516u);
}

enum block_device_status
block_device_get_geometry(block_device_handle_t handle,
			  struct block_device_geometry *geometry)
{
	if (handle != TEST_DEVICE || geometry == NULL)
		return BLOCK_DEVICE_STALE_HANDLE;
	geometry->sector_count = 100000u;
	geometry->logical_sector_bytes = BLOCK_DEVICE_SECTOR_BYTES;
	geometry->writable = 1u;
	geometry->reserved[0] = 0u;
	geometry->reserved[1] = 0u;
	geometry->reserved[2] = 0u;
	return BLOCK_DEVICE_OK;
}

enum block_device_status
block_device_read_sector(block_device_handle_t handle, block_lba_t lba,
			 union block_device_sector *sector)
{
	block_lba_t relative;
	size_t index;

	if (handle != TEST_DEVICE || lba < TEST_FIRST_LBA || sector == NULL)
		return BLOCK_DEVICE_OUT_OF_RANGE;
	++sector_reads;
	for (index = 0u; index < ARRAY_SIZE(written_sectors); ++index) {
		if (written_sectors[index].valid &&
		    written_sectors[index].lba == lba) {
			*sector = written_sectors[index].sector;
			return BLOCK_DEVICE_OK;
		}
	}
	relative = lba - TEST_FIRST_LBA;
	if (relative == 0u) {
		fill_selected_boot(sector);
		return BLOCK_DEVICE_OK;
	}
	if (selected_volume != TEST_FAT16)
		return BLOCK_DEVICE_OUT_OF_RANGE;
	if (relative == 1u || relative == 129u) {
		fill_fat16_table(sector);
		return BLOCK_DEVICE_OK;
	}
	if (relative == 257u) {
		fill_fat16_root(sector);
		return BLOCK_DEVICE_OK;
	}
	clear_sector(sector);
	if (relative == 289u || relative == 290u) {
		for (index = 0u; index < ARRAY_SIZE(sector->bytes); ++index)
			sector->bytes[index] = relative == 289u ? 'A' : 'B';
	}
	return BLOCK_DEVICE_OK;
}

enum block_device_status block_device_write_sector(
	block_device_handle_t handle, block_lba_t lba,
	const union block_device_sector *sector)
{
	size_t index;
	size_t free_index = ARRAY_SIZE(written_sectors);

	if (handle != TEST_DEVICE || lba < TEST_FIRST_LBA || sector == NULL)
		return BLOCK_DEVICE_OUT_OF_RANGE;
	for (index = 0u; index < ARRAY_SIZE(written_sectors); ++index) {
		if (written_sectors[index].valid &&
		    written_sectors[index].lba == lba) {
			written_sectors[index].sector = *sector;
			++sector_writes;
			return BLOCK_DEVICE_OK;
		}
		if (!written_sectors[index].valid &&
		    free_index == ARRAY_SIZE(written_sectors))
			free_index = index;
	}
	if (free_index == ARRAY_SIZE(written_sectors))
		return BLOCK_DEVICE_IO_ERROR;
	written_sectors[free_index].valid = true;
	written_sectors[free_index].lba = lba;
	written_sectors[free_index].sector = *sector;
	++sector_writes;
	return BLOCK_DEVICE_OK;
}

enum block_device_status block_device_flush(block_device_handle_t handle)
{
	if (handle != TEST_DEVICE)
		return BLOCK_DEVICE_STALE_HANDLE;
	++flushes;
	return BLOCK_DEVICE_OK;
}

static int named_io_check(void)
{
	static const uint8_t file_path_bytes[] = "/FILE.TXT";
	static const uint8_t search_path_bytes[] = "/*.TXT";
	static const uint8_t directory_path_bytes[] = "/NEWDIR";
	static const uint8_t created_path_bytes[] = "/NEWFILE.DAT";
	static const uint8_t renamed_path_bytes[] = "/RENAMED.DAT";
	const struct iomgr_path file_path = {
		.bytes = file_path_bytes,
		.length = sizeof(file_path_bytes) - 1u,
	};
	const struct iomgr_path search_path = {
		.bytes = search_path_bytes,
		.length = sizeof(search_path_bytes) - 1u,
	};
	const struct iomgr_path directory_path = {
		.bytes = directory_path_bytes,
		.length = sizeof(directory_path_bytes) - 1u,
	};
	const struct iomgr_path created_path = {
		.bytes = created_path_bytes,
		.length = sizeof(created_path_bytes) - 1u,
	};
	const struct iomgr_path renamed_path = {
		.bytes = renamed_path_bytes,
		.length = sizeof(renamed_path_bytes) - 1u,
	};
	struct iomgr_mount_request request = {
		.device = TEST_DEVICE,
		.first_lba = TEST_FIRST_LBA,
		.sector_count = 32768u,
		.flags = 0u,
		.reserved = 0u,
	};
	struct iomgr_node_info info;
	struct iomgr_directory_entry entry;
	struct iomgr_space_info space;
	const struct iomgr_file_update update = {
		.valid = IOMGR_FILE_UPDATE_MODIFIED,
		.modified = {
			.year = 2024u,
			.month = 2u,
			.day = 29u,
			.hour = 23u,
			.minute = 58u,
			.second = 56u,
			.centiseconds = 0u,
		},
	};
	iomgr_volume_handle_t volume;
	iomgr_file_handle_t file;
	iomgr_search_handle_t search;
	uint8_t bytes[6] = {0u};
	uint8_t write_bytes[5000];
	uint8_t verify_bytes[5000];
	size_t bytes_read;
	size_t bytes_written;
	size_t byte_index;
	size_t read_offset;
	uint32_t first_space_reads;
	uint32_t sequential_write_reads;
	uint64_t initial_free_bytes;

	selected_volume = TEST_FAT16;
	reset_writes();
	for (byte_index = 0u; byte_index < sizeof(write_bytes); ++byte_index)
		write_bytes[byte_index] = (uint8_t)(byte_index ^ 0x5au);
	if (iomgr_mount(&request, &volume) != IOMGR_OK ||
	    iomgr_stat(volume, &file_path, &info) != IOMGR_OK ||
	    info.size != 516u || info.modified.year != 2026u ||
	    iomgr_open_file(volume, &file_path, &info, &file) != IOMGR_OK ||
	    iomgr_read_file(file, 510u, bytes, sizeof(bytes), sizeof(bytes),
			    &bytes_read) != IOMGR_OK ||
	    bytes_read != sizeof(bytes) || bytes[0] != 'A' || bytes[1] != 'A' ||
	    bytes[2] != 'B' || bytes[5] != 'B' ||
	    iomgr_close_file(file) != IOMGR_OK ||
	    iomgr_open_search(volume, &search_path, 0u, &search) != IOMGR_OK ||
	    iomgr_search_next(search, &entry) != IOMGR_OK ||
	    entry.name_length != 8u || entry.name[4] != '.' ||
	    iomgr_search_next(search, &entry) != IOMGR_END_OF_SEARCH ||
	    iomgr_close_search(search) != IOMGR_OK ||
	    iomgr_query_space(volume, false, &space) != IOMGR_OK ||
	    space.allocation_unit_bytes != 512u)
		return 1;
	sector_reads = 0u;
	if (iomgr_query_space(volume, true, &space) != IOMGR_OK)
		return 1;
	first_space_reads = sector_reads;
	initial_free_bytes = space.free_bytes;
	/* This image uses 128 sectors per FAT copy. A sector-window scan reads
	 * each mirror once rather than rereading it for every cluster. */
	if (first_space_reads > 260u ||
	    iomgr_query_space(volume, true, &space) != IOMGR_OK ||
	    sector_reads != first_space_reads)
		return 1;
	if (iomgr_create_file(volume, &created_path, IOMGR_NODE_HIDDEN, &info,
			      &file) != IOMGR_OK ||
	    info.size != 0u || (info.attributes & IOMGR_NODE_HIDDEN) == 0u)
		return 1;
	sector_reads = 0u;
	if (iomgr_write_file(file, 0u, write_bytes, sizeof(write_bytes),
			     sizeof(write_bytes), &bytes_written) != IOMGR_OK ||
	    bytes_written != sizeof(write_bytes))
		return 1;
	sequential_write_reads = sector_reads;
	/* Allocation and chain cursors keep ten sequential one-cluster writes
	 * linear. Restarting both walks at cluster 2 would exceed this bound. */
	if (sequential_write_reads > 150u ||
	    iomgr_set_file_info(file, &update) != IOMGR_OK ||
	    iomgr_get_file_info(file, &info) != IOMGR_OK ||
	    info.modified.year != 2024u || info.modified.month != 2u ||
	    info.modified.day != 29u || info.modified.hour != 23u ||
	    info.modified.minute != 58u || info.modified.second != 56u ||
	    iomgr_close_file(file) != IOMGR_OK ||
	    iomgr_rename(volume, &created_path, &renamed_path) != IOMGR_OK ||
	    iomgr_stat(volume, &created_path, &info) != IOMGR_NOT_FOUND ||
	    iomgr_stat(volume, &renamed_path, &info) != IOMGR_OK ||
	    info.size != sizeof(write_bytes) || info.modified.year != 2024u ||
	    info.modified.month != 2u || info.modified.day != 29u ||
	    iomgr_open_file(volume, &renamed_path, &info, &file) != IOMGR_OK ||
	    iomgr_read_file(file, 0u, verify_bytes, sizeof(verify_bytes),
			    sizeof(verify_bytes), &bytes_read) != IOMGR_OK ||
	    bytes_read != sizeof(verify_bytes) ||
	    iomgr_close_file(file) != IOMGR_OK ||
	    iomgr_create_file(volume, &file_path, 0u, &info, &file) != IOMGR_OK ||
	    info.size != 0u || iomgr_close_file(file) != IOMGR_OK ||
	    iomgr_stat(volume, &file_path, &info) != IOMGR_OK || info.size != 0u ||
	    iomgr_create_directory(volume, &directory_path) != IOMGR_OK ||
	    sector_writes < 7u || flushes < 2u ||
	    iomgr_stat(volume, &directory_path, &info) != IOMGR_OK ||
	    (info.attributes & IOMGR_NODE_DIRECTORY) == 0u)
		return 1;
	for (byte_index = 0u; byte_index < sizeof(write_bytes); ++byte_index)
		if (verify_bytes[byte_index] != write_bytes[byte_index])
			return 2;
	if (iomgr_open_file(volume, &renamed_path, &info, &file) != IOMGR_OK)
		return 3;
	sector_reads = 0u;
	for (read_offset = 0u; read_offset < sizeof(verify_bytes);
	     read_offset += bytes_read) {
		size_t amount = sizeof(verify_bytes) - read_offset;

		if (amount > BLOCK_DEVICE_SECTOR_BYTES)
			amount = BLOCK_DEVICE_SECTOR_BYTES;
		if (iomgr_read_file(file, read_offset,
				    verify_bytes + read_offset,
				    sizeof(verify_bytes) - read_offset, amount,
				    &bytes_read) != IOMGR_OK || bytes_read != amount)
			return 4;
	}
	/* Ten one-sector clusters require ten data reads and nine mirrored FAT
	 * transitions. Rewalking from the first cluster for every call would
	 * exceed this bound by more than 3x. */
	if (sector_reads > 30u || iomgr_close_file(file) != IOMGR_OK)
		return 5;
	sector_reads = 0u;
	if (iomgr_query_space(volume, true, &space) != IOMGR_OK ||
	    sector_reads != 0u || initial_free_bytes < 9u * 512u ||
	    space.free_bytes != initial_free_bytes - 9u * 512u)
		return 6;
	if (iomgr_unmount(volume) != IOMGR_OK)
		return 7;
	return 0;
}

static int mount_and_check(enum test_volume_kind kind,
			   block_lba_t sector_count, uint8_t fat_bits)
{
	struct iomgr_mount_request request = {
		.device = TEST_DEVICE,
		.first_lba = TEST_FIRST_LBA,
		.sector_count = sector_count,
		.flags = 0u,
		.reserved = 0u,
	};
	struct iomgr_volume_info info;
	struct fat_driver_volume_snapshot snapshot;
	iomgr_volume_handle_t volume;
	uint32_t reads_before = sector_reads;

	selected_volume = kind;
	if (iomgr_mount(&request, &volume) != IOMGR_OK ||
	    sector_reads != reads_before + 2u ||
	    iomgr_get_volume_info(volume, &info) != IOMGR_OK ||
	    info.driver_identity != FAT_DRIVER_IDENTITY ||
	    info.device != TEST_DEVICE || info.first_lba != TEST_FIRST_LBA ||
	    info.sector_count != sector_count ||
	    info.capabilities !=
		(IOMGR_VOLUME_CAP_READ | IOMGR_VOLUME_CAP_WRITE |
		 IOMGR_VOLUME_CAP_CASE_PRESERVING) ||
	    info.maximum_name_units != 12u ||
	    fat_driver_get_volume(volume, &snapshot) != IOMGR_OK ||
	    snapshot.device != TEST_DEVICE ||
	    snapshot.first_lba != TEST_FIRST_LBA ||
	    snapshot.sector_count != sector_count ||
	    snapshot.layout.fat_bits != fat_bits ||
	    iomgr_unmount(volume) != IOMGR_OK)
		return fat_bits;
	return 0;
}

static int run_tests(void)
{
	struct iomgr_mount_request request = {
		.device = TEST_DEVICE,
		.first_lba = TEST_FIRST_LBA,
		.sector_count = 70000u,
		.flags = 0u,
		.reserved = 0u,
	};
	iomgr_volume_handle_t untouched = 0xa5a5a5a5a5a5a5a5ull;
	uint32_t reads_before;
	int result;

	if (iomgr_initialize() != IOMGR_OK ||
	    fat_driver_register() != IOMGR_OK ||
	    fat_driver_register() != IOMGR_DUPLICATE_DRIVER)
		return 1;
	result = mount_and_check(TEST_FAT12, 2880u, FAT_TABLE_12);
	if (result != 0)
		return 10 + result;
	result = mount_and_check(TEST_FAT16, 32768u, FAT_TABLE_16);
	if (result != 0)
		return 20 + result;
	result = mount_and_check(TEST_FAT32, 70000u, FAT_TABLE_32);
	if (result != 0)
		return 30 + result;
	if (named_io_check() != 0)
		return 39;

	selected_volume = TEST_CORRUPT_FAT32;
	reads_before = sector_reads;
	if (iomgr_mount(&request, &untouched) != IOMGR_CORRUPT ||
	    untouched != 0xa5a5a5a5a5a5a5a5ull ||
	    sector_reads != reads_before + 1u)
		return 40;
	selected_volume = TEST_NTFS;
	if (iomgr_mount(&request, &untouched) != IOMGR_NO_DRIVER ||
	    untouched != 0xa5a5a5a5a5a5a5a5ull)
		return 41;
	return 0;
}

DOSC32_TEST_ENTRY(run_tests)

// SPDX-License-Identifier: GPL-2.0-only
/* Freestanding tests for the 64-bit block-device handle boundary. */
#include "block_device.h"
#include "test_entry.h"

#define TEST_CONTEXT ((kernel_object_handle_t)0x1122334455667788ull)
#define TEST_SECTORS 4u
#define HANDLE_SENTINEL ((block_device_handle_t)0xaabbccddeeff0011ull)
#define INVALID_ADAPTER_STATUS ((enum block_device_status)0x7f)

static union block_device_sector storage[TEST_SECTORS];
static enum block_device_status probe_status = BLOCK_DEVICE_OK;
static enum block_device_status read_status = BLOCK_DEVICE_OK;
static enum block_device_status write_status = BLOCK_DEVICE_OK;
static enum block_device_status flush_status = BLOCK_DEVICE_OK;
static uint8_t writable = 1u;
static uint32_t logical_sector_bytes = BLOCK_DEVICE_SECTOR_BYTES;
static uint32_t read_calls;
static uint32_t write_calls;
static uint32_t flush_calls;

static enum block_device_status
test_probe(kernel_object_handle_t context,
	   struct block_device_geometry *geometry)
{
	if (context != TEST_CONTEXT || geometry == NULL)
		return BLOCK_DEVICE_INVALID_ARGUMENT;
	geometry->sector_count = TEST_SECTORS;
	geometry->logical_sector_bytes = logical_sector_bytes;
	geometry->writable = writable;
	return probe_status;
}

static enum block_device_status test_read(kernel_object_handle_t context,
					  block_lba_t lba,
					  union block_device_sector *sector)
{
	size_t index;

	++read_calls;
	if (context != TEST_CONTEXT || sector == NULL || lba >= TEST_SECTORS)
		return BLOCK_DEVICE_IO_ERROR;
	/* Simulate a backend that modifies its output before reporting failure.
	 */
	sector->words[0] = 0xdeadu;
	if (read_status != BLOCK_DEVICE_OK)
		return read_status;
	for (index = 0u; index < ARRAY_SIZE(sector->words); ++index)
		sector->words[index] = storage[(size_t)lba].words[index];
	return BLOCK_DEVICE_OK;
}

static enum block_device_status
test_write(kernel_object_handle_t context, block_lba_t lba,
	   const union block_device_sector *sector)
{
	size_t index;

	++write_calls;
	if (context != TEST_CONTEXT || sector == NULL || lba >= TEST_SECTORS)
		return BLOCK_DEVICE_IO_ERROR;
	if (write_status != BLOCK_DEVICE_OK)
		return write_status;
	for (index = 0u; index < ARRAY_SIZE(sector->words); ++index)
		storage[(size_t)lba].words[index] = sector->words[index];
	return BLOCK_DEVICE_OK;
}

static enum block_device_status test_flush(kernel_object_handle_t context)
{
	++flush_calls;
	return context == TEST_CONTEXT ? flush_status : BLOCK_DEVICE_IO_ERROR;
}

static const struct block_device_ops test_ops = {
    .probe = test_probe,
    .read_sector = test_read,
    .write_sector = test_write,
    .flush = test_flush,
};

static void set_geometry_sentinel(struct block_device_geometry *geometry)
{
	geometry->sector_count = 0x0102030405060708ull;
	geometry->logical_sector_bytes = 0xa1b2c3d4u;
	geometry->writable = 0x5au;
	geometry->reserved[0] = 0x11u;
	geometry->reserved[1] = 0x22u;
	geometry->reserved[2] = 0x33u;
}

static bool geometry_is_sentinel(
    const struct block_device_geometry *geometry)
{
	return geometry->sector_count == 0x0102030405060708ull &&
	       geometry->logical_sector_bytes == 0xa1b2c3d4u &&
	       geometry->writable == 0x5au && geometry->reserved[0] == 0x11u &&
	       geometry->reserved[1] == 0x22u && geometry->reserved[2] == 0x33u;
}

static int run_tests(void)
{
	union block_device_sector sector;
	struct block_device_geometry geometry;
	block_device_handle_t first;
	block_device_handle_t second;
	size_t index;

	probe_status = BLOCK_DEVICE_NO_MEDIA;
	first = HANDLE_SENTINEL;
	if (block_device_register(&test_ops, TEST_CONTEXT, &first) !=
		BLOCK_DEVICE_NO_MEDIA ||
	    first != HANDLE_SENTINEL)
		return 1;
	probe_status = INVALID_ADAPTER_STATUS;
	if (block_device_register(&test_ops, TEST_CONTEXT, &first) !=
		BLOCK_DEVICE_IO_ERROR ||
	    first != HANDLE_SENTINEL)
		return 2;
	probe_status = BLOCK_DEVICE_OK;
	writable = 2u;
	if (block_device_register(&test_ops, TEST_CONTEXT, &first) !=
		BLOCK_DEVICE_UNSUPPORTED ||
	    first != HANDLE_SENTINEL)
		return 3;
	writable = 1u;
	if (block_device_register(&test_ops, TEST_CONTEXT, &first) !=
		BLOCK_DEVICE_OK ||
	    first == BLOCK_DEVICE_HANDLE_INVALID || (first >> 32) == 0u)
		return 4;
	set_geometry_sentinel(&geometry);
	if (block_device_get_geometry(first, &geometry) != BLOCK_DEVICE_OK ||
	    geometry.sector_count != TEST_SECTORS ||
	    geometry.logical_sector_bytes != BLOCK_DEVICE_SECTOR_BYTES ||
	    geometry.writable != 1u || geometry.reserved[0] != 0u ||
	    geometry.reserved[1] != 0u || geometry.reserved[2] != 0u)
		return 5;

	for (index = 0u; index < ARRAY_SIZE(storage[1].words); ++index)
		storage[1].words[index] = (uint16_t)(index ^ 0x5a5au);
	if (block_device_read_sector(first, 1u, &sector) != BLOCK_DEVICE_OK ||
	    read_calls != 1u)
		return 6;
	for (index = 0u; index < ARRAY_SIZE(sector.words); ++index) {
		if (sector.words[index] != (uint16_t)(index ^ 0x5a5au))
			return 7;
	}
	if (block_device_read_sector(first, TEST_SECTORS, &sector) !=
		BLOCK_DEVICE_OUT_OF_RANGE ||
	    read_calls != 1u)
		return 8;

	for (index = 0u; index < ARRAY_SIZE(sector.words); ++index)
		sector.words[index] = 0xa5a5u;
	read_status = BLOCK_DEVICE_IO_ERROR;
	if (block_device_read_sector(first, 0u, &sector) !=
		BLOCK_DEVICE_IO_ERROR ||
	    sector.words[0] != 0xa5a5u)
		return 9;
	read_status = INVALID_ADAPTER_STATUS;
	if (block_device_read_sector(first, 0u, &sector) !=
		BLOCK_DEVICE_IO_ERROR ||
	    sector.words[0] != 0xa5a5u || read_calls != 3u)
		return 10;
	read_status = BLOCK_DEVICE_OK;

	for (index = 0u; index < ARRAY_SIZE(sector.words); ++index)
		sector.words[index] = (uint16_t)(0xf000u | index);
	if (block_device_write_sector(first, 2u, &sector) != BLOCK_DEVICE_OK ||
	    write_calls != 1u || storage[2].words[17] != 0xf011u)
		return 11;
	storage[3].words[17] = 0x1357u;
	write_status = INVALID_ADAPTER_STATUS;
	if (block_device_write_sector(first, 3u, &sector) !=
		BLOCK_DEVICE_IO_ERROR ||
	    write_calls != 2u || storage[3].words[17] != 0x1357u)
		return 12;
	write_status = BLOCK_DEVICE_OK;
	flush_status = INVALID_ADAPTER_STATUS;
	if (block_device_flush(first) != BLOCK_DEVICE_IO_ERROR ||
	    flush_calls != 1u)
		return 13;
	if (block_device_unregister(first) != BLOCK_DEVICE_IO_ERROR ||
	    flush_calls != 2u ||
	    block_device_get_geometry(first, &geometry) != BLOCK_DEVICE_OK)
		return 14;
	flush_status = BLOCK_DEVICE_OK;
	if (block_device_unregister(first) != BLOCK_DEVICE_OK ||
	    flush_calls != 3u)
		return 15;
	if (block_device_read_sector(first, 0u, &sector) !=
	    BLOCK_DEVICE_STALE_HANDLE)
		return 16;
	set_geometry_sentinel(&geometry);
	if (block_device_get_geometry(first, &geometry) !=
		BLOCK_DEVICE_STALE_HANDLE ||
	    !geometry_is_sentinel(&geometry))
		return 17;

	writable = 0u;
	if (block_device_register(&test_ops, TEST_CONTEXT, &second) !=
		BLOCK_DEVICE_OK ||
	    second == first)
		return 18;
	if (block_device_write_sector(second, 0u, &sector) !=
		BLOCK_DEVICE_READ_ONLY ||
	    write_calls != 2u)
		return 19;
	if (block_device_unregister(second) != BLOCK_DEVICE_OK)
		return 20;

	logical_sector_bytes = 1024u;
	second = HANDLE_SENTINEL;
	if (block_device_register(&test_ops, TEST_CONTEXT, &second) !=
		BLOCK_DEVICE_UNSUPPORTED ||
	    second != HANDLE_SENTINEL)
		return 21;
	if (block_device_register(NULL, TEST_CONTEXT, &second) !=
		BLOCK_DEVICE_INVALID_ARGUMENT ||
	    second != HANDLE_SENTINEL)
		return 22;
	return 0;
}

DOSC32_TEST_ENTRY(run_tests)

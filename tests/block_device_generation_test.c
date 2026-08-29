// SPDX-License-Identifier: GPL-2.0-only
/* Freestanding terminal-generation and stale-handle tests. */
#include "block_device.h"
#include "test_entry.h"

#ifndef BLOCK_DEVICE_TEST_GENERATION_MAX
#error "generation test requires BLOCK_DEVICE_TEST_GENERATION_MAX"
#endif

#define TEST_CONTEXT ((kernel_object_handle_t)0x8877665544332211ull)
#define TEST_SLOT_COUNT 8u
#define HANDLE_SENTINEL ((block_device_handle_t)0x123456789abcdef0ull)

static uint32_t probe_calls;

static enum block_device_status
test_probe(kernel_object_handle_t context,
	   struct block_device_geometry *geometry)
{
	++probe_calls;
	if (context != TEST_CONTEXT || geometry == NULL)
		return BLOCK_DEVICE_INVALID_ARGUMENT;
	geometry->sector_count = 1u;
	geometry->logical_sector_bytes = BLOCK_DEVICE_SECTOR_BYTES;
	geometry->writable = 0u;
	return BLOCK_DEVICE_OK;
}

static enum block_device_status test_read(kernel_object_handle_t context,
						  block_lba_t lba,
						  union block_device_sector *sector)
{
	if (context != TEST_CONTEXT || lba != 0u || sector == NULL)
		return BLOCK_DEVICE_IO_ERROR;
	sector->words[0] = 0x55aau;
	return BLOCK_DEVICE_OK;
}

static const struct block_device_ops test_ops = {
	.probe = test_probe,
	.read_sector = test_read,
	.write_sector = NULL,
	.flush = NULL,
};

static int run_tests(void)
{
	block_device_handle_t first_handles[TEST_SLOT_COUNT];
	block_device_handle_t terminal_handles[TEST_SLOT_COUNT];
	union block_device_sector sector;
	block_device_handle_t output;
	uint32_t slot;

	if ((uint32_t)BLOCK_DEVICE_TEST_GENERATION_MAX != 2u)
		return 1;
	for (slot = 0u; slot < TEST_SLOT_COUNT; ++slot) {
		if (block_device_register(&test_ops, TEST_CONTEXT,
						  &first_handles[slot]) != BLOCK_DEVICE_OK ||
		    (uint32_t)first_handles[slot] != slot + 1u ||
		    (uint32_t)(first_handles[slot] >> 32) != 1u)
			return 2;
	}
	output = HANDLE_SENTINEL;
	if (block_device_register(&test_ops, TEST_CONTEXT, &output) !=
		BLOCK_DEVICE_NO_SLOT ||
	    output != HANDLE_SENTINEL || probe_calls != TEST_SLOT_COUNT)
		return 3;

	for (slot = 0u; slot < TEST_SLOT_COUNT; ++slot) {
		if (block_device_unregister(first_handles[slot]) !=
		    BLOCK_DEVICE_OK)
			return 4;
		if (block_device_register(&test_ops, TEST_CONTEXT,
						  &terminal_handles[slot]) !=
					BLOCK_DEVICE_OK ||
		    (uint32_t)terminal_handles[slot] != slot + 1u ||
		    (uint32_t)(terminal_handles[slot] >> 32) != 2u ||
		    terminal_handles[slot] == first_handles[slot])
			return 5;

		sector.words[0] = 0xa55au;
		if (block_device_read_sector(first_handles[slot], 0u, &sector) !=
			BLOCK_DEVICE_STALE_HANDLE ||
		    sector.words[0] != 0xa55au)
			return 6;
		if (block_device_read_sector(terminal_handles[slot], 0u, &sector) !=
			BLOCK_DEVICE_OK ||
		    sector.words[0] != 0x55aau)
			return 7;
		if (block_device_unregister(terminal_handles[slot]) !=
		    BLOCK_DEVICE_OK)
			return 8;
		sector.words[0] = 0x5aa5u;
		if (block_device_read_sector(terminal_handles[slot], 0u, &sector) !=
			BLOCK_DEVICE_STALE_HANDLE ||
		    sector.words[0] != 0x5aa5u)
			return 9;
		if (slot + 1u < TEST_SLOT_COUNT) {
			uint32_t calls_before = probe_calls;

			output = HANDLE_SENTINEL;
			if (block_device_register(&test_ops, TEST_CONTEXT, &output) !=
				BLOCK_DEVICE_NO_SLOT ||
			    output != HANDLE_SENTINEL ||
			    probe_calls != calls_before)
				return 12;
		}
	}

	output = HANDLE_SENTINEL;
	if (block_device_register(&test_ops, TEST_CONTEXT, &output) !=
		BLOCK_DEVICE_GENERATION_EXHAUSTED ||
	    output != HANDLE_SENTINEL || probe_calls != TEST_SLOT_COUNT * 2u)
		return 10;
	for (slot = 0u; slot < TEST_SLOT_COUNT; ++slot) {
		sector.words[0] = 0xc33cu;
		if (block_device_read_sector(first_handles[slot], 0u, &sector) !=
			BLOCK_DEVICE_STALE_HANDLE ||
		    sector.words[0] != 0xc33cu)
			return 11;
	}
	return 0;
}

DOSC32_TEST_ENTRY(run_tests)

// SPDX-License-Identifier: GPL-2.0-only
/* Byte-level tests for the MS-DOS guest drive representation. */
#include "dos_dpb.h"
#include "test_entry.h"

static uint16_t read_le16(const uint8_t *bytes)
{
	return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8u);
}

static int run_tests(void)
{
	struct dos_dpb_parameters parameters = {
		.root_start = 0x0123u,
		.data_start = 0x0234u,
		.bytes_per_sector = 512u,
		.reserved_sectors = 1024u,
		.root_entries = 512u,
		.sectors_per_fat = 128u,
		.maximum_cluster = 0x3456u,
		.drive = 2u,
		.unit = 0u,
		.sectors_per_cluster = 4u,
		.fat_count = 2u,
		.media = 0xf8u,
	};
	uint8_t dpb[DOS_DPB_BYTES];
	uint8_t driver[DOS_DPB_DRIVER_BYTES];

	if (dos_dpb_encode(NULL, dpb) != DOS_DPB_INVALID_ARGUMENT ||
	    dos_dpb_encode(&parameters, NULL) != DOS_DPB_INVALID_ARGUMENT ||
	    dos_dpb_encode(&parameters, dpb) != DOS_DPB_OK)
		return 1;
	if (dpb[0] != 2u || dpb[1] != 0u || read_le16(dpb + 2u) != 512u ||
	    dpb[4] != 3u || dpb[5] != 2u ||
	    read_le16(dpb + 6u) != 1024u || dpb[8] != 2u ||
	    read_le16(dpb + 9u) != 512u ||
	    read_le16(dpb + 11u) != 0x0234u ||
	    read_le16(dpb + 13u) != 0x3456u ||
	    read_le16(dpb + 15u) != 128u ||
	    read_le16(dpb + 17u) != 0x0123u)
		return 2;
	if (read_le16(dpb + 19u) != DOS_DPB_DRIVER_OFFSET ||
	    read_le16(dpb + 21u) != DOS_DPB_SEGMENT || dpb[23] != 0xf8u ||
	    dpb[24] != 0u || read_le16(dpb + 25u) != 0xffffu ||
	    read_le16(dpb + 27u) != 0xffffu || read_le16(dpb + 29u) != 0u ||
	    read_le16(dpb + 31u) != 0xffffu)
		return 3;

	dos_dpb_encode_driver_header(driver);
	if (read_le16(driver) != 0xffffu || read_le16(driver + 2u) != 0xffffu ||
	    read_le16(driver + 4u) != 0x0800u ||
	    read_le16(driver + 6u) != DOS_DPB_DRIVER_OFFSET + 18u ||
	    read_le16(driver + 8u) != DOS_DPB_DRIVER_OFFSET + 19u ||
	    driver[10] != 1u || driver[18] != 0xcbu || driver[19] != 0xcbu)
		return 4;

	parameters.sectors_per_cluster = 3u;
	if (dos_dpb_encode(&parameters, dpb) != DOS_DPB_INVALID_ARGUMENT)
		return 5;
	parameters.sectors_per_cluster = 4u;
	parameters.data_start = 0x10000u;
	if (dos_dpb_encode(&parameters, dpb) != DOS_DPB_UNREPRESENTABLE)
		return 6;
	return 0;
}

DOSC32_TEST_ENTRY(run_tests)

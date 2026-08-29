/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DOSC32_DOS_DPB_H
#define DOSC32_DOS_DPB_H

#include "compiler.h"
#include "types.h"

#define DOS_DPB_BYTES 33u
#define DOS_DPB_SEGMENT 0x0f00u
#define DOS_DPB_OFFSET 0x0200u
#define DOS_DPB_DRIVER_OFFSET 0x0240u
#define DOS_DPB_DRIVER_BYTES 20u

struct dos_dpb_parameters {
	uint32_t root_start;
	uint32_t data_start;
	uint16_t bytes_per_sector;
	uint16_t reserved_sectors;
	uint16_t root_entries;
	uint16_t sectors_per_fat;
	uint16_t maximum_cluster;
	uint8_t drive;
	uint8_t unit;
	uint8_t sectors_per_cluster;
	uint8_t fat_count;
	uint8_t media;
};

enum dos_dpb_status {
	DOS_DPB_OK = 0,
	DOS_DPB_INVALID_ARGUMENT,
	DOS_DPB_UNREPRESENTABLE
};

enum dos_dpb_status dos_dpb_encode(
	const struct dos_dpb_parameters *parameters,
	uint8_t output[DOS_DPB_BYTES]) __must_check;
void dos_dpb_encode_driver_header(
	uint8_t output[DOS_DPB_DRIVER_BYTES]);

#endif

/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DOSC32_DOS_FIND_H
#define DOSC32_DOS_FIND_H

#include "address.h"
#include "compiler.h"
#include "dos_abi.h"
#include "types.h"

#define DOS_FIND_NAME83_BYTES 11u
#define DOS_FIND_PACKED_NAME_BYTES 13u

struct dos_find_record {
	uint32_t file_size;
	/* DOS-private DTA bytes 13..20 hold the I/O Manager search identity. */
	kernel_object_handle_t search_handle;
	uint16_t modified_time;
	uint16_t modified_date;
	uint8_t search_drive;
	uint8_t search_name[DOS_FIND_NAME83_BYTES];
	uint8_t search_attributes;
	uint8_t found_attributes;
	uint8_t packed_name[DOS_FIND_PACKED_NAME_BYTES];
};

enum dos_find_status {
	DOS_FIND_OK = 0,
	DOS_FIND_INVALID_ARGUMENT,
	DOS_FIND_INVALID_NAME
};

enum dos_find_status dos_find_record_encode(
	const struct dos_find_record *record,
	uint8_t output[DOS_DTA_FIND_SIZE]) __must_check;
enum dos_find_status dos_find_record_decode(
	const uint8_t input[DOS_DTA_FIND_SIZE],
	struct dos_find_record *record) __must_check;

#endif

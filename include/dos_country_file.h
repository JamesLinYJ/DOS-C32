/* SPDX-License-Identifier: GPL-2.0-only */
/* Versioned, pointer-free DOS-C32 COUNTRY.SYS interchange format. */
#ifndef DOSC32_DOS_COUNTRY_FILE_H
#define DOSC32_DOS_COUNTRY_FILE_H

#include "compiler.h"
#include "dos_nls.h"
#include "types.h"

#define DOS_COUNTRY_FILE_VERSION 1u
#define DOS_COUNTRY_HEADER_BYTES 32u
#define DOS_COUNTRY_ENTRY_BYTES 48u
#define DOS_COUNTRY_HIGH_TABLE_BYTES 128u
#define DOS_COUNTRY_MAX_DBCS_BYTES 8u
#define DOS_COUNTRY_MAX_PACKAGES 16u
#define DOS_COUNTRY_MAX_FILE_BYTES 8192u

enum dos_country_status {
	DOS_COUNTRY_OK = 0,
	DOS_COUNTRY_INVALID_ARGUMENT,
	DOS_COUNTRY_CAPACITY,
	DOS_COUNTRY_OVERFLOW,
	DOS_COUNTRY_FORMAT,
	DOS_COUNTRY_CHECKSUM
};

struct dos_country_package_storage {
	struct dos_nls_package package;
	uint8_t collate_high[DOS_COUNTRY_HIGH_TABLE_BYTES];
	uint8_t upcase_high[DOS_COUNTRY_HIGH_TABLE_BYTES];
	uint8_t dbcs[DOS_COUNTRY_MAX_DBCS_BYTES];
};

/* Do not copy a populated catalog: package pointers refer to its own slots. */
struct dos_country_catalog {
	struct dos_country_package_storage packages[DOS_COUNTRY_MAX_PACKAGES];
	uint16_t package_count;
	uint16_t format_version;
	uint32_t checksum;
};

enum dos_country_status dos_country_encoded_size(uint64_t *encoded_bytes)
	__must_check;
enum dos_country_status dos_country_encode(uint8_t *destination,
					    size_t capacity,
					    size_t *written) __must_check;
enum dos_country_status dos_country_parse(const uint8_t *file,
					   size_t file_bytes,
					   struct dos_country_catalog *catalog)
	__must_check;

#endif

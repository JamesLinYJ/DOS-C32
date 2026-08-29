/* SPDX-License-Identifier: GPL-2.0-only */
/* Immutable DOS national-language packages shared by INT 21h NLS services. */
#ifndef DOSC32_DOS_NLS_H
#define DOSC32_DOS_NLS_H

#include "compiler.h"
#include "types.h"

struct dos_nls_dbcs_table {
	const uint8_t *ranges;
	uint16_t length;
};

struct dos_nls_country_format {
	uint8_t currency[5];
	uint16_t country;
	uint16_t date_format;
	uint8_t thousands_separator;
	uint8_t decimal_separator;
	uint8_t date_separator;
	uint8_t time_separator;
	uint8_t currency_format;
	uint8_t currency_digits;
	uint8_t time_format;
	uint8_t list_separator;
};

struct dos_nls_package {
	struct dos_nls_dbcs_table dbcs;
	struct dos_nls_country_format format;
	const uint8_t *collate_high;
	const uint8_t *upcase_high;
	uint16_t code_page;
	bool complete;
};

#define DOS_NLS_RUNTIME_PACKAGE_MAX 16u

struct dos_nls_runtime {
	const struct dos_nls_package *active;
	const struct dos_nls_package *packages[DOS_NLS_RUNTIME_PACKAGE_MAX];
	uint32_t generation;
	uint16_t system_code_page;
	uint16_t package_count;
	bool initialized;
};

struct dos_nls_switch {
	const struct dos_nls_package *target;
	uint32_t expected_generation;
	bool prepared;
};

#define DOS_NLS_DEFAULT_COUNTRY 1u

size_t dos_nls_package_count(void);
const struct dos_nls_package *dos_nls_package_at(size_t index);
const struct dos_nls_package *dos_nls_find_package(uint16_t code_page);
bool dos_nls_validate_package(const struct dos_nls_package *package)
	__must_check;

/*
 * Returns the complete lead-byte range table for a code page known to the
 * DOS 4 COUNTRY.SYS data set.  length includes the terminating 00h,00h pair.
 */
bool dos_nls_get_dbcs_table(uint16_t code_page,
			    struct dos_nls_dbcs_table *table) __must_check;

bool dos_nls_runtime_initialize(struct dos_nls_runtime *runtime,
				uint16_t system_code_page,
				uint16_t active_code_page) __must_check;
bool dos_nls_runtime_publish_catalog(
	struct dos_nls_runtime *runtime,
	const struct dos_nls_package *const *packages,
	size_t package_count) __must_check;
bool dos_nls_prepare_switch(const struct dos_nls_runtime *runtime,
			    uint16_t code_page,
			    struct dos_nls_switch *transaction) __must_check;
bool dos_nls_commit_switch(struct dos_nls_runtime *runtime,
			   struct dos_nls_switch *transaction) __must_check;
void dos_nls_abort_switch(struct dos_nls_switch *transaction);

#endif

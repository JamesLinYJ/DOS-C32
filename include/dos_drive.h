/* SPDX-License-Identifier: GPL-2.0-only */
/* DOS drive namespace helpers; policy state is supplied by the boot owner. */
#ifndef DOSC32_DOS_DRIVE_H
#define DOSC32_DOS_DRIVE_H

#include "compiler.h"
#include "types.h"

#define DOS_DRIVE_COUNT 26u
#define DOS_FIRST_FIXED_DRIVE_INDEX 2u

struct dos_int21_drive_config;

enum dos_drive_status {
	DOS_DRIVE_OK = 0,
	DOS_DRIVE_INVALID_ARGUMENT,
	DOS_DRIVE_UNAVAILABLE,
	DOS_DRIVE_CAPACITY
};

bool dos_drive_index_is_valid(uint8_t drive_index);
enum dos_drive_status dos_drive_configure_single_volume(
	uint8_t drive_index, struct dos_int21_drive_config *config)
	__must_check;
enum dos_drive_status dos_drive_resolve_designator(
	const struct dos_int21_drive_config *config, uint8_t designator,
	uint8_t *drive_index) __must_check;
enum dos_drive_status dos_drive_format_root(
	uint8_t drive_index, char *destination, size_t capacity,
	size_t *length) __must_check;
enum dos_drive_status dos_drive_format_absolute(
	uint8_t drive_index, const char *component, size_t component_length,
	char *destination, size_t capacity, size_t *length) __must_check;

#endif

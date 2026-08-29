// SPDX-License-Identifier: GPL-2.0-only
/* Drive-namespace tests: runtime selection, designators and bounded paths. */
#include "dos_drive.h"
#include "dos_int21.h"
#include "string.h"
#include "test_entry.h"

static bool text_equals(const char *left, const char *right, size_t capacity)
{
	return strncmp(left, right, capacity) == 0;
}

static int run_tests(void)
{
	struct dos_int21_drive_config config = {
		.available_drive_mask = 0xaaaaaaaau,
		.current_drive = 0xaau,
		.boot_drive = 0xaau,
		.last_drive = 0xaau,
		.reserved = 0xaau,
	};
	struct dos_int21_drive_config unchanged = config;
	char path[32] = "unchanged";
	uint8_t drive = 0xffu;
	size_t length = 0u;
	int comparison = 1;

	if (dos_drive_configure_single_volume(DOS_DRIVE_COUNT, &config) !=
			DOS_DRIVE_INVALID_ARGUMENT ||
	    memcmp_s(&config, sizeof(config), &unchanged, sizeof(unchanged),
		     sizeof(config), &comparison) != MEMORY_OK ||
	    comparison != 0)
		return 1;
	if (dos_drive_configure_single_volume(DOS_FIRST_FIXED_DRIVE_INDEX,
					      &config) != DOS_DRIVE_OK ||
	    config.available_drive_mask != ((uint32_t)1u << 2u) ||
	    config.current_drive != 2u || config.boot_drive != 3u ||
	    config.last_drive != 3u || !dos_int21_drive_config_is_valid(&config))
		return 2;
	if (dos_drive_resolve_designator(&config, 0u, &drive) != DOS_DRIVE_OK ||
	    drive != 2u ||
	    dos_drive_resolve_designator(&config, 3u, &drive) != DOS_DRIVE_OK ||
	    drive != 2u ||
	    dos_drive_resolve_designator(&config, 1u, &drive) !=
			DOS_DRIVE_UNAVAILABLE)
		return 3;
	if (dos_drive_format_root(1u, path, sizeof(path), &length) !=
			DOS_DRIVE_OK ||
	    length != 3u || !text_equals(path, "B:\\", sizeof(path)))
		return 4;
	if (dos_drive_format_absolute(2u, "COMMAND.COM", 11u, path,
				      sizeof(path), &length) != DOS_DRIVE_OK ||
	    length != 14u || !text_equals(path, "C:\\COMMAND.COM", sizeof(path)))
		return 5;
	if (dos_drive_format_absolute(2u, "\\COMMAND.COM", 12u, path,
				      sizeof(path), &length) !=
			DOS_DRIVE_INVALID_ARGUMENT ||
	    dos_drive_format_absolute(2u, "COUNTRY.SYS", 11u, path, 14u,
				      &length) != DOS_DRIVE_CAPACITY ||
	    dos_drive_format_root(2u, path, 3u, &length) != DOS_DRIVE_CAPACITY)
		return 6;
	if (dos_drive_configure_single_volume(25u, &config) != DOS_DRIVE_OK ||
	    config.current_drive != 25u || config.boot_drive != 26u ||
	    config.last_drive != 26u ||
	    dos_drive_resolve_designator(&config, 26u, &drive) != DOS_DRIVE_OK ||
	    drive != 25u)
		return 7;
	return 0;
}

DOSC32_TEST_ENTRY(run_tests)

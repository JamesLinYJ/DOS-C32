/* SPDX-License-Identifier: GPL-2.0-only */
/* Fixed low-memory handoff from the legacy BIOS entry to the x86 kernel. */
#ifndef DOSC32_X86_BOOT_INFO_H
#define DOSC32_X86_BOOT_INFO_H

#define X86_BOOT_INFO_ADDRESS 0x00005000
#define X86_BOOT_INFO_SIGNATURE 0x32494258
#define X86_BOOT_INFO_VERSION 4
#define X86_BOOT_INFO_FIXED_HEADER_BYTES 16
#define X86_BOOT_MEMORY_RANGE_BYTES 24
#define X86_BOOT_MEMORY_RANGE_COUNT 64

/* Optional real-mode-only facts captured before BIOS services disappear. */
#define X86_BOOT_PLATFORM_BYTES 16
#define X86_BOOT_PLATFORM_RTC_PRESENT (1 << 0)
#define X86_BOOT_PLATFORM_DISPLAY_DCC_PRESENT (1 << 1)
#define X86_BOOT_PLATFORM_FLAG_MASK                                      \
	(X86_BOOT_PLATFORM_RTC_PRESENT | X86_BOOT_PLATFORM_DISPLAY_DCC_PRESENT)

/* Versioned INT 13h EDD handoff captured before protected-mode entry. */
#define X86_BOOT_STORAGE_SIGNATURE 0x31444445
#define X86_BOOT_STORAGE_VERSION 2
#define X86_BOOT_STORAGE_BYTES 136
#define X86_BOOT_EDD_PARAMETERS_BYTES 74
#define X86_BOOT_EDD_DPTE_BYTES 16

#define X86_BOOT_STORAGE_EXTENSIONS_PRESENT (1 << 0)
#define X86_BOOT_STORAGE_PARAMETERS_PRESENT (1 << 1)
#define X86_BOOT_STORAGE_DPTE_PRESENT (1 << 2)
#define X86_BOOT_STORAGE_VOLUME_PRESENT (1 << 3)
#define X86_BOOT_STORAGE_FLAG_MASK                                      \
	(X86_BOOT_STORAGE_EXTENSIONS_PRESENT |                            \
	 X86_BOOT_STORAGE_PARAMETERS_PRESENT | X86_BOOT_STORAGE_DPTE_PRESENT | \
	 X86_BOOT_STORAGE_VOLUME_PRESENT)

#define X86_BOOT_INFO_SIGNATURE_OFFSET 0
#define X86_BOOT_INFO_VERSION_OFFSET 4
#define X86_BOOT_INFO_HEADER_BYTES_OFFSET 6
#define X86_BOOT_INFO_RANGE_BYTES_OFFSET 8
#define X86_BOOT_INFO_RANGE_COUNT_OFFSET 10
#define X86_BOOT_INFO_FLAGS_OFFSET 12
#define X86_BOOT_INFO_PLATFORM_OFFSET X86_BOOT_INFO_FIXED_HEADER_BYTES
#define X86_BOOT_INFO_STORAGE_OFFSET                                    \
	(X86_BOOT_INFO_PLATFORM_OFFSET + X86_BOOT_PLATFORM_BYTES)
#define X86_BOOT_INFO_RANGES_OFFSET                                  \
	(X86_BOOT_INFO_STORAGE_OFFSET + X86_BOOT_STORAGE_BYTES)
#define X86_BOOT_INFO_HEADER_BYTES X86_BOOT_INFO_RANGES_OFFSET

#define X86_BOOT_PLATFORM_FLAGS_OFFSET 0
#define X86_BOOT_PLATFORM_RTC_CENTURY_BCD_OFFSET 4
#define X86_BOOT_PLATFORM_RTC_YEAR_BCD_OFFSET 5
#define X86_BOOT_PLATFORM_RTC_MONTH_BCD_OFFSET 6
#define X86_BOOT_PLATFORM_RTC_DAY_BCD_OFFSET 7
#define X86_BOOT_PLATFORM_RTC_HOUR_BCD_OFFSET 8
#define X86_BOOT_PLATFORM_RTC_MINUTE_BCD_OFFSET 9
#define X86_BOOT_PLATFORM_RTC_SECOND_BCD_OFFSET 10
#define X86_BOOT_PLATFORM_RTC_DAYLIGHT_OFFSET 11
#define X86_BOOT_PLATFORM_DISPLAY_ACTIVE_DCC_OFFSET 12
#define X86_BOOT_PLATFORM_DISPLAY_INACTIVE_DCC_OFFSET 13
#define X86_BOOT_PLATFORM_RESERVED_OFFSET 14
#define X86_BOOT_PLATFORM_RESERVED_BYTES 2

#define X86_BOOT_STORAGE_SIGNATURE_OFFSET 0
#define X86_BOOT_STORAGE_VERSION_OFFSET 4
#define X86_BOOT_STORAGE_BYTES_OFFSET 6
#define X86_BOOT_STORAGE_FLAGS_OFFSET 8
#define X86_BOOT_STORAGE_INTERFACE_SUPPORT_OFFSET 12
#define X86_BOOT_STORAGE_BOOT_DRIVE_OFFSET 14
#define X86_BOOT_STORAGE_EDD_VERSION_OFFSET 15
#define X86_BOOT_STORAGE_PARAMETERS_OFFSET 16
#define X86_BOOT_STORAGE_DPTE_OFFSET                                  \
	(X86_BOOT_STORAGE_PARAMETERS_OFFSET + X86_BOOT_EDD_PARAMETERS_BYTES)
#define X86_BOOT_STORAGE_VOLUME_START_LBA_OFFSET                      \
	(X86_BOOT_STORAGE_DPTE_OFFSET + X86_BOOT_EDD_DPTE_BYTES)
#define X86_BOOT_STORAGE_VOLUME_SECTOR_COUNT_OFFSET                   \
	(X86_BOOT_STORAGE_VOLUME_START_LBA_OFFSET + 8)
#define X86_BOOT_STORAGE_VOLUME_SECTOR_BYTES_OFFSET                   \
	(X86_BOOT_STORAGE_VOLUME_SECTOR_COUNT_OFFSET + 8)
#define X86_BOOT_STORAGE_RESERVED_OFFSET                              \
	(X86_BOOT_STORAGE_VOLUME_SECTOR_BYTES_OFFSET + 2)
#define X86_BOOT_STORAGE_RESERVED_BYTES 12

#define X86_BOOT_EDD_PARAMETERS_LENGTH_OFFSET 0
#define X86_BOOT_EDD_PARAMETERS_DPTE_POINTER_OFFSET 26

#define X86_BOOT_MEMORY_RANGE_BASE_OFFSET 0
#define X86_BOOT_MEMORY_RANGE_LENGTH_OFFSET 8
#define X86_BOOT_MEMORY_RANGE_TYPE_OFFSET 16
#define X86_BOOT_MEMORY_RANGE_ATTRIBUTES_OFFSET 20

#define X86_BOOT_INFO_MEMORY_MAP_PRESENT (1 << 0)
#define X86_BOOT_INFO_MEMORY_MAP_COMPLETE (1 << 1)
#define X86_BOOT_INFO_FLAG_MASK                                           \
	(X86_BOOT_INFO_MEMORY_MAP_PRESENT | X86_BOOT_INFO_MEMORY_MAP_COMPLETE)

#define X86_BOOT_MEMORY_USABLE 1
#define X86_BOOT_MEMORY_ENABLED (1 << 0)

#ifndef __ASSEMBLER__

#include "compiler.h"
#include "types.h"

struct x86_boot_memory_range {
	uint64_t base;
	uint64_t length;
	uint32_t type;
	uint32_t attributes;
} __packed;

struct x86_boot_platform_handoff {
	uint32_t flags;
	uint8_t rtc_century_bcd;
	uint8_t rtc_year_bcd;
	uint8_t rtc_month_bcd;
	uint8_t rtc_day_bcd;
	uint8_t rtc_hour_bcd;
	uint8_t rtc_minute_bcd;
	uint8_t rtc_second_bcd;
	uint8_t rtc_daylight;
	uint8_t display_active_dcc;
	uint8_t display_inactive_dcc;
	uint8_t reserved[X86_BOOT_PLATFORM_RESERVED_BYTES];
} __packed;

struct x86_boot_storage_handoff {
	uint32_t signature;
	uint16_t version;
	uint16_t bytes;
	uint32_t flags;
	uint16_t interface_support;
	uint8_t boot_drive;
	uint8_t edd_version;
	uint8_t parameters[X86_BOOT_EDD_PARAMETERS_BYTES];
	uint8_t dpte[X86_BOOT_EDD_DPTE_BYTES];
	uint8_t volume_start_lba[8];
	uint8_t volume_sector_count[8];
	uint16_t volume_sector_bytes;
	uint8_t reserved[X86_BOOT_STORAGE_RESERVED_BYTES];
} __packed;

struct x86_boot_info {
	uint32_t signature;
	uint16_t version;
	uint16_t header_bytes;
	uint16_t range_bytes;
	uint16_t range_count;
	uint32_t flags;
	struct x86_boot_platform_handoff platform;
	struct x86_boot_storage_handoff storage;
	struct x86_boot_memory_range ranges[X86_BOOT_MEMORY_RANGE_COUNT];
} __packed;

static_assert_expression(sizeof(struct x86_boot_platform_handoff) ==
				 X86_BOOT_PLATFORM_BYTES,
			 "BIOS boot-platform handoff layout changed");
static_assert_expression(
	__builtin_offsetof(struct x86_boot_platform_handoff, rtc_century_bcd) ==
		X86_BOOT_PLATFORM_RTC_CENTURY_BCD_OFFSET,
	"BIOS RTC handoff offset changed");
static_assert_expression(
	__builtin_offsetof(struct x86_boot_platform_handoff,
			   display_active_dcc) ==
		X86_BOOT_PLATFORM_DISPLAY_ACTIVE_DCC_OFFSET,
	"BIOS display handoff offset changed");
static_assert_expression(
	__builtin_offsetof(struct x86_boot_info, platform) ==
		X86_BOOT_INFO_PLATFORM_OFFSET,
	"BIOS boot-platform offset changed");

static_assert_expression(sizeof(struct x86_boot_storage_handoff) ==
				 X86_BOOT_STORAGE_BYTES,
			 "BIOS boot-storage handoff layout changed");
static_assert_expression(
	__builtin_offsetof(struct x86_boot_storage_handoff, parameters) ==
		X86_BOOT_STORAGE_PARAMETERS_OFFSET,
	"BIOS EDD parameter offset changed");
static_assert_expression(
	__builtin_offsetof(struct x86_boot_storage_handoff, dpte) ==
		X86_BOOT_STORAGE_DPTE_OFFSET,
	"BIOS EDD DPTE offset changed");
static_assert_expression(
	__builtin_offsetof(struct x86_boot_storage_handoff, volume_start_lba) ==
		X86_BOOT_STORAGE_VOLUME_START_LBA_OFFSET,
	"boot-volume start-LBA offset changed");
static_assert_expression(
	__builtin_offsetof(struct x86_boot_storage_handoff,
			     volume_sector_count) ==
		X86_BOOT_STORAGE_VOLUME_SECTOR_COUNT_OFFSET,
	"boot-volume sector-count offset changed");
static_assert_expression(
	__builtin_offsetof(struct x86_boot_storage_handoff,
			     volume_sector_bytes) ==
		X86_BOOT_STORAGE_VOLUME_SECTOR_BYTES_OFFSET,
	"boot-volume sector-size offset changed");

static_assert_expression(sizeof(struct x86_boot_memory_range) ==
				 X86_BOOT_MEMORY_RANGE_BYTES,
			 "BIOS memory-range layout changed");
static_assert_expression(
	__builtin_offsetof(struct x86_boot_memory_range, base) ==
		X86_BOOT_MEMORY_RANGE_BASE_OFFSET,
	"BIOS memory-range base offset changed");
static_assert_expression(
	__builtin_offsetof(struct x86_boot_memory_range, length) ==
		X86_BOOT_MEMORY_RANGE_LENGTH_OFFSET,
	"BIOS memory-range length offset changed");
static_assert_expression(
	__builtin_offsetof(struct x86_boot_memory_range, type) ==
		X86_BOOT_MEMORY_RANGE_TYPE_OFFSET,
	"BIOS memory-range type offset changed");
static_assert_expression(
	__builtin_offsetof(struct x86_boot_memory_range, attributes) ==
		X86_BOOT_MEMORY_RANGE_ATTRIBUTES_OFFSET,
	"BIOS memory-range attributes offset changed");
static_assert_expression(
	__builtin_offsetof(struct x86_boot_info, ranges) ==
		X86_BOOT_INFO_RANGES_OFFSET,
	"BIOS boot-info header layout changed");
static_assert_expression(
	sizeof(struct x86_boot_info) ==
		X86_BOOT_INFO_HEADER_BYTES +
			X86_BOOT_MEMORY_RANGE_COUNT *
				X86_BOOT_MEMORY_RANGE_BYTES,
	"BIOS boot-info capacity changed");

#endif

#endif

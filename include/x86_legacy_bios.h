/* SPDX-License-Identifier: GPL-2.0-only */
/* Validated, immutable legacy-BIOS platform facts captured at boot. */
#ifndef DOSC32_X86_LEGACY_BIOS_H
#define DOSC32_X86_LEGACY_BIOS_H

#include "compiler.h"
#include "types.h"
#include "x86_boot_info.h"
#include "x86_boot_storage.h"

/* INT 13h DL numbering is architectural; discovered devices are not. */
#define X86_BIOS_FIRST_FIXED_DISK 0x80u
#define X86_BIOS_FLOPPY_DRIVE_COUNT 2u

/* Architectural location and extent of the PC BIOS data area. */
#define X86_BIOS_DATA_AREA_ADDRESS 0x00000400u
#define X86_BIOS_DATA_AREA_BYTES 0x100u
#define X86_BIOS_SERIAL_PORT_COUNT 4u

/* Capabilities derived from BIOS Display Combination Code, never assumed. */
#define X86_LEGACY_DISPLAY_MONO_TEXT_MEMORY (1u << 0)
#define X86_LEGACY_DISPLAY_COLOR_TEXT_MEMORY (1u << 1)
#define X86_LEGACY_DISPLAY_GRAPHICS_MEMORY (1u << 2)
#define X86_LEGACY_DISPLAY_MONO_CRTC_IO (1u << 3)
#define X86_LEGACY_DISPLAY_COLOR_CRTC_IO (1u << 4)
#define X86_LEGACY_DISPLAY_CONTROL_IO (1u << 5)
#define X86_LEGACY_DISPLAY_EGA_COMPATIBLE (1u << 6)
#define X86_LEGACY_DISPLAY_VGA_COMPATIBLE (1u << 7)
#define X86_LEGACY_DISPLAY_MCGA_COMPATIBLE (1u << 8)

enum x86_legacy_bios_status {
	X86_LEGACY_BIOS_OK = 0,
	X86_LEGACY_BIOS_INVALID_ARGUMENT,
	X86_LEGACY_BIOS_INVALID_STATE,
	X86_LEGACY_BIOS_INVALID_CONVENTIONAL_MEMORY,
	X86_LEGACY_BIOS_INVALID_HANDOFF
};

struct x86_legacy_bios_snapshot {
	uint64_t generation;
	uint32_t text_memory_address;
	uint32_t text_memory_bytes;
	uint16_t conventional_kib;
	uint16_t text_columns;
	uint16_t text_rows;
	uint16_t text_page_offset;
	uint16_t crtc_index_port;
	uint16_t serial_ports[X86_BIOS_SERIAL_PORT_COUNT];
	uint8_t boot_drive;
	uint8_t video_mode;
	uint8_t active_video_page;
	uint8_t text_console_available;
	uint8_t reserved0[2];
	uint16_t rtc_year;
	uint16_t display_capabilities;
	uint8_t rtc_second;
	uint8_t rtc_minute;
	uint8_t rtc_hour;
	uint8_t rtc_weekday;
	uint8_t rtc_day;
	uint8_t rtc_month;
	uint8_t rtc_daylight;
	uint8_t rtc_valid;
	uint8_t display_active_dcc;
	uint8_t display_inactive_dcc;
	uint8_t display_dcc_valid;
	uint8_t reserved1;
	uint32_t boot_storage_status;
	struct x86_boot_device_locator boot_device;
} __aligned(8);

/* Pure decoder used by the boot owner and host tests. */
enum x86_legacy_bios_status x86_legacy_bios_decode(
	uint8_t boot_drive, const uint8_t *bios_data, size_t bios_data_bytes,
	struct x86_legacy_bios_snapshot *snapshot) __must_check;
enum x86_legacy_bios_status x86_legacy_bios_apply_boot_info(
	const struct x86_boot_info *boot_info,
	struct x86_legacy_bios_snapshot *snapshot) __must_check;

/* Captures the live BDA exactly once before any DOS program may mutate it. */
enum x86_legacy_bios_status x86_legacy_bios_initialize(
	uint8_t boot_drive, const struct x86_boot_info *boot_info) __must_check;
bool x86_legacy_bios_snapshot(
	struct x86_legacy_bios_snapshot *snapshot) __must_check;

static_assert_expression(sizeof(struct x86_legacy_bios_snapshot) == 112u,
			 "legacy-BIOS snapshot layout changed");

#endif

// SPDX-License-Identifier: GPL-2.0-only
/*
 * Immutable legacy-BIOS discovery owner.
 *
 * The BDA offsets and VGA text apertures below are PC architectural
 * contracts.  Columns, rows, active page, serial bases, boot drive, and
 * conventional-memory size are firmware observations and remain runtime data.
 */
#include "x86_legacy_bios.h"

#define BDA_SERIAL_PORTS_OFFSET 0x00u
#define BDA_CONVENTIONAL_KIB_OFFSET 0x13u
#define BDA_VIDEO_MODE_OFFSET 0x49u
#define BDA_VIDEO_COLUMNS_OFFSET 0x4au
#define BDA_VIDEO_PAGE_OFFSET 0x4eu
#define BDA_ACTIVE_VIDEO_PAGE_OFFSET 0x62u
#define BDA_CRTC_INDEX_PORT_OFFSET 0x63u
#define BDA_VIDEO_ROWS_MINUS_ONE_OFFSET 0x84u

#define PC_CONVENTIONAL_KIB_MAXIMUM 640u
#define VGA_COLOR_TEXT_MEMORY 0x000b8000u
#define VGA_MONO_TEXT_MEMORY 0x000b0000u
#define VGA_TEXT_APERTURE_BYTES 0x00008000u
#define VGA_COLOR_CRTC_INDEX_PORT 0x03d4u
#define VGA_MONO_CRTC_INDEX_PORT 0x03b4u
#define VGA_TEXT_CELL_BYTES 2u
#define VGA_TEXT_COLUMN_MINIMUM 20u
#define VGA_TEXT_COLUMN_MAXIMUM 160u
#define VGA_TEXT_ROW_MINIMUM 10u
#define VGA_TEXT_ROW_MAXIMUM 100u
#define X86_LEGACY_BIOS_FIRST_GENERATION 1u

struct x86_legacy_bios_owner {
	struct x86_legacy_bios_snapshot snapshot;
	uint8_t initialized;
	uint8_t reserved[7];
};

static struct x86_legacy_bios_owner owner;

static uint16_t read_le16(const uint8_t *bytes)
{
	return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static bool bytes_are_zero(const uint8_t *bytes, size_t count)
{
	size_t index;

	for (index = 0u; index < count; ++index) {
		if (bytes[index] != 0u)
			return false;
	}
	return true;
}

static bool bcd_to_u8(uint8_t encoded, uint8_t maximum, uint8_t *decoded)
{
	uint8_t value;

	if (decoded == NULL || (encoded & 0x0fu) > 9u ||
	    (encoded >> 4u) > 9u)
		return false;
	value = (uint8_t)((encoded >> 4u) * 10u + (encoded & 0x0fu));
	if (value > maximum)
		return false;
	*decoded = value;
	return true;
}

static bool year_is_leap(uint16_t year)
{
	return (year % 4u) == 0u &&
	       ((year % 100u) != 0u || (year % 400u) == 0u);
}

static uint8_t days_in_month(uint16_t year, uint8_t month)
{
	static const uint8_t month_days[12] = {
		31u, 28u, 31u, 30u, 31u, 30u,
		31u, 31u, 30u, 31u, 30u, 31u,
	};

	if (month == 0u || month > ARRAY_SIZE(month_days))
		return 0u;
	if (month == 2u && year_is_leap(year))
		return 29u;
	return month_days[month - 1u];
}

static uint8_t weekday_from_date(uint16_t year, uint8_t month, uint8_t day)
{
	static const uint8_t month_offsets[12] = {
		0u, 3u, 2u, 5u, 0u, 3u, 5u, 1u, 4u, 6u, 2u, 4u,
	};
	uint32_t adjusted_year = year;
	uint32_t weekday;

	if (month < 3u)
		--adjusted_year;
	weekday = adjusted_year + adjusted_year / 4u -
		  adjusted_year / 100u + adjusted_year / 400u +
		  month_offsets[month - 1u] + day;
	/* BIOS/CMOS convention consumed by the guest: Sunday=1, Saturday=7. */
	return (uint8_t)(weekday % 7u + 1u);
}

static uint16_t display_capabilities(uint8_t dcc)
{
	const uint16_t mono = X86_LEGACY_DISPLAY_MONO_TEXT_MEMORY |
			      X86_LEGACY_DISPLAY_MONO_CRTC_IO;
	const uint16_t color = X86_LEGACY_DISPLAY_COLOR_TEXT_MEMORY |
			       X86_LEGACY_DISPLAY_COLOR_CRTC_IO;
	const uint16_t switched = mono | color |
		X86_LEGACY_DISPLAY_GRAPHICS_MEMORY |
		X86_LEGACY_DISPLAY_CONTROL_IO;

	switch (dcc) {
	case 0x01u:
		return mono;
	case 0x02u:
		return color | X86_LEGACY_DISPLAY_GRAPHICS_MEMORY;
	case 0x04u:
	case 0x05u:
		return switched | X86_LEGACY_DISPLAY_EGA_COMPATIBLE;
	case 0x07u:
	case 0x08u:
		return switched | X86_LEGACY_DISPLAY_EGA_COMPATIBLE |
		       X86_LEGACY_DISPLAY_VGA_COMPATIBLE;
	case 0x0au:
	case 0x0bu:
	case 0x0cu:
		return color | X86_LEGACY_DISPLAY_GRAPHICS_MEMORY |
		       X86_LEGACY_DISPLAY_CONTROL_IO |
		       X86_LEGACY_DISPLAY_MCGA_COMPATIBLE;
	default:
		return 0u;
	}
}

enum x86_legacy_bios_status x86_legacy_bios_apply_boot_info(
	const struct x86_boot_info *boot_info,
	struct x86_legacy_bios_snapshot *snapshot)
{
	const struct x86_boot_platform_handoff *platform;
	struct x86_legacy_bios_snapshot prepared;
	uint8_t century;
	uint8_t year_low;
	uint8_t month;
	uint8_t day;
	uint8_t hour;
	uint8_t minute;
	uint8_t second;
	uint16_t year;

	if (boot_info == NULL || snapshot == NULL)
		return X86_LEGACY_BIOS_INVALID_ARGUMENT;
	if (
	    boot_info->signature != X86_BOOT_INFO_SIGNATURE ||
	    boot_info->version != X86_BOOT_INFO_VERSION ||
	    boot_info->header_bytes != X86_BOOT_INFO_HEADER_BYTES ||
	    boot_info->range_bytes != X86_BOOT_MEMORY_RANGE_BYTES)
		return X86_LEGACY_BIOS_INVALID_HANDOFF;
	platform = &boot_info->platform;
	if ((platform->flags & (uint32_t)~X86_BOOT_PLATFORM_FLAG_MASK) != 0u ||
	    !bytes_are_zero(platform->reserved,
			    ARRAY_SIZE(platform->reserved)))
		return X86_LEGACY_BIOS_INVALID_HANDOFF;
	prepared = *snapshot;
	prepared.rtc_year = 0u;
	prepared.display_capabilities = 0u;
	prepared.rtc_second = 0u;
	prepared.rtc_minute = 0u;
	prepared.rtc_hour = 0u;
	prepared.rtc_weekday = 0u;
	prepared.rtc_day = 0u;
	prepared.rtc_month = 0u;
	prepared.rtc_daylight = 0u;
	prepared.rtc_valid = 0u;
	prepared.display_active_dcc = 0u;
	prepared.display_inactive_dcc = 0u;
	prepared.display_dcc_valid = 0u;
	prepared.reserved1 = 0u;
	if ((platform->flags & X86_BOOT_PLATFORM_DISPLAY_DCC_PRESENT) != 0u) {
		prepared.display_active_dcc = platform->display_active_dcc;
		prepared.display_inactive_dcc = platform->display_inactive_dcc;
		prepared.display_capabilities =
			display_capabilities(platform->display_active_dcc);
		prepared.display_dcc_valid =
			(uint8_t)(prepared.display_capabilities != 0u);
	}
	if ((platform->flags & X86_BOOT_PLATFORM_RTC_PRESENT) == 0u ||
	    platform->rtc_daylight > 1u ||
	    !bcd_to_u8(platform->rtc_century_bcd, 99u, &century) ||
	    !bcd_to_u8(platform->rtc_year_bcd, 99u, &year_low) ||
	    !bcd_to_u8(platform->rtc_month_bcd, 12u, &month) || month == 0u ||
	    !bcd_to_u8(platform->rtc_day_bcd, 31u, &day) || day == 0u ||
	    !bcd_to_u8(platform->rtc_hour_bcd, 23u, &hour) ||
	    !bcd_to_u8(platform->rtc_minute_bcd, 59u, &minute) ||
	    !bcd_to_u8(platform->rtc_second_bcd, 59u, &second))
		goto publish;
	year = (uint16_t)((uint16_t)century * 100u + year_low);
	if (year == 0u || day > days_in_month(year, month))
		goto publish;
	prepared.rtc_year = year;
	prepared.rtc_second = second;
	prepared.rtc_minute = minute;
	prepared.rtc_hour = hour;
	prepared.rtc_weekday = weekday_from_date(year, month, day);
	prepared.rtc_day = day;
	prepared.rtc_month = month;
	prepared.rtc_daylight = platform->rtc_daylight;
	prepared.rtc_valid = 1u;
publish:
	*snapshot = prepared;
	return X86_LEGACY_BIOS_OK;
}

static bool serial_port_is_safe(uint16_t port)
{
	/* Firmware data is observed before guest entry, but it still must not
	 * authorize arbitrary native I/O.  These are the four standard PC UART
	 * apertures and each has the eight registers used by the console driver. */
	return port == 0x03f8u || port == 0x02f8u || port == 0x03e8u ||
	       port == 0x02e8u;
}

static bool video_mode_is_color_text(uint8_t mode)
{
	return mode <= 3u;
}

static bool video_mode_is_mono_text(uint8_t mode)
{
	return mode == 7u;
}

static void decode_serial_ports(const uint8_t *bios_data,
				struct x86_legacy_bios_snapshot *snapshot)
{
	size_t index;

	for (index = 0u; index < X86_BIOS_SERIAL_PORT_COUNT; ++index) {
		uint16_t port = read_le16(
			bios_data + BDA_SERIAL_PORTS_OFFSET + index * 2u);

		snapshot->serial_ports[index] =
			serial_port_is_safe(port) ? port : 0u;
	}
}

static void decode_text_console(const uint8_t *bios_data,
				struct x86_legacy_bios_snapshot *snapshot)
{
	uint32_t memory_base;
	uint32_t screen_bytes;
	uint16_t expected_crtc;
	uint16_t columns =
		read_le16(bios_data + BDA_VIDEO_COLUMNS_OFFSET);
	uint16_t rows =
		(uint16_t)bios_data[BDA_VIDEO_ROWS_MINUS_ONE_OFFSET] + 1u;
	uint16_t page_offset =
		read_le16(bios_data + BDA_VIDEO_PAGE_OFFSET);
	uint16_t crtc =
		read_le16(bios_data + BDA_CRTC_INDEX_PORT_OFFSET);
	uint8_t mode = bios_data[BDA_VIDEO_MODE_OFFSET];

	snapshot->video_mode = mode;
	snapshot->active_video_page =
		bios_data[BDA_ACTIVE_VIDEO_PAGE_OFFSET];
	if (video_mode_is_color_text(mode)) {
		memory_base = VGA_COLOR_TEXT_MEMORY;
		expected_crtc = VGA_COLOR_CRTC_INDEX_PORT;
	} else if (video_mode_is_mono_text(mode)) {
		memory_base = VGA_MONO_TEXT_MEMORY;
		expected_crtc = VGA_MONO_CRTC_INDEX_PORT;
	} else {
		return;
	}
	if (columns < VGA_TEXT_COLUMN_MINIMUM ||
	    columns > VGA_TEXT_COLUMN_MAXIMUM ||
	    rows < VGA_TEXT_ROW_MINIMUM || rows > VGA_TEXT_ROW_MAXIMUM ||
	    (page_offset & 1u) != 0u || crtc != expected_crtc)
		return;
	screen_bytes = (uint32_t)columns * (uint32_t)rows *
		       VGA_TEXT_CELL_BYTES;
	if ((uint32_t)page_offset > VGA_TEXT_APERTURE_BYTES ||
	    screen_bytes > VGA_TEXT_APERTURE_BYTES - (uint32_t)page_offset)
		return;
	snapshot->text_memory_address = memory_base + page_offset;
	snapshot->text_memory_bytes = screen_bytes;
	snapshot->text_columns = columns;
	snapshot->text_rows = rows;
	snapshot->text_page_offset = page_offset;
	snapshot->crtc_index_port = crtc;
	snapshot->text_console_available = 1u;
}

enum x86_legacy_bios_status x86_legacy_bios_decode(
	uint8_t boot_drive, const uint8_t *bios_data, size_t bios_data_bytes,
	struct x86_legacy_bios_snapshot *snapshot)
{
	struct x86_legacy_bios_snapshot prepared = {0};
	uint16_t conventional_kib;

	if (bios_data == NULL || snapshot == NULL ||
	    bios_data_bytes < X86_BIOS_DATA_AREA_BYTES)
		return X86_LEGACY_BIOS_INVALID_ARGUMENT;
	conventional_kib =
		read_le16(bios_data + BDA_CONVENTIONAL_KIB_OFFSET);
	if (conventional_kib == 0u ||
	    conventional_kib > PC_CONVENTIONAL_KIB_MAXIMUM)
		return X86_LEGACY_BIOS_INVALID_CONVENTIONAL_MEMORY;
	prepared.generation = X86_LEGACY_BIOS_FIRST_GENERATION;
	prepared.conventional_kib = conventional_kib;
	prepared.boot_drive = boot_drive;
	prepared.boot_storage_status = X86_BOOT_STORAGE_NOT_AVAILABLE;
	decode_serial_ports(bios_data, &prepared);
	decode_text_console(bios_data, &prepared);
	*snapshot = prepared;
	return X86_LEGACY_BIOS_OK;
}

enum x86_legacy_bios_status x86_legacy_bios_initialize(
	uint8_t boot_drive, const struct x86_boot_info *boot_info)
{
	const uint8_t *bios_data =
		(const uint8_t *)(uintptr_t)X86_BIOS_DATA_AREA_ADDRESS;
	struct x86_legacy_bios_snapshot prepared;
	enum x86_legacy_bios_status status;

	if (owner.initialized != 0u)
		return X86_LEGACY_BIOS_INVALID_STATE;
	status = x86_legacy_bios_decode(boot_drive, bios_data,
					X86_BIOS_DATA_AREA_BYTES, &prepared);
	if (status != X86_LEGACY_BIOS_OK)
		return status;
	status = x86_legacy_bios_apply_boot_info(boot_info, &prepared);
	if (status != X86_LEGACY_BIOS_OK) {
		/* Optional facts fail closed; BDA and storage remain independently
		 * useful and are validated by their own owners. */
		prepared.rtc_valid = 0u;
		prepared.display_dcc_valid = 0u;
		prepared.display_capabilities = 0u;
	}
	prepared.boot_storage_status = (uint32_t)x86_boot_storage_decode(
		boot_info, boot_drive, &prepared.boot_device);
	owner.snapshot = prepared;
	owner.initialized = 1u;
	return X86_LEGACY_BIOS_OK;
}

bool x86_legacy_bios_snapshot(struct x86_legacy_bios_snapshot *snapshot)
{
	if (snapshot == NULL || owner.initialized != 1u)
		return false;
	*snapshot = owner.snapshot;
	return true;
}

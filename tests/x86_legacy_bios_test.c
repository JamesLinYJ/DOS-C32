// SPDX-License-Identifier: GPL-2.0-only
#include "x86_legacy_bios.h"

#define TEST_BDA_CONVENTIONAL_KIB_OFFSET 0x13u
#define TEST_BDA_VIDEO_MODE_OFFSET 0x49u
#define TEST_BDA_VIDEO_COLUMNS_OFFSET 0x4au
#define TEST_BDA_VIDEO_PAGE_OFFSET 0x4eu
#define TEST_BDA_ACTIVE_VIDEO_PAGE_OFFSET 0x62u
#define TEST_BDA_CRTC_INDEX_PORT_OFFSET 0x63u
#define TEST_BDA_VIDEO_ROWS_MINUS_ONE_OFFSET 0x84u

static void write_le16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
}

static void prepare_color_text_bda(uint8_t *bios_data)
{
	size_t index;

	for (index = 0u; index < X86_BIOS_DATA_AREA_BYTES; ++index)
		bios_data[index] = 0u;
	write_le16(bios_data + 0u, 0x03f8u);
	write_le16(bios_data + 2u, 0x02f8u);
	write_le16(bios_data + TEST_BDA_CONVENTIONAL_KIB_OFFSET, 640u);
	bios_data[TEST_BDA_VIDEO_MODE_OFFSET] = 3u;
	write_le16(bios_data + TEST_BDA_VIDEO_COLUMNS_OFFSET, 80u);
	write_le16(bios_data + TEST_BDA_VIDEO_PAGE_OFFSET, 0u);
	bios_data[TEST_BDA_ACTIVE_VIDEO_PAGE_OFFSET] = 0u;
	write_le16(bios_data + TEST_BDA_CRTC_INDEX_PORT_OFFSET, 0x03d4u);
	bios_data[TEST_BDA_VIDEO_ROWS_MINUS_ONE_OFFSET] = 24u;
}

static struct x86_boot_info boot_info_header(void)
{
	struct x86_boot_info boot_info = {0};

	boot_info.signature = X86_BOOT_INFO_SIGNATURE;
	boot_info.version = X86_BOOT_INFO_VERSION;
	boot_info.header_bytes = X86_BOOT_INFO_HEADER_BYTES;
	boot_info.range_bytes = X86_BOOT_MEMORY_RANGE_BYTES;
	return boot_info;
}

static int test_valid_runtime_values(void)
{
	uint8_t bios_data[X86_BIOS_DATA_AREA_BYTES];
	struct x86_legacy_bios_snapshot snapshot;

	prepare_color_text_bda(bios_data);
	if (x86_legacy_bios_decode(0x81u, bios_data, sizeof(bios_data),
				   &snapshot) != X86_LEGACY_BIOS_OK)
		return 1;
	return snapshot.boot_drive == 0x81u &&
	       snapshot.conventional_kib == 640u &&
	       snapshot.serial_ports[0] == 0x03f8u &&
	       snapshot.serial_ports[1] == 0x02f8u &&
	       snapshot.text_console_available == 1u &&
	       snapshot.text_memory_address == 0x000b8000u &&
	       snapshot.text_memory_bytes == 80u * 25u * 2u &&
	       snapshot.text_columns == 80u && snapshot.text_rows == 25u &&
	       snapshot.crtc_index_port == 0x03d4u
		       ? 0
		       : 1;
}

static int test_untrusted_ports_are_not_authorized(void)
{
	uint8_t bios_data[X86_BIOS_DATA_AREA_BYTES];
	struct x86_legacy_bios_snapshot snapshot;

	prepare_color_text_bda(bios_data);
	write_le16(bios_data + 0u, 0x1234u);
	if (x86_legacy_bios_decode(0x80u, bios_data, sizeof(bios_data),
				   &snapshot) != X86_LEGACY_BIOS_OK)
		return 1;
	return snapshot.serial_ports[0] == 0u ? 0 : 1;
}

static int test_bad_video_does_not_destroy_other_facts(void)
{
	uint8_t bios_data[X86_BIOS_DATA_AREA_BYTES];
	struct x86_legacy_bios_snapshot snapshot;

	prepare_color_text_bda(bios_data);
	bios_data[TEST_BDA_VIDEO_MODE_OFFSET] = 0x13u;
	if (x86_legacy_bios_decode(0x80u, bios_data, sizeof(bios_data),
				   &snapshot) != X86_LEGACY_BIOS_OK)
		return 1;
	return snapshot.conventional_kib == 640u &&
	       snapshot.text_console_available == 0u
		       ? 0
		       : 1;
}

static int test_invalid_memory_is_rejected_atomically(void)
{
	uint8_t bios_data[X86_BIOS_DATA_AREA_BYTES];
	struct x86_legacy_bios_snapshot snapshot = {
		.generation = 99u,
	};

	prepare_color_text_bda(bios_data);
	write_le16(bios_data + TEST_BDA_CONVENTIONAL_KIB_OFFSET, 0u);
	if (x86_legacy_bios_decode(0x80u, bios_data, sizeof(bios_data),
				   &snapshot) !=
	    X86_LEGACY_BIOS_INVALID_CONVENTIONAL_MEMORY)
		return 1;
	return snapshot.generation == 99u ? 0 : 1;
}

static int test_runtime_rtc_and_vga_are_decoded(void)
{
	uint8_t bios_data[X86_BIOS_DATA_AREA_BYTES];
	struct x86_boot_info boot_info = boot_info_header();
	struct x86_legacy_bios_snapshot snapshot;
	uint16_t expected_display = X86_LEGACY_DISPLAY_MONO_TEXT_MEMORY |
		X86_LEGACY_DISPLAY_COLOR_TEXT_MEMORY |
		X86_LEGACY_DISPLAY_GRAPHICS_MEMORY |
		X86_LEGACY_DISPLAY_MONO_CRTC_IO |
		X86_LEGACY_DISPLAY_COLOR_CRTC_IO |
		X86_LEGACY_DISPLAY_CONTROL_IO |
		X86_LEGACY_DISPLAY_EGA_COMPATIBLE |
		X86_LEGACY_DISPLAY_VGA_COMPATIBLE;

	prepare_color_text_bda(bios_data);
	if (x86_legacy_bios_decode(0x80u, bios_data, sizeof(bios_data),
				   &snapshot) != X86_LEGACY_BIOS_OK)
		return 1;
	boot_info.platform.flags = X86_BOOT_PLATFORM_RTC_PRESENT |
				   X86_BOOT_PLATFORM_DISPLAY_DCC_PRESENT;
	boot_info.platform.rtc_century_bcd = 0x20u;
	boot_info.platform.rtc_year_bcd = 0x00u;
	boot_info.platform.rtc_month_bcd = 0x01u;
	boot_info.platform.rtc_day_bcd = 0x01u;
	boot_info.platform.rtc_hour_bcd = 0x23u;
	boot_info.platform.rtc_minute_bcd = 0x59u;
	boot_info.platform.rtc_second_bcd = 0x58u;
	boot_info.platform.rtc_daylight = 0u;
	boot_info.platform.display_active_dcc = 0x08u;
	boot_info.platform.display_inactive_dcc = 0x00u;
	if (x86_legacy_bios_apply_boot_info(&boot_info, &snapshot) !=
	    X86_LEGACY_BIOS_OK)
		return 1;
	return snapshot.rtc_valid == 1u && snapshot.rtc_year == 2000u &&
	       snapshot.rtc_month == 1u && snapshot.rtc_day == 1u &&
	       snapshot.rtc_weekday == 7u && snapshot.rtc_hour == 23u &&
	       snapshot.rtc_minute == 59u && snapshot.rtc_second == 58u &&
	       snapshot.display_dcc_valid == 1u &&
	       snapshot.display_active_dcc == 0x08u &&
	       snapshot.display_capabilities == expected_display
		       ? 0
		       : 1;
}

static int test_bad_rtc_does_not_destroy_display_fact(void)
{
	struct x86_boot_info boot_info = boot_info_header();
	struct x86_legacy_bios_snapshot snapshot = {0};

	boot_info.platform.flags = X86_BOOT_PLATFORM_RTC_PRESENT |
				   X86_BOOT_PLATFORM_DISPLAY_DCC_PRESENT;
	boot_info.platform.rtc_century_bcd = 0x20u;
	boot_info.platform.rtc_year_bcd = 0x26u;
	boot_info.platform.rtc_month_bcd = 0x13u;
	boot_info.platform.rtc_day_bcd = 0x01u;
	boot_info.platform.display_active_dcc = 0x02u;
	if (x86_legacy_bios_apply_boot_info(&boot_info, &snapshot) !=
	    X86_LEGACY_BIOS_OK)
		return 1;
	return snapshot.rtc_valid == 0u &&
	       snapshot.display_dcc_valid == 1u &&
	       snapshot.display_capabilities ==
		       (X86_LEGACY_DISPLAY_COLOR_TEXT_MEMORY |
			X86_LEGACY_DISPLAY_COLOR_CRTC_IO |
			X86_LEGACY_DISPLAY_GRAPHICS_MEMORY)
		       ? 0
		       : 1;
}

static int test_mcga_display_codes_keep_adapter_capabilities(void)
{
	struct x86_boot_info boot_info = boot_info_header();
	struct x86_legacy_bios_snapshot snapshot = {0};
	uint16_t expected_display = X86_LEGACY_DISPLAY_COLOR_TEXT_MEMORY |
		X86_LEGACY_DISPLAY_GRAPHICS_MEMORY |
		X86_LEGACY_DISPLAY_COLOR_CRTC_IO |
		X86_LEGACY_DISPLAY_CONTROL_IO |
		X86_LEGACY_DISPLAY_MCGA_COMPATIBLE;

	boot_info.platform.flags = X86_BOOT_PLATFORM_DISPLAY_DCC_PRESENT;
	boot_info.platform.display_active_dcc = 0x0au;
	if (x86_legacy_bios_apply_boot_info(&boot_info, &snapshot) !=
	    X86_LEGACY_BIOS_OK)
		return 1;
	return snapshot.display_dcc_valid == 1u &&
	       snapshot.display_active_dcc == 0x0au &&
	       snapshot.display_capabilities == expected_display
		       ? 0
		       : 1;
}

static int test_unknown_dcc_authorizes_nothing(void)
{
	struct x86_boot_info boot_info = boot_info_header();
	struct x86_legacy_bios_snapshot snapshot = {0};

	boot_info.platform.flags = X86_BOOT_PLATFORM_DISPLAY_DCC_PRESENT;
	boot_info.platform.display_active_dcc = 0x7fu;
	if (x86_legacy_bios_apply_boot_info(&boot_info, &snapshot) !=
	    X86_LEGACY_BIOS_OK)
		return 1;
	return snapshot.display_active_dcc == 0x7fu &&
	       snapshot.display_dcc_valid == 0u &&
	       snapshot.display_capabilities == 0u
		       ? 0
		       : 1;
}

static int test_malformed_platform_is_rejected_atomically(void)
{
	struct x86_boot_info boot_info = boot_info_header();
	struct x86_legacy_bios_snapshot snapshot = {
		.rtc_year = 1999u,
		.rtc_valid = 1u,
		.display_capabilities = 0x5a5au,
	};

	boot_info.platform.flags = X86_BOOT_PLATFORM_FLAG_MASK | (1u << 31);
	if (x86_legacy_bios_apply_boot_info(&boot_info, &snapshot) !=
	    X86_LEGACY_BIOS_INVALID_HANDOFF)
		return 1;
	return snapshot.rtc_year == 1999u && snapshot.rtc_valid == 1u &&
	       snapshot.display_capabilities == 0x5a5au
		       ? 0
		       : 1;
}

int main(void)
{
	return test_valid_runtime_values() ||
	       test_untrusted_ports_are_not_authorized() ||
	       test_bad_video_does_not_destroy_other_facts() ||
	       test_invalid_memory_is_rejected_atomically() ||
	       test_runtime_rtc_and_vga_are_decoded() ||
	       test_bad_rtc_does_not_destroy_display_fact() ||
	       test_mcga_display_codes_keep_adapter_capabilities() ||
	       test_unknown_dcc_authorizes_nothing() ||
	       test_malformed_platform_is_rejected_atomically();
}

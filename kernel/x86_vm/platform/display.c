// SPDX-License-Identifier: GPL-2.0-only
/*
 * Legacy display capability owner.
 *
 * Compatibility contract: BIOS INT 10h AX=1A00h DCC identifies the active adapter.
 * Safety changes: immutable typed ranges; unknown DCCs authorize nothing.
 */
#include "x86_display.h"

#define MDA_MEMORY_BASE 0x000b0000u
#define CGA_MEMORY_BASE 0x000b8000u
#define TEXT_MEMORY_BYTES 0x00008000u
#define PLANAR_MEMORY_BASE 0x000a0000u
#define MCGA_GRAPHICS_MEMORY_BYTES 0x00010000u
#define EGA_VGA_MEMORY_BYTES 0x00020000u

#define MDA_IO_FIRST 0x03b0u
#define MDA_IO_LAST 0x03bfu
#define DISPLAY_CONTROL_IO_FIRST 0x03c0u
#define DISPLAY_CONTROL_IO_LAST 0x03cfu
#define CGA_IO_FIRST 0x03d0u
#define CGA_IO_LAST 0x03dfu

static bool reserved_is_zero(const uint8_t *bytes, size_t count)
{
	size_t index;

	for (index = 0u; index < count; ++index) {
		if (bytes[index] != 0u)
			return false;
	}
	return true;
}

static bool text_console_matches(
	const struct x86_legacy_bios_snapshot *platform,
	const struct x86_display_capability *capability)
{
	uint32_t text_end;
	uint32_t range_end;
	size_t index;

	if (platform->text_console_available == 0u)
		return true;
	if (platform->text_memory_bytes == 0u ||
	    platform->text_memory_address > 0xffffffffu -
					 platform->text_memory_bytes)
		return false;
	text_end = platform->text_memory_address + platform->text_memory_bytes;
	for (index = 0u; index < capability->memory_range_count; ++index) {
		const struct x86_display_memory_range *range =
			&capability->memory[index];

		range_end = range->base + range->bytes;
		if (platform->text_memory_address >= range->base &&
		    text_end <= range_end)
			return true;
	}
	return false;
}

static bool crtc_matches(const struct x86_legacy_bios_snapshot *platform,
			 const struct x86_display_capability *capability)
{
	if (platform->text_console_available == 0u)
		return true;
	return platform->crtc_index_port >= capability->io_first_port &&
	       platform->crtc_index_port <= capability->io_last_port;
}

static uint16_t expected_capabilities(uint8_t dcc)
{
	const uint16_t mono = X86_LEGACY_DISPLAY_MONO_TEXT_MEMORY |
		X86_LEGACY_DISPLAY_MONO_CRTC_IO;
	const uint16_t color = X86_LEGACY_DISPLAY_COLOR_TEXT_MEMORY |
		X86_LEGACY_DISPLAY_COLOR_CRTC_IO;
	const uint16_t graphics = X86_LEGACY_DISPLAY_GRAPHICS_MEMORY;
	const uint16_t control = X86_LEGACY_DISPLAY_CONTROL_IO;
	const uint16_t switched = mono | color | graphics | control;

	switch (dcc) {
	case 0x01u:
		return mono;
	case 0x02u:
		return color | graphics;
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
		return color | graphics | control |
		       X86_LEGACY_DISPLAY_MCGA_COMPATIBLE;
	default:
		return 0u;
	}
}

static bool prepare_ranges(uint8_t dcc,
			   struct x86_display_capability *capability)
{
	switch (dcc) {
	case 0x01u:
		capability->display_class = (uint8_t)X86_DISPLAY_MDA;
		capability->memory[0] = (struct x86_display_memory_range){
			.base = MDA_MEMORY_BASE,
			.bytes = TEXT_MEMORY_BYTES,
		};
		capability->memory_range_count = 1u;
		capability->io_first_port = MDA_IO_FIRST;
		capability->io_last_port = MDA_IO_LAST;
		return true;
	case 0x02u:
		capability->display_class = (uint8_t)X86_DISPLAY_CGA;
		capability->memory[0] = (struct x86_display_memory_range){
			.base = CGA_MEMORY_BASE,
			.bytes = TEXT_MEMORY_BYTES,
		};
		capability->memory_range_count = 1u;
		capability->io_first_port = CGA_IO_FIRST;
		capability->io_last_port = CGA_IO_LAST;
		return true;
	case 0x04u:
	case 0x05u:
		capability->display_class = (uint8_t)X86_DISPLAY_EGA;
		break;
	case 0x07u:
	case 0x08u:
		capability->display_class = (uint8_t)X86_DISPLAY_VGA;
		break;
	case 0x0au:
	case 0x0bu:
	case 0x0cu:
		capability->display_class = (uint8_t)X86_DISPLAY_MCGA;
		capability->memory[0] = (struct x86_display_memory_range){
			.base = PLANAR_MEMORY_BASE,
			.bytes = MCGA_GRAPHICS_MEMORY_BYTES,
		};
		capability->memory[1] = (struct x86_display_memory_range){
			.base = CGA_MEMORY_BASE,
			.bytes = TEXT_MEMORY_BYTES,
		};
		capability->memory_range_count = 2u;
		capability->io_first_port = DISPLAY_CONTROL_IO_FIRST;
		capability->io_last_port = CGA_IO_LAST;
		return true;
	default:
		return false;
	}
	capability->memory[0] = (struct x86_display_memory_range){
		.base = PLANAR_MEMORY_BASE,
		.bytes = EGA_VGA_MEMORY_BYTES,
	};
	capability->memory_range_count = 1u;
	capability->io_first_port = MDA_IO_FIRST;
	capability->io_last_port = CGA_IO_LAST;
	return true;
}

enum x86_display_status x86_display_capability_prepare(
	const struct x86_legacy_bios_snapshot *platform,
	struct x86_display_capability *capability)
{
	struct x86_display_capability prepared = {0};
	uint16_t expected;

	if (platform == NULL || capability == NULL)
		return X86_DISPLAY_INVALID_ARGUMENT;
	if (platform->generation == 0u || platform->display_dcc_valid > 1u ||
	    !reserved_is_zero(platform->reserved0,
			      ARRAY_SIZE(platform->reserved0)) ||
	    platform->reserved1 != 0u)
		return X86_DISPLAY_INVALID_PLATFORM;
	if (platform->display_dcc_valid == 0u)
		return X86_DISPLAY_UNSUPPORTED;
	expected = expected_capabilities(platform->display_active_dcc);
	if (expected == 0u)
		return X86_DISPLAY_UNSUPPORTED;
	if (platform->display_capabilities != expected)
		return X86_DISPLAY_INVALID_PLATFORM;
	prepared.platform_generation = platform->generation;
	prepared.bios_capabilities = expected;
	prepared.active_dcc = platform->display_active_dcc;
	if (!prepare_ranges(platform->display_active_dcc, &prepared))
		return X86_DISPLAY_UNSUPPORTED;
	if (!text_console_matches(platform, &prepared) ||
	    !crtc_matches(platform, &prepared))
		return X86_DISPLAY_INVALID_PLATFORM;
	*capability = prepared;
	return X86_DISPLAY_OK;
}

// SPDX-License-Identifier: GPL-2.0-only
/* Host-safe adapter-capability tests for BIOS DCC display ownership. */
#include "test_entry.h"
#include "x86_display.h"

static struct x86_legacy_bios_snapshot platform_with(
	uint8_t dcc, uint16_t capabilities)
{
	struct x86_legacy_bios_snapshot platform = {0};

	platform.generation = 7u;
	platform.display_capabilities = capabilities;
	platform.display_active_dcc = dcc;
	platform.display_dcc_valid = 1u;
	return platform;
}

static int test_mda_and_cga(void)
{
	const uint16_t mono = X86_LEGACY_DISPLAY_MONO_TEXT_MEMORY |
		X86_LEGACY_DISPLAY_MONO_CRTC_IO;
	const uint16_t color = X86_LEGACY_DISPLAY_COLOR_TEXT_MEMORY |
		X86_LEGACY_DISPLAY_COLOR_CRTC_IO |
		X86_LEGACY_DISPLAY_GRAPHICS_MEMORY;
	struct x86_legacy_bios_snapshot platform = platform_with(0x01u, mono);
	struct x86_display_capability display;

	if (x86_display_capability_prepare(&platform, &display) !=
		    X86_DISPLAY_OK ||
	    display.platform_generation != 7u ||
	    display.display_class != X86_DISPLAY_MDA ||
	    display.memory_range_count != 1u ||
	    display.memory[0].base != 0x000b0000u ||
	    display.memory[0].bytes != 0x00008000u ||
	    display.io_first_port != 0x03b0u ||
	    display.io_last_port != 0x03bfu)
		return 1;
	platform = platform_with(0x02u, color);
	if (x86_display_capability_prepare(&platform, &display) !=
		    X86_DISPLAY_OK ||
	    display.display_class != X86_DISPLAY_CGA ||
	    display.memory_range_count != 1u ||
	    display.memory[0].base != 0x000b8000u ||
	    display.memory[0].bytes != 0x00008000u ||
	    display.io_first_port != 0x03d0u ||
	    display.io_last_port != 0x03dfu)
		return 2;
	return 0;
}

static int test_ega_and_vga(void)
{
	const uint16_t switched = X86_LEGACY_DISPLAY_MONO_TEXT_MEMORY |
		X86_LEGACY_DISPLAY_COLOR_TEXT_MEMORY |
		X86_LEGACY_DISPLAY_GRAPHICS_MEMORY |
		X86_LEGACY_DISPLAY_MONO_CRTC_IO |
		X86_LEGACY_DISPLAY_COLOR_CRTC_IO |
		X86_LEGACY_DISPLAY_CONTROL_IO;
	struct x86_legacy_bios_snapshot platform = platform_with(
		0x05u, switched | X86_LEGACY_DISPLAY_EGA_COMPATIBLE);
	struct x86_display_capability display;

	if (x86_display_capability_prepare(&platform, &display) !=
		    X86_DISPLAY_OK ||
	    display.display_class != X86_DISPLAY_EGA ||
	    display.memory_range_count != 1u ||
	    display.memory[0].base != 0x000a0000u ||
	    display.memory[0].bytes != 0x00020000u ||
	    display.io_first_port != 0x03b0u ||
	    display.io_last_port != 0x03dfu)
		return 1;
	platform = platform_with(0x08u,
				 switched | X86_LEGACY_DISPLAY_EGA_COMPATIBLE |
				 X86_LEGACY_DISPLAY_VGA_COMPATIBLE);
	if (x86_display_capability_prepare(&platform, &display) !=
		    X86_DISPLAY_OK ||
	    display.display_class != X86_DISPLAY_VGA ||
	    display.memory[0].base != 0x000a0000u ||
	    display.memory[0].bytes != 0x00020000u ||
	    display.io_first_port != 0x03b0u ||
	    display.io_last_port != 0x03dfu)
		return 2;
	return 0;
}

static int test_mcga_ranges(void)
{
	const uint16_t common = X86_LEGACY_DISPLAY_GRAPHICS_MEMORY |
		X86_LEGACY_DISPLAY_CONTROL_IO |
		X86_LEGACY_DISPLAY_MCGA_COMPATIBLE;
	struct x86_legacy_bios_snapshot platform = platform_with(
		0x0au, common | X86_LEGACY_DISPLAY_COLOR_TEXT_MEMORY |
			       X86_LEGACY_DISPLAY_COLOR_CRTC_IO);
	struct x86_display_capability display;

	if (x86_display_capability_prepare(&platform, &display) !=
		    X86_DISPLAY_OK ||
	    display.display_class != X86_DISPLAY_MCGA ||
	    display.memory_range_count != 2u ||
	    display.memory[0].base != 0x000a0000u ||
	    display.memory[0].bytes != 0x00010000u ||
	    display.memory[1].base != 0x000b8000u ||
	    display.memory[1].bytes != 0x00008000u ||
	    display.io_first_port != 0x03c0u ||
	    display.io_last_port != 0x03dfu)
		return 1;
	platform = platform_with(
		0x0bu, common | X86_LEGACY_DISPLAY_COLOR_TEXT_MEMORY |
			       X86_LEGACY_DISPLAY_COLOR_CRTC_IO);
	if (x86_display_capability_prepare(&platform, &display) !=
		    X86_DISPLAY_OK ||
	    display.memory_range_count != 2u ||
	    display.memory[1].base != 0x000b8000u ||
	    display.io_first_port != 0x03c0u ||
	    display.io_last_port != 0x03dfu)
		return 2;
	platform = platform_with(
		0x0cu, common | X86_LEGACY_DISPLAY_COLOR_TEXT_MEMORY |
			       X86_LEGACY_DISPLAY_COLOR_CRTC_IO);
	if (x86_display_capability_prepare(&platform, &display) !=
		    X86_DISPLAY_OK ||
	    display.memory_range_count != 2u ||
	    display.memory[1].base != 0x000b8000u ||
	    display.io_first_port != 0x03c0u ||
	    display.io_last_port != 0x03dfu)
		return 3;
	return 0;
}

static int test_fail_closed_and_atomic(void)
{
	struct x86_legacy_bios_snapshot platform = platform_with(0x7fu, 0u);
	struct x86_display_capability display = {
		.platform_generation = 99u,
	};

	if (x86_display_capability_prepare(&platform, &display) !=
		    X86_DISPLAY_UNSUPPORTED ||
	    display.platform_generation != 99u)
		return 1;
	platform = platform_with(0x02u,
				 X86_LEGACY_DISPLAY_COLOR_TEXT_MEMORY |
				 X86_LEGACY_DISPLAY_COLOR_CRTC_IO);
	if (x86_display_capability_prepare(&platform, &display) !=
		    X86_DISPLAY_INVALID_PLATFORM ||
	    display.platform_generation != 99u)
		return 2;
	platform = platform_with(0x01u,
				 X86_LEGACY_DISPLAY_MONO_TEXT_MEMORY |
				 X86_LEGACY_DISPLAY_MONO_CRTC_IO);
	platform.text_console_available = 1u;
	platform.text_memory_address = 0x000b8000u;
	platform.text_memory_bytes = 4000u;
	platform.crtc_index_port = 0x03d4u;
	if (x86_display_capability_prepare(&platform, &display) !=
		    X86_DISPLAY_INVALID_PLATFORM ||
	    display.platform_generation != 99u)
		return 3;
	return 0;
}

static int run_tests(void)
{
	return test_mda_and_cga() || test_ega_and_vga() ||
	       test_mcga_ranges() || test_fail_closed_and_atomic();
}

DOSC32_TEST_ENTRY(run_tests)

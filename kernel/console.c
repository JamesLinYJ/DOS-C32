// SPDX-License-Identifier: GPL-2.0-only
#include "console.h"

#include "convert.h"
#include "io.h"
#include "x86_legacy_bios.h"

#define VGA_NORMAL_ATTRIBUTE 0x07u
#define VGA_X86_GUEST_FAULT_ATTRIBUTE 0x1fu
#if CONFIG_X86_VGA_DAC_PALETTE
#define VGA_DAC_WRITE_INDEX 0x03c8u
#define VGA_DAC_DATA 0x03c9u
#endif

#if CONFIG_X86_SERIAL_BAUD_DIVISOR < 1 || \
	CONFIG_X86_SERIAL_BAUD_DIVISOR > 0xffff
#error "legacy UART divisor must fit the 16550 divisor latch"
#endif
#if CONFIG_X86_SERIAL_POLL_LIMIT < 1
#error "legacy UART poll limit must be positive"
#endif

static size_t cursor_row;
static size_t cursor_column;
static size_t vga_width;
static size_t vga_height;
static volatile uint16_t *vga_memory;
static uint16_t vga_page_start_cell;
static uint16_t crtc_index_port;
static uint16_t serial_base;
static bool serial_enabled;
static bool vga_enabled;
#if CONFIG_X86_VGA_DAC_PALETTE
static bool vga_palette_enabled;
#endif
static uint8_t vga_attribute;

#if CONFIG_X86_VGA_DAC_PALETTE
static void load_standard_text_palette(void)
{
    static const uint8_t palette[16][3] = {
        {0u, 0u, 0u},       {0u, 0u, 42u},      {0u, 42u, 0u},
        {0u, 42u, 42u},     {42u, 0u, 0u},      {42u, 0u, 42u},
        {42u, 21u, 0u},     {42u, 42u, 42u},    {21u, 21u, 21u},
        {21u, 21u, 63u},    {21u, 63u, 21u},    {21u, 63u, 63u},
        {63u, 21u, 21u},    {63u, 21u, 63u},    {63u, 63u, 21u},
        {63u, 63u, 63u},
    };
    size_t index;

	if (!vga_palette_enabled)
		return;
    outb(VGA_DAC_WRITE_INDEX, 0u);
    for (index = 0u; index < ARRAY_SIZE(palette); ++index) {
        outb(VGA_DAC_DATA, palette[index][0]);
        outb(VGA_DAC_DATA, palette[index][1]);
        outb(VGA_DAC_DATA, palette[index][2]);
    }
}
#else
#define load_standard_text_palette() ((void)0)
#endif

static bool serial_try_initialize(uint16_t candidate)
{
    uint8_t probe;
    uint32_t remaining;

	if (candidate == 0u)
		return false;
    outb((uint16_t)(candidate + 1u), 0x00);
    outb((uint16_t)(candidate + 3u), 0x80);
    outb(candidate, (uint8_t)(CONFIG_X86_SERIAL_BAUD_DIVISOR & 0xffu));
    outb((uint16_t)(candidate + 1u),
	 (uint8_t)(CONFIG_X86_SERIAL_BAUD_DIVISOR >> 8u));
    outb((uint16_t)(candidate + 3u), 0x03);
    outb((uint16_t)(candidate + 2u), 0xc7);
    outb((uint16_t)(candidate + 4u), 0x1e);
    outb(candidate, 0xae);
    remaining = CONFIG_X86_SERIAL_POLL_LIMIT;
	while ((inb((uint16_t)(candidate + 5u)) & 0x01u) == 0u &&
	       remaining != 0u) {
        --remaining;
    }
	probe = remaining != 0u ? inb(candidate) : 0u;
	if (probe != 0xae) {
		outb((uint16_t)(candidate + 1u), 0u);
		outb((uint16_t)(candidate + 4u), 0u);
		return false;
	}
	outb((uint16_t)(candidate + 4u), 0x0f);
	return true;
}

static void serial_put_raw(char character)
{
    uint32_t remaining;

#if CONFIG_X86_DEBUGCON
    /* Non-PC QEMU/Bochs debug transport: compiled out of production. */
    outb(0x00e9, (uint8_t)character);
#endif

    if (!serial_enabled) {
        return;
    }

    remaining = CONFIG_X86_SERIAL_POLL_LIMIT;
	while ((inb((uint16_t)(serial_base + 5u)) & 0x20u) == 0u &&
	       remaining != 0u) {
        --remaining;
    }
    if (remaining != 0u) {
		outb(serial_base, (uint8_t)character);
    }
}

static void serial_putc(char character)
{
    if (character == '\n') {
        serial_put_raw('\r');
    }
    serial_put_raw(character);
}

static void update_hardware_cursor(void)
{
	uint16_t position;

	if (!vga_enabled)
		return;
	position = (uint16_t)(vga_page_start_cell +
			      cursor_row * vga_width + cursor_column);
    outb(crtc_index_port, 0x0f);
	outb((uint16_t)(crtc_index_port + 1u),
	     (uint8_t)(position & 0xffu));
    outb(crtc_index_port, 0x0e);
	outb((uint16_t)(crtc_index_port + 1u), (uint8_t)(position >> 8));
}

static void scroll_if_needed(void)
{
    size_t row;
    size_t column;

	if (!vga_enabled || cursor_row < vga_height) {
        return;
    }

	for (row = 1u; row < vga_height; ++row) {
		for (column = 0u; column < vga_width; ++column) {
			vga_memory[(row - 1u) * vga_width + column] =
				vga_memory[row * vga_width + column];
        }
    }
	for (column = 0u; column < vga_width; ++column) {
		vga_memory[(vga_height - 1u) * vga_width + column] =
            (uint16_t)(vga_attribute << 8) | (uint16_t)' ';
    }
	cursor_row = vga_height - 1u;
}

static void vga_put_printable(char character)
{
	vga_memory[cursor_row * vga_width + cursor_column] =
        (uint16_t)(vga_attribute << 8) | (uint8_t)character;
    ++cursor_column;
	if (cursor_column == vga_width) {
        cursor_column = 0;
        ++cursor_row;
        scroll_if_needed();
    }
}

void console_init(void)
{
	struct x86_legacy_bios_snapshot platform;
	size_t index;

    cursor_row = 0;
    cursor_column = 0;
	vga_width = 0u;
	vga_height = 0u;
	vga_memory = NULL;
	vga_page_start_cell = 0u;
	crtc_index_port = 0u;
	serial_base = 0u;
	serial_enabled = false;
	vga_enabled = false;
#if CONFIG_X86_VGA_DAC_PALETTE
	vga_palette_enabled = false;
#endif
    vga_attribute = VGA_NORMAL_ATTRIBUTE;
	if (x86_legacy_bios_snapshot(&platform)) {
		for (index = 0u; index < X86_BIOS_SERIAL_PORT_COUNT; ++index) {
			uint16_t candidate = platform.serial_ports[index];

			if (serial_try_initialize(candidate)) {
				serial_base = candidate;
				serial_enabled = true;
				break;
			}
		}
		if (platform.text_console_available == 1u) {
			vga_width = platform.text_columns;
			vga_height = platform.text_rows;
			vga_memory = (volatile uint16_t *)(uintptr_t)
				platform.text_memory_address;
			vga_page_start_cell =
				(uint16_t)(platform.text_page_offset / 2u);
			crtc_index_port = platform.crtc_index_port;
			vga_enabled = true;
#if CONFIG_X86_VGA_DAC_PALETTE
			vga_palette_enabled = platform.video_mode != 7u;
#endif
		}
	}
	load_standard_text_palette();
    console_clear();
}

void console_clear(void)
{
    size_t position;

	if (vga_enabled) {
		for (position = 0u; position < vga_width * vga_height;
		     ++position)
			vga_memory[position] =
				(uint16_t)(vga_attribute << 8) |
				(uint16_t)' ';
	}
    cursor_row = 0;
    cursor_column = 0;
    update_hardware_cursor();
}

void console_backspace(void)
{
	if (!vga_enabled) {
		serial_put_raw('\b');
		serial_put_raw(' ');
		serial_put_raw('\b');
		return;
	}
    if (cursor_column == 0) {
        if (cursor_row == 0) {
            return;
        }
        --cursor_row;
		cursor_column = vga_width;
    }
    --cursor_column;
	vga_memory[cursor_row * vga_width + cursor_column] =
        (uint16_t)(vga_attribute << 8) | (uint16_t)' ';
    serial_put_raw('\b');
    serial_put_raw(' ');
    serial_put_raw('\b');
    update_hardware_cursor();
}

void console_putc(char character)
{
    size_t spaces;

    if (character == '\b') {
        console_backspace();
        return;
    }

    serial_putc(character);
	if (!vga_enabled)
		return;
    switch (character) {
    case '\n':
        cursor_column = 0;
        ++cursor_row;
        scroll_if_needed();
        break;
    case '\r':
        cursor_column = 0;
        break;
    case '\t':
        spaces = 8u - (cursor_column & 7u);
        while (spaces-- != 0u) {
            vga_put_printable(' ');
        }
        break;
    default:
        if ((uint8_t)character >= 0x20u) {
            vga_put_printable(character);
        }
        break;
    }
    update_hardware_cursor();
}

void console_write(const char *text, size_t count)
{
    size_t index;
    for (index = 0; index < count; ++index) {
        console_putc(text[index]);
    }
}

void console_serial_write(const char *text, size_t count)
{
	size_t index;

	if (text == NULL)
		return;
	for (index = 0u; index < count; ++index)
		serial_putc(text[index]);
}

void console_write_u32(uint32_t value)
{
    char digits[10];
    size_t count = 0;

    if (value == 0u) {
        console_putc('0');
        return;
    }
    while (value != 0u) {
        digits[count++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    while (count != 0u) {
        console_putc(digits[--count]);
	}
}

void console_write_u64(uint64_t value)
{
	char digits[21];
	size_t count;

	if (format_u64_s(digits, sizeof(digits), value, 10u, false, &count) !=
	    CONVERT_OK)
		return;
	console_write(digits, count);
}

void console_write_hex(uint32_t value)
{
    static const char hexadecimal[] = "0123456789ABCDEF";
    int shift;

    console_write_literal("0x");
    for (shift = 28; shift >= 0; shift -= 4) {
        console_putc(hexadecimal[(value >> (uint32_t)shift) & 0x0fu]);
	}
}

void console_write_hex64(uint64_t value)
{
	char digits[17];
	size_t count;
	size_t padding;

	if (format_u64_s(digits, sizeof(digits), value, 16u, true, &count) !=
	    CONVERT_OK)
		return;
	console_write_literal("0x");
	padding = 16u - count;
	while (padding-- != 0u)
		console_putc('0');
	console_write(digits, count);
}

void console_begin_x86_guest_fault_screen(void)
{
	load_standard_text_palette();
	vga_attribute = VGA_X86_GUEST_FAULT_ATTRIBUTE;
	console_clear();
}

void console_end_x86_guest_fault_screen(void)
{
	load_standard_text_palette();
    vga_attribute = VGA_NORMAL_ATTRIBUTE;
    console_clear();
}

void console_set_vga_attribute(uint8_t attribute)
{
    vga_attribute = attribute;
}

bool console_text_geometry(size_t *columns, size_t *rows)
{
	if (columns == NULL || rows == NULL || !vga_enabled)
		return false;
	*columns = vga_width;
	*rows = vga_height;
	return true;
}

void console_set_cursor_position(size_t row, size_t column)
{
	if (!vga_enabled)
		return;
	if (row >= vga_height)
		row = vga_height - 1u;
	if (column >= vga_width)
		column = vga_width - 1u;
    cursor_row = row;
    cursor_column = column;
    update_hardware_cursor();
}

void console_fill_row(size_t row, char character, uint8_t attribute)
{
    size_t column;

	if (!vga_enabled || row >= vga_height)
        return;
	for (column = 0u; column < vga_width; ++column)
		vga_memory[row * vga_width + column] =
            (uint16_t)(attribute << 8) | (uint8_t)character;
}

void console_fill_rectangle(size_t top, size_t left, size_t height,
                            size_t width, char character, uint8_t attribute)
{
    size_t row;
    size_t column;

	if (!vga_enabled || top >= vga_height || left >= vga_width)
        return;
	if (height > vga_height - top)
		height = vga_height - top;
	if (width > vga_width - left)
		width = vga_width - left;
    for (row = 0u; row < height; ++row) {
        for (column = 0u; column < width; ++column)
			vga_memory[(top + row) * vga_width + left + column] =
                (uint16_t)(attribute << 8) | (uint8_t)character;
    }
}

void console_draw_box(size_t top, size_t left, size_t height, size_t width,
                      uint8_t attribute)
{
    size_t index;

	if (!vga_enabled || top >= vga_height || left >= vga_width ||
	    height < 2u || width < 2u || height > vga_height - top ||
	    width > vga_width - left)
        return;
    for (index = 1u; index + 1u < width; ++index) {
		vga_memory[top * vga_width + left + index] =
            (uint16_t)(attribute << 8) | 0xc4u;
		vga_memory[(top + height - 1u) * vga_width + left + index] =
            (uint16_t)(attribute << 8) | 0xc4u;
    }
    for (index = 1u; index + 1u < height; ++index) {
		vga_memory[(top + index) * vga_width + left] =
            (uint16_t)(attribute << 8) | 0xb3u;
		vga_memory[(top + index) * vga_width + left + width - 1u] =
            (uint16_t)(attribute << 8) | 0xb3u;
    }
	vga_memory[top * vga_width + left] =
        (uint16_t)(attribute << 8) | 0xdau;
	vga_memory[top * vga_width + left + width - 1u] =
        (uint16_t)(attribute << 8) | 0xbfu;
	vga_memory[(top + height - 1u) * vga_width + left] =
        (uint16_t)(attribute << 8) | 0xc0u;
	vga_memory[(top + height - 1u) * vga_width + left + width - 1u] =
        (uint16_t)(attribute << 8) | 0xd9u;
}

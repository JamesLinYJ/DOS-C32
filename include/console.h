/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DOSC32_CONSOLE_H
#define DOSC32_CONSOLE_H

#include "types.h"

void console_init(void);
void console_clear(void);
void console_putc(char character);
void console_write(const char *text, size_t count);
void console_write_u32(uint32_t value);
void console_write_u64(uint64_t value);
void console_write_hex(uint32_t value);
void console_write_hex64(uint64_t value);
/* Diagnostics which must not disturb a guest-owned VGA screen. */
void console_serial_write(const char *text, size_t count);
void console_backspace(void);
/* Enter the blue guest-fault display. Serial diagnostics stay plain text. */
void console_begin_x86_guest_fault_screen(void);
void console_end_x86_guest_fault_screen(void);
void console_set_vga_attribute(uint8_t attribute);
bool console_text_geometry(size_t *columns, size_t *rows);
void console_set_cursor_position(size_t row, size_t column);
void console_fill_row(size_t row, char character, uint8_t attribute);
void console_fill_rectangle(size_t top, size_t left, size_t height,
			    size_t width, char character, uint8_t attribute);
void console_draw_box(size_t top, size_t left, size_t height, size_t width,
		      uint8_t attribute);

#define console_write_literal(text) console_write((text), sizeof(text) - 1u)

#endif

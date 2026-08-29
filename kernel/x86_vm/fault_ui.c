// SPDX-License-Identifier: GPL-2.0-only
#include "x86_guest_fault_ui.h"

#include "console.h"

#define UI_TOPBAR 0x3fu
#define UI_CARD 0x07u
#define UI_TEXT 0x0fu
#define UI_CYAN 0x0bu
#define UI_YELLOW 0x0eu
#define UI_MUTED 0x08u
#define UI_FOOTER 0x1bu

#define UI_DETAILED_MINIMUM_COLUMNS 80u
#define UI_DETAILED_MINIMUM_ROWS 25u

/* Computed once from the detected console width for each fault display. */
static size_t ui_content_end;

static size_t content_width(size_t column)
{
	return column < ui_content_end ? ui_content_end - column : 0u;
}

static void text_at(size_t row, size_t column, uint8_t attribute,
		    const char *text, size_t length)
{
	console_set_vga_attribute(attribute);
	console_set_cursor_position(row, column);
	console_write(text, length);
}

static void bounded_text_at(size_t row, size_t column, size_t width,
			    uint8_t attribute, const char *text, size_t length)
{
	if (length > width)
		length = width;
	text_at(row, column, attribute, text, length);
}

static void paragraph_at(size_t row, size_t column, size_t width,
			 size_t max_lines, uint8_t attribute,
			 const char *text, size_t length)
{
	size_t offset = 0u;
	size_t line;

	if (width == 0u)
		return;
	for (line = 0u; line < max_lines; ++line) {
		size_t end;
		size_t take;

		while (offset < length && text[offset] == ' ')
			++offset;
		if (offset == length)
			return;
		take = length - offset;
		if (take > width) {
			take = width;
			end = offset + take;
			while (end > offset && text[end] != ' ')
				--end;
			if (end != offset)
				take = end - offset;
		}
		bounded_text_at(row + line, column, width, attribute,
				text + offset, take);
		offset += take;
	}
}

#define literal_at(row, column, attribute, text) \
	text_at((row), (column), (attribute), (text), sizeof(text) - 1u)
#define card_literal_at(row, column, attribute, text) \
	bounded_text_at((row), (column), content_width(column), (attribute), \
			(text), sizeof(text) - 1u)
#define card_paragraph_at(row, column, lines, attribute, text) \
	paragraph_at((row), (column), content_width(column), (lines), \
		     (attribute), (text), sizeof(text) - 1u)

static void compact_fault_ui(size_t columns, size_t rows)
{
	size_t footer_row = rows > 1u ? rows - 2u : 0u;
	size_t width = columns > 6u ? columns - 6u : columns;
	size_t left = columns > 6u ? 3u : 0u;

	console_fill_row(0u, ' ', UI_TOPBAR);
	bounded_text_at(0u, left, width, UI_TOPBAR,
			"DOS-C32  SYSTEM PROTECTION",
			sizeof("DOS-C32  SYSTEM PROTECTION") - 1u);
	if (rows > 3u)
		bounded_text_at(2u, left, width, UI_YELLOW,
				"APPLICATION STOPPED SAFELY",
				sizeof("APPLICATION STOPPED SAFELY") - 1u);
	if (rows > 6u)
		paragraph_at(4u, left, width, 2u, UI_TEXT,
			     "The application entered an unsafe state. Its context "
			     "was isolated and the system remains protected.",
			     sizeof("The application entered an unsafe state. Its "
				    "context was isolated and the system remains "
				    "protected.") - 1u);
	console_fill_row(footer_row, ' ', UI_FOOTER);
	bounded_text_at(footer_row, left, width, UI_FOOTER,
			"Press any key to return",
			sizeof("Press any key to return") - 1u);
}

void x86_guest_fault_ui_show(const struct x86_guest_fault_snapshot *fault)
{
	size_t card_height;
	size_t card_left = 4u;
	size_t card_top = 3u;
	size_t card_width;
	size_t columns;
	size_t footer_row;
	size_t rows;
	size_t right;

	if (fault == NULL)
		return;
	console_begin_x86_guest_fault_screen();
	if (!console_text_geometry(&columns, &rows))
		return;
	if (columns < UI_DETAILED_MINIMUM_COLUMNS ||
	    rows < UI_DETAILED_MINIMUM_ROWS) {
		compact_fault_ui(columns, rows);
		return;
	}
	card_width = columns - card_left * 2u;
	footer_row = rows - 2u;
	card_height = footer_row - card_top - 1u;
	ui_content_end = card_left + card_width - 4u;
	right = columns - 3u - (sizeof("SYSTEM SAFE") - 1u);
	console_fill_row(0u, ' ', UI_TOPBAR);
	literal_at(0u, 3u, UI_TOPBAR, "DOS-C32  SYSTEM PROTECTION");
	literal_at(0u, right, UI_TOPBAR, "SYSTEM SAFE");
	literal_at(2u,
		   (columns - (sizeof("[  ISOLATED  ]") - 1u)) / 2u,
		   UI_YELLOW, "[  ISOLATED  ]");
	/* A black card and two-cell shadow add hierarchy without a graphics
	 * dependency; CP437 line art remains available in the earliest VGA mode. */
	console_fill_rectangle(card_top + 1u, card_left + 2u, card_height,
			       card_width, ' ', 0x00u);
	console_fill_rectangle(card_top, card_left, card_height, card_width,
			       ' ', UI_CARD);
	console_draw_box(card_top, card_left, card_height, card_width, UI_CYAN);
	card_literal_at(4u, 8u, UI_CYAN, "APPLICATION PROTECTION FAULT");
	card_paragraph_at(5u, 8u, 2u, UI_TEXT,
			  "An unsafe application state was detected and stopped "
			  "before it could spread.");
	card_literal_at(7u, 8u, UI_YELLOW, "[ STOP RECORD ]");
	card_literal_at(8u, 10u, UI_TEXT, "Step  ");
	console_write_u32(fault->step_status);
	console_write_literal("    Event  ");
	console_write_u32(fault->event_kind);
	console_write_literal("    Vector  ");
	console_write_hex(fault->vector);
	card_literal_at(9u, 10u, UI_TEXT, "Machine  ");
	console_write_u32(fault->machine_status);
	console_write_literal("    Session  ");
	console_write_u32(fault->session_status);
	card_literal_at(11u, 8u, UI_YELLOW, "[ PROCESSOR SNAPSHOT ]");
	card_literal_at(12u, 10u, UI_TEXT, "CS:IP  ");
	console_write_hex(fault->cpu.cs);
	console_putc(':');
	console_write_hex(dos_register_low16(fault->cpu.eip));
	console_write_literal("    SS:SP  ");
	console_write_hex(fault->cpu.ss);
	console_putc(':');
	console_write_hex(dos_register_low16(fault->cpu.esp));
	card_literal_at(13u, 10u, UI_TEXT, "AX  ");
	console_write_hex(dos_register_low16(fault->cpu.eax));
	console_write_literal("  BX  ");
	console_write_hex(dos_register_low16(fault->cpu.ebx));
	console_write_literal("  CX  ");
	console_write_hex(dos_register_low16(fault->cpu.ecx));
	console_write_literal("  DX  ");
	console_write_hex(dos_register_low16(fault->cpu.edx));
	card_literal_at(15u, 8u, UI_YELLOW, "[ HARDWARE BOUNDARY ]");
	card_literal_at(16u, 10u, UI_TEXT, "Port  ");
	console_write_hex(fault->port);
	console_write_literal("  Width  ");
	console_write_u32(fault->io_width);
	console_write_literal("  Operation  ");
	console_write(fault->io_write ? "WRITE" : "READ",
		      fault->io_write ? 5u : 4u);
	card_literal_at(17u, 10u, UI_TEXT, "Last chained interrupt  ");
	console_write_hex(fault->last_chained_vector);
	console_fill_rectangle(18u, 8u, 1u, ui_content_end - 8u,
			       (char)0xc4u, UI_MUTED);
	card_literal_at(19u, 8u, UI_YELLOW,
			"PROTECTED  Kernel and virtual hardware integrity preserved");
	card_literal_at(20u, 8u, UI_MUTED,
			"Application context discarded safely. Restart is not required.");
	console_fill_row(footer_row, ' ', UI_FOOTER);
	literal_at(footer_row, 3u, UI_FOOTER,
		   "Application stopped / System protected");
	right = columns - 3u - (sizeof("Press any key to return") - 1u);
	literal_at(footer_row, right, UI_FOOTER, "Press any key to return");
	console_set_vga_attribute(UI_FOOTER);
	console_set_cursor_position(footer_row, columns - 8u);
}

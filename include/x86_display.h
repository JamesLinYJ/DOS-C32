/* SPDX-License-Identifier: GPL-2.0-only */
/* Immutable legacy-display capabilities derived from validated BIOS facts. */
#ifndef DOSC32_X86_DISPLAY_H
#define DOSC32_X86_DISPLAY_H

#include "compiler.h"
#include "types.h"
#include "x86_legacy_bios.h"

#define X86_DISPLAY_MEMORY_RANGE_CAPACITY 2u

enum x86_display_class {
	X86_DISPLAY_NONE = 0,
	X86_DISPLAY_MDA,
	X86_DISPLAY_CGA,
	X86_DISPLAY_EGA,
	X86_DISPLAY_VGA,
	X86_DISPLAY_MCGA
};

enum x86_display_status {
	X86_DISPLAY_OK = 0,
	X86_DISPLAY_INVALID_ARGUMENT,
	X86_DISPLAY_INVALID_PLATFORM,
	X86_DISPLAY_UNSUPPORTED
};

struct x86_display_memory_range {
	uint32_t base;
	uint32_t bytes;
};

struct x86_display_capability {
	uint64_t platform_generation;
	struct x86_display_memory_range
		memory[X86_DISPLAY_MEMORY_RANGE_CAPACITY];
	uint16_t io_first_port;
	uint16_t io_last_port;
	uint16_t bios_capabilities;
	uint8_t memory_range_count;
	uint8_t display_class;
	uint8_t active_dcc;
	uint8_t reserved[7];
} __aligned(8);

/* Pure acquire-before-publish decoder used by guest-space and host tests. */
enum x86_display_status x86_display_capability_prepare(
	const struct x86_legacy_bios_snapshot *platform,
	struct x86_display_capability *capability) __must_check;

static_assert_expression(sizeof(struct x86_display_memory_range) == 8u,
			 "display memory ranges must remain fixed width");
static_assert_expression(sizeof(struct x86_display_capability) == 40u,
			 "display capabilities must remain fixed width");

#endif

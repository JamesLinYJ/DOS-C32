/* SPDX-License-Identifier: GPL-2.0-only */
/* DOS-C32 native protected-mode executable format. */
#ifndef DOSC32_C32_IMAGE_H
#define DOSC32_C32_IMAGE_H

#include "compiler.h"
#include "types.h"
#include "x86_paging.h"

#define C32_IMAGE_MAGIC 0x58323343u /* "C32X" in little-endian order. */
#define C32_IMAGE_FORMAT_VERSION 1u
#define C32_IMAGE_ABI_VERSION 1u
#define C32_IMAGE_HEADER_BYTES 64u

struct c32_image_header {
	uint32_t magic;
	uint16_t header_size;
	uint16_t format_version;
	uint32_t image_base;
	uint32_t image_offset;
	uint32_t file_bytes;
	uint32_t memory_bytes;
	uint32_t entry_rva;
	uint32_t stack_top;
	uint32_t required_abi;
	uint32_t flags;
	uint32_t reserved[6];
} __packed;

enum c32_image_status {
	C32_IMAGE_OK = 0,
	C32_IMAGE_INVALID_ARGUMENT,
	C32_IMAGE_TRUNCATED,
	C32_IMAGE_BAD_MAGIC,
	C32_IMAGE_UNSUPPORTED,
	C32_IMAGE_BAD_LAYOUT
};

struct c32_image_plan {
	uint32_t image_base;
	uint32_t image_offset;
	uint32_t file_bytes;
	uint32_t memory_bytes;
	uint32_t entry_point;
	uint32_t stack_top;
};

enum c32_image_status c32_image_plan_create(
	const uint8_t *header_bytes, size_t header_capacity,
	uint32_t complete_file_size, struct c32_image_plan *plan) __must_check;

static_assert_expression(sizeof(struct c32_image_header) ==
			 C32_IMAGE_HEADER_BYTES,
			 "C32 executable header must remain 64 bytes");

#endif

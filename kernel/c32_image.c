// SPDX-License-Identifier: GPL-2.0-only
/* Pointer-safe parser for the fixed C32 native executable header. */
#include "c32_image.h"

static uint16_t read_le16(const uint8_t *bytes)
{
	return (uint16_t)bytes[0] | (uint16_t)((uint16_t)bytes[1] << 8u);
}

static uint32_t read_le32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8u) |
	       ((uint32_t)bytes[2] << 16u) | ((uint32_t)bytes[3] << 24u);
}

static bool add_u32(uint32_t left, uint32_t right, uint32_t *sum)
{
	if (sum == NULL || right > ~(uint32_t)0 - left)
		return false;
	*sum = left + right;
	return true;
}

enum c32_image_status c32_image_plan_create(
	const uint8_t *bytes, size_t capacity, uint32_t complete_file_size,
	struct c32_image_plan *plan)
{
	uint32_t image_base;
	uint32_t image_offset;
	uint32_t file_bytes;
	uint32_t memory_bytes;
	uint32_t entry_rva;
	uint32_t stack_top;
	uint32_t file_end;
	uint32_t memory_end;
	uint32_t entry_point;
	size_t index;

	if (bytes == NULL || plan == NULL)
		return C32_IMAGE_INVALID_ARGUMENT;
	if (capacity < C32_IMAGE_HEADER_BYTES ||
	    complete_file_size < C32_IMAGE_HEADER_BYTES)
		return C32_IMAGE_TRUNCATED;
	if (read_le32(bytes) != C32_IMAGE_MAGIC)
		return C32_IMAGE_BAD_MAGIC;
	if (read_le16(bytes + 4u) != C32_IMAGE_HEADER_BYTES ||
	    read_le16(bytes + 6u) != C32_IMAGE_FORMAT_VERSION ||
	    read_le32(bytes + 32u) != C32_IMAGE_ABI_VERSION ||
	    read_le32(bytes + 36u) != 0u)
		return C32_IMAGE_UNSUPPORTED;
	for (index = 40u; index < C32_IMAGE_HEADER_BYTES; ++index) {
		if (bytes[index] != 0u)
			return C32_IMAGE_UNSUPPORTED;
	}

	image_base = read_le32(bytes + 8u);
	image_offset = read_le32(bytes + 12u);
	file_bytes = read_le32(bytes + 16u);
	memory_bytes = read_le32(bytes + 20u);
	entry_rva = read_le32(bytes + 24u);
	stack_top = read_le32(bytes + 28u);
	if (image_base != X86_PROTECTED_USER_BASE ||
	    image_offset != C32_IMAGE_HEADER_BYTES || file_bytes == 0u ||
	    memory_bytes < file_bytes || entry_rva >= file_bytes ||
	    stack_top != X86_PROTECTED_USER_STACK_TOP ||
	    !add_u32(image_offset, file_bytes, &file_end) ||
	    file_end != complete_file_size ||
	    !add_u32(image_base, memory_bytes, &memory_end) ||
	    memory_end > X86_PROTECTED_USER_IMAGE_LIMIT ||
	    !add_u32(image_base, entry_rva, &entry_point))
		return C32_IMAGE_BAD_LAYOUT;

	*plan = (struct c32_image_plan){
		.image_base = image_base,
		.image_offset = image_offset,
		.file_bytes = file_bytes,
		.memory_bytes = memory_bytes,
		.entry_point = entry_point,
		.stack_top = stack_top,
	};
	return C32_IMAGE_OK;
}

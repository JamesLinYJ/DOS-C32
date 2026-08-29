/* SPDX-License-Identifier: GPL-2.0-only */
/* Backend-neutral executable image tags used by DOS loading and execution. */
#ifndef DOSC32_DOS_IMAGE_H
#define DOSC32_DOS_IMAGE_H

#include "types.h"

enum dos_image_format {
	DOS_IMAGE_COM = 0,
	DOS_IMAGE_MZ,
	DOS_IMAGE_NATIVE32
};

static inline bool dos_image_format_value_is_valid(uint8_t format)
{
	return format == (uint8_t)DOS_IMAGE_COM ||
	       format == (uint8_t)DOS_IMAGE_MZ ||
	       format == (uint8_t)DOS_IMAGE_NATIVE32;
}

#endif

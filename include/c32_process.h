/* SPDX-License-Identifier: GPL-2.0-only */
/* Filesystem-neutral loader for one validated native C32 process image. */
#ifndef DOSC32_C32_PROCESS_H
#define DOSC32_C32_PROCESS_H

#include "c32_image.h"
#include "iomgr.h"

enum c32_process_load_status {
	C32_PROCESS_LOAD_OK = 0,
	C32_PROCESS_LOAD_INVALID_ARGUMENT,
	C32_PROCESS_LOAD_NOT_FOUND,
	C32_PROCESS_LOAD_IO_ERROR,
	C32_PROCESS_LOAD_BAD_IMAGE,
	C32_PROCESS_LOAD_PAGING_ERROR
};

enum c32_process_load_status c32_process_load(
	iomgr_volume_handle_t volume, const char *path, size_t path_capacity,
	struct c32_image_plan *plan) __must_check;

#endif

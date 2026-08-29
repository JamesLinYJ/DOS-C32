/* SPDX-License-Identifier: GPL-2.0-only */
/* XMS adapter for the shared generation-bound x86 physical-page owner. */
#ifndef DOSC32_X86_XMS_MEMORY_H
#define DOSC32_X86_XMS_MEMORY_H

#include "dos_xms.h"

const struct dos_xms_memory_ops *
x86_xms_memory_runtime_operations(void);

#endif

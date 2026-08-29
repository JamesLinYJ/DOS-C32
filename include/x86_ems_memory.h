/* SPDX-License-Identifier: GPL-2.0-only */
/* EMS/VCPI adapter for the shared x86 guest physical-page runtime owner. */
#ifndef DOSC32_X86_EMS_MEMORY_H
#define DOSC32_X86_EMS_MEMORY_H

#include "dos_ems.h"

const struct dos_ems_page_ops *
x86_ems_memory_runtime_operations(void);

#endif

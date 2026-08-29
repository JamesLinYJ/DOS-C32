/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DOSC32_X86_NATIVE_IRQ_PRIVATE_H
#define DOSC32_X86_NATIVE_IRQ_PRIVATE_H

#include "x86_native_irq_dispatch.h"
#include "overflow.h"

#define X86_NATIVE_IRQ_GENERATION_MAX ((uint64_t)-2)
#define X86_NATIVE_IRQ_DISPATCH_COOKIE 0x4e495244u
#define X86_NATIVE_IRQ_INVALID_SLOT ((uint32_t)-1)

#endif

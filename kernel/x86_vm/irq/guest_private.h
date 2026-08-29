/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DOSC32_X86_GUEST_IRQ_PRIVATE_H
#define DOSC32_X86_GUEST_IRQ_PRIVATE_H

#include "x86_guest_irq_router.h"
#include "overflow.h"

#define X86_GUEST_IRQ_GENERATION_MAX ((uint64_t)-2)
#define X86_GUEST_IRQ_ROUTER_COOKIE 0x47524952u
#define PRODUCER_EMPTY 0u
#define PRODUCER_ACTIVE 1u
#define PRODUCER_QUIESCED 2u

#endif

/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DOSC32_CONFIG_X86_NATIVE_INPUT_H
#define DOSC32_CONFIG_X86_NATIVE_INPUT_H

/* Controller protocol shape only; presence/capacities are runtime facts. */
#define CONFIG_X86_NATIVE_INPUT_ENDPOINT_COUNT 2u
/* Process-context safety ceiling; each platform instance supplies its bound. */
#define CONFIG_X86_NATIVE_INPUT_WRITE_POLL_LIMIT_MAX 1000000u

#endif

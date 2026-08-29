/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DOSC32_CONFIG_X86_LEGACY_IRQ_H
#define DOSC32_CONFIG_X86_LEGACY_IRQ_H

/*
 * Native scheduling policy.  A 1193-clock reload samples the 8254 input at
 * approximately one millisecond without claiming wall-clock calibration.
 * The guest keeps its own reload/divider; multiple guest expirations within
 * one native quantum are counted and coalesced only at the guest PIC edge.
 */
#define CONFIG_X86_NATIVE_PIT_RELOAD 1193u
#define CONFIG_X86_NATIVE_PIT_INPUT_QUANTUM 1193ull
#define CONFIG_X86_NATIVE_PIT_RATE_CALIBRATED 0u

/* Early-boot descriptor storage capacity; not a discovered device count. */
#define CONFIG_X86_LEGACY_IRQ_ACTION_CAPACITY 8u

#endif

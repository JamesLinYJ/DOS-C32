/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DOSC32_CONFIG_X86_LEGACY_CHIPSET_H
#define DOSC32_CONFIG_X86_LEGACY_CHIPSET_H

/*
 * Deterministic legacy-PC guest policy used until a trusted clock or firmware
 * state provider is bound.  These values initialize a private software model;
 * they are never reported as detected host hardware.
 */
#define CONFIG_X86_GUEST_PIC_PRIMARY_VECTOR_BASE 0x08u
#define CONFIG_X86_GUEST_PIC_SECONDARY_VECTOR_BASE 0x70u
#define CONFIG_X86_GUEST_PIC_PRIMARY_MASK 0x00u
#define CONFIG_X86_GUEST_PIC_SECONDARY_MASK 0x00u
#define CONFIG_X86_GUEST_PIC_PRIMARY_CASCADE 0x04u
#define CONFIG_X86_GUEST_PIC_SECONDARY_CASCADE 0x02u

#define CONFIG_X86_GUEST_PIT_CHANNEL0_RELOAD 0x0000u
#define CONFIG_X86_GUEST_PIT_CHANNEL1_RELOAD 0x0000u
#define CONFIG_X86_GUEST_PIT_CHANNEL2_RELOAD 0x0000u
#define CONFIG_X86_GUEST_PIT_DEFAULT_ACCESS 0x03u
#define CONFIG_X86_GUEST_PIT_DEFAULT_MODE 0x03u

/* Register-mode policy only; calendar values come from the boot RTC owner. */
#define CONFIG_X86_GUEST_RTC_STATUS_A 0x26u
#define CONFIG_X86_GUEST_RTC_STATUS_B 0x02u

#endif

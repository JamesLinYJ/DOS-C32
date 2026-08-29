// SPDX-License-Identifier: GPL-2.0-only
/* Deterministic fallback policy for the isolated legacy-PC chipset model. */
#include "x86_legacy_chipset.h"

#include "x86_legacy_bios.h"

#include "../../../config/x86-legacy-chipset.h"

enum x86_legacy_chipset_status x86_legacy_chipset_policy_config(
	const struct x86_legacy_bios_snapshot *platform,
	struct x86_legacy_chipset_config *config)
{
	if (platform == NULL || config == NULL || platform->rtc_valid > 1u)
		return X86_LEGACY_CHIPSET_INVALID_ARGUMENT;
	*config = (struct x86_legacy_chipset_config){
		.rtc_year = platform->rtc_valid != 0u ? platform->rtc_year : 0u,
		.pit_reload = {
			CONFIG_X86_GUEST_PIT_CHANNEL0_RELOAD,
			CONFIG_X86_GUEST_PIT_CHANNEL1_RELOAD,
			CONFIG_X86_GUEST_PIT_CHANNEL2_RELOAD,
		},
		.pic_vector_base = {
			CONFIG_X86_GUEST_PIC_PRIMARY_VECTOR_BASE,
			CONFIG_X86_GUEST_PIC_SECONDARY_VECTOR_BASE,
		},
		.pic_mask = {
			CONFIG_X86_GUEST_PIC_PRIMARY_MASK,
			CONFIG_X86_GUEST_PIC_SECONDARY_MASK,
		},
		.pit_access = {
			CONFIG_X86_GUEST_PIT_DEFAULT_ACCESS,
			CONFIG_X86_GUEST_PIT_DEFAULT_ACCESS,
			CONFIG_X86_GUEST_PIT_DEFAULT_ACCESS,
		},
		.pit_mode = {
			CONFIG_X86_GUEST_PIT_DEFAULT_MODE,
			CONFIG_X86_GUEST_PIT_DEFAULT_MODE,
			CONFIG_X86_GUEST_PIT_DEFAULT_MODE,
		},
		.pit_bcd = {0u, 0u, 0u},
		.rtc_second = platform->rtc_valid != 0u
				      ? platform->rtc_second
				      : 0u,
		.rtc_minute = platform->rtc_valid != 0u
				      ? platform->rtc_minute
				      : 0u,
		.rtc_hour = platform->rtc_valid != 0u ? platform->rtc_hour : 0u,
		.rtc_weekday = platform->rtc_valid != 0u
				       ? platform->rtc_weekday
				       : 0u,
		.rtc_day = platform->rtc_valid != 0u ? platform->rtc_day : 0u,
		.rtc_month = platform->rtc_valid != 0u
				     ? platform->rtc_month
				     : 0u,
		.rtc_status_a = CONFIG_X86_GUEST_RTC_STATUS_A,
		.rtc_status_b = CONFIG_X86_GUEST_RTC_STATUS_B,
		.rtc_valid = platform->rtc_valid,
		.pic_cascade_config = {
			CONFIG_X86_GUEST_PIC_PRIMARY_CASCADE,
			CONFIG_X86_GUEST_PIC_SECONDARY_CASCADE,
		},
	};
	return X86_LEGACY_CHIPSET_OK;
}

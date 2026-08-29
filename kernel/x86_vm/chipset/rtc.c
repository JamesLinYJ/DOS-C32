// SPDX-License-Identifier: GPL-2.0-only
/* Stable MC146818-compatible CMOS/RTC register model for the guest. */
#include "private.h"

#include "math64.h"

#define RTC_REGISTER_SECONDS 0x00u
#define RTC_REGISTER_MINUTES 0x02u
#define RTC_REGISTER_HOURS 0x04u
#define RTC_REGISTER_WEEKDAY 0x06u
#define RTC_REGISTER_DAY 0x07u
#define RTC_REGISTER_MONTH 0x08u
#define RTC_REGISTER_YEAR 0x09u
#define RTC_REGISTER_STATUS_A 0x0au
#define RTC_REGISTER_STATUS_B 0x0bu
#define RTC_REGISTER_STATUS_C 0x0cu
#define RTC_REGISTER_STATUS_D 0x0du
#define RTC_REGISTER_CENTURY 0x32u
#define RTC_REGISTER_ALT_CENTURY 0x37u
#define RTC_STATUS_B_SET 0x80u
#define RTC_STATUS_B_BINARY 0x04u
#define RTC_STATUS_B_24_HOUR 0x02u
#define RTC_STATUS_B_SUPPORTED_MASK 0x87u

static bool year_is_leap(uint16_t year)
{
	return (year % 4u) == 0u &&
	       ((year % 100u) != 0u || (year % 400u) == 0u);
}

static uint8_t days_in_month(uint16_t year, uint8_t month)
{
	static const uint8_t month_days[12] = {
		31u, 28u, 31u, 30u, 31u, 30u,
		31u, 31u, 30u, 31u, 30u, 31u,
	};

	if (month == 0u || month > ARRAY_SIZE(month_days))
		return 0u;
	if (month == 2u && year_is_leap(year))
		return 29u;
	return month_days[month - 1u];
}

static bool rtc_date_is_valid(const struct x86_chipset_rtc_state *rtc)
{
	uint8_t month_days;

	if (rtc->year == 0u || rtc->year > 9999u || rtc->month == 0u ||
	    rtc->month > 12u || rtc->weekday == 0u || rtc->weekday > 7u ||
	    rtc->hour > 23u || rtc->minute > 59u || rtc->second > 59u)
		return false;
	month_days = days_in_month(rtc->year, rtc->month);
	return rtc->day != 0u && rtc->day <= month_days;
}

static bool advance_days(struct x86_chipset_rtc_state *rtc, uint64_t days)
{
	uint64_t ignored_quotient;
	uint32_t weekday_delta;

	if (math64_div_u64_u32(days, 7u, &ignored_quotient,
				   &weekday_delta) != MATH64_OK)
		return false;
	rtc->weekday = (uint8_t)(((uint32_t)rtc->weekday - 1u +
				  weekday_delta) % 7u + 1u);
	while (days != 0u) {
		uint8_t month_days = days_in_month(rtc->year, rtc->month);
		uint8_t remaining = (uint8_t)(month_days - rtc->day);

		if (days <= remaining) {
			rtc->day = (uint8_t)(rtc->day + (uint8_t)days);
			return true;
		}
		days -= (uint64_t)remaining + 1u;
		rtc->day = 1u;
		if (rtc->month != 12u) {
			++rtc->month;
			continue;
		}
		if (rtc->year == 9999u)
			return false;
		rtc->month = 1u;
		++rtc->year;
	}
	return true;
}

static bool advance_seconds(struct x86_chipset_rtc_state *rtc,
			    uint64_t seconds)
{
	uint64_t elapsed_days;
	uint32_t second_of_day;
	uint32_t remainder;
	uint32_t current;

	if (seconds == 0u)
		return true;
	if (math64_div_u64_u32(seconds, 86400u, &elapsed_days, &remainder) !=
	    MATH64_OK)
		return false;
	current = (uint32_t)rtc->hour * 3600u +
		  (uint32_t)rtc->minute * 60u + rtc->second;
	second_of_day = current + remainder;
	if (second_of_day >= 86400u) {
		second_of_day -= 86400u;
		if (elapsed_days == (uint64_t)-1)
			return false;
		++elapsed_days;
	}
	rtc->hour = (uint8_t)(second_of_day / 3600u);
	second_of_day %= 3600u;
	rtc->minute = (uint8_t)(second_of_day / 60u);
	rtc->second = (uint8_t)(second_of_day % 60u);
	return advance_days(rtc, elapsed_days);
}

void x86_legacy_rtc_advance(struct x86_chipset_rtc_state *rtc,
			    uint64_t input_ticks)
{
	struct x86_chipset_rtc_state prepared;
	uint64_t seconds;
	uint32_t remainder;
	uint32_t fraction;

	if (rtc == NULL || input_ticks == 0u ||
	    (rtc->status_d & 0x80u) == 0u ||
	    (rtc->status_b & RTC_STATUS_B_SET) != 0u)
		return;
	prepared = *rtc;
	if (!rtc_date_is_valid(&prepared) ||
	    prepared.subsecond_ticks >= X86_LEGACY_PIT_INPUT_HZ ||
	    math64_div_u64_u32(input_ticks, X86_LEGACY_PIT_INPUT_HZ,
				   &seconds, &remainder) != MATH64_OK) {
		rtc->status_d = 0u;
		return;
	}
	fraction = prepared.subsecond_ticks + remainder;
	if (fraction >= X86_LEGACY_PIT_INPUT_HZ) {
		fraction -= X86_LEGACY_PIT_INPUT_HZ;
		if (seconds == (uint64_t)-1) {
			rtc->status_d = 0u;
			return;
		}
		++seconds;
	}
	if (!advance_seconds(&prepared, seconds)) {
		rtc->status_d = 0u;
		return;
	}
	prepared.subsecond_ticks = fraction;
	*rtc = prepared;
}

static uint8_t binary_to_bcd(uint8_t value)
{
	return (uint8_t)(((value / 10u) << 4u) | (value % 10u));
}

static bool decode_number(uint8_t encoded, bool binary, uint8_t *value)
{
	if (binary) {
		*value = encoded;
		return true;
	}
	if ((encoded & 0x0fu) > 9u || (encoded >> 4u) > 9u)
		return false;
	*value = (uint8_t)((encoded >> 4u) * 10u + (encoded & 0x0fu));
	return true;
}

static uint8_t encode_number(const struct x86_chipset_rtc_state *rtc,
			     uint8_t value)
{
	return (rtc->status_b & RTC_STATUS_B_BINARY) != 0u
		       ? value
		       : binary_to_bcd(value);
}

static uint8_t encode_hour(const struct x86_chipset_rtc_state *rtc)
{
	uint8_t hour;
	uint8_t result;

	if ((rtc->status_b & RTC_STATUS_B_24_HOUR) != 0u)
		return encode_number(rtc, rtc->hour);
	hour = (uint8_t)(rtc->hour % 12u);
	if (hour == 0u)
		hour = 12u;
	result = encode_number(rtc, hour);
	if (rtc->hour >= 12u)
		result |= 0x80u;
	return result;
}

static bool decode_hour(const struct x86_chipset_rtc_state *rtc,
			uint8_t encoded, uint8_t *hour)
{
	bool binary = (rtc->status_b & RTC_STATUS_B_BINARY) != 0u;
	uint8_t decoded;
	bool afternoon;

	if ((rtc->status_b & RTC_STATUS_B_24_HOUR) != 0u) {
		if (!decode_number(encoded, binary, &decoded) || decoded > 23u)
			return false;
		*hour = decoded;
		return true;
	}
	afternoon = (encoded & 0x80u) != 0u;
	if (!decode_number((uint8_t)(encoded & 0x7fu), binary, &decoded) ||
	    decoded == 0u || decoded > 12u)
		return false;
	if (decoded == 12u)
		decoded = 0u;
	*hour = (uint8_t)(decoded + (afternoon ? 12u : 0u));
	return true;
}

static uint8_t read_register(struct x86_chipset_rtc_state *rtc)
{
	switch (rtc->selected_register) {
	case RTC_REGISTER_SECONDS:
		return encode_number(rtc, rtc->second);
	case RTC_REGISTER_MINUTES:
		return encode_number(rtc, rtc->minute);
	case RTC_REGISTER_HOURS:
		return encode_hour(rtc);
	case RTC_REGISTER_WEEKDAY:
		return encode_number(rtc, rtc->weekday);
	case RTC_REGISTER_DAY:
		return encode_number(rtc, rtc->day);
	case RTC_REGISTER_MONTH:
		return encode_number(rtc, rtc->month);
	case RTC_REGISTER_YEAR:
		return encode_number(rtc, (uint8_t)(rtc->year % 100u));
	case RTC_REGISTER_CENTURY:
	case RTC_REGISTER_ALT_CENTURY:
		return encode_number(rtc, (uint8_t)(rtc->year / 100u));
	case RTC_REGISTER_STATUS_A:
		return (uint8_t)(rtc->status_a & 0x7fu);
	case RTC_REGISTER_STATUS_B:
		return rtc->status_b;
	case RTC_REGISTER_STATUS_C: {
		uint8_t result = rtc->status_c;

		rtc->status_c = 0u;
		return result;
	}
	case RTC_REGISTER_STATUS_D:
		return rtc->status_d;
	default:
		return rtc->cmos[rtc->selected_register];
	}
}

static void write_number(uint8_t encoded, bool binary, uint8_t maximum,
			 uint8_t *target)
{
	uint8_t decoded;

	if (decode_number(encoded, binary, &decoded) && decoded <= maximum)
		*target = decoded;
}

static bool decode_calendar_number(uint8_t encoded, bool binary,
				   uint8_t minimum, uint8_t maximum,
				   uint8_t *decoded)
{
	return decode_number(encoded, binary, decoded) && *decoded >= minimum &&
	       *decoded <= maximum;
}

static void write_register(struct x86_chipset_rtc_state *rtc, uint8_t value)
{
	bool binary = (rtc->status_b & RTC_STATUS_B_BINARY) != 0u;
	uint8_t decoded;
	uint16_t year;

	switch (rtc->selected_register) {
	case RTC_REGISTER_SECONDS:
		write_number(value, binary, 59u, &rtc->second);
		break;
	case RTC_REGISTER_MINUTES:
		write_number(value, binary, 59u, &rtc->minute);
		break;
	case RTC_REGISTER_HOURS:
		if (decode_hour(rtc, value, &decoded))
			rtc->hour = decoded;
		break;
	case RTC_REGISTER_WEEKDAY:
		if (decode_calendar_number(value, binary, 1u, 7u, &decoded))
			rtc->weekday = decoded;
		break;
	case RTC_REGISTER_DAY:
		if (decode_calendar_number(value, binary, 1u,
					   days_in_month(rtc->year,
							 rtc->month),
					   &decoded))
			rtc->day = decoded;
		break;
	case RTC_REGISTER_MONTH:
		if (decode_calendar_number(value, binary, 1u, 12u, &decoded) &&
		    rtc->day <= days_in_month(rtc->year, decoded))
			rtc->month = decoded;
		break;
	case RTC_REGISTER_YEAR:
		if (decode_number(value, binary, &decoded)) {
			year = (uint16_t)((rtc->year / 100u) * 100u + decoded);
			if (year != 0u && year <= 9999u &&
			    rtc->day <= days_in_month(year, rtc->month))
				rtc->year = year;
		}
		break;
	case RTC_REGISTER_CENTURY:
	case RTC_REGISTER_ALT_CENTURY:
		if (decode_number(value, binary, &decoded)) {
			year = (uint16_t)((uint16_t)decoded * 100u +
					  (rtc->year % 100u));
			if (year != 0u && year <= 9999u &&
			    rtc->day <= days_in_month(year, rtc->month))
				rtc->year = year;
		}
		break;
	case RTC_REGISTER_STATUS_A:
		rtc->status_a = (uint8_t)(value & 0x7fu);
		break;
	case RTC_REGISTER_STATUS_B:
		/* Interrupt and square-wave enables stay clear until delivery exists. */
		rtc->status_b = (uint8_t)(value & RTC_STATUS_B_SUPPORTED_MASK);
		if ((rtc->status_b & RTC_STATUS_B_SET) != 0u)
			rtc->status_c = 0u;
		break;
	case RTC_REGISTER_STATUS_C:
	case RTC_REGISTER_STATUS_D:
		break;
	default:
		rtc->cmos[rtc->selected_register] = value;
		break;
	}
}

enum x86_io_callback_status x86_legacy_rtc_read(
	kernel_object_handle_t context, uint16_t port,
	enum dos_io_width width, uint32_t *value)
{
	struct x86_legacy_chipset_owner *chipset;
	struct x86_chipset_rtc_state *rtc;

	chipset = x86_legacy_chipset_active_owner(context);
	if (chipset == NULL || width != DOS_IO_WIDTH_8 || value == NULL)
		return X86_IO_CALLBACK_FAULT;
	rtc = &chipset->rtc;
	if (port == X86_RTC_INDEX_PORT) {
		*value = (uint32_t)(rtc->selected_register |
				    (rtc->nmi_disabled != 0u ? 0x80u : 0u));
		return X86_IO_CALLBACK_OK;
	}
	if (port != X86_RTC_DATA_PORT)
		return X86_IO_CALLBACK_FAULT;
	*value = read_register(rtc);
	return X86_IO_CALLBACK_OK;
}

enum x86_io_callback_status x86_legacy_rtc_write(
	kernel_object_handle_t context, uint16_t port,
	enum dos_io_width width, uint32_t value)
{
	struct x86_legacy_chipset_owner *chipset;
	struct x86_chipset_rtc_state *rtc;

	chipset = x86_legacy_chipset_active_owner(context);
	if (chipset == NULL || width != DOS_IO_WIDTH_8 || value > 0xffu)
		return X86_IO_CALLBACK_FAULT;
	rtc = &chipset->rtc;
	if (port == X86_RTC_INDEX_PORT) {
		rtc->selected_register = (uint8_t)(value & 0x7fu);
		rtc->nmi_disabled = (uint8_t)((value & 0x80u) != 0u);
		return X86_IO_CALLBACK_OK;
	}
	if (port != X86_RTC_DATA_PORT)
		return X86_IO_CALLBACK_FAULT;
	write_register(rtc, (uint8_t)value);
	return X86_IO_CALLBACK_OK;
}

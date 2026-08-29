// SPDX-License-Identifier: GPL-2.0-only
/* Pure input-key to guest-visible set-1/set-2 sequence encoding. */
#include "private.h"

#define PS2_PREFIX_E0 0xe0u
#define PS2_PREFIX_E1 0xe1u
#define PS2_PREFIX_RELEASE 0xf0u
#define PS2_EXTENDED_BIT 0x100u

static const uint8_t set1_pause[] = {
	PS2_PREFIX_E1, 0x1du, 0x45u, PS2_PREFIX_E1, 0x9du, 0xc5u
};
static const uint8_t set1_print_make[] = {
	PS2_PREFIX_E0, 0x2au, PS2_PREFIX_E0, 0x37u
};
static const uint8_t set1_print_break[] = {
	PS2_PREFIX_E0, 0xb7u, PS2_PREFIX_E0, 0xaau
};
static const uint8_t set2_pause[] = {
	PS2_PREFIX_E1, 0x14u, 0x77u, PS2_PREFIX_E1,
	PS2_PREFIX_RELEASE, 0x14u, PS2_PREFIX_RELEASE, 0x77u
};
static const uint8_t set2_print_make[] = {
	PS2_PREFIX_E0, 0x12u, PS2_PREFIX_E0, 0x7cu
};
static const uint8_t set2_print_break[] = {
	PS2_PREFIX_E0, PS2_PREFIX_RELEASE, 0x7cu, PS2_PREFIX_E0,
	PS2_PREFIX_RELEASE, 0x12u
};

static void clear_sequence(struct guest_ps2_sequence *sequence)
{
	size_t index;

	for (index = 0u; index < ARRAY_SIZE(sequence->values); ++index)
		sequence->values[index] = 0u;
	sequence->count = 0u;
	for (index = 0u; index < ARRAY_SIZE(sequence->reserved); ++index)
		sequence->reserved[index] = 0u;
}

static bool reserved_is_zero(const uint8_t *reserved, size_t count)
{
	size_t index;

	for (index = 0u; index < count; ++index) {
		if (reserved[index] != 0u)
			return false;
	}
	return true;
}

static enum guest_ps2_encode_status copy_sequence(
	struct guest_ps2_sequence *output, const uint8_t *values, size_t count)
{
	size_t index;

	if (count == 0u || count > ARRAY_SIZE(output->values))
		return GUEST_PS2_ENCODE_UNSUPPORTED;
	for (index = 0u; index < count; ++index)
		output->values[index] = values[index];
	output->count = (uint8_t)count;
	return GUEST_PS2_ENCODE_OK;
}

static enum guest_ps2_encode_status encode_special(
	const struct input_event *event, bool set1,
	struct guest_ps2_sequence *sequence)
{
	if (event->code == INPUT_KEY_CODE_PAUSE) {
		if (event->value != (uint8_t)INPUT_KEY_PRESSED)
			return GUEST_PS2_ENCODE_NO_OUTPUT;
		return set1 ? copy_sequence(sequence, set1_pause,
					    ARRAY_SIZE(set1_pause))
			    : copy_sequence(sequence, set2_pause,
					    ARRAY_SIZE(set2_pause));
	}
	if (event->code != INPUT_KEY_CODE_SYSRQ)
		return GUEST_PS2_ENCODE_UNSUPPORTED;
	if (event->value == (uint8_t)INPUT_KEY_RELEASED)
		return set1 ? copy_sequence(sequence, set1_print_break,
					    ARRAY_SIZE(set1_print_break))
			    : copy_sequence(sequence, set2_print_break,
					    ARRAY_SIZE(set2_print_break));
	return set1 ? copy_sequence(sequence, set1_print_make,
				    ARRAY_SIZE(set1_print_make))
		    : copy_sequence(sequence, set2_print_make,
				    ARRAY_SIZE(set2_print_make));
}

static enum guest_ps2_encode_status encode_ordinary(
	const struct input_event *event, bool set1,
	struct guest_ps2_sequence *sequence)
{
	uint16_t set1_code;
	uint16_t set2_code;
	uint16_t encoded;
	uint8_t scan;
	bool extended;

	if (!guest_ps2_internal_lookup(event->code, &set1_code, &set2_code))
		return GUEST_PS2_ENCODE_UNSUPPORTED;
	encoded = set1 ? set1_code : set2_code;
	extended = (encoded & PS2_EXTENDED_BIT) != 0u;
	scan = (uint8_t)encoded;
	if (extended)
		sequence->values[sequence->count++] = PS2_PREFIX_E0;
	if (event->value == (uint8_t)INPUT_KEY_RELEASED) {
		if (set1) {
			scan |= 0x80u;
		} else {
			sequence->values[sequence->count++] =
				PS2_PREFIX_RELEASE;
		}
	}
	sequence->values[sequence->count++] = scan;
	return GUEST_PS2_ENCODE_OK;
}

enum guest_ps2_encode_status guest_ps2_internal_encode(
	const struct input_event *event,
	const struct x86_i8042_keyboard_mode *mode,
	struct guest_ps2_sequence *sequence)
{
	bool set1;

	if (event == NULL || mode == NULL || sequence == NULL)
		return GUEST_PS2_ENCODE_UNSUPPORTED;
	clear_sequence(sequence);
	if (event->type != (uint8_t)INPUT_EVENT_KEY || event->code == 0u ||
	    event->value > (uint8_t)INPUT_KEY_REPEATED ||
	    (event->flags & (uint8_t)~INPUT_EVENT_FLAG_MASK) != 0u ||
	    mode->scan_set == 0u || mode->scan_set > 3u ||
	    mode->translation_enabled > 1u || mode->scanning_enabled > 1u ||
	    mode->interface_enabled > 1u ||
	    !reserved_is_zero(mode->reserved, ARRAY_SIZE(mode->reserved)))
		return GUEST_PS2_ENCODE_UNSUPPORTED;
	/* The 8042 translation bit does not turn arbitrary set-3 semantics into
	 * the complete set-1 stream modeled here.  Reject it until set 3 has its
	 * own audited encoder instead of publishing plausible-looking bytes. */
	if (mode->scan_set == 3u)
		return GUEST_PS2_ENCODE_UNSUPPORTED;
	set1 = mode->translation_enabled != 0u || mode->scan_set == 1u;
	if (event->code == INPUT_KEY_CODE_PAUSE ||
	    event->code == INPUT_KEY_CODE_SYSRQ)
		return encode_special(event, set1, sequence);
	return encode_ordinary(event, set1, sequence);
}

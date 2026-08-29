// SPDX-License-Identifier: GPL-2.0-only
/*
 * Bounded translated-set-1 and raw-set-2 decoder.
 *
 * E0/E1 sequences, release events, translation responses and repeat state are
 * decoded through explicit bounded state.
 */
#include "private.h"

static const uint8_t translated_pause[] = {
	0xe1u, 0x1du, 0x45u, 0xe1u, 0x9du, 0xc5u
};
static const uint8_t translated_print_make[] = {
	0xe0u, 0x2au, 0xe0u, 0x37u
};
static const uint8_t translated_print_break[] = {
	0xe0u, 0xb7u, 0xe0u, 0xaau
};
static const uint8_t raw_pause[] = {
	0xe1u, 0x14u, 0x77u, 0xe1u, 0xf0u, 0x14u, 0xf0u, 0x77u
};
static const uint8_t raw_print_make[] = {
	0xe0u, 0x12u, 0xe0u, 0x7cu
};
static const uint8_t raw_print_break[] = {
	0xe0u, 0xf0u, 0x7cu, 0xe0u, 0xf0u, 0x12u
};

static bool sequence_equal(const uint8_t *left, const uint8_t *right,
			   size_t count)
{
	size_t index;

	for (index = 0u; index < count; ++index) {
		if (left[index] != right[index])
			return false;
	}
	return true;
}

static bool sequence_is_prefix(const struct atkbd_decode_state *state,
			       const uint8_t *sequence, size_t length)
{
	return state->sequence_length <= length &&
	       sequence_equal(state->sequence, sequence,
			      state->sequence_length);
}

static bool sequence_is_exact(const struct atkbd_decode_state *state,
			      const uint8_t *sequence, size_t length)
{
	return state->sequence_length == length &&
	       sequence_equal(state->sequence, sequence, length);
}

static void sequence_clear(struct atkbd_decode_state *state)
{
	size_t index;

	for (index = 0u; index < ARRAY_SIZE(state->sequence); ++index)
		state->sequence[index] = 0u;
	state->sequence_length = 0u;
	state->release = 0u;
	state->extended = 0u;
}

void atkbd_internal_decode_reset(struct atkbd_decode_state *state)
{
	size_t index;

	if (state == NULL)
		return;
	for (index = 0u; index < ARRAY_SIZE(state->pressed); ++index)
		state->pressed[index] = 0u;
	sequence_clear(state);
	state->translated_response_bits = 0u;
	for (index = 0u; index < ARRAY_SIZE(state->reserved); ++index)
		state->reserved[index] = 0u;
}

void atkbd_internal_decode_cancel_sequence(struct atkbd_decode_state *state)
{
	if (state == NULL)
		return;
	sequence_clear(state);
	state->translated_response_bits = 0u;
}

static bool key_is_pressed(const struct atkbd_decode_state *state,
			   input_key_code_t keycode)
{
	uint16_t word = (uint16_t)(keycode / 64u);
	uint8_t bit = (uint8_t)(keycode % 64u);

	return word < ARRAY_SIZE(state->pressed) &&
	       (state->pressed[word] & ((uint64_t)1u << bit)) != 0u;
}

static void key_set_pressed(struct atkbd_decode_state *state,
			    input_key_code_t keycode, bool pressed)
{
	uint16_t word = (uint16_t)(keycode / 64u);
	uint8_t bit = (uint8_t)(keycode % 64u);
	uint64_t mask = (uint64_t)1u << bit;

	if (word >= ARRAY_SIZE(state->pressed))
		return;
	if (pressed)
		state->pressed[word] |= mask;
	else
		state->pressed[word] &= ~mask;
}

static void decoded_key(struct atkbd_decode_state *state,
			struct atkbd_decoded_key *decoded,
			input_key_code_t keycode, uint32_t hardware_code,
			bool release, bool extended)
{
	uint8_t value;

	if (release) {
		value = INPUT_KEY_RELEASED;
		key_set_pressed(state, keycode, false);
	} else if (keycode != INPUT_KEY_CODE_RESERVED &&
		   key_is_pressed(state, keycode)) {
		value = INPUT_KEY_REPEATED;
	} else {
		value = INPUT_KEY_PRESSED;
		if (keycode != INPUT_KEY_CODE_RESERVED)
			key_set_pressed(state, keycode, true);
	}
	decoded->code = keycode;
	decoded->hardware_code = hardware_code;
	decoded->value = value;
	decoded->flags = extended ? INPUT_EVENT_EXTENDED : 0u;
}

static int translated_response_index(uint8_t data)
{
	static const uint8_t responses[] = {
		ATKBD_RET_BAT, ATKBD_RET_ERR, ATKBD_RET_ACK,
		ATKBD_RET_NAK, ATKBD_RET_HANJA, ATKBD_RET_HANGEUL
	};
	size_t index;

	for (index = 0u; index < ARRAY_SIZE(responses); ++index) {
		if ((data & 0x7fu) == (responses[index] & 0x7fu))
			return (int)index;
	}
	return -1;
}

static void translated_update_response_bit(struct atkbd_decode_state *state,
					   uint8_t data)
{
	int index = translated_response_index(data);
	uint8_t mask;

	if (index < 0)
		return;
	mask = (uint8_t)(1u << (uint8_t)index);
	if ((data & 0x80u) != 0u)
		state->translated_response_bits &= (uint8_t)~mask;
	else
		state->translated_response_bits |= mask;
}

static enum atkbd_decode_result decode_translated_ordinary(
	struct atkbd_decode_state *state, uint8_t data, bool extended,
	struct atkbd_decoded_key *decoded)
{
	bool release = (data & 0x80u) != 0u;
	uint8_t code = data & 0x7fu;
	input_key_code_t keycode =
		atkbd_internal_set1_keycode(code, extended);

	if (!extended)
		translated_update_response_bit(state, data);
	decoded_key(state, decoded, keycode,
		    (uint32_t)code | (extended ? 0x100u : 0u), release,
		    extended);
	return ATKBD_DECODE_EVENT;
}

static enum atkbd_decode_result decode_raw_ordinary(
	struct atkbd_decode_state *state, uint8_t code, bool release,
	bool extended, struct atkbd_decoded_key *decoded)
{
	uint16_t scan = (uint16_t)(code & 0x7fu) |
			((uint16_t)(code & 0x80u) << 1u);
	input_key_code_t keycode;

	if (extended)
		scan |= 0x80u;
	keycode = atkbd_internal_set2_keycode(scan);

	decoded_key(state, decoded, keycode,
		    (uint32_t)code | (extended ? 0x100u : 0u), release,
		    extended);
	return ATKBD_DECODE_EVENT;
}

static enum atkbd_decode_result translated_sequence(
	struct atkbd_decode_state *state, struct atkbd_decoded_key *decoded)
{
	if (sequence_is_exact(state, translated_pause,
			      ARRAY_SIZE(translated_pause))) {
		sequence_clear(state);
		decoded_key(state, decoded, INPUT_KEY_CODE_PAUSE, 0x00e11d45u,
			    false, true);
		/* Pause is a pulse and has no physical break sequence. */
		key_set_pressed(state, INPUT_KEY_CODE_PAUSE, false);
		return ATKBD_DECODE_EVENT;
	}
	if (sequence_is_exact(state, translated_print_make,
			      ARRAY_SIZE(translated_print_make))) {
		sequence_clear(state);
		decoded_key(state, decoded, INPUT_KEY_CODE_SYSRQ, 0x0000e037u,
			    false, true);
		return ATKBD_DECODE_EVENT;
	}
	if (sequence_is_exact(state, translated_print_break,
			      ARRAY_SIZE(translated_print_break))) {
		sequence_clear(state);
		decoded_key(state, decoded, INPUT_KEY_CODE_SYSRQ, 0x0000e037u,
			    true, true);
		return ATKBD_DECODE_EVENT;
	}
	if (sequence_is_prefix(state, translated_pause,
			       ARRAY_SIZE(translated_pause)) ||
	    sequence_is_prefix(state, translated_print_make,
			       ARRAY_SIZE(translated_print_make)) ||
	    sequence_is_prefix(state, translated_print_break,
			       ARRAY_SIZE(translated_print_break)))
		return ATKBD_DECODE_NO_EVENT;
	if (state->sequence_length == 2u && state->sequence[0] == 0xe0u) {
		uint8_t data = state->sequence[1];

		sequence_clear(state);
		return decode_translated_ordinary(state, data, true, decoded);
	}
	sequence_clear(state);
	return ATKBD_DECODE_MALFORMED;
}

static enum atkbd_decode_result raw_sequence(
	struct atkbd_decode_state *state, struct atkbd_decoded_key *decoded)
{
	if (sequence_is_exact(state, raw_pause, ARRAY_SIZE(raw_pause))) {
		sequence_clear(state);
		decoded_key(state, decoded, INPUT_KEY_CODE_PAUSE, 0xe11477u,
			    false, true);
		key_set_pressed(state, INPUT_KEY_CODE_PAUSE, false);
		return ATKBD_DECODE_EVENT;
	}
	if (sequence_is_exact(state, raw_print_make,
			      ARRAY_SIZE(raw_print_make))) {
		sequence_clear(state);
		decoded_key(state, decoded, INPUT_KEY_CODE_SYSRQ, 0xe012e07cu,
			    false, true);
		return ATKBD_DECODE_EVENT;
	}
	if (sequence_is_exact(state, raw_print_break,
			      ARRAY_SIZE(raw_print_break))) {
		sequence_clear(state);
		decoded_key(state, decoded, INPUT_KEY_CODE_SYSRQ, 0xe0f07c12u,
			    true, true);
		return ATKBD_DECODE_EVENT;
	}
	if (sequence_is_prefix(state, raw_pause, ARRAY_SIZE(raw_pause)) ||
	    sequence_is_prefix(state, raw_print_make,
			       ARRAY_SIZE(raw_print_make)) ||
	    sequence_is_prefix(state, raw_print_break,
			       ARRAY_SIZE(raw_print_break)))
		return ATKBD_DECODE_NO_EVENT;
	if (state->sequence[0] == ATKBD_RET_RELEASE &&
	    state->sequence_length == 2u) {
		uint8_t code = state->sequence[1];

		sequence_clear(state);
		return decode_raw_ordinary(state, code, true, false, decoded);
	}
	if (state->sequence[0] == ATKBD_RET_EMUL0) {
		if (state->sequence_length == 2u) {
			uint8_t code = state->sequence[1];

			sequence_clear(state);
			return decode_raw_ordinary(state, code, false, true,
						   decoded);
		}
		if (state->sequence_length == 3u &&
		    state->sequence[1] == ATKBD_RET_RELEASE) {
			uint8_t code = state->sequence[2];

			sequence_clear(state);
			return decode_raw_ordinary(state, code, true, true,
						   decoded);
		}
	}
	sequence_clear(state);
	return ATKBD_DECODE_MALFORMED;
}

enum atkbd_decode_result atkbd_internal_decode(
	uint8_t mode, struct atkbd_decode_state *state, uint8_t data,
	struct atkbd_decoded_key *decoded)
{
	if (state == NULL || decoded == NULL)
		return ATKBD_DECODE_MALFORMED;
	if (state->sequence_length != 0u) {
		if (state->sequence_length >= ARRAY_SIZE(state->sequence)) {
			sequence_clear(state);
			return ATKBD_DECODE_MALFORMED;
		}
		state->sequence[state->sequence_length++] = data;
		return mode == ATKBD_SCAN_TRANSLATED_SET1
			       ? translated_sequence(state, decoded)
			       : raw_sequence(state, decoded);
	}
	if (data == ATKBD_RET_EMUL0 || data == ATKBD_RET_EMUL1 ||
	    (mode == ATKBD_SCAN_RAW_SET2 && data == ATKBD_RET_RELEASE)) {
		state->sequence[0] = data;
		state->sequence_length = 1u;
		return ATKBD_DECODE_NO_EVENT;
	}
	return mode == ATKBD_SCAN_TRANSLATED_SET1
		       ? decode_translated_ordinary(state, data, false, decoded)
		       : decode_raw_ordinary(state, data, false, false, decoded);
}

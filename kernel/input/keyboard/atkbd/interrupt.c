// SPDX-License-Identifier: GPL-2.0-only
/*
 * IRQ-safe AT keyboard receive path.
 *
 * This path allocates nothing, blocks nowhere, and schedules writes for
 * process context.
 */
#include "private.h"

enum protocol_byte_kind {
	PROTOCOL_BYTE_NONE = 0,
	PROTOCOL_BYTE_BAT,
	PROTOCOL_BYTE_ERROR,
	PROTOCOL_BYTE_ACK,
	PROTOCOL_BYTE_NAK,
	PROTOCOL_BYTE_HANJA,
	PROTOCOL_BYTE_HANGEUL
};

static int translated_protocol_index(uint8_t data)
{
	static const uint8_t bytes[] = {
		ATKBD_RET_BAT, ATKBD_RET_ERR, ATKBD_RET_ACK,
		ATKBD_RET_NAK, ATKBD_RET_HANJA, ATKBD_RET_HANGEUL
	};
	size_t index;

	for (index = 0u; index < ARRAY_SIZE(bytes); ++index) {
		if (bytes[index] == data)
			return (int)index;
	}
	return -1;
}

static enum protocol_byte_kind protocol_byte(
	const struct atkbd_endpoint *endpoint, uint8_t data)
{
	int index;

	if (endpoint->decode.sequence_length != 0u)
		return PROTOCOL_BYTE_NONE;
	index = translated_protocol_index(data);
	if (index < 0)
		return PROTOCOL_BYTE_NONE;
	if (endpoint->config.scan_mode == ATKBD_SCAN_TRANSLATED_SET1 &&
	    (endpoint->decode.translated_response_bits &
	     (uint8_t)(1u << (uint8_t)index)) != 0u)
		return PROTOCOL_BYTE_NONE;
	switch (data) {
	case ATKBD_RET_BAT:
		return PROTOCOL_BYTE_BAT;
	case ATKBD_RET_ERR:
		return PROTOCOL_BYTE_ERROR;
	case ATKBD_RET_ACK:
		return PROTOCOL_BYTE_ACK;
	case ATKBD_RET_NAK:
		return PROTOCOL_BYTE_NAK;
	case ATKBD_RET_HANJA:
		return PROTOCOL_BYTE_HANJA;
	case ATKBD_RET_HANGEUL:
		return PROTOCOL_BYTE_HANGEUL;
	default:
		return PROTOCOL_BYTE_NONE;
	}
}

static enum serio_receive_result submit_key(
	struct atkbd_endpoint *endpoint, input_key_code_t code, uint8_t value,
	uint32_t hardware_code, uint8_t flags)
{
	enum input_status status = input_submit(
		endpoint->owner->config.input_core, &endpoint->input_binding,
		INPUT_EVENT_KEY, code, value, hardware_code, flags);

	switch (status) {
	case INPUT_OK:
	case INPUT_DEFERRED:
	case INPUT_UNAVAILABLE:
		return SERIO_RECEIVE_HANDLED;
	case INPUT_ACCESS_DENIED:
	case INPUT_HANDLER_FAULT:
		endpoint->downstream_drop_count =
			atkbd_internal_saturating_increment(
				endpoint->downstream_drop_count);
		return SERIO_RECEIVE_HANDLED;
	case INPUT_RETRY:
	case INPUT_BUSY:
	case INPUT_CAPACITY_EXHAUSTED:
	case INPUT_INVALID_STATE:
		endpoint->downstream_retry_count =
			atkbd_internal_saturating_increment(
				endpoint->downstream_retry_count);
		return SERIO_RECEIVE_DEFER;
	default:
		atkbd_internal_poison_endpoint(endpoint);
		return SERIO_RECEIVE_REJECTED;
	}
}

static enum serio_receive_result handle_protocol_byte(
	struct atkbd_endpoint *endpoint, enum protocol_byte_kind kind,
	uint8_t data)
{
	enum serio_receive_result result;

	switch (kind) {
	case PROTOCOL_BYTE_ACK:
		endpoint->ack_count =
			atkbd_internal_saturating_increment(endpoint->ack_count);
		if (endpoint->command.phase == ATKBD_COMMAND_WAIT_ACK) {
			endpoint->command.script_index++;
			/* ACK starts a fresh retry budget for the next byte. */
			endpoint->command.byte_naks = 0u;
			if (endpoint->command.script_index >=
			    endpoint->command.script_length) {
				endpoint->command.phase = ATKBD_COMMAND_COMPLETE;
				if (endpoint->command.kind ==
				    ATKBD_COMMAND_NEGOTIATE) {
					endpoint->negotiated = 1u;
					endpoint->enabled = 1u;
				} else if (endpoint->command.kind ==
					   ATKBD_COMMAND_ENABLE) {
					endpoint->enabled = 1u;
				} else if (endpoint->command.kind ==
					   ATKBD_COMMAND_DISABLE) {
					endpoint->enabled = 0u;
				}
			} else {
				endpoint->command.phase = ATKBD_COMMAND_READY;
			}
		}
		return SERIO_RECEIVE_HANDLED;
	case PROTOCOL_BYTE_NAK:
		endpoint->nak_count =
			atkbd_internal_saturating_increment(endpoint->nak_count);
		if (endpoint->command.phase == ATKBD_COMMAND_WAIT_ACK) {
			endpoint->command.byte_naks++;
			if (endpoint->command.byte_naks >=
			    endpoint->owner->config.command_nak_limit)
				endpoint->command.phase = ATKBD_COMMAND_FAILED;
			else
				endpoint->command.phase = ATKBD_COMMAND_READY;
		}
		return SERIO_RECEIVE_HANDLED;
	case PROTOCOL_BYTE_BAT:
		endpoint->bat_count =
			atkbd_internal_saturating_increment(endpoint->bat_count);
		endpoint->enabled = 0u;
		endpoint->negotiated = 0u;
		endpoint->reconnect_required = 1u;
		endpoint->reconnect_active = 0u;
		endpoint->reconnect_release_cursor = 0u;
		endpoint->command.phase = ATKBD_COMMAND_FAILED;
		/* Preserve the pressed bitmap until process-context reconnect can
		 * publish synthetic releases. Only the partial byte sequence is now
		 * unusable. Synthetic release prevents stuck keys while keeping
		 * all recovery work out of hard IRQ. */
		atkbd_internal_decode_cancel_sequence(&endpoint->decode);
		return SERIO_RECEIVE_HANDLED;
	case PROTOCOL_BYTE_ERROR:
		endpoint->malformed_count =
			atkbd_internal_saturating_increment(
				endpoint->malformed_count);
		return SERIO_RECEIVE_HANDLED;
	case PROTOCOL_BYTE_HANJA:
	case PROTOCOL_BYTE_HANGEUL:
		result = submit_key(endpoint,
			kind == PROTOCOL_BYTE_HANJA ? INPUT_KEY_CODE_HANJA
						   : INPUT_KEY_CODE_HANGEUL,
			INPUT_KEY_PRESSED, data, 0u);
		if (result == SERIO_RECEIVE_HANDLED)
			endpoint->decoded_count =
				atkbd_internal_saturating_increment(
					endpoint->decoded_count);
		return result;
	case PROTOCOL_BYTE_NONE:
	default:
		return SERIO_RECEIVE_REJECTED;
	}
}

static bool binding_is_current(const struct atkbd_endpoint *endpoint,
	const struct serio_port *port, const struct serio_driver *driver,
	const struct serio_raw_event *event)
{
	return endpoint->lifecycle_cookie == ATKBD_ENDPOINT_COOKIE &&
	       endpoint->phase == ATKBD_ENDPOINT_BOUND &&
	       endpoint->owner != NULL && endpoint->port == port &&
	       (endpoint->owner->phase == ATKBD_DRIVER_PREPARED ||
		endpoint->owner->phase == ATKBD_DRIVER_ACTIVE) &&
	       endpoint->owner->poisoned == 0u &&
	       &endpoint->owner->serio_driver == driver &&
	       endpoint->config.port_identity == event->port_identity &&
	       endpoint->port_generation == event->port_generation &&
	       driver->config.identity == event->driver_identity &&
	       port->binding_generation == event->binding_generation;
}

static void endpoint_guard_enter(const struct atkbd_endpoint *endpoint)
{
	if (endpoint->port->config.irq_enter != NULL)
		endpoint->port->config.irq_enter(
			endpoint->port->config.callback_context);
}

static void endpoint_guard_exit(const struct atkbd_endpoint *endpoint)
{
	if (endpoint->port->config.irq_exit != NULL)
		endpoint->port->config.irq_exit(
			endpoint->port->config.callback_context);
}

enum serio_receive_result atkbd_internal_interrupt(
	struct serio_port *port, struct serio_driver *driver,
	void *binding_context, const struct serio_raw_event *event)
{
	struct atkbd_endpoint *endpoint = binding_context;
	struct atkbd_decode_state next_state;
	struct atkbd_decoded_key decoded;
	enum atkbd_decode_result decode_result;
	enum protocol_byte_kind kind;
	enum serio_receive_result result = SERIO_RECEIVE_HANDLED;

	if (port == NULL || driver == NULL || endpoint == NULL || event == NULL ||
	    !binding_is_current(endpoint, port, driver, event))
		return SERIO_RECEIVE_REJECTED;
	endpoint_guard_enter(endpoint);
	if (!binding_is_current(endpoint, port, driver, event)) {
		endpoint_guard_exit(endpoint);
		return SERIO_RECEIVE_REJECTED;
	}
	if (endpoint->in_flight != 0u) {
		endpoint_guard_exit(endpoint);
		return SERIO_RECEIVE_DEFER;
	}
	endpoint->in_flight = 1u;
	endpoint_guard_exit(endpoint);
	if ((event->flags & SERIO_RAW_FLAG_MASK) != 0u) {
		endpoint->bad_frame_count =
			atkbd_internal_saturating_increment(
				endpoint->bad_frame_count);
		if ((event->flags & (SERIO_RAW_PARITY_ERROR |
				     SERIO_RAW_FRAME_ERROR)) != 0u &&
		    (event->flags & SERIO_RAW_TIMEOUT_ERROR) == 0u &&
		    port->config.write != NULL)
			endpoint->resend_pending = 1u;
		goto finish;
	}
	/* Any clean byte proves forward progress after a resend request. */
	if (endpoint->resend_pending == 0u)
		endpoint->resend_attempts = 0u;
	kind = protocol_byte(endpoint, event->data);
	if (kind != PROTOCOL_BYTE_NONE) {
		result = handle_protocol_byte(endpoint, kind, event->data);
		goto finish;
	}
	if (endpoint->enabled == 0u)
		goto finish;
	next_state = endpoint->decode;
	decode_result = atkbd_internal_decode(endpoint->config.scan_mode,
					      &next_state, event->data,
					      &decoded);
	if (decode_result == ATKBD_DECODE_NO_EVENT) {
		endpoint->decode = next_state;
		goto finish;
	}
	if (decode_result == ATKBD_DECODE_MALFORMED) {
		endpoint->decode = next_state;
		endpoint->malformed_count =
			atkbd_internal_saturating_increment(
				endpoint->malformed_count);
		goto finish;
	}
	if (decoded.code == INPUT_KEY_CODE_RESERVED) {
		endpoint->decode = next_state;
		endpoint->unknown_count =
			atkbd_internal_saturating_increment(endpoint->unknown_count);
		goto finish;
	}
	result = submit_key(endpoint, decoded.code, decoded.value,
			    decoded.hardware_code, decoded.flags);
	if (result != SERIO_RECEIVE_DEFER) {
		endpoint->decode = next_state;
		if (result == SERIO_RECEIVE_HANDLED) {
			endpoint->decoded_count =
				atkbd_internal_saturating_increment(
					endpoint->decoded_count);
			if (decoded.value == INPUT_KEY_REPEATED)
				endpoint->repeat_count =
					atkbd_internal_saturating_increment(
						endpoint->repeat_count);
		}
	}

finish:
	endpoint_guard_enter(endpoint);
	endpoint->in_flight = 0u;
	endpoint_guard_exit(endpoint);
	return result;
}

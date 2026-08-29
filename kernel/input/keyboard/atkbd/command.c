// SPDX-License-Identifier: GPL-2.0-only
/*
 * Bounded process-context PS/2 command state machine.
 *
 * DOS-C32 advances at most one write per explicit process-context call, with
 * all retry and response state retained in the endpoint.
 */
#include "private.h"

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

static bool command_can_start(const struct atkbd_command_state *command)
{
	return command->phase == ATKBD_COMMAND_IDLE ||
	       command->phase == ATKBD_COMMAND_COMPLETE ||
	       command->phase == ATKBD_COMMAND_FAILED;
}

static enum atkbd_status map_zero_commit_status(enum serio_status status)
{
	switch (status) {
	case SERIO_RETRY:
		return ATKBD_RETRY;
	case SERIO_UNAVAILABLE:
		return ATKBD_UNAVAILABLE;
	case SERIO_POISONED:
		return ATKBD_POISONED;
	default:
		return ATKBD_INVALID_STATE;
	}
}

static void mark_write_uncertain(struct atkbd_endpoint *endpoint)
{
	endpoint->write_uncertain = 1u;
	endpoint->enabled = 0u;
	endpoint->negotiated = 0u;
	endpoint->reconnect_required = 1u;
	endpoint->resend_pending = 0u;
	endpoint->command.phase = ATKBD_COMMAND_FAILED;
	atkbd_internal_decode_cancel_sequence(&endpoint->decode);
}

static enum atkbd_status committed_command_status(
	const struct atkbd_command_state *command, uint8_t script_index)
{
	switch (command->phase) {
	case ATKBD_COMMAND_WAIT_ACK:
		return ATKBD_WAITING;
	case ATKBD_COMMAND_COMPLETE:
		return ATKBD_OK;
	case ATKBD_COMMAND_READY:
		return command->script_index != script_index ? ATKBD_OK
						      : ATKBD_RETRY;
	case ATKBD_COMMAND_FAILED:
		return ATKBD_PROTOCOL_ERROR;
	default:
		return ATKBD_INVALID_STATE;
	}
}

static void append_command_byte(struct atkbd_command_state *command,
				uint8_t value)
{
	if (command->script_length < ARRAY_SIZE(command->script))
		command->script[command->script_length++] = value;
}

static bool build_command(struct atkbd_endpoint *endpoint,
			  enum atkbd_command_kind kind, uint8_t value,
			  struct atkbd_command_state *command)
{
	*command = (struct atkbd_command_state){
		.kind = (uint8_t)kind,
		.phase = ATKBD_COMMAND_READY,
	};
	switch (kind) {
	case ATKBD_COMMAND_NEGOTIATE:
		append_command_byte(command, ATKBD_CMD_DISABLE);
		if (endpoint->config.scan_mode == ATKBD_SCAN_RAW_SET2) {
			append_command_byte(command, ATKBD_CMD_SET_SCANSET);
			append_command_byte(command, 2u);
		}
		append_command_byte(command, ATKBD_CMD_SET_LEDS);
		append_command_byte(command, 0u);
		append_command_byte(command, ATKBD_CMD_SET_TYPEMATIC);
		append_command_byte(command, 0u);
		append_command_byte(command, ATKBD_CMD_ENABLE);
		break;
	case ATKBD_COMMAND_SET_LEDS:
		if ((value & (uint8_t)~0x07u) != 0u)
			return false;
		append_command_byte(command, ATKBD_CMD_SET_LEDS);
		append_command_byte(command, value);
		break;
	case ATKBD_COMMAND_SET_TYPEMATIC:
		if ((value & 0x80u) != 0u)
			return false;
		append_command_byte(command, ATKBD_CMD_SET_TYPEMATIC);
		append_command_byte(command, value);
		break;
	case ATKBD_COMMAND_ENABLE:
		append_command_byte(command, ATKBD_CMD_ENABLE);
		break;
	case ATKBD_COMMAND_DISABLE:
		append_command_byte(command, ATKBD_CMD_DISABLE);
		break;
	case ATKBD_COMMAND_NONE:
	default:
		return false;
	}
	return command->script_length != 0u &&
	       command->script_length <= ARRAY_SIZE(command->script);
}

enum atkbd_status atkbd_command_begin(
	struct atkbd_driver *driver,
	const struct atkbd_endpoint_reference *reference,
	enum atkbd_command_kind kind, uint8_t value)
{
	struct atkbd_endpoint *endpoint;
	struct atkbd_command_state command;
	enum atkbd_status status =
		atkbd_internal_reference_status(driver, reference, &endpoint);

	if (status != ATKBD_OK)
		return status;
	if (endpoint->write_uncertain != 0u)
		return ATKBD_WRITE_UNCERTAIN;
	if (endpoint->port->config.write == NULL)
		return ATKBD_UNAVAILABLE;
	endpoint_guard_enter(endpoint);
	if (endpoint->in_flight != 0u) {
		endpoint_guard_exit(endpoint);
		return ATKBD_BUSY;
	}
	if (!command_can_start(&endpoint->command)) {
		endpoint_guard_exit(endpoint);
		return ATKBD_BUSY;
	}
	if (!build_command(endpoint, kind, value, &command)) {
		endpoint_guard_exit(endpoint);
		return ATKBD_INVALID_ARGUMENT;
	}
	if (kind == ATKBD_COMMAND_NEGOTIATE) {
		endpoint->enabled = 0u;
		endpoint->negotiated = 0u;
		atkbd_internal_decode_reset(&endpoint->decode);
	}
	endpoint->command = command;
	endpoint_guard_exit(endpoint);
	return ATKBD_OK;
}

static enum atkbd_status process_resend(struct atkbd_endpoint *endpoint)
{
	struct serio_write_result result;

	if (endpoint->resend_attempts >=
	    endpoint->owner->config.command_write_limit) {
		endpoint->resend_pending = 0u;
		return ATKBD_PROTOCOL_ERROR;
	}
	endpoint->resend_attempts++;
	endpoint->resend_pending = 0u;
	result = serio_write(endpoint->port, ATKBD_CMD_RESEND);
	if (result.commit == SERIO_WRITE_UNCERTAIN) {
		mark_write_uncertain(endpoint);
		return ATKBD_WRITE_UNCERTAIN;
	}
	if (result.commit == SERIO_WRITE_COMMITTED) {
		endpoint->protocol_committed = 1u;
		if (result.status == SERIO_OK)
			return ATKBD_OK;
		return result.status == SERIO_POISONED ? ATKBD_POISONED
						      : ATKBD_INVALID_STATE;
	}
	if (result.status == SERIO_RETRY) {
		endpoint->resend_pending = 1u;
		return ATKBD_RETRY;
	}
	return map_zero_commit_status(result.status);
}

static enum atkbd_status process_command(struct atkbd_endpoint *endpoint)
{
	struct atkbd_command_state *command = &endpoint->command;
	struct serio_write_result result;
	uint8_t byte;
	uint8_t script_index;

	switch (command->phase) {
	case ATKBD_COMMAND_IDLE:
		return ATKBD_EMPTY;
	case ATKBD_COMMAND_WAIT_ACK:
		return ATKBD_WAITING;
	case ATKBD_COMMAND_COMPLETE:
		return ATKBD_OK;
	case ATKBD_COMMAND_FAILED:
		return ATKBD_PROTOCOL_ERROR;
	case ATKBD_COMMAND_READY:
		break;
	default:
		return ATKBD_INVALID_STATE;
	}
	if (command->script_index >= command->script_length) {
		command->phase = ATKBD_COMMAND_FAILED;
		return ATKBD_PROTOCOL_ERROR;
	}
	if (command->writes >= endpoint->owner->config.command_write_limit) {
		command->phase = ATKBD_COMMAND_FAILED;
		return ATKBD_PROTOCOL_ERROR;
	}
	script_index = command->script_index;
	byte = command->script[script_index];
	command->writes++;
	/* Publish the expected ACK before the device can answer the write. */
	command->phase = ATKBD_COMMAND_WAIT_ACK;
	result = serio_write(endpoint->port, byte);
	if (result.commit == SERIO_WRITE_UNCERTAIN) {
		mark_write_uncertain(endpoint);
		return ATKBD_WRITE_UNCERTAIN;
	}
	if (result.commit == SERIO_WRITE_COMMITTED) {
		endpoint->protocol_committed = 1u;
		return committed_command_status(command, script_index);
	}
	/* A response to a byte that provably did not commit is stale protocol
	 * state. It is not safe to overwrite a synchronous transition or retry. */
	if (command->phase != ATKBD_COMMAND_WAIT_ACK) {
		mark_write_uncertain(endpoint);
		return ATKBD_WRITE_UNCERTAIN;
	}
	if (result.status == SERIO_RETRY) {
		command->phase = ATKBD_COMMAND_READY;
		return ATKBD_RETRY;
	}
	command->phase = ATKBD_COMMAND_FAILED;
	return map_zero_commit_status(result.status);
}

enum atkbd_status atkbd_process(
	struct atkbd_driver *driver,
	const struct atkbd_endpoint_reference *reference)
{
	struct atkbd_endpoint *endpoint;
	enum atkbd_status status =
		atkbd_internal_reference_status(driver, reference, &endpoint);

	if (status != ATKBD_OK)
		return status;
	if (endpoint->write_uncertain != 0u)
		return ATKBD_WRITE_UNCERTAIN;
	endpoint_guard_enter(endpoint);
	if (endpoint->in_flight != 0u) {
		endpoint_guard_exit(endpoint);
		return ATKBD_BUSY;
	}
	endpoint->in_flight = 1u;
	endpoint_guard_exit(endpoint);
	if (endpoint->reconnect_required != 0u) {
		enum serio_status serio_status =
			serio_port_reconnect(endpoint->port);

		if (serio_status == SERIO_OK)
			status = ATKBD_OK;
		else
			status = serio_status == SERIO_RETRY ? ATKBD_RETRY
							  : ATKBD_INVALID_STATE;
		goto finish;
	}
	if (endpoint->resend_pending != 0u)
		status = process_resend(endpoint);
	else
		status = process_command(endpoint);

finish:
	endpoint_guard_enter(endpoint);
	endpoint->in_flight = 0u;
	endpoint_guard_exit(endpoint);
	return status;
}

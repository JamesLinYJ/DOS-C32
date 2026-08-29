// SPDX-License-Identifier: GPL-2.0-only
/* Native i8042 serio backend; initialization and command-byte policy stay out. */
#include "x86_native_input.h"

#include "../../../config/x86-native-input.h"

#define NATIVE_INPUT_GENERATION_MAX ((uint64_t)-2)
#define NATIVE_INPUT_COOKIE 0x4e495043u
#define NATIVE_INPUT_AUXILIARY_WRITE_COMMAND 0xd4u

static_assert_expression(CONFIG_X86_NATIVE_INPUT_ENDPOINT_COUNT == 2u,
			 "native input routing has keyboard and aux endpoints");

enum native_input_phase {
	NATIVE_INPUT_EMPTY = 0,
	NATIVE_INPUT_PREPARED,
	NATIVE_INPUT_ACTIVE,
	NATIVE_INPUT_QUIESCING,
	NATIVE_INPUT_QUIESCED,
	NATIVE_INPUT_POISONED
};

static bool identity_is_valid(kernel_object_handle_t identity)
{
	return identity != 0u && identity != KERNEL_OBJECT_HANDLE_INVALID;
}

static bool bytes_are_zero(const uint8_t *bytes, size_t count)
{
	size_t index;

	for (index = 0u; index < count; ++index) {
		if (bytes[index] != 0u)
			return false;
	}
	return true;
}

static bool one_bit_or_zero(uint8_t value)
{
	return value == 0u || (value & (uint8_t)(value - 1u)) == 0u;
}

static bool endpoint_is_valid(uint8_t present,
			      kernel_object_handle_t identity,
			      const struct serio_raw_event *queue,
			      uint16_t capacity)
{
	if (present == 0u)
		return identity == KERNEL_OBJECT_HANDLE_INVALID && queue == NULL &&
		       capacity == 0u;
	return identity_is_valid(identity) && queue != NULL && capacity != 0u;
}

static bool absent_config_is_valid(const struct x86_native_input_config *c)
{
	return c->presence_evidence == X86_NATIVE_INPUT_EVIDENCE_NONE &&
	       c->keyboard_present == 0u && c->auxiliary_present == 0u &&
	       c->read8 == NULL && c->write8 == NULL && c->data_port == 0u &&
	       c->status_port == 0u && c->command_port == 0u &&
	       c->write_poll_limit == 0u && c->writes_supported == 0u &&
	       c->keyboard_port_identity == KERNEL_OBJECT_HANDLE_INVALID &&
	       c->auxiliary_port_identity == KERNEL_OBJECT_HANDLE_INVALID &&
	       c->keyboard_queue == NULL && c->auxiliary_queue == NULL &&
	       c->keyboard_queue_capacity == 0u &&
	       c->auxiliary_queue_capacity == 0u &&
	       c->status_allowed_mask == 0u &&
	       c->status_output_full_mask == 0u &&
	       c->status_input_full_mask == 0u &&
	       c->status_auxiliary_mask == 0u && c->status_parity_mask == 0u &&
	       c->status_timeout_mask == 0u && c->status_frame_mask == 0u;
}

static bool present_config_is_valid(const struct x86_native_input_config *c)
{
	uint8_t semantics;
	uint16_t sum;

	if (c->read8 == NULL || c->data_port == 0u || c->status_port == 0u ||
	    c->data_port == c->status_port ||
	    c->presence_evidence == X86_NATIVE_INPUT_EVIDENCE_NONE ||
	    (c->keyboard_present == 0u && c->auxiliary_present == 0u) ||
	    !endpoint_is_valid(c->keyboard_present, c->keyboard_port_identity,
			       c->keyboard_queue, c->keyboard_queue_capacity) ||
	    !endpoint_is_valid(c->auxiliary_present, c->auxiliary_port_identity,
			       c->auxiliary_queue, c->auxiliary_queue_capacity) ||
	    (c->keyboard_present != 0u && c->auxiliary_present != 0u &&
	     c->keyboard_port_identity == c->auxiliary_port_identity) ||
	    !one_bit_or_zero(c->status_output_full_mask) ||
	    c->status_output_full_mask == 0u ||
	    !one_bit_or_zero(c->status_input_full_mask) ||
	    !one_bit_or_zero(c->status_auxiliary_mask) ||
	    c->status_auxiliary_mask == 0u ||
	    !one_bit_or_zero(c->status_parity_mask) ||
	    c->status_parity_mask == 0u ||
	    !one_bit_or_zero(c->status_timeout_mask) ||
	    c->status_timeout_mask == 0u || !one_bit_or_zero(c->status_frame_mask))
		return false;
	if (c->writes_supported != 0u) {
		if (c->write8 == NULL || c->write_poll_limit == 0u ||
		    c->write_poll_limit >
			    CONFIG_X86_NATIVE_INPUT_WRITE_POLL_LIMIT_MAX ||
		    c->command_port == 0u || c->status_input_full_mask == 0u)
			return false;
	} else if (c->write8 != NULL || c->write_poll_limit != 0u ||
		   c->command_port != 0u || c->status_input_full_mask != 0u) {
		return false;
	}
	semantics = c->status_output_full_mask | c->status_auxiliary_mask |
		    c->status_input_full_mask | c->status_parity_mask |
		    c->status_timeout_mask |
		    c->status_frame_mask;
	sum = (uint16_t)c->status_output_full_mask +
	      c->status_input_full_mask + c->status_auxiliary_mask +
	      c->status_parity_mask + c->status_timeout_mask +
	      c->status_frame_mask;
	return (semantics & c->status_allowed_mask) == semantics &&
	       sum == (uint16_t)semantics;
}

static bool config_is_valid(const struct x86_native_input_config *c)
{
	bool guarded;

	if (c == NULL || !identity_is_valid(c->controller_identity) ||
	    c->registry == NULL || c->present > 1u || c->keyboard_present > 1u ||
	    c->auxiliary_present > 1u || c->caller_serializes_irq > 1u ||
	    c->writes_supported > 1u ||
	    c->presence_evidence >
		    X86_NATIVE_INPUT_EVIDENCE_CONTROLLER_OBSERVED ||
	    !bytes_are_zero(c->reserved, ARRAY_SIZE(c->reserved)) ||
	    (c->irq_enter == NULL) != (c->irq_exit == NULL))
		return false;
	guarded = c->irq_enter != NULL;
	if (!guarded && c->caller_serializes_irq == 0u)
		return false;
	return c->present != 0u ? present_config_is_valid(c)
				: absent_config_is_valid(c);
}

static uint64_t saturating_increment(uint64_t value)
{
	return value == (uint64_t)-1 ? value : value + 1u;
}

static uint64_t saturating_add(uint64_t left, uint64_t right)
{
	return (uint64_t)-1 - left < right ? (uint64_t)-1 : left + right;
}

static enum x86_native_input_status map_serio(enum serio_status status)
{
	switch (status) {
	case SERIO_OK:
		return X86_NATIVE_INPUT_OK;
	case SERIO_EMPTY:
		return X86_NATIVE_INPUT_EMPTY;
	case SERIO_RETRY:
		return X86_NATIVE_INPUT_RETRY;
	case SERIO_STREAM_LOST:
		return X86_NATIVE_INPUT_STREAM_LOST;
	case SERIO_RECOVERY_PENDING:
		return X86_NATIVE_INPUT_RECOVERY_PENDING;
	case SERIO_STREAM_ISOLATED:
		return X86_NATIVE_INPUT_STREAM_ISOLATED;
	case SERIO_UNAVAILABLE:
	case SERIO_NOT_FOUND:
		return X86_NATIVE_INPUT_UNAVAILABLE;
	case SERIO_CAPACITY_EXHAUSTED:
		return X86_NATIVE_INPUT_CAPACITY_EXHAUSTED;
	case SERIO_IDENTITY_MISMATCH:
		return X86_NATIVE_INPUT_IDENTITY_MISMATCH;
	case SERIO_POISONED:
		return X86_NATIVE_INPUT_POISONED;
	case SERIO_INVALID_ARGUMENT:
		return X86_NATIVE_INPUT_INVALID_ARGUMENT;
	default:
		return X86_NATIVE_INPUT_INVALID_STATE;
	}
}

static enum x86_native_input_status owner_status(
	const struct x86_native_input_controller *controller,
	kernel_object_handle_t identity)
{
	if (controller == NULL ||
	    controller->lifecycle_cookie != NATIVE_INPUT_COOKIE ||
	    !identity_is_valid(identity))
		return X86_NATIVE_INPUT_INVALID_ARGUMENT;
	if (controller->phase == NATIVE_INPUT_EMPTY)
		return X86_NATIVE_INPUT_INVALID_STATE;
	if (controller->config.controller_identity != identity)
		return X86_NATIVE_INPUT_IDENTITY_MISMATCH;
	return controller->phase == NATIVE_INPUT_POISONED
		       ? X86_NATIVE_INPUT_POISONED
		       : X86_NATIVE_INPUT_OK;
}

static void clear_controller(struct x86_native_input_controller *controller)
{
	uint64_t generation = controller->generation;
	uint64_t keyboard_generation = controller->keyboard_port.generation;
	uint64_t auxiliary_generation = controller->auxiliary_port.generation;
	uint8_t *bytes = (uint8_t *)controller;
	size_t index;

	for (index = 0u; index < sizeof(*controller); ++index)
		bytes[index] = 0u;
	controller->generation = generation;
	controller->lifecycle_cookie = NATIVE_INPUT_COOKIE;
	serio_port_construct(&controller->keyboard_port);
	serio_port_construct(&controller->auxiliary_port);
	controller->keyboard_port.generation = keyboard_generation;
	controller->auxiliary_port.generation = auxiliary_generation;
}

static struct serio_write_result write_result(enum serio_status status,
					      enum serio_write_commit commit)
{
	const struct serio_write_result result = {
		.status = status,
		.commit = commit,
	};

	return result;
}

static void finish_write_guard(
	const struct x86_native_input_controller *controller)
{
	if (controller->config.irq_exit != NULL)
		controller->config.irq_exit(controller->config.callback_context);
}

static struct serio_write_result native_write_byte(struct serio_port *port,
						    bool auxiliary,
						    uint8_t data)
{
	struct x86_native_input_controller *controller = port->config.port_context;
	enum serio_write_commit failure_commit = SERIO_WRITE_ZERO_COMMIT;
	enum x86_native_input_status status;
	uint32_t attempt;
	uint8_t controller_status;

	if (controller == NULL || controller->phase != NATIVE_INPUT_ACTIVE ||
	    controller->config.writes_supported == 0u)
		return write_result(SERIO_UNAVAILABLE, SERIO_WRITE_ZERO_COMMIT);
	if (controller->config.irq_enter != NULL)
		controller->config.irq_enter(controller->config.callback_context);
	/* A complete IBF preflight proves that no write has been attempted yet. */
	for (attempt = 0u; attempt < controller->config.write_poll_limit;
	     ++attempt) {
		status = controller->config.read8(
			controller->config.callback_context,
			controller->config.status_port, &controller_status);
		if (status != X86_NATIVE_INPUT_OK)
			goto io_error;
		if ((controller_status &
		     controller->config.status_input_full_mask) == 0u)
			break;
	}
	if (attempt == controller->config.write_poll_limit)
		goto timeout;
	if (auxiliary) {
		/* 0xd4 changes the meaning of the next data-port write.  Once its
		 * callback is attempted, a failure cannot prove that routing state was
		 * left untouched, so replaying the whole operation is unsafe. */
		failure_commit = SERIO_WRITE_UNCERTAIN;
		status = controller->config.write8(
			controller->config.callback_context,
			controller->config.command_port,
			NATIVE_INPUT_AUXILIARY_WRITE_COMMAND);
		if (status != X86_NATIVE_INPUT_OK)
			goto io_error;
		for (attempt = 0u; attempt < controller->config.write_poll_limit;
		     ++attempt) {
			status = controller->config.read8(
				controller->config.callback_context,
				controller->config.status_port, &controller_status);
			if (status != X86_NATIVE_INPUT_OK)
				goto io_error;
			if ((controller_status &
			     controller->config.status_input_full_mask) == 0u)
				break;
		}
		if (attempt == controller->config.write_poll_limit)
			goto timeout;
	}
	/* The data callback is itself the commit boundary.  A callback failure
	 * cannot prove whether the physical port observed the byte. */
	failure_commit = SERIO_WRITE_UNCERTAIN;
	status = controller->config.write8(controller->config.callback_context,
					  controller->config.data_port, data);
	if (status != X86_NATIVE_INPUT_OK)
		goto io_error;
	controller->write_count = saturating_increment(controller->write_count);
	finish_write_guard(controller);
	return write_result(SERIO_OK, SERIO_WRITE_COMMITTED);

timeout:
	controller->write_timeout_count =
		saturating_increment(controller->write_timeout_count);
	finish_write_guard(controller);
	return write_result(SERIO_RETRY, failure_commit);

io_error:
	controller->write_error_count =
		saturating_increment(controller->write_error_count);
	finish_write_guard(controller);
	return write_result(SERIO_INVALID_STATE, failure_commit);
}

static struct serio_write_result native_keyboard_write(struct serio_port *port,
							uint8_t data)
{
	return native_write_byte(port, false, data);
}

static struct serio_write_result native_auxiliary_write(
	struct serio_port *port, uint8_t data)
{
	return native_write_byte(port, true, data);
}

void x86_native_input_construct(
	struct x86_native_input_controller *controller)
{
	uint8_t *bytes = (uint8_t *)controller;
	size_t index;

	if (controller == NULL)
		return;
	for (index = 0u; index < sizeof(*controller); ++index)
		bytes[index] = 0u;
	controller->lifecycle_cookie = NATIVE_INPUT_COOKIE;
	serio_port_construct(&controller->keyboard_port);
	serio_port_construct(&controller->auxiliary_port);
}

static void make_port_config(struct x86_native_input_controller *controller,
			     const struct x86_native_input_config *config, bool auxiliary,
			     struct serio_port_config *port_config)
{
	port_config->identity = auxiliary ? config->auxiliary_port_identity
					  : config->keyboard_port_identity;
	port_config->parent_identity = KERNEL_OBJECT_HANDLE_INVALID;
	port_config->device_id = auxiliary ? config->auxiliary_id
					   : config->keyboard_id;
	port_config->manual_bind = 0u;
	port_config->caller_serializes_irq = config->caller_serializes_irq;
	port_config->reserved[0] = 0u;
	port_config->reserved[1] = 0u;
	port_config->callback_context = config->callback_context;
	port_config->port_context = controller;
	port_config->irq_enter = config->irq_enter;
	port_config->irq_exit = config->irq_exit;
	port_config->start = NULL;
	port_config->stop = NULL;
	port_config->open = NULL;
	port_config->close = NULL;
	port_config->write = config->writes_supported != 0u
				     ? (auxiliary ? native_auxiliary_write
						  : native_keyboard_write)
				     : NULL;
	port_config->queue = auxiliary ? config->auxiliary_queue
				       : config->keyboard_queue;
	port_config->queue_capacity = auxiliary
					      ? config->auxiliary_queue_capacity
					      : config->keyboard_queue_capacity;
	port_config->reserved_capacity = 0u;
}

static enum serio_status abort_prepared_ports(
	struct x86_native_input_controller *controller)
{
	enum serio_status status;

	if (controller->config.auxiliary_present != 0u &&
	    controller->auxiliary_port.phase == SERIO_PORT_PREPARED) {
		status = serio_port_abort(&controller->auxiliary_port);
		if (status != SERIO_OK)
			return status;
	}
	if (controller->config.keyboard_present != 0u &&
	    controller->keyboard_port.phase == SERIO_PORT_PREPARED) {
		status = serio_port_abort(&controller->keyboard_port);
		if (status != SERIO_OK)
			return status;
	}
	return SERIO_OK;
}

static enum serio_status roll_back_published_ports(
	struct x86_native_input_controller *controller)
{
	enum serio_status status;

	if (controller->auxiliary_port.phase == SERIO_PORT_ACTIVE) {
		status = serio_port_quiesce(&controller->auxiliary_port);
		if (status != SERIO_OK)
			return status;
	}
	if (controller->auxiliary_port.phase == SERIO_PORT_QUIESCED) {
		status = serio_port_unregister(&controller->auxiliary_port);
		if (status != SERIO_OK)
			return status;
	}
	if (controller->keyboard_port.phase == SERIO_PORT_ACTIVE) {
		status = serio_port_quiesce(&controller->keyboard_port);
		if (status != SERIO_OK)
			return status;
	}
	if (controller->keyboard_port.phase == SERIO_PORT_QUIESCED) {
		status = serio_port_unregister(&controller->keyboard_port);
		if (status != SERIO_OK)
			return status;
	}
	return abort_prepared_ports(controller);
}

static bool port_can_unregister(const struct serio_port *port)
{
	return port->phase == SERIO_PORT_QUIESCED && port->queue_count == 0u &&
	       port->in_flight == 0u && port->registry != NULL &&
	       port->registry_slot < port->registry->port_capacity &&
	       port->registry->ports[port->registry_slot] == port;
}

enum x86_native_input_status x86_native_input_prepare(
	struct x86_native_input_controller *controller,
	const struct x86_native_input_config *config)
{
	struct serio_port_config port_config;
	enum serio_status result;

	if (controller == NULL ||
	    controller->lifecycle_cookie != NATIVE_INPUT_COOKIE ||
	    !config_is_valid(config))
		return X86_NATIVE_INPUT_INVALID_ARGUMENT;
	if (controller->phase == NATIVE_INPUT_POISONED)
		return X86_NATIVE_INPUT_POISONED;
	if (controller->phase != NATIVE_INPUT_EMPTY)
		return X86_NATIVE_INPUT_INVALID_STATE;
	if (controller->generation >= NATIVE_INPUT_GENERATION_MAX)
		return X86_NATIVE_INPUT_CAPACITY_EXHAUSTED;
	controller->generation++;
	clear_controller(controller);
	controller->config = *config;
	if (config->keyboard_present != 0u) {
		make_port_config(controller, config, false, &port_config);
		result = serio_port_prepare(config->registry,
					    &controller->keyboard_port,
					    &port_config);
		if (result != SERIO_OK)
			goto fail;
	}
	if (config->auxiliary_present != 0u) {
		make_port_config(controller, config, true, &port_config);
		result = serio_port_prepare(config->registry,
					    &controller->auxiliary_port,
					    &port_config);
		if (result != SERIO_OK) {
			if (abort_prepared_ports(controller) != SERIO_OK) {
				controller->phase = NATIVE_INPUT_POISONED;
				return X86_NATIVE_INPUT_POISONED;
			}
			goto fail;
		}
	}
	controller->phase = NATIVE_INPUT_PREPARED;
	return X86_NATIVE_INPUT_OK;

fail:
	clear_controller(controller);
	return map_serio(result);
}

enum x86_native_input_status x86_native_input_publish(
	struct x86_native_input_controller *controller,
	kernel_object_handle_t identity)
{
	enum x86_native_input_status status = owner_status(controller, identity);
	enum serio_status result;

	if (status != X86_NATIVE_INPUT_OK)
		return status;
	if (controller->phase != NATIVE_INPUT_PREPARED)
		return X86_NATIVE_INPUT_INVALID_STATE;
	if (controller->config.keyboard_present != 0u) {
		result = serio_port_publish(&controller->keyboard_port);
		if (result != SERIO_OK)
			return map_serio(result);
	}
	if (controller->config.auxiliary_present != 0u) {
		result = serio_port_publish(&controller->auxiliary_port);
		if (result != SERIO_OK) {
			if (roll_back_published_ports(controller) != SERIO_OK) {
				controller->phase = NATIVE_INPUT_POISONED;
				return X86_NATIVE_INPUT_POISONED;
			}
			controller->phase = NATIVE_INPUT_EMPTY;
			return map_serio(result);
		}
	}
	controller->phase = NATIVE_INPUT_ACTIVE;
	return X86_NATIVE_INPUT_OK;
}

enum x86_native_input_status x86_native_input_abort(
	struct x86_native_input_controller *controller,
	kernel_object_handle_t identity)
{
	enum x86_native_input_status status = owner_status(controller, identity);

	if (status != X86_NATIVE_INPUT_OK)
		return status;
	if (controller->phase != NATIVE_INPUT_PREPARED)
		return X86_NATIVE_INPUT_INVALID_STATE;
	if (abort_prepared_ports(controller) != SERIO_OK) {
		controller->phase = NATIVE_INPUT_POISONED;
		return X86_NATIVE_INPUT_POISONED;
	}
	clear_controller(controller);
	return X86_NATIVE_INPUT_OK;
}

enum x86_native_input_status x86_native_input_quiesce(
	struct x86_native_input_controller *controller,
	kernel_object_handle_t identity)
{
	enum x86_native_input_status status = owner_status(controller, identity);
	enum serio_status result;

	if (status != X86_NATIVE_INPUT_OK)
		return status;
	if (controller->phase != NATIVE_INPUT_ACTIVE &&
	    controller->phase != NATIVE_INPUT_QUIESCING)
		return X86_NATIVE_INPUT_INVALID_STATE;
	controller->phase = NATIVE_INPUT_QUIESCING;
	if (controller->config.keyboard_present != 0u &&
	    controller->keyboard_port.phase == SERIO_PORT_ACTIVE) {
		result = serio_port_quiesce(&controller->keyboard_port);
		if (result != SERIO_OK)
			return map_serio(result);
	}
	if (controller->config.auxiliary_present != 0u &&
	    controller->auxiliary_port.phase == SERIO_PORT_ACTIVE) {
		result = serio_port_quiesce(&controller->auxiliary_port);
		if (result != SERIO_OK)
			return map_serio(result);
	}
	controller->phase = NATIVE_INPUT_QUIESCED;
	return X86_NATIVE_INPUT_OK;
}

enum x86_native_input_status x86_native_input_retire(
	struct x86_native_input_controller *controller,
	kernel_object_handle_t identity)
{
	enum x86_native_input_status status = owner_status(controller, identity);
	enum serio_status result;

	if (status != X86_NATIVE_INPUT_OK)
		return status;
	if (controller->phase != NATIVE_INPUT_QUIESCED)
		return X86_NATIVE_INPUT_INVALID_STATE;
	if ((controller->config.keyboard_present != 0u &&
	     !port_can_unregister(&controller->keyboard_port)) ||
	    (controller->config.auxiliary_present != 0u &&
	     !port_can_unregister(&controller->auxiliary_port)))
		return X86_NATIVE_INPUT_RETRY;
	if (controller->config.auxiliary_present != 0u) {
		result = serio_port_unregister(&controller->auxiliary_port);
		if (result != SERIO_OK)
			return map_serio(result);
	}
	if (controller->config.keyboard_present != 0u) {
		result = serio_port_unregister(&controller->keyboard_port);
		if (result != SERIO_OK)
			return map_serio(result);
	}
	clear_controller(controller);
	return X86_NATIVE_INPUT_OK;
}

enum x86_native_input_status x86_native_input_poison(
	struct x86_native_input_controller *controller,
	kernel_object_handle_t identity)
{
	enum x86_native_input_status status = owner_status(controller, identity);

	if (status != X86_NATIVE_INPUT_OK)
		return status;
	if (controller->phase == NATIVE_INPUT_ACTIVE ||
	    controller->phase == NATIVE_INPUT_QUIESCING) {
		status = x86_native_input_quiesce(controller, identity);
		if (status != X86_NATIVE_INPUT_OK)
			return status;
	} else if (controller->phase == NATIVE_INPUT_PREPARED) {
		if (abort_prepared_ports(controller) != SERIO_OK) {
			controller->phase = NATIVE_INPUT_POISONED;
			return X86_NATIVE_INPUT_POISONED;
		}
	}
	controller->phase = NATIVE_INPUT_POISONED;
	return X86_NATIVE_INPUT_OK;
}

enum x86_native_input_status x86_native_input_capture(
	struct x86_native_input_controller *controller,
	kernel_object_handle_t identity)
{
	struct serio_port *port;
	enum x86_native_input_status status = owner_status(controller, identity);
	uint8_t raw_status;
	uint8_t data;
	uint8_t flags = 0u;
	uint64_t loss_epoch;
	enum serio_status serio_status;

	if (status != X86_NATIVE_INPUT_OK)
		return status;
	if (controller->phase != NATIVE_INPUT_ACTIVE)
		return X86_NATIVE_INPUT_INVALID_STATE;
	if (controller->config.present == 0u)
		return X86_NATIVE_INPUT_UNAVAILABLE;
	status = controller->config.read8(controller->config.callback_context,
					 controller->config.status_port,
					 &raw_status);
	if (status != X86_NATIVE_INPUT_OK)
		goto io_error;
	if ((raw_status & controller->config.status_output_full_mask) == 0u) {
		controller->empty_poll_count =
			saturating_increment(controller->empty_poll_count);
		return X86_NATIVE_INPUT_EMPTY;
	}
	if ((raw_status & controller->config.status_auxiliary_mask) != 0u) {
		port = controller->config.auxiliary_present != 0u
			       ? &controller->auxiliary_port
			       : NULL;
	} else {
		port = controller->config.keyboard_present != 0u
			       ? &controller->keyboard_port
			       : NULL;
	}
	/* Status is observational. Leave 60h untouched while an earlier consumed
	 * byte has made this stream discontinuous; the explicit process-context
	 * recovery owner must reset it before more hardware state is consumed. */
	if (port != NULL) {
		serio_status = serio_port_receive_preflight(port, &loss_epoch);
		if (serio_status == SERIO_RETRY)
			return X86_NATIVE_INPUT_SOURCE_BACKPRESSURE;
		if (serio_status != SERIO_OK)
			return map_serio(serio_status);
	}
	status = controller->config.read8(controller->config.callback_context,
					 controller->config.data_port, &data);
	if (status != X86_NATIVE_INPUT_OK)
		goto io_error;
	if ((raw_status & controller->config.status_parity_mask) != 0u)
		flags |= SERIO_RAW_PARITY_ERROR;
	if ((raw_status & controller->config.status_timeout_mask) != 0u)
		flags |= SERIO_RAW_TIMEOUT_ERROR;
	if (controller->config.status_frame_mask != 0u &&
	    (raw_status & controller->config.status_frame_mask) != 0u)
		flags |= SERIO_RAW_FRAME_ERROR;
	if ((raw_status &
	     (uint8_t)~controller->config.status_allowed_mask) != 0u)
		flags |= SERIO_RAW_STATUS_UNRECOGNIZED;
	if (port == NULL) {
		controller->unrouted_count =
			saturating_increment(controller->unrouted_count);
		return X86_NATIVE_INPUT_DROPPED;
	}
	serio_status = serio_interrupt(port, data, raw_status, flags);
	return map_serio(serio_status);

io_error:
	controller->io_error_count =
		saturating_increment(controller->io_error_count);
	return X86_NATIVE_INPUT_IO_ERROR;
}

enum x86_native_input_irq_result x86_native_input_irq(
	struct x86_native_input_controller *controller,
	kernel_object_handle_t identity)
{
	enum x86_native_input_status status =
		x86_native_input_capture(controller, identity);

	if (status == X86_NATIVE_INPUT_EMPTY ||
	    status == X86_NATIVE_INPUT_UNAVAILABLE)
		return X86_NATIVE_INPUT_IRQ_NONE;
	if (status == X86_NATIVE_INPUT_OK || status == X86_NATIVE_INPUT_DROPPED ||
	    status == X86_NATIVE_INPUT_RETRY ||
	    status == X86_NATIVE_INPUT_SOURCE_BACKPRESSURE ||
	    status == X86_NATIVE_INPUT_CAPACITY_EXHAUSTED ||
	    status == X86_NATIVE_INPUT_STREAM_LOST ||
	    status == X86_NATIVE_INPUT_STREAM_ISOLATED)
		return X86_NATIVE_INPUT_IRQ_HANDLED;
	return X86_NATIVE_INPUT_IRQ_FAULT;
}

static struct serio_port *recovery_port(
	struct x86_native_input_controller *controller,
	enum x86_native_input_endpoint endpoint)
{
	if (endpoint == X86_NATIVE_INPUT_KEYBOARD)
		return controller->config.keyboard_present != 0u
			       ? &controller->keyboard_port
			       : NULL;
	if (endpoint == X86_NATIVE_INPUT_AUXILIARY)
		return controller->config.auxiliary_present != 0u
			       ? &controller->auxiliary_port
			       : NULL;
	return NULL;
}

enum x86_native_input_status x86_native_input_recover_stream(
	struct x86_native_input_controller *controller,
	kernel_object_handle_t identity, enum x86_native_input_endpoint endpoint,
	uint64_t loss_epoch)
{
	enum x86_native_input_status status = owner_status(controller, identity);
	struct serio_port *port;
	enum serio_status serio_status;

	if (status != X86_NATIVE_INPUT_OK)
		return status;
	if (loss_epoch == 0u)
		return X86_NATIVE_INPUT_INVALID_ARGUMENT;
	if (controller->phase != NATIVE_INPUT_ACTIVE)
		return X86_NATIVE_INPUT_INVALID_STATE;
	port = recovery_port(controller, endpoint);
	if (port == NULL)
		return endpoint == X86_NATIVE_INPUT_KEYBOARD ||
			       endpoint == X86_NATIVE_INPUT_AUXILIARY
			       ? X86_NATIVE_INPUT_UNAVAILABLE
			       : X86_NATIVE_INPUT_INVALID_ARGUMENT;
	serio_status = serio_port_recover_stream(port, loss_epoch);
	return map_serio(serio_status);
}

enum x86_native_input_status x86_native_input_isolate_stream(
	struct x86_native_input_controller *controller,
	kernel_object_handle_t identity, enum x86_native_input_endpoint endpoint,
	uint64_t loss_epoch)
{
	enum x86_native_input_status status = owner_status(controller, identity);
	struct serio_port *port;
	enum serio_status serio_status;

	if (status != X86_NATIVE_INPUT_OK)
		return status;
	if (loss_epoch == 0u)
		return X86_NATIVE_INPUT_INVALID_ARGUMENT;
	if (controller->phase != NATIVE_INPUT_ACTIVE)
		return X86_NATIVE_INPUT_INVALID_STATE;
	port = recovery_port(controller, endpoint);
	if (port == NULL)
		return endpoint == X86_NATIVE_INPUT_KEYBOARD ||
			       endpoint == X86_NATIVE_INPUT_AUXILIARY
			       ? X86_NATIVE_INPUT_UNAVAILABLE
			       : X86_NATIVE_INPUT_INVALID_ARGUMENT;
	serio_status = serio_port_isolate_stream(port, loss_epoch);
	return map_serio(serio_status);
}

enum x86_native_input_status x86_native_input_snapshot(
	const struct x86_native_input_controller *controller,
	kernel_object_handle_t identity,
	struct x86_native_input_snapshot *snapshot)
{
	struct serio_port_snapshot keyboard_snapshot = {0};
	struct serio_port_snapshot auxiliary_snapshot = {0};

	if (controller == NULL || snapshot == NULL ||
	    controller->lifecycle_cookie != NATIVE_INPUT_COOKIE ||
	    !identity_is_valid(identity))
		return X86_NATIVE_INPUT_INVALID_ARGUMENT;
	if (controller->phase == NATIVE_INPUT_EMPTY)
		return X86_NATIVE_INPUT_INVALID_STATE;
	if (controller->config.controller_identity != identity)
		return X86_NATIVE_INPUT_IDENTITY_MISMATCH;
	if (controller->config.keyboard_present != 0u &&
	    serio_port_snapshot(&controller->keyboard_port,
				&keyboard_snapshot) != SERIO_OK)
		return X86_NATIVE_INPUT_INVALID_STATE;
	if (controller->config.auxiliary_present != 0u &&
	    serio_port_snapshot(&controller->auxiliary_port,
				&auxiliary_snapshot) != SERIO_OK)
		return X86_NATIVE_INPUT_INVALID_STATE;
	snapshot->controller_identity = controller->config.controller_identity;
	snapshot->generation = controller->generation;
	snapshot->empty_poll_count = controller->empty_poll_count;
	snapshot->io_error_count = controller->io_error_count;
	snapshot->unrouted_count = controller->unrouted_count;
	snapshot->write_count = controller->write_count;
	snapshot->write_timeout_count = controller->write_timeout_count;
	snapshot->write_error_count = controller->write_error_count;
	snapshot->stream_loss_count = saturating_add(
		keyboard_snapshot.stream_loss_epoch,
		auxiliary_snapshot.stream_loss_epoch);
	snapshot->stream_recovery_count = saturating_add(
		keyboard_snapshot.recovery_count,
		auxiliary_snapshot.recovery_count);
	snapshot->stream_isolation_count = saturating_add(
		keyboard_snapshot.isolation_count,
		auxiliary_snapshot.isolation_count);
	snapshot->keyboard_stream_loss_epoch =
		keyboard_snapshot.stream_loss_epoch;
	snapshot->keyboard_stream_recovery_epoch =
		keyboard_snapshot.stream_recovery_epoch;
	snapshot->auxiliary_stream_loss_epoch =
		auxiliary_snapshot.stream_loss_epoch;
	snapshot->auxiliary_stream_recovery_epoch =
		auxiliary_snapshot.stream_recovery_epoch;
	snapshot->data_port = controller->config.data_port;
	snapshot->status_port = controller->config.status_port;
	snapshot->command_port = controller->config.command_port;
	snapshot->present = controller->config.present;
	snapshot->keyboard_present = controller->config.keyboard_present;
	snapshot->auxiliary_present = controller->config.auxiliary_present;
	snapshot->presence_evidence = controller->config.presence_evidence;
	snapshot->writes_supported = controller->config.writes_supported;
	snapshot->prepared = controller->phase == NATIVE_INPUT_PREPARED ? 1u : 0u;
	snapshot->active = controller->phase == NATIVE_INPUT_ACTIVE ? 1u : 0u;
	snapshot->quiesced = controller->phase == NATIVE_INPUT_QUIESCED ? 1u : 0u;
	snapshot->poisoned = controller->phase == NATIVE_INPUT_POISONED ? 1u : 0u;
	snapshot->keyboard_recovery_required =
		keyboard_snapshot.recovery_required;
	snapshot->keyboard_stream_isolated =
		keyboard_snapshot.stream_isolated;
	snapshot->keyboard_recovery_abandoned =
		keyboard_snapshot.recovery_abandoned;
	snapshot->auxiliary_recovery_required =
		auxiliary_snapshot.recovery_required;
	snapshot->auxiliary_stream_isolated =
		auxiliary_snapshot.stream_isolated;
	snapshot->auxiliary_recovery_abandoned =
		auxiliary_snapshot.recovery_abandoned;
	return controller->phase == NATIVE_INPUT_POISONED
		       ? X86_NATIVE_INPUT_POISONED
		       : X86_NATIVE_INPUT_OK;
}

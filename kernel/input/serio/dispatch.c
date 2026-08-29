// SPDX-License-Identifier: GPL-2.0-only
/* Hard-IRQ direct delivery and generation-bound deferred FIFO. */
#include "serio.h"

#include "private.h"

static uint64_t saturating_increment(uint64_t value)
{
	return value == (uint64_t)-1 ? value : value + 1u;
}

static void port_guard_enter(const struct serio_port *port)
{
	if (port->config.irq_enter != NULL)
		port->config.irq_enter(port->config.callback_context);
}

static void port_guard_exit(const struct serio_port *port)
{
	if (port->config.irq_exit != NULL)
		port->config.irq_exit(port->config.callback_context);
}

static enum serio_status queue_event(struct serio_port *port,
				     const struct serio_raw_event *event)
{
	uint16_t index;

	if (port->queue_count >= port->config.queue_capacity) {
		port->overflow_count = saturating_increment(port->overflow_count);
		return SERIO_CAPACITY_EXHAUSTED;
	}
	index = (uint16_t)((port->queue_head + port->queue_count) %
			   port->config.queue_capacity);
	port->config.queue[index] = *event;
	port->queue_count++;
	return SERIO_OK;
}

/* The caller has already consumed this byte from its hardware source. */
static enum serio_status mark_stream_loss(struct serio_port *port)
{
	port->lost_byte_count = saturating_increment(port->lost_byte_count);
	if (port->recovery_required != 0u)
		return port->recovery_abandoned != 0u
			       ? SERIO_STREAM_ISOLATED
			       : SERIO_STREAM_LOST;
	if (port->stream_loss_epoch >= SERIO_GENERATION_MAX) {
		port->accepting = 0u;
		port->stream_isolated = 1u;
		port->recovery_abandoned = 1u;
		port->phase = SERIO_PORT_POISONED;
		return SERIO_POISONED;
	}
	port->stream_loss_epoch++;
	port->recovery_required = 1u;
	port->stream_isolated = 1u;
	port->recovery_abandoned = 0u;
	port->accepting = 0u;
	return SERIO_STREAM_LOST;
}

enum serio_status serio_port_receive_preflight(const struct serio_port *port,
					       uint64_t *loss_epoch)
{
	enum serio_status status;

	if (port == NULL || port->lifecycle_cookie != SERIO_PORT_COOKIE ||
	    loss_epoch == NULL)
		return SERIO_INVALID_ARGUMENT;
	port_guard_enter(port);
	if (port->phase == SERIO_PORT_POISONED) {
		status = SERIO_POISONED;
	} else if (port->phase != SERIO_PORT_ACTIVE) {
		status = SERIO_INVALID_STATE;
	} else if (port->recovery_required != 0u) {
		status = port->recovery_abandoned != 0u
				 ? SERIO_STREAM_ISOLATED
				 : SERIO_STREAM_LOST;
	} else if (port->queue_count >= port->config.queue_capacity) {
		/* A non-empty FIFO owns ordering, so no direct-delivery path can
		 * consume the next source byte. This is zero-commit backpressure. */
		status = SERIO_RETRY;
	} else if (port->next_sequence >= SERIO_SEQUENCE_MAX) {
		status = SERIO_CAPACITY_EXHAUSTED;
	} else if (port->accepting == 0u) {
		status = SERIO_INVALID_STATE;
	} else {
		status = SERIO_OK;
	}
	*loss_epoch = port->stream_loss_epoch;
	port_guard_exit(port);
	return status;
}

enum serio_status serio_interrupt(struct serio_port *port, uint8_t data,
				  uint8_t raw_status, uint8_t flags)
{
	struct serio_raw_event event;
	struct serio_driver *driver;
	void *binding_context;
	enum serio_receive_result result;
	enum serio_status status;
	uint8_t reserved_index;

	if (port == NULL || port->lifecycle_cookie != SERIO_PORT_COOKIE ||
	    (flags & (uint8_t)~SERIO_RAW_FLAG_MASK) != 0u)
		return SERIO_INVALID_ARGUMENT;
	if (port->phase == SERIO_PORT_POISONED ||
	    (port->registry != NULL && port->registry->poisoned != 0u))
		return SERIO_POISONED;
	port_guard_enter(port);
	if (port->phase != SERIO_PORT_ACTIVE) {
		port_guard_exit(port);
		return SERIO_INVALID_STATE;
	}
	if (port->recovery_required != 0u) {
		status = mark_stream_loss(port);
		port_guard_exit(port);
		return status;
	}
	if (port->accepting == 0u) {
		port_guard_exit(port);
		return SERIO_INVALID_STATE;
	}
	if (port->next_sequence >= SERIO_SEQUENCE_MAX) {
		port->overflow_count = saturating_increment(port->overflow_count);
		status = mark_stream_loss(port);
		port_guard_exit(port);
		return status;
	}
	driver = port->driver;
	binding_context = port->binding_context;
	event.port_identity = port->config.identity;
	event.driver_identity = driver != NULL ? driver->config.identity
					       : KERNEL_OBJECT_HANDLE_INVALID;
	event.port_generation = port->generation;
	event.binding_generation = port->binding_generation;
	event.sequence = port->next_sequence + 1u;
	event.data = data;
	event.raw_status = raw_status;
	event.flags = flags;
	for (reserved_index = 0u;
	     reserved_index < ARRAY_SIZE(event.reserved); ++reserved_index)
		event.reserved[reserved_index] = 0u;
	port->next_sequence = event.sequence;
	port->received_count = saturating_increment(port->received_count);
	if (driver == NULL || driver->withdrawing != 0u) {
		port->unbound_count = saturating_increment(port->unbound_count);
		port_guard_exit(port);
		return SERIO_UNAVAILABLE;
	}
	/* Once a byte is deferred, every later byte joins the same FIFO.  Calling
	 * the driver here would let a newer byte pass the queued head. */
	if (port->queue_count != 0u) {
		port->deferred_count = saturating_increment(port->deferred_count);
		status = queue_event(port, &event);
		if (status == SERIO_CAPACITY_EXHAUSTED)
			status = mark_stream_loss(port);
		port_guard_exit(port);
		return status == SERIO_OK ? SERIO_RETRY : status;
	}
	if (port->in_flight == (uint8_t)-1) {
		port->overflow_count = saturating_increment(port->overflow_count);
		status = mark_stream_loss(port);
		port_guard_exit(port);
		return status;
	}
	port->in_flight++;
	port_guard_exit(port);

	result = driver->config.interrupt(port, driver, binding_context, &event);
	port_guard_enter(port);
	port->in_flight--;
	if (result == SERIO_RECEIVE_HANDLED) {
		port_guard_exit(port);
		return SERIO_OK;
	}
	if (result == SERIO_RECEIVE_REJECTED) {
		port->rejected_count = saturating_increment(port->rejected_count);
		port_guard_exit(port);
		return SERIO_OK;
	}
	if (result != SERIO_RECEIVE_DEFER) {
		port->rejected_count = saturating_increment(port->rejected_count);
		port_guard_exit(port);
		return SERIO_INVALID_STATE;
	}
	port->deferred_count = saturating_increment(port->deferred_count);
	status = queue_event(port, &event);
	if (status == SERIO_CAPACITY_EXHAUSTED)
		status = mark_stream_loss(port);
	port_guard_exit(port);
	return status == SERIO_OK ? SERIO_RETRY : status;
}

enum serio_status serio_port_pump(struct serio_port *port,
				  uint16_t budget, uint16_t *delivered)
{
	uint16_t completed = 0u;

	if (port == NULL || port->lifecycle_cookie != SERIO_PORT_COOKIE ||
	    delivered == NULL || budget == 0u)
		return SERIO_INVALID_ARGUMENT;
	*delivered = 0u;
	while (completed < budget) {
		struct serio_raw_event event;
		struct serio_driver *driver;
		void *binding_context;
		enum serio_receive_result result;

		port_guard_enter(port);
		if (port->phase != SERIO_PORT_ACTIVE) {
			port_guard_exit(port);
			return SERIO_INVALID_STATE;
		}
		if (port->recovery_required != 0u) {
			enum serio_status stream_status =
				port->recovery_abandoned != 0u
					? SERIO_STREAM_ISOLATED
					: SERIO_STREAM_LOST;

			port_guard_exit(port);
			return stream_status;
		}
		if (port->accepting == 0u) {
			port_guard_exit(port);
			return SERIO_INVALID_STATE;
		}
		if (port->queue_count == 0u) {
			port_guard_exit(port);
			*delivered = completed;
			return completed == 0u ? SERIO_EMPTY : SERIO_OK;
		}
		driver = port->driver;
		binding_context = port->binding_context;
		if (driver == NULL || driver->withdrawing != 0u) {
			port_guard_exit(port);
			*delivered = completed;
			return SERIO_RETRY;
		}
		event = port->config.queue[port->queue_head];
		if (event.port_generation != port->generation ||
		    event.binding_generation != port->binding_generation ||
		    event.driver_identity != driver->config.identity) {
			port->queue_head =
				(uint16_t)((port->queue_head + 1u) %
					   port->config.queue_capacity);
			port->queue_count--;
			port->stale_binding_drop_count = saturating_increment(
				port->stale_binding_drop_count);
			port_guard_exit(port);
			continue;
		}
		if (port->in_flight == (uint8_t)-1) {
			port_guard_exit(port);
			*delivered = completed;
			return SERIO_CAPACITY_EXHAUSTED;
		}
		port->in_flight++;
		port_guard_exit(port);
		result = driver->config.interrupt(port, driver, binding_context,
						  &event);
		port_guard_enter(port);
		port->in_flight--;
		if (result == SERIO_RECEIVE_DEFER) {
			port_guard_exit(port);
			*delivered = completed;
			return SERIO_RETRY;
		}
		port->queue_head = (uint16_t)((port->queue_head + 1u) %
					      port->config.queue_capacity);
		port->queue_count--;
		if (result == SERIO_RECEIVE_REJECTED) {
			port->rejected_count =
				saturating_increment(port->rejected_count);
		} else if (result != SERIO_RECEIVE_HANDLED) {
			port->rejected_count =
				saturating_increment(port->rejected_count);
			port_guard_exit(port);
			*delivered = completed;
			return SERIO_INVALID_STATE;
		}
		port_guard_exit(port);
		completed++;
	}
	*delivered = completed;
	return SERIO_OK;
}

enum serio_status serio_port_peek(struct serio_port *port,
				  struct serio_raw_event *event)
{
	if (port == NULL || port->lifecycle_cookie != SERIO_PORT_COOKIE ||
	    event == NULL)
		return SERIO_INVALID_ARGUMENT;
	if (port->phase != SERIO_PORT_ACTIVE &&
	    port->phase != SERIO_PORT_QUIESCED)
		return port->phase == SERIO_PORT_POISONED ? SERIO_POISONED
						       : SERIO_INVALID_STATE;
	port_guard_enter(port);
	if (port->queue_count == 0u) {
		port_guard_exit(port);
		return SERIO_EMPTY;
	}
	*event = port->config.queue[port->queue_head];
	port_guard_exit(port);
	return SERIO_OK;
}

enum serio_status serio_port_consume(struct serio_port *port,
				     uint64_t sequence)
{
	if (port == NULL || port->lifecycle_cookie != SERIO_PORT_COOKIE ||
	    sequence == 0u)
		return SERIO_INVALID_ARGUMENT;
	if (port->phase != SERIO_PORT_ACTIVE &&
	    port->phase != SERIO_PORT_QUIESCED)
		return port->phase == SERIO_PORT_POISONED ? SERIO_POISONED
						       : SERIO_INVALID_STATE;
	port_guard_enter(port);
	if (port->queue_count == 0u) {
		port_guard_exit(port);
		return SERIO_EMPTY;
	}
	if (port->config.queue[port->queue_head].sequence != sequence) {
		port_guard_exit(port);
		return SERIO_STALE_EVENT;
	}
	port->queue_head = (uint16_t)((port->queue_head + 1u) %
				      port->config.queue_capacity);
	port->queue_count--;
	port_guard_exit(port);
	return SERIO_OK;
}

enum serio_status serio_port_snapshot(const struct serio_port *port,
				       struct serio_port_snapshot *snapshot)
{
	if (port == NULL || port->lifecycle_cookie != SERIO_PORT_COOKIE ||
	    snapshot == NULL)
		return SERIO_INVALID_ARGUMENT;
	port_guard_enter(port);
	if (port->phase == SERIO_PORT_EMPTY) {
		port_guard_exit(port);
		return SERIO_INVALID_STATE;
	}
	snapshot->identity = port->config.identity;
	snapshot->parent_identity = port->config.parent_identity;
	snapshot->driver_identity = port->driver != NULL
				    ? port->driver->config.identity
				    : KERNEL_OBJECT_HANDLE_INVALID;
	snapshot->generation = port->generation;
	snapshot->binding_generation = port->binding_generation;
	snapshot->received_count = port->received_count;
	snapshot->deferred_count = port->deferred_count;
	snapshot->rejected_count = port->rejected_count;
	snapshot->overflow_count = port->overflow_count;
	snapshot->unbound_count = port->unbound_count;
	snapshot->stale_binding_drop_count = port->stale_binding_drop_count;
	snapshot->stream_loss_epoch = port->stream_loss_epoch;
	snapshot->stream_recovery_epoch = port->stream_recovery_epoch;
	snapshot->lost_byte_count = port->lost_byte_count;
	snapshot->recovery_discard_count = port->recovery_discard_count;
	snapshot->recovery_count = port->recovery_count;
	snapshot->isolation_count = port->isolation_count;
	snapshot->parent_generation = port->parent_generation;
	snapshot->device_id = port->config.device_id;
	snapshot->queue_capacity = port->config.queue_capacity;
	snapshot->queue_count = port->queue_count;
	snapshot->depth = port->depth;
	snapshot->phase = port->phase;
	snapshot->accepting = port->accepting;
	snapshot->in_flight = port->in_flight;
	snapshot->recovery_required = port->recovery_required;
	snapshot->stream_isolated = port->stream_isolated;
	snapshot->recovery_abandoned = port->recovery_abandoned;
	if (port->phase == SERIO_PORT_POISONED) {
		port_guard_exit(port);
		return SERIO_POISONED;
	}
	port_guard_exit(port);
	return SERIO_OK;
}

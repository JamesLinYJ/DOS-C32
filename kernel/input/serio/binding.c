// SPDX-License-Identifier: GPL-2.0-only
/* Port/driver matching, binding rollback and parent/child lifecycle. */
#include "serio.h"

#include "../../../config/serio.h"
#include "private.h"

static bool identity_is_valid(kernel_object_handle_t identity)
{
	return identity != 0u && identity != KERNEL_OBJECT_HANDLE_INVALID;
}

static uint64_t saturating_increment(uint64_t value)
{
	return value == (uint64_t)-1 ? value : value + 1u;
}

static uint64_t saturating_add_u16(uint64_t value, uint16_t addend)
{
	return (uint64_t)-1 - value < addend ? (uint64_t)-1
						  : value + addend;
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

static bool registry_is_usable(const struct serio_registry *registry)
{
	return registry != NULL &&
	       registry->lifecycle_cookie == SERIO_REGISTRY_COOKIE &&
	       registry->active != 0u && registry->poisoned == 0u &&
	       identity_is_valid(registry->identity);
}

static bool port_config_is_valid(const struct serio_port_config *config)
{
	bool has_enter;
	bool has_exit;

	if (config == NULL || !identity_is_valid(config->identity) ||
	    config->identity == config->parent_identity ||
	    config->manual_bind > 1u || config->caller_serializes_irq > 1u ||
	    !bytes_are_zero(config->reserved, ARRAY_SIZE(config->reserved)) ||
	    config->queue == NULL || config->queue_capacity == 0u ||
	    config->reserved_capacity != 0u)
		return false;
	has_enter = config->irq_enter != NULL;
	has_exit = config->irq_exit != NULL;
	if (has_enter != has_exit)
		return false;
	return has_enter || config->caller_serializes_irq != 0u;
}

static bool driver_config_is_valid(const struct serio_driver_config *config)
{
	if (config == NULL || !identity_is_valid(config->identity) ||
	    config->matches == NULL || config->match_count == 0u ||
	    config->manual_bind > 1u ||
	    !bytes_are_zero(config->reserved, ARRAY_SIZE(config->reserved)) ||
	    config->connect == NULL || config->disconnect == NULL ||
	    config->interrupt == NULL)
		return false;
	return true;
}

static struct serio_port *find_port(const struct serio_registry *registry,
				    kernel_object_handle_t identity)
{
	uint16_t slot;

	for (slot = 0u; slot < registry->port_capacity; ++slot) {
		if (registry->ports[slot] != NULL &&
		    registry->ports[slot]->config.identity == identity)
			return registry->ports[slot];
	}
	return NULL;
}

static struct serio_driver *find_driver(const struct serio_registry *registry,
					kernel_object_handle_t identity)
{
	uint16_t slot;

	for (slot = 0u; slot < registry->driver_capacity; ++slot) {
		if (registry->drivers[slot] != NULL &&
		    registry->drivers[slot]->config.identity == identity)
			return registry->drivers[slot];
	}
	return NULL;
}

static bool id_field_matches(uint8_t wanted, uint8_t actual)
{
	return wanted == SERIO_MATCH_ANY || wanted == actual;
}

static bool driver_matches_port(const struct serio_driver *driver,
				const struct serio_port *port)
{
	size_t index;

	for (index = 0u; index < driver->config.match_count; ++index) {
		const struct serio_device_id *match =
			&driver->config.matches[index];

		if (id_field_matches(match->type, port->config.device_id.type) &&
		    id_field_matches(match->protocol,
				     port->config.device_id.protocol) &&
		    id_field_matches(match->id, port->config.device_id.id) &&
		    id_field_matches(match->extra,
				     port->config.device_id.extra))
			return true;
	}
	return false;
}

static void clear_port_preserving_generation(struct serio_port *port)
{
	uint64_t generation = port->generation;
	uint32_t lifecycle_cookie = port->lifecycle_cookie;
	size_t byte;
	uint8_t *storage = (uint8_t *)port;

	for (byte = 0u; byte < sizeof(*port); ++byte)
		storage[byte] = 0u;
	port->generation = generation;
	port->lifecycle_cookie = lifecycle_cookie;
	port->registry_slot = SERIO_SLOT_INVALID;
}

static void clear_driver_preserving_generation(struct serio_driver *driver)
{
	uint64_t generation = driver->generation;
	uint32_t lifecycle_cookie = driver->lifecycle_cookie;
	size_t byte;
	uint8_t *storage = (uint8_t *)driver;

	for (byte = 0u; byte < sizeof(*driver); ++byte)
		storage[byte] = 0u;
	driver->generation = generation;
	driver->lifecycle_cookie = lifecycle_cookie;
	driver->registry_slot = SERIO_SLOT_INVALID;
}

static enum serio_status bind_driver(struct serio_port *port,
				     struct serio_driver *driver)
{
	enum serio_status status;
	void *binding_context = NULL;

	if (port->phase != SERIO_PORT_ACTIVE || port->driver != NULL ||
	    driver->registered == 0u || driver->withdrawing != 0u)
		return SERIO_INVALID_STATE;
	if (!driver_matches_port(driver, port))
		return SERIO_NOT_FOUND;

	status = driver->config.connect(port, driver, &binding_context);
	if (status != SERIO_OK)
		return status;
	if (port->binding_generation >= SERIO_GENERATION_MAX) {
		driver->config.disconnect(port, driver, binding_context);
		return SERIO_CAPACITY_EXHAUSTED;
	}
	/* Publish under the IRQ guard before open, matching serio's race order. */
	port_guard_enter(port);
	port->binding_generation++;
	port->binding_context = binding_context;
	port->driver = driver;
	port_guard_exit(port);
	if (port->config.open != NULL) {
		status = port->config.open(port, driver, binding_context);
		if (status != SERIO_OK) {
			port_guard_enter(port);
			if (port->in_flight != 0u) {
				port->accepting = 0u;
				port->phase = SERIO_PORT_POISONED;
				port_guard_exit(port);
				return SERIO_POISONED;
			}
			port->driver = NULL;
			port->binding_context = NULL;
			port_guard_exit(port);
			driver->config.disconnect(port, driver, binding_context);
			return status;
		}
	}
	return SERIO_OK;
}

static void detach_driver(struct serio_port *port)
{
	struct serio_driver *driver;
	void *binding_context;

	port_guard_enter(port);
	driver = port->driver;
	binding_context = port->binding_context;
	port->driver = NULL;
	port->binding_context = NULL;
	port_guard_exit(port);
	if (driver == NULL)
		return;
	if (port->config.close != NULL)
		port->config.close(port, driver, binding_context);
	driver->config.disconnect(port, driver, binding_context);
}

static enum serio_status attach_first_driver(struct serio_port *port)
{
	struct serio_registry *registry = port->registry;
	uint16_t slot;

	if (port->config.manual_bind != 0u)
		return SERIO_NOT_FOUND;
	for (slot = 0u; slot < registry->driver_capacity; ++slot) {
		struct serio_driver *driver = registry->drivers[slot];
		enum serio_status status;

		if (driver == NULL || driver->config.manual_bind != 0u ||
		    driver->withdrawing != 0u ||
		    !driver_matches_port(driver, port))
			continue;
		status = bind_driver(port, driver);
		if (status == SERIO_OK)
			return SERIO_OK;
	}
	return SERIO_NOT_FOUND;
}

static bool port_has_children(const struct serio_port *port)
{
	const struct serio_registry *registry = port->registry;
	uint16_t slot;

	for (slot = 0u; slot < registry->port_capacity; ++slot) {
		const struct serio_port *candidate = registry->ports[slot];

		if (candidate != NULL && candidate != port &&
		    candidate->phase != SERIO_PORT_EMPTY &&
		    candidate->config.parent_identity == port->config.identity)
			return true;
	}
	return false;
}

enum serio_status serio_port_prepare(struct serio_registry *registry,
	struct serio_port *port, const struct serio_port_config *config)
{
	struct serio_port *parent = NULL;
	uint16_t slot;
	uint8_t depth = 0u;
	uint64_t generation;

	if (!registry_is_usable(registry))
		return registry != NULL && registry->poisoned != 0u
			       ? SERIO_POISONED
			       : SERIO_INVALID_STATE;
	if (port == NULL || port->lifecycle_cookie != SERIO_PORT_COOKIE ||
	    !port_config_is_valid(config))
		return SERIO_INVALID_ARGUMENT;
	if (port->phase == SERIO_PORT_POISONED)
		return SERIO_POISONED;
	if (port->phase != SERIO_PORT_EMPTY)
		return SERIO_ALREADY_REGISTERED;
	if (find_port(registry, config->identity) != NULL ||
	    find_driver(registry, config->identity) != NULL)
		return SERIO_ALREADY_REGISTERED;
	if (config->parent_identity != KERNEL_OBJECT_HANDLE_INVALID &&
	    config->parent_identity != 0u) {
		parent = find_port(registry, config->parent_identity);
		if (parent == NULL || parent->phase != SERIO_PORT_ACTIVE)
			return SERIO_NOT_FOUND;
		if (parent->depth >= CONFIG_SERIO_PARENT_DEPTH_MAX)
			return SERIO_CAPACITY_EXHAUSTED;
		depth = (uint8_t)(parent->depth + 1u);
	}
	for (slot = 0u; slot < registry->port_capacity; ++slot) {
		if (registry->ports[slot] == NULL)
			break;
	}
	if (slot == registry->port_capacity ||
	    port->generation >= SERIO_GENERATION_MAX)
		return SERIO_CAPACITY_EXHAUSTED;

	generation = port->generation + 1u;
	clear_port_preserving_generation(port);
	port->generation = generation;
	port->config = *config;
	port->registry = registry;
	port->registry_slot = slot;
	port->depth = depth;
	port->parent_generation = parent != NULL ? parent->generation : 0u;
	port->phase = SERIO_PORT_PREPARED;
	registry->ports[slot] = port;
	registry->port_count++;
	return SERIO_OK;
}

enum serio_status serio_port_publish(struct serio_port *port)
{
	enum serio_status status;

	if (port == NULL || port->lifecycle_cookie != SERIO_PORT_COOKIE)
		return SERIO_INVALID_ARGUMENT;
	if (!registry_is_usable(port->registry))
		return port->registry != NULL && port->registry->poisoned != 0u
			       ? SERIO_POISONED
			       : SERIO_INVALID_STATE;
	if (port->phase != SERIO_PORT_PREPARED)
		return SERIO_INVALID_STATE;
	if (port->config.start != NULL) {
		status = port->config.start(port);
		if (status != SERIO_OK)
			return status;
	}
	port_guard_enter(port);
	port->accepting = 1u;
	port->phase = SERIO_PORT_ACTIVE;
	port_guard_exit(port);
	(void)attach_first_driver(port);
	return SERIO_OK;
}

enum serio_status serio_port_abort(struct serio_port *port)
{
	struct serio_registry *registry;
	uint16_t slot;

	if (port == NULL || port->lifecycle_cookie != SERIO_PORT_COOKIE)
		return SERIO_INVALID_ARGUMENT;
	if (port->phase != SERIO_PORT_PREPARED || port->registry == NULL)
		return SERIO_INVALID_STATE;
	registry = port->registry;
	slot = port->registry_slot;
	if (slot >= registry->port_capacity || registry->ports[slot] != port)
		return SERIO_IDENTITY_MISMATCH;
	registry->ports[slot] = NULL;
	registry->port_count--;
	clear_port_preserving_generation(port);
	return SERIO_OK;
}

enum serio_status serio_port_quiesce(struct serio_port *port)
{
	if (port == NULL || port->lifecycle_cookie != SERIO_PORT_COOKIE)
		return SERIO_INVALID_ARGUMENT;
	if (port->phase == SERIO_PORT_POISONED)
		return SERIO_POISONED;
	if (port->phase != SERIO_PORT_ACTIVE)
		return SERIO_INVALID_STATE;
	if (port_has_children(port))
		return SERIO_PARENT_BUSY;
	port_guard_enter(port);
	if (port->recovery_required != 0u &&
	    port->recovery_abandoned == 0u) {
		port_guard_exit(port);
		return SERIO_STREAM_LOST;
	}
	port->accepting = 0u;
	if (port->in_flight != 0u) {
		port_guard_exit(port);
		return SERIO_RETRY;
	}
	port_guard_exit(port);
	detach_driver(port);
	if (port->config.stop != NULL)
		port->config.stop(port);
	port->phase = SERIO_PORT_QUIESCED;
	return SERIO_OK;
}

enum serio_status serio_port_unregister(struct serio_port *port)
{
	struct serio_registry *registry;
	uint16_t slot;

	if (port == NULL || port->lifecycle_cookie != SERIO_PORT_COOKIE)
		return SERIO_INVALID_ARGUMENT;
	if (port->phase != SERIO_PORT_QUIESCED || port->registry == NULL)
		return SERIO_INVALID_STATE;
	if (port->queue_count != 0u)
		return SERIO_DRIVER_BUSY;
	registry = port->registry;
	slot = port->registry_slot;
	if (slot >= registry->port_capacity || registry->ports[slot] != port)
		return SERIO_IDENTITY_MISMATCH;
	registry->ports[slot] = NULL;
	registry->port_count--;
	clear_port_preserving_generation(port);
	return SERIO_OK;
}

static bool port_is_in_subtree(const struct serio_port *candidate,
			       const struct serio_port *root)
{
	const struct serio_port *current = candidate;
	uint8_t depth;

	for (depth = 0u; depth <= CONFIG_SERIO_PARENT_DEPTH_MAX; ++depth) {
		struct serio_port *parent;

		if (current == root)
			return true;
		if (current->config.parent_identity == 0u ||
		    current->config.parent_identity == KERNEL_OBJECT_HANDLE_INVALID)
			return false;
		parent = find_port(current->registry,
				   current->config.parent_identity);
		if (parent == NULL ||
		    current->parent_generation != parent->generation)
			return false;
		current = parent;
	}
	return false;
}

enum serio_status serio_port_unregister_subtree(struct serio_port *root)
{
	struct serio_registry *registry;
	uint16_t slot;
	uint8_t depth;

	if (root == NULL || root->lifecycle_cookie != SERIO_PORT_COOKIE ||
	    root->registry == NULL || root->phase == SERIO_PORT_EMPTY)
		return SERIO_INVALID_ARGUMENT;
	registry = root->registry;
	/* Complete no-side-effect preflight before stopping any member. */
	for (slot = 0u; slot < registry->port_capacity; ++slot) {
		struct serio_port *port = registry->ports[slot];

		if (port == NULL || !port_is_in_subtree(port, root))
			continue;
		port_guard_enter(port);
		if (port->in_flight != 0u || port->queue_count != 0u ||
		    (port->recovery_required != 0u &&
		     port->recovery_abandoned == 0u)) {
			port_guard_exit(port);
			return SERIO_RETRY;
		}
		port_guard_exit(port);
	}
	for (slot = 0u; slot < registry->port_capacity; ++slot) {
		struct serio_port *port = registry->ports[slot];

		if (port == NULL || !port_is_in_subtree(port, root))
			continue;
		port_guard_enter(port);
		port->accepting = 0u;
		port_guard_exit(port);
	}
	/* Highest depth first is the explicit child-before-parent contract. */
	for (depth = CONFIG_SERIO_PARENT_DEPTH_MAX + 1u; depth > 0u; --depth) {
		uint8_t target_depth = (uint8_t)(depth - 1u);

		for (slot = 0u; slot < registry->port_capacity; ++slot) {
			struct serio_port *port = registry->ports[slot];

			if (port == NULL || port->depth != target_depth ||
			    !port_is_in_subtree(port, root))
				continue;
			if (port->phase == SERIO_PORT_ACTIVE) {
				detach_driver(port);
				if (port->config.stop != NULL)
					port->config.stop(port);
			}
			registry->ports[slot] = NULL;
			registry->port_count--;
			clear_port_preserving_generation(port);
		}
	}
	return SERIO_OK;
}

enum serio_status serio_port_bind(struct serio_port *port,
				  kernel_object_handle_t driver_identity)
{
	struct serio_driver *driver;

	if (port == NULL || port->lifecycle_cookie != SERIO_PORT_COOKIE ||
	    !identity_is_valid(driver_identity))
		return SERIO_INVALID_ARGUMENT;
	if (port->phase != SERIO_PORT_ACTIVE || port->driver != NULL ||
	    !registry_is_usable(port->registry))
		return SERIO_INVALID_STATE;
	driver = find_driver(port->registry, driver_identity);
	if (driver == NULL)
		return SERIO_NOT_FOUND;
	return bind_driver(port, driver);
}

enum serio_status serio_port_unbind(struct serio_port *port)
{
	if (port == NULL || port->lifecycle_cookie != SERIO_PORT_COOKIE)
		return SERIO_INVALID_ARGUMENT;
	if (port->phase != SERIO_PORT_ACTIVE || port->driver == NULL)
		return SERIO_INVALID_STATE;
	port_guard_enter(port);
	if (port->recovery_required != 0u) {
		enum serio_status stream_status =
			port->recovery_abandoned != 0u
				? SERIO_STREAM_ISOLATED
				: SERIO_STREAM_LOST;

		port_guard_exit(port);
		return stream_status;
	}
	if (port->in_flight != 0u) {
		port_guard_exit(port);
		return SERIO_RETRY;
	}
	port_guard_exit(port);
	detach_driver(port);
	(void)attach_first_driver(port);
	return SERIO_OK;
}

enum serio_status serio_port_reconnect(struct serio_port *port)
{
	enum serio_status status;

	if (port == NULL || port->lifecycle_cookie != SERIO_PORT_COOKIE)
		return SERIO_INVALID_ARGUMENT;
	if (port->phase != SERIO_PORT_ACTIVE || port->driver == NULL)
		return SERIO_INVALID_STATE;
	if (port->recovery_required != 0u)
		return port->recovery_abandoned != 0u
			       ? SERIO_STREAM_ISOLATED
			       : SERIO_STREAM_LOST;
	if (port->driver->config.reconnect == NULL)
		status = SERIO_UNAVAILABLE;
	else
		status = port->driver->config.reconnect(
			port, port->driver, port->binding_context);
	if (status == SERIO_OK)
		return SERIO_OK;
	/* A typed zero-commit retry retains the same binding and any bounded
	 * driver reconnect cursor. Detaching here would destroy the only state
	 * capable of completing stuck-key release on the next process call. */
	if (status == SERIO_RETRY || status == SERIO_RECOVERY_PENDING)
		return SERIO_RETRY;
	status = serio_port_unbind(port);
	return status == SERIO_OK ? SERIO_RETRY : status;
}

static void discard_recovery_queue(struct serio_port *port)
{
	port->recovery_discard_count = saturating_add_u16(
		port->recovery_discard_count, port->queue_count);
	port->queue_head = 0u;
	port->queue_count = 0u;
}

static enum serio_status isolate_stream_locked(struct serio_port *port)
{
	discard_recovery_queue(port);
	port->accepting = 0u;
	port->stream_isolated = 1u;
	if (port->recovery_abandoned == 0u) {
		port->recovery_abandoned = 1u;
		port->isolation_count = saturating_increment(
			port->isolation_count);
	}
	return SERIO_STREAM_ISOLATED;
}

enum serio_status serio_port_recover_stream(struct serio_port *port,
					     uint64_t loss_epoch)
{
	struct serio_driver *driver;
	void *binding_context;
	uint64_t binding_generation;
	enum serio_status status;

	if (port == NULL || port->lifecycle_cookie != SERIO_PORT_COOKIE ||
	    loss_epoch == 0u)
		return SERIO_INVALID_ARGUMENT;
	port_guard_enter(port);
	if (port->phase == SERIO_PORT_POISONED) {
		port_guard_exit(port);
		return SERIO_POISONED;
	}
	if (port->phase != SERIO_PORT_ACTIVE) {
		port_guard_exit(port);
		return SERIO_INVALID_STATE;
	}
	if (port->stream_loss_epoch != loss_epoch) {
		port_guard_exit(port);
		return SERIO_STALE_EVENT;
	}
	if (port->recovery_required == 0u) {
		status = port->stream_recovery_epoch == loss_epoch
				 ? SERIO_OK
				 : SERIO_INVALID_STATE;
		port_guard_exit(port);
		return status;
	}
	if (port->recovery_abandoned != 0u) {
		port_guard_exit(port);
		return SERIO_STREAM_ISOLATED;
	}
	if (port->in_flight != 0u) {
		port_guard_exit(port);
		return SERIO_RECOVERY_PENDING;
	}
	discard_recovery_queue(port);
	driver = port->driver;
	binding_context = port->binding_context;
	if (driver == NULL || driver->withdrawing != 0u ||
	    driver->config.reconnect == NULL) {
		status = isolate_stream_locked(port);
		port_guard_exit(port);
		return status;
	}
	binding_generation = port->binding_generation;
	port->in_flight++;
	port_guard_exit(port);

	status = driver->config.reconnect(port, driver, binding_context);

	port_guard_enter(port);
	port->in_flight--;
	if (port->phase != SERIO_PORT_ACTIVE ||
	    port->recovery_required == 0u ||
	    port->stream_loss_epoch != loss_epoch || port->driver != driver ||
	    port->binding_context != binding_context ||
	    port->binding_generation != binding_generation) {
		port->accepting = 0u;
		port->stream_isolated = 1u;
		port->recovery_abandoned = 1u;
		port->phase = SERIO_PORT_POISONED;
		port_guard_exit(port);
		return SERIO_POISONED;
	}
	if (status == SERIO_OK) {
		if (port->next_sequence >= SERIO_SEQUENCE_MAX) {
			status = isolate_stream_locked(port);
			port_guard_exit(port);
			return status;
		}
		port->stream_recovery_epoch = loss_epoch;
		port->recovery_required = 0u;
		port->stream_isolated = 0u;
		port->recovery_abandoned = 0u;
		port->accepting = 1u;
		port->recovery_count = saturating_increment(
			port->recovery_count);
		port_guard_exit(port);
		return SERIO_OK;
	}
	if (status == SERIO_RETRY || status == SERIO_RECOVERY_PENDING) {
		port_guard_exit(port);
		return SERIO_RECOVERY_PENDING;
	}
	status = isolate_stream_locked(port);
	port_guard_exit(port);
	return status;
}

enum serio_status serio_port_isolate_stream(struct serio_port *port,
					     uint64_t loss_epoch)
{
	enum serio_status status;

	if (port == NULL || port->lifecycle_cookie != SERIO_PORT_COOKIE ||
	    loss_epoch == 0u)
		return SERIO_INVALID_ARGUMENT;
	port_guard_enter(port);
	if (port->phase == SERIO_PORT_POISONED) {
		port_guard_exit(port);
		return SERIO_POISONED;
	}
	if (port->phase != SERIO_PORT_ACTIVE ||
	    port->recovery_required == 0u) {
		port_guard_exit(port);
		return SERIO_INVALID_STATE;
	}
	if (port->stream_loss_epoch != loss_epoch) {
		port_guard_exit(port);
		return SERIO_STALE_EVENT;
	}
	if (port->in_flight != 0u) {
		port_guard_exit(port);
		return SERIO_RECOVERY_PENDING;
	}
	status = isolate_stream_locked(port);
	port_guard_exit(port);
	return status == SERIO_STREAM_ISOLATED ? SERIO_OK : status;
}

static struct serio_write_result write_result(enum serio_status status,
					       enum serio_write_commit commit)
{
	return (struct serio_write_result){
		.status = status,
		.commit = commit,
	};
}

struct serio_write_result serio_write(struct serio_port *port, uint8_t data)
{
	serio_port_write_fn write;

	if (port == NULL || port->lifecycle_cookie != SERIO_PORT_COOKIE)
		return write_result(SERIO_INVALID_ARGUMENT,
				    SERIO_WRITE_ZERO_COMMIT);
	port_guard_enter(port);
	if (port->phase != SERIO_PORT_ACTIVE || port->accepting == 0u ||
	    port->driver == NULL) {
		if (port->recovery_required != 0u) {
			enum serio_status stream_status =
				port->recovery_abandoned != 0u
					? SERIO_STREAM_ISOLATED
					: SERIO_STREAM_LOST;

			port_guard_exit(port);
			return write_result(stream_status,
					    SERIO_WRITE_ZERO_COMMIT);
		}
		port_guard_exit(port);
		return write_result(SERIO_INVALID_STATE,
				    SERIO_WRITE_ZERO_COMMIT);
	}
	write = port->config.write;
	if (write == NULL) {
		port_guard_exit(port);
		return write_result(SERIO_UNAVAILABLE,
				    SERIO_WRITE_ZERO_COMMIT);
	}
	if (port->in_flight == (uint8_t)-1) {
		port_guard_exit(port);
		return write_result(SERIO_CAPACITY_EXHAUSTED,
				    SERIO_WRITE_ZERO_COMMIT);
	}
	port->in_flight++;
	port_guard_exit(port);

	{
		struct serio_write_result result = write(port, data);

		port_guard_enter(port);
		port->in_flight--;
		port_guard_exit(port);
		if (result.commit != SERIO_WRITE_ZERO_COMMIT &&
		    result.commit != SERIO_WRITE_COMMITTED &&
		    result.commit != SERIO_WRITE_UNCERTAIN)
			return write_result(SERIO_INVALID_STATE,
					    SERIO_WRITE_UNCERTAIN);
		if (result.status == SERIO_OK &&
		    result.commit != SERIO_WRITE_COMMITTED)
			result.status = SERIO_INVALID_STATE;
		return result;
	}
}

enum serio_status serio_driver_register(struct serio_registry *registry,
	struct serio_driver *driver, const struct serio_driver_config *config)
{
	uint16_t slot;
	uint64_t generation;

	if (!registry_is_usable(registry))
		return registry != NULL && registry->poisoned != 0u
			       ? SERIO_POISONED
			       : SERIO_INVALID_STATE;
	if (driver == NULL || driver->lifecycle_cookie != SERIO_DRIVER_COOKIE ||
	    !driver_config_is_valid(config))
		return SERIO_INVALID_ARGUMENT;
	if (driver->registered != 0u)
		return SERIO_ALREADY_REGISTERED;
	if (find_driver(registry, config->identity) != NULL ||
	    find_port(registry, config->identity) != NULL)
		return SERIO_ALREADY_REGISTERED;
	for (slot = 0u; slot < registry->driver_capacity; ++slot) {
		if (registry->drivers[slot] == NULL)
			break;
	}
	if (slot == registry->driver_capacity ||
	    driver->generation >= SERIO_GENERATION_MAX)
		return SERIO_CAPACITY_EXHAUSTED;
	generation = driver->generation + 1u;
	clear_driver_preserving_generation(driver);
	driver->generation = generation;
	driver->config = *config;
	driver->registry = registry;
	driver->registry_slot = slot;
	driver->registered = 1u;
	registry->drivers[slot] = driver;
	registry->driver_count++;
	if (config->manual_bind == 0u) {
		for (slot = 0u; slot < registry->port_capacity; ++slot) {
			struct serio_port *port = registry->ports[slot];

			if (port != NULL && port->phase == SERIO_PORT_ACTIVE &&
			    port->driver == NULL &&
			    port->config.manual_bind == 0u &&
			    driver_matches_port(driver, port))
				(void)bind_driver(port, driver);
		}
	}
	return SERIO_OK;
}

enum serio_status serio_driver_unregister(struct serio_driver *driver)
{
	struct serio_registry *registry;
	uint16_t slot;

	if (driver == NULL || driver->lifecycle_cookie != SERIO_DRIVER_COOKIE)
		return SERIO_INVALID_ARGUMENT;
	if (driver->registered == 0u || driver->registry == NULL)
		return SERIO_INVALID_STATE;
	registry = driver->registry;
	slot = driver->registry_slot;
	if (slot >= registry->driver_capacity ||
	    registry->drivers[slot] != driver)
		return SERIO_IDENTITY_MISMATCH;
	driver->withdrawing = 1u;
	for (slot = 0u; slot < registry->port_capacity; ++slot) {
		struct serio_port *port = registry->ports[slot];

		if (port == NULL || port->driver != driver)
			continue;
		port_guard_enter(port);
		if (port->in_flight != 0u) {
			port_guard_exit(port);
			driver->withdrawing = 0u;
			return SERIO_RETRY;
		}
		port_guard_exit(port);
	}
	for (slot = 0u; slot < registry->port_capacity; ++slot) {
		struct serio_port *port = registry->ports[slot];

		if (port != NULL && port->driver == driver)
			detach_driver(port);
	}
	slot = driver->registry_slot;
	registry->drivers[slot] = NULL;
	registry->driver_count--;
	clear_driver_preserving_generation(driver);
	for (slot = 0u; slot < registry->port_capacity; ++slot) {
		struct serio_port *port = registry->ports[slot];

		if (port != NULL && port->phase == SERIO_PORT_ACTIVE &&
		    port->driver == NULL)
			(void)attach_first_driver(port);
	}
	return SERIO_OK;
}

// SPDX-License-Identifier: GPL-2.0-only
/* Constructed-object registry and replaceable caller-owned storage. */
#include "serio.h"

#include "private.h"

static bool identity_is_valid(kernel_object_handle_t identity)
{
	return identity != 0u && identity != KERNEL_OBJECT_HANDLE_INVALID;
}

static bool registry_is_usable(const struct serio_registry *registry)
{
	return registry != NULL &&
	       registry->lifecycle_cookie == SERIO_REGISTRY_COOKIE &&
	       registry->active != 0u && registry->poisoned == 0u &&
	       identity_is_valid(registry->identity);
}

void serio_registry_construct(struct serio_registry *registry)
{
	uint8_t *bytes = (uint8_t *)registry;
	size_t index;

	if (registry == NULL)
		return;
	for (index = 0u; index < sizeof(*registry); ++index)
		bytes[index] = 0u;
	registry->lifecycle_cookie = SERIO_REGISTRY_COOKIE;
}

void serio_port_construct(struct serio_port *port)
{
	uint8_t *bytes = (uint8_t *)port;
	size_t index;

	if (port == NULL)
		return;
	for (index = 0u; index < sizeof(*port); ++index)
		bytes[index] = 0u;
	port->lifecycle_cookie = SERIO_PORT_COOKIE;
	port->registry_slot = SERIO_SLOT_INVALID;
}

void serio_driver_construct(struct serio_driver *driver)
{
	uint8_t *bytes = (uint8_t *)driver;
	size_t index;

	if (driver == NULL)
		return;
	for (index = 0u; index < sizeof(*driver); ++index)
		bytes[index] = 0u;
	driver->lifecycle_cookie = SERIO_DRIVER_COOKIE;
	driver->registry_slot = SERIO_SLOT_INVALID;
}

enum serio_status serio_registry_initialize(
	struct serio_registry *registry, kernel_object_handle_t identity,
	struct serio_port **ports, uint16_t port_capacity,
	struct serio_driver **drivers, uint16_t driver_capacity)
{
	uint16_t slot;

	if (registry == NULL || registry->lifecycle_cookie != SERIO_REGISTRY_COOKIE ||
	    !identity_is_valid(identity) || ports == NULL ||
	    drivers == NULL || port_capacity == 0u || driver_capacity == 0u)
		return SERIO_INVALID_ARGUMENT;
	if (registry->active != 0u)
		return registry->poisoned != 0u ? SERIO_POISONED
						       : SERIO_INVALID_STATE;
	if (registry->generation >= SERIO_GENERATION_MAX)
		return SERIO_CAPACITY_EXHAUSTED;
	for (slot = 0u; slot < port_capacity; ++slot)
		ports[slot] = NULL;
	for (slot = 0u; slot < driver_capacity; ++slot)
		drivers[slot] = NULL;
	registry->identity = identity;
	registry->generation++;
	registry->ports = ports;
	registry->drivers = drivers;
	registry->port_capacity = port_capacity;
	registry->driver_capacity = driver_capacity;
	registry->port_count = 0u;
	registry->driver_count = 0u;
	registry->active = 1u;
	registry->poisoned = 0u;
	for (slot = 0u; slot < ARRAY_SIZE(registry->reserved); ++slot)
		registry->reserved[slot] = 0u;
	return SERIO_OK;
}

enum serio_status serio_registry_replace_storage(
	struct serio_registry *registry, kernel_object_handle_t identity,
	struct serio_port **ports, uint16_t port_capacity,
	struct serio_driver **drivers, uint16_t driver_capacity)
{
	uint16_t slot;

	if (!registry_is_usable(registry) || registry->identity != identity)
		return registry != NULL && registry->poisoned != 0u
			       ? SERIO_POISONED
			       : SERIO_IDENTITY_MISMATCH;
	if (ports == NULL || drivers == NULL || ports == registry->ports ||
	    drivers == registry->drivers ||
	    port_capacity < registry->port_capacity ||
	    driver_capacity < registry->driver_capacity)
		return SERIO_INVALID_ARGUMENT;
	for (slot = 0u; slot < registry->port_capacity; ++slot) {
		struct serio_port *port = registry->ports[slot];

		if (port != NULL && (port->phase == SERIO_PORT_ACTIVE ||
				     port->in_flight != 0u))
			return SERIO_RETRY;
	}
	for (slot = 0u; slot < port_capacity; ++slot)
		ports[slot] = slot < registry->port_capacity
			      ? registry->ports[slot]
			      : NULL;
	for (slot = 0u; slot < driver_capacity; ++slot)
		drivers[slot] = slot < registry->driver_capacity
				? registry->drivers[slot]
				: NULL;
	registry->ports = ports;
	registry->drivers = drivers;
	registry->port_capacity = port_capacity;
	registry->driver_capacity = driver_capacity;
	return SERIO_OK;
}

enum serio_status serio_registry_poison(struct serio_registry *registry,
					kernel_object_handle_t identity)
{
	if (registry == NULL ||
	    registry->lifecycle_cookie != SERIO_REGISTRY_COOKIE ||
	    !identity_is_valid(identity))
		return SERIO_INVALID_ARGUMENT;
	if (registry->active == 0u)
		return SERIO_INVALID_STATE;
	if (registry->identity != identity)
		return SERIO_IDENTITY_MISMATCH;
	registry->poisoned = 1u;
	return SERIO_OK;
}

void *serio_driver_context(const struct serio_driver *driver)
{
	if (driver == NULL || driver->lifecycle_cookie != SERIO_DRIVER_COOKIE ||
	    driver->registered == 0u)
		return NULL;
	return driver->config.driver_context;
}

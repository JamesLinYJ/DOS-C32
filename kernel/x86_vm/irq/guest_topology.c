// SPDX-License-Identifier: GPL-2.0-only
/* Producer and explicit native-route topology; active mutation is forbidden. */
#include "guest_private.h"

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

static void clear_producer_slot(struct x86_guest_irq_producer_slot *slot)
{
	uint64_t generation = slot->generation;

	*slot = (struct x86_guest_irq_producer_slot){
		.identity = KERNEL_OBJECT_HANDLE_INVALID,
		.generation = generation,
	};
}

static void clear_route_slot(struct x86_guest_irq_route_slot *slot)
{
	uint64_t generation = slot->generation;

	*slot = (struct x86_guest_irq_route_slot){
		.generation = generation,
	};
}

static bool producer_config_is_valid(
	const struct x86_guest_irq_producer_config *config)
{
	if (config == NULL || !identity_is_valid(config->identity) ||
	    config->capabilities == 0u ||
	    (config->capabilities &
	     (uint32_t)~X86_GUEST_IRQ_PRODUCER_CAPABILITIES) != 0u ||
	    !bytes_are_zero(config->reserved, ARRAY_SIZE(config->reserved)))
		return false;
	if ((config->capabilities & X86_GUEST_IRQ_PRODUCER_PIT_CLOCK) != 0u &&
	    (config->allowed_guest_irqs & 1u) == 0u)
		return false;
	if ((config->capabilities & X86_GUEST_IRQ_PRODUCER_IRQ_EDGE) != 0u &&
	    config->allowed_guest_irqs == 0u)
		return false;
	return true;
}

static enum x86_guest_irq_router_status producer_slot_status(
	struct x86_guest_irq_router *router,
	const struct x86_guest_irq_producer_binding *binding,
	struct x86_guest_irq_producer_slot **result)
{
	struct x86_guest_irq_producer_slot *slot;

	if (router == NULL || binding == NULL || result == NULL ||
	    !identity_is_valid(binding->router_identity) ||
	    !identity_is_valid(binding->producer_identity) ||
	    !bytes_are_zero(binding->reserved, ARRAY_SIZE(binding->reserved)) ||
	    binding->slot >= router->producer_capacity)
		return X86_GUEST_IRQ_ROUTER_INVALID_ARGUMENT;
	if (router->lifecycle_cookie != X86_GUEST_IRQ_ROUTER_COOKIE ||
	    router->initialized != 1u ||
	    router->phase == X86_GUEST_IRQ_ROUTER_EMPTY ||
	    router->phase == X86_GUEST_IRQ_ROUTER_UNINITIALIZED)
		return X86_GUEST_IRQ_ROUTER_INVALID_STATE;
	if (router->phase == X86_GUEST_IRQ_ROUTER_POISONED_PHASE)
		return X86_GUEST_IRQ_ROUTER_POISONED;
	if (binding->router_identity != router->identity)
		return X86_GUEST_IRQ_ROUTER_IDENTITY_MISMATCH;
	if (binding->router_generation != router->generation)
		return X86_GUEST_IRQ_ROUTER_STALE_BINDING;
	slot = &router->producers[binding->slot];
	if (slot->phase == PRODUCER_EMPTY ||
	    slot->generation != binding->producer_generation)
		return X86_GUEST_IRQ_ROUTER_STALE_BINDING;
	if (slot->identity != binding->producer_identity)
		return X86_GUEST_IRQ_ROUTER_IDENTITY_MISMATCH;
	*result = slot;
	return X86_GUEST_IRQ_ROUTER_OK;
}

static bool route_config_is_valid(
	const struct x86_guest_irq_native_route_config *config)
{
	if (config == NULL ||
	    !bytes_are_zero(config->reserved, ARRAY_SIZE(config->reserved)))
		return false;
	if (config->native_kind == (uint8_t)X86_LEGACY_IRQ_EVENT_PIT_CLOCK)
		return config->native_irq == 0u &&
		       config->guest_kind ==
			       (uint8_t)X86_GUEST_IRQ_EVENT_PIT_CLOCK &&
		       config->guest_irq == 0u;
	if (config->native_kind == (uint8_t)X86_LEGACY_IRQ_EVENT_IRQ_EDGE)
		return config->native_irq < X86_LEGACY_IRQ_COUNT &&
		       config->guest_kind ==
			       (uint8_t)X86_GUEST_IRQ_EVENT_IRQ_EDGE &&
		       config->guest_irq < X86_LEGACY_CHIPSET_IRQ_COUNT;
	return false;
}

enum x86_guest_irq_router_status x86_guest_irq_producer_register(
	struct x86_guest_irq_router *router,
	const struct x86_guest_irq_producer_config *config,
	struct x86_guest_irq_producer_binding *binding)
{
	struct x86_guest_irq_producer_slot *slot = NULL;
	uint32_t index;

	if (router == NULL || binding == NULL ||
	    !producer_config_is_valid(config))
		return X86_GUEST_IRQ_ROUTER_INVALID_ARGUMENT;
	if (router->lifecycle_cookie != X86_GUEST_IRQ_ROUTER_COOKIE ||
	    router->initialized != 1u ||
	    (router->phase != X86_GUEST_IRQ_ROUTER_PREPARED &&
	     router->phase != X86_GUEST_IRQ_ROUTER_QUIESCED))
		return X86_GUEST_IRQ_ROUTER_INVALID_STATE;
	if (config->identity == router->identity ||
	    config->identity == router->sink_context_identity)
		return X86_GUEST_IRQ_ROUTER_INVALID_ARGUMENT;
	if ((config->capabilities & ~router->capabilities) != 0u)
		return X86_GUEST_IRQ_ROUTER_ACCESS_DENIED;
	for (index = 0u; index < router->producer_capacity; ++index) {
		if (router->producers[index].phase != PRODUCER_EMPTY &&
		    router->producers[index].identity == config->identity)
			return X86_GUEST_IRQ_ROUTER_ALREADY_REGISTERED;
		if (slot == NULL &&
		    router->producers[index].phase == PRODUCER_EMPTY &&
		    router->producers[index].generation <
			    X86_GUEST_IRQ_GENERATION_MAX)
			slot = &router->producers[index];
	}
	if (slot == NULL)
		return X86_GUEST_IRQ_ROUTER_CAPACITY_EXHAUSTED;
	index = (uint32_t)(slot - router->producers);
	slot->generation++;
	slot->identity = config->identity;
	slot->submitted_count = 0u;
	slot->rejected_count = 0u;
	slot->sink_failure_count = 0u;
	slot->allowed_guest_irqs = config->allowed_guest_irqs;
	slot->capabilities = (uint8_t)config->capabilities;
	slot->phase = PRODUCER_ACTIVE;
	slot->in_flight = 0u;
	router->producer_count++;
	*binding = (struct x86_guest_irq_producer_binding){
		.router_identity = router->identity,
		.router_generation = router->generation,
		.producer_identity = slot->identity,
		.producer_generation = slot->generation,
		.slot = index,
		.reserved = {0u},
	};
	return X86_GUEST_IRQ_ROUTER_OK;
}

enum x86_guest_irq_router_status x86_guest_irq_producer_quiesce(
	struct x86_guest_irq_router *router,
	const struct x86_guest_irq_producer_binding *binding)
{
	struct x86_guest_irq_producer_slot *slot;
	enum x86_guest_irq_router_status status =
		producer_slot_status(router, binding, &slot);

	if (status != X86_GUEST_IRQ_ROUTER_OK)
		return status;
	if (router->phase != X86_GUEST_IRQ_ROUTER_PREPARED &&
	    router->phase != X86_GUEST_IRQ_ROUTER_QUIESCED)
		return X86_GUEST_IRQ_ROUTER_INVALID_STATE;
	if (slot->phase != PRODUCER_ACTIVE)
		return X86_GUEST_IRQ_ROUTER_INVALID_STATE;
	if (slot->in_flight != 0u)
		return X86_GUEST_IRQ_ROUTER_BUSY;
	slot->phase = PRODUCER_QUIESCED;
	return X86_GUEST_IRQ_ROUTER_OK;
}

static bool producer_has_route(const struct x86_guest_irq_router *router,
	uint32_t producer_slot, uint64_t producer_generation)
{
	uint32_t index;

	for (index = 0u; index < router->route_capacity; ++index) {
		if (router->routes[index].active != 0u &&
		    router->routes[index].producer_slot == producer_slot &&
		    router->routes[index].producer_generation ==
			    producer_generation)
			return true;
	}
	return false;
}

enum x86_guest_irq_router_status x86_guest_irq_producer_unregister(
	struct x86_guest_irq_router *router,
	const struct x86_guest_irq_producer_binding *binding)
{
	struct x86_guest_irq_producer_slot *slot;
	enum x86_guest_irq_router_status status =
		producer_slot_status(router, binding, &slot);

	if (status != X86_GUEST_IRQ_ROUTER_OK)
		return status;
	if (router->phase != X86_GUEST_IRQ_ROUTER_PREPARED &&
	    router->phase != X86_GUEST_IRQ_ROUTER_QUIESCED)
		return X86_GUEST_IRQ_ROUTER_INVALID_STATE;
	if (slot->phase != PRODUCER_QUIESCED || slot->in_flight != 0u)
		return X86_GUEST_IRQ_ROUTER_INVALID_STATE;
	if (producer_has_route(router, binding->slot,
			       binding->producer_generation))
		return X86_GUEST_IRQ_ROUTER_BUSY;
	clear_producer_slot(slot);
	router->producer_count--;
	return X86_GUEST_IRQ_ROUTER_OK;
}

static bool route_allowed_by_producer(
	const struct x86_guest_irq_producer_slot *producer,
	const struct x86_guest_irq_native_route_config *config)
{
	uint32_t capability =
		config->guest_kind == (uint8_t)X86_GUEST_IRQ_EVENT_PIT_CLOCK
			? X86_GUEST_IRQ_PRODUCER_PIT_CLOCK
			: X86_GUEST_IRQ_PRODUCER_IRQ_EDGE;

	return (producer->capabilities & capability) != 0u &&
	       (producer->allowed_guest_irqs &
		(uint16_t)(1u << config->guest_irq)) != 0u;
}

enum x86_guest_irq_router_status x86_guest_irq_native_route_install(
	struct x86_guest_irq_router *router,
	const struct x86_guest_irq_producer_binding *producer,
	const struct x86_guest_irq_native_route_config *config,
	struct x86_guest_irq_route_binding *route)
{
	struct x86_guest_irq_producer_slot *producer_slot;
	struct x86_guest_irq_route_slot *route_slot = NULL;
	enum x86_guest_irq_router_status status;
	uint32_t index;

	if (route == NULL || !route_config_is_valid(config))
		return X86_GUEST_IRQ_ROUTER_INVALID_ARGUMENT;
	status = producer_slot_status(router, producer, &producer_slot);
	if (status != X86_GUEST_IRQ_ROUTER_OK)
		return status;
	if (producer_slot->phase != PRODUCER_ACTIVE ||
	    (router->phase != X86_GUEST_IRQ_ROUTER_PREPARED &&
	     router->phase != X86_GUEST_IRQ_ROUTER_QUIESCED))
		return X86_GUEST_IRQ_ROUTER_INVALID_STATE;
	if (!route_allowed_by_producer(producer_slot, config))
		return X86_GUEST_IRQ_ROUTER_ACCESS_DENIED;
	for (index = 0u; index < router->route_capacity; ++index) {
		if (router->routes[index].active != 0u &&
		    router->routes[index].producer_slot == producer->slot &&
		    router->routes[index].producer_generation ==
			    producer->producer_generation &&
		    router->routes[index].native_kind == config->native_kind &&
		    router->routes[index].native_irq == config->native_irq)
			return X86_GUEST_IRQ_ROUTER_ALREADY_REGISTERED;
		if (route_slot == NULL && router->routes[index].active == 0u &&
		    router->routes[index].generation <
			    X86_GUEST_IRQ_GENERATION_MAX)
			route_slot = &router->routes[index];
	}
	if (route_slot == NULL)
		return X86_GUEST_IRQ_ROUTER_CAPACITY_EXHAUSTED;
	index = (uint32_t)(route_slot - router->routes);
	route_slot->generation++;
	route_slot->producer_generation = producer->producer_generation;
	route_slot->producer_slot = producer->slot;
	route_slot->native_kind = config->native_kind;
	route_slot->native_irq = config->native_irq;
	route_slot->guest_kind = config->guest_kind;
	route_slot->guest_irq = config->guest_irq;
	route_slot->active = 1u;
	router->route_count++;
	*route = (struct x86_guest_irq_route_binding){
		.router_identity = router->identity,
		.router_generation = router->generation,
		.route_generation = route_slot->generation,
		.producer_generation = producer->producer_generation,
		.route_slot = index,
		.producer_slot = producer->slot,
	};
	return X86_GUEST_IRQ_ROUTER_OK;
}

static enum x86_guest_irq_router_status route_binding_status(
	struct x86_guest_irq_router *router,
	const struct x86_guest_irq_producer_binding *producer,
	const struct x86_guest_irq_route_binding *route,
	struct x86_guest_irq_route_slot **result)
{
	struct x86_guest_irq_route_slot *slot;

	if (route == NULL || result == NULL ||
	    !identity_is_valid(route->router_identity) ||
	    route->route_slot >= router->route_capacity)
		return X86_GUEST_IRQ_ROUTER_INVALID_ARGUMENT;
	if (route->router_identity != router->identity ||
	    route->router_identity != producer->router_identity)
		return X86_GUEST_IRQ_ROUTER_IDENTITY_MISMATCH;
	if (route->router_generation != router->generation ||
	    route->router_generation != producer->router_generation ||
	    route->producer_generation != producer->producer_generation ||
	    route->producer_slot != producer->slot)
		return X86_GUEST_IRQ_ROUTER_STALE_BINDING;
	slot = &router->routes[route->route_slot];
	if (slot->active == 0u ||
	    slot->generation != route->route_generation ||
	    slot->producer_generation != route->producer_generation ||
	    slot->producer_slot != route->producer_slot)
		return X86_GUEST_IRQ_ROUTER_STALE_BINDING;
	*result = slot;
	return X86_GUEST_IRQ_ROUTER_OK;
}

enum x86_guest_irq_router_status x86_guest_irq_native_route_uninstall(
	struct x86_guest_irq_router *router,
	const struct x86_guest_irq_producer_binding *producer,
	const struct x86_guest_irq_route_binding *route)
{
	struct x86_guest_irq_producer_slot *producer_slot;
	struct x86_guest_irq_route_slot *route_slot;
	enum x86_guest_irq_router_status status;

	status = producer_slot_status(router, producer, &producer_slot);
	if (status != X86_GUEST_IRQ_ROUTER_OK)
		return status;
	if (router->phase != X86_GUEST_IRQ_ROUTER_PREPARED &&
	    router->phase != X86_GUEST_IRQ_ROUTER_QUIESCED)
		return X86_GUEST_IRQ_ROUTER_INVALID_STATE;
	if (producer_slot->in_flight != 0u || router->dispatch_active != 0u)
		return X86_GUEST_IRQ_ROUTER_BUSY;
	status = route_binding_status(router, producer, route, &route_slot);
	if (status != X86_GUEST_IRQ_ROUTER_OK)
		return status;
	clear_route_slot(route_slot);
	router->route_count--;
	return X86_GUEST_IRQ_ROUTER_OK;
}

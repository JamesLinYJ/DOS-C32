// SPDX-License-Identifier: GPL-2.0-only
/*
 * Guest interrupt producer routing domain.
 *
 * DOS-C32 owns the producer identities, event types and guest-chipset ABI.
 */
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

static void clear_lifetime(struct x86_guest_irq_router *router)
{
	struct x86_guest_irq_producer_slot *producers = router->producers;
	struct x86_guest_irq_route_slot *routes = router->routes;
	uint64_t generation = router->generation;
	uint32_t producer_capacity = router->producer_capacity;
	uint32_t route_capacity = router->route_capacity;

	*router = (struct x86_guest_irq_router){
		.identity = KERNEL_OBJECT_HANDLE_INVALID,
		.sink_context_identity = KERNEL_OBJECT_HANDLE_INVALID,
		.generation = generation,
		.producers = producers,
		.routes = routes,
		.producer_capacity = producer_capacity,
		.route_capacity = route_capacity,
		.lifecycle_cookie = X86_GUEST_IRQ_ROUTER_COOKIE,
		.phase = X86_GUEST_IRQ_ROUTER_EMPTY,
		.initialized = 1u,
	};
}

static bool storage_is_valid(
	struct x86_guest_irq_producer_slot *producers,
	uint32_t producer_capacity, struct x86_guest_irq_route_slot *routes,
	uint32_t route_capacity)
{
	uintptr_t producer_bytes;
	uintptr_t producer_end;
	uintptr_t producer_start = (uintptr_t)producers;
	uintptr_t route_bytes;
	uintptr_t route_end;
	uintptr_t route_start = (uintptr_t)routes;

	if (producers == NULL || producer_capacity == 0u || routes == NULL ||
	    route_capacity == 0u ||
	    producer_start % __alignof__(struct x86_guest_irq_producer_slot) !=
		    0u ||
	    route_start % __alignof__(struct x86_guest_irq_route_slot) != 0u ||
	    check_mul_overflow((uintptr_t)producer_capacity,
			       (uintptr_t)sizeof(*producers), &producer_bytes) ||
	    check_add_overflow(producer_start, producer_bytes, &producer_end) ||
	    check_mul_overflow((uintptr_t)route_capacity,
			       (uintptr_t)sizeof(*routes), &route_bytes) ||
	    check_add_overflow(route_start, route_bytes, &route_end))
		return false;
	return producer_start >= route_end || route_start >= producer_end;
}

static bool replacement_is_disjoint(
	const struct x86_guest_irq_router *router,
	struct x86_guest_irq_producer_slot *producers,
	uint32_t producer_capacity, struct x86_guest_irq_route_slot *routes,
	uint32_t route_capacity)
{
	uintptr_t old_producer_end;
	uintptr_t old_producer_start = (uintptr_t)router->producers;
	uintptr_t old_route_end;
	uintptr_t old_route_start = (uintptr_t)router->routes;
	uintptr_t producer_end;
	uintptr_t producer_start = (uintptr_t)producers;
	uintptr_t route_end;
	uintptr_t route_start = (uintptr_t)routes;

	if (check_add_overflow(
		    old_producer_start,
		    (uintptr_t)router->producer_capacity *
			    (uintptr_t)sizeof(*router->producers),
		    &old_producer_end) ||
	    check_add_overflow(old_route_start,
			       (uintptr_t)router->route_capacity *
				       (uintptr_t)sizeof(*router->routes),
			       &old_route_end) ||
	    check_add_overflow(producer_start,
			       (uintptr_t)producer_capacity *
				       (uintptr_t)sizeof(*producers),
			       &producer_end) ||
	    check_add_overflow(route_start,
			       (uintptr_t)route_capacity *
				       (uintptr_t)sizeof(*routes),
			       &route_end))
		return false;
	return (producer_start >= old_producer_end ||
		old_producer_start >= producer_end) &&
	       (producer_start >= old_route_end ||
		old_route_start >= producer_end) &&
	       (route_start >= old_producer_end ||
		old_producer_start >= route_end) &&
	       (route_start >= old_route_end || old_route_start >= route_end);
}

static void clear_storage(struct x86_guest_irq_producer_slot *producers,
	uint32_t producer_capacity, struct x86_guest_irq_route_slot *routes,
	uint32_t route_capacity)
{
	uint32_t index;

	for (index = 0u; index < producer_capacity; ++index) {
		producers[index] = (struct x86_guest_irq_producer_slot){
			.identity = KERNEL_OBJECT_HANDLE_INVALID,
		};
	}
	for (index = 0u; index < route_capacity; ++index)
		routes[index] = (struct x86_guest_irq_route_slot){0};
}

static bool router_config_is_valid(
	const struct x86_guest_irq_router_config *config)
{
	if (config == NULL || !identity_is_valid(config->identity) ||
	    !identity_is_valid(config->sink_context_identity) ||
	    config->identity == config->sink_context_identity ||
	    config->sink.bind == NULL || config->sink.submit == NULL ||
	    config->sink.quiesce == NULL || config->sink.resume == NULL ||
	    config->sink.unbind == NULL ||
	    config->capabilities == 0u ||
	    (config->capabilities &
	     (uint32_t)~X86_GUEST_IRQ_PRODUCER_CAPABILITIES) != 0u ||
	    config->pit_rate_calibrated > 1u ||
	    !bytes_are_zero(config->reserved, ARRAY_SIZE(config->reserved)))
		return false;
	if ((config->capabilities & X86_GUEST_IRQ_PRODUCER_PIT_CLOCK) != 0u)
		return config->pit_input_quantum != 0u;
	return config->pit_input_quantum == 0u &&
	       config->pit_rate_calibrated == 0u;
}

static enum x86_guest_irq_router_status router_identity_status(
	const struct x86_guest_irq_router *router,
	kernel_object_handle_t identity)
{
	if (router == NULL || !identity_is_valid(identity))
		return X86_GUEST_IRQ_ROUTER_INVALID_ARGUMENT;
	if (router->lifecycle_cookie != X86_GUEST_IRQ_ROUTER_COOKIE ||
	    router->initialized != 1u ||
	    router->phase == X86_GUEST_IRQ_ROUTER_UNINITIALIZED ||
	    router->phase == X86_GUEST_IRQ_ROUTER_EMPTY)
		return X86_GUEST_IRQ_ROUTER_INVALID_STATE;
	if (router->identity != identity)
		return X86_GUEST_IRQ_ROUTER_IDENTITY_MISMATCH;
	if (router->phase == X86_GUEST_IRQ_ROUTER_POISONED_PHASE)
		return X86_GUEST_IRQ_ROUTER_POISONED;
	return X86_GUEST_IRQ_ROUTER_OK;
}

static bool storage_encoding_is_valid(
	const struct x86_guest_irq_router *router)
{
	uint32_t active_producers = 0u;
	uint32_t active_routes = 0u;
	uint32_t index;

	for (index = 0u; index < router->producer_capacity; ++index) {
		const struct x86_guest_irq_producer_slot *producer =
			&router->producers[index];

		if (producer->phase == PRODUCER_EMPTY)
			continue;
		if (producer->phase != PRODUCER_ACTIVE &&
		    producer->phase != PRODUCER_QUIESCED)
			return false;
		if (!identity_is_valid(producer->identity) ||
		    producer->generation == 0u ||
		    producer->generation > X86_GUEST_IRQ_GENERATION_MAX ||
		    producer->in_flight != 0u || producer->capabilities == 0u ||
		    (producer->capabilities &
		     (uint8_t)~X86_GUEST_IRQ_PRODUCER_CAPABILITIES) != 0u ||
		    ((producer->capabilities &
		      X86_GUEST_IRQ_PRODUCER_PIT_CLOCK) != 0u &&
		     (producer->allowed_guest_irqs & 1u) == 0u) ||
		    ((producer->capabilities &
		      X86_GUEST_IRQ_PRODUCER_IRQ_EDGE) != 0u &&
		     producer->allowed_guest_irqs == 0u))
			return false;
		active_producers++;
	}
	for (index = 0u; index < router->route_capacity; ++index) {
		const struct x86_guest_irq_route_slot *route =
			&router->routes[index];
		const struct x86_guest_irq_producer_slot *producer;

		if (route->active == 0u)
			continue;
		if (route->producer_slot >= router->producer_capacity ||
		    route->generation == 0u ||
		    route->generation > X86_GUEST_IRQ_GENERATION_MAX)
			return false;
		producer = &router->producers[route->producer_slot];
		if (producer->phase == PRODUCER_EMPTY ||
		    producer->generation != route->producer_generation ||
		    (route->native_kind == X86_LEGACY_IRQ_EVENT_PIT_CLOCK &&
		     (route->native_irq != 0u ||
		      route->guest_kind != X86_GUEST_IRQ_EVENT_PIT_CLOCK ||
		      route->guest_irq != 0u)) ||
		    (route->native_kind == X86_LEGACY_IRQ_EVENT_IRQ_EDGE &&
		     (route->native_irq >= X86_LEGACY_IRQ_COUNT ||
		      route->guest_kind != X86_GUEST_IRQ_EVENT_IRQ_EDGE ||
		      route->guest_irq >= X86_LEGACY_CHIPSET_IRQ_COUNT)) ||
		    (route->native_kind != X86_LEGACY_IRQ_EVENT_PIT_CLOCK &&
		     route->native_kind != X86_LEGACY_IRQ_EVENT_IRQ_EDGE))
			return false;
		active_routes++;
	}
	return active_producers == router->producer_count &&
	       active_routes == router->route_count;
}

enum x86_guest_irq_router_status x86_guest_irq_router_initialize(
	struct x86_guest_irq_router *router,
	struct x86_guest_irq_producer_slot *producers,
	uint32_t producer_capacity,
	struct x86_guest_irq_route_slot *routes, uint32_t route_capacity)
{
	if (router == NULL || !storage_is_valid(producers, producer_capacity,
					       routes, route_capacity))
		return X86_GUEST_IRQ_ROUTER_INVALID_ARGUMENT;
	if (router->lifecycle_cookie != X86_GUEST_IRQ_ROUTER_COOKIE ||
	    router->initialized != 0u ||
	    router->phase != X86_GUEST_IRQ_ROUTER_UNINITIALIZED)
		return X86_GUEST_IRQ_ROUTER_INVALID_STATE;
	clear_storage(producers, producer_capacity, routes, route_capacity);
	*router = (struct x86_guest_irq_router){
		.identity = KERNEL_OBJECT_HANDLE_INVALID,
		.sink_context_identity = KERNEL_OBJECT_HANDLE_INVALID,
		.producers = producers,
		.routes = routes,
		.producer_capacity = producer_capacity,
		.route_capacity = route_capacity,
		.lifecycle_cookie = X86_GUEST_IRQ_ROUTER_COOKIE,
		.phase = X86_GUEST_IRQ_ROUTER_EMPTY,
		.initialized = 1u,
	};
	return X86_GUEST_IRQ_ROUTER_OK;
}

void x86_guest_irq_router_construct(struct x86_guest_irq_router *router)
{
	if (router == NULL)
		return;
	*router = (struct x86_guest_irq_router){
		.identity = KERNEL_OBJECT_HANDLE_INVALID,
		.sink_context_identity = KERNEL_OBJECT_HANDLE_INVALID,
		.lifecycle_cookie = X86_GUEST_IRQ_ROUTER_COOKIE,
		.phase = X86_GUEST_IRQ_ROUTER_UNINITIALIZED,
	};
}

enum x86_guest_irq_router_status x86_guest_irq_router_replace_storage(
	struct x86_guest_irq_router *router,
	struct x86_guest_irq_producer_slot *producers,
	uint32_t producer_capacity,
	struct x86_guest_irq_route_slot *routes, uint32_t route_capacity)
{
	if (router == NULL || !storage_is_valid(producers, producer_capacity,
					       routes, route_capacity))
		return X86_GUEST_IRQ_ROUTER_INVALID_ARGUMENT;
	if (router->lifecycle_cookie != X86_GUEST_IRQ_ROUTER_COOKIE ||
	    router->initialized != 1u ||
	    (router->phase != X86_GUEST_IRQ_ROUTER_EMPTY &&
	     router->phase != X86_GUEST_IRQ_ROUTER_PREPARED &&
	     router->phase != X86_GUEST_IRQ_ROUTER_QUIESCED) ||
	    router->dispatch_active != 0u)
		return X86_GUEST_IRQ_ROUTER_INVALID_STATE;
	if (router->phase != X86_GUEST_IRQ_ROUTER_EMPTY &&
	    (producer_capacity < router->producer_capacity ||
	     route_capacity < router->route_capacity))
		return X86_GUEST_IRQ_ROUTER_CAPACITY_EXHAUSTED;
	if (!replacement_is_disjoint(router, producers, producer_capacity,
				     routes, route_capacity))
		return X86_GUEST_IRQ_ROUTER_INVALID_ARGUMENT;
	if (!storage_encoding_is_valid(router))
		return X86_GUEST_IRQ_ROUTER_POISONED;
	clear_storage(producers, producer_capacity, routes, route_capacity);
	if (router->phase != X86_GUEST_IRQ_ROUTER_EMPTY) {
		uint32_t index;

		for (index = 0u; index < router->producer_capacity; ++index)
			producers[index] = router->producers[index];
		for (index = 0u; index < router->route_capacity; ++index)
			routes[index] = router->routes[index];
	}
	router->producers = producers;
	router->routes = routes;
	router->producer_capacity = producer_capacity;
	router->route_capacity = route_capacity;
	return X86_GUEST_IRQ_ROUTER_OK;
}

enum x86_guest_irq_router_status x86_guest_irq_router_prepare(
	struct x86_guest_irq_router *router,
	const struct x86_guest_irq_router_config *config)
{
	if (router == NULL || !router_config_is_valid(config))
		return X86_GUEST_IRQ_ROUTER_INVALID_ARGUMENT;
	if (router->lifecycle_cookie != X86_GUEST_IRQ_ROUTER_COOKIE ||
	    router->initialized != 1u ||
	    router->phase != X86_GUEST_IRQ_ROUTER_EMPTY ||
	    router->producer_count != 0u || router->route_count != 0u)
		return X86_GUEST_IRQ_ROUTER_INVALID_STATE;
	if (router->generation >= X86_GUEST_IRQ_GENERATION_MAX)
		return X86_GUEST_IRQ_ROUTER_CAPACITY_EXHAUSTED;
	router->generation++;
	router->identity = config->identity;
	router->sink_context_identity = config->sink_context_identity;
	router->sink = config->sink;
	router->pit_input_quantum = config->pit_input_quantum;
	router->capabilities = config->capabilities;
	router->pit_rate_calibrated = config->pit_rate_calibrated;
	router->submitted_count = 0u;
	router->unmapped_count = 0u;
	router->rejected_count = 0u;
	router->sink_failure_count = 0u;
	router->phase = X86_GUEST_IRQ_ROUTER_PREPARED;
	return X86_GUEST_IRQ_ROUTER_OK;
}

enum x86_guest_irq_router_status x86_guest_irq_router_publish(
	struct x86_guest_irq_router *router)
{
	struct x86_legacy_chipset_source_config sink_config;
	enum x86_guest_irq_sink_result result;

	if (router == NULL)
		return X86_GUEST_IRQ_ROUTER_INVALID_ARGUMENT;
	if (router->lifecycle_cookie != X86_GUEST_IRQ_ROUTER_COOKIE ||
	    router->initialized != 1u ||
	    router->phase != X86_GUEST_IRQ_ROUTER_PREPARED)
		return X86_GUEST_IRQ_ROUTER_INVALID_STATE;
	sink_config = (struct x86_legacy_chipset_source_config){
		.pit_input_quantum = router->pit_input_quantum,
		.capabilities = 0u,
		.pit_rate_calibrated = router->pit_rate_calibrated,
		.reserved = {0u},
	};
	if ((router->capabilities & X86_GUEST_IRQ_PRODUCER_PIT_CLOCK) != 0u)
		sink_config.capabilities |=
			X86_LEGACY_CHIPSET_SOURCE_PIT_CLOCK;
	if ((router->capabilities & X86_GUEST_IRQ_PRODUCER_IRQ_EDGE) != 0u)
		sink_config.capabilities |=
			X86_LEGACY_CHIPSET_SOURCE_IRQ_EDGE;
	result = router->sink.bind(router->sink_context_identity,
				   router->identity, &sink_config);
	if (result == X86_GUEST_IRQ_SINK_OK) {
		router->phase = X86_GUEST_IRQ_ROUTER_ACTIVE;
		return X86_GUEST_IRQ_ROUTER_OK;
	}
	if (result == X86_GUEST_IRQ_SINK_REJECTED)
		return X86_GUEST_IRQ_ROUTER_SINK_REJECTED;
	router->phase = X86_GUEST_IRQ_ROUTER_POISONED_PHASE;
	return X86_GUEST_IRQ_ROUTER_POISONED;
}

enum x86_guest_irq_router_status x86_guest_irq_router_abort(
	struct x86_guest_irq_router *router)
{
	if (router == NULL)
		return X86_GUEST_IRQ_ROUTER_INVALID_ARGUMENT;
	if (router->lifecycle_cookie != X86_GUEST_IRQ_ROUTER_COOKIE ||
	    router->initialized != 1u ||
	    router->phase != X86_GUEST_IRQ_ROUTER_PREPARED ||
	    router->producer_count != 0u || router->route_count != 0u)
		return X86_GUEST_IRQ_ROUTER_INVALID_STATE;
	clear_lifetime(router);
	return X86_GUEST_IRQ_ROUTER_OK;
}

enum x86_guest_irq_router_status x86_guest_irq_router_quiesce(
	struct x86_guest_irq_router *router, kernel_object_handle_t identity)
{
	enum x86_guest_irq_sink_result sink_result;
	enum x86_guest_irq_router_status status =
		router_identity_status(router, identity);

	if (status != X86_GUEST_IRQ_ROUTER_OK)
		return status;
	if (router->phase != X86_GUEST_IRQ_ROUTER_ACTIVE)
		return X86_GUEST_IRQ_ROUTER_INVALID_STATE;
	if (router->dispatch_active != 0u)
		return X86_GUEST_IRQ_ROUTER_BUSY;
	/* Close local publication before asking the sink to drain this source. */
	router->phase = X86_GUEST_IRQ_ROUTER_QUIESCED;
	sink_result = router->sink.quiesce(router->sink_context_identity,
					  router->identity);
	if (sink_result == X86_GUEST_IRQ_SINK_OK)
		return X86_GUEST_IRQ_ROUTER_OK;
	router->phase = X86_GUEST_IRQ_ROUTER_POISONED_PHASE;
	return X86_GUEST_IRQ_ROUTER_POISONED;
}

enum x86_guest_irq_router_status x86_guest_irq_router_resume(
	struct x86_guest_irq_router *router, kernel_object_handle_t identity)
{
	enum x86_guest_irq_sink_result sink_result;
	enum x86_guest_irq_router_status status =
		router_identity_status(router, identity);

	if (status != X86_GUEST_IRQ_ROUTER_OK)
		return status;
	if (router->phase != X86_GUEST_IRQ_ROUTER_QUIESCED ||
	    router->dispatch_active != 0u)
		return X86_GUEST_IRQ_ROUTER_INVALID_STATE;
	sink_result = router->sink.resume(router->sink_context_identity,
					 router->identity);
	if (sink_result == X86_GUEST_IRQ_SINK_OK) {
		router->phase = X86_GUEST_IRQ_ROUTER_ACTIVE;
		return X86_GUEST_IRQ_ROUTER_OK;
	}
	if (sink_result == X86_GUEST_IRQ_SINK_REJECTED)
		return X86_GUEST_IRQ_ROUTER_SINK_REJECTED;
	router->phase = X86_GUEST_IRQ_ROUTER_POISONED_PHASE;
	return X86_GUEST_IRQ_ROUTER_POISONED;
}

enum x86_guest_irq_router_status x86_guest_irq_router_retire(
	struct x86_guest_irq_router *router, kernel_object_handle_t identity)
{
	enum x86_guest_irq_sink_result sink_result;
	enum x86_guest_irq_router_status status =
		router_identity_status(router, identity);

	if (status != X86_GUEST_IRQ_ROUTER_OK)
		return status;
	if (router->phase != X86_GUEST_IRQ_ROUTER_QUIESCED ||
	    router->producer_count != 0u || router->route_count != 0u ||
	    router->dispatch_active != 0u)
		return X86_GUEST_IRQ_ROUTER_INVALID_STATE;
	sink_result = router->sink.unbind(router->sink_context_identity,
					 router->identity);
	if (sink_result != X86_GUEST_IRQ_SINK_OK) {
		router->phase = X86_GUEST_IRQ_ROUTER_POISONED_PHASE;
		return X86_GUEST_IRQ_ROUTER_POISONED;
	}
	clear_lifetime(router);
	return X86_GUEST_IRQ_ROUTER_OK;
}

enum x86_guest_irq_router_status x86_guest_irq_router_poison(
	struct x86_guest_irq_router *router, kernel_object_handle_t identity)
{
	enum x86_guest_irq_router_status status =
		router_identity_status(router, identity);

	if (status != X86_GUEST_IRQ_ROUTER_OK)
		return status;
	router->phase = X86_GUEST_IRQ_ROUTER_POISONED_PHASE;
	return X86_GUEST_IRQ_ROUTER_OK;
}

enum x86_guest_irq_router_status x86_guest_irq_router_snapshot(
	const struct x86_guest_irq_router *router,
	struct x86_guest_irq_router_snapshot *snapshot)
{
	if (router == NULL || snapshot == NULL)
		return X86_GUEST_IRQ_ROUTER_INVALID_ARGUMENT;
	if (router->lifecycle_cookie != X86_GUEST_IRQ_ROUTER_COOKIE ||
	    router->initialized != 1u)
		return X86_GUEST_IRQ_ROUTER_INVALID_STATE;
	if (router->phase == X86_GUEST_IRQ_ROUTER_ACTIVE ||
	    router->dispatch_active != 0u)
		return X86_GUEST_IRQ_ROUTER_BUSY;
	*snapshot = (struct x86_guest_irq_router_snapshot){
		.identity = router->identity,
		.sink_context_identity = router->sink_context_identity,
		.generation = router->generation,
		.submitted_count = router->submitted_count,
		.unmapped_count = router->unmapped_count,
		.rejected_count = router->rejected_count,
		.sink_failure_count = router->sink_failure_count,
		.pit_input_quantum = router->pit_input_quantum,
		.producer_capacity = router->producer_capacity,
		.route_capacity = router->route_capacity,
		.producer_count = router->producer_count,
		.route_count = router->route_count,
		.capabilities = router->capabilities,
		.pit_rate_calibrated = router->pit_rate_calibrated,
		.phase = router->phase,
		.dispatch_active = router->dispatch_active,
		.reserved = 0u,
	};
	return X86_GUEST_IRQ_ROUTER_OK;
}

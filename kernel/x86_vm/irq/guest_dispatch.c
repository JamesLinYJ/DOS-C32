// SPDX-License-Identifier: GPL-2.0-only
/* Allocation-free synchronous guest interrupt submission and translation. */
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

static uint64_t saturating_increment(uint64_t value)
{
	return value == (uint64_t)-1 ? value : value + 1u;
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

static bool guest_event_is_valid(const struct x86_guest_irq_event *event)
{
	if (event == NULL ||
	    !bytes_are_zero(event->reserved, ARRAY_SIZE(event->reserved)))
		return false;
	if (event->kind == (uint8_t)X86_GUEST_IRQ_EVENT_PIT_CLOCK)
		return event->pit_input_ticks != 0u && event->irq == 0u;
	if (event->kind == (uint8_t)X86_GUEST_IRQ_EVENT_IRQ_EDGE)
		return event->pit_input_ticks == 0u &&
		       event->irq < X86_LEGACY_CHIPSET_IRQ_COUNT;
	return false;
}

static bool native_event_is_valid(const struct x86_legacy_irq_event *event)
{
	if (event == NULL || !identity_is_valid(event->source_identity) ||
	    event->pit_rate_calibrated > 1u ||
	    !bytes_are_zero(event->reserved, ARRAY_SIZE(event->reserved)))
		return false;
	if (event->kind == (uint8_t)X86_LEGACY_IRQ_EVENT_PIT_CLOCK)
		return event->pit_input_ticks != 0u && event->irq == 0u;
	if (event->kind == (uint8_t)X86_LEGACY_IRQ_EVENT_IRQ_EDGE)
		return event->pit_input_ticks == 0u &&
		       event->irq < X86_LEGACY_IRQ_COUNT;
	return false;
}

static bool producer_allows_event(
	const struct x86_guest_irq_producer_slot *producer,
	const struct x86_guest_irq_event *event)
{
	uint32_t capability;

	capability = event->kind == (uint8_t)X86_GUEST_IRQ_EVENT_PIT_CLOCK
			     ? X86_GUEST_IRQ_PRODUCER_PIT_CLOCK
			     : X86_GUEST_IRQ_PRODUCER_IRQ_EDGE;
	return (producer->capabilities & capability) != 0u &&
	       (producer->allowed_guest_irqs &
		(uint16_t)(1u << event->irq)) != 0u;
}

static enum x86_guest_irq_router_status dispatch_guest_event(
	struct x86_guest_irq_router *router,
	struct x86_guest_irq_producer_slot *producer,
	const struct x86_guest_irq_event *event)
{
	struct x86_legacy_chipset_source_event sink_event = {0};
	enum x86_guest_irq_sink_result result;

	if (router->phase != X86_GUEST_IRQ_ROUTER_ACTIVE ||
	    producer->phase != PRODUCER_ACTIVE)
		return X86_GUEST_IRQ_ROUTER_INVALID_STATE;
	if (!producer_allows_event(producer, event)) {
		producer->rejected_count =
			saturating_increment(producer->rejected_count);
		router->rejected_count =
			saturating_increment(router->rejected_count);
		return X86_GUEST_IRQ_ROUTER_ACCESS_DENIED;
	}
	sink_event.pit_input_ticks = event->pit_input_ticks;
	sink_event.irq = event->irq;
	if (event->kind == (uint8_t)X86_GUEST_IRQ_EVENT_PIT_CLOCK)
		sink_event.kind =
			(uint8_t)X86_LEGACY_CHIPSET_EVENT_PIT_CLOCK;
	else
		sink_event.kind = (uint8_t)X86_LEGACY_CHIPSET_EVENT_IRQ_EDGE;
	if (router->dispatch_active != 0u || producer->in_flight != 0u)
		return X86_GUEST_IRQ_ROUTER_BUSY;
	router->dispatch_active = 1u;
	producer->in_flight = 1u;
	result = router->sink.submit(router->sink_context_identity,
				     router->identity, &sink_event);
	producer->in_flight = 0u;
	router->dispatch_active = 0u;
	if (result == X86_GUEST_IRQ_SINK_OK) {
		producer->submitted_count =
			saturating_increment(producer->submitted_count);
		router->submitted_count =
			saturating_increment(router->submitted_count);
		return X86_GUEST_IRQ_ROUTER_OK;
	}
	producer->sink_failure_count =
		saturating_increment(producer->sink_failure_count);
	router->sink_failure_count =
		saturating_increment(router->sink_failure_count);
	if (result == X86_GUEST_IRQ_SINK_REJECTED)
		return X86_GUEST_IRQ_ROUTER_SINK_REJECTED;
	router->phase = X86_GUEST_IRQ_ROUTER_POISONED_PHASE;
	return X86_GUEST_IRQ_ROUTER_POISONED;
}

enum x86_guest_irq_router_status x86_guest_irq_submit(
	struct x86_guest_irq_router *router,
	const struct x86_guest_irq_producer_binding *producer,
	const struct x86_guest_irq_event *event)
{
	struct x86_guest_irq_producer_slot *slot;
	enum x86_guest_irq_router_status status;

	if (!guest_event_is_valid(event))
		return X86_GUEST_IRQ_ROUTER_INVALID_ARGUMENT;
	status = producer_slot_status(router, producer, &slot);
	if (status != X86_GUEST_IRQ_ROUTER_OK)
		return status;
	return dispatch_guest_event(router, slot, event);
}

enum x86_guest_irq_router_status x86_guest_irq_route_native(
	struct x86_guest_irq_router *router,
	const struct x86_guest_irq_producer_binding *producer,
	const struct x86_legacy_irq_event *event)
{
	struct x86_guest_irq_producer_slot *producer_slot;
	struct x86_guest_irq_event guest_event = {0};
	enum x86_guest_irq_router_status status;
	uint32_t index;

	if (!native_event_is_valid(event))
		return X86_GUEST_IRQ_ROUTER_INVALID_ARGUMENT;
	status = producer_slot_status(router, producer, &producer_slot);
	if (status != X86_GUEST_IRQ_ROUTER_OK)
		return status;
	if (event->source_identity != producer->producer_identity)
		return X86_GUEST_IRQ_ROUTER_IDENTITY_MISMATCH;
	for (index = 0u; index < router->route_capacity; ++index) {
		const struct x86_guest_irq_route_slot *route =
			&router->routes[index];

		if (route->active != 0u &&
		    route->producer_slot == producer->slot &&
		    route->producer_generation == producer->producer_generation &&
		    route->native_kind == event->kind &&
		    route->native_irq == event->irq) {
			guest_event.pit_input_ticks = event->pit_input_ticks;
			guest_event.kind = route->guest_kind;
			guest_event.irq = route->guest_irq;
			return dispatch_guest_event(router, producer_slot,
						    &guest_event);
		}
	}
	router->unmapped_count = saturating_increment(router->unmapped_count);
	return X86_GUEST_IRQ_ROUTER_NOT_MAPPED;
}

// SPDX-License-Identifier: GPL-2.0-only
/*
 * AT keyboard driver/port binding and explicit lifetime ownership.
 *
 * Caller-owned, generation-bound objects make binding and teardown explicit.
 */
#include "private.h"

bool atkbd_internal_identity_valid(kernel_object_handle_t identity)
{
	return identity != 0u && identity != KERNEL_OBJECT_HANDLE_INVALID;
}

bool atkbd_internal_bytes_zero(const uint8_t *bytes, size_t count)
{
	size_t index;

	for (index = 0u; index < count; ++index) {
		if (bytes[index] != 0u)
			return false;
	}
	return true;
}

uint64_t atkbd_internal_saturating_increment(uint64_t value)
{
	return value == (uint64_t)-1 ? value : value + 1u;
}

static void bytes_clear(void *object, size_t size)
{
	uint8_t *bytes = object;
	size_t index;

	for (index = 0u; index < size; ++index)
		bytes[index] = 0u;
}

void atkbd_endpoint_construct(struct atkbd_endpoint *endpoint)
{
	if (endpoint == NULL)
		return;
	bytes_clear(endpoint, sizeof(*endpoint));
	input_device_construct(&endpoint->input_device);
	endpoint->lifecycle_cookie = ATKBD_ENDPOINT_COOKIE;
	endpoint->slot = ATKBD_SLOT_INVALID;
}

void atkbd_driver_construct(struct atkbd_driver *driver)
{
	if (driver == NULL)
		return;
	bytes_clear(driver, sizeof(*driver));
	serio_driver_construct(&driver->serio_driver);
	driver->lifecycle_cookie = ATKBD_DRIVER_COOKIE;
	driver->phase = ATKBD_DRIVER_EMPTY;
}

static bool endpoint_config_valid(const struct atkbd_endpoint_config *config)
{
	return config != NULL &&
	       atkbd_internal_identity_valid(config->port_identity) &&
	       atkbd_internal_identity_valid(config->input_device_identity) &&
	       config->input_queue != NULL && config->input_queue_capacity != 0u &&
	       (config->scan_mode == ATKBD_SCAN_TRANSLATED_SET1 ||
		config->scan_mode == ATKBD_SCAN_RAW_SET2) &&
	       config->start_enabled <= 1u &&
	       atkbd_internal_bytes_zero(config->reserved,
					 ARRAY_SIZE(config->reserved));
}

static bool endpoint_configs_unique(const struct atkbd_driver_config *config)
{
	uint16_t left;

	for (left = 0u; left < config->endpoint_count; ++left) {
		uint16_t right;

		if (!endpoint_config_valid(&config->endpoint_configs[left]))
			return false;
		for (right = (uint16_t)(left + 1u);
		     right < config->endpoint_count; ++right) {
			if (config->endpoint_configs[left].port_identity ==
				    config->endpoint_configs[right].port_identity ||
			    config->endpoint_configs[left].input_device_identity ==
				    config->endpoint_configs[right]
					    .input_device_identity)
				return false;
		}
	}
	return true;
}

static bool input_core_ready(const struct atkbd_driver_config *config)
{
	struct input_core_snapshot snapshot;
	enum input_status status;

	status = input_core_snapshot(config->input_core, &snapshot);
	return status == INPUT_OK &&
	       snapshot.identity == config->input_core_identity &&
	       (snapshot.phase == INPUT_CORE_PREPARED ||
		snapshot.phase == INPUT_CORE_QUIESCED);
}

static bool driver_config_valid(const struct atkbd_driver_config *config)
{
	return config != NULL &&
	       atkbd_internal_identity_valid(config->identity) &&
	       atkbd_internal_identity_valid(config->input_core_identity) &&
	       config->input_core != NULL && config->endpoints != NULL &&
	       config->endpoint_configs != NULL && config->endpoint_count != 0u &&
	       config->matches != NULL && config->match_count != 0u &&
	       config->command_write_limit != 0u &&
	       config->command_write_limit <=
		       CONFIG_ATKBD_COMMAND_WRITE_LIMIT_MAX &&
	       config->command_nak_limit != 0u &&
	       config->command_nak_limit <= CONFIG_ATKBD_COMMAND_NAK_LIMIT_MAX &&
	       atkbd_internal_bytes_zero(config->reserved,
					 ARRAY_SIZE(config->reserved)) &&
	       endpoint_configs_unique(config) && input_core_ready(config);
}

static enum serio_status map_input_to_serio(enum input_status status)
{
	switch (status) {
	case INPUT_OK:
		return SERIO_OK;
	case INPUT_CAPACITY_EXHAUSTED:
		return SERIO_CAPACITY_EXHAUSTED;
	case INPUT_IDENTITY_MISMATCH:
		return SERIO_IDENTITY_MISMATCH;
	case INPUT_BUSY:
	case INPUT_RETRY:
		return SERIO_RETRY;
	case INPUT_POISONED:
		return SERIO_POISONED;
	case INPUT_INVALID_ARGUMENT:
		return SERIO_INVALID_ARGUMENT;
	default:
		return SERIO_INVALID_STATE;
	}
}

static struct atkbd_endpoint *find_endpoint(struct atkbd_driver *owner,
					    kernel_object_handle_t port_identity)
{
	uint16_t slot;

	for (slot = 0u; slot < owner->config.endpoint_count; ++slot) {
		struct atkbd_endpoint *endpoint = &owner->config.endpoints[slot];

		if (endpoint->config.port_identity == port_identity)
			return endpoint;
	}
	return NULL;
}

static struct atkbd_driver *owner_from_serio(struct serio_driver *driver)
{
	return serio_driver_context(driver);
}

static enum serio_status connect_port(struct serio_port *port,
	struct serio_driver *driver, void **binding_context)
{
	struct atkbd_driver *owner = owner_from_serio(driver);
	struct atkbd_endpoint *endpoint;
	struct input_device_config input_config;
	struct input_device_binding input_binding;
	enum input_status input_status;
	uint64_t generation;

	if (port == NULL || driver == NULL || binding_context == NULL ||
	    owner == NULL ||
	    &owner->serio_driver != driver ||
	    owner->lifecycle_cookie != ATKBD_DRIVER_COOKIE ||
	    (owner->phase != ATKBD_DRIVER_PREPARED &&
	     owner->phase != ATKBD_DRIVER_ACTIVE) || owner->poisoned != 0u)
		return SERIO_INVALID_ARGUMENT;
	endpoint = find_endpoint(owner, port->config.identity);
	if (endpoint == NULL)
		return SERIO_NOT_FOUND;
	if (endpoint->lifecycle_cookie != ATKBD_ENDPOINT_COOKIE ||
	    endpoint->phase != ATKBD_ENDPOINT_EMPTY || endpoint->port != NULL)
		return SERIO_DRIVER_BUSY;
	if (endpoint->generation >= ATKBD_GENERATION_MAX)
		return SERIO_CAPACITY_EXHAUSTED;
	input_config = (struct input_device_config){
		.identity = endpoint->config.input_device_identity,
		.capabilities = INPUT_CAPABILITY_KEY,
		.queue = endpoint->config.input_queue,
		.queue_capacity = endpoint->config.input_queue_capacity,
		.reserved = {0u},
	};
	input_status = input_device_register(owner->config.input_core,
					     &endpoint->input_device,
					     &input_config, &input_binding);
	if (input_status != INPUT_OK)
		return map_input_to_serio(input_status);
	generation = endpoint->generation + 1u;
	endpoint->input_binding = input_binding;
	endpoint->owner = owner;
	endpoint->port = port;
	endpoint->generation = generation;
	endpoint->port_generation = port->generation;
	endpoint->phase = ATKBD_ENDPOINT_BOUND;
	endpoint->enabled = endpoint->config.start_enabled;
	endpoint->negotiated = 0u;
	endpoint->reconnect_required = 0u;
	endpoint->resend_pending = 0u;
	endpoint->resend_attempts = 0u;
	endpoint->in_flight = 0u;
	endpoint->reconnect_release_cursor = 0u;
	endpoint->reconnect_active = 0u;
	endpoint->protocol_committed = 0u;
	endpoint->write_uncertain = 0u;
	endpoint->decoded_count = 0u;
	endpoint->repeat_count = 0u;
	endpoint->unknown_count = 0u;
	endpoint->malformed_count = 0u;
	endpoint->bad_frame_count = 0u;
	endpoint->downstream_retry_count = 0u;
	endpoint->downstream_drop_count = 0u;
	endpoint->ack_count = 0u;
	endpoint->nak_count = 0u;
	endpoint->bat_count = 0u;
	endpoint->reconnect_count = 0u;
	endpoint->reconnect_release_count = 0u;
	endpoint->reconnect_retry_count = 0u;
	endpoint->command = (struct atkbd_command_state){0};
	atkbd_internal_decode_reset(&endpoint->decode);
	*binding_context = endpoint;
	return SERIO_OK;
}

static bool input_device_can_detach(const struct atkbd_endpoint *endpoint)
{
	struct input_device_snapshot snapshot;
	enum input_status status =
		input_device_snapshot(&endpoint->input_device, &snapshot);

	return status == INPUT_OK && endpoint->in_flight == 0u &&
	       snapshot.queue_count == 0u &&
	       snapshot.in_flight == 0u &&
	       snapshot.phase == INPUT_DEVICE_ACTIVE;
}

static bool reconnect_key_is_pressed(const struct atkbd_endpoint *endpoint,
				     input_key_code_t code)
{
	uint16_t word = (uint16_t)(code / 64u);
	uint8_t bit = (uint8_t)(code % 64u);

	return word < ARRAY_SIZE(endpoint->decode.pressed) &&
	       (endpoint->decode.pressed[word] & ((uint64_t)1u << bit)) != 0u;
}

static void reconnect_clear_key(struct atkbd_endpoint *endpoint,
				input_key_code_t code)
{
	uint16_t word = (uint16_t)(code / 64u);
	uint8_t bit = (uint8_t)(code % 64u);

	if (word < ARRAY_SIZE(endpoint->decode.pressed))
		endpoint->decode.pressed[word] &= ~((uint64_t)1u << bit);
}

/*
 * Reset releases every key recorded as down through the bounded input core.
 * A committed/deferred release advances the cursor once;
 * zero-commit backpressure preserves it for the next process-context call.
 */
static enum serio_status reconnect_release_pressed(
	struct atkbd_endpoint *endpoint)
{
	const uint16_t key_count =
		(uint16_t)(ARRAY_SIZE(endpoint->decode.pressed) * 64u);

	while (endpoint->reconnect_release_cursor < key_count) {
		input_key_code_t code = endpoint->reconnect_release_cursor;
		enum input_status status;

		if (!reconnect_key_is_pressed(endpoint, code)) {
			endpoint->reconnect_release_cursor++;
			continue;
		}
		status = input_submit(endpoint->owner->config.input_core,
			&endpoint->input_binding, INPUT_EVENT_KEY, code,
			INPUT_KEY_RELEASED, 0u, INPUT_EVENT_SYNTHETIC);
		switch (status) {
		case INPUT_OK:
		case INPUT_DEFERRED:
			reconnect_clear_key(endpoint, code);
			endpoint->reconnect_release_cursor++;
			endpoint->reconnect_release_count =
				atkbd_internal_saturating_increment(
					endpoint->reconnect_release_count);
			break;
		case INPUT_UNAVAILABLE:
			/* Focus teardown owns its transient state. There is no current
			 * consumer to leave stuck, so this bit can be retired. */
			reconnect_clear_key(endpoint, code);
			endpoint->reconnect_release_cursor++;
			endpoint->downstream_drop_count =
				atkbd_internal_saturating_increment(
					endpoint->downstream_drop_count);
			break;
		case INPUT_RETRY:
		case INPUT_BUSY:
		case INPUT_CAPACITY_EXHAUSTED:
			endpoint->reconnect_retry_count =
				atkbd_internal_saturating_increment(
					endpoint->reconnect_retry_count);
			return SERIO_RETRY;
		case INPUT_POISONED:
		case INPUT_HANDLER_FAULT:
			return SERIO_POISONED;
		case INPUT_ACCESS_DENIED:
		default:
			return SERIO_INVALID_STATE;
		}
	}
	return SERIO_OK;
}

void atkbd_internal_poison_endpoint(struct atkbd_endpoint *endpoint)
{
	if (endpoint == NULL)
		return;
	endpoint->phase = ATKBD_ENDPOINT_POISONED;
	endpoint->enabled = 0u;
	endpoint->reconnect_active = 0u;
	if (endpoint->owner != NULL) {
		endpoint->owner->poisoned = 1u;
		endpoint->owner->phase = ATKBD_DRIVER_POISONED;
	}
}

static void clear_detached_endpoint(struct atkbd_endpoint *endpoint)
{
	endpoint->port = NULL;
	endpoint->port_generation = 0u;
	endpoint->phase = ATKBD_ENDPOINT_EMPTY;
	endpoint->enabled = 0u;
	endpoint->negotiated = 0u;
	endpoint->reconnect_required = 0u;
	endpoint->resend_pending = 0u;
	endpoint->resend_attempts = 0u;
	endpoint->in_flight = 0u;
	endpoint->reconnect_release_cursor = 0u;
	endpoint->reconnect_active = 0u;
	endpoint->protocol_committed = 0u;
	endpoint->write_uncertain = 0u;
	endpoint->command = (struct atkbd_command_state){0};
	endpoint->input_binding = (struct input_device_binding){0};
	atkbd_internal_decode_reset(&endpoint->decode);
}

static void disconnect_port(struct serio_port *port,
	struct serio_driver *driver, void *binding_context)
{
	struct atkbd_driver *owner = owner_from_serio(driver);
	struct atkbd_endpoint *endpoint = binding_context;
	enum input_status status;

	if (port == NULL || driver == NULL || owner == NULL || endpoint == NULL ||
	    &owner->serio_driver != driver || endpoint->owner != owner ||
	    endpoint->port != port ||
	    (endpoint->phase != ATKBD_ENDPOINT_BOUND &&
	     endpoint->phase != ATKBD_ENDPOINT_POISONED) ||
	    endpoint->port_generation != port->generation) {
		atkbd_internal_poison_endpoint(endpoint);
		return;
	}
	endpoint->enabled = 0u;
	if (!input_device_can_detach(endpoint)) {
		atkbd_internal_poison_endpoint(endpoint);
		return;
	}
	status = input_device_quiesce(owner->config.input_core,
				      &endpoint->input_binding);
	if (status != INPUT_OK) {
		atkbd_internal_poison_endpoint(endpoint);
		return;
	}
	status = input_device_unregister(owner->config.input_core,
					 &endpoint->input_binding);
	if (status != INPUT_OK) {
		atkbd_internal_poison_endpoint(endpoint);
		return;
	}
	clear_detached_endpoint(endpoint);
}

static enum serio_status reconnect_port(struct serio_port *port,
	struct serio_driver *driver, void *binding_context)
{
	struct atkbd_driver *owner = owner_from_serio(driver);
	struct atkbd_endpoint *endpoint = binding_context;

	if (port == NULL || owner == NULL || endpoint == NULL ||
	    &owner->serio_driver != driver ||
	    owner->phase != ATKBD_DRIVER_ACTIVE || owner->poisoned != 0u ||
	    endpoint->owner != owner || endpoint->port != port ||
	    endpoint->phase != ATKBD_ENDPOINT_BOUND ||
	    endpoint->port_generation != port->generation)
		return SERIO_INVALID_STATE;
	if (endpoint->reconnect_active == 0u) {
		endpoint->enabled = 0u;
		/* Key code zero is the internal RESERVED value, never a key. */
		endpoint->reconnect_release_cursor = 1u;
		endpoint->reconnect_active = 1u;
		atkbd_internal_decode_cancel_sequence(&endpoint->decode);
	}
	{
		enum serio_status status = reconnect_release_pressed(endpoint);

		if (status != SERIO_OK)
			return status;
	}
	atkbd_internal_decode_reset(&endpoint->decode);
	endpoint->command = (struct atkbd_command_state){0};
	endpoint->resend_pending = 0u;
	endpoint->resend_attempts = 0u;
	endpoint->reconnect_required = 0u;
	endpoint->reconnect_release_cursor = 0u;
	endpoint->reconnect_active = 0u;
	endpoint->negotiated = 0u;
	endpoint->enabled = endpoint->config.start_enabled;
	endpoint->reconnect_count = atkbd_internal_saturating_increment(
		endpoint->reconnect_count);
	return SERIO_OK;
}

static void prepare_endpoint(struct atkbd_driver *driver, uint16_t slot)
{
	struct atkbd_endpoint *endpoint = &driver->config.endpoints[slot];

	endpoint->config = driver->config.endpoint_configs[slot];
	endpoint->owner = driver;
	endpoint->slot = slot;
	endpoint->phase = ATKBD_ENDPOINT_EMPTY;
	endpoint->port = NULL;
	endpoint->port_generation = 0u;
	endpoint->enabled = 0u;
	endpoint->negotiated = 0u;
	endpoint->reconnect_required = 0u;
	endpoint->resend_pending = 0u;
	endpoint->resend_attempts = 0u;
	endpoint->in_flight = 0u;
	endpoint->reconnect_release_cursor = 0u;
	endpoint->reconnect_active = 0u;
	endpoint->protocol_committed = 0u;
	endpoint->write_uncertain = 0u;
	endpoint->command = (struct atkbd_command_state){0};
	atkbd_internal_decode_reset(&endpoint->decode);
}

enum atkbd_status atkbd_driver_register(
	struct atkbd_driver *driver, struct serio_registry *registry,
	const struct atkbd_driver_config *config)
{
	struct serio_driver_config serio_config;
	enum serio_status status;
	uint16_t slot;
	uint64_t generation;

	if (driver == NULL || registry == NULL || !driver_config_valid(config))
		return ATKBD_INVALID_ARGUMENT;
	if (driver->lifecycle_cookie != ATKBD_DRIVER_COOKIE ||
	    driver->serio_driver.lifecycle_cookie == 0u)
		return ATKBD_INVALID_ARGUMENT;
	if (driver->phase != ATKBD_DRIVER_EMPTY)
		return driver->phase == ATKBD_DRIVER_POISONED ? ATKBD_POISONED
							    : ATKBD_INVALID_STATE;
	if (driver->generation >= ATKBD_GENERATION_MAX)
		return ATKBD_CAPACITY_EXHAUSTED;
	for (slot = 0u; slot < config->endpoint_count; ++slot) {
		if (config->endpoints[slot].lifecycle_cookie !=
			    ATKBD_ENDPOINT_COOKIE ||
		    config->endpoints[slot].phase != ATKBD_ENDPOINT_EMPTY ||
		    config->endpoints[slot].owner != NULL)
			return ATKBD_INVALID_ARGUMENT;
	}
	generation = driver->generation + 1u;
	driver->config = *config;
	driver->generation = generation;
	driver->phase = ATKBD_DRIVER_PREPARED;
	driver->poisoned = 0u;
	for (slot = 0u; slot < config->endpoint_count; ++slot)
		prepare_endpoint(driver, slot);
	serio_config = (struct serio_driver_config){
		.identity = config->identity,
		.driver_context = driver,
		.matches = config->matches,
		.match_count = config->match_count,
		.manual_bind = 0u,
		.reserved = {0u},
		.connect = connect_port,
		.disconnect = disconnect_port,
		.reconnect = reconnect_port,
		.interrupt = atkbd_internal_interrupt,
	};
	status = serio_driver_register(registry, &driver->serio_driver,
				       &serio_config);
	if (status != SERIO_OK) {
		for (slot = 0u; slot < config->endpoint_count; ++slot) {
			config->endpoints[slot].owner = NULL;
			config->endpoints[slot].slot = ATKBD_SLOT_INVALID;
		}
		driver->config = (struct atkbd_driver_config){0};
		driver->phase = ATKBD_DRIVER_EMPTY;
		return status == SERIO_CAPACITY_EXHAUSTED
			       ? ATKBD_CAPACITY_EXHAUSTED
			       : ATKBD_INVALID_STATE;
	}
	if (driver->poisoned != 0u)
		return ATKBD_POISONED;
	driver->phase = ATKBD_DRIVER_ACTIVE;
	return ATKBD_OK;
}

enum atkbd_status atkbd_driver_unregister(
	struct atkbd_driver *driver, kernel_object_handle_t identity)
{
	struct atkbd_endpoint *endpoints;
	uint16_t endpoint_count;
	uint16_t slot;
	enum serio_status status;

	if (driver == NULL || !atkbd_internal_identity_valid(identity) ||
	    driver->lifecycle_cookie != ATKBD_DRIVER_COOKIE)
		return ATKBD_INVALID_ARGUMENT;
	if (driver->config.identity != identity)
		return ATKBD_IDENTITY_MISMATCH;
	if (driver->phase != ATKBD_DRIVER_ACTIVE &&
	    driver->phase != ATKBD_DRIVER_POISONED)
		return ATKBD_INVALID_STATE;
	endpoints = driver->config.endpoints;
	endpoint_count = driver->config.endpoint_count;
	for (slot = 0u; slot < endpoint_count; ++slot) {
		if (endpoints[slot].phase != ATKBD_ENDPOINT_EMPTY ||
		    endpoints[slot].port != NULL)
			return ATKBD_BUSY;
	}
	status = serio_driver_unregister(&driver->serio_driver);
	if (status != SERIO_OK)
		return status == SERIO_RETRY ? ATKBD_RETRY
					    : ATKBD_INVALID_STATE;
	for (slot = 0u; slot < endpoint_count; ++slot) {
		endpoints[slot].owner = NULL;
		endpoints[slot].slot = ATKBD_SLOT_INVALID;
		endpoints[slot].config = (struct atkbd_endpoint_config){0};
	}
	driver->config = (struct atkbd_driver_config){0};
	driver->phase = ATKBD_DRIVER_EMPTY;
	driver->poisoned = 0u;
	return ATKBD_OK;
}

enum atkbd_status atkbd_internal_reference_status(
	const struct atkbd_driver *driver,
	const struct atkbd_endpoint_reference *reference,
	struct atkbd_endpoint **endpoint)
{
	struct atkbd_endpoint *candidate;

	if (driver == NULL || reference == NULL || endpoint == NULL ||
	    driver->lifecycle_cookie != ATKBD_DRIVER_COOKIE ||
	    !atkbd_internal_identity_valid(reference->owner_identity) ||
	    !atkbd_internal_identity_valid(reference->port_identity) ||
	    !atkbd_internal_bytes_zero(reference->reserved,
				      ARRAY_SIZE(reference->reserved)))
		return ATKBD_INVALID_ARGUMENT;
	if (driver->phase == ATKBD_DRIVER_POISONED || driver->poisoned != 0u)
		return ATKBD_POISONED;
	if (driver->phase != ATKBD_DRIVER_ACTIVE)
		return ATKBD_INVALID_STATE;
	if (reference->owner_identity != driver->config.identity)
		return ATKBD_IDENTITY_MISMATCH;
	if (reference->owner_generation != driver->generation)
		return ATKBD_STALE_REFERENCE;
	if (reference->slot >= driver->config.endpoint_count)
		return ATKBD_INVALID_ARGUMENT;
	candidate = &driver->config.endpoints[reference->slot];
	if (candidate->lifecycle_cookie != ATKBD_ENDPOINT_COOKIE ||
	    candidate->owner != driver || candidate->phase != ATKBD_ENDPOINT_BOUND ||
	    candidate->port == NULL)
		return ATKBD_STALE_REFERENCE;
	if (candidate->config.port_identity != reference->port_identity)
		return ATKBD_IDENTITY_MISMATCH;
	if (candidate->generation != reference->endpoint_generation ||
	    candidate->port_generation != reference->port_generation ||
	    candidate->port->generation != reference->port_generation)
		return ATKBD_STALE_REFERENCE;
	*endpoint = candidate;
	return ATKBD_OK;
}

enum atkbd_status atkbd_endpoint_reference(
	const struct atkbd_driver *driver, kernel_object_handle_t identity,
	uint16_t slot, struct atkbd_endpoint_reference *reference)
{
	const struct atkbd_endpoint *endpoint;

	if (driver == NULL || reference == NULL ||
	    !atkbd_internal_identity_valid(identity) ||
	    driver->lifecycle_cookie != ATKBD_DRIVER_COOKIE)
		return ATKBD_INVALID_ARGUMENT;
	if (driver->phase == ATKBD_DRIVER_POISONED || driver->poisoned != 0u)
		return ATKBD_POISONED;
	if (driver->phase != ATKBD_DRIVER_ACTIVE)
		return ATKBD_INVALID_STATE;
	if (driver->config.identity != identity)
		return ATKBD_IDENTITY_MISMATCH;
	if (slot >= driver->config.endpoint_count)
		return ATKBD_INVALID_ARGUMENT;
	endpoint = &driver->config.endpoints[slot];
	if (endpoint->phase != ATKBD_ENDPOINT_BOUND || endpoint->port == NULL)
		return ATKBD_UNAVAILABLE;
	*reference = (struct atkbd_endpoint_reference){
		.owner_identity = driver->config.identity,
		.owner_generation = driver->generation,
		.port_identity = endpoint->config.port_identity,
		.port_generation = endpoint->port_generation,
		.endpoint_generation = endpoint->generation,
		.slot = slot,
		.reserved = {0u},
	};
	return ATKBD_OK;
}

enum atkbd_status atkbd_endpoint_snapshot(
	const struct atkbd_driver *driver,
	const struct atkbd_endpoint_reference *reference,
	struct atkbd_endpoint_snapshot *snapshot)
{
	struct atkbd_endpoint *endpoint;
	struct serio_port_snapshot port_snapshot;
	enum atkbd_status status;

	if (snapshot == NULL)
		return ATKBD_INVALID_ARGUMENT;
	status = atkbd_internal_reference_status(driver, reference, &endpoint);
	if (status != ATKBD_OK)
		return status;
	if (serio_port_snapshot(endpoint->port, &port_snapshot) != SERIO_OK)
		return ATKBD_INVALID_STATE;
	*snapshot = (struct atkbd_endpoint_snapshot){
		.owner_identity = driver->config.identity,
		.port_identity = endpoint->config.port_identity,
		.input_device_identity = endpoint->config.input_device_identity,
		.owner_generation = driver->generation,
		.endpoint_generation = endpoint->generation,
		.port_generation = endpoint->port_generation,
		.decoded_count = endpoint->decoded_count,
		.repeat_count = endpoint->repeat_count,
		.unknown_count = endpoint->unknown_count,
		.malformed_count = endpoint->malformed_count,
		.bad_frame_count = endpoint->bad_frame_count,
		.downstream_retry_count = endpoint->downstream_retry_count,
		.downstream_drop_count = endpoint->downstream_drop_count,
		.ack_count = endpoint->ack_count,
		.nak_count = endpoint->nak_count,
		.bat_count = endpoint->bat_count,
		.reconnect_count = endpoint->reconnect_count,
		.reconnect_release_count = endpoint->reconnect_release_count,
		.reconnect_retry_count = endpoint->reconnect_retry_count,
		.stream_loss_epoch = port_snapshot.stream_loss_epoch,
		.stream_recovery_epoch = port_snapshot.stream_recovery_epoch,
		.reconnect_release_cursor = endpoint->reconnect_release_cursor,
		.scan_mode = endpoint->config.scan_mode,
		.phase = endpoint->phase,
		.enabled = endpoint->enabled,
		.negotiated = endpoint->negotiated,
		.reconnect_required = endpoint->reconnect_required,
		.resend_pending = endpoint->resend_pending,
		.command_kind = endpoint->command.kind,
		.command_phase = endpoint->command.phase,
		.in_flight = endpoint->in_flight,
		.reconnect_active = endpoint->reconnect_active,
		.protocol_committed = endpoint->protocol_committed,
		.write_uncertain = endpoint->write_uncertain,
		.stream_recovery_required = port_snapshot.recovery_required,
		.stream_isolated = port_snapshot.stream_isolated,
		.stream_recovery_abandoned = port_snapshot.recovery_abandoned,
		.reserved = {0u},
	};
	return ATKBD_OK;
}

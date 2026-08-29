/* SPDX-License-Identifier: GPL-2.0-only */
/* Native AT/PS2 keyboard protocol driver; emits only internal input events. */
#ifndef DOSC32_ATKBD_H
#define DOSC32_ATKBD_H

#include "input.h"
#include "serio.h"

#include "../config/atkbd.h"

enum atkbd_status {
	ATKBD_OK = 0,
	ATKBD_EMPTY,
	ATKBD_WAITING,
	ATKBD_RETRY,
	ATKBD_UNAVAILABLE,
	ATKBD_INVALID_ARGUMENT,
	ATKBD_INVALID_STATE,
	ATKBD_CAPACITY_EXHAUSTED,
	ATKBD_IDENTITY_MISMATCH,
	ATKBD_STALE_REFERENCE,
	ATKBD_BUSY,
	ATKBD_PROTOCOL_ERROR,
	/* A hardware write may have committed; only owner isolation is safe. */
	ATKBD_WRITE_UNCERTAIN,
	ATKBD_POISONED
};

enum atkbd_scan_mode {
	ATKBD_SCAN_TRANSLATED_SET1 = 1,
	ATKBD_SCAN_RAW_SET2
};

enum atkbd_command_kind {
	ATKBD_COMMAND_NONE = 0,
	ATKBD_COMMAND_NEGOTIATE,
	ATKBD_COMMAND_SET_LEDS,
	ATKBD_COMMAND_SET_TYPEMATIC,
	ATKBD_COMMAND_ENABLE,
	ATKBD_COMMAND_DISABLE
};

enum atkbd_command_phase {
	ATKBD_COMMAND_IDLE = 0,
	ATKBD_COMMAND_READY,
	ATKBD_COMMAND_WAIT_ACK,
	ATKBD_COMMAND_COMPLETE,
	ATKBD_COMMAND_FAILED
};

enum atkbd_endpoint_phase {
	ATKBD_ENDPOINT_EMPTY = 0,
	ATKBD_ENDPOINT_BOUND,
	ATKBD_ENDPOINT_POISONED
};

enum atkbd_driver_phase {
	ATKBD_DRIVER_EMPTY = 0,
	ATKBD_DRIVER_PREPARED,
	ATKBD_DRIVER_ACTIVE,
	ATKBD_DRIVER_POISONED
};

struct atkbd_endpoint_config {
	kernel_object_handle_t port_identity;
	kernel_object_handle_t input_device_identity;
	struct input_event *input_queue;
	uint16_t input_queue_capacity;
	uint8_t scan_mode;
	uint8_t start_enabled;
	uint8_t reserved[4];
};

struct atkbd_driver;

struct atkbd_decode_state {
	uint64_t pressed[CONFIG_ATKBD_PRESSED_WORD_COUNT];
	uint8_t sequence[CONFIG_ATKBD_SPECIAL_SEQUENCE_MAX];
	uint8_t sequence_length;
	uint8_t release;
	uint8_t extended;
	uint8_t translated_response_bits;
	uint8_t reserved[4];
};

struct atkbd_command_state {
	uint8_t script[CONFIG_ATKBD_COMMAND_SCRIPT_MAX];
	uint8_t script_length;
	uint8_t script_index;
	uint8_t kind;
	uint8_t phase;
	uint8_t writes;
	uint8_t byte_naks;
	uint8_t reserved;
};

struct atkbd_endpoint {
	struct input_device input_device;
	struct input_device_binding input_binding;
	struct atkbd_endpoint_config config;
	struct atkbd_driver *owner;
	struct serio_port *port;
	struct atkbd_decode_state decode;
	struct atkbd_command_state command;
	uint64_t generation;
	uint64_t port_generation;
	uint64_t decoded_count;
	uint64_t repeat_count;
	uint64_t unknown_count;
	uint64_t malformed_count;
	uint64_t bad_frame_count;
	uint64_t downstream_retry_count;
	uint64_t downstream_drop_count;
	uint64_t ack_count;
	uint64_t nak_count;
	uint64_t bat_count;
	uint64_t reconnect_count;
	uint64_t reconnect_release_count;
	uint64_t reconnect_retry_count;
	uint32_t lifecycle_cookie;
	uint16_t slot;
	uint16_t reconnect_release_cursor;
	uint8_t phase;
	uint8_t enabled;
	uint8_t negotiated;
	uint8_t reconnect_required;
	uint8_t resend_pending;
	uint8_t resend_attempts;
	uint8_t in_flight;
	uint8_t reconnect_active;
	/* Sticky for this endpoint generation; exposed for owner rollback policy. */
	uint8_t protocol_committed;
	uint8_t write_uncertain;
	uint8_t reserved;
} __aligned(8);

struct atkbd_driver_config {
	kernel_object_handle_t identity;
	kernel_object_handle_t input_core_identity;
	struct input_core *input_core;
	struct atkbd_endpoint *endpoints;
	const struct atkbd_endpoint_config *endpoint_configs;
	uint16_t endpoint_count;
	uint8_t command_write_limit;
	uint8_t command_nak_limit;
	const struct serio_device_id *matches;
	size_t match_count;
	uint8_t reserved[8];
};

struct atkbd_driver {
	struct serio_driver serio_driver;
	struct atkbd_driver_config config;
	uint64_t generation;
	uint32_t lifecycle_cookie;
	uint8_t phase;
	uint8_t poisoned;
	uint8_t reserved[2];
} __aligned(8);

struct atkbd_endpoint_reference {
	kernel_object_handle_t owner_identity;
	uint64_t owner_generation;
	kernel_object_handle_t port_identity;
	uint64_t port_generation;
	uint64_t endpoint_generation;
	uint16_t slot;
	uint8_t reserved[6];
} __aligned(8);

struct atkbd_endpoint_snapshot {
	kernel_object_handle_t owner_identity;
	kernel_object_handle_t port_identity;
	kernel_object_handle_t input_device_identity;
	uint64_t owner_generation;
	uint64_t endpoint_generation;
	uint64_t port_generation;
	uint64_t decoded_count;
	uint64_t repeat_count;
	uint64_t unknown_count;
	uint64_t malformed_count;
	uint64_t bad_frame_count;
	uint64_t downstream_retry_count;
	uint64_t downstream_drop_count;
	uint64_t ack_count;
	uint64_t nak_count;
	uint64_t bat_count;
	uint64_t reconnect_count;
	uint64_t reconnect_release_count;
	uint64_t reconnect_retry_count;
	uint64_t stream_loss_epoch;
	uint64_t stream_recovery_epoch;
	uint16_t reconnect_release_cursor;
	uint8_t scan_mode;
	uint8_t phase;
	uint8_t enabled;
	uint8_t negotiated;
	uint8_t reconnect_required;
	uint8_t resend_pending;
	uint8_t command_kind;
	uint8_t command_phase;
	uint8_t in_flight;
	uint8_t reconnect_active;
	uint8_t protocol_committed;
	uint8_t write_uncertain;
	uint8_t stream_recovery_required;
	uint8_t stream_isolated;
	uint8_t stream_recovery_abandoned;
	uint8_t reserved[2];
} __aligned(8);

void atkbd_driver_construct(struct atkbd_driver *driver);
void atkbd_endpoint_construct(struct atkbd_endpoint *endpoint);

/*
 * matches and endpoint_configs remain borrowed for the registered lifetime.
 * Register while the input core is PREPARED/QUIESCED. Before detaching a
 * bound serio port, drain its input FIFO and quiesce that same input core;
 * disconnect then removes the exact generation-bound input device. All ports
 * must be detached before driver teardown.
 */
enum atkbd_status atkbd_driver_register(
	struct atkbd_driver *driver, struct serio_registry *registry,
	const struct atkbd_driver_config *config) __must_check;
enum atkbd_status atkbd_driver_unregister(
	struct atkbd_driver *driver,
	kernel_object_handle_t identity) __must_check;

enum atkbd_status atkbd_endpoint_reference(
	const struct atkbd_driver *driver, kernel_object_handle_t identity,
	uint16_t slot, struct atkbd_endpoint_reference *reference) __must_check;
enum atkbd_status atkbd_command_begin(
	struct atkbd_driver *driver,
	const struct atkbd_endpoint_reference *reference,
	enum atkbd_command_kind kind, uint8_t value) __must_check;
/* Process-context only; performs at most one bounded serio write. */
enum atkbd_status atkbd_process(
	struct atkbd_driver *driver,
	const struct atkbd_endpoint_reference *reference) __must_check;
enum atkbd_status atkbd_endpoint_snapshot(
	const struct atkbd_driver *driver,
	const struct atkbd_endpoint_reference *reference,
	struct atkbd_endpoint_snapshot *snapshot) __must_check;

static_assert_expression(sizeof(struct atkbd_endpoint_reference) == 48u,
			 "atkbd endpoint reference layout changed");

#endif

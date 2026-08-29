/* SPDX-License-Identifier: GPL-2.0-only */
/* DOS-C32 serial-input bus. Native pointers never cross a guest ABI. */
#ifndef DOSC32_SERIO_H
#define DOSC32_SERIO_H

#include "object_identity.h"

#define SERIO_MATCH_ANY 0xffu

#define SERIO_RAW_PARITY_ERROR (1u << 0)
#define SERIO_RAW_TIMEOUT_ERROR (1u << 1)
#define SERIO_RAW_FRAME_ERROR (1u << 2)
#define SERIO_RAW_STATUS_UNRECOGNIZED (1u << 3)
#define SERIO_RAW_FLAG_MASK                                               \
	(SERIO_RAW_PARITY_ERROR | SERIO_RAW_TIMEOUT_ERROR |                 \
	 SERIO_RAW_FRAME_ERROR | SERIO_RAW_STATUS_UNRECOGNIZED)

enum serio_status {
	SERIO_OK = 0,
	SERIO_EMPTY,
	SERIO_RETRY,
	/* A source byte was consumed but could not be represented downstream. */
	SERIO_STREAM_LOST,
	/* Explicit process-context stream recovery has not completed yet. */
	SERIO_RECOVERY_PENDING,
	/* The affected stream is intentionally closed until owner teardown. */
	SERIO_STREAM_ISOLATED,
	SERIO_UNAVAILABLE,
	SERIO_INVALID_ARGUMENT,
	SERIO_INVALID_STATE,
	SERIO_CAPACITY_EXHAUSTED,
	SERIO_IDENTITY_MISMATCH,
	SERIO_ALREADY_REGISTERED,
	SERIO_NOT_FOUND,
	SERIO_PARENT_BUSY,
	SERIO_DRIVER_BUSY,
	SERIO_STALE_EVENT,
	SERIO_POISONED
};

enum serio_receive_result {
	SERIO_RECEIVE_HANDLED = 0,
	SERIO_RECEIVE_DEFER,
	SERIO_RECEIVE_REJECTED
};

/*
 * A port write reports whether the byte crossed the irreversible device
 * boundary independently from the transport status.  A caller may retry only
 * SERIO_RETRY with SERIO_WRITE_ZERO_COMMIT.  COMMITTED must never be replayed;
 * UNCERTAIN requires protocol recovery or isolation before another write.
 */
enum serio_write_commit {
	SERIO_WRITE_ZERO_COMMIT = 0,
	SERIO_WRITE_COMMITTED,
	SERIO_WRITE_UNCERTAIN
};

struct serio_write_result {
	enum serio_status status;
	enum serio_write_commit commit;
};

enum serio_port_phase {
	SERIO_PORT_EMPTY = 0,
	SERIO_PORT_PREPARED,
	SERIO_PORT_ACTIVE,
	SERIO_PORT_QUIESCED,
	SERIO_PORT_POISONED
};

struct serio_device_id {
	uint8_t type;
	uint8_t protocol;
	uint8_t id;
	uint8_t extra;
};

struct serio_raw_event {
	kernel_object_handle_t port_identity;
	kernel_object_handle_t driver_identity;
	uint64_t port_generation;
	uint64_t binding_generation;
	uint64_t sequence;
	uint8_t data;
	uint8_t raw_status;
	uint8_t flags;
	uint8_t reserved[5];
} __aligned(8);

struct serio_registry;
struct serio_port;
struct serio_driver;

typedef void (*serio_irq_guard_fn)(kernel_object_handle_t context);
typedef enum serio_status (*serio_port_open_fn)(
	struct serio_port *port, struct serio_driver *driver,
	void *binding_context);
typedef void (*serio_port_close_fn)(struct serio_port *port,
				    struct serio_driver *driver,
				    void *binding_context);
typedef struct serio_write_result (*serio_port_write_fn)(
	struct serio_port *port, uint8_t data);
typedef enum serio_status (*serio_port_start_fn)(struct serio_port *port);
typedef void (*serio_port_stop_fn)(struct serio_port *port);
typedef enum serio_status (*serio_driver_connect_fn)(
	struct serio_port *port, struct serio_driver *driver,
	void **binding_context);
typedef void (*serio_driver_disconnect_fn)(struct serio_port *port,
					   struct serio_driver *driver,
					   void *binding_context);
typedef enum serio_status (*serio_driver_reconnect_fn)(
	struct serio_port *port, struct serio_driver *driver,
	void *binding_context);
typedef enum serio_receive_result (*serio_driver_interrupt_fn)(
	struct serio_port *port, struct serio_driver *driver,
	void *binding_context, const struct serio_raw_event *event);

struct serio_port_config {
	kernel_object_handle_t identity;
	kernel_object_handle_t parent_identity;
	struct serio_device_id device_id;
	uint8_t manual_bind;
	uint8_t caller_serializes_irq;
	uint8_t reserved[2];
	kernel_object_handle_t callback_context;
	void *port_context;
	serio_irq_guard_fn irq_enter;
	serio_irq_guard_fn irq_exit;
	serio_port_start_fn start;
	serio_port_stop_fn stop;
	serio_port_open_fn open;
	serio_port_close_fn close;
	serio_port_write_fn write;
	struct serio_raw_event *queue;
	uint16_t queue_capacity;
	uint16_t reserved_capacity;
};

struct serio_driver_config {
	kernel_object_handle_t identity;
	void *driver_context;
	const struct serio_device_id *matches;
	size_t match_count;
	uint8_t manual_bind;
	uint8_t reserved[7];
	serio_driver_connect_fn connect;
	serio_driver_disconnect_fn disconnect;
	serio_driver_reconnect_fn reconnect;
	serio_driver_interrupt_fn interrupt;
};

/* Caller-owned objects; mutate them only through the serio API. */
struct serio_port {
	struct serio_port_config config;
	struct serio_registry *registry;
	struct serio_driver *driver;
	void *binding_context;
	uint64_t generation;
	uint64_t binding_generation;
	uint64_t next_sequence;
	uint64_t received_count;
	uint64_t deferred_count;
	uint64_t rejected_count;
	uint64_t overflow_count;
	uint64_t unbound_count;
	uint64_t stale_binding_drop_count;
	uint64_t stream_loss_epoch;
	uint64_t stream_recovery_epoch;
	uint64_t lost_byte_count;
	uint64_t recovery_discard_count;
	uint64_t recovery_count;
	uint64_t isolation_count;
	uint64_t parent_generation;
	uint32_t lifecycle_cookie;
	uint16_t queue_head;
	uint16_t queue_count;
	uint16_t registry_slot;
	uint8_t depth;
	uint8_t phase;
	uint8_t accepting;
	uint8_t in_flight;
	uint8_t recovery_required;
	uint8_t stream_isolated;
	uint8_t recovery_abandoned;
};

struct serio_driver {
	struct serio_driver_config config;
	struct serio_registry *registry;
	uint64_t generation;
	uint32_t lifecycle_cookie;
	uint16_t registry_slot;
	uint8_t registered;
	uint8_t withdrawing;
	uint8_t reserved[4];
};

struct serio_registry {
	kernel_object_handle_t identity;
	uint64_t generation;
	uint32_t lifecycle_cookie;
	struct serio_port **ports;
	struct serio_driver **drivers;
	uint16_t port_capacity;
	uint16_t driver_capacity;
	uint16_t port_count;
	uint16_t driver_count;
	uint8_t active;
	uint8_t poisoned;
	uint8_t reserved[6];
};

struct serio_port_snapshot {
	kernel_object_handle_t identity;
	kernel_object_handle_t parent_identity;
	kernel_object_handle_t driver_identity;
	uint64_t generation;
	uint64_t binding_generation;
	uint64_t received_count;
	uint64_t deferred_count;
	uint64_t rejected_count;
	uint64_t overflow_count;
	uint64_t unbound_count;
	uint64_t stale_binding_drop_count;
	uint64_t stream_loss_epoch;
	uint64_t stream_recovery_epoch;
	uint64_t lost_byte_count;
	uint64_t recovery_discard_count;
	uint64_t recovery_count;
	uint64_t isolation_count;
	uint64_t parent_generation;
	struct serio_device_id device_id;
	uint16_t queue_capacity;
	uint16_t queue_count;
	uint8_t depth;
	uint8_t phase;
	uint8_t accepting;
	uint8_t in_flight;
	uint8_t recovery_required;
	uint8_t stream_isolated;
	uint8_t recovery_abandoned;
} __aligned(8);

enum serio_status serio_registry_initialize(
	struct serio_registry *registry, kernel_object_handle_t identity,
	struct serio_port **ports, uint16_t port_capacity,
	struct serio_driver **drivers, uint16_t driver_capacity) __must_check;
void serio_registry_construct(struct serio_registry *registry);
void serio_port_construct(struct serio_port *port);
void serio_driver_construct(struct serio_driver *driver);
enum serio_status serio_registry_replace_storage(
	struct serio_registry *registry, kernel_object_handle_t identity,
	struct serio_port **ports, uint16_t port_capacity,
	struct serio_driver **drivers, uint16_t driver_capacity) __must_check;
enum serio_status serio_registry_poison(
	struct serio_registry *registry,
	kernel_object_handle_t identity) __must_check;

enum serio_status serio_port_prepare(struct serio_registry *registry,
	struct serio_port *port,
	const struct serio_port_config *config) __must_check;
enum serio_status serio_port_publish(struct serio_port *port) __must_check;
enum serio_status serio_port_abort(struct serio_port *port) __must_check;
enum serio_status serio_port_quiesce(struct serio_port *port) __must_check;
enum serio_status serio_port_unregister(struct serio_port *port) __must_check;
enum serio_status serio_port_unregister_subtree(
	struct serio_port *root) __must_check;
enum serio_status serio_port_reconnect(struct serio_port *port) __must_check;

/*
 * A stream-loss epoch starts only after a source byte was irreversibly
 * consumed without being delivered or queued. IRQ producers must stop
 * consuming that port while serio_port_receive_preflight() reports loss.
 * SERIO_RETRY from the status query is pre-consumption backpressure: the
 * source byte is still owned by hardware and must not be read yet.
 * Recovery is process-context only: it discards pre-loss raw records, asks
 * the bound driver to reset protocol/pressed state, and reopens the stream
 * only after that callback succeeds. RECOVERY_PENDING is never an ordinary
 * replay request for the already-lost byte. Isolation is terminal for this
 * binding and leaves the port closed for explicit owner teardown.
 */
enum serio_status serio_port_receive_preflight(
	const struct serio_port *port, uint64_t *loss_epoch) __must_check;
enum serio_status serio_port_recover_stream(
	struct serio_port *port, uint64_t loss_epoch) __must_check;
enum serio_status serio_port_isolate_stream(
	struct serio_port *port, uint64_t loss_epoch) __must_check;
struct serio_write_result serio_write(struct serio_port *port,
	uint8_t data) __must_check;
enum serio_status serio_port_bind(struct serio_port *port,
	kernel_object_handle_t driver_identity) __must_check;
enum serio_status serio_port_unbind(struct serio_port *port) __must_check;

enum serio_status serio_driver_register(struct serio_registry *registry,
	struct serio_driver *driver,
	const struct serio_driver_config *config) __must_check;
enum serio_status serio_driver_unregister(
	struct serio_driver *driver) __must_check;
void *serio_driver_context(
	const struct serio_driver *driver) __must_check;

/* IRQ-safe: direct delivery is allowed only while the deferred FIFO is empty. */
enum serio_status serio_interrupt(struct serio_port *port, uint8_t data,
	uint8_t raw_status, uint8_t flags) __must_check;
/* Process-context delivery of queued bytes; RETRY preserves FIFO order. */
enum serio_status serio_port_pump(struct serio_port *port,
	uint16_t budget, uint16_t *delivered) __must_check;
enum serio_status serio_port_peek(struct serio_port *port,
	struct serio_raw_event *event) __must_check;
enum serio_status serio_port_consume(struct serio_port *port,
	uint64_t sequence) __must_check;
enum serio_status serio_port_snapshot(const struct serio_port *port,
	struct serio_port_snapshot *snapshot) __must_check;

static_assert_expression(sizeof(struct serio_device_id) == 4u,
			 "serio match ID layout changed");
static_assert_expression(sizeof(struct serio_raw_event) == 48u,
			 "serio raw event layout changed");

#endif

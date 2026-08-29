/* SPDX-License-Identifier: GPL-2.0-only */
/* Internal input-device/handler core; no backend or guest ABI is exposed. */
#ifndef DOSC32_INPUT_H
#define DOSC32_INPUT_H

#include "object_identity.h"

#define INPUT_CAPABILITY_KEY (1u << 0)
#define INPUT_CAPABILITY_POINTER (1u << 1)
#define INPUT_CAPABILITY_MASK \
	(INPUT_CAPABILITY_KEY | INPUT_CAPABILITY_POINTER)

#define INPUT_EVENT_EXTENDED (1u << 0)
#define INPUT_EVENT_SYNTHETIC (1u << 1)
#define INPUT_EVENT_FLAG_MASK (INPUT_EVENT_EXTENDED | INPUT_EVENT_SYNTHETIC)

enum input_status {
	INPUT_OK = 0,
	INPUT_EMPTY,
	INPUT_DEFERRED,
	INPUT_UNAVAILABLE,
	INPUT_RETRY,
	INPUT_INVALID_ARGUMENT,
	INPUT_INVALID_STATE,
	INPUT_CAPACITY_EXHAUSTED,
	INPUT_IDENTITY_MISMATCH,
	INPUT_ALREADY_REGISTERED,
	INPUT_NOT_FOUND,
	INPUT_STALE_BINDING,
	INPUT_ACCESS_DENIED,
	INPUT_BUSY,
	INPUT_HANDLER_FAULT,
	INPUT_POISONED
};

enum input_core_phase {
	INPUT_CORE_UNINITIALIZED = 0,
	INPUT_CORE_PREPARED,
	INPUT_CORE_ACTIVE,
	INPUT_CORE_QUIESCED,
	INPUT_CORE_RETIRED,
	INPUT_CORE_POISONED_PHASE
};

enum input_device_phase {
	INPUT_DEVICE_EMPTY = 0,
	INPUT_DEVICE_ACTIVE,
	INPUT_DEVICE_QUIESCED,
	INPUT_DEVICE_POISONED_PHASE
};

enum input_handler_phase {
	INPUT_HANDLER_EMPTY = 0,
	INPUT_HANDLER_ACTIVE,
	INPUT_HANDLER_QUIESCED,
	INPUT_HANDLER_POISONED_PHASE
};

enum input_event_type {
	INPUT_EVENT_KEY = 1
};

enum input_key_value {
	INPUT_KEY_RELEASED = 0,
	INPUT_KEY_PRESSED,
	INPUT_KEY_REPEATED
};

enum input_handler_result {
	INPUT_HANDLER_HANDLED = 0,
	INPUT_HANDLER_DEFER,
	INPUT_HANDLER_REJECTED,
	INPUT_HANDLER_BROKEN
};

enum input_focus_result {
	INPUT_FOCUS_OK = 0,
	INPUT_FOCUS_REJECTED,
	INPUT_FOCUS_BROKEN
};

/*
 * Key codes use one stable internal namespace for auditable keyboard maps.
 * These values are never a user ABI.
 */
typedef uint16_t input_key_code_t;

struct input_event {
	kernel_object_handle_t device_identity;
	kernel_object_handle_t handler_identity;
	uint64_t device_generation;
	uint64_t focus_generation;
	uint64_t sequence;
	uint32_t hardware_code;
	input_key_code_t code;
	uint8_t type;
	uint8_t value;
	uint8_t flags;
	uint8_t reserved[7];
} __aligned(8);

struct input_core;
struct input_device;
struct input_handler;

typedef void (*input_irq_guard_fn)(kernel_object_handle_t context);
typedef enum input_focus_result (*input_focus_enter_fn)(
	struct input_handler *handler, kernel_object_handle_t context,
	kernel_object_handle_t core_identity, uint64_t focus_generation);
typedef void (*input_focus_leave_fn)(
	struct input_handler *handler, kernel_object_handle_t context,
	kernel_object_handle_t core_identity, uint64_t focus_generation);
typedef enum input_handler_result (*input_receive_fn)(
	struct input_handler *handler, kernel_object_handle_t context,
	const struct input_event *event);

struct input_core_config {
	kernel_object_handle_t identity;
	kernel_object_handle_t guard_context;
	input_irq_guard_fn irq_enter;
	input_irq_guard_fn irq_exit;
	uint8_t caller_serializes_irq;
	uint8_t reserved[7];
};

struct input_device_config {
	kernel_object_handle_t identity;
	uint32_t capabilities;
	struct input_event *queue;
	uint16_t queue_capacity;
	uint8_t reserved[2];
};

struct input_handler_config {
	kernel_object_handle_t identity;
	kernel_object_handle_t context;
	void *handler_context;
	uint32_t capabilities;
	input_focus_enter_fn focus_enter;
	input_focus_leave_fn focus_leave;
	input_receive_fn receive;
	uint8_t reserved[4];
};

struct input_device_binding {
	kernel_object_handle_t core_identity;
	uint64_t core_generation;
	kernel_object_handle_t device_identity;
	uint64_t device_generation;
	uint32_t slot;
	uint8_t reserved[4];
} __aligned(8);

struct input_handler_binding {
	kernel_object_handle_t core_identity;
	uint64_t core_generation;
	kernel_object_handle_t handler_identity;
	uint64_t handler_generation;
	uint32_t slot;
	uint8_t reserved[4];
} __aligned(8);

/* Caller-owned objects and pointer arrays may be heap-backed after boot. */
struct input_device {
	struct input_device_config config;
	struct input_core *core;
	uint64_t generation;
	uint64_t next_sequence;
	uint64_t submitted_count;
	uint64_t deferred_count;
	uint64_t rejected_count;
	uint64_t overflow_count;
	uint64_t stale_focus_drop_count;
	uint32_t lifecycle_cookie;
	uint16_t queue_head;
	uint16_t queue_count;
	uint16_t registry_slot;
	uint8_t phase;
	uint8_t in_flight;
	uint8_t reserved[4];
} __aligned(8);

struct input_handler {
	struct input_handler_config config;
	struct input_core *core;
	uint64_t generation;
	uint64_t handled_count;
	uint64_t deferred_count;
	uint64_t rejected_count;
	uint64_t fault_count;
	uint32_t lifecycle_cookie;
	uint16_t registry_slot;
	uint8_t phase;
	uint8_t in_flight;
	uint8_t reserved[4];
} __aligned(8);

struct input_core {
	kernel_object_handle_t identity;
	kernel_object_handle_t guard_context;
	uint64_t generation;
	uint64_t focus_generation;
	uint64_t unfocused_drop_count;
	struct input_device **devices;
	struct input_handler **handlers;
	struct input_handler *focus;
	input_irq_guard_fn irq_enter;
	input_irq_guard_fn irq_exit;
	uint32_t lifecycle_cookie;
	uint16_t device_capacity;
	uint16_t handler_capacity;
	uint16_t device_count;
	uint16_t handler_count;
	uint8_t phase;
	uint8_t caller_serializes_irq;
	uint8_t dispatch_active;
	uint8_t reserved[5];
} __aligned(8);

struct input_core_snapshot {
	kernel_object_handle_t identity;
	kernel_object_handle_t focus_identity;
	uint64_t generation;
	uint64_t focus_generation;
	uint64_t unfocused_drop_count;
	uint16_t device_capacity;
	uint16_t handler_capacity;
	uint16_t device_count;
	uint16_t handler_count;
	uint8_t phase;
	uint8_t dispatch_active;
	uint8_t reserved[6];
} __aligned(8);

struct input_device_snapshot {
	kernel_object_handle_t identity;
	uint64_t generation;
	uint64_t next_sequence;
	uint64_t submitted_count;
	uint64_t deferred_count;
	uint64_t rejected_count;
	uint64_t overflow_count;
	uint64_t stale_focus_drop_count;
	uint32_t capabilities;
	uint16_t queue_capacity;
	uint16_t queue_count;
	uint8_t phase;
	uint8_t in_flight;
	uint8_t reserved[6];
} __aligned(8);

void input_core_construct(struct input_core *core);
void input_device_construct(struct input_device *device);
void input_handler_construct(struct input_handler *handler);
void *input_handler_context(
	const struct input_handler *handler) __must_check;

enum input_status input_core_initialize(
	struct input_core *core, const struct input_core_config *config,
	struct input_device **devices, uint16_t device_capacity,
	struct input_handler **handlers,
	uint16_t handler_capacity) __must_check;
enum input_status input_core_replace_storage(
	struct input_core *core, kernel_object_handle_t identity,
	struct input_device **devices, uint16_t device_capacity,
	struct input_handler **handlers,
	uint16_t handler_capacity) __must_check;
enum input_status input_core_publish(
	struct input_core *core, kernel_object_handle_t identity) __must_check;
enum input_status input_core_quiesce(
	struct input_core *core, kernel_object_handle_t identity) __must_check;
enum input_status input_core_resume(
	struct input_core *core, kernel_object_handle_t identity) __must_check;
enum input_status input_core_retire(
	struct input_core *core, kernel_object_handle_t identity) __must_check;
enum input_status input_core_poison(
	struct input_core *core, kernel_object_handle_t identity) __must_check;

enum input_status input_device_register(
	struct input_core *core, struct input_device *device,
	const struct input_device_config *config,
	struct input_device_binding *binding) __must_check;
enum input_status input_device_quiesce(
	struct input_core *core,
	const struct input_device_binding *binding) __must_check;
enum input_status input_device_unregister(
	struct input_core *core,
	const struct input_device_binding *binding) __must_check;

enum input_status input_handler_register(
	struct input_core *core, struct input_handler *handler,
	const struct input_handler_config *config,
	struct input_handler_binding *binding) __must_check;
enum input_status input_handler_quiesce(
	struct input_core *core,
	const struct input_handler_binding *binding) __must_check;
enum input_status input_handler_unregister(
	struct input_core *core,
	const struct input_handler_binding *binding) __must_check;

/* Focus publication is atomic with respect to input_submit(). */
enum input_status input_focus_set(
	struct input_core *core,
	const struct input_handler_binding *binding) __must_check;
enum input_status input_focus_clear(
	struct input_core *core,
	const struct input_handler_binding *binding) __must_check;

/* IRQ-safe: no allocation, blocking, or topology mutation. */
enum input_status input_submit(
	struct input_core *core,
	const struct input_device_binding *device,
	uint8_t type, input_key_code_t code, uint8_t value,
	uint32_t hardware_code, uint8_t flags) __must_check;
/* Process-context retry of decoded events; stale-focus events are discarded. */
enum input_status input_device_pump(
	struct input_core *core,
	const struct input_device_binding *device,
	uint16_t budget, uint16_t *delivered) __must_check;

enum input_status input_core_snapshot(
	const struct input_core *core,
	struct input_core_snapshot *snapshot) __must_check;
enum input_status input_device_snapshot(
	const struct input_device *device,
	struct input_device_snapshot *snapshot) __must_check;

static_assert_expression(sizeof(struct input_event) == 56u,
			 "input event layout changed");
static_assert_expression(sizeof(struct input_device_binding) == 40u,
			 "input device binding layout changed");
static_assert_expression(sizeof(struct input_handler_binding) == 40u,
			 "input handler binding layout changed");
static_assert_expression(
	__builtin_offsetof(struct input_handler_config, handler_context) == 16u,
	"input handler private-context offset changed");
static_assert_expression(
	__builtin_offsetof(struct input_handler_config, capabilities) ==
		16u + sizeof(void *),
	"input handler capability offset changed");

#endif

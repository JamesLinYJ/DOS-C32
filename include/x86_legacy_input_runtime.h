/* SPDX-License-Identifier: GPL-2.0-only */
/* Native legacy-keyboard topology and lifetime owner. */
#ifndef DOSC32_X86_LEGACY_INPUT_RUNTIME_H
#define DOSC32_X86_LEGACY_INPUT_RUNTIME_H

#include "atkbd.h"
#include "guest_ps2_keyboard.h"
#include "keyboard.h"
#include "x86_legacy_irq.h"
#include "x86_native_i8042.h"
#include "x86_native_input.h"

enum x86_legacy_input_status {
	X86_LEGACY_INPUT_OK = 0,
	X86_LEGACY_INPUT_RETRY,
	/* A consumed-byte loss is still in bounded process-context recovery. */
	X86_LEGACY_INPUT_RECOVERY_PENDING,
	/* The exact failed keyboard stream is terminally closed. */
	X86_LEGACY_INPUT_STREAM_ISOLATED,
	X86_LEGACY_INPUT_UNAVAILABLE,
	X86_LEGACY_INPUT_INVALID_ARGUMENT,
	X86_LEGACY_INPUT_INVALID_STATE,
	X86_LEGACY_INPUT_CAPACITY_EXHAUSTED,
	X86_LEGACY_INPUT_IDENTITY_MISMATCH,
	X86_LEGACY_INPUT_STALE_REFERENCE,
	X86_LEGACY_INPUT_BUSY,
	X86_LEGACY_INPUT_IO_ERROR,
	X86_LEGACY_INPUT_NEGOTIATION_FAILED,
	X86_LEGACY_INPUT_PROTOCOL_COMMITTED,
	X86_LEGACY_INPUT_PROTOCOL_UNCERTAIN,
	X86_LEGACY_INPUT_POISONED
};

enum x86_legacy_input_phase {
	X86_LEGACY_INPUT_UNINITIALIZED = 0,
	X86_LEGACY_INPUT_EMPTY,
	X86_LEGACY_INPUT_PREPARED,
	X86_LEGACY_INPUT_PUBLISHING,
	X86_LEGACY_INPUT_ACTIVE,
	X86_LEGACY_INPUT_RETIRED,
	X86_LEGACY_INPUT_POISONED_PHASE
};

enum x86_legacy_input_focus {
	X86_LEGACY_INPUT_FOCUS_NONE = 0,
	X86_LEGACY_INPUT_FOCUS_CONSOLE,
	X86_LEGACY_INPUT_FOCUS_GUEST
};

/*
 * Storage is supplied by the boot owner today and may be heap-backed later.
 * Capacities are policy limits; non-NULL slots are the only topology truth.
 */
struct x86_legacy_input_storage {
	struct serio_port **serio_ports;
	struct serio_driver **serio_drivers;
	struct input_device **input_devices;
	struct input_handler **input_handlers;
	struct serio_raw_event *keyboard_bytes;
	struct input_event *decoded_events;
	struct keyboard_key_record *console_keys;
	uint16_t serio_port_capacity;
	uint16_t serio_driver_capacity;
	uint16_t input_device_capacity;
	uint16_t input_handler_capacity;
	uint16_t keyboard_byte_capacity;
	uint16_t decoded_event_capacity;
	uint16_t console_key_capacity;
	uint8_t reserved[2];
};

struct x86_legacy_input_runtime_config {
	kernel_object_handle_t identity;
	kernel_object_handle_t serio_registry_identity;
	kernel_object_handle_t input_core_identity;
	kernel_object_handle_t controller_identity;
	/* Capability handle presented to the selected platform I/O callbacks. */
	kernel_object_handle_t io_context_identity;
	kernel_object_handle_t keyboard_port_identity;
	kernel_object_handle_t atkbd_driver_identity;
	kernel_object_handle_t input_device_identity;
	kernel_object_handle_t console_handler_identity;
	kernel_object_handle_t guest_handler_identity;
	/* Input-handler callback identity; not a guest address-space handle. */
	kernel_object_handle_t guest_context_identity;
	kernel_object_handle_t guest_machine_identity;
	kernel_object_handle_t guest_source_identity;
	kernel_object_handle_t legacy_irq_source_identity;
	kernel_object_handle_t irq_action_identity;
	x86_native_i8042_read8_fn control_read8;
	x86_native_i8042_write8_fn control_write8;
	x86_native_input_read8_fn input_read8;
	x86_native_input_write8_fn input_write8;
	/* Production passes keyboard_console_x86_wait; host tests may inject one. */
	keyboard_console_wait_fn console_wait;
	void *console_wait_context;
	struct x86_legacy_input_storage storage;
	uint32_t controller_poll_limit;
	uint32_t input_write_poll_limit;
	uint32_t negotiation_step_limit;
	uint16_t controller_drain_limit;
	uint8_t controller_stability_attempts;
	uint8_t atkbd_command_write_limit;
	uint8_t atkbd_command_nak_limit;
	uint8_t controller_present;
	uint8_t keyboard_present;
	uint8_t presence_evidence;
	uint8_t reserved[7];
} __aligned(8);

struct x86_legacy_input_pump_result {
	uint64_t serio_events;
	uint64_t input_events;
	uint64_t guest_device_events;
	uint64_t active_captures;
	uint64_t stream_recoveries;
	uint8_t made_progress;
	uint8_t reserved[7];
} __aligned(8);

struct x86_legacy_input_snapshot {
	kernel_object_handle_t identity;
	kernel_object_handle_t focused_handler_identity;
	uint64_t generation;
	uint64_t irq_count;
	uint64_t irq_empty_count;
	uint64_t irq_fault_count;
	uint64_t pump_count;
	uint64_t negotiation_step_count;
	uint64_t protocol_write_attempt_count;
	uint64_t source_backpressure_count;
	uint64_t active_capture_count;
	uint64_t stream_loss_epoch;
	uint64_t stream_recovery_epoch;
	uint64_t stream_recovery_attempt_count;
	uint64_t stream_isolation_count;
	uint16_t raw_queue_count;
	uint16_t decoded_queue_count;
	uint16_t console_queue_count;
	uint8_t phase;
	uint8_t focus;
	uint8_t scan_mode;
	uint8_t translation_enabled;
	uint8_t protocol_committed;
	uint8_t protocol_write_uncertain;
	uint8_t irq_action_registered;
	uint8_t controller_quarantined;
	uint8_t source_backpressure_pending;
	uint8_t stream_recovery_pending;
	uint8_t stream_isolated;
	uint8_t io_fault_pending;
	uint8_t poisoned;
	uint8_t reserved[4];
} __aligned(8);

/* Caller-owned object. All topology and focus calls require IF=0. */
struct x86_legacy_input_runtime {
	struct x86_legacy_input_runtime_config config;
	struct serio_registry serio_registry;
	struct input_core input_core;
	struct x86_native_i8042_control controller_control;
	struct x86_native_input_controller native_input;
	struct atkbd_driver atkbd_driver;
	struct atkbd_endpoint atkbd_endpoint;
	struct atkbd_endpoint_config atkbd_endpoint_config;
	struct atkbd_endpoint_reference atkbd_reference;
	struct serio_device_id keyboard_match;
	struct keyboard_console console;
	struct guest_ps2_keyboard guest_keyboard;
	struct guest_ps2_keyboard_reference guest_reference;
	struct x86_native_irq_action_binding irq_binding;
	uint64_t generation;
	uint64_t irq_count;
	uint64_t irq_empty_count;
	uint64_t irq_fault_count;
	uint64_t pump_count;
	uint64_t negotiation_step_count;
	uint64_t protocol_write_attempt_count;
	uint64_t source_backpressure_count;
	uint64_t active_capture_count;
	uint64_t stream_loss_epoch;
	uint64_t stream_recovery_epoch;
	uint64_t stream_recovery_attempt_count;
	uint64_t stream_isolation_count;
	uint32_t lifecycle_cookie;
	uint32_t acquired;
	uint8_t phase;
	uint8_t focus;
	uint8_t scan_mode;
	uint8_t translation_enabled;
	uint8_t protocol_committed;
	uint8_t protocol_write_uncertain;
	uint8_t controller_quarantined;
	uint8_t source_backpressure_pending;
	uint8_t stream_recovery_pending;
	uint8_t stream_isolated;
	uint8_t io_fault_pending;
	uint8_t poisoned;
} __aligned(8);

void x86_legacy_input_runtime_construct(
	struct x86_legacy_input_runtime *runtime);
enum x86_legacy_input_status x86_legacy_input_runtime_prepare(
	struct x86_legacy_input_runtime *runtime,
	const struct x86_legacy_input_runtime_config *config) __must_check;
/*
 * Publish performs bounded keyboard negotiation, selects the native console,
 * and registers IRQ1 while the legacy PIC domain is still masked. A proven
 * committed protocol byte ends the rollback right. An uncertain write also
 * forbids rollback, but remains distinguishable in the runtime snapshot.
 */
enum x86_legacy_input_status x86_legacy_input_runtime_publish(
	struct x86_legacy_input_runtime *runtime,
	kernel_object_handle_t identity) __must_check;
enum x86_legacy_input_status x86_legacy_input_runtime_focus_console(
	struct x86_legacy_input_runtime *runtime,
	kernel_object_handle_t identity) __must_check;
enum x86_legacy_input_status x86_legacy_input_runtime_focus_guest(
	struct x86_legacy_input_runtime *runtime,
	kernel_object_handle_t identity) __must_check;
/* RECOVERY_PENDING is exact-epoch work, never replay of a lost byte. A
 * STREAM_ISOLATED runtime remains inspectable, but its IRQ source is closed. */
enum x86_legacy_input_status x86_legacy_input_runtime_pump(
	struct x86_legacy_input_runtime *runtime,
	kernel_object_handle_t identity, uint16_t budget,
	struct x86_legacy_input_pump_result *result) __must_check;
/* Exact only before the keyboard-protocol commit point. */
enum x86_legacy_input_status x86_legacy_input_runtime_rollback(
	struct x86_legacy_input_runtime *runtime,
	kernel_object_handle_t identity) __must_check;
enum x86_legacy_input_status x86_legacy_input_runtime_snapshot(
	const struct x86_legacy_input_runtime *runtime,
	kernel_object_handle_t identity,
	struct x86_legacy_input_snapshot *snapshot) __must_check;

static_assert_expression(sizeof(struct x86_legacy_input_pump_result) == 48u,
			 "legacy input pump-result layout changed");

#endif

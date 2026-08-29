/* SPDX-License-Identifier: GPL-2.0-only */
/* Native i8042 backend for the DOS-C32 serio bus. */
#ifndef DOSC32_X86_NATIVE_INPUT_H
#define DOSC32_X86_NATIVE_INPUT_H

#include "serio.h"

enum x86_native_input_status {
	X86_NATIVE_INPUT_OK = 0,
	X86_NATIVE_INPUT_EMPTY,
	X86_NATIVE_INPUT_DROPPED,
	X86_NATIVE_INPUT_RETRY,
	/* Output byte remains in hardware because receive preflight blocked it. */
	X86_NATIVE_INPUT_SOURCE_BACKPRESSURE,
	X86_NATIVE_INPUT_STREAM_LOST,
	X86_NATIVE_INPUT_RECOVERY_PENDING,
	X86_NATIVE_INPUT_STREAM_ISOLATED,
	X86_NATIVE_INPUT_UNAVAILABLE,
	X86_NATIVE_INPUT_IO_ERROR,
	X86_NATIVE_INPUT_INVALID_ARGUMENT,
	X86_NATIVE_INPUT_INVALID_STATE,
	X86_NATIVE_INPUT_CAPACITY_EXHAUSTED,
	X86_NATIVE_INPUT_IDENTITY_MISMATCH,
	X86_NATIVE_INPUT_POISONED
};

enum x86_native_input_irq_result {
	X86_NATIVE_INPUT_IRQ_NONE = 0,
	X86_NATIVE_INPUT_IRQ_HANDLED,
	X86_NATIVE_INPUT_IRQ_FAULT
};

enum x86_native_input_endpoint {
	X86_NATIVE_INPUT_KEYBOARD = 0,
	X86_NATIVE_INPUT_AUXILIARY
};

enum x86_native_input_evidence {
	X86_NATIVE_INPUT_EVIDENCE_NONE = 0,
	X86_NATIVE_INPUT_EVIDENCE_PLATFORM_ASSIGNED,
	X86_NATIVE_INPUT_EVIDENCE_FIRMWARE_REPORTED,
	X86_NATIVE_INPUT_EVIDENCE_CONTROLLER_OBSERVED
};

typedef enum x86_native_input_status (*x86_native_input_read8_fn)(
	kernel_object_handle_t context, uint16_t port, uint8_t *value);
/* OK proves the port write completed.  Once this callback is invoked, a
 * non-OK result does not prove that hardware observed no write; the native
 * serio backend therefore reports an uncertain commit boundary. */
typedef enum x86_native_input_status (*x86_native_input_write8_fn)(
	kernel_object_handle_t context, uint16_t port, uint8_t value);

struct x86_native_input_config {
	kernel_object_handle_t controller_identity;
	kernel_object_handle_t callback_context;
	kernel_object_handle_t keyboard_port_identity;
	kernel_object_handle_t auxiliary_port_identity;
	struct serio_registry *registry;
	struct serio_raw_event *keyboard_queue;
	struct serio_raw_event *auxiliary_queue;
	x86_native_input_read8_fn read8;
	x86_native_input_write8_fn write8;
	serio_irq_guard_fn irq_enter;
	serio_irq_guard_fn irq_exit;
	uint16_t data_port;
	uint16_t status_port;
	uint16_t command_port;
	uint16_t keyboard_queue_capacity;
	uint16_t auxiliary_queue_capacity;
	uint32_t write_poll_limit;
	struct serio_device_id keyboard_id;
	struct serio_device_id auxiliary_id;
	uint8_t present;
	uint8_t keyboard_present;
	uint8_t auxiliary_present;
	uint8_t presence_evidence;
	uint8_t caller_serializes_irq;
	uint8_t writes_supported;
	uint8_t status_allowed_mask;
	uint8_t status_output_full_mask;
	uint8_t status_input_full_mask;
	uint8_t status_auxiliary_mask;
	uint8_t status_parity_mask;
	uint8_t status_timeout_mask;
	uint8_t status_frame_mask;
	uint8_t reserved[3];
};

struct x86_native_input_controller {
	struct x86_native_input_config config;
	struct serio_port keyboard_port;
	struct serio_port auxiliary_port;
	uint64_t generation;
	uint64_t empty_poll_count;
	uint64_t io_error_count;
	uint64_t unrouted_count;
	uint64_t write_count;
	uint64_t write_timeout_count;
	uint64_t write_error_count;
	uint32_t lifecycle_cookie;
	uint8_t phase;
	uint8_t reserved[3];
};

struct x86_native_input_snapshot {
	kernel_object_handle_t controller_identity;
	uint64_t generation;
	uint64_t empty_poll_count;
	uint64_t io_error_count;
	uint64_t unrouted_count;
	uint64_t write_count;
	uint64_t write_timeout_count;
	uint64_t write_error_count;
	uint64_t stream_loss_count;
	uint64_t stream_recovery_count;
	uint64_t stream_isolation_count;
	uint64_t keyboard_stream_loss_epoch;
	uint64_t keyboard_stream_recovery_epoch;
	uint64_t auxiliary_stream_loss_epoch;
	uint64_t auxiliary_stream_recovery_epoch;
	uint16_t data_port;
	uint16_t status_port;
	uint16_t command_port;
	uint8_t present;
	uint8_t keyboard_present;
	uint8_t auxiliary_present;
	uint8_t presence_evidence;
	uint8_t writes_supported;
	uint8_t prepared;
	uint8_t active;
	uint8_t quiesced;
	uint8_t poisoned;
	uint8_t keyboard_recovery_required;
	uint8_t keyboard_stream_isolated;
	uint8_t keyboard_recovery_abandoned;
	uint8_t auxiliary_recovery_required;
	uint8_t auxiliary_stream_isolated;
	uint8_t auxiliary_recovery_abandoned;
} __aligned(8);

enum x86_native_input_status x86_native_input_prepare(
	struct x86_native_input_controller *controller,
	const struct x86_native_input_config *config) __must_check;
void x86_native_input_construct(
	struct x86_native_input_controller *controller);
enum x86_native_input_status x86_native_input_publish(
	struct x86_native_input_controller *controller,
	kernel_object_handle_t controller_identity) __must_check;
enum x86_native_input_status x86_native_input_abort(
	struct x86_native_input_controller *controller,
	kernel_object_handle_t controller_identity) __must_check;
enum x86_native_input_status x86_native_input_quiesce(
	struct x86_native_input_controller *controller,
	kernel_object_handle_t controller_identity) __must_check;
enum x86_native_input_status x86_native_input_retire(
	struct x86_native_input_controller *controller,
	kernel_object_handle_t controller_identity) __must_check;
enum x86_native_input_status x86_native_input_poison(
	struct x86_native_input_controller *controller,
	kernel_object_handle_t controller_identity) __must_check;

/* IRQ/poll boundary: status once, then data once only when output-full. */
enum x86_native_input_status x86_native_input_capture(
	struct x86_native_input_controller *controller,
	kernel_object_handle_t controller_identity) __must_check;
enum x86_native_input_irq_result x86_native_input_irq(
	struct x86_native_input_controller *controller,
	kernel_object_handle_t controller_identity) __must_check;
/* Process-context recovery/isolation of one exact consumed-byte loss epoch. */
enum x86_native_input_status x86_native_input_recover_stream(
	struct x86_native_input_controller *controller,
	kernel_object_handle_t controller_identity,
	enum x86_native_input_endpoint endpoint,
	uint64_t loss_epoch) __must_check;
enum x86_native_input_status x86_native_input_isolate_stream(
	struct x86_native_input_controller *controller,
	kernel_object_handle_t controller_identity,
	enum x86_native_input_endpoint endpoint,
	uint64_t loss_epoch) __must_check;
enum x86_native_input_status x86_native_input_snapshot(
	const struct x86_native_input_controller *controller,
	kernel_object_handle_t controller_identity,
	struct x86_native_input_snapshot *snapshot) __must_check;

#endif

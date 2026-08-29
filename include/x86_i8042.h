/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Generation-bound guest i8042 keyboard-controller model.
 *
 * Hardware contract: IBM AT-compatible i8042 and PS/2 controller ports.
 * Safety changes: isolated software state, bounded FIFOs, typed events,
 * direction-specific I/O policy, and suppression of native reset writes.
 */
#ifndef DOSC32_X86_I8042_H
#define DOSC32_X86_I8042_H

#include "x86_io_resource.h"

#define X86_I8042_RESOURCE_COUNT 2u

#define X86_I8042_DATA_PORT 0x0060u
#define X86_I8042_STATUS_PORT 0x0064u
#define X86_I8042_COMMAND_PORT X86_I8042_STATUS_PORT

#define X86_I8042_STATUS_OUTPUT_FULL 0x01u
#define X86_I8042_STATUS_INPUT_FULL 0x02u
#define X86_I8042_STATUS_SYSTEM 0x04u
#define X86_I8042_STATUS_COMMAND 0x08u
#define X86_I8042_STATUS_KEY_UNLOCKED 0x10u
#define X86_I8042_STATUS_AUXILIARY 0x20u
#define X86_I8042_STATUS_TIMEOUT 0x40u
#define X86_I8042_STATUS_PARITY 0x80u

#define X86_I8042_COMMAND_BYTE_IRQ1 0x01u
#define X86_I8042_COMMAND_BYTE_IRQ12 0x02u
#define X86_I8042_COMMAND_BYTE_SYSTEM 0x04u
#define X86_I8042_COMMAND_BYTE_IGNORE_LOCK 0x08u
#define X86_I8042_COMMAND_BYTE_KEYBOARD_DISABLED 0x10u
#define X86_I8042_COMMAND_BYTE_AUXILIARY_DISABLED 0x20u
#define X86_I8042_COMMAND_BYTE_TRANSLATE 0x40u

#define X86_I8042_OUTPUT_PORT_RESET_HIGH 0x01u
#define X86_I8042_OUTPUT_PORT_A20 0x02u

#define X86_I8042_INPUT_KEYBOARD (1u << 0)
#define X86_I8042_INPUT_AUXILIARY (1u << 1)
#define X86_I8042_INPUT_CAPABILITIES                                      \
	(X86_I8042_INPUT_KEYBOARD | X86_I8042_INPUT_AUXILIARY)

struct x86_i8042_config {
	uint8_t command_byte;
	uint8_t input_port;
	uint8_t output_port;
	uint8_t keyboard_present;
	uint8_t auxiliary_present;
	uint8_t keyboard_scanning_enabled;
	uint8_t keyboard_scan_set;
	uint8_t keyboard_unlocked;
	uint8_t keyboard_leds;
	uint8_t keyboard_typematic;
	uint8_t keyboard_id_length;
	uint8_t keyboard_id_first;
	uint8_t keyboard_id_second;
	uint8_t auxiliary_id;
	uint8_t reserved[2];
} __aligned(8);

struct x86_i8042_input_config {
	uint32_t capabilities;
	uint8_t reserved[4];
} __aligned(8);

struct x86_i8042_input_binding {
	kernel_object_handle_t context_identity;
	kernel_object_handle_t owner_identity;
	uint64_t controller_generation;
	kernel_object_handle_t source_identity;
	uint64_t source_generation;
	uint32_t capabilities;
	uint8_t reserved[4];
} __aligned(8);

struct x86_i8042_keyboard_mode {
	kernel_object_handle_t source_identity;
	uint64_t controller_generation;
	uint64_t source_generation;
	uint64_t mode_generation;
	uint8_t scan_set;
	uint8_t translation_enabled;
	uint8_t scanning_enabled;
	uint8_t interface_enabled;
	uint8_t reserved[4];
} __aligned(8);

enum x86_i8042_input_kind {
	X86_I8042_INPUT_KIND_KEYBOARD_SCAN = 1,
	X86_I8042_INPUT_KIND_AUXILIARY_BYTE
};

enum x86_i8042_event_kind {
	X86_I8042_EVENT_IRQ_REQUEST = 1,
	X86_I8042_EVENT_A20_CHANGE
};

struct x86_i8042_event {
	uint64_t controller_generation;
	uint64_t sequence;
	uint8_t kind;
	uint8_t irq;
	uint8_t a20_enabled;
	uint8_t reserved[5];
} __aligned(8);

struct x86_i8042_snapshot {
	uint64_t generation;
	kernel_object_handle_t context_identity;
	kernel_object_handle_t owner_identity;
	kernel_object_handle_t input_source_identity;
	uint64_t input_source_generation;
	uint64_t output_overflow_count;
	uint64_t event_overflow_count;
	uint64_t suppressed_reset_requests;
	uint64_t unsupported_command_count;
	uint8_t command_byte;
	uint8_t input_port;
	uint8_t output_port;
	uint8_t status;
	uint8_t pending_command;
	uint8_t output_count;
	uint8_t event_count;
	uint8_t pending_irq_mask;
	uint8_t keyboard_present;
	uint8_t auxiliary_present;
	uint8_t keyboard_scanning_enabled;
	uint8_t keyboard_scan_set;
	uint8_t keyboard_leds;
	uint8_t keyboard_typematic;
	uint8_t keyboard_unlocked;
	uint8_t active;
	uint8_t poisoned;
	uint8_t input_source_bound;
	uint8_t a20_enabled;
	uint8_t keyboard_id_length;
	uint8_t keyboard_id_first;
	uint8_t keyboard_id_second;
	uint8_t auxiliary_id;
	uint8_t input_source_quiesced;
} __aligned(8);

enum x86_i8042_status {
	X86_I8042_OK = 0,
	X86_I8042_INVALID_ARGUMENT,
	X86_I8042_INVALID_STATE,
	X86_I8042_CAPACITY_EXHAUSTED,
	X86_I8042_IDENTITY_MISMATCH,
	X86_I8042_POISONED,
	X86_I8042_INPUT_DISABLED,
	X86_I8042_NO_EVENT,
	X86_I8042_STALE_EVENT,
	X86_I8042_STALE_BINDING,
	X86_I8042_MODE_CHANGED
};

enum x86_i8042_status x86_i8042_prepare(
	kernel_object_handle_t context_identity,
	kernel_object_handle_t owner_identity,
	const struct x86_i8042_config *config,
	struct x86_io_resource_descriptor *descriptors,
	size_t descriptor_capacity) __must_check;
enum x86_i8042_status x86_i8042_publish(
	kernel_object_handle_t context_identity) __must_check;
enum x86_i8042_status x86_i8042_abort(
	kernel_object_handle_t context_identity) __must_check;
enum x86_i8042_status x86_i8042_poison(
	kernel_object_handle_t context_identity) __must_check;
enum x86_i8042_status x86_i8042_input_bind(
	kernel_object_handle_t context_identity,
	kernel_object_handle_t owner_identity,
	kernel_object_handle_t source_identity,
	const struct x86_i8042_input_config *config,
	struct x86_i8042_input_binding *binding) __must_check;
enum x86_i8042_status x86_i8042_input_quiesce(
	kernel_object_handle_t context_identity,
	kernel_object_handle_t owner_identity,
	const struct x86_i8042_input_binding *binding) __must_check;
enum x86_i8042_status x86_i8042_input_resume(
	kernel_object_handle_t context_identity,
	kernel_object_handle_t owner_identity,
	const struct x86_i8042_input_binding *binding) __must_check;
enum x86_i8042_status x86_i8042_input_unbind(
	kernel_object_handle_t context_identity,
	kernel_object_handle_t owner_identity,
	const struct x86_i8042_input_binding *binding) __must_check;
/* Query an immutable epoch, encode outside, then inject against that epoch. */
enum x86_i8042_status x86_i8042_input_keyboard_mode(
	const struct x86_i8042_input_binding *binding,
	struct x86_i8042_keyboard_mode *mode) __must_check;
enum x86_i8042_status x86_i8042_input_inject_keyboard(
	const struct x86_i8042_input_binding *binding,
	const struct x86_i8042_keyboard_mode *mode, uint8_t value) __must_check;
enum x86_i8042_status x86_i8042_input_inject_keyboard_sequence(
	const struct x86_i8042_input_binding *binding,
	const struct x86_i8042_keyboard_mode *mode, const uint8_t *values,
	size_t values_capacity, size_t count) __must_check;
/* The execution owner serializes injection with port callbacks. */
enum x86_i8042_status x86_i8042_input_inject(
	const struct x86_i8042_input_binding *binding,
	enum x86_i8042_input_kind kind, uint8_t value) __must_check;
enum x86_i8042_status x86_i8042_input_inject_sequence(
	const struct x86_i8042_input_binding *binding,
	enum x86_i8042_input_kind kind, const uint8_t *values,
	size_t values_capacity, size_t count) __must_check;
/* Route the peeked request first, then consume that exact sequence. */
enum x86_i8042_status x86_i8042_event_peek(
	kernel_object_handle_t context_identity,
	kernel_object_handle_t owner_identity,
	struct x86_i8042_event *event) __must_check;
enum x86_i8042_status x86_i8042_event_consume(
	kernel_object_handle_t context_identity,
	kernel_object_handle_t owner_identity,
	uint64_t sequence) __must_check;
enum x86_i8042_status x86_i8042_snapshot(
	kernel_object_handle_t context_identity,
	struct x86_i8042_snapshot *snapshot) __must_check;

static_assert_expression(sizeof(struct x86_i8042_config) == 16u,
			 "i8042 config layout changed");
static_assert_expression(
	__builtin_offsetof(struct x86_i8042_config, keyboard_id_length) == 10u,
	"i8042 config identity offset changed");
static_assert_expression(sizeof(struct x86_i8042_input_config) == 8u,
			 "i8042 input config layout changed");
static_assert_expression(sizeof(struct x86_i8042_input_binding) == 48u,
			 "i8042 input binding layout changed");
static_assert_expression(sizeof(struct x86_i8042_keyboard_mode) == 40u,
			 "i8042 keyboard mode layout changed");
static_assert_expression(sizeof(struct x86_i8042_event) == 24u,
			 "i8042 event layout changed");
static_assert_expression(
	__builtin_offsetof(struct x86_i8042_event, sequence) == 8u,
	"i8042 event sequence offset changed");
static_assert_expression(
	__builtin_offsetof(struct x86_i8042_event, kind) == 16u,
	"i8042 event kind offset changed");
static_assert_expression(sizeof(struct x86_i8042_snapshot) == 96u,
			 "i8042 snapshot layout changed");
static_assert_expression(
	__builtin_offsetof(struct x86_i8042_snapshot, command_byte) == 72u,
	"i8042 snapshot state offset changed");

#endif

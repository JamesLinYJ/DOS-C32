/* SPDX-License-Identifier: GPL-2.0-only */
/* Transactional ownership of the native PC/AT i8042 command byte. */
#ifndef DOSC32_X86_NATIVE_I8042_H
#define DOSC32_X86_NATIVE_I8042_H

#include "object_identity.h"

enum x86_native_i8042_status {
	X86_NATIVE_I8042_OK = 0,
	X86_NATIVE_I8042_INVALID_ARGUMENT,
	X86_NATIVE_I8042_INVALID_STATE,
	X86_NATIVE_I8042_IDENTITY_MISMATCH,
	X86_NATIVE_I8042_UNAVAILABLE,
	X86_NATIVE_I8042_TIMEOUT,
	X86_NATIVE_I8042_IO_ERROR,
	X86_NATIVE_I8042_CAPACITY_EXHAUSTED,
	X86_NATIVE_I8042_POISONED
};

enum x86_native_i8042_phase {
	X86_NATIVE_I8042_EMPTY = 0,
	X86_NATIVE_I8042_PREPARED,
	X86_NATIVE_I8042_ACTIVE,
	X86_NATIVE_I8042_QUIESCED,
	X86_NATIVE_I8042_POISONED_PHASE
};

enum x86_native_i8042_io_status {
	X86_NATIVE_I8042_IO_OK = 0,
	X86_NATIVE_I8042_IO_UNAVAILABLE,
	X86_NATIVE_I8042_IO_FAULT
};

typedef enum x86_native_i8042_io_status (*x86_native_i8042_read8_fn)(
	kernel_object_handle_t context, uint16_t port, uint8_t *value);
typedef enum x86_native_i8042_io_status (*x86_native_i8042_write8_fn)(
	kernel_object_handle_t context, uint16_t port, uint8_t value);

struct x86_native_i8042_config {
	kernel_object_handle_t controller_identity;
	kernel_object_handle_t callback_context;
	x86_native_i8042_read8_fn read8;
	x86_native_i8042_write8_fn write8;
	uint32_t poll_limit;
	uint16_t data_port;
	uint16_t status_port;
	uint16_t command_port;
	uint16_t drain_limit;
	uint8_t stability_attempts;
	uint8_t reserved[7];
};

struct x86_native_i8042_control {
	struct x86_native_i8042_config config;
	uint64_t generation;
	uint64_t status_read_count;
	uint64_t data_read_count;
	uint64_t command_write_count;
	uint64_t data_write_count;
	uint32_t lifecycle_cookie;
	uint8_t original_command_byte;
	uint8_t staged_command_byte;
	uint8_t active_command_byte;
	uint8_t phase;
	uint8_t hardware_mutated;
	uint8_t reserved[3];
} __aligned(8);

struct x86_native_i8042_snapshot {
	kernel_object_handle_t controller_identity;
	uint64_t generation;
	uint64_t status_read_count;
	uint64_t data_read_count;
	uint64_t command_write_count;
	uint64_t data_write_count;
	uint32_t poll_limit;
	uint16_t data_port;
	uint16_t status_port;
	uint16_t command_port;
	uint16_t drain_limit;
	uint8_t stability_attempts;
	uint8_t original_command_byte;
	uint8_t staged_command_byte;
	uint8_t active_command_byte;
	uint8_t translation_enabled;
	uint8_t phase;
	uint8_t hardware_mutated;
	uint8_t reserved[5];
} __aligned(8);

void x86_native_i8042_construct(
	struct x86_native_i8042_control *control);
/*
 * Prepare drains stale output, reads a stable firmware command byte, then
 * stages both interfaces disabled and both controller IRQ bits clear.
 */
enum x86_native_i8042_status x86_native_i8042_prepare(
	struct x86_native_i8042_control *control,
	const struct x86_native_i8042_config *config) __must_check;
/* Publish enables only the keyboard interface/IRQ; auxiliary stays disabled. */
enum x86_native_i8042_status x86_native_i8042_publish(
	struct x86_native_i8042_control *control,
	kernel_object_handle_t controller_identity) __must_check;
enum x86_native_i8042_status x86_native_i8042_abort(
	struct x86_native_i8042_control *control,
	kernel_object_handle_t controller_identity) __must_check;
enum x86_native_i8042_status x86_native_i8042_quiesce(
	struct x86_native_i8042_control *control,
	kernel_object_handle_t controller_identity) __must_check;
enum x86_native_i8042_status x86_native_i8042_resume(
	struct x86_native_i8042_control *control,
	kernel_object_handle_t controller_identity) __must_check;
enum x86_native_i8042_status x86_native_i8042_retire(
	struct x86_native_i8042_control *control,
	kernel_object_handle_t controller_identity) __must_check;
enum x86_native_i8042_status x86_native_i8042_snapshot(
	const struct x86_native_i8042_control *control,
	kernel_object_handle_t controller_identity,
	struct x86_native_i8042_snapshot *snapshot) __must_check;

static_assert_expression(
	__builtin_offsetof(struct x86_native_i8042_config, poll_limit) ==
		16u + 2u * sizeof(void *),
	"native i8042 config callback layout changed");

#endif

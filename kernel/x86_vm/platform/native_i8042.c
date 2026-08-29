// SPDX-License-Identifier: GPL-2.0-only
/*
 * Native i8042 command-byte transaction.
 *
 * Bounded flush, stable command-byte reads, disable-before-publication ordering
 * and exact restore are owned by this separate DOS-C32 transaction boundary.
 */
#include "x86_native_i8042.h"

#include "x86_i8042.h"

#include "../../../config/x86-native-i8042.h"

#define NATIVE_I8042_COOKIE 0x4e493832u
#define NATIVE_I8042_GENERATION_MAX ((uint64_t)-2)
#define NATIVE_I8042_READ_COMMAND_BYTE 0x20u
#define NATIVE_I8042_WRITE_COMMAND_BYTE 0x60u

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

static bool config_is_valid(const struct x86_native_i8042_config *config)
{
	return config != NULL &&
	       identity_is_valid(config->controller_identity) &&
	       identity_is_valid(config->callback_context) &&
	       config->read8 != NULL && config->write8 != NULL &&
	       config->poll_limit != 0u &&
	       config->poll_limit <= CONFIG_X86_NATIVE_I8042_POLL_LIMIT_MAX &&
	       config->drain_limit != 0u &&
	       config->drain_limit <= CONFIG_X86_NATIVE_I8042_DRAIN_LIMIT_MAX &&
	       config->stability_attempts >= 2u &&
	       config->stability_attempts <=
		       CONFIG_X86_NATIVE_I8042_STABILITY_ATTEMPTS_MAX &&
	       config->data_port != 0u && config->status_port != 0u &&
	       config->command_port == config->status_port &&
	       config->data_port != config->status_port &&
	       bytes_are_zero(config->reserved, ARRAY_SIZE(config->reserved));
}

static enum x86_native_i8042_status map_io(
	enum x86_native_i8042_io_status status)
{
	if (status == X86_NATIVE_I8042_IO_UNAVAILABLE)
		return X86_NATIVE_I8042_UNAVAILABLE;
	return status == X86_NATIVE_I8042_IO_OK ? X86_NATIVE_I8042_OK
					      : X86_NATIVE_I8042_IO_ERROR;
}

static enum x86_native_i8042_status read_status(
	struct x86_native_i8042_control *control, uint8_t *value)
{
	enum x86_native_i8042_io_status status = control->config.read8(
		control->config.callback_context, control->config.status_port,
		value);

	control->status_read_count =
		saturating_increment(control->status_read_count);
	return map_io(status);
}

static enum x86_native_i8042_status read_data(
	struct x86_native_i8042_control *control, uint8_t *value)
{
	enum x86_native_i8042_io_status status = control->config.read8(
		control->config.callback_context, control->config.data_port, value);

	control->data_read_count = saturating_increment(control->data_read_count);
	return map_io(status);
}

static enum x86_native_i8042_status write_command(
	struct x86_native_i8042_control *control, uint8_t value)
{
	enum x86_native_i8042_io_status status = control->config.write8(
		control->config.callback_context, control->config.command_port,
		value);

	control->command_write_count =
		saturating_increment(control->command_write_count);
	return map_io(status);
}

static enum x86_native_i8042_status write_data(
	struct x86_native_i8042_control *control, uint8_t value)
{
	enum x86_native_i8042_io_status status = control->config.write8(
		control->config.callback_context, control->config.data_port, value);

	control->data_write_count =
		saturating_increment(control->data_write_count);
	return map_io(status);
}

static enum x86_native_i8042_status wait_input_empty(
	struct x86_native_i8042_control *control)
{
	uint32_t attempt;

	for (attempt = 0u; attempt < control->config.poll_limit; ++attempt) {
		uint8_t status;
		enum x86_native_i8042_status result = read_status(control, &status);

		if (result != X86_NATIVE_I8042_OK)
			return result;
		if ((status & X86_I8042_STATUS_INPUT_FULL) == 0u)
			return X86_NATIVE_I8042_OK;
	}
	return X86_NATIVE_I8042_TIMEOUT;
}

static enum x86_native_i8042_status wait_controller_output(
	struct x86_native_i8042_control *control)
{
	uint32_t attempt;

	for (attempt = 0u; attempt < control->config.poll_limit; ++attempt) {
		uint8_t status;
		enum x86_native_i8042_status result = read_status(control, &status);

		if (result != X86_NATIVE_I8042_OK)
			return result;
		if ((status & X86_I8042_STATUS_OUTPUT_FULL) == 0u)
			continue;
		if ((status & (X86_I8042_STATUS_AUXILIARY |
			       X86_I8042_STATUS_TIMEOUT |
			       X86_I8042_STATUS_PARITY)) != 0u)
			return X86_NATIVE_I8042_IO_ERROR;
		return X86_NATIVE_I8042_OK;
	}
	return X86_NATIVE_I8042_TIMEOUT;
}

static enum x86_native_i8042_status drain_output(
	struct x86_native_i8042_control *control)
{
	uint16_t count;

	for (count = 0u; count < control->config.drain_limit; ++count) {
		uint8_t ignored;
		uint8_t status;
		enum x86_native_i8042_status result = read_status(control, &status);

		if (result != X86_NATIVE_I8042_OK)
			return result;
		if ((status & X86_I8042_STATUS_OUTPUT_FULL) == 0u)
			return X86_NATIVE_I8042_OK;
		result = read_data(control, &ignored);
		if (result != X86_NATIVE_I8042_OK)
			return result;
	}
	return X86_NATIVE_I8042_CAPACITY_EXHAUSTED;
}

static enum x86_native_i8042_status read_command_byte_once(
	struct x86_native_i8042_control *control, uint8_t *value)
{
	enum x86_native_i8042_status status = wait_input_empty(control);

	if (status != X86_NATIVE_I8042_OK)
		return status;
	status = write_command(control, NATIVE_I8042_READ_COMMAND_BYTE);
	if (status != X86_NATIVE_I8042_OK)
		return status;
	status = wait_controller_output(control);
	if (status != X86_NATIVE_I8042_OK)
		return status;
	return read_data(control, value);
}

static enum x86_native_i8042_status read_stable_command_byte(
	struct x86_native_i8042_control *control, uint8_t *value)
{
	uint8_t previous = 0u;
	uint8_t attempt;

	for (attempt = 0u; attempt < control->config.stability_attempts;
	     ++attempt) {
		uint8_t current;
		enum x86_native_i8042_status status =
			read_command_byte_once(control, &current);

		if (status != X86_NATIVE_I8042_OK)
			return status;
		if (attempt != 0u && current == previous) {
			*value = current;
			return X86_NATIVE_I8042_OK;
		}
		previous = current;
	}
	return X86_NATIVE_I8042_UNAVAILABLE;
}

/* A failed write after the 60h command makes hardware state uncertain. */
static enum x86_native_i8042_status write_command_byte_unverified(
	struct x86_native_i8042_control *control, uint8_t value)
{
	enum x86_native_i8042_status status = wait_input_empty(control);

	if (status != X86_NATIVE_I8042_OK)
		return status;
	status = write_command(control, NATIVE_I8042_WRITE_COMMAND_BYTE);
	if (status != X86_NATIVE_I8042_OK)
		return status;
	control->hardware_mutated = 1u;
	status = wait_input_empty(control);
	if (status != X86_NATIVE_I8042_OK)
		return status;
	return write_data(control, value);
}

static enum x86_native_i8042_status write_and_verify_command_byte(
	struct x86_native_i8042_control *control, uint8_t value)
{
	uint8_t observed;
	enum x86_native_i8042_status status =
		write_command_byte_unverified(control, value);

	if (status != X86_NATIVE_I8042_OK)
		return status;
	status = read_stable_command_byte(control, &observed);
	if (status != X86_NATIVE_I8042_OK)
		return status;
	return observed == value ? X86_NATIVE_I8042_OK
				 : X86_NATIVE_I8042_IO_ERROR;
}

static enum x86_native_i8042_status owner_status(
	const struct x86_native_i8042_control *control,
	kernel_object_handle_t identity)
{
	if (control == NULL || control->lifecycle_cookie != NATIVE_I8042_COOKIE ||
	    !identity_is_valid(identity))
		return X86_NATIVE_I8042_INVALID_ARGUMENT;
	if (control->phase == X86_NATIVE_I8042_EMPTY)
		return X86_NATIVE_I8042_INVALID_STATE;
	if (control->config.controller_identity != identity)
		return X86_NATIVE_I8042_IDENTITY_MISMATCH;
	return control->phase == X86_NATIVE_I8042_POISONED_PHASE
		       ? X86_NATIVE_I8042_POISONED
		       : X86_NATIVE_I8042_OK;
}

static void clear_lifetime(struct x86_native_i8042_control *control)
{
	uint64_t generation = control->generation;

	*control = (struct x86_native_i8042_control){
		.generation = generation,
		.lifecycle_cookie = NATIVE_I8042_COOKIE,
		.phase = X86_NATIVE_I8042_EMPTY,
	};
}

static enum x86_native_i8042_status poison(
	struct x86_native_i8042_control *control)
{
	control->phase = X86_NATIVE_I8042_POISONED_PHASE;
	return X86_NATIVE_I8042_POISONED;
}

void x86_native_i8042_construct(
	struct x86_native_i8042_control *control)
{
	if (control == NULL)
		return;
	*control = (struct x86_native_i8042_control){
		.lifecycle_cookie = NATIVE_I8042_COOKIE,
		.phase = X86_NATIVE_I8042_EMPTY,
	};
}

enum x86_native_i8042_status x86_native_i8042_prepare(
	struct x86_native_i8042_control *control,
	const struct x86_native_i8042_config *config)
{
	uint8_t original;
	uint8_t staged;
	enum x86_native_i8042_status status;

	if (control == NULL || control->lifecycle_cookie != NATIVE_I8042_COOKIE ||
	    !config_is_valid(config))
		return X86_NATIVE_I8042_INVALID_ARGUMENT;
	if (control->phase == X86_NATIVE_I8042_POISONED_PHASE)
		return X86_NATIVE_I8042_POISONED;
	if (control->phase != X86_NATIVE_I8042_EMPTY)
		return X86_NATIVE_I8042_INVALID_STATE;
	if (control->generation >= NATIVE_I8042_GENERATION_MAX)
		return X86_NATIVE_I8042_CAPACITY_EXHAUSTED;
	control->generation++;
	control->config = *config;
	status = drain_output(control);
	if (status != X86_NATIVE_I8042_OK)
		goto no_mutation_failure;
	status = read_stable_command_byte(control, &original);
	if (status != X86_NATIVE_I8042_OK)
		goto no_mutation_failure;
	staged = (uint8_t)(original |
			   X86_I8042_COMMAND_BYTE_KEYBOARD_DISABLED |
			   X86_I8042_COMMAND_BYTE_AUXILIARY_DISABLED);
	staged &= (uint8_t)~(X86_I8042_COMMAND_BYTE_IRQ1 |
			     X86_I8042_COMMAND_BYTE_IRQ12);
	control->original_command_byte = original;
	control->staged_command_byte = staged;
	control->active_command_byte =
		(uint8_t)((staged &
			   (uint8_t)~X86_I8042_COMMAND_BYTE_KEYBOARD_DISABLED) |
			  X86_I8042_COMMAND_BYTE_IRQ1);
	status = write_and_verify_command_byte(control, staged);
	if (status != X86_NATIVE_I8042_OK)
		return poison(control);
	control->phase = X86_NATIVE_I8042_PREPARED;
	return X86_NATIVE_I8042_OK;

no_mutation_failure:
	clear_lifetime(control);
	return status;
}

enum x86_native_i8042_status x86_native_i8042_publish(
	struct x86_native_i8042_control *control,
	kernel_object_handle_t controller_identity)
{
	enum x86_native_i8042_status status =
		owner_status(control, controller_identity);

	if (status != X86_NATIVE_I8042_OK)
		return status;
	if (control->phase != X86_NATIVE_I8042_PREPARED)
		return X86_NATIVE_I8042_INVALID_STATE;
	status = write_and_verify_command_byte(control,
					       control->active_command_byte);
	if (status != X86_NATIVE_I8042_OK)
		return poison(control);
	control->phase = X86_NATIVE_I8042_ACTIVE;
	return X86_NATIVE_I8042_OK;
}

static enum x86_native_i8042_status restore_firmware_command_byte(
	struct x86_native_i8042_control *control)
{
	enum x86_native_i8042_status status = write_and_verify_command_byte(
		control, control->original_command_byte);

	if (status != X86_NATIVE_I8042_OK)
		return poison(control);
	clear_lifetime(control);
	return X86_NATIVE_I8042_OK;
}

enum x86_native_i8042_status x86_native_i8042_abort(
	struct x86_native_i8042_control *control,
	kernel_object_handle_t controller_identity)
{
	enum x86_native_i8042_status status =
		owner_status(control, controller_identity);

	if (status != X86_NATIVE_I8042_OK)
		return status;
	if (control->phase != X86_NATIVE_I8042_PREPARED)
		return X86_NATIVE_I8042_INVALID_STATE;
	return restore_firmware_command_byte(control);
}

enum x86_native_i8042_status x86_native_i8042_quiesce(
	struct x86_native_i8042_control *control,
	kernel_object_handle_t controller_identity)
{
	enum x86_native_i8042_status status =
		owner_status(control, controller_identity);

	if (status != X86_NATIVE_I8042_OK)
		return status;
	if (control->phase != X86_NATIVE_I8042_ACTIVE)
		return X86_NATIVE_I8042_INVALID_STATE;
	/* Teardown owns all unread controller output. Drain it before issuing the
	 * command-byte readback sequence, otherwise a held keyboard byte can be
	 * mistaken for the controller response and make fail-closed isolation
	 * poison an otherwise known hardware state. */
	status = drain_output(control);
	if (status != X86_NATIVE_I8042_OK)
		return status;
	status = write_and_verify_command_byte(control,
					       control->staged_command_byte);
	if (status != X86_NATIVE_I8042_OK)
		return poison(control);
	control->phase = X86_NATIVE_I8042_QUIESCED;
	return X86_NATIVE_I8042_OK;
}

enum x86_native_i8042_status x86_native_i8042_resume(
	struct x86_native_i8042_control *control,
	kernel_object_handle_t controller_identity)
{
	enum x86_native_i8042_status status =
		owner_status(control, controller_identity);

	if (status != X86_NATIVE_I8042_OK)
		return status;
	if (control->phase != X86_NATIVE_I8042_QUIESCED)
		return X86_NATIVE_I8042_INVALID_STATE;
	status = write_and_verify_command_byte(control,
					       control->active_command_byte);
	if (status != X86_NATIVE_I8042_OK)
		return poison(control);
	control->phase = X86_NATIVE_I8042_ACTIVE;
	return X86_NATIVE_I8042_OK;
}

enum x86_native_i8042_status x86_native_i8042_retire(
	struct x86_native_i8042_control *control,
	kernel_object_handle_t controller_identity)
{
	enum x86_native_i8042_status status =
		owner_status(control, controller_identity);

	if (status != X86_NATIVE_I8042_OK)
		return status;
	if (control->phase != X86_NATIVE_I8042_QUIESCED)
		return X86_NATIVE_I8042_INVALID_STATE;
	return restore_firmware_command_byte(control);
}

enum x86_native_i8042_status x86_native_i8042_snapshot(
	const struct x86_native_i8042_control *control,
	kernel_object_handle_t controller_identity,
	struct x86_native_i8042_snapshot *snapshot)
{
	enum x86_native_i8042_status status;

	if (snapshot == NULL)
		return X86_NATIVE_I8042_INVALID_ARGUMENT;
	status = owner_status(control, controller_identity);
	if (status != X86_NATIVE_I8042_OK)
		return status;
	*snapshot = (struct x86_native_i8042_snapshot){
		.controller_identity = control->config.controller_identity,
		.generation = control->generation,
		.status_read_count = control->status_read_count,
		.data_read_count = control->data_read_count,
		.command_write_count = control->command_write_count,
		.data_write_count = control->data_write_count,
		.poll_limit = control->config.poll_limit,
		.data_port = control->config.data_port,
		.status_port = control->config.status_port,
		.command_port = control->config.command_port,
		.drain_limit = control->config.drain_limit,
		.stability_attempts = control->config.stability_attempts,
		.original_command_byte = control->original_command_byte,
		.staged_command_byte = control->staged_command_byte,
		.active_command_byte = control->active_command_byte,
		.translation_enabled = (uint8_t)(
			(control->active_command_byte &
			 X86_I8042_COMMAND_BYTE_TRANSLATE) != 0u),
		.phase = control->phase,
		.hardware_mutated = control->hardware_mutated,
		.reserved = {0u},
	};
	return X86_NATIVE_I8042_OK;
}

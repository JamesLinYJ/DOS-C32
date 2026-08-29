// SPDX-License-Identifier: GPL-2.0-only
/* Host-safe native i8042 command-byte transaction tests. */
#include "test_entry.h"
#include "x86_i8042.h"
#include "x86_native_i8042.h"

#define CONTROLLER_ID ((kernel_object_handle_t)0x4e49383034324354ull)
#define CALLBACK_ID ((kernel_object_handle_t)0x4e4938303432494full)

struct fake_i8042 {
	uint8_t command_byte;
	uint8_t output_byte;
	uint8_t output_full;
	uint8_t expect_command_byte;
	uint8_t force_input_full;
	uint8_t stuck_output;
	uint8_t fail_data_write;
	uint8_t unstable_reads;
	uint8_t command_read_count;
	uint8_t reserved[7];
};

static struct fake_i8042 fake;

static enum x86_native_i8042_io_status fake_read8(
	kernel_object_handle_t context, uint16_t port, uint8_t *value)
{
	if (context != CALLBACK_ID || value == NULL)
		return X86_NATIVE_I8042_IO_FAULT;
	if (port == X86_I8042_STATUS_PORT) {
		*value = (uint8_t)((fake.output_full != 0u
					    ? X86_I8042_STATUS_OUTPUT_FULL
					    : 0u) |
				   (fake.force_input_full != 0u
					    ? X86_I8042_STATUS_INPUT_FULL
					    : 0u));
		return X86_NATIVE_I8042_IO_OK;
	}
	if (port != X86_I8042_DATA_PORT || fake.output_full == 0u)
		return X86_NATIVE_I8042_IO_FAULT;
	*value = fake.output_byte;
	if (fake.stuck_output == 0u)
		fake.output_full = 0u;
	return X86_NATIVE_I8042_IO_OK;
}

static enum x86_native_i8042_io_status fake_write8(
	kernel_object_handle_t context, uint16_t port, uint8_t value)
{
	if (context != CALLBACK_ID)
		return X86_NATIVE_I8042_IO_FAULT;
	if (port == X86_I8042_COMMAND_PORT) {
		if (value == 0x20u) {
			uint8_t response = fake.command_byte;

			fake.command_read_count++;
			if (fake.unstable_reads != 0u) {
				response ^= 0x08u;
				fake.unstable_reads--;
			}
			fake.output_byte = response;
			fake.output_full = 1u;
			return X86_NATIVE_I8042_IO_OK;
		}
		if (value == 0x60u) {
			fake.expect_command_byte = 1u;
			return X86_NATIVE_I8042_IO_OK;
		}
		return X86_NATIVE_I8042_IO_FAULT;
	}
	if (port != X86_I8042_DATA_PORT ||
	    fake.expect_command_byte == 0u)
		return X86_NATIVE_I8042_IO_FAULT;
	if (fake.fail_data_write != 0u) {
		fake.fail_data_write = 0u;
		return X86_NATIVE_I8042_IO_FAULT;
	}
	fake.expect_command_byte = 0u;
	fake.command_byte = value;
	return X86_NATIVE_I8042_IO_OK;
}

static struct x86_native_i8042_config test_config(void)
{
	return (struct x86_native_i8042_config){
		.controller_identity = CONTROLLER_ID,
		.callback_context = CALLBACK_ID,
		.read8 = fake_read8,
		.write8 = fake_write8,
		.poll_limit = 4u,
		.data_port = X86_I8042_DATA_PORT,
		.status_port = X86_I8042_STATUS_PORT,
		.command_port = X86_I8042_COMMAND_PORT,
		.drain_limit = 4u,
		.stability_attempts = 4u,
		.reserved = {0u},
	};
}

static int test_lifecycle(void)
{
	struct x86_native_i8042_control control;
	struct x86_native_i8042_snapshot snapshot;
	struct x86_native_i8042_config config = test_config();
	const uint8_t original =
		X86_I8042_COMMAND_BYTE_IRQ1 | X86_I8042_COMMAND_BYTE_IRQ12 |
		X86_I8042_COMMAND_BYTE_SYSTEM |
		X86_I8042_COMMAND_BYTE_TRANSLATE;
	const uint8_t staged =
		X86_I8042_COMMAND_BYTE_SYSTEM |
		X86_I8042_COMMAND_BYTE_KEYBOARD_DISABLED |
		X86_I8042_COMMAND_BYTE_AUXILIARY_DISABLED |
		X86_I8042_COMMAND_BYTE_TRANSLATE;
	const uint8_t active =
		X86_I8042_COMMAND_BYTE_IRQ1 | X86_I8042_COMMAND_BYTE_SYSTEM |
		X86_I8042_COMMAND_BYTE_AUXILIARY_DISABLED |
		X86_I8042_COMMAND_BYTE_TRANSLATE;

	fake = (struct fake_i8042){
		.command_byte = original,
		.output_byte = 0xa5u,
		.output_full = 1u,
		.unstable_reads = 1u,
	};
	x86_native_i8042_construct(&control);
	if (x86_native_i8042_prepare(&control, &config) !=
		    X86_NATIVE_I8042_OK ||
	    fake.command_byte != staged ||
	    x86_native_i8042_snapshot(&control, CONTROLLER_ID, &snapshot) !=
		    X86_NATIVE_I8042_OK ||
	    snapshot.original_command_byte != original ||
	    snapshot.staged_command_byte != staged ||
	    snapshot.active_command_byte != active ||
	    snapshot.translation_enabled != 1u ||
	    snapshot.phase != X86_NATIVE_I8042_PREPARED ||
	    snapshot.hardware_mutated != 1u || fake.command_read_count < 5u)
		return 1;
	if (x86_native_i8042_publish(&control, CONTROLLER_ID + 1u) !=
		    X86_NATIVE_I8042_IDENTITY_MISMATCH ||
	    fake.command_byte != staged)
		return 2;
	if (x86_native_i8042_publish(&control, CONTROLLER_ID) !=
		    X86_NATIVE_I8042_OK ||
	    fake.command_byte != active)
		return 3;
	/* A scan byte held at isolation time must be drained before command-byte
	 * readback or it could be mistaken for the controller response. */
	fake.output_byte = 0x1cu;
	fake.output_full = 1u;
	if (x86_native_i8042_quiesce(&control, CONTROLLER_ID) !=
		    X86_NATIVE_I8042_OK ||
	    fake.output_full != 0u || fake.command_byte != staged ||
	    x86_native_i8042_resume(&control, CONTROLLER_ID) !=
		    X86_NATIVE_I8042_OK ||
	    fake.command_byte != active ||
	    x86_native_i8042_quiesce(&control, CONTROLLER_ID) !=
		    X86_NATIVE_I8042_OK ||
	    x86_native_i8042_retire(&control, CONTROLLER_ID) !=
		    X86_NATIVE_I8042_OK ||
	    fake.command_byte != original ||
	    control.phase != X86_NATIVE_I8042_EMPTY)
		return 4;
	return 0;
}

static int test_abort_and_bounded_failures(void)
{
	struct x86_native_i8042_control control;
	struct x86_native_i8042_config config = test_config();
	const uint8_t original = X86_I8042_COMMAND_BYTE_SYSTEM;

	fake = (struct fake_i8042){.command_byte = original};
	x86_native_i8042_construct(&control);
	if (x86_native_i8042_prepare(&control, &config) !=
		    X86_NATIVE_I8042_OK ||
	    x86_native_i8042_abort(&control, CONTROLLER_ID) !=
		    X86_NATIVE_I8042_OK ||
	    fake.command_byte != original ||
	    control.phase != X86_NATIVE_I8042_EMPTY)
		return 1;

	fake = (struct fake_i8042){
		.command_byte = original,
		.force_input_full = 1u,
	};
	x86_native_i8042_construct(&control);
	if (x86_native_i8042_prepare(&control, &config) !=
		    X86_NATIVE_I8042_TIMEOUT ||
	    fake.command_byte != original ||
	    control.phase != X86_NATIVE_I8042_EMPTY)
		return 2;

	config.drain_limit = 2u;
	fake = (struct fake_i8042){
		.command_byte = original,
		.output_byte = 0xffu,
		.output_full = 1u,
		.stuck_output = 1u,
	};
	x86_native_i8042_construct(&control);
	if (x86_native_i8042_prepare(&control, &config) !=
		    X86_NATIVE_I8042_CAPACITY_EXHAUSTED ||
	    fake.command_byte != original ||
	    control.phase != X86_NATIVE_I8042_EMPTY)
		return 3;

	config = test_config();
	fake = (struct fake_i8042){
		.command_byte = original,
		.fail_data_write = 1u,
	};
	x86_native_i8042_construct(&control);
	if (x86_native_i8042_prepare(&control, &config) !=
		    X86_NATIVE_I8042_POISONED ||
	    control.phase != X86_NATIVE_I8042_POISONED_PHASE ||
	    control.hardware_mutated != 1u)
		return 4;
	return 0;
}

static int run_native_i8042_test(void)
{
	int status = test_lifecycle();

	if (status != 0)
		return status;
	status = test_abort_and_bounded_failures();
	return status == 0 ? 0 : 10 + status;
}

DOSC32_TEST_ENTRY(run_native_i8042_test)

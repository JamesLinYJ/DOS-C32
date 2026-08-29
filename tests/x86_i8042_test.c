// SPDX-License-Identifier: GPL-2.0-only
/* Host-safe lifecycle, port-sequence and fail-safe FIFO tests for i8042. */
#include "test_entry.h"
#include "x86_i8042.h"

#define REGISTRY_IDENTITY ((kernel_object_handle_t)0x4938303432524547ull)
#define CONTEXT_IDENTITY ((kernel_object_handle_t)0x4938303432435458ull)
#define OWNER_IDENTITY ((kernel_object_handle_t)0x49383034324f574eull)
#define SOURCE_IDENTITY ((kernel_object_handle_t)0x4938303432494e50ull)
#define OTHER_IDENTITY ((kernel_object_handle_t)0x49383034324f5448ull)

static struct x86_i8042_input_binding input_binding;

static enum x86_i8042_status inject_keyboard_sequence(
	const struct x86_i8042_input_binding *binding, const uint8_t *values,
	size_t values_capacity, size_t count)
{
	struct x86_i8042_keyboard_mode mode;
	enum x86_i8042_status status =
		x86_i8042_input_keyboard_mode(binding, &mode);

	if (status != X86_I8042_OK)
		return status;
	return x86_i8042_input_inject_keyboard_sequence(
		binding, &mode, values, values_capacity, count);
}

static enum x86_i8042_status inject_keyboard(
	const struct x86_i8042_input_binding *binding, uint8_t value)
{
	return inject_keyboard_sequence(binding, &value, sizeof(value), 1u);
}

static bool write8(uint16_t port, uint8_t value)
{
	return x86_io_resource_write(KERNEL_OBJECT_HANDLE_INVALID, port,
				     DOS_IO_WIDTH_8, value) ==
	       X86_IO_RESOURCE_OK;
}

static bool read8(uint16_t port, uint8_t expected)
{
	uint32_t value = 0x12345678u;

	return x86_io_resource_read(KERNEL_OBJECT_HANDLE_INVALID, port,
				    DOS_IO_WIDTH_8, &value) ==
		       X86_IO_RESOURCE_OK &&
	       value == expected;
}

static bool consume_event(uint8_t kind, uint8_t irq, uint8_t a20)
{
	struct x86_i8042_event first;
	struct x86_i8042_event second;

	return x86_i8042_event_peek(CONTEXT_IDENTITY, OWNER_IDENTITY,
				    &first) == X86_I8042_OK &&
	       x86_i8042_event_peek(CONTEXT_IDENTITY, OWNER_IDENTITY,
				    &second) == X86_I8042_OK &&
	       first.sequence == second.sequence && first.sequence != 0u &&
	       first.controller_generation == 2u && first.kind == kind &&
	       first.irq == irq && first.a20_enabled == a20 &&
	       x86_i8042_event_consume(CONTEXT_IDENTITY, OWNER_IDENTITY,
				       first.sequence + 1u) ==
		       X86_I8042_STALE_EVENT &&
	       x86_i8042_event_consume(CONTEXT_IDENTITY, OWNER_IDENTITY,
				       first.sequence) == X86_I8042_OK;
}

static int test_lifecycle(void)
{
	const struct x86_i8042_config config = {
		.command_byte = X86_I8042_COMMAND_BYTE_SYSTEM |
				X86_I8042_COMMAND_BYTE_IRQ1 |
				X86_I8042_COMMAND_BYTE_IRQ12,
		.input_port = 0x5au,
		.output_port = X86_I8042_OUTPUT_PORT_RESET_HIGH |
			       X86_I8042_OUTPUT_PORT_A20,
		.keyboard_present = 1u,
		.auxiliary_present = 1u,
		.keyboard_scanning_enabled = 1u,
		.keyboard_scan_set = 1u,
		.keyboard_unlocked = 1u,
		.keyboard_leds = 0u,
		.keyboard_typematic = 0u,
		.keyboard_id_length = 2u,
		.keyboard_id_first = 0xabu,
		.keyboard_id_second = 0x83u,
		.auxiliary_id = 0x00u,
		.reserved = {0u},
	};
	const struct x86_i8042_input_config input = {
		.capabilities = X86_I8042_INPUT_CAPABILITIES,
		.reserved = {0u},
	};
	struct x86_io_resource_descriptor
		descriptors[X86_I8042_RESOURCE_COUNT];
	x86_io_resource_handle_t resources[X86_I8042_RESOURCE_COUNT];
	uint32_t value = 0xabcdef01u;

	if (x86_i8042_prepare(CONTEXT_IDENTITY, OWNER_IDENTITY, &config,
				descriptors, 1u) !=
		    X86_I8042_CAPACITY_EXHAUSTED)
		return 1;
	if (x86_i8042_prepare(CONTEXT_IDENTITY, OWNER_IDENTITY, &config,
				descriptors, ARRAY_SIZE(descriptors)) !=
		    X86_I8042_OK ||
	    descriptors[0].read(CONTEXT_IDENTITY, X86_I8042_DATA_PORT,
				DOS_IO_WIDTH_8, &value) != X86_IO_CALLBACK_FAULT ||
	    x86_i8042_abort(CONTEXT_IDENTITY) != X86_I8042_OK)
		return 2;
	if (x86_i8042_prepare(CONTEXT_IDENTITY, OWNER_IDENTITY, &config,
				descriptors, ARRAY_SIZE(descriptors)) !=
		    X86_I8042_OK ||
	    x86_io_resource_registry_initialize(REGISTRY_IDENTITY) !=
		    X86_IO_RESOURCE_OK ||
	    x86_io_resource_register_batch(descriptors,
					   ARRAY_SIZE(descriptors), resources,
					   ARRAY_SIZE(resources)) !=
		    X86_IO_RESOURCE_OK ||
	    x86_i8042_publish(CONTEXT_IDENTITY) != X86_I8042_OK)
		return 3;
	if (x86_i8042_input_bind(CONTEXT_IDENTITY, OWNER_IDENTITY,
				   OWNER_IDENTITY, &input, &input_binding) !=
		    X86_I8042_INVALID_ARGUMENT ||
	    x86_i8042_input_bind(CONTEXT_IDENTITY, OWNER_IDENTITY,
				   SOURCE_IDENTITY, &input, &input_binding) !=
		    X86_I8042_OK ||
	    x86_i8042_input_bind(CONTEXT_IDENTITY, OWNER_IDENTITY,
				   OTHER_IDENTITY, &input, &input_binding) !=
		    X86_I8042_INVALID_STATE)
		return 4;
	if (x86_io_resource_read(KERNEL_OBJECT_HANDLE_INVALID,
				 X86_I8042_DATA_PORT, DOS_IO_WIDTH_16,
				 &value) != X86_IO_RESOURCE_ACCESS_DENIED ||
	    value != 0xabcdef01u ||
	    !read8(X86_I8042_STATUS_PORT,
		   X86_I8042_STATUS_SYSTEM |
			   X86_I8042_STATUS_KEY_UNLOCKED))
		return 5;
	return 0;
}

static int test_input_and_irq(void)
{
	struct x86_i8042_input_binding other_binding = input_binding;
	struct x86_i8042_event event;
	struct x86_i8042_snapshot snapshot;

	other_binding.source_identity = OTHER_IDENTITY;
	if (inject_keyboard(&other_binding, 0x1eu) !=
		    X86_I8042_IDENTITY_MISMATCH ||
	    inject_keyboard(&input_binding, 0x1eu) != X86_I8042_OK ||
	    !read8(X86_I8042_STATUS_PORT,
		   X86_I8042_STATUS_OUTPUT_FULL |
			   X86_I8042_STATUS_SYSTEM |
			   X86_I8042_STATUS_KEY_UNLOCKED) ||
	    !consume_event(X86_I8042_EVENT_IRQ_REQUEST, 1u, 0u) ||
	    !read8(X86_I8042_DATA_PORT, 0x1eu) ||
	    x86_i8042_event_peek(CONTEXT_IDENTITY, OWNER_IDENTITY, &event) !=
		    X86_I8042_NO_EVENT)
		return 1;
	if (x86_i8042_input_inject(&input_binding,
				    X86_I8042_INPUT_KIND_AUXILIARY_BYTE,
				    0x11u) != X86_I8042_OK ||
	    !read8(X86_I8042_STATUS_PORT,
		   X86_I8042_STATUS_OUTPUT_FULL |
			   X86_I8042_STATUS_SYSTEM |
			   X86_I8042_STATUS_KEY_UNLOCKED |
			   X86_I8042_STATUS_AUXILIARY) ||
	    !consume_event(X86_I8042_EVENT_IRQ_REQUEST, 12u, 0u) ||
	    !read8(X86_I8042_DATA_PORT, 0x11u))
		return 2;
	/* Disable the keyboard interface through the command byte. */
	if (!write8(X86_I8042_COMMAND_PORT, 0x60u) ||
	    !write8(X86_I8042_DATA_PORT,
		    X86_I8042_COMMAND_BYTE_SYSTEM |
			    X86_I8042_COMMAND_BYTE_IRQ1 |
			    X86_I8042_COMMAND_BYTE_IRQ12 |
			    X86_I8042_COMMAND_BYTE_KEYBOARD_DISABLED) ||
	    inject_keyboard(&input_binding, 0x30u) !=
		    X86_I8042_INPUT_DISABLED ||
	    !write8(X86_I8042_COMMAND_PORT, 0xd2u) ||
	    !write8(X86_I8042_DATA_PORT, 0x30u) ||
	    !write8(X86_I8042_COMMAND_PORT, 0xaeu) ||
	    !consume_event(X86_I8042_EVENT_IRQ_REQUEST, 1u, 0u) ||
	    !read8(X86_I8042_DATA_PORT, 0x30u) ||
	    x86_i8042_snapshot(CONTEXT_IDENTITY, &snapshot) != X86_I8042_OK ||
	    (snapshot.command_byte &
	     X86_I8042_COMMAND_BYTE_KEYBOARD_DISABLED) != 0u)
		return 3;
	return 0;
}

static int test_controller_and_keyboard_commands(void)
{
	struct x86_i8042_snapshot snapshot;

	if (!write8(X86_I8042_COMMAND_PORT, 0x20u) ||
	    !read8(X86_I8042_DATA_PORT,
		   X86_I8042_COMMAND_BYTE_SYSTEM |
			   X86_I8042_COMMAND_BYTE_IRQ1 |
			   X86_I8042_COMMAND_BYTE_IRQ12) ||
	    !write8(X86_I8042_COMMAND_PORT, 0xaau) ||
	    !read8(X86_I8042_DATA_PORT, 0x55u) ||
	    !write8(X86_I8042_COMMAND_PORT, 0xabu) ||
	    !read8(X86_I8042_DATA_PORT, 0x00u) ||
	    !write8(X86_I8042_COMMAND_PORT, 0xa9u) ||
	    !read8(X86_I8042_DATA_PORT, 0x00u))
		return 1;
	/* Keyboard identify returns ACK, ABh, 83h as three distinct bytes. */
	if (!write8(X86_I8042_DATA_PORT, 0xf2u) ||
	    !consume_event(X86_I8042_EVENT_IRQ_REQUEST, 1u, 0u) ||
	    !read8(X86_I8042_DATA_PORT, 0xfau) ||
	    !consume_event(X86_I8042_EVENT_IRQ_REQUEST, 1u, 0u) ||
	    !read8(X86_I8042_DATA_PORT, 0xabu) ||
	    !consume_event(X86_I8042_EVENT_IRQ_REQUEST, 1u, 0u) ||
	    !read8(X86_I8042_DATA_PORT, 0x83u))
		return 2;
	if (!write8(X86_I8042_DATA_PORT, 0xedu) ||
	    !consume_event(X86_I8042_EVENT_IRQ_REQUEST, 1u, 0u) ||
	    !read8(X86_I8042_DATA_PORT, 0xfau) ||
	    !write8(X86_I8042_DATA_PORT, 0x07u) ||
	    !consume_event(X86_I8042_EVENT_IRQ_REQUEST, 1u, 0u) ||
	    !read8(X86_I8042_DATA_PORT, 0xfau) ||
	    x86_i8042_snapshot(CONTEXT_IDENTITY, &snapshot) != X86_I8042_OK ||
	    snapshot.keyboard_leds != 0x07u)
		return 3;
	return 0;
}

static int test_a20_and_reset_containment(void)
{
	struct x86_i8042_snapshot snapshot;

	if (!write8(X86_I8042_COMMAND_PORT, 0xddu) ||
	    !consume_event(X86_I8042_EVENT_A20_CHANGE, 0u, 0u) ||
	    !write8(X86_I8042_COMMAND_PORT, 0xd0u) ||
	    !read8(X86_I8042_DATA_PORT, X86_I8042_OUTPUT_PORT_RESET_HIGH) ||
	    !write8(X86_I8042_COMMAND_PORT, 0xdfu) ||
	    !consume_event(X86_I8042_EVENT_A20_CHANGE, 0u, 1u))
		return 1;
	/* D1 with reset low is contained and observable only in the snapshot. */
	if (!write8(X86_I8042_COMMAND_PORT, 0xd1u) ||
	    !write8(X86_I8042_DATA_PORT, 0x00u) ||
	    !consume_event(X86_I8042_EVENT_A20_CHANGE, 0u, 0u) ||
	    !write8(X86_I8042_COMMAND_PORT, 0xfeu) ||
	    x86_i8042_snapshot(CONTEXT_IDENTITY, &snapshot) != X86_I8042_OK ||
	    snapshot.output_port != X86_I8042_OUTPUT_PORT_RESET_HIGH ||
	    snapshot.a20_enabled != 0u ||
	    snapshot.suppressed_reset_requests != 2u)
		return 2;
	return 0;
}

static int test_bounded_fifos(void)
{
	struct x86_i8042_event event;
	struct x86_i8042_snapshot snapshot;
	uint8_t index;

	/* Disable IRQ production so output capacity is tested independently. */
	if (!write8(X86_I8042_COMMAND_PORT, 0x60u) ||
	    !write8(X86_I8042_DATA_PORT, X86_I8042_COMMAND_BYTE_SYSTEM))
		return 1;
	for (index = 0u; index < 16u; ++index) {
		if (inject_keyboard(&input_binding, index) !=
			    X86_I8042_OK)
			return 2;
	}
	if (inject_keyboard(&input_binding, 0xffu) !=
		    X86_I8042_CAPACITY_EXHAUSTED ||
	    x86_i8042_snapshot(CONTEXT_IDENTITY, &snapshot) != X86_I8042_OK ||
	    snapshot.output_count != 16u || snapshot.output_overflow_count != 1u)
		return 3;
	for (index = 0u; index < 16u; ++index) {
		if (!read8(X86_I8042_DATA_PORT, index))
			return 4;
	}
	/* Sixteen A20 transitions fill the event queue without native effects. */
	for (index = 0u; index < 16u; ++index) {
		if (!write8(X86_I8042_COMMAND_PORT,
			    (index & 1u) == 0u ? 0xdfu : 0xddu))
			return 5;
	}
	if (x86_io_resource_write(KERNEL_OBJECT_HANDLE_INVALID,
				  X86_I8042_COMMAND_PORT, DOS_IO_WIDTH_8,
				  0xdfu) != X86_IO_RESOURCE_CALLBACK_FAULT ||
	    x86_i8042_snapshot(CONTEXT_IDENTITY, &snapshot) != X86_I8042_OK ||
	    snapshot.event_count != 16u || snapshot.event_overflow_count != 1u ||
	    snapshot.a20_enabled != 0u)
		return 6;
	for (index = 0u; index < 16u; ++index) {
		if (x86_i8042_event_peek(CONTEXT_IDENTITY, OWNER_IDENTITY,
					    &event) != X86_I8042_OK ||
		    x86_i8042_event_consume(CONTEXT_IDENTITY, OWNER_IDENTITY,
					       event.sequence) != X86_I8042_OK)
			return 7;
	}
	return x86_i8042_event_peek(CONTEXT_IDENTITY, OWNER_IDENTITY,
				    &event) == X86_I8042_NO_EVENT
		       ? 0
		       : 8;
}

static int test_input_lifecycle(void)
{
	const struct x86_i8042_input_config config = {
		.capabilities = X86_I8042_INPUT_CAPABILITIES,
		.reserved = {0u},
	};
	const uint8_t filler[] = {
		0x01u, 0x02u, 0x03u, 0x04u, 0x05u,
		0x06u, 0x07u, 0x08u, 0x09u, 0x0au,
		0x0bu, 0x0cu, 0x0du, 0x0eu, 0x0fu,
	};
	const uint8_t sequence[] = {0x1eu, 0x9eu, 0x30u};
	struct x86_i8042_input_binding old_binding = input_binding;
	struct x86_i8042_input_binding replacement;
	struct x86_i8042_keyboard_mode old_mode;
	struct x86_i8042_keyboard_mode new_mode;
	struct x86_i8042_snapshot snapshot;
	size_t index;

	if (x86_i8042_input_quiesce(CONTEXT_IDENTITY, OWNER_IDENTITY,
				       &input_binding) != X86_I8042_OK ||
	    x86_i8042_snapshot(CONTEXT_IDENTITY, &snapshot) != X86_I8042_OK ||
	    snapshot.input_source_bound != 1u ||
	    snapshot.input_source_quiesced != 1u ||
	    inject_keyboard(&input_binding, 0x1eu) != X86_I8042_INVALID_STATE ||
	    x86_i8042_input_resume(CONTEXT_IDENTITY, OWNER_IDENTITY,
				      &input_binding) != X86_I8042_OK)
		return 1;
	if (x86_i8042_input_keyboard_mode(&input_binding, &old_mode) !=
		    X86_I8042_OK ||
	    old_mode.scan_set != 1u || old_mode.translation_enabled != 0u ||
	    x86_i8042_input_inject(
		    &input_binding, X86_I8042_INPUT_KIND_KEYBOARD_SCAN,
		    sequence[0]) != X86_I8042_INVALID_ARGUMENT ||
	    !write8(X86_I8042_DATA_PORT, 0xf0u) ||
	    !read8(X86_I8042_DATA_PORT, 0xfau) ||
	    !write8(X86_I8042_DATA_PORT, 0x02u) ||
	    !read8(X86_I8042_DATA_PORT, 0xfau) ||
	    x86_i8042_input_inject_keyboard_sequence(
		    &input_binding, &old_mode, sequence, ARRAY_SIZE(sequence),
		    ARRAY_SIZE(sequence)) != X86_I8042_MODE_CHANGED ||
	    x86_i8042_snapshot(CONTEXT_IDENTITY, &snapshot) != X86_I8042_OK ||
	    snapshot.output_count != 0u ||
	    x86_i8042_input_keyboard_mode(&input_binding, &new_mode) !=
		    X86_I8042_OK ||
	    new_mode.scan_set != 2u ||
	    new_mode.mode_generation <= old_mode.mode_generation ||
	    x86_i8042_input_inject_keyboard_sequence(
		    &input_binding, &new_mode, sequence, 2u,
		    ARRAY_SIZE(sequence)) != X86_I8042_INVALID_ARGUMENT ||
	    x86_i8042_input_inject_keyboard_sequence(
		    &input_binding, &new_mode, filler, ARRAY_SIZE(filler),
		    ARRAY_SIZE(filler)) != X86_I8042_OK ||
	    x86_i8042_input_inject_keyboard_sequence(
		    &input_binding, &new_mode, sequence, ARRAY_SIZE(sequence),
		    ARRAY_SIZE(sequence)) != X86_I8042_CAPACITY_EXHAUSTED ||
	    x86_i8042_snapshot(CONTEXT_IDENTITY, &snapshot) != X86_I8042_OK ||
	    snapshot.output_count != ARRAY_SIZE(filler))
		return 2;
	for (index = 0u; index < ARRAY_SIZE(filler); ++index) {
		if (!read8(X86_I8042_DATA_PORT, filler[index]))
			return 3;
	}
	if (x86_i8042_input_inject_keyboard_sequence(
		    &input_binding, &new_mode, sequence, ARRAY_SIZE(sequence),
		    ARRAY_SIZE(sequence)) != X86_I8042_OK ||
	    !read8(X86_I8042_DATA_PORT, sequence[0]) ||
	    !read8(X86_I8042_DATA_PORT, sequence[1]) ||
	    !read8(X86_I8042_DATA_PORT, sequence[2]))
		return 4;
	if (x86_i8042_input_quiesce(CONTEXT_IDENTITY, OWNER_IDENTITY,
				       &input_binding) != X86_I8042_OK ||
	    x86_i8042_input_unbind(CONTEXT_IDENTITY, OWNER_IDENTITY,
				      &input_binding) != X86_I8042_OK ||
	    inject_keyboard(&old_binding, 0x1eu) != X86_I8042_STALE_BINDING ||
	    x86_i8042_input_bind(CONTEXT_IDENTITY, OWNER_IDENTITY,
				   SOURCE_IDENTITY, &config, &replacement) !=
		    X86_I8042_OK ||
	    replacement.source_generation <= old_binding.source_generation ||
	    inject_keyboard(&old_binding, 0x1eu) != X86_I8042_STALE_BINDING ||
	    inject_keyboard(&replacement, 0x1eu) != X86_I8042_OK ||
	    !read8(X86_I8042_DATA_PORT, 0x1eu))
		return 5;
	input_binding = replacement;
	return 0;
}

static int run_tests(void)
{
	int status;

	status = test_lifecycle();
	if (status != 0)
		return 10 + status;
	status = test_input_and_irq();
	if (status != 0)
		return 20 + status;
	status = test_controller_and_keyboard_commands();
	if (status != 0)
		return 30 + status;
	status = test_a20_and_reset_containment();
	if (status != 0)
		return 40 + status;
	status = test_bounded_fifos();
	if (status != 0)
		return 50 + status;
	status = test_input_lifecycle();
	if (status != 0)
		return 60 + status;
	return 0;
}

DOSC32_TEST_ENTRY(run_tests)

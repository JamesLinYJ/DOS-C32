// SPDX-License-Identifier: GPL-2.0-only
/*
 * Isolated guest i8042 keyboard-controller model.
 *
 * Hardware contract: IBM AT-compatible ports 60h/64h and PS/2 commands.
 * Safety changes: one generation-bound owner, bounded output/event FIFOs,
 * typed IRQ/A20 requests, and no path from guest reset writes to native I/O.
 */
#include "x86_i8042.h"

#include "../../../config/x86-i8042.h"

#define X86_I8042_GENERATION_MAX ((uint64_t)-2)
#define X86_I8042_INPUT_GENERATION_MAX ((uint64_t)-2)
#define X86_I8042_MODE_GENERATION_MAX ((uint64_t)-2)
#define X86_I8042_EVENT_SEQUENCE_MAX ((uint64_t)-2)

static_assert_expression(CONFIG_X86_I8042_OUTPUT_FIFO_CAPACITY > 0u &&
			 CONFIG_X86_I8042_OUTPUT_FIFO_CAPACITY <= 255u,
			 "i8042 output FIFO must fit its fixed-width count");
static_assert_expression(CONFIG_X86_I8042_EVENT_FIFO_CAPACITY > 0u &&
			 CONFIG_X86_I8042_EVENT_FIFO_CAPACITY <= 255u,
			 "i8042 event FIFO must fit its fixed-width count");

#define I8042_COMMAND_READ_COMMAND_BYTE 0x20u
#define I8042_COMMAND_WRITE_COMMAND_BYTE 0x60u
#define I8042_COMMAND_DISABLE_AUXILIARY 0xa7u
#define I8042_COMMAND_ENABLE_AUXILIARY 0xa8u
#define I8042_COMMAND_TEST_AUXILIARY 0xa9u
#define I8042_COMMAND_SELF_TEST 0xaau
#define I8042_COMMAND_TEST_KEYBOARD 0xabu
#define I8042_COMMAND_DISABLE_KEYBOARD 0xadu
#define I8042_COMMAND_ENABLE_KEYBOARD 0xaeu
#define I8042_COMMAND_READ_INPUT_PORT 0xc0u
#define I8042_COMMAND_READ_OUTPUT_PORT 0xd0u
#define I8042_COMMAND_WRITE_OUTPUT_PORT 0xd1u
#define I8042_COMMAND_WRITE_KEYBOARD_OUTPUT 0xd2u
#define I8042_COMMAND_WRITE_AUXILIARY_OUTPUT 0xd3u
#define I8042_COMMAND_WRITE_AUXILIARY 0xd4u
#define I8042_COMMAND_DISABLE_A20 0xddu
#define I8042_COMMAND_ENABLE_A20 0xdfu
#define I8042_COMMAND_PULSE_RESET 0xfeu

#define I8042_RESPONSE_CONTROLLER_OK 0x55u
#define I8042_RESPONSE_PORT_OK 0x00u
#define I8042_RESPONSE_ACK 0xfau
#define I8042_RESPONSE_RESEND 0xfeu
#define I8042_RESPONSE_BAT_OK 0xaau

#define I8042_KEYBOARD_SET_LEDS 0xedu
#define I8042_KEYBOARD_ECHO 0xeeu
#define I8042_KEYBOARD_SET_SCAN_SET 0xf0u
#define I8042_KEYBOARD_IDENTIFY 0xf2u
#define I8042_KEYBOARD_SET_TYPEMATIC 0xf3u
#define I8042_KEYBOARD_ENABLE_SCANNING 0xf4u
#define I8042_KEYBOARD_DISABLE_SCANNING 0xf5u
#define I8042_KEYBOARD_DEFAULTS 0xf6u
#define I8042_KEYBOARD_RESEND 0xfeu
#define I8042_KEYBOARD_RESET 0xffu

#define I8042_PENDING_IRQ1 0x01u
#define I8042_PENDING_IRQ12 0x02u

enum i8042_phase {
	I8042_PHASE_EMPTY = 0,
	I8042_PHASE_PREPARED,
	I8042_PHASE_ACTIVE,
	I8042_PHASE_POISONED
};

enum i8042_input_phase {
	I8042_INPUT_EMPTY = 0,
	I8042_INPUT_ACTIVE,
	I8042_INPUT_QUIESCED
};

enum i8042_output_source {
	I8042_OUTPUT_CONTROLLER = 0,
	I8042_OUTPUT_KEYBOARD,
	I8042_OUTPUT_AUXILIARY
};

enum i8042_pending_command {
	I8042_PENDING_NONE = 0,
	I8042_PENDING_COMMAND_BYTE,
	I8042_PENDING_OUTPUT_PORT,
	I8042_PENDING_KEYBOARD_OUTPUT,
	I8042_PENDING_AUXILIARY_OUTPUT,
	I8042_PENDING_AUXILIARY_COMMAND,
	I8042_PENDING_KEYBOARD_LEDS,
	I8042_PENDING_KEYBOARD_SCAN_SET,
	I8042_PENDING_KEYBOARD_TYPEMATIC,
	I8042_PENDING_AUXILIARY_PARAMETER
};

struct i8042_output_entry {
	uint8_t value;
	uint8_t source;
};

struct i8042_owner {
	kernel_object_handle_t context_identity;
	kernel_object_handle_t owner_identity;
	kernel_object_handle_t input_source_identity;
	uint64_t generation;
	uint64_t input_source_generation;
	uint64_t keyboard_mode_generation;
	uint64_t next_event_sequence;
	uint64_t output_overflow_count;
	uint64_t event_overflow_count;
	uint64_t suppressed_reset_requests;
	uint64_t unsupported_command_count;
	uint32_t input_capabilities;
	struct i8042_output_entry
		output_fifo[CONFIG_X86_I8042_OUTPUT_FIFO_CAPACITY];
	struct x86_i8042_event
		event_fifo[CONFIG_X86_I8042_EVENT_FIFO_CAPACITY];
	uint8_t output_head;
	uint8_t output_count;
	uint8_t event_head;
	uint8_t event_count;
	uint8_t command_byte;
	uint8_t input_port;
	uint8_t output_port;
	uint8_t pending_command;
	uint8_t pending_irq_mask;
	uint8_t keyboard_present;
	uint8_t auxiliary_present;
	uint8_t keyboard_scanning_enabled;
	uint8_t keyboard_scan_set;
	uint8_t keyboard_leds;
	uint8_t keyboard_typematic;
	uint8_t keyboard_unlocked;
	uint8_t keyboard_id_length;
	uint8_t keyboard_id_first;
	uint8_t keyboard_id_second;
	uint8_t auxiliary_id;
	uint8_t last_write_command;
	uint8_t last_keyboard_reply;
	uint8_t phase;
	uint8_t input_source_phase;
} __aligned(8);

static struct i8042_owner runtime_owner;

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

static bool config_is_valid(const struct x86_i8042_config *config)
{
	if (config == NULL ||
	    !bytes_are_zero(config->reserved, ARRAY_SIZE(config->reserved)) ||
	    config->keyboard_present > 1u || config->auxiliary_present > 1u ||
	    config->keyboard_scanning_enabled > 1u ||
	    config->keyboard_unlocked > 1u || config->keyboard_scan_set == 0u ||
	    config->keyboard_scan_set > 3u ||
	    config->keyboard_id_length > 2u ||
	    (config->output_port & X86_I8042_OUTPUT_PORT_RESET_HIGH) == 0u)
		return false;
	if (config->keyboard_present == 0u &&
	    (config->keyboard_scanning_enabled != 0u ||
	     (config->command_byte & X86_I8042_COMMAND_BYTE_IRQ1) != 0u ||
	     (config->command_byte &
	      X86_I8042_COMMAND_BYTE_KEYBOARD_DISABLED) == 0u))
		return false;
	if (config->auxiliary_present == 0u &&
	    ((config->command_byte & X86_I8042_COMMAND_BYTE_IRQ12) != 0u ||
	     (config->command_byte &
	      X86_I8042_COMMAND_BYTE_AUXILIARY_DISABLED) == 0u))
		return false;
	return true;
}

static bool input_config_is_valid(
	const struct x86_i8042_input_config *config)
{
	return config != NULL && config->capabilities != 0u &&
	       (config->capabilities & (uint32_t)~X86_I8042_INPUT_CAPABILITIES) ==
		       0u &&
	       bytes_are_zero(config->reserved, ARRAY_SIZE(config->reserved));
}

static bool keyboard_mode_is_valid(
	const struct x86_i8042_keyboard_mode *mode)
{
	return mode != NULL && identity_is_valid(mode->source_identity) &&
	       mode->controller_generation != 0u &&
	       mode->source_generation != 0u && mode->mode_generation != 0u &&
	       mode->scan_set != 0u && mode->scan_set <= 3u &&
	       mode->translation_enabled <= 1u && mode->scanning_enabled <= 1u &&
	       mode->interface_enabled <= 1u &&
	       bytes_are_zero(mode->reserved, ARRAY_SIZE(mode->reserved));
}

static bool keyboard_mode_epoch_available(
	const struct i8042_owner *owner, bool changing)
{
	return !changing ||
	       owner->keyboard_mode_generation < X86_I8042_MODE_GENERATION_MAX;
}

static void keyboard_mode_epoch_commit(struct i8042_owner *owner,
				       bool changing)
{
	if (changing)
		owner->keyboard_mode_generation++;
}

static uint8_t output_index(const struct i8042_owner *owner, uint8_t offset)
{
	return (uint8_t)((owner->output_head + offset) %
			 CONFIG_X86_I8042_OUTPUT_FIFO_CAPACITY);
}

static uint8_t event_index(const struct i8042_owner *owner, uint8_t offset)
{
	return (uint8_t)((owner->event_head + offset) %
			 CONFIG_X86_I8042_EVENT_FIFO_CAPACITY);
}

static uint8_t source_irq_mask(uint8_t source)
{
	if (source == I8042_OUTPUT_KEYBOARD)
		return I8042_PENDING_IRQ1;
	if (source == I8042_OUTPUT_AUXILIARY)
		return I8042_PENDING_IRQ12;
	return 0u;
}

static bool command_byte_source_irq_enabled(uint8_t command_byte,
				    uint8_t source)
{
	if (source == I8042_OUTPUT_KEYBOARD) {
		return (command_byte & X86_I8042_COMMAND_BYTE_IRQ1) != 0u &&
		       (command_byte &
			X86_I8042_COMMAND_BYTE_KEYBOARD_DISABLED) == 0u;
	}
	if (source == I8042_OUTPUT_AUXILIARY) {
		return (command_byte & X86_I8042_COMMAND_BYTE_IRQ12) != 0u &&
		       (command_byte &
			X86_I8042_COMMAND_BYTE_AUXILIARY_DISABLED) == 0u;
	}
	return false;
}

static bool source_irq_enabled(const struct i8042_owner *owner,
			       uint8_t source)
{
	return command_byte_source_irq_enabled(owner->command_byte, source);
}

static bool event_has_capacity(const struct i8042_owner *owner)
{
	return owner->event_count < CONFIG_X86_I8042_EVENT_FIFO_CAPACITY &&
	       owner->next_event_sequence < X86_I8042_EVENT_SEQUENCE_MAX;
}

static bool irq_event_needs_slot(const struct i8042_owner *owner,
				 uint8_t source)
{
	uint8_t mask = source_irq_mask(source);

	return source_irq_enabled(owner, source) &&
	       (owner->pending_irq_mask & mask) == 0u;
}

static bool enqueue_event(struct i8042_owner *owner, uint8_t kind,
			  uint8_t irq, uint8_t a20_enabled)
{
	struct x86_i8042_event event;
	uint8_t mask = 0u;
	uint8_t index;

	if (kind == X86_I8042_EVENT_IRQ_REQUEST) {
		mask = irq == 1u ? I8042_PENDING_IRQ1 : I8042_PENDING_IRQ12;
		if ((owner->pending_irq_mask & mask) != 0u)
			return true;
	}
	if (!event_has_capacity(owner)) {
		owner->event_overflow_count =
			saturating_increment(owner->event_overflow_count);
		return false;
	}
	event = (struct x86_i8042_event){
		.controller_generation = owner->generation,
		.sequence = owner->next_event_sequence + 1u,
		.kind = kind,
		.irq = irq,
		.a20_enabled = a20_enabled,
		.reserved = {0u},
	};
	index = event_index(owner, owner->event_count);
	owner->event_fifo[index] = event;
	owner->next_event_sequence = event.sequence;
	++owner->event_count;
	owner->pending_irq_mask |= mask;
	return true;
}

static bool enqueue_irq_for_source(struct i8042_owner *owner,
				   uint8_t source)
{
	if (!source_irq_enabled(owner, source))
		return true;
	return enqueue_event(owner, X86_I8042_EVENT_IRQ_REQUEST,
			     source == I8042_OUTPUT_KEYBOARD ? 1u : 12u, 0u);
}

static bool output_has_capacity(const struct i8042_owner *owner,
				 size_t count)
{
	return count <= CONFIG_X86_I8042_OUTPUT_FIFO_CAPACITY &&
	       owner->output_count <=
		       CONFIG_X86_I8042_OUTPUT_FIFO_CAPACITY - count;
}

static bool enqueue_output(struct i8042_owner *owner, uint8_t source,
			   const uint8_t *values, size_t count)
{
	uint8_t index;
	size_t offset;

	if (values == NULL || count == 0u || !output_has_capacity(owner, count)) {
		owner->output_overflow_count =
			saturating_increment(owner->output_overflow_count);
		return false;
	}
	if (owner->output_count == 0u && irq_event_needs_slot(owner, source) &&
	    !event_has_capacity(owner)) {
		owner->event_overflow_count =
			saturating_increment(owner->event_overflow_count);
		return false;
	}
	for (offset = 0u; offset < count; ++offset) {
		index = output_index(owner, owner->output_count);
		owner->output_fifo[index].value = values[offset];
		owner->output_fifo[index].source = source;
		++owner->output_count;
	}
	if (owner->output_count == count &&
	    !enqueue_irq_for_source(owner, source)) {
		owner->output_count = 0u;
		return false;
	}
	return true;
}

static uint8_t controller_status(const struct i8042_owner *owner)
{
	uint8_t status = 0u;

	if (owner->output_count != 0u) {
		status |= X86_I8042_STATUS_OUTPUT_FULL;
		if (owner->output_fifo[owner->output_head].source ==
		    I8042_OUTPUT_AUXILIARY)
			status |= X86_I8042_STATUS_AUXILIARY;
	}
	if ((owner->command_byte & X86_I8042_COMMAND_BYTE_SYSTEM) != 0u)
		status |= X86_I8042_STATUS_SYSTEM;
	if (owner->last_write_command != 0u)
		status |= X86_I8042_STATUS_COMMAND;
	if (owner->keyboard_unlocked != 0u ||
	    (owner->command_byte & X86_I8042_COMMAND_BYTE_IGNORE_LOCK) != 0u)
		status |= X86_I8042_STATUS_KEY_UNLOCKED;
	return status;
}

static struct x86_io_resource_descriptor descriptor(
	kernel_object_handle_t owner_identity,
	kernel_object_handle_t context_identity, uint16_t port,
	x86_io_read_callback_t read, x86_io_write_callback_t write)
{
	return (struct x86_io_resource_descriptor){
		.owner_identity = owner_identity,
		.callback_context = context_identity,
		.read = read,
		.write = write,
		.first_port = port,
		.last_port = port,
		.read_width_mask = X86_IO_WIDTH_MASK_8,
		.write_width_mask = X86_IO_WIDTH_MASK_8,
		.read_action = X86_IO_RESOURCE_ACTION_EMULATE,
		.write_action = X86_IO_RESOURCE_ACTION_EMULATE,
		.flags = 0u,
		.reserved = {0u},
	};
}

static enum x86_io_callback_status i8042_read(
	kernel_object_handle_t context, uint16_t port,
	enum dos_io_width width, uint32_t *value);
static enum x86_io_callback_status i8042_write(
	kernel_object_handle_t context, uint16_t port,
	enum dos_io_width width, uint32_t value);

enum x86_i8042_status x86_i8042_prepare(
	kernel_object_handle_t context_identity,
	kernel_object_handle_t owner_identity,
	const struct x86_i8042_config *config,
	struct x86_io_resource_descriptor *descriptors,
	size_t descriptor_capacity)
{
	struct i8042_owner prepared;
	uint64_t generation;

	if (!identity_is_valid(context_identity) ||
	    !identity_is_valid(owner_identity) ||
	    context_identity == owner_identity || descriptors == NULL ||
	    !config_is_valid(config))
		return X86_I8042_INVALID_ARGUMENT;
	if (descriptor_capacity < X86_I8042_RESOURCE_COUNT)
		return X86_I8042_CAPACITY_EXHAUSTED;
	if (runtime_owner.phase == I8042_PHASE_POISONED)
		return X86_I8042_POISONED;
	if (runtime_owner.phase != I8042_PHASE_EMPTY)
		return X86_I8042_INVALID_STATE;
	if (runtime_owner.generation >= X86_I8042_GENERATION_MAX)
		return X86_I8042_CAPACITY_EXHAUSTED;

	generation = runtime_owner.generation + 1u;
	prepared = (struct i8042_owner){
		.context_identity = context_identity,
		.owner_identity = owner_identity,
		.input_source_identity = KERNEL_OBJECT_HANDLE_INVALID,
		.generation = generation,
		.keyboard_mode_generation = 1u,
		.command_byte = config->command_byte,
		.input_port = config->input_port,
		.output_port = config->output_port,
		.keyboard_present = config->keyboard_present,
		.auxiliary_present = config->auxiliary_present,
		.keyboard_scanning_enabled =
			config->keyboard_scanning_enabled,
		.keyboard_scan_set = config->keyboard_scan_set,
		.keyboard_unlocked = config->keyboard_unlocked,
		.keyboard_leds = config->keyboard_leds,
		.keyboard_typematic = config->keyboard_typematic,
		.keyboard_id_length = config->keyboard_id_length,
		.keyboard_id_first = config->keyboard_id_first,
		.keyboard_id_second = config->keyboard_id_second,
		.auxiliary_id = config->auxiliary_id,
		.last_keyboard_reply = I8042_RESPONSE_RESEND,
		.phase = I8042_PHASE_PREPARED,
	};
	descriptors[0] = descriptor(owner_identity, context_identity,
				    X86_I8042_DATA_PORT, i8042_read,
				    i8042_write);
	descriptors[1] = descriptor(owner_identity, context_identity,
				    X86_I8042_COMMAND_PORT, i8042_read,
				    i8042_write);
	runtime_owner = prepared;
	return X86_I8042_OK;
}

enum x86_i8042_status x86_i8042_publish(
	kernel_object_handle_t context_identity)
{
	if (!identity_is_valid(context_identity))
		return X86_I8042_INVALID_ARGUMENT;
	if (runtime_owner.phase == I8042_PHASE_POISONED)
		return X86_I8042_POISONED;
	if (runtime_owner.context_identity != context_identity)
		return X86_I8042_IDENTITY_MISMATCH;
	if (runtime_owner.phase != I8042_PHASE_PREPARED)
		return X86_I8042_INVALID_STATE;
	runtime_owner.phase = I8042_PHASE_ACTIVE;
	return X86_I8042_OK;
}

enum x86_i8042_status x86_i8042_abort(
	kernel_object_handle_t context_identity)
{
	uint64_t generation;

	if (!identity_is_valid(context_identity))
		return X86_I8042_INVALID_ARGUMENT;
	if (runtime_owner.phase == I8042_PHASE_POISONED)
		return X86_I8042_POISONED;
	if (runtime_owner.context_identity != context_identity)
		return X86_I8042_IDENTITY_MISMATCH;
	if (runtime_owner.phase != I8042_PHASE_PREPARED)
		return X86_I8042_INVALID_STATE;
	generation = runtime_owner.generation;
	runtime_owner = (struct i8042_owner){
		.generation = generation,
	};
	return X86_I8042_OK;
}

enum x86_i8042_status x86_i8042_poison(
	kernel_object_handle_t context_identity)
{
	if (!identity_is_valid(context_identity))
		return X86_I8042_INVALID_ARGUMENT;
	if (runtime_owner.context_identity != context_identity)
		return X86_I8042_IDENTITY_MISMATCH;
	if (runtime_owner.phase == I8042_PHASE_EMPTY)
		return X86_I8042_INVALID_STATE;
	runtime_owner.phase = I8042_PHASE_POISONED;
	return X86_I8042_OK;
}

enum x86_i8042_status x86_i8042_input_bind(
	kernel_object_handle_t context_identity,
	kernel_object_handle_t owner_identity,
	kernel_object_handle_t source_identity,
	const struct x86_i8042_input_config *config,
	struct x86_i8042_input_binding *binding)
{
	if (!identity_is_valid(context_identity) ||
	    !identity_is_valid(owner_identity) ||
	    !identity_is_valid(source_identity) ||
	    source_identity == context_identity ||
	    source_identity == owner_identity || !input_config_is_valid(config) ||
	    binding == NULL)
		return X86_I8042_INVALID_ARGUMENT;
	if (runtime_owner.phase == I8042_PHASE_POISONED)
		return X86_I8042_POISONED;
	if (runtime_owner.context_identity != context_identity ||
	    runtime_owner.owner_identity != owner_identity)
		return X86_I8042_IDENTITY_MISMATCH;
	if (runtime_owner.phase != I8042_PHASE_ACTIVE ||
	    runtime_owner.input_source_phase != I8042_INPUT_EMPTY)
		return X86_I8042_INVALID_STATE;
	if (runtime_owner.input_source_generation >=
	    X86_I8042_INPUT_GENERATION_MAX)
		return X86_I8042_CAPACITY_EXHAUSTED;
	if (((config->capabilities & X86_I8042_INPUT_KEYBOARD) != 0u &&
	     runtime_owner.keyboard_present == 0u) ||
	    ((config->capabilities & X86_I8042_INPUT_AUXILIARY) != 0u &&
	     runtime_owner.auxiliary_present == 0u))
		return X86_I8042_INVALID_ARGUMENT;

	runtime_owner.input_source_identity = source_identity;
	runtime_owner.input_source_generation++;
	runtime_owner.input_capabilities = config->capabilities;
	runtime_owner.input_source_phase = I8042_INPUT_ACTIVE;
	*binding = (struct x86_i8042_input_binding){
		.context_identity = context_identity,
		.owner_identity = owner_identity,
		.controller_generation = runtime_owner.generation,
		.source_identity = source_identity,
		.source_generation = runtime_owner.input_source_generation,
		.capabilities = config->capabilities,
		.reserved = {0u},
	};
	return X86_I8042_OK;
}

static enum x86_i8042_status input_binding_status(
	kernel_object_handle_t context_identity,
	kernel_object_handle_t owner_identity,
	const struct x86_i8042_input_binding *binding)
{
	if (!identity_is_valid(context_identity) ||
	    !identity_is_valid(owner_identity) || binding == NULL ||
	    !identity_is_valid(binding->context_identity) ||
	    !identity_is_valid(binding->owner_identity) ||
	    !identity_is_valid(binding->source_identity) ||
	    binding->capabilities == 0u ||
	    (binding->capabilities & (uint32_t)~X86_I8042_INPUT_CAPABILITIES) !=
		    0u ||
	    !bytes_are_zero(binding->reserved, ARRAY_SIZE(binding->reserved)))
		return X86_I8042_INVALID_ARGUMENT;
	if (runtime_owner.phase == I8042_PHASE_POISONED)
		return X86_I8042_POISONED;
	if (runtime_owner.context_identity != context_identity ||
	    runtime_owner.owner_identity != owner_identity ||
	    binding->context_identity != context_identity ||
	    binding->owner_identity != owner_identity)
		return X86_I8042_IDENTITY_MISMATCH;
	if (runtime_owner.phase != I8042_PHASE_ACTIVE)
		return X86_I8042_INVALID_STATE;
	if (binding->controller_generation != runtime_owner.generation ||
	    binding->source_generation !=
		    runtime_owner.input_source_generation ||
	    runtime_owner.input_source_phase == I8042_INPUT_EMPTY)
		return X86_I8042_STALE_BINDING;
	if (runtime_owner.input_source_identity != binding->source_identity)
		return X86_I8042_IDENTITY_MISMATCH;
	if (runtime_owner.input_capabilities != binding->capabilities)
		return X86_I8042_STALE_BINDING;
	return X86_I8042_OK;
}

enum x86_i8042_status x86_i8042_input_quiesce(
	kernel_object_handle_t context_identity,
	kernel_object_handle_t owner_identity,
	const struct x86_i8042_input_binding *binding)
{
	enum x86_i8042_status status = input_binding_status(
		context_identity, owner_identity, binding);

	if (status != X86_I8042_OK)
		return status;
	if (runtime_owner.input_source_phase != I8042_INPUT_ACTIVE)
		return X86_I8042_INVALID_STATE;
	runtime_owner.input_source_phase = I8042_INPUT_QUIESCED;
	return X86_I8042_OK;
}

enum x86_i8042_status x86_i8042_input_resume(
	kernel_object_handle_t context_identity,
	kernel_object_handle_t owner_identity,
	const struct x86_i8042_input_binding *binding)
{
	enum x86_i8042_status status = input_binding_status(
		context_identity, owner_identity, binding);

	if (status != X86_I8042_OK)
		return status;
	if (runtime_owner.input_source_phase != I8042_INPUT_QUIESCED)
		return X86_I8042_INVALID_STATE;
	runtime_owner.input_source_phase = I8042_INPUT_ACTIVE;
	return X86_I8042_OK;
}

enum x86_i8042_status x86_i8042_input_unbind(
	kernel_object_handle_t context_identity,
	kernel_object_handle_t owner_identity,
	const struct x86_i8042_input_binding *binding)
{
	enum x86_i8042_status status = input_binding_status(
		context_identity, owner_identity, binding);

	if (status != X86_I8042_OK)
		return status;
	if (runtime_owner.input_source_phase != I8042_INPUT_QUIESCED)
		return X86_I8042_INVALID_STATE;
	runtime_owner.input_source_identity = KERNEL_OBJECT_HANDLE_INVALID;
	runtime_owner.input_capabilities = 0u;
	runtime_owner.input_source_phase = I8042_INPUT_EMPTY;
	return X86_I8042_OK;
}

static enum x86_i8042_status active_input_binding(
	const struct x86_i8042_input_binding *binding)
{
	enum x86_i8042_status status;

	if (binding == NULL)
		return X86_I8042_INVALID_ARGUMENT;
	status = input_binding_status(binding->context_identity,
				      binding->owner_identity, binding);
	if (status != X86_I8042_OK)
		return status;
	if (runtime_owner.input_source_phase != I8042_INPUT_ACTIVE)
		return X86_I8042_INVALID_STATE;
	return X86_I8042_OK;
}

enum x86_i8042_status x86_i8042_input_keyboard_mode(
	const struct x86_i8042_input_binding *binding,
	struct x86_i8042_keyboard_mode *mode)
{
	struct x86_i8042_keyboard_mode prepared;
	enum x86_i8042_status status;

	if (mode == NULL)
		return X86_I8042_INVALID_ARGUMENT;
	status = active_input_binding(binding);
	if (status != X86_I8042_OK)
		return status;
	if ((runtime_owner.input_capabilities & X86_I8042_INPUT_KEYBOARD) == 0u ||
	    runtime_owner.keyboard_present == 0u)
		return X86_I8042_INVALID_ARGUMENT;
	prepared = (struct x86_i8042_keyboard_mode){
		.source_identity = runtime_owner.input_source_identity,
		.controller_generation = runtime_owner.generation,
		.source_generation = runtime_owner.input_source_generation,
		.mode_generation = runtime_owner.keyboard_mode_generation,
		.scan_set = runtime_owner.keyboard_scan_set,
		.translation_enabled = (uint8_t)(
			(runtime_owner.command_byte &
			 X86_I8042_COMMAND_BYTE_TRANSLATE) != 0u),
		.scanning_enabled = runtime_owner.keyboard_scanning_enabled,
		.interface_enabled = (uint8_t)(
			(runtime_owner.command_byte &
			 X86_I8042_COMMAND_BYTE_KEYBOARD_DISABLED) == 0u),
		.reserved = {0u},
	};
	*mode = prepared;
	return X86_I8042_OK;
}

static bool keyboard_mode_matches_owner(
	const struct x86_i8042_keyboard_mode *mode)
{
	return mode->source_identity == runtime_owner.input_source_identity &&
	       mode->controller_generation == runtime_owner.generation &&
	       mode->source_generation == runtime_owner.input_source_generation &&
	       mode->mode_generation == runtime_owner.keyboard_mode_generation &&
	       mode->scan_set == runtime_owner.keyboard_scan_set &&
	       mode->translation_enabled ==
		       (uint8_t)((runtime_owner.command_byte &
				  X86_I8042_COMMAND_BYTE_TRANSLATE) != 0u) &&
	       mode->scanning_enabled == runtime_owner.keyboard_scanning_enabled &&
	       mode->interface_enabled ==
		       (uint8_t)((runtime_owner.command_byte &
				  X86_I8042_COMMAND_BYTE_KEYBOARD_DISABLED) == 0u);
}

enum x86_i8042_status x86_i8042_input_inject_keyboard_sequence(
	const struct x86_i8042_input_binding *binding,
	const struct x86_i8042_keyboard_mode *mode, const uint8_t *values,
	size_t values_capacity, size_t count)
{
	enum x86_i8042_status status;

	if (!keyboard_mode_is_valid(mode) || values == NULL || count == 0u ||
	    count > values_capacity)
		return X86_I8042_INVALID_ARGUMENT;
	status = active_input_binding(binding);
	if (status != X86_I8042_OK)
		return status;
	if ((runtime_owner.input_capabilities & X86_I8042_INPUT_KEYBOARD) == 0u ||
	    runtime_owner.keyboard_present == 0u)
		return X86_I8042_INVALID_ARGUMENT;
	if (!keyboard_mode_matches_owner(mode))
		return X86_I8042_MODE_CHANGED;
	if (runtime_owner.keyboard_scanning_enabled == 0u ||
	    (runtime_owner.command_byte &
	     X86_I8042_COMMAND_BYTE_KEYBOARD_DISABLED) != 0u)
		return X86_I8042_INPUT_DISABLED;
	if (!enqueue_output(&runtime_owner, I8042_OUTPUT_KEYBOARD, values, count))
		return X86_I8042_CAPACITY_EXHAUSTED;
	return X86_I8042_OK;
}

enum x86_i8042_status x86_i8042_input_inject_keyboard(
	const struct x86_i8042_input_binding *binding,
	const struct x86_i8042_keyboard_mode *mode, uint8_t value)
{
	return x86_i8042_input_inject_keyboard_sequence(
		binding, mode, &value, sizeof(value), 1u);
}

enum x86_i8042_status x86_i8042_input_inject_sequence(
	const struct x86_i8042_input_binding *binding,
	enum x86_i8042_input_kind kind, const uint8_t *values,
	size_t values_capacity, size_t count)
{
	enum x86_i8042_status status;

	if (values == NULL || count == 0u || count > values_capacity ||
	    kind != X86_I8042_INPUT_KIND_AUXILIARY_BYTE)
		return X86_I8042_INVALID_ARGUMENT;
	status = active_input_binding(binding);
	if (status != X86_I8042_OK)
		return status;
	if ((runtime_owner.input_capabilities & X86_I8042_INPUT_AUXILIARY) == 0u ||
	    runtime_owner.auxiliary_present == 0u)
		return X86_I8042_INVALID_ARGUMENT;
	if ((runtime_owner.command_byte &
	     X86_I8042_COMMAND_BYTE_AUXILIARY_DISABLED) != 0u)
		return X86_I8042_INPUT_DISABLED;
	if (!enqueue_output(&runtime_owner, I8042_OUTPUT_AUXILIARY, values, count))
		return X86_I8042_CAPACITY_EXHAUSTED;
	return X86_I8042_OK;
}

enum x86_i8042_status x86_i8042_input_inject(
	const struct x86_i8042_input_binding *binding,
	enum x86_i8042_input_kind kind, uint8_t value)
{
	return x86_i8042_input_inject_sequence(binding, kind, &value,
					       sizeof(value), 1u);
}

static enum x86_i8042_status active_owner(
	kernel_object_handle_t context_identity,
	kernel_object_handle_t owner_identity)
{
	if (!identity_is_valid(context_identity) ||
	    !identity_is_valid(owner_identity))
		return X86_I8042_INVALID_ARGUMENT;
	if (runtime_owner.phase == I8042_PHASE_POISONED)
		return X86_I8042_POISONED;
	if (runtime_owner.context_identity != context_identity ||
	    runtime_owner.owner_identity != owner_identity)
		return X86_I8042_IDENTITY_MISMATCH;
	if (runtime_owner.phase != I8042_PHASE_ACTIVE)
		return X86_I8042_INVALID_STATE;
	return X86_I8042_OK;
}

enum x86_i8042_status x86_i8042_event_peek(
	kernel_object_handle_t context_identity,
	kernel_object_handle_t owner_identity,
	struct x86_i8042_event *event)
{
	enum x86_i8042_status status;

	if (event == NULL)
		return X86_I8042_INVALID_ARGUMENT;
	status = active_owner(context_identity, owner_identity);
	if (status != X86_I8042_OK)
		return status;
	if (runtime_owner.event_count == 0u)
		return X86_I8042_NO_EVENT;
	*event = runtime_owner.event_fifo[runtime_owner.event_head];
	return X86_I8042_OK;
}

enum x86_i8042_status x86_i8042_event_consume(
	kernel_object_handle_t context_identity,
	kernel_object_handle_t owner_identity, uint64_t sequence)
{
	const struct x86_i8042_event *event;
	enum x86_i8042_status status;
	uint8_t mask = 0u;

	status = active_owner(context_identity, owner_identity);
	if (status != X86_I8042_OK)
		return status;
	if (runtime_owner.event_count == 0u)
		return X86_I8042_NO_EVENT;
	event = &runtime_owner.event_fifo[runtime_owner.event_head];
	if (sequence == 0u || event->sequence != sequence)
		return X86_I8042_STALE_EVENT;
	if (event->kind == X86_I8042_EVENT_IRQ_REQUEST)
		mask = event->irq == 1u ? I8042_PENDING_IRQ1 :
					     I8042_PENDING_IRQ12;
	runtime_owner.pending_irq_mask &= (uint8_t)~mask;
	runtime_owner.event_head = event_index(&runtime_owner, 1u);
	--runtime_owner.event_count;
	return X86_I8042_OK;
}

static bool queue_keyboard_reply(struct i8042_owner *owner,
				 const uint8_t *reply, size_t count)
{
	if (!enqueue_output(owner, I8042_OUTPUT_KEYBOARD, reply, count))
		return false;
	owner->last_keyboard_reply = reply[count - 1u];
	return true;
}

static bool keyboard_parameter(struct i8042_owner *owner, uint8_t value)
{
	uint8_t reply[2] = {I8042_RESPONSE_ACK, 0u};

	if (owner->pending_command == I8042_PENDING_KEYBOARD_LEDS) {
		if (!queue_keyboard_reply(owner, reply, 1u))
			return false;
		owner->keyboard_leds = (uint8_t)(value & 0x07u);
	} else if (owner->pending_command == I8042_PENDING_KEYBOARD_TYPEMATIC) {
		if (!queue_keyboard_reply(owner, reply, 1u))
			return false;
		owner->keyboard_typematic = value;
	} else if (owner->pending_command == I8042_PENDING_KEYBOARD_SCAN_SET) {
		if (value == 0u) {
			reply[1] = owner->keyboard_scan_set;
			if (!queue_keyboard_reply(owner, reply, 2u))
				return false;
		} else if (value <= 3u) {
			bool mode_changing = owner->keyboard_scan_set != value;

			if (!keyboard_mode_epoch_available(owner, mode_changing))
				return false;
			if (!queue_keyboard_reply(owner, reply, 1u))
				return false;
			owner->keyboard_scan_set = value;
			keyboard_mode_epoch_commit(owner, mode_changing);
		} else {
			reply[0] = I8042_RESPONSE_RESEND;
			if (!queue_keyboard_reply(owner, reply, 1u))
				return false;
		}
	} else {
		return false;
	}
	owner->pending_command = I8042_PENDING_NONE;
	return true;
}

static bool keyboard_command(struct i8042_owner *owner, uint8_t command)
{
	uint8_t reply[3] = {I8042_RESPONSE_ACK, 0u, 0u};

	if (owner->keyboard_present == 0u)
		return true;
	switch (command) {
	case I8042_KEYBOARD_SET_LEDS:
		if (!queue_keyboard_reply(owner, reply, 1u))
			return false;
		owner->pending_command = I8042_PENDING_KEYBOARD_LEDS;
		break;
	case I8042_KEYBOARD_ECHO:
		reply[0] = I8042_KEYBOARD_ECHO;
		if (!queue_keyboard_reply(owner, reply, 1u))
			return false;
		break;
	case I8042_KEYBOARD_SET_SCAN_SET:
		if (!queue_keyboard_reply(owner, reply, 1u))
			return false;
		owner->pending_command = I8042_PENDING_KEYBOARD_SCAN_SET;
		break;
	case I8042_KEYBOARD_IDENTIFY:
		reply[1] = owner->keyboard_id_first;
		reply[2] = owner->keyboard_id_second;
		if (!queue_keyboard_reply(owner, reply,
					 1u + owner->keyboard_id_length))
			return false;
		break;
	case I8042_KEYBOARD_SET_TYPEMATIC:
		if (!queue_keyboard_reply(owner, reply, 1u))
			return false;
		owner->pending_command = I8042_PENDING_KEYBOARD_TYPEMATIC;
		break;
	case I8042_KEYBOARD_ENABLE_SCANNING:
	{
		bool mode_changing = owner->keyboard_scanning_enabled == 0u;

		if (!keyboard_mode_epoch_available(owner, mode_changing))
			return false;
		if (!queue_keyboard_reply(owner, reply, 1u))
			return false;
		owner->keyboard_scanning_enabled = 1u;
		keyboard_mode_epoch_commit(owner, mode_changing);
		break;
	}
	case I8042_KEYBOARD_DISABLE_SCANNING:
	{
		bool mode_changing = owner->keyboard_scanning_enabled != 0u;

		if (!keyboard_mode_epoch_available(owner, mode_changing))
			return false;
		if (!queue_keyboard_reply(owner, reply, 1u))
			return false;
		owner->keyboard_scanning_enabled = 0u;
		keyboard_mode_epoch_commit(owner, mode_changing);
		break;
	}
	case I8042_KEYBOARD_DEFAULTS:
	{
		bool mode_changing = owner->keyboard_scanning_enabled != 0u ||
				     owner->keyboard_scan_set != 2u;

		if (!keyboard_mode_epoch_available(owner, mode_changing))
			return false;
		if (!queue_keyboard_reply(owner, reply, 1u))
			return false;
		owner->keyboard_scanning_enabled = 0u;
		owner->keyboard_scan_set = 2u;
		owner->keyboard_leds = 0u;
		owner->keyboard_typematic = 0u;
		keyboard_mode_epoch_commit(owner, mode_changing);
		break;
	}
	case I8042_KEYBOARD_RESEND:
		reply[0] = owner->last_keyboard_reply;
		if (!queue_keyboard_reply(owner, reply, 1u))
			return false;
		break;
	case I8042_KEYBOARD_RESET:
	{
		bool mode_changing = owner->keyboard_scanning_enabled == 0u ||
				     owner->keyboard_scan_set != 2u;

		if (!keyboard_mode_epoch_available(owner, mode_changing))
			return false;
		reply[1] = I8042_RESPONSE_BAT_OK;
		if (!queue_keyboard_reply(owner, reply, 2u))
			return false;
		owner->keyboard_scanning_enabled = 1u;
		owner->keyboard_scan_set = 2u;
		owner->keyboard_leds = 0u;
		owner->keyboard_typematic = 0u;
		keyboard_mode_epoch_commit(owner, mode_changing);
		break;
	}
	default:
		reply[0] = I8042_RESPONSE_RESEND;
		if (!queue_keyboard_reply(owner, reply, 1u))
			return false;
		owner->unsupported_command_count =
			saturating_increment(owner->unsupported_command_count);
		break;
	}
	return true;
}

static bool auxiliary_command(struct i8042_owner *owner, uint8_t command)
{
	uint8_t reply[3] = {I8042_RESPONSE_ACK, 0u, 0u};

	if (owner->auxiliary_present == 0u)
		return true;
	if (owner->pending_command == I8042_PENDING_AUXILIARY_PARAMETER) {
		if (!enqueue_output(owner, I8042_OUTPUT_AUXILIARY, reply, 1u))
			return false;
		owner->pending_command = I8042_PENDING_NONE;
		return true;
	}
	if (command == I8042_KEYBOARD_RESET) {
		reply[1] = I8042_RESPONSE_BAT_OK;
		reply[2] = owner->auxiliary_id;
		return enqueue_output(owner, I8042_OUTPUT_AUXILIARY, reply, 3u);
	}
	if (command == I8042_KEYBOARD_IDENTIFY) {
		reply[1] = owner->auxiliary_id;
		return enqueue_output(owner, I8042_OUTPUT_AUXILIARY, reply, 2u);
	}
	if (command == 0xe8u || command == I8042_KEYBOARD_SET_TYPEMATIC) {
		if (!enqueue_output(owner, I8042_OUTPUT_AUXILIARY, reply, 1u))
			return false;
		owner->pending_command = I8042_PENDING_AUXILIARY_PARAMETER;
		return true;
	}
	return enqueue_output(owner, I8042_OUTPUT_AUXILIARY, reply, 1u);
}

static bool set_output_port(struct i8042_owner *owner, uint8_t value)
{
	uint8_t a20 = (uint8_t)((value & X86_I8042_OUTPUT_PORT_A20) != 0u);
	uint8_t old_a20 = (uint8_t)(
		(owner->output_port & X86_I8042_OUTPUT_PORT_A20) != 0u);

	if (a20 != old_a20 && !event_has_capacity(owner)) {
		owner->event_overflow_count =
			saturating_increment(owner->event_overflow_count);
		return false;
	}
	if ((value & X86_I8042_OUTPUT_PORT_RESET_HIGH) == 0u) {
		owner->suppressed_reset_requests =
			saturating_increment(owner->suppressed_reset_requests);
		value |= X86_I8042_OUTPUT_PORT_RESET_HIGH;
	}
	if (a20 != old_a20 &&
	    !enqueue_event(owner, X86_I8042_EVENT_A20_CHANGE, 0u, a20))
		return false;
	owner->output_port = value;
	return true;
}

static bool write_command_byte(struct i8042_owner *owner, uint8_t value)
{
	bool mode_changing =
		((owner->command_byte ^ value) &
		 (X86_I8042_COMMAND_BYTE_KEYBOARD_DISABLED |
		  X86_I8042_COMMAND_BYTE_TRANSLATE)) != 0u;
	uint8_t source = I8042_OUTPUT_CONTROLLER;

	if (!keyboard_mode_epoch_available(owner, mode_changing))
		return false;
	if (owner->output_count != 0u)
		source = owner->output_fifo[owner->output_head].source;
	if (owner->output_count != 0u &&
	    command_byte_source_irq_enabled(value, source) &&
	    (owner->pending_irq_mask & source_irq_mask(source)) == 0u &&
	    !event_has_capacity(owner)) {
		owner->event_overflow_count =
			saturating_increment(owner->event_overflow_count);
		return false;
	}
	owner->command_byte = value;
	keyboard_mode_epoch_commit(owner, mode_changing);
	return owner->output_count == 0u || enqueue_irq_for_source(owner, source);
}

static enum x86_io_callback_status write_data(struct i8042_owner *owner,
				       uint8_t value)
{
	uint8_t pending = owner->pending_command;
	bool successful;

	if (pending == I8042_PENDING_COMMAND_BYTE)
		successful = write_command_byte(owner, value);
	else if (pending == I8042_PENDING_OUTPUT_PORT)
		successful = set_output_port(owner, value);
	else if (pending == I8042_PENDING_KEYBOARD_OUTPUT)
		successful = enqueue_output(owner, I8042_OUTPUT_KEYBOARD,
					    &value, 1u);
	else if (pending == I8042_PENDING_AUXILIARY_OUTPUT)
		successful = enqueue_output(owner, I8042_OUTPUT_AUXILIARY,
					    &value, 1u);
	else if (pending == I8042_PENDING_AUXILIARY_COMMAND ||
		 pending == I8042_PENDING_AUXILIARY_PARAMETER)
		successful = auxiliary_command(owner, value);
	else if (pending == I8042_PENDING_KEYBOARD_LEDS ||
		 pending == I8042_PENDING_KEYBOARD_SCAN_SET ||
		 pending == I8042_PENDING_KEYBOARD_TYPEMATIC)
		successful = keyboard_parameter(owner, value);
	else
		successful = keyboard_command(owner, value);
	if (!successful)
		return X86_IO_CALLBACK_FAULT;
	if (pending >= I8042_PENDING_COMMAND_BYTE &&
	    pending <= I8042_PENDING_AUXILIARY_OUTPUT)
		owner->pending_command = I8042_PENDING_NONE;
	owner->last_write_command = 0u;
	return X86_IO_CALLBACK_OK;
}

static bool queue_controller_byte(struct i8042_owner *owner, uint8_t value)
{
	return enqueue_output(owner, I8042_OUTPUT_CONTROLLER, &value, 1u);
}

static enum x86_io_callback_status queue_controller_command_response(
	struct i8042_owner *owner, uint8_t value)
{
	if (!queue_controller_byte(owner, value))
		return X86_IO_CALLBACK_FAULT;
	owner->last_write_command = 1u;
	return X86_IO_CALLBACK_OK;
}

static enum x86_io_callback_status write_command(
	struct i8042_owner *owner, uint8_t command)
{
	uint8_t response;

	switch (command) {
	case I8042_COMMAND_READ_COMMAND_BYTE:
		return queue_controller_command_response(owner,
						 owner->command_byte);
	case I8042_COMMAND_WRITE_COMMAND_BYTE:
		owner->pending_command = I8042_PENDING_COMMAND_BYTE;
		break;
	case I8042_COMMAND_DISABLE_AUXILIARY:
		owner->command_byte |=
			X86_I8042_COMMAND_BYTE_AUXILIARY_DISABLED;
		break;
	case I8042_COMMAND_ENABLE_AUXILIARY:
		if (!write_command_byte(
			    owner,
			    (uint8_t)(
				    owner->command_byte &
				    (uint8_t)~X86_I8042_COMMAND_BYTE_AUXILIARY_DISABLED)))
			return X86_IO_CALLBACK_FAULT;
		break;
	case I8042_COMMAND_TEST_AUXILIARY:
		response = owner->auxiliary_present != 0u ?
				   I8042_RESPONSE_PORT_OK : 0xffu;
		return queue_controller_command_response(owner, response);
	case I8042_COMMAND_SELF_TEST:
		response = I8042_RESPONSE_CONTROLLER_OK;
		if (!queue_controller_byte(owner, response))
			return X86_IO_CALLBACK_FAULT;
		owner->command_byte |= X86_I8042_COMMAND_BYTE_SYSTEM;
		owner->last_write_command = 1u;
		return X86_IO_CALLBACK_OK;
	case I8042_COMMAND_TEST_KEYBOARD:
		response = owner->keyboard_present != 0u ?
				   I8042_RESPONSE_PORT_OK : 0xffu;
		return queue_controller_command_response(owner, response);
	case I8042_COMMAND_DISABLE_KEYBOARD:
		if (!write_command_byte(
			    owner,
			    (uint8_t)(
				    owner->command_byte |
				    X86_I8042_COMMAND_BYTE_KEYBOARD_DISABLED)))
			return X86_IO_CALLBACK_FAULT;
		break;
	case I8042_COMMAND_ENABLE_KEYBOARD:
		if (!write_command_byte(
			    owner,
			    (uint8_t)(
				    owner->command_byte &
				    (uint8_t)~X86_I8042_COMMAND_BYTE_KEYBOARD_DISABLED)))
			return X86_IO_CALLBACK_FAULT;
		break;
	case I8042_COMMAND_READ_INPUT_PORT:
		return queue_controller_command_response(owner,
						 owner->input_port);
	case I8042_COMMAND_READ_OUTPUT_PORT:
		return queue_controller_command_response(owner,
						 owner->output_port);
	case I8042_COMMAND_WRITE_OUTPUT_PORT:
		owner->pending_command = I8042_PENDING_OUTPUT_PORT;
		break;
	case I8042_COMMAND_WRITE_KEYBOARD_OUTPUT:
		owner->pending_command = I8042_PENDING_KEYBOARD_OUTPUT;
		break;
	case I8042_COMMAND_WRITE_AUXILIARY_OUTPUT:
		owner->pending_command = I8042_PENDING_AUXILIARY_OUTPUT;
		break;
	case I8042_COMMAND_WRITE_AUXILIARY:
		owner->pending_command = I8042_PENDING_AUXILIARY_COMMAND;
		break;
	case I8042_COMMAND_DISABLE_A20:
		if (!set_output_port(
			    owner, (uint8_t)(
				   owner->output_port &
				   (uint8_t)~X86_I8042_OUTPUT_PORT_A20)))
			return X86_IO_CALLBACK_FAULT;
		break;
	case I8042_COMMAND_ENABLE_A20:
		if (!set_output_port(
			    owner, (uint8_t)(owner->output_port |
					     X86_I8042_OUTPUT_PORT_A20)))
			return X86_IO_CALLBACK_FAULT;
		break;
	case I8042_COMMAND_PULSE_RESET:
		owner->suppressed_reset_requests = saturating_increment(
			owner->suppressed_reset_requests);
		break;
	default:
		owner->unsupported_command_count = saturating_increment(
			owner->unsupported_command_count);
		break;
	}
	owner->last_write_command = 1u;
	return X86_IO_CALLBACK_OK;
}

static enum x86_io_callback_status i8042_read(
	kernel_object_handle_t context, uint16_t port,
	enum dos_io_width width, uint32_t *value)
{
	struct i8042_output_entry entry;
	uint8_t next_source;

	if (value == NULL || width != DOS_IO_WIDTH_8 ||
	    (port != X86_I8042_DATA_PORT && port != X86_I8042_STATUS_PORT))
		return X86_IO_CALLBACK_DENIED;
	if (runtime_owner.phase != I8042_PHASE_ACTIVE ||
	    runtime_owner.context_identity != context)
		return X86_IO_CALLBACK_FAULT;
	if (port == X86_I8042_STATUS_PORT) {
		*value = controller_status(&runtime_owner);
		return X86_IO_CALLBACK_OK;
	}
	if (runtime_owner.output_count == 0u) {
		*value = 0xffu;
		return X86_IO_CALLBACK_OK;
	}
	if (runtime_owner.output_count > 1u) {
		next_source = runtime_owner.output_fifo[
			output_index(&runtime_owner, 1u)].source;
		if (irq_event_needs_slot(&runtime_owner, next_source) &&
		    !event_has_capacity(&runtime_owner)) {
			runtime_owner.event_overflow_count = saturating_increment(
				runtime_owner.event_overflow_count);
			return X86_IO_CALLBACK_FAULT;
		}
	}
	entry = runtime_owner.output_fifo[runtime_owner.output_head];
	runtime_owner.output_head = output_index(&runtime_owner, 1u);
	--runtime_owner.output_count;
	if (runtime_owner.output_count != 0u) {
		next_source = runtime_owner.output_fifo[
			runtime_owner.output_head].source;
		if (!enqueue_irq_for_source(&runtime_owner, next_source))
			return X86_IO_CALLBACK_FAULT;
	}
	*value = entry.value;
	return X86_IO_CALLBACK_OK;
}

static enum x86_io_callback_status i8042_write(
	kernel_object_handle_t context, uint16_t port,
	enum dos_io_width width, uint32_t value)
{
	if (width != DOS_IO_WIDTH_8 || value > 0xffu ||
	    (port != X86_I8042_DATA_PORT && port != X86_I8042_COMMAND_PORT))
		return X86_IO_CALLBACK_DENIED;
	if (runtime_owner.phase != I8042_PHASE_ACTIVE ||
	    runtime_owner.context_identity != context)
		return X86_IO_CALLBACK_FAULT;
	if (port == X86_I8042_COMMAND_PORT)
		return write_command(&runtime_owner, (uint8_t)value);
	return write_data(&runtime_owner, (uint8_t)value);
}

enum x86_i8042_status x86_i8042_snapshot(
	kernel_object_handle_t context_identity,
	struct x86_i8042_snapshot *snapshot)
{
	struct x86_i8042_snapshot prepared;

	if (!identity_is_valid(context_identity) || snapshot == NULL)
		return X86_I8042_INVALID_ARGUMENT;
	if (runtime_owner.context_identity != context_identity)
		return X86_I8042_IDENTITY_MISMATCH;
	if (runtime_owner.phase == I8042_PHASE_POISONED)
		return X86_I8042_POISONED;
	if (runtime_owner.phase != I8042_PHASE_ACTIVE)
		return X86_I8042_INVALID_STATE;
	prepared = (struct x86_i8042_snapshot){
		.generation = runtime_owner.generation,
		.context_identity = runtime_owner.context_identity,
		.owner_identity = runtime_owner.owner_identity,
		.input_source_identity = runtime_owner.input_source_identity,
		.input_source_generation =
			runtime_owner.input_source_generation,
		.output_overflow_count = runtime_owner.output_overflow_count,
		.event_overflow_count = runtime_owner.event_overflow_count,
		.suppressed_reset_requests =
			runtime_owner.suppressed_reset_requests,
		.unsupported_command_count =
			runtime_owner.unsupported_command_count,
		.command_byte = runtime_owner.command_byte,
		.input_port = runtime_owner.input_port,
		.output_port = runtime_owner.output_port,
		.status = controller_status(&runtime_owner),
		.pending_command = runtime_owner.pending_command,
		.output_count = runtime_owner.output_count,
		.event_count = runtime_owner.event_count,
		.pending_irq_mask = runtime_owner.pending_irq_mask,
		.keyboard_present = runtime_owner.keyboard_present,
		.auxiliary_present = runtime_owner.auxiliary_present,
		.keyboard_scanning_enabled =
			runtime_owner.keyboard_scanning_enabled,
		.keyboard_scan_set = runtime_owner.keyboard_scan_set,
		.keyboard_leds = runtime_owner.keyboard_leds,
		.keyboard_typematic = runtime_owner.keyboard_typematic,
		.keyboard_unlocked = runtime_owner.keyboard_unlocked,
		.active = 1u,
		.poisoned = 0u,
		.input_source_bound = (uint8_t)(
			runtime_owner.input_source_phase != I8042_INPUT_EMPTY),
		.a20_enabled = (uint8_t)(
			(runtime_owner.output_port & X86_I8042_OUTPUT_PORT_A20) !=
			0u),
		.keyboard_id_length = runtime_owner.keyboard_id_length,
		.keyboard_id_first = runtime_owner.keyboard_id_first,
		.keyboard_id_second = runtime_owner.keyboard_id_second,
		.auxiliary_id = runtime_owner.auxiliary_id,
		.input_source_quiesced = (uint8_t)(
			runtime_owner.input_source_phase == I8042_INPUT_QUIESCED),
	};
	*snapshot = prepared;
	return X86_I8042_OK;
}

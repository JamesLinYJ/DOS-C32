// SPDX-License-Identifier: GPL-2.0-only
/* Decoded-key to generation-bound guest PS/2 keyboard integration tests. */
#include "guest_ps2_keyboard.h"
#include "input_keycodes.h"
#include "test_entry.h"
#include "x86_guest_space.h"

#define CORE_ID ((kernel_object_handle_t)0x475032434f524531ull)
#define DEVICE_ID ((kernel_object_handle_t)0x4750324445564943ull)
#define HANDLER_A_ID ((kernel_object_handle_t)0x47503248414e4441ull)
#define HANDLER_B_ID ((kernel_object_handle_t)0x47503248414e4442ull)
#define CONTEXT_A_ID ((kernel_object_handle_t)0x475032434f4e5441ull)
#define CONTEXT_B_ID ((kernel_object_handle_t)0x475032434f4e5442ull)
#define MACHINE_ID ((kernel_object_handle_t)0x4750324d41434831ull)
#define SOURCE_A_ID ((kernel_object_handle_t)0x475032534f555241ull)
#define SOURCE_B_ID ((kernel_object_handle_t)0x475032534f555242ull)
#define CONTROLLER_ID ((kernel_object_handle_t)0x4750324938303432ull)
#define CONTROLLER_OWNER_ID ((kernel_object_handle_t)0x47503249384f574eull)

#define CAPTURE_CAPACITY 48u

enum fake_inject_behavior {
	FAKE_INJECT_NORMAL = 0,
	FAKE_INJECT_CAPACITY_ONCE,
	FAKE_INJECT_MODE_CHANGE_ONCE,
	FAKE_INJECT_COMMITTED_PENDING_ONCE
};

struct captured_sequence {
	kernel_object_handle_t source_identity;
	uint64_t source_generation;
	uint64_t mode_generation;
	uint8_t values[GUEST_PS2_KEYBOARD_SEQUENCE_CAPACITY];
	uint8_t count;
	uint8_t reserved[7];
};

struct fake_guest_keyboard {
	struct x86_i8042_input_binding binding;
	struct x86_i8042_keyboard_mode mode;
	struct captured_sequence captured[CAPTURE_CAPACITY];
	uint64_t controller_generation;
	uint64_t next_source_generation;
	size_t capture_count;
	size_t mode_query_count;
	size_t injection_attempt_count;
	size_t device_pump_count;
	uint8_t bound;
	uint8_t quiesced;
	uint8_t delivery_pending;
	uint8_t behavior;
	uint8_t reserved[4];
};

static struct fake_guest_keyboard fake_guest;

static bool bytes_are_zero(const uint8_t *bytes, size_t count)
{
	size_t index;

	for (index = 0u; index < count; ++index) {
		if (bytes[index] != 0u)
			return false;
	}
	return true;
}

static bool binding_matches(const struct x86_i8042_input_binding *binding)
{
	return binding != NULL && fake_guest.bound != 0u &&
	       binding->context_identity == fake_guest.binding.context_identity &&
	       binding->owner_identity == fake_guest.binding.owner_identity &&
	       binding->controller_generation ==
		       fake_guest.binding.controller_generation &&
	       binding->source_identity == fake_guest.binding.source_identity &&
	       binding->source_generation == fake_guest.binding.source_generation &&
	       binding->capabilities == fake_guest.binding.capabilities &&
	       bytes_are_zero(binding->reserved,
			      ARRAY_SIZE(binding->reserved));
}

static bool mode_matches(const struct x86_i8042_keyboard_mode *mode)
{
	return mode != NULL &&
	       mode->source_identity == fake_guest.mode.source_identity &&
	       mode->controller_generation ==
		       fake_guest.mode.controller_generation &&
	       mode->source_generation == fake_guest.mode.source_generation &&
	       mode->mode_generation == fake_guest.mode.mode_generation &&
	       mode->scan_set == fake_guest.mode.scan_set &&
	       mode->translation_enabled == fake_guest.mode.translation_enabled &&
	       mode->scanning_enabled == fake_guest.mode.scanning_enabled &&
	       mode->interface_enabled == fake_guest.mode.interface_enabled &&
	       bytes_are_zero(mode->reserved, ARRAY_SIZE(mode->reserved));
}

static void select_mode(uint8_t scan_set, uint8_t translation_enabled)
{
	fake_guest.mode.mode_generation++;
	fake_guest.mode.scan_set = scan_set;
	fake_guest.mode.translation_enabled = translation_enabled;
	fake_guest.mode.scanning_enabled = 1u;
	fake_guest.mode.interface_enabled = 1u;
}

enum x86_guest_space_status x86_guest_space_i8042_input_bind(
	kernel_object_handle_t machine_identity,
	kernel_object_handle_t source_identity,
	const struct x86_i8042_input_config *config,
	struct x86_i8042_input_binding *binding)
{
	if (machine_identity != MACHINE_ID || source_identity == 0u ||
	    source_identity == KERNEL_OBJECT_HANDLE_INVALID || config == NULL ||
	    binding == NULL || config->capabilities != X86_I8042_INPUT_KEYBOARD ||
	    !bytes_are_zero(config->reserved, ARRAY_SIZE(config->reserved)))
		return X86_GUEST_SPACE_INVALID_ARGUMENT;
	if (fake_guest.bound != 0u)
		return X86_GUEST_SPACE_INVALID_STATE;
	fake_guest.next_source_generation++;
	fake_guest.binding = (struct x86_i8042_input_binding){
		.context_identity = CONTROLLER_ID,
		.owner_identity = CONTROLLER_OWNER_ID,
		.controller_generation = fake_guest.controller_generation,
		.source_identity = source_identity,
		.source_generation = fake_guest.next_source_generation,
		.capabilities = X86_I8042_INPUT_KEYBOARD,
		.reserved = {0u},
	};
	fake_guest.mode.source_identity = source_identity;
	fake_guest.mode.controller_generation =
		fake_guest.controller_generation;
	fake_guest.mode.source_generation = fake_guest.next_source_generation;
	fake_guest.mode.mode_generation++;
	fake_guest.bound = 1u;
	fake_guest.quiesced = 0u;
	*binding = fake_guest.binding;
	return X86_GUEST_SPACE_OK;
}

enum x86_guest_space_status x86_guest_space_i8042_input_quiesce(
	kernel_object_handle_t machine_identity,
	const struct x86_i8042_input_binding *binding)
{
	if (machine_identity != MACHINE_ID)
		return X86_GUEST_SPACE_MACHINE_MISMATCH;
	if (!binding_matches(binding))
		return X86_GUEST_SPACE_STALE_BINDING;
	if (fake_guest.quiesced != 0u)
		return X86_GUEST_SPACE_INVALID_STATE;
	fake_guest.quiesced = 1u;
	return X86_GUEST_SPACE_OK;
}

enum x86_guest_space_status x86_guest_space_i8042_input_unbind(
	kernel_object_handle_t machine_identity,
	const struct x86_i8042_input_binding *binding)
{
	if (machine_identity != MACHINE_ID)
		return X86_GUEST_SPACE_MACHINE_MISMATCH;
	if (!binding_matches(binding))
		return X86_GUEST_SPACE_STALE_BINDING;
	if (fake_guest.quiesced == 0u)
		return X86_GUEST_SPACE_INVALID_STATE;
	fake_guest.bound = 0u;
	fake_guest.quiesced = 0u;
	fake_guest.delivery_pending = 0u;
	return X86_GUEST_SPACE_OK;
}

enum x86_guest_space_status x86_guest_space_i8042_input_keyboard_mode(
	const struct x86_i8042_input_binding *binding,
	struct x86_i8042_keyboard_mode *mode)
{
	if (mode == NULL)
		return X86_GUEST_SPACE_INVALID_ARGUMENT;
	if (!binding_matches(binding))
		return X86_GUEST_SPACE_STALE_BINDING;
	if (fake_guest.quiesced != 0u)
		return X86_GUEST_SPACE_INVALID_STATE;
	fake_guest.mode_query_count++;
	*mode = fake_guest.mode;
	return X86_GUEST_SPACE_OK;
}

enum x86_guest_space_status
x86_guest_space_i8042_input_inject_keyboard_sequence(
	const struct x86_i8042_input_binding *binding,
	const struct x86_i8042_keyboard_mode *mode, const uint8_t *values,
	size_t values_capacity, size_t count)
{
	struct captured_sequence *capture;
	size_t index;

	if (values == NULL || count == 0u ||
	    count > GUEST_PS2_KEYBOARD_SEQUENCE_CAPACITY ||
	    values_capacity < count)
		return X86_GUEST_SPACE_INVALID_ARGUMENT;
	if (!binding_matches(binding))
		return X86_GUEST_SPACE_STALE_BINDING;
	if (fake_guest.quiesced != 0u)
		return X86_GUEST_SPACE_INVALID_STATE;
	fake_guest.injection_attempt_count++;
	if (!mode_matches(mode))
		return X86_GUEST_SPACE_INPUT_MODE_CHANGED;
	if (fake_guest.behavior == FAKE_INJECT_CAPACITY_ONCE) {
		fake_guest.behavior = FAKE_INJECT_NORMAL;
		return X86_GUEST_SPACE_CAPACITY_EXHAUSTED;
	}
	if (fake_guest.behavior == FAKE_INJECT_MODE_CHANGE_ONCE) {
		fake_guest.behavior = FAKE_INJECT_NORMAL;
		fake_guest.mode.mode_generation++;
		fake_guest.mode.translation_enabled =
			(uint8_t)(fake_guest.mode.translation_enabled == 0u);
		return X86_GUEST_SPACE_INPUT_MODE_CHANGED;
	}
	if (fake_guest.capture_count >= ARRAY_SIZE(fake_guest.captured))
		return X86_GUEST_SPACE_CAPACITY_EXHAUSTED;
	capture = &fake_guest.captured[fake_guest.capture_count];
	*capture = (struct captured_sequence){
		.source_identity = binding->source_identity,
		.source_generation = binding->source_generation,
		.mode_generation = mode->mode_generation,
		.count = (uint8_t)count,
		.reserved = {0u},
	};
	for (index = 0u; index < count; ++index)
		capture->values[index] = values[index];
	fake_guest.capture_count++;
	if (fake_guest.behavior == FAKE_INJECT_COMMITTED_PENDING_ONCE) {
		fake_guest.behavior = FAKE_INJECT_NORMAL;
		fake_guest.delivery_pending = 1u;
		return X86_GUEST_SPACE_INPUT_COMMITTED_DELIVERY_PENDING;
	}
	return X86_GUEST_SPACE_OK;
}

enum x86_guest_space_status x86_guest_space_device_events_pump(
	kernel_object_handle_t machine_identity, size_t budget,
	size_t *processed)
{
	if (machine_identity != MACHINE_ID)
		return X86_GUEST_SPACE_MACHINE_MISMATCH;
	if (processed == NULL || budget == 0u)
		return X86_GUEST_SPACE_INVALID_ARGUMENT;
	*processed = 0u;
	fake_guest.device_pump_count++;
	if (fake_guest.delivery_pending != 0u) {
		fake_guest.delivery_pending = 0u;
		*processed = 1u;
	}
	return X86_GUEST_SPACE_OK;
}

static bool capture_is(size_t capture_index,
	kernel_object_handle_t source_identity, const uint8_t *values,
	size_t count)
{
	const struct captured_sequence *capture;
	size_t index;

	if (values == NULL || capture_index >= fake_guest.capture_count)
		return false;
	capture = &fake_guest.captured[capture_index];
	if (capture->source_identity != source_identity ||
	    capture->source_generation == 0u || capture->mode_generation == 0u ||
	    capture->count != count)
		return false;
	for (index = 0u; index < count; ++index) {
		if (capture->values[index] != values[index])
			return false;
	}
	return true;
}

static enum input_status submit_key(struct input_core *core,
	const struct input_device_binding *binding, input_key_code_t code,
	uint8_t value)
{
	return input_submit(core, binding, INPUT_EVENT_KEY, code, value,
			    (uint32_t)code, 0u);
}

static struct guest_ps2_keyboard_config keyboard_config(
	kernel_object_handle_t identity, kernel_object_handle_t context_identity,
	kernel_object_handle_t source_identity, struct input_core *core)
{
	return (struct guest_ps2_keyboard_config){
		.identity = identity,
		.context_identity = context_identity,
		.input_core_identity = CORE_ID,
		.machine_identity = MACHINE_ID,
		.source_identity = source_identity,
		.input_core = core,
		.reserved = {0u},
	};
}

static int test_set2_sequences(struct input_core *core,
	const struct input_device_binding *binding)
{
	static const uint8_t a_make[] = {0x1cu};
	static const uint8_t a_break[] = {0xf0u, 0x1cu};
	static const uint8_t shift_make[] = {0x12u};
	static const uint8_t shift_break[] = {0xf0u, 0x12u};
	static const uint8_t right_ctrl_make[] = {0xe0u, 0x14u};
	static const uint8_t right_ctrl_break[] = {0xe0u, 0xf0u, 0x14u};
	static const uint8_t print_make[] = {0xe0u, 0x12u, 0xe0u, 0x7cu};
	static const uint8_t print_break[] = {
		0xe0u, 0xf0u, 0x7cu, 0xe0u, 0xf0u, 0x12u
	};
	static const uint8_t pause_make[] = {
		0xe1u, 0x14u, 0x77u, 0xe1u, 0xf0u, 0x14u, 0xf0u, 0x77u
	};
	size_t start = fake_guest.capture_count;

	select_mode(2u, 0u);
	if (submit_key(core, binding, INPUT_KEY_CODE_A, INPUT_KEY_PRESSED) !=
		    INPUT_OK ||
	    submit_key(core, binding, INPUT_KEY_CODE_A, INPUT_KEY_REPEATED) !=
		    INPUT_OK ||
	    submit_key(core, binding, INPUT_KEY_CODE_A, INPUT_KEY_RELEASED) !=
		    INPUT_OK ||
	    submit_key(core, binding, INPUT_KEY_CODE_LEFTSHIFT,
		       INPUT_KEY_PRESSED) != INPUT_OK ||
	    submit_key(core, binding, INPUT_KEY_CODE_LEFTSHIFT,
		       INPUT_KEY_RELEASED) != INPUT_OK ||
	    submit_key(core, binding, INPUT_KEY_CODE_RIGHTCTRL,
		       INPUT_KEY_PRESSED) != INPUT_OK ||
	    submit_key(core, binding, INPUT_KEY_CODE_RIGHTCTRL,
		       INPUT_KEY_RELEASED) != INPUT_OK ||
	    submit_key(core, binding, INPUT_KEY_CODE_SYSRQ, INPUT_KEY_PRESSED) !=
		    INPUT_OK ||
	    submit_key(core, binding, INPUT_KEY_CODE_SYSRQ, INPUT_KEY_RELEASED) !=
		    INPUT_OK ||
	    submit_key(core, binding, INPUT_KEY_CODE_PAUSE, INPUT_KEY_PRESSED) !=
		    INPUT_OK ||
	    submit_key(core, binding, INPUT_KEY_CODE_PAUSE, INPUT_KEY_RELEASED) !=
		    INPUT_OK)
		return 1;
	if (fake_guest.capture_count != start + 10u ||
	    !capture_is(start + 0u, SOURCE_A_ID, a_make, ARRAY_SIZE(a_make)) ||
	    !capture_is(start + 1u, SOURCE_A_ID, a_make, ARRAY_SIZE(a_make)) ||
	    !capture_is(start + 2u, SOURCE_A_ID, a_break,
			ARRAY_SIZE(a_break)) ||
	    !capture_is(start + 3u, SOURCE_A_ID, shift_make,
			ARRAY_SIZE(shift_make)) ||
	    !capture_is(start + 4u, SOURCE_A_ID, shift_break,
			ARRAY_SIZE(shift_break)) ||
	    !capture_is(start + 5u, SOURCE_A_ID, right_ctrl_make,
			ARRAY_SIZE(right_ctrl_make)) ||
	    !capture_is(start + 6u, SOURCE_A_ID, right_ctrl_break,
			ARRAY_SIZE(right_ctrl_break)) ||
	    !capture_is(start + 7u, SOURCE_A_ID, print_make,
			ARRAY_SIZE(print_make)) ||
	    !capture_is(start + 8u, SOURCE_A_ID, print_break,
			ARRAY_SIZE(print_break)) ||
	    !capture_is(start + 9u, SOURCE_A_ID, pause_make,
			ARRAY_SIZE(pause_make)))
		return 2;
	return 0;
}

static int test_set1_sequences(struct input_core *core,
	const struct input_device_binding *binding)
{
	static const uint8_t a_make[] = {0x1eu};
	static const uint8_t a_break[] = {0x9eu};
	static const uint8_t shift_make[] = {0x2au};
	static const uint8_t shift_break[] = {0xaau};
	static const uint8_t right_ctrl_make[] = {0xe0u, 0x1du};
	static const uint8_t right_ctrl_break[] = {0xe0u, 0x9du};
	static const uint8_t print_make[] = {0xe0u, 0x2au, 0xe0u, 0x37u};
	static const uint8_t print_break[] = {0xe0u, 0xb7u, 0xe0u, 0xaau};
	static const uint8_t pause_make[] = {
		0xe1u, 0x1du, 0x45u, 0xe1u, 0x9du, 0xc5u
	};
	size_t start = fake_guest.capture_count;

	/* Controller translation converts a set-2 keyboard into set-1 bytes. */
	select_mode(2u, 1u);
	if (submit_key(core, binding, INPUT_KEY_CODE_A, INPUT_KEY_PRESSED) !=
		    INPUT_OK ||
	    submit_key(core, binding, INPUT_KEY_CODE_A, INPUT_KEY_RELEASED) !=
		    INPUT_OK ||
	    submit_key(core, binding, INPUT_KEY_CODE_LEFTSHIFT,
		       INPUT_KEY_PRESSED) != INPUT_OK ||
	    submit_key(core, binding, INPUT_KEY_CODE_LEFTSHIFT,
		       INPUT_KEY_RELEASED) != INPUT_OK ||
	    submit_key(core, binding, INPUT_KEY_CODE_RIGHTCTRL,
		       INPUT_KEY_PRESSED) != INPUT_OK ||
	    submit_key(core, binding, INPUT_KEY_CODE_RIGHTCTRL,
		       INPUT_KEY_RELEASED) != INPUT_OK ||
	    submit_key(core, binding, INPUT_KEY_CODE_SYSRQ, INPUT_KEY_PRESSED) !=
		    INPUT_OK ||
	    submit_key(core, binding, INPUT_KEY_CODE_SYSRQ, INPUT_KEY_RELEASED) !=
		    INPUT_OK ||
	    submit_key(core, binding, INPUT_KEY_CODE_PAUSE, INPUT_KEY_PRESSED) !=
		    INPUT_OK)
		return 1;
	if (fake_guest.capture_count != start + 9u ||
	    !capture_is(start + 0u, SOURCE_A_ID, a_make, ARRAY_SIZE(a_make)) ||
	    !capture_is(start + 1u, SOURCE_A_ID, a_break,
			ARRAY_SIZE(a_break)) ||
	    !capture_is(start + 2u, SOURCE_A_ID, shift_make,
			ARRAY_SIZE(shift_make)) ||
	    !capture_is(start + 3u, SOURCE_A_ID, shift_break,
			ARRAY_SIZE(shift_break)) ||
	    !capture_is(start + 4u, SOURCE_A_ID, right_ctrl_make,
			ARRAY_SIZE(right_ctrl_make)) ||
	    !capture_is(start + 5u, SOURCE_A_ID, right_ctrl_break,
			ARRAY_SIZE(right_ctrl_break)) ||
	    !capture_is(start + 6u, SOURCE_A_ID, print_make,
			ARRAY_SIZE(print_make)) ||
	    !capture_is(start + 7u, SOURCE_A_ID, print_break,
			ARRAY_SIZE(print_break)) ||
	    !capture_is(start + 8u, SOURCE_A_ID, pause_make,
			ARRAY_SIZE(pause_make)))
		return 2;
	return 0;
}

static int test_named_map_edges(struct input_core *core,
	const struct input_device_binding *binding)
{
	static const uint8_t ro_set2[] = {0x51u};
	static const uint8_t search_set2[] = {0xe0u, 0x10u};
	static const uint8_t jp_comma_set2[] = {0x27u};
	static const uint8_t ro_set1[] = {0x73u};
	static const uint8_t search_set1[] = {0xe0u, 0x65u};
	static const uint8_t jp_comma_set1[] = {0x5cu};
	size_t start = fake_guest.capture_count;

	select_mode(2u, 0u);
	if (submit_key(core, binding, INPUT_KEY_CODE_RO,
		       INPUT_KEY_PRESSED) != INPUT_OK ||
	    submit_key(core, binding, INPUT_KEY_CODE_SEARCH,
		       INPUT_KEY_PRESSED) != INPUT_OK ||
	    submit_key(core, binding, INPUT_KEY_CODE_KPJPCOMMA,
		       INPUT_KEY_PRESSED) != INPUT_OK)
		return 1;
	select_mode(2u, 1u);
	if (submit_key(core, binding, INPUT_KEY_CODE_RO,
		       INPUT_KEY_PRESSED) != INPUT_OK ||
	    submit_key(core, binding, INPUT_KEY_CODE_SEARCH,
		       INPUT_KEY_PRESSED) != INPUT_OK ||
	    submit_key(core, binding, INPUT_KEY_CODE_KPJPCOMMA,
		       INPUT_KEY_PRESSED) != INPUT_OK)
		return 2;
	if (fake_guest.capture_count != start + 6u ||
	    !capture_is(start + 0u, SOURCE_A_ID, ro_set2,
			ARRAY_SIZE(ro_set2)) ||
	    !capture_is(start + 1u, SOURCE_A_ID, search_set2,
			ARRAY_SIZE(search_set2)) ||
	    !capture_is(start + 2u, SOURCE_A_ID, jp_comma_set2,
			ARRAY_SIZE(jp_comma_set2)) ||
	    !capture_is(start + 3u, SOURCE_A_ID, ro_set1,
			ARRAY_SIZE(ro_set1)) ||
	    !capture_is(start + 4u, SOURCE_A_ID, search_set1,
			ARRAY_SIZE(search_set1)) ||
	    !capture_is(start + 5u, SOURCE_A_ID, jp_comma_set1,
			ARRAY_SIZE(jp_comma_set1)))
		return 3;
	return 0;
}

static int run_guest_ps2_keyboard_test(void)
{
	static struct input_core core;
	static struct input_device device;
	static struct input_device *devices[1];
	static struct input_handler *handlers[2];
	static struct input_event event_queue[4];
	static struct guest_ps2_keyboard keyboard_a;
	static struct guest_ps2_keyboard keyboard_b;
	const struct input_core_config core_config = {
		.identity = CORE_ID,
		.guard_context = KERNEL_OBJECT_HANDLE_INVALID,
		.irq_enter = NULL,
		.irq_exit = NULL,
		.caller_serializes_irq = 1u,
		.reserved = {0u},
	};
	const struct input_device_config device_config = {
		.identity = DEVICE_ID,
		.capabilities = INPUT_CAPABILITY_KEY,
		.queue = event_queue,
		.queue_capacity = ARRAY_SIZE(event_queue),
		.reserved = {0u},
	};
	struct guest_ps2_keyboard_config config_a;
	struct guest_ps2_keyboard_config config_b;
	struct guest_ps2_keyboard_reference reference_a;
	struct guest_ps2_keyboard_reference reference_b;
	struct guest_ps2_keyboard_reference replacement_reference;
	struct guest_ps2_keyboard_snapshot keyboard_snapshot;
	struct input_device_binding device_binding;
	struct input_device_snapshot device_snapshot;
	static const uint8_t b_set2[] = {0x32u};
	static const uint8_t c_set2[] = {0x21u};
	static const uint8_t d_set1[] = {0x20u};
	static const uint8_t e_set1[] = {0x12u};
	static const uint8_t g_set2[] = {0x34u};
	size_t capture_start;
	size_t injection_attempt_start;
	size_t processed;
	uint16_t delivered;
	int sequence_result;

	fake_guest = (struct fake_guest_keyboard){
		.controller_generation = 7u,
		.next_source_generation = 10u,
		.mode = {
			.mode_generation = 1u,
			.scan_set = 2u,
			.translation_enabled = 0u,
			.scanning_enabled = 1u,
			.interface_enabled = 1u,
			.reserved = {0u},
		},
	};
	input_core_construct(&core);
	input_device_construct(&device);
	guest_ps2_keyboard_construct(&keyboard_a);
	guest_ps2_keyboard_construct(&keyboard_b);
	config_a = keyboard_config(HANDLER_A_ID, CONTEXT_A_ID, SOURCE_A_ID,
				   &core);
	config_b = keyboard_config(HANDLER_B_ID, CONTEXT_B_ID, SOURCE_B_ID,
				   &core);
	if (input_core_initialize(&core, &core_config, devices,
				  ARRAY_SIZE(devices), handlers,
				  ARRAY_SIZE(handlers)) != INPUT_OK ||
	    input_device_register(&core, &device, &device_config,
				  &device_binding) != INPUT_OK ||
	    guest_ps2_keyboard_register(&keyboard_a, &config_a, &reference_a) !=
		    GUEST_PS2_KEYBOARD_OK ||
	    guest_ps2_keyboard_register(&keyboard_b, &config_b, &reference_b) !=
		    GUEST_PS2_KEYBOARD_OK ||
	    guest_ps2_keyboard_focus(&keyboard_a, &reference_a) !=
		    GUEST_PS2_KEYBOARD_OK ||
	    input_core_publish(&core, CORE_ID) != INPUT_OK)
		return 1;
	sequence_result = test_set2_sequences(&core, &device_binding);
	if (sequence_result != 0)
		return 10 + sequence_result;
	sequence_result = test_set1_sequences(&core, &device_binding);
	if (sequence_result != 0)
		return 20 + sequence_result;
	sequence_result = test_named_map_edges(&core, &device_binding);
	if (sequence_result != 0)
		return 25 + sequence_result;

	/* Set 3 is explicitly unsupported: no bytes are guessed or committed. */
	capture_start = fake_guest.capture_count;
	select_mode(3u, 0u);
	if (submit_key(&core, &device_binding, INPUT_KEY_CODE_A,
		       INPUT_KEY_PRESSED) != INPUT_ACCESS_DENIED ||
	    fake_guest.capture_count != capture_start)
		return 30;
	select_mode(3u, 1u);
	if (submit_key(&core, &device_binding, INPUT_KEY_CODE_A,
		       INPUT_KEY_PRESSED) != INPUT_ACCESS_DENIED ||
	    fake_guest.capture_count != capture_start)
		return 30;

	/* A zero-commit capacity failure owns the FIFO head; later events append
	 * and cannot overtake it when downstream becomes writable. */
	select_mode(2u, 0u);
	capture_start = fake_guest.capture_count;
	fake_guest.behavior = FAKE_INJECT_CAPACITY_ONCE;
	if (submit_key(&core, &device_binding, INPUT_KEY_CODE_B,
		       INPUT_KEY_PRESSED) != INPUT_DEFERRED ||
	    submit_key(&core, &device_binding, INPUT_KEY_CODE_C,
		       INPUT_KEY_PRESSED) != INPUT_DEFERRED ||
	    fake_guest.capture_count != capture_start || device.queue_count != 2u)
		return 31;
	delivered = 0u;
	if (input_device_pump(&core, &device_binding, 1u, &delivered) != INPUT_OK ||
	    delivered != 1u || device.queue_count != 1u ||
	    !capture_is(capture_start, SOURCE_A_ID, b_set2,
			ARRAY_SIZE(b_set2)))
		return 32;
	delivered = 0u;
	if (input_device_pump(&core, &device_binding, 1u, &delivered) != INPUT_OK ||
	    delivered != 1u || device.queue_count != 0u ||
	    !capture_is(capture_start + 1u, SOURCE_A_ID, c_set2,
			ARRAY_SIZE(c_set2)))
		return 33;

	/* A mode-epoch race commits nothing. The queued event is re-encoded
	 * against the new translated mode and injected exactly once. */
	select_mode(2u, 0u);
	capture_start = fake_guest.capture_count;
	injection_attempt_start = fake_guest.injection_attempt_count;
	fake_guest.behavior = FAKE_INJECT_MODE_CHANGE_ONCE;
	if (submit_key(&core, &device_binding, INPUT_KEY_CODE_D,
		       INPUT_KEY_PRESSED) != INPUT_DEFERRED ||
	    fake_guest.capture_count != capture_start || device.queue_count != 1u)
		return 34;
	delivered = 0u;
	if (input_device_pump(&core, &device_binding, 1u, &delivered) != INPUT_OK ||
	    delivered != 1u || fake_guest.capture_count != capture_start + 1u ||
	    fake_guest.injection_attempt_count != injection_attempt_start + 2u ||
	    !capture_is(capture_start, SOURCE_A_ID, d_set1,
			ARRAY_SIZE(d_set1)))
		return 35;

	/* A committed sequence stays handled even when central IRQ delivery is
	 * pending. Pumping may route it, but must never inject those bytes twice. */
	capture_start = fake_guest.capture_count;
	injection_attempt_start = fake_guest.injection_attempt_count;
	fake_guest.behavior = FAKE_INJECT_COMMITTED_PENDING_ONCE;
	if (submit_key(&core, &device_binding, INPUT_KEY_CODE_E,
		       INPUT_KEY_PRESSED) != INPUT_OK ||
	    fake_guest.capture_count != capture_start + 1u ||
	    fake_guest.injection_attempt_count != injection_attempt_start + 1u ||
	    device.queue_count != 0u || fake_guest.delivery_pending == 0u ||
	    !capture_is(capture_start, SOURCE_A_ID, e_set1,
			ARRAY_SIZE(e_set1)))
		return 36;
	processed = 0u;
	if (guest_ps2_keyboard_pump(&keyboard_a, &reference_a, 1u,
				    &processed) != GUEST_PS2_KEYBOARD_OK ||
	    processed != 1u || fake_guest.capture_count != capture_start + 1u ||
	    fake_guest.injection_attempt_count != injection_attempt_start + 1u ||
	    fake_guest.delivery_pending != 0u)
		return 37;

	/* A deferred item from the old focus is stale after unbind/rebind. The
	 * new-focus event remains ordered behind it, then survives the stale-head
	 * discard without leaking the old key into the new guest source. */
	select_mode(2u, 0u);
	capture_start = fake_guest.capture_count;
	fake_guest.behavior = FAKE_INJECT_CAPACITY_ONCE;
	if (submit_key(&core, &device_binding, INPUT_KEY_CODE_F,
		       INPUT_KEY_PRESSED) != INPUT_DEFERRED ||
	    guest_ps2_keyboard_unfocus(&keyboard_a, &reference_a) !=
		    GUEST_PS2_KEYBOARD_OK ||
	    guest_ps2_keyboard_focus(&keyboard_b, &reference_b) !=
		    GUEST_PS2_KEYBOARD_OK ||
	    submit_key(&core, &device_binding, INPUT_KEY_CODE_G,
		       INPUT_KEY_PRESSED) != INPUT_DEFERRED ||
	    fake_guest.capture_count != capture_start || device.queue_count != 2u)
		return 38;
	delivered = 0u;
	if (input_device_pump(&core, &device_binding, 1u, &delivered) != INPUT_OK ||
	    delivered != 1u || fake_guest.capture_count != capture_start + 1u ||
	    !capture_is(capture_start, SOURCE_B_ID, g_set2,
			ARRAY_SIZE(g_set2)) ||
	    input_device_snapshot(&device, &device_snapshot) != INPUT_OK ||
	    device_snapshot.queue_count != 0u ||
	    device_snapshot.stale_focus_drop_count != 1u)
		return 39;
	if (guest_ps2_keyboard_snapshot(&keyboard_a, &reference_a,
					&keyboard_snapshot) !=
		    GUEST_PS2_KEYBOARD_OK ||
	    keyboard_snapshot.focus_bind_count != 1u ||
	    keyboard_snapshot.focus_unbind_count != 1u ||
	    keyboard_snapshot.downstream_bound != 0u ||
	    keyboard_snapshot.phase != GUEST_PS2_KEYBOARD_REGISTERED)
		return 40;
	if (guest_ps2_keyboard_snapshot(&keyboard_b, &reference_b,
					&keyboard_snapshot) !=
		    GUEST_PS2_KEYBOARD_OK ||
	    keyboard_snapshot.focus_bind_count != 1u ||
	    keyboard_snapshot.downstream_bound == 0u ||
	    keyboard_snapshot.phase != GUEST_PS2_KEYBOARD_FOCUSED)
		return 41;

	if (guest_ps2_keyboard_unfocus(&keyboard_b, &reference_b) !=
		    GUEST_PS2_KEYBOARD_OK ||
	    input_core_quiesce(&core, CORE_ID) != INPUT_OK ||
	    input_device_quiesce(&core, &device_binding) != INPUT_OK ||
	    guest_ps2_keyboard_quiesce(&keyboard_a, &reference_a) !=
		    GUEST_PS2_KEYBOARD_OK ||
	    guest_ps2_keyboard_quiesce(&keyboard_b, &reference_b) !=
		    GUEST_PS2_KEYBOARD_OK ||
	    input_device_unregister(&core, &device_binding) != INPUT_OK ||
	    guest_ps2_keyboard_unregister(&keyboard_a, &reference_a) !=
		    GUEST_PS2_KEYBOARD_OK ||
	    guest_ps2_keyboard_unregister(&keyboard_b, &reference_b) !=
		    GUEST_PS2_KEYBOARD_OK)
		return 42;

	/* Reusing the caller-owned object advances its generation. */
	if (guest_ps2_keyboard_register(&keyboard_a, &config_a,
					&replacement_reference) !=
		    GUEST_PS2_KEYBOARD_OK ||
	    replacement_reference.generation == reference_a.generation ||
	    guest_ps2_keyboard_snapshot(&keyboard_a, &reference_a,
					&keyboard_snapshot) !=
		    GUEST_PS2_KEYBOARD_STALE_REFERENCE ||
	    guest_ps2_keyboard_quiesce(&keyboard_a, &replacement_reference) !=
		    GUEST_PS2_KEYBOARD_OK ||
	    guest_ps2_keyboard_unregister(&keyboard_a,
					   &replacement_reference) !=
		    GUEST_PS2_KEYBOARD_OK ||
	    input_core_retire(&core, CORE_ID) != INPUT_OK)
		return 43;
	return 0;
}

DOSC32_TEST_ENTRY(run_guest_ps2_keyboard_test)

// SPDX-License-Identifier: GPL-2.0-only
/* Host-safe native console consumer, keymap and FIFO lifecycle tests. */
#include "console.h"
#include "input_keycodes.h"
#include "keyboard.h"
#include "test_entry.h"

#define CORE_ID ((kernel_object_handle_t)0x4b42434f52454944ull)
#define DEVICE_ID ((kernel_object_handle_t)0x4b42444556494345ull)
#define CONSOLE_ID ((kernel_object_handle_t)0x4b42434f4e534f4cull)

static struct input_core core;
static struct input_device device;
static struct input_device *devices[2];
static struct input_handler *handlers[2];
static struct input_event device_queue[8];
static struct input_device_binding device_binding;
static struct keyboard_console console;
static struct keyboard_key_record console_queue[3];

struct wait_fixture {
	uint32_t calls;
	uint8_t submitted;
	enum input_status status;
};

static struct wait_fixture wait_fixture;
static uint32_t hardware_code;

void console_putc(char character)
{
	(void)character;
}

void console_backspace(void)
{
}

static enum input_status submit(input_key_code_t code, uint8_t value,
				uint8_t flags)
{
	hardware_code++;
	return input_submit(&core, &device_binding, INPUT_EVENT_KEY, code, value,
			    hardware_code, flags);
}

static void test_wait(void *context)
{
	struct wait_fixture *fixture = context;

	fixture->calls++;
	if (fixture->submitted != 0u)
		return;
	fixture->submitted = 1u;
	fixture->status = submit(INPUT_KEY_CODE_A, INPUT_KEY_PRESSED, 0u);
}

static int setup(void)
{
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
		.queue = device_queue,
		.queue_capacity = ARRAY_SIZE(device_queue),
		.reserved = {0u},
	};
	const struct keyboard_console_config console_config = {
		.identity = CONSOLE_ID,
		.core_identity = CORE_ID,
		.queue = console_queue,
		.wait = test_wait,
		.wait_context = &wait_fixture,
		.queue_capacity = ARRAY_SIZE(console_queue),
		.reserved = {0u},
	};

	keyboard_init();
	input_core_construct(&core);
	input_device_construct(&device);
	keyboard_console_construct(&console);
	if (input_core_initialize(&core, &core_config, devices,
				  ARRAY_SIZE(devices), handlers,
				  ARRAY_SIZE(handlers)) != INPUT_OK)
		return 1;
	if (input_device_register(&core, &device, &device_config,
				  &device_binding) != INPUT_OK)
		return 2;
	if (keyboard_console_register(&console, &core, &console_config) !=
	    INPUT_OK)
		return 3;
	if (keyboard_console_focus(&console) != INPUT_OK ||
	    input_core_publish(&core, CORE_ID) != INPUT_OK)
		return 4;
	if (keyboard_default_bind(&console, CONSOLE_ID,
				  console.generation + 1u) !=
		    INPUT_STALE_BINDING ||
	    keyboard_default_bind(&console, CONSOLE_ID, console.generation) !=
		    INPUT_OK ||
	    keyboard_default_console() != &console)
		return 5;
	return 0;
}

static bool read_bios(uint16_t expected)
{
	uint16_t key = 0xffffu;

	return keyboard_console_read_bios_key(&console, &key) &&
	       key == expected;
}

static int test_basic_and_modifiers(void)
{
	char character = '\0';
	uint16_t key = 0u;
	uint16_t flags;

	if (submit(INPUT_KEY_CODE_A, INPUT_KEY_PRESSED, 0u) != INPUT_OK ||
	    !keyboard_console_peek_character(&console, &character) ||
	    character != 'a' ||
	    !keyboard_console_peek_bios_key(&console, &key) || key != 0x1e61u ||
	    !keyboard_console_read_character(&console, &character) ||
	    character != 'a' ||
	    !keyboard_bios_key_from_character(character, &key) ||
	    key != 0x1e61u ||
	    submit(INPUT_KEY_CODE_A, INPUT_KEY_RELEASED, 0u) != INPUT_OK)
		return 1;

	if (submit(INPUT_KEY_CODE_LEFTSHIFT, INPUT_KEY_PRESSED, 0u) != INPUT_OK)
		return 2;
	flags = keyboard_console_bios_shift_flags(&console);
	if ((flags & (1u << 1)) == 0u ||
	    submit(INPUT_KEY_CODE_A, INPUT_KEY_PRESSED, 0u) != INPUT_OK ||
	    submit(INPUT_KEY_CODE_A, INPUT_KEY_REPEATED, 0u) != INPUT_OK ||
	    submit(INPUT_KEY_CODE_A, INPUT_KEY_RELEASED, 0u) != INPUT_OK ||
	    submit(INPUT_KEY_CODE_LEFTSHIFT, INPUT_KEY_RELEASED, 0u) != INPUT_OK ||
	    !read_bios(0x1e41u) || !read_bios(0x1e41u))
		return 3;

	if (submit(INPUT_KEY_CODE_CAPSLOCK, INPUT_KEY_PRESSED, 0u) != INPUT_OK ||
	    submit(INPUT_KEY_CODE_CAPSLOCK, INPUT_KEY_REPEATED, 0u) != INPUT_OK ||
	    submit(INPUT_KEY_CODE_CAPSLOCK, INPUT_KEY_RELEASED, 0u) != INPUT_OK ||
	    (keyboard_console_bios_shift_flags(&console) & (1u << 6)) == 0u ||
	    submit(INPUT_KEY_CODE_A, INPUT_KEY_PRESSED, 0u) != INPUT_OK ||
	    !read_bios(0x1e41u) ||
	    submit(INPUT_KEY_CODE_A, INPUT_KEY_RELEASED, 0u) != INPUT_OK ||
	    submit(INPUT_KEY_CODE_LEFTSHIFT, INPUT_KEY_PRESSED, 0u) != INPUT_OK ||
	    submit(INPUT_KEY_CODE_A, INPUT_KEY_PRESSED, 0u) != INPUT_OK ||
	    !read_bios(0x1e61u) ||
	    submit(INPUT_KEY_CODE_A, INPUT_KEY_RELEASED, 0u) != INPUT_OK ||
	    submit(INPUT_KEY_CODE_LEFTSHIFT, INPUT_KEY_RELEASED, 0u) != INPUT_OK)
		return 4;
	if (submit(INPUT_KEY_CODE_CAPSLOCK, INPUT_KEY_PRESSED, 0u) != INPUT_OK ||
	    submit(INPUT_KEY_CODE_CAPSLOCK, INPUT_KEY_RELEASED, 0u) != INPUT_OK ||
	    (keyboard_console_bios_shift_flags(&console) & (1u << 6)) != 0u)
		return 5;

	if (submit(INPUT_KEY_CODE_LEFTCTRL, INPUT_KEY_PRESSED, 0u) != INPUT_OK)
		return 6;
	flags = keyboard_console_bios_shift_flags(&console);
	if ((flags & (1u << 2)) == 0u || (flags & (1u << 8)) == 0u ||
	    submit(INPUT_KEY_CODE_C, INPUT_KEY_PRESSED, 0u) != INPUT_OK ||
	    !read_bios(0x2e03u) ||
	    submit(INPUT_KEY_CODE_C, INPUT_KEY_RELEASED, 0u) != INPUT_OK ||
	    submit(INPUT_KEY_CODE_LEFTCTRL, INPUT_KEY_RELEASED, 0u) != INPUT_OK)
		return 7;

	if (submit(INPUT_KEY_CODE_RIGHTALT, INPUT_KEY_PRESSED,
		   INPUT_EVENT_EXTENDED) != INPUT_OK)
		return 8;
	flags = keyboard_console_bios_shift_flags(&console);
	if ((flags & (1u << 3)) == 0u || (flags & (1u << 9)) != 0u ||
	    submit(INPUT_KEY_CODE_F1, INPUT_KEY_PRESSED, 0u) != INPUT_OK ||
	    !read_bios(0x6800u) ||
	    submit(INPUT_KEY_CODE_F1, INPUT_KEY_RELEASED, 0u) != INPUT_OK ||
	    submit(INPUT_KEY_CODE_RIGHTALT, INPUT_KEY_RELEASED,
		   INPUT_EVENT_EXTENDED) != INPUT_OK)
		return 9;
	if (submit(INPUT_KEY_CODE_LEFTALT, INPUT_KEY_PRESSED, 0u) != INPUT_OK ||
	    (keyboard_console_bios_shift_flags(&console) & (1u << 9)) == 0u ||
	    submit(INPUT_KEY_CODE_LEFTALT, INPUT_KEY_RELEASED, 0u) != INPUT_OK)
		return 10;

	if (submit(INPUT_KEY_CODE_LEFTSHIFT, INPUT_KEY_PRESSED, 0u) != INPUT_OK ||
	    submit(INPUT_KEY_CODE_F11, INPUT_KEY_PRESSED, 0u) != INPUT_OK ||
	    !read_bios(0x8700u) ||
	    submit(INPUT_KEY_CODE_F11, INPUT_KEY_RELEASED, 0u) != INPUT_OK ||
	    submit(INPUT_KEY_CODE_LEFTSHIFT, INPUT_KEY_RELEASED, 0u) != INPUT_OK ||
	    submit(INPUT_KEY_CODE_LEFT, INPUT_KEY_PRESSED,
		   INPUT_EVENT_EXTENDED) != INPUT_OK ||
	    !read_bios(0x4b00u) ||
	    submit(INPUT_KEY_CODE_LEFT, INPUT_KEY_RELEASED,
		   INPUT_EVENT_EXTENDED) != INPUT_OK)
		return 11;
	return 0;
}

static int test_keypad_insert_and_flush(void)
{
	struct keyboard_console_snapshot snapshot;
	char character = '\0';
	uint16_t flags;

	if (submit(INPUT_KEY_CODE_NUMLOCK, INPUT_KEY_PRESSED, 0u) != INPUT_OK ||
	    submit(INPUT_KEY_CODE_NUMLOCK, INPUT_KEY_REPEATED, 0u) != INPUT_OK ||
	    submit(INPUT_KEY_CODE_NUMLOCK, INPUT_KEY_RELEASED, 0u) != INPUT_OK ||
	    submit(INPUT_KEY_CODE_KP7, INPUT_KEY_PRESSED, 0u) != INPUT_OK ||
	    !read_bios(0x4737u) ||
	    submit(INPUT_KEY_CODE_KP7, INPUT_KEY_RELEASED, 0u) != INPUT_OK ||
	    submit(INPUT_KEY_CODE_LEFTSHIFT, INPUT_KEY_PRESSED, 0u) != INPUT_OK ||
	    submit(INPUT_KEY_CODE_KP7, INPUT_KEY_PRESSED, 0u) != INPUT_OK ||
	    !read_bios(0x4700u) ||
	    submit(INPUT_KEY_CODE_KP7, INPUT_KEY_RELEASED, 0u) != INPUT_OK ||
	    submit(INPUT_KEY_CODE_LEFTSHIFT, INPUT_KEY_RELEASED, 0u) != INPUT_OK ||
	    submit(INPUT_KEY_CODE_NUMLOCK, INPUT_KEY_PRESSED, 0u) != INPUT_OK ||
	    submit(INPUT_KEY_CODE_NUMLOCK, INPUT_KEY_RELEASED, 0u) != INPUT_OK)
		return 1;

	if (submit(INPUT_KEY_CODE_INSERT, INPUT_KEY_PRESSED,
		   INPUT_EVENT_EXTENDED) != INPUT_OK)
		return 2;
	flags = keyboard_console_bios_shift_flags(&console);
	if ((flags & (1u << 7)) == 0u || (flags & (1u << 15)) == 0u ||
	    submit(INPUT_KEY_CODE_INSERT, INPUT_KEY_REPEATED,
		   INPUT_EVENT_EXTENDED) != INPUT_OK ||
	    !read_bios(0x5200u) || !read_bios(0x5200u) ||
	    submit(INPUT_KEY_CODE_INSERT, INPUT_KEY_RELEASED,
		   INPUT_EVENT_EXTENDED) != INPUT_OK)
		return 3;
	flags = keyboard_console_bios_shift_flags(&console);
	if ((flags & (1u << 7)) == 0u || (flags & (1u << 15)) != 0u)
		return 4;
	if (submit(INPUT_KEY_CODE_INSERT, INPUT_KEY_PRESSED,
		   INPUT_EVENT_EXTENDED) != INPUT_OK ||
	    !read_bios(0x5200u) ||
	    submit(INPUT_KEY_CODE_INSERT, INPUT_KEY_RELEASED,
		   INPUT_EVENT_EXTENDED) != INPUT_OK ||
	    (keyboard_console_bios_shift_flags(&console) & (1u << 7)) != 0u)
		return 5;

	if (submit(INPUT_KEY_CODE_LEFT, INPUT_KEY_PRESSED,
		   INPUT_EVENT_EXTENDED) != INPUT_OK ||
	    submit(INPUT_KEY_CODE_B, INPUT_KEY_PRESSED, 0u) != INPUT_OK ||
	    !keyboard_console_read_character(&console, &character) ||
	    character != 'b' || keyboard_console_read_bios_key(&console, &flags))
		return 6;
	if (submit(INPUT_KEY_CODE_VOLUMEUP, INPUT_KEY_PRESSED,
		   INPUT_EVENT_EXTENDED) != INPUT_OK ||
	    keyboard_console_snapshot(&console, &snapshot) != INPUT_OK ||
	    snapshot.unsupported_count == 0u || snapshot.queued_keys != 0u)
		return 7;
	if (submit(INPUT_KEY_CODE_A, INPUT_KEY_PRESSED, 0u) != INPUT_OK)
		return 8;
	keyboard_console_flush(&console);
	if (keyboard_console_snapshot(&console, &snapshot) != INPUT_OK ||
	    snapshot.queued_keys != 0u)
		return 9;
	return 0;
}

static int test_backpressure_and_pump(void)
{
	struct keyboard_console_snapshot snapshot;
	uint16_t delivered = 0u;

	keyboard_console_flush(&console);
	if (submit(INPUT_KEY_CODE_A, INPUT_KEY_PRESSED, 0u) != INPUT_OK ||
	    submit(INPUT_KEY_CODE_B, INPUT_KEY_PRESSED, 0u) != INPUT_OK ||
	    submit(INPUT_KEY_CODE_C, INPUT_KEY_PRESSED, 0u) != INPUT_OK ||
	    submit(INPUT_KEY_CODE_D, INPUT_KEY_PRESSED, 0u) != INPUT_DEFERRED ||
	    submit(INPUT_KEY_CODE_E, INPUT_KEY_PRESSED, 0u) != INPUT_DEFERRED ||
	    device.queue_count != 2u || !read_bios(0x1e61u))
		return 1;
	if (keyboard_console_pump(&console, &device_binding, 1u, &delivered) !=
		    INPUT_OK ||
	    delivered != 1u || device.queue_count != 1u)
		return 2;
	delivered = 0u;
	if (keyboard_console_pump(&console, &device_binding, 1u, &delivered) !=
		    INPUT_RETRY ||
	    delivered != 0u || device.queue_count != 1u ||
	    !read_bios(0x3062u))
		return 3;
	if (keyboard_console_pump(&console, &device_binding, 1u, &delivered) !=
		    INPUT_OK ||
	    delivered != 1u || device.queue_count != 0u ||
	    !read_bios(0x2e63u) || !read_bios(0x2064u) ||
	    !read_bios(0x1265u))
		return 4;
	if (keyboard_console_snapshot(&console, &snapshot) != INPUT_OK ||
	    snapshot.backpressure_count < 2u || snapshot.queued_keys != 0u)
		return 5;
	return 0;
}

static int test_wait_focus_and_teardown(void)
{
	struct keyboard_console_snapshot snapshot;
	char character = '\0';
	struct input_handler_binding old_binding = console.binding;

	wait_fixture = (struct wait_fixture){0u, 0u, INPUT_INVALID_STATE};
	if (!keyboard_console_wait_character(&console, &character) ||
	    character != 'a' || wait_fixture.calls != 1u ||
	    wait_fixture.status != INPUT_OK)
		return 1;
	if (submit(INPUT_KEY_CODE_RIGHTSHIFT, INPUT_KEY_PRESSED, 0u) != INPUT_OK ||
	    submit(INPUT_KEY_CODE_A, INPUT_KEY_PRESSED, 0u) != INPUT_OK ||
	    keyboard_console_unfocus(&console) != INPUT_OK ||
	    keyboard_console_snapshot(&console, &snapshot) != INPUT_OK ||
	    snapshot.phase != KEYBOARD_CONSOLE_REGISTERED ||
	    snapshot.queued_keys != 0u ||
	    (snapshot.bios_shift_flags & 0x0003u) != 0u ||
	    submit(INPUT_KEY_CODE_A, INPUT_KEY_PRESSED, 0u) != INPUT_UNAVAILABLE ||
	    keyboard_console_focus(&console) != INPUT_OK ||
	    console.focus_generation == old_binding.handler_generation)
		return 2;
	if (keyboard_default_unbind(&console, CONSOLE_ID,
				    console.generation + 1u) !=
		    INPUT_STALE_BINDING ||
	    keyboard_default_unbind(&console, CONSOLE_ID, console.generation) !=
		    INPUT_OK ||
	    keyboard_default_console() != NULL ||
	    input_core_quiesce(&core, CORE_ID) != INPUT_OK ||
	    keyboard_console_unfocus(&console) != INPUT_OK ||
	    keyboard_console_unregister(&console) != INPUT_OK ||
	    input_device_quiesce(&core, &device_binding) != INPUT_OK ||
	    input_device_unregister(&core, &device_binding) != INPUT_OK ||
	    input_core_retire(&core, CORE_ID) != INPUT_OK ||
	    keyboard_console_snapshot(&console, &snapshot) != INPUT_OK ||
	    snapshot.phase != KEYBOARD_CONSOLE_EMPTY || snapshot.generation != 1u)
		return 3;
	return 0;
}

static int run_tests(void)
{
	int status = setup();

	if (status != 0)
		return 10 + status;
	status = test_basic_and_modifiers();
	if (status != 0)
		return 30 + status;
	status = test_keypad_insert_and_flush();
	if (status != 0)
		return 60 + status;
	status = test_backpressure_and_pump();
	if (status != 0)
		return 90 + status;
	status = test_wait_focus_and_teardown();
	if (status != 0)
		return 120 + status;
	return 0;
}

DOSC32_TEST_ENTRY(run_tests)

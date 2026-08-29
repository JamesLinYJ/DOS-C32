// SPDX-License-Identifier: GPL-2.0-only
/*
 * Native console input consumer
 *
 * Compatibility contract: retain one BIOS scan/ASCII word per decoded make/repeat and
 *                 expose the BIOS keyboard shift/lock state
 * Safety changes: input-core events replace controller polling; an IRQ-safe
 *                 bounded SPSC FIFO defers before mutation when full, while
 *                 waits and screen output remain process-context operations
 */
#include "keyboard.h"

#include "console.h"
#include "input/console/keymap.h"
#include "input_keycodes.h"

#define KEYBOARD_CONSOLE_COOKIE 0x4b42434fu
#define KEYBOARD_GENERATION_MAX ((uint64_t)-2)
struct keyboard_default_reference {
	struct keyboard_console *console;
	kernel_object_handle_t identity;
	uint64_t generation;
};

static struct keyboard_default_reference default_reference = {
	.console = NULL,
	.identity = KERNEL_OBJECT_HANDLE_INVALID,
	.generation = 0u,
};

static bool identity_valid(kernel_object_handle_t identity)
{
	return identity != 0u && identity != KERNEL_OBJECT_HANDLE_INVALID;
}

static bool bytes_zero(const uint8_t *bytes, size_t count)
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

static void publication_barrier(void)
{
	__asm__ volatile("" ::: "memory");
}

static bool console_is_constructed(const struct keyboard_console *console)
{
	return console != NULL &&
	       console->lifecycle_cookie == KEYBOARD_CONSOLE_COOKIE &&
	       console->phase != KEYBOARD_CONSOLE_UNINITIALIZED;
}

static bool console_is_registered(const struct keyboard_console *console)
{
	return console_is_constructed(console) &&
	       identity_valid(console->identity) &&
	       identity_valid(console->core_identity) && console->core != NULL &&
	       console->queue != NULL && console->queue_capacity != 0u &&
	       (console->phase == KEYBOARD_CONSOLE_REGISTERED ||
		console->phase == KEYBOARD_CONSOLE_FOCUSED ||
		console->phase == KEYBOARD_CONSOLE_POISONED);
}

static void clear_transient_state(struct keyboard_console *console)
{
	console->left_shift = 0u;
	console->right_shift = 0u;
	console->left_ctrl = 0u;
	console->right_ctrl = 0u;
	console->left_alt = 0u;
	console->right_alt = 0u;
	console->caps_pressed = 0u;
	console->num_pressed = 0u;
	console->scroll_pressed = 0u;
	console->insert_pressed = 0u;
	console->sysrq_pressed = 0u;
}

static void discard_records(struct keyboard_console *console)
{
	uint32_t write_sequence = console->write_sequence;

	publication_barrier();
	console->read_sequence = write_sequence;
	console->last_character_valid = 0u;
	console->last_character = 0u;
	console->last_bios_key = 0u;
}

static void reset_after_unregister(struct keyboard_console *console)
{
	struct input_handler handler = console->handler;
	uint64_t generation = console->generation;
	uint8_t *bytes = (uint8_t *)console;
	size_t index;

	for (index = 0u; index < sizeof(*console); ++index)
		bytes[index] = 0u;
	console->handler = handler;
	console->identity = KERNEL_OBJECT_HANDLE_INVALID;
	console->core_identity = KERNEL_OBJECT_HANDLE_INVALID;
	console->generation = generation;
	console->lifecycle_cookie = KEYBOARD_CONSOLE_COOKIE;
	console->phase = KEYBOARD_CONSOLE_EMPTY;
}

void keyboard_console_construct(struct keyboard_console *console)
{
	uint8_t *bytes = (uint8_t *)console;
	size_t index;

	if (console == NULL)
		return;
	for (index = 0u; index < sizeof(*console); ++index)
		bytes[index] = 0u;
	input_handler_construct(&console->handler);
	console->identity = KERNEL_OBJECT_HANDLE_INVALID;
	console->core_identity = KERNEL_OBJECT_HANDLE_INVALID;
	console->lifecycle_cookie = KEYBOARD_CONSOLE_COOKIE;
	console->phase = KEYBOARD_CONSOLE_EMPTY;
}

static bool config_valid(const struct input_core *core,
			 const struct keyboard_console_config *config)
{
	return core != NULL && config != NULL &&
	       identity_valid(config->identity) &&
	       identity_valid(config->core_identity) &&
	       core->identity == config->core_identity && config->queue != NULL &&
	       config->queue_capacity != 0u &&
	       bytes_zero(config->reserved, ARRAY_SIZE(config->reserved));
}

static struct keyboard_console *console_from_handler(
	struct input_handler *handler, kernel_object_handle_t context)
{
	struct keyboard_console *console;

	if (handler == NULL)
		return NULL;
	console = input_handler_context(handler);
	if (!console_is_registered(console) || handler != &console->handler ||
	    context != console->identity)
		return NULL;
	return console;
}

static enum input_focus_result console_focus_enter(
	struct input_handler *handler, kernel_object_handle_t context,
	kernel_object_handle_t core_identity, uint64_t focus_generation)
{
	struct keyboard_console *console =
		console_from_handler(handler, context);

	if (console == NULL || core_identity != console->core_identity ||
	    focus_generation == 0u ||
	    console->phase != KEYBOARD_CONSOLE_REGISTERED)
		return INPUT_FOCUS_BROKEN;
	discard_records(console);
	clear_transient_state(console);
	console->focus_generation = focus_generation;
	console->phase = KEYBOARD_CONSOLE_FOCUSED;
	return INPUT_FOCUS_OK;
}

static void console_focus_leave(struct input_handler *handler,
	kernel_object_handle_t context, kernel_object_handle_t core_identity,
	uint64_t focus_generation)
{
	struct keyboard_console *console =
		console_from_handler(handler, context);

	if (console == NULL)
		return;
	if (core_identity != console->core_identity || focus_generation == 0u ||
	    focus_generation != console->focus_generation) {
		console->phase = KEYBOARD_CONSOLE_POISONED;
		return;
	}
	discard_records(console);
	clear_transient_state(console);
	console->focus_generation = 0u;
	if (console->phase != KEYBOARD_CONSOLE_POISONED)
		console->phase = KEYBOARD_CONSOLE_REGISTERED;
}

static bool event_reserved_zero(const struct input_event *event)
{
	return bytes_zero(event->reserved, ARRAY_SIZE(event->reserved));
}

static bool event_is_valid(const struct keyboard_console *console,
			   const struct input_event *event)
{
	if (event == NULL || event->type != (uint8_t)INPUT_EVENT_KEY ||
	    event->code == INPUT_KEY_CODE_RESERVED || event->sequence == 0u ||
	    event->handler_identity != console->identity ||
	    event->focus_generation != console->focus_generation ||
	    !identity_valid(event->device_identity) ||
	    event->device_generation == 0u ||
	    (event->flags & (uint8_t)~INPUT_EVENT_FLAG_MASK) != 0u ||
	    !event_reserved_zero(event))
		return false;
	return event->value == (uint8_t)INPUT_KEY_RELEASED ||
	       event->value == (uint8_t)INPUT_KEY_PRESSED ||
	       event->value == (uint8_t)INPUT_KEY_REPEATED;
}

static void update_held(uint8_t *held, uint8_t value)
{
	*held = value == (uint8_t)INPUT_KEY_RELEASED ? 0u : 1u;
}

static void update_lock(uint8_t *lock, uint8_t *held, uint8_t value)
{
	if (value == (uint8_t)INPUT_KEY_PRESSED && *held == 0u)
		*lock ^= 1u;
	update_held(held, value);
}

static bool consume_state_event(struct keyboard_console *console,
				const struct input_event *event)
{
	switch (event->code) {
	case INPUT_KEY_CODE_LEFTSHIFT:
		update_held(&console->left_shift, event->value);
		return true;
	case INPUT_KEY_CODE_RIGHTSHIFT:
		update_held(&console->right_shift, event->value);
		return true;
	case INPUT_KEY_CODE_LEFTCTRL:
		update_held(&console->left_ctrl, event->value);
		return true;
	case INPUT_KEY_CODE_RIGHTCTRL:
		update_held(&console->right_ctrl, event->value);
		return true;
	case INPUT_KEY_CODE_LEFTALT:
		update_held(&console->left_alt, event->value);
		return true;
	case INPUT_KEY_CODE_RIGHTALT:
		update_held(&console->right_alt, event->value);
		return true;
	case INPUT_KEY_CODE_CAPSLOCK:
		update_lock(&console->caps_lock, &console->caps_pressed,
			    event->value);
		return true;
	case INPUT_KEY_CODE_NUMLOCK:
		update_lock(&console->num_lock, &console->num_pressed,
			    event->value);
		return true;
	case INPUT_KEY_CODE_SCROLLLOCK:
		update_lock(&console->scroll_lock, &console->scroll_pressed,
			    event->value);
		return true;
	case INPUT_KEY_CODE_SYSRQ:
		update_held(&console->sysrq_pressed, event->value);
		return true;
	default:
		return false;
	}
}

static enum input_status reserve_queue_slot(
	const struct keyboard_console *console, uint32_t *write_sequence)
{
	uint32_t write = console->write_sequence;
	uint32_t read;
	uint32_t queued;

	publication_barrier();
	read = console->read_sequence;
	queued = write - read;
	if (queued > console->queue_capacity)
		return INPUT_POISONED;
	if (queued == console->queue_capacity)
		return INPUT_CAPACITY_EXHAUSTED;
	*write_sequence = write;
	return INPUT_OK;
}

static void publish_record(struct keyboard_console *console,
			   uint32_t write_sequence,
			   const struct keyboard_key_record *record)
{
	uint16_t index = (uint16_t)(write_sequence % console->queue_capacity);

	console->queue[index] = *record;
	publication_barrier();
	console->write_sequence = write_sequence + 1u;
}

static enum input_handler_result console_receive(
	struct input_handler *handler, kernel_object_handle_t context,
	const struct input_event *event)
{
	struct keyboard_console *console =
		console_from_handler(handler, context);
	struct keyboard_key_translation translation;
	struct keyboard_key_record record;
	uint32_t write_sequence;
	enum input_status queue_status;

	if (console == NULL)
		return INPUT_HANDLER_BROKEN;
	if (console->phase != KEYBOARD_CONSOLE_FOCUSED ||
	    !event_is_valid(console, event)) {
		console->phase = KEYBOARD_CONSOLE_POISONED;
		return INPUT_HANDLER_BROKEN;
	}
	if (consume_state_event(console, event)) {
		console->received_count =
			saturating_increment(console->received_count);
		return INPUT_HANDLER_HANDLED;
	}
	if (event->value == (uint8_t)INPUT_KEY_RELEASED) {
		if (event->code == INPUT_KEY_CODE_INSERT ||
		    event->code == INPUT_KEY_CODE_KP0)
			console->insert_pressed = 0u;
		console->received_count =
			saturating_increment(console->received_count);
		return INPUT_HANDLER_HANDLED;
	}
	if (!keyboard_keymap_translate(console, event->code, &translation)) {
		console->unsupported_count =
			saturating_increment(console->unsupported_count);
		console->received_count =
			saturating_increment(console->received_count);
		return INPUT_HANDLER_HANDLED;
	}
	queue_status = reserve_queue_slot(console, &write_sequence);
	if (queue_status == INPUT_POISONED) {
		console->phase = KEYBOARD_CONSOLE_POISONED;
		return INPUT_HANDLER_BROKEN;
	}
	if (queue_status == INPUT_CAPACITY_EXHAUSTED) {
		console->backpressure_count =
			saturating_increment(console->backpressure_count);
		return INPUT_HANDLER_DEFER;
	}
	if (event->value == (uint8_t)INPUT_KEY_PRESSED &&
	    keyboard_keymap_toggles_insert(console, event->code)) {
		console->insert_lock ^= 1u;
		console->insert_pressed = 1u;
	} else if (event->value == (uint8_t)INPUT_KEY_REPEATED &&
		   keyboard_keymap_toggles_insert(console, event->code)) {
		console->insert_pressed = 1u;
	}
	record = (struct keyboard_key_record){
		.input_sequence = event->sequence,
		.hardware_code = event->hardware_code,
		.bios_key = translation.bios_key,
		.code = event->code,
		.character = translation.character,
		.has_character = translation.has_character,
		.value = event->value,
		.flags = event->flags,
		.reserved = {0u},
	};
	publish_record(console, write_sequence, &record);
	console->received_count = saturating_increment(console->received_count);
	return INPUT_HANDLER_HANDLED;
}

enum input_status keyboard_console_register(
	struct keyboard_console *console, struct input_core *core,
	const struct keyboard_console_config *config)
{
	struct input_handler_config handler_config;
	struct input_handler_binding binding;
	uint64_t next_generation;
	enum input_status status;

	if (!console_is_constructed(console) || !config_valid(core, config))
		return INPUT_INVALID_ARGUMENT;
	if (console->phase != KEYBOARD_CONSOLE_EMPTY ||
	    console->handler.phase != INPUT_HANDLER_EMPTY)
		return INPUT_ALREADY_REGISTERED;
	if (console->generation >= KEYBOARD_GENERATION_MAX)
		return INPUT_CAPACITY_EXHAUSTED;
	next_generation = console->generation + 1u;
	console->core = core;
	console->queue = config->queue;
	console->wait = config->wait;
	console->wait_context = config->wait_context;
	console->identity = config->identity;
	console->core_identity = config->core_identity;
	console->queue_capacity = config->queue_capacity;
	handler_config = (struct input_handler_config){
		.identity = config->identity,
		.context = config->identity,
		.handler_context = console,
		.capabilities = INPUT_CAPABILITY_KEY,
		.focus_enter = console_focus_enter,
		.focus_leave = console_focus_leave,
		.receive = console_receive,
		.reserved = {0u},
	};
	status = input_handler_register(core, &console->handler, &handler_config,
					&binding);
	if (status != INPUT_OK) {
		reset_after_unregister(console);
		return status;
	}
	console->binding = binding;
	console->generation = next_generation;
	console->phase = KEYBOARD_CONSOLE_REGISTERED;
	return INPUT_OK;
}

enum input_status keyboard_console_focus(struct keyboard_console *console)
{
	if (!console_is_registered(console))
		return INPUT_INVALID_ARGUMENT;
	if (console->phase == KEYBOARD_CONSOLE_POISONED)
		return INPUT_POISONED;
	if (console->phase == KEYBOARD_CONSOLE_FOCUSED)
		return INPUT_OK;
	if (console->phase != KEYBOARD_CONSOLE_REGISTERED)
		return INPUT_INVALID_STATE;
	return input_focus_set(console->core, &console->binding);
}

enum input_status keyboard_console_unfocus(struct keyboard_console *console)
{
	if (!console_is_registered(console))
		return INPUT_INVALID_ARGUMENT;
	if (console->phase != KEYBOARD_CONSOLE_FOCUSED &&
	    (console->phase != KEYBOARD_CONSOLE_POISONED ||
	     console->focus_generation == 0u))
		return INPUT_INVALID_STATE;
	return input_focus_clear(console->core, &console->binding);
}

enum input_status keyboard_console_unregister(struct keyboard_console *console)
{
	enum input_status status;

	if (!console_is_registered(console))
		return INPUT_INVALID_ARGUMENT;
	if (console->phase != KEYBOARD_CONSOLE_REGISTERED &&
	    (console->phase != KEYBOARD_CONSOLE_POISONED ||
	     console->focus_generation != 0u))
		return INPUT_INVALID_STATE;
	if (console->handler.phase != INPUT_HANDLER_QUIESCED) {
		status = input_handler_quiesce(console->core, &console->binding);
		if (status != INPUT_OK)
			return status;
	}
	status = input_handler_unregister(console->core, &console->binding);
	if (status != INPUT_OK) {
		console->phase = KEYBOARD_CONSOLE_POISONED;
		return status;
	}
	reset_after_unregister(console);
	return INPUT_OK;
}

static bool queue_window(const struct keyboard_console *console,
			 uint32_t *read_sequence, uint32_t *write_sequence)
{
	uint32_t read;
	uint32_t write;

	if (!console_is_registered(console) ||
	    console->phase == KEYBOARD_CONSOLE_POISONED)
		return false;
	read = console->read_sequence;
	publication_barrier();
	write = console->write_sequence;
	if (write - read > console->queue_capacity)
		return false;
	*read_sequence = read;
	*write_sequence = write;
	return true;
}

static bool consume_record(struct keyboard_console *console,
			   uint32_t limit,
			   struct keyboard_key_record *record)
{
	uint32_t read = console->read_sequence;
	uint16_t index;

	if (read == limit)
		return false;
	index = (uint16_t)(read % console->queue_capacity);
	publication_barrier();
	*record = console->queue[index];
	publication_barrier();
	console->read_sequence = read + 1u;
	return true;
}

bool keyboard_console_read_character(struct keyboard_console *console,
				     char *character)
{
	struct keyboard_key_record record;
	uint32_t read_sequence;
	uint32_t write_sequence;

	if (character == NULL ||
	    !queue_window(console, &read_sequence, &write_sequence))
		return false;
	(void)read_sequence;
	while (consume_record(console, write_sequence, &record)) {
		if (record.has_character == 0u)
			continue;
		*character = (char)record.character;
		console->last_character = record.character;
		console->last_bios_key = record.bios_key;
		console->last_character_valid = 1u;
		return true;
	}
	return false;
}

bool keyboard_console_peek_character(const struct keyboard_console *console,
				     char *character)
{
	uint32_t read_sequence;
	uint32_t write_sequence;

	if (character == NULL ||
	    !queue_window(console, &read_sequence, &write_sequence))
		return false;
	while (read_sequence != write_sequence) {
		uint16_t index =
			(uint16_t)(read_sequence % console->queue_capacity);

		if (console->queue[index].has_character != 0u) {
			*character = (char)console->queue[index].character;
			return true;
		}
		read_sequence++;
	}
	return false;
}

bool keyboard_console_wait_character(struct keyboard_console *console,
				     char *character)
{
	if (console == NULL || character == NULL)
		return false;
	for (;;) {
		if (keyboard_console_read_character(console, character))
			return true;
		if (console->phase != KEYBOARD_CONSOLE_FOCUSED ||
		    console->wait == NULL)
			return false;
		console->wait(console->wait_context);
	}
}

bool keyboard_console_read_bios_key(struct keyboard_console *console,
				    uint16_t *key)
{
	struct keyboard_key_record record;
	uint32_t read_sequence;
	uint32_t write_sequence;

	if (key == NULL ||
	    !queue_window(console, &read_sequence, &write_sequence) ||
	    read_sequence == write_sequence ||
	    !consume_record(console, write_sequence, &record))
		return false;
	*key = record.bios_key;
	if (record.has_character != 0u) {
		console->last_character = record.character;
		console->last_bios_key = record.bios_key;
		console->last_character_valid = 1u;
	}
	return true;
}

bool keyboard_console_peek_bios_key(const struct keyboard_console *console,
				    uint16_t *key)
{
	uint32_t read_sequence;
	uint32_t write_sequence;
	uint16_t index;

	if (key == NULL ||
	    !queue_window(console, &read_sequence, &write_sequence) ||
	    read_sequence == write_sequence)
		return false;
	index = (uint16_t)(read_sequence % console->queue_capacity);
	*key = console->queue[index].bios_key;
	return true;
}

bool keyboard_console_wait_bios_key(struct keyboard_console *console,
				    uint16_t *key)
{
	if (console == NULL || key == NULL)
		return false;
	for (;;) {
		if (keyboard_console_read_bios_key(console, key))
			return true;
		if (console->phase != KEYBOARD_CONSOLE_FOCUSED ||
		    console->wait == NULL)
			return false;
		console->wait(console->wait_context);
	}
}

enum input_status keyboard_console_pump(
	struct keyboard_console *console,
	const struct input_device_binding *device, uint16_t budget,
	uint16_t *delivered)
{
	if (!console_is_registered(console) || device == NULL ||
	    delivered == NULL || budget == 0u)
		return INPUT_INVALID_ARGUMENT;
	if (console->phase == KEYBOARD_CONSOLE_POISONED)
		return INPUT_POISONED;
	return input_device_pump(console->core, device, budget, delivered);
}

void keyboard_console_flush(struct keyboard_console *console)
{
	if (!console_is_registered(console) ||
	    console->phase == KEYBOARD_CONSOLE_POISONED)
		return;
	discard_records(console);
}

uint16_t keyboard_console_bios_shift_flags(
	const struct keyboard_console *console)
{
	uint16_t flags = 0u;
	uint8_t low = 0u;
	uint8_t high = 0u;

	if (!console_is_registered(console))
		return 0u;
	if (console->right_shift != 0u)
		low |= 1u << 0;
	if (console->left_shift != 0u)
		low |= 1u << 1;
	if (console->left_ctrl != 0u || console->right_ctrl != 0u)
		low |= 1u << 2;
	if (console->left_alt != 0u || console->right_alt != 0u)
		low |= 1u << 3;
	if (console->scroll_lock != 0u)
		low |= 1u << 4;
	if (console->num_lock != 0u)
		low |= 1u << 5;
	if (console->caps_lock != 0u)
		low |= 1u << 6;
	if (console->insert_lock != 0u)
		low |= 1u << 7;
	if (console->left_ctrl != 0u)
		high |= 1u << 0;
	if (console->left_alt != 0u)
		high |= 1u << 1;
	if (console->sysrq_pressed != 0u)
		high |= 1u << 2;
	if (console->scroll_pressed != 0u)
		high |= 1u << 4;
	if (console->num_pressed != 0u)
		high |= 1u << 5;
	if (console->caps_pressed != 0u)
		high |= 1u << 6;
	if (console->insert_pressed != 0u)
		high |= 1u << 7;
	flags = ((uint16_t)high << 8u) | low;
	return flags;
}

enum input_status keyboard_console_snapshot(
	const struct keyboard_console *console,
	struct keyboard_console_snapshot *snapshot)
{
	uint32_t read_sequence;
	uint32_t write_sequence;

	if (!console_is_constructed(console) || snapshot == NULL)
		return INPUT_INVALID_ARGUMENT;
	if (console->phase == KEYBOARD_CONSOLE_EMPTY) {
		*snapshot = (struct keyboard_console_snapshot){
			.identity = KERNEL_OBJECT_HANDLE_INVALID,
			.generation = console->generation,
			.phase = KEYBOARD_CONSOLE_EMPTY,
			.reserved = {0u},
		};
		return INPUT_OK;
	}
	if (!queue_window(console, &read_sequence, &write_sequence))
		return console->phase == KEYBOARD_CONSOLE_POISONED
			       ? INPUT_POISONED
			       : INPUT_INVALID_STATE;
	*snapshot = (struct keyboard_console_snapshot){
		.identity = console->identity,
		.generation = console->generation,
		.focus_generation = console->focus_generation,
		.received_count = console->received_count,
		.backpressure_count = console->backpressure_count,
		.unsupported_count = console->unsupported_count,
		.queued_keys = write_sequence - read_sequence,
		.queue_capacity = console->queue_capacity,
		.bios_shift_flags = keyboard_console_bios_shift_flags(console),
		.phase = console->phase,
		.reserved = {0u},
	};
	return INPUT_OK;
}

void keyboard_init(void)
{
	if (default_reference.console != NULL)
		return;
	default_reference.identity = KERNEL_OBJECT_HANDLE_INVALID;
	default_reference.generation = 0u;
}

static struct keyboard_console *resolve_default_reference(void)
{
	struct keyboard_console *console = default_reference.console;
	kernel_object_handle_t identity;
	uint64_t generation;

	if (console == NULL)
		return NULL;
	publication_barrier();
	identity = default_reference.identity;
	generation = default_reference.generation;
	if (!console_is_registered(console) ||
	    console->phase != KEYBOARD_CONSOLE_FOCUSED ||
	    console->identity != identity || console->generation != generation)
		return NULL;
	return console;
}

struct keyboard_console *keyboard_default_console(void)
{
	return resolve_default_reference();
}

enum input_status keyboard_default_bind(
	struct keyboard_console *console, kernel_object_handle_t identity,
	uint64_t generation)
{
	if (!console_is_registered(console) || !identity_valid(identity) ||
	    generation == 0u)
		return INPUT_INVALID_ARGUMENT;
	if (console->phase == KEYBOARD_CONSOLE_POISONED)
		return INPUT_POISONED;
	if (console->phase != KEYBOARD_CONSOLE_FOCUSED)
		return INPUT_INVALID_STATE;
	if (console->identity != identity)
		return INPUT_IDENTITY_MISMATCH;
	if (console->generation != generation)
		return INPUT_STALE_BINDING;
	if (default_reference.console != NULL) {
		if (default_reference.console == console &&
		    default_reference.identity == identity &&
		    default_reference.generation == generation)
			return INPUT_OK;
		return INPUT_BUSY;
	}
	default_reference.identity = identity;
	default_reference.generation = generation;
	publication_barrier();
	default_reference.console = console;
	return INPUT_OK;
}

enum input_status keyboard_default_unbind(
	struct keyboard_console *console, kernel_object_handle_t identity,
	uint64_t generation)
{
	if (console == NULL || !identity_valid(identity) || generation == 0u)
		return INPUT_INVALID_ARGUMENT;
	if (default_reference.console == NULL)
		return INPUT_NOT_FOUND;
	if (default_reference.console != console ||
	    default_reference.identity != identity)
		return INPUT_IDENTITY_MISMATCH;
	if (default_reference.generation != generation)
		return INPUT_STALE_BINDING;
	default_reference.console = NULL;
	publication_barrier();
	default_reference.identity = KERNEL_OBJECT_HANDLE_INVALID;
	default_reference.generation = 0u;
	return INPUT_OK;
}

char keyboard_getchar(void)
{
	char character;

	for (;;) {
		struct keyboard_console *console = resolve_default_reference();

		if (console != NULL &&
		    keyboard_console_wait_character(console, &character))
			return character;
		keyboard_console_x86_wait(NULL);
	}
}

bool keyboard_character_available(void)
{
	char character;
	struct keyboard_console *console = resolve_default_reference();

	return console != NULL &&
	       keyboard_console_peek_character(console, &character);
}

bool keyboard_peekchar(char *character)
{
	struct keyboard_console *console = resolve_default_reference();

	return console != NULL &&
	       keyboard_console_peek_character(console, character);
}

void keyboard_flush(void)
{
	struct keyboard_console *console = resolve_default_reference();

	if (console != NULL)
		keyboard_console_flush(console);
}

uint16_t keyboard_get_bios_key(void)
{
	uint16_t key;

	for (;;) {
		struct keyboard_console *console = resolve_default_reference();

		if (console != NULL &&
		    keyboard_console_wait_bios_key(console, &key))
			return key;
		keyboard_console_x86_wait(NULL);
	}
}

bool keyboard_bios_key_available(void)
{
	uint16_t key;
	struct keyboard_console *console = resolve_default_reference();

	return console != NULL && keyboard_console_peek_bios_key(console, &key);
}

bool keyboard_peek_bios_key(uint16_t *key)
{
	struct keyboard_console *console = resolve_default_reference();

	return console != NULL && keyboard_console_peek_bios_key(console, key);
}

static bool queued_bios_key_for_character(
	const struct keyboard_console *console, char character, uint16_t *key)
{
	uint32_t read_sequence;
	uint32_t write_sequence;

	if (!queue_window(console, &read_sequence, &write_sequence))
		return false;
	while (read_sequence != write_sequence) {
		uint16_t index =
			(uint16_t)(read_sequence % console->queue_capacity);
		const struct keyboard_key_record *record = &console->queue[index];

		if (record->has_character != 0u &&
		    record->character == (uint8_t)character) {
			*key = record->bios_key;
			return true;
		}
		read_sequence++;
	}
	return false;
}

bool keyboard_bios_key_from_character(char character, uint16_t *key)
{
	struct keyboard_console *console = resolve_default_reference();

	if (key == NULL)
		return false;
	if (console != NULL && console->last_character_valid != 0u &&
	    console->last_character == (uint8_t)character) {
		*key = console->last_bios_key;
		return true;
	}
	if (console != NULL &&
	    queued_bios_key_for_character(console, character, key))
		return true;
	/* Compatibility for callers that did not obtain the byte from this FIFO.
	 * The normal event path always returns the captured record above. */
	return keyboard_keymap_compatibility_bios_key(character, key);
}

uint16_t keyboard_bios_shift_flags(void)
{
	struct keyboard_console *console = resolve_default_reference();

	return console == NULL ? 0u
			       : keyboard_console_bios_shift_flags(console);
}

size_t keyboard_readline(char *buffer, size_t capacity)
{
	size_t length = 0u;
	char character;

	if (buffer == NULL || capacity == 0u)
		return 0u;
	for (;;) {
		character = keyboard_getchar();
		if (character == '\n' || character == '\r') {
			console_putc('\n');
			break;
		}
		if (character == '\b') {
			if (length != 0u) {
				--length;
				console_backspace();
			}
			continue;
		}
		if ((uint8_t)character >= 0x20u &&
		    (uint8_t)character <= 0x7eu && length + 1u < capacity) {
			buffer[length++] = character;
			console_putc(character);
		}
	}
	buffer[length] = '\0';
	return length;
}

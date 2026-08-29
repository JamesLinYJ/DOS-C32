/* SPDX-License-Identifier: GPL-2.0-only */
/* Native console consumer for decoded input-core keyboard events. */
#ifndef DOSC32_KEYBOARD_H
#define DOSC32_KEYBOARD_H

#include "input.h"

enum keyboard_console_phase {
	KEYBOARD_CONSOLE_UNINITIALIZED = 0,
	KEYBOARD_CONSOLE_EMPTY,
	KEYBOARD_CONSOLE_REGISTERED,
	KEYBOARD_CONSOLE_FOCUSED,
	KEYBOARD_CONSOLE_POISONED
};

/* Process-context idle hook used only by the blocking read helpers. */
typedef void (*keyboard_console_wait_fn)(void *context);

/* One decoded make/repeat result. BIOS AX is captured at event time. */
struct keyboard_key_record {
	uint64_t input_sequence;
	uint32_t hardware_code;
	uint16_t bios_key;
	input_key_code_t code;
	uint8_t character;
	uint8_t has_character;
	uint8_t value;
	uint8_t flags;
	uint8_t reserved[4];
} __aligned(8);

struct keyboard_console_config {
	kernel_object_handle_t identity;
	kernel_object_handle_t core_identity;
	struct keyboard_key_record *queue;
	keyboard_console_wait_fn wait;
	void *wait_context;
	uint16_t queue_capacity;
	uint8_t reserved[6];
} __aligned(8);

/* Caller-owned instance; event receipt is a single producer, reads a single
 * consumer. Separate publication counters make IRQ receipt nonblocking. */
struct keyboard_console {
	struct input_handler handler;
	struct input_handler_binding binding;
	struct input_core *core;
	struct keyboard_key_record *queue;
	keyboard_console_wait_fn wait;
	void *wait_context;
	kernel_object_handle_t identity;
	kernel_object_handle_t core_identity;
	uint64_t generation;
	uint64_t focus_generation;
	uint64_t received_count;
	uint64_t backpressure_count;
	uint64_t unsupported_count;
	volatile uint32_t read_sequence;
	volatile uint32_t write_sequence;
	uint32_t lifecycle_cookie;
	uint16_t queue_capacity;
	uint8_t phase;
	uint8_t left_shift;
	uint8_t right_shift;
	uint8_t left_ctrl;
	uint8_t right_ctrl;
	uint8_t left_alt;
	uint8_t right_alt;
	uint8_t caps_lock;
	uint8_t num_lock;
	uint8_t scroll_lock;
	uint8_t insert_lock;
	uint8_t caps_pressed;
	uint8_t num_pressed;
	uint8_t scroll_pressed;
	uint8_t insert_pressed;
	uint8_t sysrq_pressed;
	uint8_t last_character;
	uint16_t last_bios_key;
	uint8_t last_character_valid;
	uint8_t reserved[4];
} __aligned(8);

struct keyboard_console_snapshot {
	kernel_object_handle_t identity;
	uint64_t generation;
	uint64_t focus_generation;
	uint64_t received_count;
	uint64_t backpressure_count;
	uint64_t unsupported_count;
	uint32_t queued_keys;
	uint16_t queue_capacity;
	uint16_t bios_shift_flags;
	uint8_t phase;
	uint8_t reserved[7];
} __aligned(8);

void keyboard_console_construct(struct keyboard_console *console);
enum input_status keyboard_console_register(
	struct keyboard_console *console, struct input_core *core,
	const struct keyboard_console_config *config) __must_check;
enum input_status keyboard_console_focus(
	struct keyboard_console *console) __must_check;
enum input_status keyboard_console_unfocus(
	struct keyboard_console *console) __must_check;
enum input_status keyboard_console_unregister(
	struct keyboard_console *console) __must_check;
enum input_status keyboard_console_snapshot(
	const struct keyboard_console *console,
	struct keyboard_console_snapshot *snapshot) __must_check;

bool keyboard_console_read_character(struct keyboard_console *console,
				     char *character);
bool keyboard_console_peek_character(const struct keyboard_console *console,
				     char *character);
bool keyboard_console_wait_character(struct keyboard_console *console,
				     char *character);
bool keyboard_console_read_bios_key(struct keyboard_console *console,
				    uint16_t *key);
bool keyboard_console_peek_bios_key(const struct keyboard_console *console,
				    uint16_t *key);
bool keyboard_console_wait_bios_key(struct keyboard_console *console,
				    uint16_t *key);
enum input_status keyboard_console_pump(
	struct keyboard_console *console,
	const struct input_device_binding *device, uint16_t budget,
	uint16_t *delivered) __must_check;
void keyboard_console_flush(struct keyboard_console *console);
uint16_t keyboard_console_bios_shift_flags(
	const struct keyboard_console *console);
/* Ring-0 process-context wait. Entry and return both require IF=0. */
void keyboard_console_x86_wait(void *context);

/* The runtime owner explicitly publishes its one focused console here. */
void keyboard_init(void);
struct keyboard_console *keyboard_default_console(void) __must_check;
enum input_status keyboard_default_bind(
	struct keyboard_console *console, kernel_object_handle_t identity,
	uint64_t generation) __must_check;
enum input_status keyboard_default_unbind(
	struct keyboard_console *console, kernel_object_handle_t identity,
	uint64_t generation) __must_check;

char keyboard_getchar(void);
bool keyboard_character_available(void);
bool keyboard_peekchar(char *character);
void keyboard_flush(void);
uint16_t keyboard_get_bios_key(void);
bool keyboard_bios_key_available(void);
bool keyboard_peek_bios_key(uint16_t *key);
bool keyboard_bios_key_from_character(char character, uint16_t *key);
uint16_t keyboard_bios_shift_flags(void);
size_t keyboard_readline(char *buffer, size_t capacity);

static_assert_expression(sizeof(struct keyboard_key_record) == 24u,
			 "keyboard key record layout changed");
static_assert_expression(sizeof(struct keyboard_console_snapshot) == 64u,
			 "keyboard console snapshot layout changed");

#endif

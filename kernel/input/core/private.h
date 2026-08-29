/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DOSC32_INPUT_CORE_PRIVATE_H
#define DOSC32_INPUT_CORE_PRIVATE_H

#include "input.h"

#define INPUT_CORE_COOKIE 0x494e5043u
#define INPUT_DEVICE_COOKIE 0x494e5044u
#define INPUT_HANDLER_COOKIE 0x494e5048u
#define INPUT_GENERATION_MAX ((uint64_t)-2)
#define INPUT_SLOT_INVALID ((uint16_t)-1)

bool input_internal_identity_valid(kernel_object_handle_t identity);
bool input_internal_bytes_zero(const uint8_t *bytes, size_t count);
uint64_t input_internal_saturating_increment(uint64_t value);
void input_internal_guard_enter(const struct input_core *core);
void input_internal_guard_exit(const struct input_core *core);
bool input_internal_core_is_usable(const struct input_core *core);
enum input_status input_internal_core_identity_status(
	const struct input_core *core, kernel_object_handle_t identity);
enum input_status input_internal_device_binding_status(
	struct input_core *core, const struct input_device_binding *binding,
	struct input_device **device);
enum input_status input_internal_handler_binding_status(
	struct input_core *core, const struct input_handler_binding *binding,
	struct input_handler **handler);

#endif

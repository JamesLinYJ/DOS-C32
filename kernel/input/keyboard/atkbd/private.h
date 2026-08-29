/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DOSC32_ATKBD_PRIVATE_H
#define DOSC32_ATKBD_PRIVATE_H

#include "atkbd.h"
#include "input_keycodes.h"

#define ATKBD_DRIVER_COOKIE 0x41544b44u
#define ATKBD_ENDPOINT_COOKIE 0x41544b45u
#define ATKBD_GENERATION_MAX ((uint64_t)-2)
#define ATKBD_SLOT_INVALID ((uint16_t)-1)

#define ATKBD_RET_BAT 0xaau
#define ATKBD_RET_EMUL0 0xe0u
#define ATKBD_RET_EMUL1 0xe1u
#define ATKBD_RET_RELEASE 0xf0u
#define ATKBD_RET_HANJA 0xf1u
#define ATKBD_RET_HANGEUL 0xf2u
#define ATKBD_RET_ACK 0xfau
#define ATKBD_RET_NAK 0xfeu
#define ATKBD_RET_ERR 0xffu

#define ATKBD_CMD_SET_LEDS 0xedu
#define ATKBD_CMD_SET_SCANSET 0xf0u
#define ATKBD_CMD_SET_TYPEMATIC 0xf3u
#define ATKBD_CMD_ENABLE 0xf4u
#define ATKBD_CMD_DISABLE 0xf5u
#define ATKBD_CMD_RESEND 0xfeu

struct atkbd_decoded_key {
	input_key_code_t code;
	uint32_t hardware_code;
	uint8_t value;
	uint8_t flags;
};

enum atkbd_decode_result {
	ATKBD_DECODE_NO_EVENT = 0,
	ATKBD_DECODE_EVENT,
	ATKBD_DECODE_MALFORMED
};

bool atkbd_internal_identity_valid(kernel_object_handle_t identity);
bool atkbd_internal_bytes_zero(const uint8_t *bytes, size_t count);
uint64_t atkbd_internal_saturating_increment(uint64_t value);
void atkbd_internal_decode_reset(struct atkbd_decode_state *state);
void atkbd_internal_decode_cancel_sequence(struct atkbd_decode_state *state);
input_key_code_t atkbd_internal_set1_keycode(uint8_t code, bool extended);
input_key_code_t atkbd_internal_set2_keycode(uint16_t code);
enum atkbd_decode_result atkbd_internal_decode(
	uint8_t mode, struct atkbd_decode_state *state, uint8_t data,
	struct atkbd_decoded_key *decoded);
enum serio_receive_result atkbd_internal_interrupt(
	struct serio_port *port, struct serio_driver *driver,
	void *binding_context, const struct serio_raw_event *event);
enum atkbd_status atkbd_internal_reference_status(
	const struct atkbd_driver *driver,
	const struct atkbd_endpoint_reference *reference,
	struct atkbd_endpoint **endpoint);
void atkbd_internal_poison_endpoint(struct atkbd_endpoint *endpoint);

#endif

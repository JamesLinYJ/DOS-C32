// SPDX-License-Identifier: GPL-2.0-only
/* Bounded 8254 programming, read-back and explicit input-clock state. */
#include "private.h"

#include "math64.h"

#define PIT_BINARY_MAX_COUNT 65536u
#define PIT_BCD_MAX_COUNT 10000u

static struct x86_chipset_pit_state *pit_for_port(
	struct x86_legacy_chipset_owner *chipset, uint16_t port)
{
	if (port < X86_PIT_CHANNEL0_PORT || port > X86_PIT_CHANNEL2_PORT)
		return NULL;
	return &chipset->pit[port - X86_PIT_CHANNEL0_PORT];
}

static bool decode_bcd_count(uint16_t encoded, uint32_t *count)
{
	uint32_t thousands = (uint32_t)((encoded >> 12u) & 0x0fu);
	uint32_t hundreds = (uint32_t)((encoded >> 8u) & 0x0fu);
	uint32_t tens = (uint32_t)((encoded >> 4u) & 0x0fu);
	uint32_t ones = (uint32_t)(encoded & 0x0fu);

	if (count == NULL || thousands > 9u || hundreds > 9u || tens > 9u ||
	    ones > 9u)
		return false;
	*count = thousands * 1000u + hundreds * 100u + tens * 10u + ones;
	if (*count == 0u)
		*count = PIT_BCD_MAX_COUNT;
	return true;
}

static bool decode_reload(uint16_t encoded, uint8_t bcd, uint32_t *count)
{
	if (count == NULL)
		return false;
	if (bcd != 0u)
		return decode_bcd_count(encoded, count);
	*count = encoded == 0u ? PIT_BINARY_MAX_COUNT : (uint32_t)encoded;
	return true;
}

static uint16_t encode_bcd_count(uint32_t count)
{
	if (count == PIT_BCD_MAX_COUNT)
		return 0u;
	return (uint16_t)(((count / 1000u) << 12u) |
			  (((count / 100u) % 10u) << 8u) |
			  (((count / 10u) % 10u) << 4u) |
			  (count % 10u));
}

static uint16_t encode_count(const struct x86_chipset_pit_state *pit,
			     uint32_t count)
{
	if (count == 0u)
		return 0u;
	if (pit->bcd != 0u)
		return encode_bcd_count(count);
	return count == PIT_BINARY_MAX_COUNT ? 0u : (uint16_t)count;
}

static uint32_t reload_count(const struct x86_chipset_pit_state *pit)
{
	uint32_t count = 0u;

	if (!decode_reload(pit->reload, pit->bcd, &count))
		return 0u;
	return count;
}

static void update_periodic_output(struct x86_chipset_pit_state *pit)
{
	uint32_t period = reload_count(pit);
	uint32_t elapsed;
	uint32_t high_ticks;

	if (period == 0u || pit->current_count == 0u) {
		pit->output = 0u;
		return;
	}
	if (pit->mode == 2u) {
		pit->output = (uint8_t)(pit->current_count != 1u);
		return;
	}
	elapsed = period - pit->current_count;
	high_ticks = (period + 1u) / 2u;
	pit->output = (uint8_t)(elapsed < high_ticks);
}

static void sync_visible_count(struct x86_chipset_pit_state *pit)
{
	pit->current = encode_count(pit, pit->current_count);
}

static void complete_reload(struct x86_chipset_pit_state *pit,
			    uint16_t reload)
{
	uint32_t count;

	pit->reload = reload;
	pit->read_phase = 0u;
	pit->write_phase = 0u;
	pit->strobe_pending = 0u;
	if (!decode_reload(reload, pit->bcd, &count)) {
		pit->current = reload;
		pit->current_count = 0u;
		pit->null_count = 1u;
		pit->running = 0u;
		return;
	}
	pit->current_count = count;
	pit->current = reload;
	pit->null_count = 0u;
	pit->running = (uint8_t)(pit->mode == 0u || pit->mode == 2u ||
				 pit->mode == 3u || pit->mode == 4u);
	pit->output = (uint8_t)(pit->mode == 0u ? 0u : 1u);
}

void x86_legacy_pit_initialize_state(
	struct x86_chipset_pit_state *pit, uint16_t reload, uint8_t access,
	uint8_t mode, uint8_t bcd)
{
	if (pit == NULL)
		return;
	*pit = (struct x86_chipset_pit_state){
		.access = access,
		.mode = mode,
		.bcd = bcd,
	};
	complete_reload(pit, reload);
}

static void latch_count(struct x86_chipset_pit_state *pit)
{
	if (pit->count_latched != 0u)
		return;
	pit->latched_count = pit->current;
	pit->latch_access = pit->access;
	pit->latch_read_phase = 0u;
	pit->count_latched = 1u;
}

static uint8_t status_byte(const struct x86_chipset_pit_state *pit)
{
	return (uint8_t)((pit->output != 0u ? 0x80u : 0u) |
			 (pit->null_count != 0u ? 0x40u : 0u) |
			 (uint8_t)(pit->access << 4u) |
			 (uint8_t)(pit->mode << 1u) | pit->bcd);
}

static void latch_status(struct x86_chipset_pit_state *pit)
{
	if (pit->status_latched != 0u)
		return;
	pit->latched_status = status_byte(pit);
	pit->status_latched = 1u;
}

static uint8_t read_count(struct x86_chipset_pit_state *pit)
{
	uint16_t count = pit->count_latched != 0u ? pit->latched_count
						  : pit->current;
	uint8_t access = pit->count_latched != 0u ? pit->latch_access
						  : pit->access;
	uint8_t *phase = pit->count_latched != 0u ? &pit->latch_read_phase
						  : &pit->read_phase;
	uint8_t result;

	if (access == 1u) {
		result = (uint8_t)count;
		pit->count_latched = 0u;
		return result;
	}
	if (access == 2u) {
		result = (uint8_t)(count >> 8u);
		pit->count_latched = 0u;
		return result;
	}
	if (*phase == 0u) {
		*phase = 1u;
		return (uint8_t)count;
	}
	*phase = 0u;
	pit->count_latched = 0u;
	return (uint8_t)(count >> 8u);
}

static void write_count(struct x86_chipset_pit_state *pit, uint8_t value)
{
	if (pit->access == 1u) {
		complete_reload(pit, value);
		return;
	}
	if (pit->access == 2u) {
		complete_reload(pit, (uint16_t)value << 8u);
		return;
	}
	if (pit->write_phase == 0u) {
		pit->pending_low_byte = value;
		pit->write_phase = 1u;
		pit->null_count = 1u;
		pit->running = 0u;
		return;
	}
	complete_reload(pit, (uint16_t)(pit->pending_low_byte |
				       ((uint16_t)value << 8u)));
}

static void program_channel(struct x86_chipset_pit_state *pit,
			    uint8_t control)
{
	uint8_t mode = (uint8_t)((control >> 1u) & 7u);

	if (mode > 5u)
		mode = (uint8_t)(mode - 4u);
	pit->access = (uint8_t)((control >> 4u) & 3u);
	pit->mode = mode;
	pit->bcd = (uint8_t)(control & 1u);
	pit->null_count = 1u;
	pit->write_phase = 0u;
	pit->read_phase = 0u;
	pit->status_latched = 0u;
	pit->running = 0u;
	pit->strobe_pending = 0u;
	pit->output = (uint8_t)(mode == 0u ? 0u : 1u);
}

static void read_back(struct x86_legacy_chipset_owner *chipset,
			 uint8_t control)
{
	size_t channel;

	for (channel = 0u; channel < X86_LEGACY_CHIPSET_PIT_CHANNEL_COUNT;
	     ++channel) {
		uint8_t select = (uint8_t)(1u << (channel + 1u));

		/* 8254 read-back channel selects are active low. */
		if ((control & select) != 0u)
			continue;
		if ((control & 0x20u) == 0u)
			latch_count(&chipset->pit[channel]);
		if ((control & 0x10u) == 0u)
			latch_status(&chipset->pit[channel]);
	}
}

static uint64_t advance_periodic(struct x86_chipset_pit_state *pit,
				 uint64_t input_ticks)
{
	uint64_t complete_periods;
	uint64_t edges;
	uint32_t remainder;
	uint32_t period = reload_count(pit);

	if (period == 0u || input_ticks == 0u)
		return 0u;
	if (input_ticks < (uint64_t)pit->current_count) {
		pit->current_count -= (uint32_t)input_ticks;
		update_periodic_output(pit);
		sync_visible_count(pit);
		return 0u;
	}
	input_ticks -= pit->current_count;
	edges = 1u;
	if (math64_div_u64_u32(input_ticks, period, &complete_periods,
				    &remainder) != MATH64_OK)
		return 0u;
	if ((uint64_t)-1 - edges < complete_periods)
		edges = (uint64_t)-1;
	else
		edges += complete_periods;
	pit->current_count = remainder == 0u ? period : period - remainder;
	update_periodic_output(pit);
	sync_visible_count(pit);
	return edges;
}

static uint64_t advance_one_shot(struct x86_chipset_pit_state *pit,
				 uint64_t input_ticks)
{
	if (input_ticks < (uint64_t)pit->current_count) {
		pit->current_count -= (uint32_t)input_ticks;
		sync_visible_count(pit);
		return 0u;
	}
	pit->current_count = 0u;
	pit->current = 0u;
	pit->output = 1u;
	pit->running = 0u;
	return 1u;
}

static uint64_t advance_strobe(struct x86_chipset_pit_state *pit,
			       uint64_t input_ticks)
{
	if (pit->strobe_pending != 0u) {
		pit->strobe_pending = 0u;
		pit->output = 1u;
		pit->running = 0u;
		return input_ticks == 0u ? 0u : 1u;
	}
	if (input_ticks < (uint64_t)pit->current_count) {
		pit->current_count -= (uint32_t)input_ticks;
		sync_visible_count(pit);
		return 0u;
	}
	input_ticks -= pit->current_count;
	pit->current_count = 0u;
	pit->current = 0u;
	pit->output = 0u;
	pit->strobe_pending = 1u;
	if (input_ticks == 0u)
		return 0u;
	pit->strobe_pending = 0u;
	pit->output = 1u;
	pit->running = 0u;
	return 1u;
}

static uint64_t advance_channel(struct x86_chipset_pit_state *pit,
				uint64_t input_ticks)
{
	if (pit->input_ticks > (uint64_t)-1 - input_ticks)
		pit->input_ticks = (uint64_t)-1;
	else
		pit->input_ticks += input_ticks;
	if (input_ticks == 0u || pit->null_count != 0u ||
	    pit->running == 0u ||
	    (pit->current_count == 0u && pit->strobe_pending == 0u))
		return 0u;
	switch (pit->mode) {
	case 0u:
		return advance_one_shot(pit, input_ticks);
	case 2u:
	case 3u:
		return advance_periodic(pit, input_ticks);
	case 4u:
		return advance_strobe(pit, input_ticks);
	default:
		/* Modes 1 and 5 need a gate-trigger owner which is not bound yet. */
		return 0u;
	}
}

uint64_t x86_legacy_pit_advance(
	struct x86_legacy_chipset_owner *chipset, uint64_t input_ticks)
{
	uint64_t irq0_edges = 0u;
	size_t channel;

	if (chipset == NULL || input_ticks == 0u)
		return 0u;
	for (channel = 0u; channel < X86_LEGACY_CHIPSET_PIT_CHANNEL_COUNT;
	     ++channel) {
		uint64_t edges = advance_channel(&chipset->pit[channel],
						 input_ticks);

		if (channel == 0u)
			irq0_edges = edges;
	}
	return irq0_edges;
}

enum x86_io_callback_status x86_legacy_pit_read(
	kernel_object_handle_t context, uint16_t port,
	enum dos_io_width width, uint32_t *value)
{
	struct x86_legacy_chipset_owner *chipset;
	struct x86_chipset_pit_state *pit;

	chipset = x86_legacy_chipset_active_owner(context);
	if (chipset == NULL || width != DOS_IO_WIDTH_8 || value == NULL)
		return X86_IO_CALLBACK_FAULT;
	if (port == X86_PIT_CONTROL_PORT) {
		*value = 0xffu;
		return X86_IO_CALLBACK_OK;
	}
	pit = pit_for_port(chipset, port);
	if (pit == NULL)
		return X86_IO_CALLBACK_FAULT;
	if (pit->status_latched != 0u) {
		*value = pit->latched_status;
		pit->status_latched = 0u;
		return X86_IO_CALLBACK_OK;
	}
	*value = read_count(pit);
	return X86_IO_CALLBACK_OK;
}

enum x86_io_callback_status x86_legacy_pit_write(
	kernel_object_handle_t context, uint16_t port,
	enum dos_io_width width, uint32_t value)
{
	struct x86_legacy_chipset_owner *chipset;
	struct x86_chipset_pit_state *pit;
	uint8_t control;
	uint8_t channel;

	chipset = x86_legacy_chipset_active_owner(context);
	if (chipset == NULL || width != DOS_IO_WIDTH_8 || value > 0xffu)
		return X86_IO_CALLBACK_FAULT;
	if (port != X86_PIT_CONTROL_PORT) {
		pit = pit_for_port(chipset, port);
		if (pit == NULL)
			return X86_IO_CALLBACK_FAULT;
		write_count(pit, (uint8_t)value);
		return X86_IO_CALLBACK_OK;
	}
	control = (uint8_t)value;
	channel = (uint8_t)(control >> 6u);
	if (channel == 3u) {
		read_back(chipset, control);
		return X86_IO_CALLBACK_OK;
	}
	pit = &chipset->pit[channel];
	if ((control & 0x30u) == 0u)
		latch_count(pit);
	else
		program_channel(pit, control);
	return X86_IO_CALLBACK_OK;
}

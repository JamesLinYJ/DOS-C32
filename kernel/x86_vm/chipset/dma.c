// SPDX-License-Identifier: GPL-2.0-only
/*
 * Guest-only Intel 8237A-compatible ISA DMA register model.
 *
 * No register access performed here reaches the native controller, and
 * programming a channel cannot transfer memory without a separately owned and
 * validated device endpoint.
 */
#include "private.h"

#define DMA_CHANNEL_MASK 0x03u
#define DMA_REQUEST_SET 0x04u
#define DMA_ALL_CHANNELS_MASK 0x0fu
#define DMA_STATUS_TERMINAL_MASK 0x0fu
#define DMA_STATUS_REQUEST_MASK 0xf0u
#define DMA_SECONDARY_PORT_STRIDE 2u
#define DMA_UNDRIVEN_VALUE 0xffu

static const uint8_t page_port_channel[16] = {
	0xffu, 2u, 3u, 1u, 0xffu, 0xffu, 0xffu, 0u,
	0xffu, 6u, 7u, 5u, 0xffu, 0xffu, 0xffu, 4u,
};

static bool decode_page_port(uint16_t port, size_t *controller_index,
			     size_t *channel_index)
{
	uint8_t global_channel;

	if (port < X86_DMA_PAGE_FIRST_PORT ||
	    port > X86_DMA_PAGE_LAST_PORT || controller_index == NULL ||
	    channel_index == NULL)
		return false;
	global_channel = page_port_channel[port - X86_DMA_PAGE_FIRST_PORT];
	if (global_channel == 0xffu)
		return false;
	*controller_index = global_channel /
			    X86_LEGACY_CHIPSET_DMA_CHANNELS_PER_CONTROLLER;
	*channel_index = global_channel %
			 X86_LEGACY_CHIPSET_DMA_CHANNELS_PER_CONTROLLER;
	return true;
}

/*
 * The PC/AT publishes the secondary controller at twice the primary stride.
 * The otherwise unused low address bit is ignored inside this decoded
 * aperture, so each odd port aliases the preceding
 * even register without granting any native access.
 */
static bool decode_controller_port(uint16_t port, size_t *controller_index,
				   uint8_t *logical_port)
{
	uint16_t offset;

	if (controller_index == NULL || logical_port == NULL)
		return false;
	if (port <= X86_DMA_PRIMARY_LAST_PORT) {
		*controller_index = 0u;
		*logical_port = (uint8_t)(port - X86_DMA_PRIMARY_FIRST_PORT);
		return true;
	}
	if (port < X86_DMA_SECONDARY_FIRST_PORT ||
	    port > X86_DMA_SECONDARY_LAST_PORT)
		return false;
	offset = (uint16_t)(port - X86_DMA_SECONDARY_FIRST_PORT);
	*controller_index = 1u;
	*logical_port = (uint8_t)(offset / DMA_SECONDARY_PORT_STRIDE);
	return true;
}

static uint8_t read_channel_register(
	struct x86_chipset_dma_controller_state *controller,
	uint8_t logical_port)
{
	struct x86_chipset_dma_channel_state *channel =
		&controller->channel[logical_port >> 1u];
	uint16_t current = (logical_port & 1u) == 0u
				   ? channel->current_address
				   : channel->current_count;
	uint8_t shift = controller->flip_flop != 0u ? 8u : 0u;
	uint8_t result = (uint8_t)(current >> shift);

	controller->flip_flop ^= 1u;
	return result;
}

static void write_channel_register(
	struct x86_chipset_dma_controller_state *controller,
	uint8_t logical_port, uint8_t value)
{
	struct x86_chipset_dma_channel_state *channel =
		&controller->channel[logical_port >> 1u];
	uint16_t *base = (logical_port & 1u) == 0u ? &channel->base_address
						 : &channel->base_count;
	uint16_t *current = (logical_port & 1u) == 0u
				      ? &channel->current_address
				      : &channel->current_count;

	if (controller->flip_flop == 0u) {
		*base = (uint16_t)((*base & 0xff00u) | value);
		*current = (uint16_t)((*current & 0xff00u) | value);
		controller->flip_flop = 1u;
		return;
	}
	*base = (uint16_t)((*base & 0x00ffu) | ((uint16_t)value << 8u));
	*current = (uint16_t)((*current & 0x00ffu) |
			      ((uint16_t)value << 8u));
	controller->flip_flop = 0u;
}

static uint8_t read_control_register(
	struct x86_chipset_dma_controller_state *controller,
	uint8_t logical_port)
{
	uint8_t register_index = (uint8_t)(logical_port - 8u);

	switch (register_index) {
	case 0u: {
		uint8_t result = controller->status;

		/* Reading status acknowledges only terminal-count indications. */
		controller->status &= DMA_STATUS_REQUEST_MASK;
		return result;
	}
	case 5u:
		return controller->temporary;
	default:
		/* Write-only/illegal register reads leave the ISA bus undriven. */
		return DMA_UNDRIVEN_VALUE;
	}
}

static void master_clear(struct x86_chipset_dma_controller_state *controller)
{
	controller->command = 0u;
	controller->status = 0u;
	controller->mask = DMA_ALL_CHANNELS_MASK;
	controller->flip_flop = 0u;
	controller->temporary = 0u;
}

static void write_control_register(
	struct x86_chipset_dma_controller_state *controller,
	uint8_t logical_port, uint8_t value)
{
	uint8_t register_index = (uint8_t)(logical_port - 8u);
	uint8_t channel = (uint8_t)(value & DMA_CHANNEL_MASK);
	uint8_t channel_bit = (uint8_t)(1u << channel);

	switch (register_index) {
	case 0u:
		controller->command = value;
		break;
	case 1u:
		if ((value & DMA_REQUEST_SET) != 0u)
			controller->status |= (uint8_t)(channel_bit << 4u);
		else
			controller->status &=
				(uint8_t)~(uint8_t)(channel_bit << 4u);
		break;
	case 2u:
		if ((value & DMA_REQUEST_SET) != 0u)
			controller->mask |= channel_bit;
		else
			controller->mask &= (uint8_t)~channel_bit;
		controller->mask &= DMA_ALL_CHANNELS_MASK;
		break;
	case 3u:
		controller->channel[channel].mode = value;
		break;
	case 4u:
		controller->flip_flop = 0u;
		break;
	case 5u:
		master_clear(controller);
		break;
	case 6u:
		controller->mask = 0u;
		break;
	case 7u:
		controller->mask = (uint8_t)(value & DMA_ALL_CHANNELS_MASK);
		break;
	default:
		break;
	}
}

void x86_legacy_dma_initialize_state(
	struct x86_chipset_dma_controller_state
		dma[X86_LEGACY_CHIPSET_DMA_CONTROLLER_COUNT])
{
	size_t index;

	if (dma == NULL)
		return;
	for (index = 0u; index < X86_LEGACY_CHIPSET_DMA_CONTROLLER_COUNT;
	     ++index) {
		dma[index] = (struct x86_chipset_dma_controller_state){
			.mask = DMA_ALL_CHANNELS_MASK,
			.word_addressed = (uint8_t)(index == 1u),
		};
	}
	/* The second controller's channel 0 is the PC/AT cascade for channels
	 * 0-3.  This is the deterministic post-BIOS state established by the
	 * standard firmware initialization sequence. */
	dma[1].channel[0].mode = X86_DMA_MODE_CASCADE;
	dma[1].mask = (uint8_t)(DMA_ALL_CHANNELS_MASK & (uint8_t)~1u);
}

void x86_legacy_dma_snapshot_state(
	const struct x86_chipset_dma_controller_state *dma,
	struct x86_legacy_dma_controller_view *view)
{
	size_t index;

	if (dma == NULL || view == NULL)
		return;
	*view = (struct x86_legacy_dma_controller_view){
		.command = dma->command,
		.status = dma->status,
		.mask = dma->mask,
		.flip_flop = dma->flip_flop,
		.temporary = dma->temporary,
		.word_addressed = dma->word_addressed,
		.reserved = {0u},
	};
	for (index = 0u;
	     index < X86_LEGACY_CHIPSET_DMA_CHANNELS_PER_CONTROLLER;
	     ++index) {
		const struct x86_chipset_dma_channel_state *channel =
			&dma->channel[index];

		view->channel[index] = (struct x86_legacy_dma_channel_view){
			.base_address = channel->base_address,
			.current_address = channel->current_address,
			.base_count = channel->base_count,
			.current_count = channel->current_count,
			.page = channel->page,
			.mode = channel->mode,
			.request_active = (uint8_t)(
				(dma->status >> (index + 4u)) & 1u),
			.terminal_count =
				(uint8_t)((dma->status >> index) & 1u),
			.reserved = {0u},
		};
	}
}

enum x86_io_callback_status x86_legacy_dma_read(
	kernel_object_handle_t context, uint16_t port,
	enum dos_io_width width, uint32_t *value)
{
	struct x86_legacy_chipset_owner *chipset;
	size_t controller_index;
	size_t channel_index;
	uint8_t logical_port;

	chipset = x86_legacy_chipset_active_owner(context);
	if (chipset == NULL || width != DOS_IO_WIDTH_8 || value == NULL)
		return X86_IO_CALLBACK_FAULT;
	if (port >= X86_DMA_PAGE_FIRST_PORT &&
	    port <= X86_DMA_PAGE_LAST_PORT) {
		if (!decode_page_port(port, &controller_index, &channel_index)) {
			*value = DMA_UNDRIVEN_VALUE;
			return X86_IO_CALLBACK_OK;
		}
		*value = chipset->dma[controller_index]
				 .channel[channel_index]
				 .page;
		return X86_IO_CALLBACK_OK;
	}
	if (!decode_controller_port(port, &controller_index, &logical_port))
		return X86_IO_CALLBACK_FAULT;
	if (logical_port < 8u)
		*value = read_channel_register(&chipset->dma[controller_index],
					       logical_port);
	else
		*value = read_control_register(&chipset->dma[controller_index],
					       logical_port);
	return X86_IO_CALLBACK_OK;
}

enum x86_io_callback_status x86_legacy_dma_write(
	kernel_object_handle_t context, uint16_t port,
	enum dos_io_width width, uint32_t value)
{
	struct x86_legacy_chipset_owner *chipset;
	size_t controller_index;
	size_t channel_index;
	uint8_t logical_port;

	chipset = x86_legacy_chipset_active_owner(context);
	if (chipset == NULL || width != DOS_IO_WIDTH_8 || value > 0xffu)
		return X86_IO_CALLBACK_FAULT;
	if (port >= X86_DMA_PAGE_FIRST_PORT &&
	    port <= X86_DMA_PAGE_LAST_PORT) {
		if (!decode_page_port(port, &controller_index, &channel_index))
			return X86_IO_CALLBACK_OK;
		if (controller_index == 1u)
			value &= 0xfeu;
		chipset->dma[controller_index].channel[channel_index].page =
			(uint8_t)value;
		return X86_IO_CALLBACK_OK;
	}
	if (!decode_controller_port(port, &controller_index, &logical_port))
		return X86_IO_CALLBACK_FAULT;
	if (logical_port < 8u)
		write_channel_register(&chipset->dma[controller_index], logical_port,
				       (uint8_t)value);
	else
		write_control_register(&chipset->dma[controller_index], logical_port,
				       (uint8_t)value);
	return X86_IO_CALLBACK_OK;
}

// SPDX-License-Identifier: GPL-2.0-only
/* Bounded 8259A command/data state machine for the isolated guest. */
#include "private.h"

#define PIC_ICW1_INITIALIZE 0x10u
#define PIC_ICW1_LEVEL 0x08u
#define PIC_ICW1_SINGLE 0x02u
#define PIC_ICW1_NEEDS_ICW4 0x01u
#define PIC_OCW3_SELECT 0x08u
#define PIC_OCW3_SPECIAL_MASK_ENABLE 0x40u
#define PIC_OCW3_SPECIAL_MASK 0x20u
#define PIC_OCW3_POLL 0x04u
#define PIC_OCW3_READ_REGISTER 0x02u
#define PIC_OCW3_READ_ISR 0x01u

static struct x86_chipset_pic_state *pic_for_port(
	struct x86_legacy_chipset_owner *chipset, uint16_t port,
	bool *command_port)
{
	if (port == X86_PIC_PRIMARY_COMMAND_PORT ||
	    port == X86_PIC_PRIMARY_DATA_PORT) {
		*command_port = port == X86_PIC_PRIMARY_COMMAND_PORT;
		return &chipset->pic[0];
	}
	if (port == X86_PIC_SECONDARY_COMMAND_PORT ||
	    port == X86_PIC_SECONDARY_DATA_PORT) {
		*command_port = port == X86_PIC_SECONDARY_COMMAND_PORT;
		return &chipset->pic[1];
	}
	return NULL;
}

static bool highest_priority_level(uint8_t bits, uint8_t priority_lowest,
				   uint8_t *level)
{
	uint8_t offset;

	for (offset = 1u; offset <= 8u; ++offset) {
		uint8_t candidate = (uint8_t)((priority_lowest + offset) & 7u);

		if ((bits & (uint8_t)(1u << candidate)) != 0u) {
			*level = candidate;
			return true;
		}
	}
	return false;
}

static uint8_t priority_rank(const struct x86_chipset_pic_state *pic,
			     uint8_t level)
{
	uint8_t highest = (uint8_t)((pic->priority_lowest + 1u) & 7u);

	return (uint8_t)((level - highest) & 7u);
}

static bool next_eligible_level(const struct x86_chipset_pic_state *pic,
				uint8_t *level)
{
	uint8_t effective_isr = pic->in_service;
	uint8_t in_service_level;
	uint8_t pending;

	pending = (uint8_t)(pic->interrupt_request &
			    (uint8_t)~pic->interrupt_mask);
	if (!highest_priority_level(pending, pic->priority_lowest, level))
		return false;
	/* In special-mask mode, a masked in-service level no longer blocks a
	 * lower-priority request.  The ISR bit remains visible until EOI. */
	if (pic->special_mask != 0u)
		effective_isr &= (uint8_t)~pic->interrupt_mask;
	if (!highest_priority_level(effective_isr, pic->priority_lowest,
				    &in_service_level))
		return true;
	return priority_rank(pic, *level) <
	       priority_rank(pic, in_service_level);
}

static void acknowledge_level(struct x86_chipset_pic_state *pic,
			      uint8_t level)
{
	pic->interrupt_request &= (uint8_t)~(uint8_t)(1u << level);
	if (pic->auto_eoi == 0u)
		pic->in_service |= (uint8_t)(1u << level);
	else if (pic->rotate_on_auto_eoi != 0u)
		pic->priority_lowest = level;
}

static bool acknowledge_next(struct x86_chipset_pic_state *pic,
			     uint8_t *level)
{
	if (!next_eligible_level(pic, level))
		return false;
	acknowledge_level(pic, *level);
	return true;
}

static bool cascade_level(const struct x86_legacy_chipset_owner *chipset,
			  uint8_t *level)
{
	uint8_t configured;

	if (chipset->pic[0].single != 0u || chipset->pic[1].single != 0u)
		return false;
	configured = (uint8_t)(chipset->pic[1].cascade_config & 7u);
	if ((chipset->pic[0].cascade_config &
	     (uint8_t)(1u << configured)) == 0u)
		return false;
	*level = configured;
	return true;
}

static void refresh_cascade_request(
	struct x86_legacy_chipset_owner *chipset)
{
	uint8_t configured;
	uint8_t pending_level;

	if (!cascade_level(chipset, &configured))
		return;
	if (next_eligible_level(&chipset->pic[1], &pending_level))
		chipset->pic[0].interrupt_request |=
			(uint8_t)(1u << configured);
	else
		chipset->pic[0].interrupt_request &=
			(uint8_t)~(uint8_t)(1u << configured);
}

static bool clear_non_specific_eoi(struct x86_chipset_pic_state *pic,
				   uint8_t *level)
{
	if (!highest_priority_level(pic->in_service, pic->priority_lowest,
				   level))
		return false;
	pic->in_service &= (uint8_t)~(uint8_t)(1u << *level);
	return true;
}

static void write_icw1(struct x86_chipset_pic_state *pic, uint8_t command)
{
	pic->interrupt_mask = 0u;
	pic->interrupt_request = 0u;
	pic->in_service = 0u;
	pic->initialization_step = 2u;
	pic->cascade_config = 0u;
	pic->priority_lowest = 7u;
	pic->read_isr = 0u;
	pic->mode_8086 = 0u;
	pic->auto_eoi = 0u;
	pic->special_mask = 0u;
	pic->rotate_on_auto_eoi = 0u;
	pic->single = (uint8_t)((command & PIC_ICW1_SINGLE) != 0u);
	pic->level_triggered = (uint8_t)((command & PIC_ICW1_LEVEL) != 0u);
	pic->poll_pending = 0u;
	pic->needs_icw4 =
		(uint8_t)((command & PIC_ICW1_NEEDS_ICW4) != 0u);
}

static void write_ocw3(struct x86_chipset_pic_state *pic, uint8_t command)
{
	if ((command & PIC_OCW3_SPECIAL_MASK_ENABLE) != 0u) {
		pic->special_mask =
			(uint8_t)((command & PIC_OCW3_SPECIAL_MASK) != 0u);
	}
	if ((command & PIC_OCW3_POLL) != 0u)
		pic->poll_pending = 1u;
	if ((command & PIC_OCW3_READ_REGISTER) != 0u) {
		pic->read_isr =
			(uint8_t)((command & PIC_OCW3_READ_ISR) != 0u);
	}
}

static void write_ocw2(struct x86_chipset_pic_state *pic, uint8_t command)
{
	uint8_t operation = (uint8_t)((command >> 5u) & 7u);
	uint8_t level = (uint8_t)(command & 7u);
	uint8_t completed_level = 0u;

	switch (operation) {
	case 0u:
		pic->rotate_on_auto_eoi = 0u;
		break;
	case 1u:
		(void)clear_non_specific_eoi(pic, &completed_level);
		break;
	case 2u:
		break;
	case 3u:
		pic->in_service &= (uint8_t)~(uint8_t)(1u << level);
		break;
	case 4u:
		pic->rotate_on_auto_eoi = 1u;
		break;
	case 5u:
		if (clear_non_specific_eoi(pic, &completed_level))
			pic->priority_lowest = completed_level;
		break;
	case 6u:
		pic->priority_lowest = level;
		break;
	case 7u:
		pic->in_service &= (uint8_t)~(uint8_t)(1u << level);
		pic->priority_lowest = level;
		break;
	}
}

static void write_data(struct x86_chipset_pic_state *pic, uint8_t data)
{
	switch (pic->initialization_step) {
	case 0u:
		pic->interrupt_mask = data;
		break;
	case 2u:
		pic->vector_base = (uint8_t)(data & 0xf8u);
		if (pic->single != 0u)
			pic->initialization_step =
				pic->needs_icw4 != 0u ? 4u : 0u;
		else
			pic->initialization_step = 3u;
		break;
	case 3u:
		pic->cascade_config = data;
		pic->initialization_step = pic->needs_icw4 != 0u ? 4u : 0u;
		break;
	case 4u:
		pic->mode_8086 = (uint8_t)(data & 1u);
		pic->auto_eoi = (uint8_t)((data >> 1u) & 1u);
		pic->initialization_step = 0u;
		break;
	default:
		pic->initialization_step = 0u;
		break;
	}
}

enum x86_io_callback_status x86_legacy_pic_read(
	kernel_object_handle_t context, uint16_t port,
	enum dos_io_width width, uint32_t *value)
{
	struct x86_legacy_chipset_owner *chipset;
	struct x86_chipset_pic_state *pic;
	bool command_port = false;
	uint8_t level;

	chipset = x86_legacy_chipset_active_owner(context);
	if (chipset == NULL || width != DOS_IO_WIDTH_8 || value == NULL)
		return X86_IO_CALLBACK_FAULT;
	pic = pic_for_port(chipset, port, &command_port);
	if (pic == NULL)
		return X86_IO_CALLBACK_FAULT;
	if (!command_port) {
		*value = pic->interrupt_mask;
		return X86_IO_CALLBACK_OK;
	}
	if (pic->poll_pending != 0u) {
		pic->poll_pending = 0u;
		if (!acknowledge_next(pic, &level)) {
			*value = 0u;
			return X86_IO_CALLBACK_OK;
		}
		refresh_cascade_request(chipset);
		*value = (uint32_t)(0x80u | level);
		return X86_IO_CALLBACK_OK;
	}
	*value = pic->read_isr != 0u ? pic->in_service
				      : pic->interrupt_request;
	return X86_IO_CALLBACK_OK;
}

enum x86_io_callback_status x86_legacy_pic_write(
	kernel_object_handle_t context, uint16_t port,
	enum dos_io_width width, uint32_t value)
{
	struct x86_legacy_chipset_owner *chipset;
	struct x86_chipset_pic_state *pic;
	bool command_port = false;
	uint8_t byte;

	chipset = x86_legacy_chipset_active_owner(context);
	if (chipset == NULL || width != DOS_IO_WIDTH_8 || value > 0xffu)
		return X86_IO_CALLBACK_FAULT;
	pic = pic_for_port(chipset, port, &command_port);
	if (pic == NULL)
		return X86_IO_CALLBACK_FAULT;
	byte = (uint8_t)value;
	if (!command_port) {
		write_data(pic, byte);
		refresh_cascade_request(chipset);
		return X86_IO_CALLBACK_OK;
	}
	if ((byte & PIC_ICW1_INITIALIZE) != 0u)
		write_icw1(pic, byte);
	else if ((byte & PIC_OCW3_SELECT) != 0u)
		write_ocw3(pic, byte);
	else
		write_ocw2(pic, byte);
	refresh_cascade_request(chipset);
	return X86_IO_CALLBACK_OK;
}

bool x86_legacy_pic_request_irq(
	struct x86_legacy_chipset_owner *chipset, uint8_t irq)
{
	struct x86_chipset_pic_state *pic;
	uint8_t level;
	bool already_pending;

	if (chipset == NULL || irq >= X86_LEGACY_CHIPSET_IRQ_COUNT)
		return true;
	if (irq < 8u) {
		pic = &chipset->pic[0];
		level = irq;
	} else {
		pic = &chipset->pic[1];
		level = (uint8_t)(irq - 8u);
	}
	already_pending =
		(pic->interrupt_request & (uint8_t)(1u << level)) != 0u;
	pic->interrupt_request |= (uint8_t)(1u << level);
	if (irq >= 8u)
		refresh_cascade_request(chipset);
	return already_pending;
}

bool x86_legacy_pic_prepare_interrupt(
	const struct x86_legacy_chipset_owner *chipset,
	struct x86_legacy_interrupt_claim *claim)
{
	struct x86_legacy_interrupt_claim prepared = {0};
	struct x86_legacy_chipset_owner sampled;
	uint8_t primary_level;
	uint8_t secondary_level;
	uint8_t configured_cascade;

	/* Refresh only derives the primary cascade IRR bit.  Work on a private
	 * copy so preparing delivery cannot change any guest-visible PIC state. */
	if (chipset == NULL || claim == NULL)
		return false;
	sampled = *chipset;
	refresh_cascade_request(&sampled);
	if (!next_eligible_level(&sampled.pic[0], &primary_level))
		return false;
	if (cascade_level(&sampled, &configured_cascade) &&
	    primary_level == configured_cascade &&
	    next_eligible_level(&sampled.pic[1], &secondary_level)) {
		prepared.irq = (uint8_t)(8u + secondary_level);
		prepared.vector = (uint8_t)(sampled.pic[1].vector_base +
					    secondary_level);
		prepared.primary_auto_eoi = sampled.pic[0].auto_eoi;
		prepared.secondary_auto_eoi = sampled.pic[1].auto_eoi;
		prepared.cascaded = 1u;
	} else {
		prepared.irq = primary_level;
		prepared.vector = (uint8_t)(sampled.pic[0].vector_base +
					    primary_level);
		prepared.primary_auto_eoi = sampled.pic[0].auto_eoi;
	}
	prepared.chipset_generation = sampled.generation;
	*claim = prepared;
	return true;
}

static bool direct_claim_matches(const struct x86_legacy_chipset_owner *chipset,
				 const struct x86_legacy_interrupt_claim *claim)
{
	uint8_t level = claim->irq;

	return claim->cascaded == 0u && level < 8u &&
	       claim->vector == (uint8_t)(chipset->pic[0].vector_base + level) &&
	       claim->primary_auto_eoi == chipset->pic[0].auto_eoi &&
	       claim->secondary_auto_eoi == 0u &&
	       (chipset->pic[0].interrupt_request &
		(uint8_t)(1u << level)) != 0u;
}

static bool cascaded_claim_matches(
	const struct x86_legacy_chipset_owner *chipset,
	const struct x86_legacy_interrupt_claim *claim, uint8_t *primary_level)
{
	uint8_t secondary_level;

	if (claim->cascaded != 1u || claim->irq < 8u || claim->irq >= 16u ||
	    !cascade_level(chipset, primary_level))
		return false;
	secondary_level = (uint8_t)(claim->irq - 8u);
	return claim->vector ==
		       (uint8_t)(chipset->pic[1].vector_base + secondary_level) &&
	       claim->primary_auto_eoi == chipset->pic[0].auto_eoi &&
	       claim->secondary_auto_eoi == chipset->pic[1].auto_eoi &&
	       (chipset->pic[0].interrupt_request &
		(uint8_t)(1u << *primary_level)) != 0u &&
	       (chipset->pic[1].interrupt_request &
		(uint8_t)(1u << secondary_level)) != 0u;
}

bool x86_legacy_pic_commit_interrupt(
	struct x86_legacy_chipset_owner *chipset,
	const struct x86_legacy_interrupt_claim *claim)
{
	struct x86_legacy_chipset_owner committed;
	uint8_t primary_level = 0u;

	if (chipset == NULL || claim == NULL ||
	    claim->chipset_generation != chipset->generation)
		return false;
	/* Validate and perform the complete intack on a private copy.  The chipset
	 * owner serializes PIC mutation, so publishing both PICs after every check is
	 * the transaction boundary; failure leaves the live pair byte-exact. */
	committed = *chipset;
	refresh_cascade_request(&committed);
	if (direct_claim_matches(&committed, claim)) {
		acknowledge_level(&committed.pic[0], claim->irq);
		refresh_cascade_request(&committed);
	} else {
		if (!cascaded_claim_matches(&committed, claim, &primary_level))
			return false;
		acknowledge_level(&committed.pic[0], primary_level);
		acknowledge_level(&committed.pic[1],
				  (uint8_t)(claim->irq - 8u));
		refresh_cascade_request(&committed);
	}
	chipset->pic[0] = committed.pic[0];
	chipset->pic[1] = committed.pic[1];
	return true;
}

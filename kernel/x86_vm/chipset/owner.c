// SPDX-License-Identifier: GPL-2.0-only
/*
 * Single-owner lifecycle and I/O-resource publication for guest chipset state.
 *
 * Safety design: configuration is validated before a private state is
 * prepared, I/O descriptors are registered as one batch by the caller, and
 * callbacks remain closed until an explicit publish.  Abort preserves the
 * non-reusing lifetime generation; an uncertain unwind is sticky-poisoned.
 */
#include "private.h"

#define X86_CHIPSET_GENERATION_MAX ((uint64_t)-2)
#define X86_CHIPSET_INTERRUPT_TOKEN_MAX ((uint64_t)-2)
#define X86_RTC_STATUS_B_UNSUPPORTED 0x78u

static struct x86_legacy_chipset_owner runtime_owner;

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

static bool year_is_leap(uint16_t year)
{
	return (year % 4u) == 0u &&
	       ((year % 100u) != 0u || (year % 400u) == 0u);
}

static uint8_t days_in_month(uint16_t year, uint8_t month)
{
	static const uint8_t month_days[12] = {
		31u, 28u, 31u, 30u, 31u, 30u,
		31u, 31u, 30u, 31u, 30u, 31u,
	};

	if (month == 0u || month > ARRAY_SIZE(month_days))
		return 0u;
	if (month == 2u && year_is_leap(year))
		return 29u;
	return month_days[month - 1u];
}

static bool vector_ranges_overlap(uint8_t first, uint8_t second)
{
	uint16_t first_last = (uint16_t)first + 7u;
	uint16_t second_last = (uint16_t)second + 7u;

	return (uint16_t)first <= second_last &&
	       (uint16_t)second <= first_last;
}

static bool config_is_valid(const struct x86_legacy_chipset_config *config)
{
	size_t index;
	uint8_t month_days;

	if (config == NULL ||
	    (config->pic_vector_base[0] & 7u) != 0u ||
	    (config->pic_vector_base[1] & 7u) != 0u ||
	    vector_ranges_overlap(config->pic_vector_base[0],
				  config->pic_vector_base[1]) ||
	    (config->rtc_status_a & 0x80u) != 0u ||
	    (config->rtc_status_b & X86_RTC_STATUS_B_UNSUPPORTED) != 0u ||
	    config->rtc_valid > 1u || config->pic_cascade_config[1] > 7u ||
	    (config->pic_cascade_config[0] &
	     (uint8_t)(1u << config->pic_cascade_config[1])) == 0u)
		return false;
	if (config->rtc_valid != 0u) {
		if (config->rtc_year == 0u || config->rtc_year > 9999u ||
		    config->rtc_second > 59u || config->rtc_minute > 59u ||
		    config->rtc_hour > 23u || config->rtc_weekday == 0u ||
		    config->rtc_weekday > 7u || config->rtc_month == 0u ||
		    config->rtc_month > 12u)
			return false;
		month_days = days_in_month(config->rtc_year, config->rtc_month);
		if (config->rtc_day == 0u || config->rtc_day > month_days)
			return false;
	} else if (config->rtc_year != 0u || config->rtc_second != 0u ||
		   config->rtc_minute != 0u || config->rtc_hour != 0u ||
		   config->rtc_weekday != 0u || config->rtc_day != 0u ||
		   config->rtc_month != 0u) {
		return false;
	}
	for (index = 0u; index < X86_LEGACY_CHIPSET_PIT_CHANNEL_COUNT;
	     ++index) {
		if (config->pit_access[index] == 0u ||
		    config->pit_access[index] > 3u ||
		    config->pit_mode[index] > 5u ||
		    config->pit_bcd[index] > 1u)
			return false;
	}
	return true;
}

static struct x86_io_resource_descriptor descriptor(
	kernel_object_handle_t owner_identity,
	kernel_object_handle_t context_identity, uint16_t first_port,
	uint16_t last_port, x86_io_read_callback_t read,
	x86_io_write_callback_t write)
{
	return (struct x86_io_resource_descriptor){
		.owner_identity = owner_identity,
		.callback_context = context_identity,
		.read = read,
		.write = write,
		.first_port = first_port,
		.last_port = last_port,
		.read_width_mask = X86_IO_WIDTH_MASK_8,
		.write_width_mask = X86_IO_WIDTH_MASK_8,
		.read_action = X86_IO_RESOURCE_ACTION_EMULATE,
		.write_action = X86_IO_RESOURCE_ACTION_EMULATE,
		.flags = 0u,
		.reserved = {0u},
	};
}

static void initialize_pic(struct x86_chipset_pic_state *pic,
			   uint8_t vector_base, uint8_t interrupt_mask,
			   uint8_t cascade_config)
{
	*pic = (struct x86_chipset_pic_state){
		.vector_base = vector_base,
		.interrupt_mask = interrupt_mask,
		.cascade_config = cascade_config,
		.priority_lowest = 7u,
		.mode_8086 = 1u,
	};
}

static void initialize_rtc(struct x86_chipset_rtc_state *rtc,
			   const struct x86_legacy_chipset_config *config)
{
	size_t index;

	for (index = 0u; index < ARRAY_SIZE(rtc->cmos); ++index)
		rtc->cmos[index] = 0u;
	rtc->year = config->rtc_year;
	rtc->second = config->rtc_second;
	rtc->minute = config->rtc_minute;
	rtc->hour = config->rtc_hour;
	rtc->weekday = config->rtc_weekday;
	rtc->day = config->rtc_day;
	rtc->month = config->rtc_month;
	rtc->selected_register = 0u;
	rtc->nmi_disabled = 0u;
	rtc->status_a = config->rtc_status_a;
	rtc->status_b = config->rtc_status_b;
	rtc->status_c = 0u;
	rtc->status_d = (uint8_t)(config->rtc_valid != 0u ? 0x80u : 0u);
}

static void prepare_owner(struct x86_legacy_chipset_owner *prepared,
			  kernel_object_handle_t context_identity,
			  kernel_object_handle_t owner_identity,
			  const struct x86_legacy_chipset_config *config,
			  uint64_t generation)
{
	size_t index;

	*prepared = (struct x86_legacy_chipset_owner){
		.context_identity = context_identity,
		.owner_identity = owner_identity,
		.interrupt_source_identity = KERNEL_OBJECT_HANDLE_INVALID,
		.generation = generation,
		.phase = X86_CHIPSET_PREPARED,
	};
	for (index = 0u; index < X86_LEGACY_CHIPSET_PIC_COUNT; ++index) {
		initialize_pic(&prepared->pic[index],
			       config->pic_vector_base[index],
			       config->pic_mask[index],
			       config->pic_cascade_config[index]);
	}
	for (index = 0u; index < X86_LEGACY_CHIPSET_PIT_CHANNEL_COUNT;
	     ++index) {
		x86_legacy_pit_initialize_state(
			&prepared->pit[index], config->pit_reload[index],
			config->pit_access[index], config->pit_mode[index],
			config->pit_bcd[index]);
	}
	initialize_rtc(&prepared->rtc, config);
	x86_legacy_dma_initialize_state(prepared->dma);
}

struct x86_legacy_chipset_owner *x86_legacy_chipset_active_owner(
	kernel_object_handle_t context_identity)
{
	if (runtime_owner.phase != X86_CHIPSET_ACTIVE ||
	    runtime_owner.context_identity != context_identity ||
	    runtime_owner.interrupt_prepared != 0u)
		return NULL;
	return &runtime_owner;
}

void x86_legacy_chipset_poison_internal(
	kernel_object_handle_t context_identity)
{
	if (runtime_owner.context_identity == context_identity &&
	    runtime_owner.phase != X86_CHIPSET_EMPTY)
		runtime_owner.phase = X86_CHIPSET_POISONED;
}

enum x86_legacy_chipset_status x86_legacy_chipset_prepare(
	kernel_object_handle_t context_identity,
	kernel_object_handle_t owner_identity,
	const struct x86_legacy_chipset_config *config,
	struct x86_io_resource_descriptor *descriptors,
	size_t descriptor_capacity)
{
	struct x86_io_resource_descriptor prepared_descriptors[
		X86_LEGACY_CHIPSET_RESOURCE_COUNT];
	struct x86_legacy_chipset_owner prepared_owner;
	uint64_t generation;
	size_t index;

	if (!identity_is_valid(context_identity) ||
	    !identity_is_valid(owner_identity) ||
	    context_identity == owner_identity || descriptors == NULL ||
	    !config_is_valid(config))
		return X86_LEGACY_CHIPSET_INVALID_ARGUMENT;
	if (descriptor_capacity < X86_LEGACY_CHIPSET_RESOURCE_COUNT)
		return X86_LEGACY_CHIPSET_CAPACITY_EXHAUSTED;
	if (runtime_owner.phase == X86_CHIPSET_POISONED)
		return X86_LEGACY_CHIPSET_POISONED;
	if (runtime_owner.phase != X86_CHIPSET_EMPTY)
		return X86_LEGACY_CHIPSET_INVALID_STATE;
	if (runtime_owner.generation >= X86_CHIPSET_GENERATION_MAX)
		return X86_LEGACY_CHIPSET_CAPACITY_EXHAUSTED;
	generation = runtime_owner.generation + 1u;
	prepare_owner(&prepared_owner, context_identity, owner_identity, config,
		      generation);
	prepared_descriptors[0] = descriptor(
		owner_identity, context_identity, X86_PIC_PRIMARY_COMMAND_PORT,
		X86_PIC_PRIMARY_DATA_PORT, x86_legacy_pic_read,
		x86_legacy_pic_write);
	prepared_descriptors[1] = descriptor(
		owner_identity, context_identity, X86_PIC_SECONDARY_COMMAND_PORT,
		X86_PIC_SECONDARY_DATA_PORT, x86_legacy_pic_read,
		x86_legacy_pic_write);
	prepared_descriptors[2] = descriptor(
		owner_identity, context_identity, X86_PIT_CHANNEL0_PORT,
		X86_PIT_CONTROL_PORT, x86_legacy_pit_read,
		x86_legacy_pit_write);
	prepared_descriptors[3] = descriptor(
		owner_identity, context_identity, X86_RTC_INDEX_PORT,
		X86_RTC_DATA_PORT, x86_legacy_rtc_read,
		x86_legacy_rtc_write);
	prepared_descriptors[4] = descriptor(
		owner_identity, context_identity, X86_DMA_PRIMARY_FIRST_PORT,
		X86_DMA_PRIMARY_LAST_PORT, x86_legacy_dma_read,
		x86_legacy_dma_write);
	prepared_descriptors[5] = descriptor(
		owner_identity, context_identity,
		X86_DMA_PAGE_PRIMARY_CHANNELS123_FIRST_PORT,
		X86_DMA_PAGE_PRIMARY_CHANNELS123_LAST_PORT, x86_legacy_dma_read,
		x86_legacy_dma_write);
	prepared_descriptors[6] = descriptor(
		owner_identity, context_identity,
		X86_DMA_PAGE_PRIMARY_CHANNEL0_PORT,
		X86_DMA_PAGE_PRIMARY_CHANNEL0_PORT, x86_legacy_dma_read,
		x86_legacy_dma_write);
	prepared_descriptors[7] = descriptor(
		owner_identity, context_identity,
		X86_DMA_PAGE_SECONDARY_CHANNELS567_FIRST_PORT,
		X86_DMA_PAGE_SECONDARY_CHANNELS567_LAST_PORT, x86_legacy_dma_read,
		x86_legacy_dma_write);
	prepared_descriptors[8] = descriptor(
		owner_identity, context_identity,
		X86_DMA_PAGE_SECONDARY_CHANNEL4_PORT,
		X86_DMA_PAGE_SECONDARY_CHANNEL4_PORT, x86_legacy_dma_read,
		x86_legacy_dma_write);
	prepared_descriptors[9] = descriptor(
		owner_identity, context_identity, X86_DMA_SECONDARY_FIRST_PORT,
		X86_DMA_SECONDARY_LAST_PORT, x86_legacy_dma_read,
		x86_legacy_dma_write);

	runtime_owner = prepared_owner;
	for (index = 0u; index < ARRAY_SIZE(prepared_descriptors); ++index)
		descriptors[index] = prepared_descriptors[index];
	return X86_LEGACY_CHIPSET_OK;
}

enum x86_legacy_chipset_status x86_legacy_chipset_publish(
	kernel_object_handle_t context_identity)
{
	if (!identity_is_valid(context_identity))
		return X86_LEGACY_CHIPSET_INVALID_ARGUMENT;
	if (runtime_owner.phase == X86_CHIPSET_POISONED)
		return X86_LEGACY_CHIPSET_POISONED;
	if (runtime_owner.context_identity != context_identity)
		return X86_LEGACY_CHIPSET_IDENTITY_MISMATCH;
	if (runtime_owner.phase != X86_CHIPSET_PREPARED)
		return X86_LEGACY_CHIPSET_INVALID_STATE;
	runtime_owner.phase = X86_CHIPSET_ACTIVE;
	return X86_LEGACY_CHIPSET_OK;
}

enum x86_legacy_chipset_status x86_legacy_chipset_abort(
	kernel_object_handle_t context_identity)
{
	uint64_t generation;

	if (!identity_is_valid(context_identity))
		return X86_LEGACY_CHIPSET_INVALID_ARGUMENT;
	if (runtime_owner.phase == X86_CHIPSET_POISONED)
		return X86_LEGACY_CHIPSET_POISONED;
	if (runtime_owner.context_identity != context_identity)
		return X86_LEGACY_CHIPSET_IDENTITY_MISMATCH;
	if (runtime_owner.phase != X86_CHIPSET_PREPARED)
		return X86_LEGACY_CHIPSET_INVALID_STATE;
	generation = runtime_owner.generation;
	runtime_owner = (struct x86_legacy_chipset_owner){
		.generation = generation,
	};
	return X86_LEGACY_CHIPSET_OK;
}

enum x86_legacy_chipset_status x86_legacy_chipset_poison(
	kernel_object_handle_t context_identity)
{
	if (!identity_is_valid(context_identity))
		return X86_LEGACY_CHIPSET_INVALID_ARGUMENT;
	if (runtime_owner.context_identity != context_identity)
		return X86_LEGACY_CHIPSET_IDENTITY_MISMATCH;
	if (runtime_owner.phase == X86_CHIPSET_EMPTY)
		return X86_LEGACY_CHIPSET_INVALID_STATE;
	runtime_owner.phase = X86_CHIPSET_POISONED;
	return X86_LEGACY_CHIPSET_OK;
}

static bool source_config_is_valid(
	const struct x86_legacy_chipset_source_config *config)
{
	if (config == NULL ||
	    !bytes_are_zero(config->reserved, ARRAY_SIZE(config->reserved)) ||
	    config->capabilities == 0u ||
	    (config->capabilities &
	     (uint32_t)~X86_LEGACY_CHIPSET_SOURCE_CAPABILITIES) != 0u ||
	    config->pit_rate_calibrated > 1u)
		return false;
	if ((config->capabilities & X86_LEGACY_CHIPSET_SOURCE_PIT_CLOCK) != 0u)
		return config->pit_input_quantum != 0u;
	return config->pit_input_quantum == 0u &&
	       config->pit_rate_calibrated == 0u;
}

static bool source_event_is_valid(
	const struct x86_legacy_chipset_source_event *event)
{
	if (event == NULL ||
	    !bytes_are_zero(event->reserved, ARRAY_SIZE(event->reserved)))
		return false;
	if (event->kind == (uint8_t)X86_LEGACY_CHIPSET_EVENT_PIT_CLOCK)
		return event->pit_input_ticks != 0u && event->irq == 0u;
	if (event->kind == (uint8_t)X86_LEGACY_CHIPSET_EVENT_IRQ_EDGE)
		return event->pit_input_ticks == 0u &&
		       event->irq < X86_LEGACY_CHIPSET_IRQ_COUNT;
	return false;
}

static uint64_t saturating_add_u64(uint64_t left, uint64_t right)
{
	if ((uint64_t)-1 - left < right)
		return (uint64_t)-1;
	return left + right;
}

enum x86_legacy_chipset_status x86_legacy_chipset_source_bind(
	kernel_object_handle_t context_identity,
	kernel_object_handle_t owner_identity,
	kernel_object_handle_t source_identity,
	const struct x86_legacy_chipset_source_config *config)
{
	if (!identity_is_valid(context_identity) ||
	    !identity_is_valid(owner_identity) ||
	    !identity_is_valid(source_identity) ||
	    source_identity == context_identity ||
	    source_identity == owner_identity || !source_config_is_valid(config))
		return X86_LEGACY_CHIPSET_INVALID_ARGUMENT;
	if (runtime_owner.phase == X86_CHIPSET_POISONED)
		return X86_LEGACY_CHIPSET_POISONED;
	if (runtime_owner.context_identity != context_identity ||
	    runtime_owner.owner_identity != owner_identity)
		return X86_LEGACY_CHIPSET_IDENTITY_MISMATCH;
	if (runtime_owner.phase != X86_CHIPSET_ACTIVE ||
	    runtime_owner.interrupt_source_phase != X86_CHIPSET_SOURCE_EMPTY)
		return X86_LEGACY_CHIPSET_INVALID_STATE;
	if (runtime_owner.interrupt_source_generation == (uint64_t)-1)
		return X86_LEGACY_CHIPSET_CAPACITY_EXHAUSTED;

	/* All validation precedes this single-owner publication. */
	runtime_owner.interrupt_source_identity = source_identity;
	runtime_owner.interrupt_source_generation++;
	runtime_owner.pit_input_quantum = config->pit_input_quantum;
	runtime_owner.interrupt_source_capabilities = config->capabilities;
	runtime_owner.pit_rate_calibrated = config->pit_rate_calibrated;
	runtime_owner.interrupt_source_phase = X86_CHIPSET_SOURCE_ACTIVE;
	return X86_LEGACY_CHIPSET_OK;
}

static enum x86_legacy_chipset_status source_identity_status(
	kernel_object_handle_t context_identity,
	kernel_object_handle_t source_identity)
{
	if (!identity_is_valid(context_identity) ||
	    !identity_is_valid(source_identity))
		return X86_LEGACY_CHIPSET_INVALID_ARGUMENT;
	if (runtime_owner.phase == X86_CHIPSET_POISONED)
		return X86_LEGACY_CHIPSET_POISONED;
	if (runtime_owner.context_identity != context_identity ||
	    runtime_owner.interrupt_source_identity != source_identity)
		return X86_LEGACY_CHIPSET_IDENTITY_MISMATCH;
	if (runtime_owner.phase != X86_CHIPSET_ACTIVE ||
	    runtime_owner.interrupt_source_phase == X86_CHIPSET_SOURCE_EMPTY)
		return X86_LEGACY_CHIPSET_INVALID_STATE;
	return X86_LEGACY_CHIPSET_OK;
}

enum x86_legacy_chipset_status x86_legacy_chipset_source_quiesce(
	kernel_object_handle_t context_identity,
	kernel_object_handle_t source_identity)
{
	enum x86_legacy_chipset_status status = source_identity_status(
		context_identity, source_identity);

	if (status != X86_LEGACY_CHIPSET_OK)
		return status;
	if (runtime_owner.interrupt_source_phase != X86_CHIPSET_SOURCE_ACTIVE)
		return X86_LEGACY_CHIPSET_INVALID_STATE;
	if (runtime_owner.interrupt_prepared != 0u)
		return X86_LEGACY_CHIPSET_INVALID_STATE;
	runtime_owner.interrupt_source_phase = X86_CHIPSET_SOURCE_QUIESCED;
	return X86_LEGACY_CHIPSET_OK;
}

enum x86_legacy_chipset_status x86_legacy_chipset_source_resume(
	kernel_object_handle_t context_identity,
	kernel_object_handle_t source_identity)
{
	enum x86_legacy_chipset_status status = source_identity_status(
		context_identity, source_identity);

	if (status != X86_LEGACY_CHIPSET_OK)
		return status;
	if (runtime_owner.interrupt_source_phase != X86_CHIPSET_SOURCE_QUIESCED)
		return X86_LEGACY_CHIPSET_INVALID_STATE;
	runtime_owner.interrupt_source_phase = X86_CHIPSET_SOURCE_ACTIVE;
	return X86_LEGACY_CHIPSET_OK;
}

enum x86_legacy_chipset_status x86_legacy_chipset_source_unbind(
	kernel_object_handle_t context_identity,
	kernel_object_handle_t source_identity)
{
	enum x86_legacy_chipset_status status = source_identity_status(
		context_identity, source_identity);
	uint64_t source_generation;

	if (status != X86_LEGACY_CHIPSET_OK)
		return status;
	if (runtime_owner.interrupt_source_phase != X86_CHIPSET_SOURCE_QUIESCED)
		return X86_LEGACY_CHIPSET_INVALID_STATE;
	source_generation = runtime_owner.interrupt_source_generation;
	runtime_owner.interrupt_source_identity = KERNEL_OBJECT_HANDLE_INVALID;
	runtime_owner.interrupt_source_generation = source_generation;
	runtime_owner.pit_input_quantum = 0u;
	runtime_owner.interrupt_source_capabilities = 0u;
	runtime_owner.pit_rate_calibrated = 0u;
	runtime_owner.interrupt_source_phase = X86_CHIPSET_SOURCE_EMPTY;
	return X86_LEGACY_CHIPSET_OK;
}

enum x86_legacy_chipset_status x86_legacy_chipset_source_submit(
	kernel_object_handle_t context_identity,
	kernel_object_handle_t source_identity,
	const struct x86_legacy_chipset_source_event *event)
{
	uint64_t coalesced;
	uint64_t irq0_edges;
	bool already_pending;

	if (!identity_is_valid(context_identity) ||
	    !identity_is_valid(source_identity) || !source_event_is_valid(event))
		return X86_LEGACY_CHIPSET_INVALID_ARGUMENT;
	if (runtime_owner.phase == X86_CHIPSET_POISONED)
		return X86_LEGACY_CHIPSET_POISONED;
	if (runtime_owner.context_identity != context_identity ||
	    runtime_owner.interrupt_source_identity != source_identity)
		return X86_LEGACY_CHIPSET_IDENTITY_MISMATCH;
	if (runtime_owner.phase != X86_CHIPSET_ACTIVE ||
	    runtime_owner.interrupt_source_phase != X86_CHIPSET_SOURCE_ACTIVE)
		return X86_LEGACY_CHIPSET_INVALID_STATE;

	if (event->kind == (uint8_t)X86_LEGACY_CHIPSET_EVENT_IRQ_EDGE) {
		if ((runtime_owner.interrupt_source_capabilities &
		     X86_LEGACY_CHIPSET_SOURCE_IRQ_EDGE) == 0u)
			return X86_LEGACY_CHIPSET_INVALID_STATE;
		(void)x86_legacy_pic_request_irq(&runtime_owner, event->irq);
		return X86_LEGACY_CHIPSET_OK;
	}
	if ((runtime_owner.interrupt_source_capabilities &
	     X86_LEGACY_CHIPSET_SOURCE_PIT_CLOCK) == 0u)
		return X86_LEGACY_CHIPSET_INVALID_STATE;
	x86_legacy_rtc_advance(&runtime_owner.rtc, event->pit_input_ticks);
	irq0_edges = x86_legacy_pit_advance(&runtime_owner,
					    event->pit_input_ticks);
	if (irq0_edges == 0u)
		return X86_LEGACY_CHIPSET_OK;
	already_pending = x86_legacy_pic_request_irq(&runtime_owner, 0u);
	coalesced = irq0_edges - 1u;
	if (already_pending)
		coalesced = saturating_add_u64(coalesced, 1u);
	runtime_owner.pit_irq0_edges = saturating_add_u64(
		runtime_owner.pit_irq0_edges, irq0_edges);
	runtime_owner.pit_irq0_coalesced_edges = saturating_add_u64(
		runtime_owner.pit_irq0_coalesced_edges, coalesced);
	return X86_LEGACY_CHIPSET_OK;
}

enum x86_legacy_chipset_status x86_legacy_chipset_interrupt_claim(
	kernel_object_handle_t context_identity,
	kernel_object_handle_t owner_identity,
	struct x86_legacy_interrupt_claim *claim)
{
	enum x86_legacy_chipset_status status;

	status = x86_legacy_chipset_interrupt_prepare(
		context_identity, owner_identity, claim);
	if (status != X86_LEGACY_CHIPSET_OK)
		return status;
	return x86_legacy_chipset_interrupt_commit(
		context_identity, owner_identity, claim);
}

static enum x86_legacy_chipset_status interrupt_owner_status(
	kernel_object_handle_t context_identity,
	kernel_object_handle_t owner_identity)
{
	if (!identity_is_valid(context_identity) ||
	    !identity_is_valid(owner_identity))
		return X86_LEGACY_CHIPSET_INVALID_ARGUMENT;
	if (runtime_owner.phase == X86_CHIPSET_POISONED)
		return X86_LEGACY_CHIPSET_POISONED;
	if (runtime_owner.context_identity != context_identity ||
	    runtime_owner.owner_identity != owner_identity)
		return X86_LEGACY_CHIPSET_IDENTITY_MISMATCH;
	if (runtime_owner.phase != X86_CHIPSET_ACTIVE ||
	    runtime_owner.interrupt_source_phase != X86_CHIPSET_SOURCE_ACTIVE)
		return X86_LEGACY_CHIPSET_INVALID_STATE;
	return X86_LEGACY_CHIPSET_OK;
}

static bool interrupt_claim_matches(
	const struct x86_legacy_interrupt_claim *left,
	const struct x86_legacy_interrupt_claim *right)
{
	return left != NULL && right != NULL &&
	       left->chipset_generation == right->chipset_generation &&
	       left->delivery_token == right->delivery_token &&
	       left->irq == right->irq && left->vector == right->vector &&
	       left->primary_auto_eoi == right->primary_auto_eoi &&
	       left->secondary_auto_eoi == right->secondary_auto_eoi &&
	       left->cascaded == right->cascaded &&
	       bytes_are_zero(left->reserved, ARRAY_SIZE(left->reserved)) &&
	       bytes_are_zero(right->reserved, ARRAY_SIZE(right->reserved));
}

enum x86_legacy_chipset_status x86_legacy_chipset_interrupt_prepare(
	kernel_object_handle_t context_identity,
	kernel_object_handle_t owner_identity,
	struct x86_legacy_interrupt_claim *claim)
{
	struct x86_legacy_interrupt_claim prepared;
	enum x86_legacy_chipset_status status = interrupt_owner_status(
		context_identity, owner_identity);

	if (claim == NULL)
		return X86_LEGACY_CHIPSET_INVALID_ARGUMENT;
	if (status != X86_LEGACY_CHIPSET_OK)
		return status;
	if (runtime_owner.interrupt_prepared != 0u)
		return X86_LEGACY_CHIPSET_INVALID_STATE;
	if (runtime_owner.interrupt_delivery_sequence >=
	    X86_CHIPSET_INTERRUPT_TOKEN_MAX)
		return X86_LEGACY_CHIPSET_CAPACITY_EXHAUSTED;
	if (!x86_legacy_pic_prepare_interrupt(&runtime_owner, &prepared))
		return X86_LEGACY_CHIPSET_NO_INTERRUPT;
	prepared.delivery_token = ++runtime_owner.interrupt_delivery_sequence;
	runtime_owner.prepared_interrupt = prepared;
	runtime_owner.interrupt_prepared = 1u;
	*claim = prepared;
	return X86_LEGACY_CHIPSET_OK;
}

enum x86_legacy_chipset_status x86_legacy_chipset_interrupt_commit(
	kernel_object_handle_t context_identity,
	kernel_object_handle_t owner_identity,
	const struct x86_legacy_interrupt_claim *claim)
{
	enum x86_legacy_chipset_status status = interrupt_owner_status(
		context_identity, owner_identity);

	if (claim == NULL)
		return X86_LEGACY_CHIPSET_INVALID_ARGUMENT;
	if (status != X86_LEGACY_CHIPSET_OK)
		return status;
	if (runtime_owner.interrupt_prepared == 0u)
		return X86_LEGACY_CHIPSET_INVALID_STATE;
	if (!interrupt_claim_matches(&runtime_owner.prepared_interrupt, claim))
		return X86_LEGACY_CHIPSET_STALE_INTERRUPT;
	if (!x86_legacy_pic_commit_interrupt(&runtime_owner, claim))
		return X86_LEGACY_CHIPSET_STALE_INTERRUPT;
	runtime_owner.prepared_interrupt =
		(struct x86_legacy_interrupt_claim){0};
	runtime_owner.interrupt_prepared = 0u;
	return X86_LEGACY_CHIPSET_OK;
}

enum x86_legacy_chipset_status x86_legacy_chipset_interrupt_cancel(
	kernel_object_handle_t context_identity,
	kernel_object_handle_t owner_identity,
	const struct x86_legacy_interrupt_claim *claim)
{
	enum x86_legacy_chipset_status status = interrupt_owner_status(
		context_identity, owner_identity);

	if (claim == NULL)
		return X86_LEGACY_CHIPSET_INVALID_ARGUMENT;
	if (status != X86_LEGACY_CHIPSET_OK)
		return status;
	if (runtime_owner.interrupt_prepared == 0u)
		return X86_LEGACY_CHIPSET_INVALID_STATE;
	if (!interrupt_claim_matches(&runtime_owner.prepared_interrupt, claim))
		return X86_LEGACY_CHIPSET_STALE_INTERRUPT;
	runtime_owner.prepared_interrupt =
		(struct x86_legacy_interrupt_claim){0};
	runtime_owner.interrupt_prepared = 0u;
	return X86_LEGACY_CHIPSET_OK;
}

static struct x86_legacy_pic_view pic_view(
	const struct x86_chipset_pic_state *pic)
{
	return (struct x86_legacy_pic_view){
		.vector_base = pic->vector_base,
		.interrupt_mask = pic->interrupt_mask,
		.interrupt_request = pic->interrupt_request,
		.in_service = pic->in_service,
		.initialization_step = pic->initialization_step,
		.cascade_config = pic->cascade_config,
		.priority_lowest = pic->priority_lowest,
		.read_isr = pic->read_isr,
		.mode_8086 = pic->mode_8086,
		.auto_eoi = pic->auto_eoi,
		.special_mask = pic->special_mask,
		.rotate_on_auto_eoi = pic->rotate_on_auto_eoi,
		.single = pic->single,
		.level_triggered = pic->level_triggered,
		.poll_pending = pic->poll_pending,
	};
}

static struct x86_legacy_pit_view pit_view(
	const struct x86_chipset_pit_state *pit,
	bool interrupt_delivery_active)
{
	return (struct x86_legacy_pit_view){
		.reload = pit->reload,
		.current = pit->current,
		.latched_count = pit->latched_count,
		.pending_low_byte = pit->pending_low_byte,
		.access = pit->access,
		.mode = pit->mode,
		.bcd = pit->bcd,
		.null_count = pit->null_count,
		.count_latched = pit->count_latched,
		.status_latched = pit->status_latched,
		.output = pit->output,
		.write_phase = pit->write_phase,
		.read_phase = pit->read_phase,
		.interrupt_delivery_active =
			(uint8_t)interrupt_delivery_active,
		.reserved = {0u},
	};
}

static struct x86_legacy_rtc_view rtc_view(
	const struct x86_chipset_rtc_state *rtc, bool time_source_active)
{
	return (struct x86_legacy_rtc_view){
		.year = rtc->year,
		.second = rtc->second,
		.minute = rtc->minute,
		.hour = rtc->hour,
		.weekday = rtc->weekday,
		.day = rtc->day,
		.month = rtc->month,
		.selected_register = rtc->selected_register,
		.nmi_disabled = rtc->nmi_disabled,
		.status_a = rtc->status_a,
		.status_b = rtc->status_b,
		.status_c = rtc->status_c,
		.status_d = rtc->status_d,
		.time_source_active = (uint8_t)time_source_active,
	};
}

enum x86_legacy_chipset_status x86_legacy_chipset_snapshot(
	kernel_object_handle_t context_identity,
	struct x86_legacy_chipset_snapshot *snapshot)
{
	struct x86_legacy_chipset_snapshot prepared;
	size_t index;

	if (!identity_is_valid(context_identity) || snapshot == NULL)
		return X86_LEGACY_CHIPSET_INVALID_ARGUMENT;
	if (runtime_owner.context_identity != context_identity)
		return X86_LEGACY_CHIPSET_IDENTITY_MISMATCH;
	if (runtime_owner.phase == X86_CHIPSET_POISONED)
		return X86_LEGACY_CHIPSET_POISONED;
	if (runtime_owner.phase != X86_CHIPSET_ACTIVE)
		return X86_LEGACY_CHIPSET_INVALID_STATE;
	prepared = (struct x86_legacy_chipset_snapshot){
		.generation = runtime_owner.generation,
		.context_identity = runtime_owner.context_identity,
		.owner_identity = runtime_owner.owner_identity,
		.interrupt_source_identity =
			runtime_owner.interrupt_source_identity,
		.interrupt_source_generation =
			runtime_owner.interrupt_source_generation,
		.pit_input_quantum = runtime_owner.pit_input_quantum,
		.pit_irq0_edges = runtime_owner.pit_irq0_edges,
		.pit_irq0_coalesced_edges =
			runtime_owner.pit_irq0_coalesced_edges,
		.rtc = rtc_view(
			&runtime_owner.rtc,
			runtime_owner.interrupt_source_phase ==
				X86_CHIPSET_SOURCE_ACTIVE &&
			(runtime_owner.interrupt_source_capabilities &
			 X86_LEGACY_CHIPSET_SOURCE_PIT_CLOCK) != 0u &&
			(runtime_owner.rtc.status_d & 0x80u) != 0u),
		.active = 1u,
		.poisoned = 0u,
		.interrupt_delivery_active = (uint8_t)(
			runtime_owner.interrupt_source_phase ==
			X86_CHIPSET_SOURCE_ACTIVE),
		.pit_time_source_active = (uint8_t)(
			runtime_owner.interrupt_source_phase ==
				X86_CHIPSET_SOURCE_ACTIVE &&
			(runtime_owner.interrupt_source_capabilities &
			 X86_LEGACY_CHIPSET_SOURCE_PIT_CLOCK) != 0u),
		.pit_rate_calibrated = runtime_owner.pit_rate_calibrated,
		.reserved = {0u},
	};
	for (index = 0u; index < ARRAY_SIZE(prepared.pic); ++index)
		prepared.pic[index] = pic_view(&runtime_owner.pic[index]);
	for (index = 0u; index < ARRAY_SIZE(prepared.pit); ++index) {
		prepared.pit[index] = pit_view(
			&runtime_owner.pit[index],
			index == 0u &&
				runtime_owner.interrupt_source_phase ==
					X86_CHIPSET_SOURCE_ACTIVE &&
				(runtime_owner.interrupt_source_capabilities &
				 X86_LEGACY_CHIPSET_SOURCE_PIT_CLOCK) != 0u);
	}
	for (index = 0u; index < ARRAY_SIZE(prepared.dma); ++index)
		x86_legacy_dma_snapshot_state(&runtime_owner.dma[index],
					      &prepared.dma[index]);
	*snapshot = prepared;
	return X86_LEGACY_CHIPSET_OK;
}

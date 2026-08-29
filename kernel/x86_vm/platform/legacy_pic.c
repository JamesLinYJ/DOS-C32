// SPDX-License-Identifier: GPL-2.0-only
/* Controller-owned native 8259 acknowledgement, spurious detection and EOI. */
#include "x86_legacy_pic.h"

#if defined(DOSC32_HOST_TEST)
uint8_t x86_legacy_pic_test_inb(uint16_t port);
void x86_legacy_pic_test_outb(uint16_t port, uint8_t value);
void x86_legacy_pic_test_io_wait(void);

static uint8_t native_inb(uint16_t port)
{
	return x86_legacy_pic_test_inb(port);
}

static void native_outb(uint16_t port, uint8_t value)
{
	x86_legacy_pic_test_outb(port, value);
}

static void native_io_wait(void)
{
	x86_legacy_pic_test_io_wait();
}
#else
#include "io.h"

static uint8_t native_inb(uint16_t port)
{
	return inb(port);
}

static void native_outb(uint16_t port, uint8_t value)
{
	outb(port, value);
}

static void native_io_wait(void)
{
	io_wait();
}
#endif

#define PIC_MASTER_COMMAND 0x20u
#define PIC_MASTER_DATA 0x21u
#define PIC_SLAVE_COMMAND 0xa0u
#define PIC_SLAVE_DATA 0xa1u
#define PIC_INITIALIZE 0x11u
#define PIC_8086_MODE 0x01u
#define PIC_MASTER_HAS_SLAVE_IRQ2 0x04u
#define PIC_SLAVE_IDENTITY_IRQ2 0x02u
#define PIC_END_OF_INTERRUPT 0x20u
#define PIC_READ_ISR 0x0bu
#define PIC_ALL_MASKED 0xffu
#define PIC_CASCADE_IRQ 2u

#define PIT_CHANNEL_ZERO 0x40u
#define PIT_CONTROL 0x43u
#define PIT_CHANNEL_ZERO_SQUARE_WAVE 0x36u

#define LEGACY_PIC_GENERATION_MAX ((uint64_t)-2 >> 8u)
#define LEGACY_PIC_COOKIE_IRQ_MASK 0xffu

struct x86_legacy_pic_owner {
	struct x86_legacy_pic_config config;
	uint64_t generation;
	uint8_t phase;
	uint8_t reserved[7];
} __aligned(8);

static struct x86_legacy_pic_owner owner;

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

static bool config_is_valid(const struct x86_legacy_pic_config *config)
{
	const uint16_t slave_mask = 0xff00u;
	const uint16_t cascade_mask = (uint16_t)(1u << PIC_CASCADE_IRQ);

	if (config == NULL ||
	    !identity_is_valid(config->controller_identity) ||
	    !identity_is_valid(config->dispatch_identity) ||
	    config->controller_identity == config->dispatch_identity ||
	    config->pit_input_quantum == 0u || config->pit_reload == 0u ||
	    config->vector_base < 0x20u || config->vector_base > 0xf0u ||
	    config->pit_rate_calibrated > 1u || config->present != 1u ||
	    config->presence_evidence == X86_LEGACY_PIC_EVIDENCE_NONE ||
	    config->presence_evidence >
		    X86_LEGACY_PIC_EVIDENCE_FIRMWARE_REPORTED ||
	    config->present_irq_mask == 0u ||
	    ((config->present_irq_mask & slave_mask) != 0u &&
	     (config->present_irq_mask & cascade_mask) == 0u) ||
	    (config->enabled_irq_mask &
	     (uint16_t)~config->present_irq_mask) != 0u ||
	    !bytes_are_zero(config->reserved, ARRAY_SIZE(config->reserved)))
		return false;
	return true;
}

static uint32_t irq_count(uint16_t mask)
{
	uint32_t count = 0u;

	while (mask != 0u) {
		count += mask & 1u;
		mask >>= 1u;
	}
	return count;
}

static void mask_all(void)
{
	native_outb(PIC_SLAVE_DATA, PIC_ALL_MASKED);
	native_outb(PIC_MASTER_DATA, PIC_ALL_MASKED);
}

static void program_masks(uint16_t enabled)
{
	uint8_t master_enabled = (uint8_t)enabled;
	uint8_t slave_enabled = (uint8_t)(enabled >> 8u);

	if (slave_enabled != 0u)
		master_enabled |= (uint8_t)(1u << PIC_CASCADE_IRQ);
	native_outb(PIC_SLAVE_DATA, (uint8_t)~slave_enabled);
	native_outb(PIC_MASTER_DATA, (uint8_t)~master_enabled);
}

static void initialize_controller(const struct x86_legacy_pic_config *config)
{
	native_outb(PIC_MASTER_COMMAND, PIC_INITIALIZE);
	native_io_wait();
	native_outb(PIC_SLAVE_COMMAND, PIC_INITIALIZE);
	native_io_wait();
	native_outb(PIC_MASTER_DATA, (uint8_t)config->vector_base);
	native_io_wait();
	native_outb(PIC_SLAVE_DATA, (uint8_t)(config->vector_base + 8u));
	native_io_wait();
	native_outb(PIC_MASTER_DATA, PIC_MASTER_HAS_SLAVE_IRQ2);
	native_io_wait();
	native_outb(PIC_SLAVE_DATA, PIC_SLAVE_IDENTITY_IRQ2);
	native_io_wait();
	native_outb(PIC_MASTER_DATA, PIC_8086_MODE);
	native_io_wait();
	native_outb(PIC_SLAVE_DATA, PIC_8086_MODE);
	native_io_wait();
	mask_all();
	native_outb(PIT_CONTROL, PIT_CHANNEL_ZERO_SQUARE_WAVE);
	native_outb(PIT_CHANNEL_ZERO, (uint8_t)config->pit_reload);
	native_outb(PIT_CHANNEL_ZERO, (uint8_t)(config->pit_reload >> 8u));
}

enum x86_legacy_pic_status x86_legacy_pic_prepare(
	const struct x86_legacy_pic_config *config,
	struct x86_native_irq_line_config *lines, uint32_t line_capacity,
	uint32_t *line_count)
{
	struct x86_legacy_pic_owner prepared;
	struct x86_native_irq_line_config discovered[X86_LEGACY_PIC_IRQ_COUNT];
	uint32_t discovered_count;
	uint32_t irq;

	if (!config_is_valid(config) || lines == NULL || line_count == NULL)
		return X86_LEGACY_PIC_INVALID_ARGUMENT;
	discovered_count = irq_count(config->present_irq_mask);
	if (line_capacity < discovered_count)
		return X86_LEGACY_PIC_CAPACITY_EXHAUSTED;
	if (owner.phase == X86_LEGACY_PIC_POISONED_PHASE)
		return X86_LEGACY_PIC_POISONED;
	if (owner.phase != X86_LEGACY_PIC_EMPTY)
		return X86_LEGACY_PIC_INVALID_STATE;
	if (owner.generation >= LEGACY_PIC_GENERATION_MAX)
		return X86_LEGACY_PIC_CAPACITY_EXHAUSTED;
	prepared = (struct x86_legacy_pic_owner){
		.config = *config,
		.generation = owner.generation + 1u,
		.phase = X86_LEGACY_PIC_PREPARED,
		.reserved = {0u},
	};
	discovered_count = 0u;
	for (irq = 0u; irq < X86_LEGACY_PIC_IRQ_COUNT; ++irq) {
		if ((config->present_irq_mask & (uint16_t)(1u << irq)) == 0u)
			continue;
		discovered[discovered_count++] =
			(struct x86_native_irq_line_config){
			.vector = config->vector_base + irq,
			.hardware_irq = irq,
			.flags = 0u,
			.reserved = {0u},
		};
	}
	initialize_controller(config);
	owner = prepared;
	for (irq = 0u; irq < discovered_count; ++irq)
		lines[irq] = discovered[irq];
	*line_count = discovered_count;
	return X86_LEGACY_PIC_OK;
}

enum x86_legacy_pic_status x86_legacy_pic_abort(
	kernel_object_handle_t controller_identity)
{
	uint64_t generation;

	if (!identity_is_valid(controller_identity))
		return X86_LEGACY_PIC_INVALID_ARGUMENT;
	if (owner.config.controller_identity != controller_identity)
		return X86_LEGACY_PIC_IDENTITY_MISMATCH;
	if (owner.phase != X86_LEGACY_PIC_PREPARED)
		return owner.phase == X86_LEGACY_PIC_POISONED_PHASE
			       ? X86_LEGACY_PIC_POISONED
			       : X86_LEGACY_PIC_INVALID_STATE;
	mask_all();
	generation = owner.generation;
	owner = (struct x86_legacy_pic_owner){
		.generation = generation,
	};
	return X86_LEGACY_PIC_OK;
}

enum x86_legacy_pic_status x86_legacy_pic_retire(
	kernel_object_handle_t controller_identity,
	kernel_object_handle_t dispatch_identity)
{
	uint64_t generation;

	if (!identity_is_valid(controller_identity) ||
	    !identity_is_valid(dispatch_identity))
		return X86_LEGACY_PIC_INVALID_ARGUMENT;
	if (owner.config.controller_identity != controller_identity ||
	    owner.config.dispatch_identity != dispatch_identity)
		return X86_LEGACY_PIC_IDENTITY_MISMATCH;
	if (owner.phase != X86_LEGACY_PIC_QUIESCED)
		return owner.phase == X86_LEGACY_PIC_POISONED_PHASE
			       ? X86_LEGACY_PIC_POISONED
			       : X86_LEGACY_PIC_INVALID_STATE;
	mask_all();
	generation = owner.generation;
	owner = (struct x86_legacy_pic_owner){
		.generation = generation,
	};
	return X86_LEGACY_PIC_OK;
}

enum x86_legacy_pic_status x86_legacy_pic_set_enabled_irqs(
	kernel_object_handle_t controller_identity,
	kernel_object_handle_t dispatch_identity, uint16_t enabled_irq_mask)
{
	if (!identity_is_valid(controller_identity) ||
	    !identity_is_valid(dispatch_identity))
		return X86_LEGACY_PIC_INVALID_ARGUMENT;
	if (owner.config.controller_identity != controller_identity ||
	    owner.config.dispatch_identity != dispatch_identity)
		return X86_LEGACY_PIC_IDENTITY_MISMATCH;
	if (owner.phase != X86_LEGACY_PIC_PREPARED &&
	    owner.phase != X86_LEGACY_PIC_QUIESCED)
		return owner.phase == X86_LEGACY_PIC_POISONED_PHASE
			       ? X86_LEGACY_PIC_POISONED
			       : X86_LEGACY_PIC_INVALID_STATE;
	if ((enabled_irq_mask & (uint16_t)~owner.config.present_irq_mask) != 0u)
		return X86_LEGACY_PIC_UNAVAILABLE;
	owner.config.enabled_irq_mask = enabled_irq_mask;
	return X86_LEGACY_PIC_OK;
}

static enum x86_native_irq_controller_result controller_quiesce(
	kernel_object_handle_t context, kernel_object_handle_t dispatch_identity)
{
	if (context != owner.config.controller_identity ||
	    dispatch_identity != owner.config.dispatch_identity ||
	    owner.phase != X86_LEGACY_PIC_ACTIVE)
		return X86_NATIVE_IRQ_CONTROLLER_RESULT_POISONED;
	mask_all();
	owner.phase = X86_LEGACY_PIC_QUIESCED;
	return X86_NATIVE_IRQ_CONTROLLER_RESULT_OK;
}

static enum x86_native_irq_controller_result controller_resume(
	kernel_object_handle_t context, kernel_object_handle_t dispatch_identity)
{
	if (context != owner.config.controller_identity ||
	    dispatch_identity != owner.config.dispatch_identity)
		return X86_NATIVE_IRQ_CONTROLLER_RESULT_POISONED;
	if (owner.phase != X86_LEGACY_PIC_PREPARED &&
	    owner.phase != X86_LEGACY_PIC_QUIESCED)
		return X86_NATIVE_IRQ_CONTROLLER_RESULT_REJECTED;
	program_masks(owner.config.enabled_irq_mask);
	owner.phase = X86_LEGACY_PIC_ACTIVE;
	return X86_NATIVE_IRQ_CONTROLLER_RESULT_OK;
}

static uint64_t observation_cookie(uint32_t irq)
{
	return (owner.generation << 8u) | (uint64_t)(irq + 1u);
}

static bool event_matches_owner(const struct x86_native_irq_event *event)
{
	return event != NULL &&
	       event->controller_identity == owner.config.controller_identity &&
	       event->hardware_irq < X86_LEGACY_PIC_IRQ_COUNT &&
	       event->vector == owner.config.vector_base + event->hardware_irq &&
	       (owner.config.present_irq_mask &
		(uint16_t)(1u << event->hardware_irq)) != 0u;
}

static enum x86_native_irq_controller_result controller_begin(
	kernel_object_handle_t context, const struct x86_native_irq_event *event,
	struct x86_native_irq_observation *observation)
{
	uint8_t isr;
	uint16_t command_port;

	if (context != owner.config.controller_identity || observation == NULL ||
	    owner.phase != X86_LEGACY_PIC_ACTIVE || !event_matches_owner(event))
		return X86_NATIVE_IRQ_CONTROLLER_RESULT_POISONED;
	*observation = (struct x86_native_irq_observation){
		.controller_cookie = observation_cookie(event->hardware_irq),
		.kind = X86_NATIVE_IRQ_OBSERVATION_DELIVER,
		.reserved = {0u},
	};
	if (event->hardware_irq != 7u && event->hardware_irq != 15u)
		return X86_NATIVE_IRQ_CONTROLLER_RESULT_OK;
	command_port = event->hardware_irq == 7u ? PIC_MASTER_COMMAND
						 : PIC_SLAVE_COMMAND;
	native_outb(command_port, PIC_READ_ISR);
	isr = native_inb(command_port);
	if ((isr & 0x80u) == 0u)
		observation->kind = X86_NATIVE_IRQ_OBSERVATION_SPURIOUS;
	return X86_NATIVE_IRQ_CONTROLLER_RESULT_OK;
}

static bool observation_matches_event(
	const struct x86_native_irq_event *event,
	const struct x86_native_irq_observation *observation)
{
	return observation != NULL &&
	       observation->controller_cookie ==
		       observation_cookie(event->hardware_irq) &&
	       (observation->kind == X86_NATIVE_IRQ_OBSERVATION_DELIVER ||
		observation->kind == X86_NATIVE_IRQ_OBSERVATION_SPURIOUS) &&
	       bytes_are_zero(observation->reserved,
			      ARRAY_SIZE(observation->reserved));
}

static enum x86_native_irq_controller_result controller_end(
	kernel_object_handle_t context, const struct x86_native_irq_event *event,
	const struct x86_native_irq_observation *observation,
	enum x86_native_irq_completion completion)
{
	if (context != owner.config.controller_identity ||
	    owner.phase != X86_LEGACY_PIC_ACTIVE || !event_matches_owner(event) ||
	    !observation_matches_event(event, observation))
		return X86_NATIVE_IRQ_CONTROLLER_RESULT_POISONED;
	if ((observation->kind == X86_NATIVE_IRQ_OBSERVATION_SPURIOUS) !=
	    (completion == X86_NATIVE_IRQ_COMPLETE_SPURIOUS))
		return X86_NATIVE_IRQ_CONTROLLER_RESULT_POISONED;
	if (observation->kind == X86_NATIVE_IRQ_OBSERVATION_DELIVER &&
	    completion != X86_NATIVE_IRQ_COMPLETE_HANDLED &&
	    completion != X86_NATIVE_IRQ_COMPLETE_UNHANDLED &&
	    completion != X86_NATIVE_IRQ_COMPLETE_HANDLER_FAULT)
		return X86_NATIVE_IRQ_CONTROLLER_RESULT_POISONED;
	if (observation->kind == X86_NATIVE_IRQ_OBSERVATION_SPURIOUS) {
		if (event->hardware_irq == 15u)
			native_outb(PIC_MASTER_COMMAND, PIC_END_OF_INTERRUPT);
		else if (event->hardware_irq != 7u)
			return X86_NATIVE_IRQ_CONTROLLER_RESULT_POISONED;
		return X86_NATIVE_IRQ_CONTROLLER_RESULT_OK;
	}
	if (event->hardware_irq >= 8u)
		native_outb(PIC_SLAVE_COMMAND, PIC_END_OF_INTERRUPT);
	native_outb(PIC_MASTER_COMMAND, PIC_END_OF_INTERRUPT);
	return X86_NATIVE_IRQ_CONTROLLER_RESULT_OK;
}

struct x86_native_irq_controller_ops x86_legacy_pic_controller_ops(void)
{
	return (struct x86_native_irq_controller_ops){
		.begin = controller_begin,
		.end = controller_end,
		.quiesce = controller_quiesce,
		.resume = controller_resume,
	};
}

enum x86_legacy_pic_status x86_legacy_pic_poison(
	kernel_object_handle_t controller_identity)
{
	if (!identity_is_valid(controller_identity))
		return X86_LEGACY_PIC_INVALID_ARGUMENT;
	if (owner.config.controller_identity != controller_identity)
		return X86_LEGACY_PIC_IDENTITY_MISMATCH;
	if (owner.phase == X86_LEGACY_PIC_EMPTY)
		return X86_LEGACY_PIC_INVALID_STATE;
	mask_all();
	owner.phase = X86_LEGACY_PIC_POISONED_PHASE;
	return X86_LEGACY_PIC_OK;
}

enum x86_legacy_pic_status x86_legacy_pic_snapshot(
	kernel_object_handle_t controller_identity,
	struct x86_legacy_pic_snapshot *snapshot)
{
	if (!identity_is_valid(controller_identity) || snapshot == NULL)
		return X86_LEGACY_PIC_INVALID_ARGUMENT;
	if (owner.config.controller_identity != controller_identity)
		return X86_LEGACY_PIC_IDENTITY_MISMATCH;
	if (owner.phase == X86_LEGACY_PIC_EMPTY)
		return X86_LEGACY_PIC_INVALID_STATE;
	if (owner.phase == X86_LEGACY_PIC_ACTIVE)
		return X86_LEGACY_PIC_BUSY;
	*snapshot = (struct x86_legacy_pic_snapshot){
		.controller_identity = owner.config.controller_identity,
		.dispatch_identity = owner.config.dispatch_identity,
		.generation = owner.generation,
		.pit_input_quantum = owner.config.pit_input_quantum,
		.vector_base = owner.config.vector_base,
		.present_irq_mask = owner.config.present_irq_mask,
		.enabled_irq_mask = owner.config.enabled_irq_mask,
		.pit_reload = owner.config.pit_reload,
		.pit_rate_calibrated = owner.config.pit_rate_calibrated,
		.presence_evidence = owner.config.presence_evidence,
		.phase = owner.phase,
		.reserved = {0u},
	};
	return owner.phase == X86_LEGACY_PIC_POISONED_PHASE
		       ? X86_LEGACY_PIC_POISONED
		       : X86_LEGACY_PIC_OK;
}

/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DOSC32_X86_LEGACY_CHIPSET_PRIVATE_H
#define DOSC32_X86_LEGACY_CHIPSET_PRIVATE_H

#include "x86_legacy_chipset.h"

#define X86_CHIPSET_CMOS_REGISTER_COUNT 128u

enum x86_chipset_phase {
	X86_CHIPSET_EMPTY = 0,
	X86_CHIPSET_PREPARED,
	X86_CHIPSET_ACTIVE,
	X86_CHIPSET_POISONED
};

enum x86_chipset_source_phase {
	X86_CHIPSET_SOURCE_EMPTY = 0,
	X86_CHIPSET_SOURCE_ACTIVE,
	X86_CHIPSET_SOURCE_QUIESCED
};

struct x86_chipset_pic_state {
	uint8_t vector_base;
	uint8_t interrupt_mask;
	uint8_t interrupt_request;
	uint8_t in_service;
	uint8_t initialization_step;
	uint8_t cascade_config;
	uint8_t priority_lowest;
	uint8_t read_isr;
	uint8_t mode_8086;
	uint8_t auto_eoi;
	uint8_t special_mask;
	uint8_t rotate_on_auto_eoi;
	uint8_t single;
	uint8_t level_triggered;
	uint8_t poll_pending;
	uint8_t needs_icw4;
};

struct x86_chipset_pit_state {
	uint64_t input_ticks;
	uint32_t current_count;
	uint16_t reload;
	uint16_t current;
	uint16_t latched_count;
	uint16_t pending_low_byte;
	uint8_t access;
	uint8_t mode;
	uint8_t bcd;
	uint8_t null_count;
	uint8_t count_latched;
	uint8_t status_latched;
	uint8_t latched_status;
	uint8_t output;
	uint8_t write_phase;
	uint8_t read_phase;
	uint8_t latch_read_phase;
	uint8_t latch_access;
	uint8_t running;
	uint8_t strobe_pending;
};

struct x86_chipset_rtc_state {
	uint8_t cmos[X86_CHIPSET_CMOS_REGISTER_COUNT];
	uint32_t subsecond_ticks;
	uint16_t year;
	uint8_t second;
	uint8_t minute;
	uint8_t hour;
	uint8_t weekday;
	uint8_t day;
	uint8_t month;
	uint8_t selected_register;
	uint8_t nmi_disabled;
	uint8_t status_a;
	uint8_t status_b;
	uint8_t status_c;
	uint8_t status_d;
};

struct x86_chipset_dma_channel_state {
	uint16_t base_address;
	uint16_t current_address;
	uint16_t base_count;
	uint16_t current_count;
	uint8_t page;
	uint8_t mode;
	uint8_t reserved[6];
};

struct x86_chipset_dma_controller_state {
	struct x86_chipset_dma_channel_state
		channel[X86_LEGACY_CHIPSET_DMA_CHANNELS_PER_CONTROLLER];
	uint8_t command;
	uint8_t status;
	uint8_t mask;
	uint8_t flip_flop;
	uint8_t temporary;
	uint8_t word_addressed;
	uint8_t reserved[2];
};

struct x86_legacy_chipset_owner {
	kernel_object_handle_t context_identity;
	kernel_object_handle_t owner_identity;
	kernel_object_handle_t interrupt_source_identity;
	uint64_t generation;
	uint64_t interrupt_source_generation;
	uint64_t pit_input_quantum;
	uint64_t pit_irq0_edges;
	uint64_t pit_irq0_coalesced_edges;
	uint64_t interrupt_delivery_sequence;
	struct x86_legacy_interrupt_claim prepared_interrupt;
	uint32_t interrupt_source_capabilities;
	struct x86_chipset_pic_state pic[X86_LEGACY_CHIPSET_PIC_COUNT];
	struct x86_chipset_pit_state pit[X86_LEGACY_CHIPSET_PIT_CHANNEL_COUNT];
	struct x86_chipset_rtc_state rtc;
	struct x86_chipset_dma_controller_state
		dma[X86_LEGACY_CHIPSET_DMA_CONTROLLER_COUNT];
	uint8_t phase;
	uint8_t interrupt_source_phase;
	uint8_t pit_rate_calibrated;
	uint8_t interrupt_prepared;
	uint8_t reserved[4];
} __aligned(8);

struct x86_legacy_chipset_owner *x86_legacy_chipset_active_owner(
	kernel_object_handle_t context_identity);
void x86_legacy_chipset_poison_internal(
	kernel_object_handle_t context_identity);

bool x86_legacy_pic_request_irq(
	struct x86_legacy_chipset_owner *chipset, uint8_t irq);
bool x86_legacy_pic_prepare_interrupt(
	const struct x86_legacy_chipset_owner *chipset,
	struct x86_legacy_interrupt_claim *claim);
bool x86_legacy_pic_commit_interrupt(
	struct x86_legacy_chipset_owner *chipset,
	const struct x86_legacy_interrupt_claim *claim);
uint64_t x86_legacy_pit_advance(
	struct x86_legacy_chipset_owner *chipset, uint64_t input_ticks);
void x86_legacy_pit_initialize_state(
	struct x86_chipset_pit_state *pit, uint16_t reload, uint8_t access,
	uint8_t mode, uint8_t bcd);
void x86_legacy_rtc_advance(struct x86_chipset_rtc_state *rtc,
	uint64_t input_ticks);
void x86_legacy_dma_initialize_state(
	struct x86_chipset_dma_controller_state
		dma[X86_LEGACY_CHIPSET_DMA_CONTROLLER_COUNT]);
void x86_legacy_dma_snapshot_state(
	const struct x86_chipset_dma_controller_state *dma,
	struct x86_legacy_dma_controller_view *view);

enum x86_io_callback_status x86_legacy_pic_read(
	kernel_object_handle_t context, uint16_t port,
	enum dos_io_width width, uint32_t *value);
enum x86_io_callback_status x86_legacy_pic_write(
	kernel_object_handle_t context, uint16_t port,
	enum dos_io_width width, uint32_t value);
enum x86_io_callback_status x86_legacy_pit_read(
	kernel_object_handle_t context, uint16_t port,
	enum dos_io_width width, uint32_t *value);
enum x86_io_callback_status x86_legacy_pit_write(
	kernel_object_handle_t context, uint16_t port,
	enum dos_io_width width, uint32_t value);
enum x86_io_callback_status x86_legacy_rtc_read(
	kernel_object_handle_t context, uint16_t port,
	enum dos_io_width width, uint32_t *value);
enum x86_io_callback_status x86_legacy_rtc_write(
	kernel_object_handle_t context, uint16_t port,
	enum dos_io_width width, uint32_t value);
enum x86_io_callback_status x86_legacy_dma_read(
	kernel_object_handle_t context, uint16_t port,
	enum dos_io_width width, uint32_t *value);
enum x86_io_callback_status x86_legacy_dma_write(
	kernel_object_handle_t context, uint16_t port,
	enum dos_io_width width, uint32_t value);

#endif

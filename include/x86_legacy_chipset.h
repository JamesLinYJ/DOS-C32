/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Generation-bound software model of the guest-visible 8237A, 8259A, 8254
 * and RTC.
 *
 * The model consumes legacy I/O itself.  It never grants access to the native
 * DMA controllers, interrupt controllers, timer, NMI gate or CMOS.  The DMA
 * register file is isolated guest state; actual device transfers require a
 * separately validated endpoint and are never inferred from register writes.
 * Interrupt and PIT-clock inputs cross one generation-bound source capability;
 * delivery claims are resolved entirely from the modeled PIC state.
 */
#ifndef DOSC32_X86_LEGACY_CHIPSET_H
#define DOSC32_X86_LEGACY_CHIPSET_H

#include "x86_io_resource.h"

struct x86_legacy_bios_snapshot;

#define X86_LEGACY_CHIPSET_RESOURCE_COUNT 10u
#define X86_LEGACY_CHIPSET_DMA_CONTROLLER_COUNT 2u
#define X86_LEGACY_CHIPSET_DMA_CHANNELS_PER_CONTROLLER 4u
#define X86_LEGACY_CHIPSET_PIC_COUNT 2u
#define X86_LEGACY_CHIPSET_PIT_CHANNEL_COUNT 3u
#define X86_LEGACY_CHIPSET_IRQ_COUNT 16u
#define X86_LEGACY_PIT_INPUT_HZ 1193182u

#define X86_LEGACY_CHIPSET_SOURCE_PIT_CLOCK (1u << 0)
#define X86_LEGACY_CHIPSET_SOURCE_IRQ_EDGE (1u << 1)
#define X86_LEGACY_CHIPSET_SOURCE_CAPABILITIES                            \
	(X86_LEGACY_CHIPSET_SOURCE_PIT_CLOCK |                              \
	 X86_LEGACY_CHIPSET_SOURCE_IRQ_EDGE)

#define X86_PIC_PRIMARY_COMMAND_PORT 0x0020u
#define X86_PIC_PRIMARY_DATA_PORT 0x0021u
#define X86_DMA_PRIMARY_FIRST_PORT 0x0000u
#define X86_DMA_PRIMARY_LAST_PORT 0x000fu
#define X86_DMA_PRIMARY_STATUS_COMMAND_PORT 0x0008u
#define X86_DMA_PRIMARY_REQUEST_PORT 0x0009u
#define X86_DMA_PRIMARY_SINGLE_MASK_PORT 0x000au
#define X86_DMA_PRIMARY_MODE_PORT 0x000bu
#define X86_DMA_PRIMARY_CLEAR_FLIP_FLOP_PORT 0x000cu
#define X86_DMA_PRIMARY_TEMPORARY_MASTER_CLEAR_PORT 0x000du
#define X86_DMA_PRIMARY_CLEAR_MASK_PORT 0x000eu
#define X86_DMA_PRIMARY_ALL_MASK_PORT 0x000fu
#define X86_PIT_CHANNEL0_PORT 0x0040u
#define X86_PIT_CHANNEL1_PORT 0x0041u
#define X86_PIT_CHANNEL2_PORT 0x0042u
#define X86_PIT_CONTROL_PORT 0x0043u
#define X86_RTC_INDEX_PORT 0x0070u
#define X86_RTC_DATA_PORT 0x0071u
#define X86_DMA_PAGE_FIRST_PORT 0x0080u
#define X86_DMA_PAGE_LAST_PORT 0x008fu
#define X86_DMA_PAGE_PRIMARY_CHANNELS123_FIRST_PORT 0x0081u
#define X86_DMA_PAGE_PRIMARY_CHANNELS123_LAST_PORT 0x0083u
#define X86_DMA_PAGE_PRIMARY_CHANNEL0_PORT 0x0087u
#define X86_DMA_PAGE_SECONDARY_CHANNELS567_FIRST_PORT 0x0089u
#define X86_DMA_PAGE_SECONDARY_CHANNELS567_LAST_PORT 0x008bu
#define X86_DMA_PAGE_SECONDARY_CHANNEL4_PORT 0x008fu
#define X86_PIC_SECONDARY_COMMAND_PORT 0x00a0u
#define X86_PIC_SECONDARY_DATA_PORT 0x00a1u
#define X86_DMA_SECONDARY_FIRST_PORT 0x00c0u
#define X86_DMA_SECONDARY_LAST_PORT 0x00dfu
#define X86_DMA_SECONDARY_STATUS_COMMAND_PORT 0x00d0u
#define X86_DMA_SECONDARY_REQUEST_PORT 0x00d2u
#define X86_DMA_SECONDARY_SINGLE_MASK_PORT 0x00d4u
#define X86_DMA_SECONDARY_MODE_PORT 0x00d6u
#define X86_DMA_SECONDARY_CLEAR_FLIP_FLOP_PORT 0x00d8u
#define X86_DMA_SECONDARY_TEMPORARY_MASTER_CLEAR_PORT 0x00dau
#define X86_DMA_SECONDARY_CLEAR_MASK_PORT 0x00dcu
#define X86_DMA_SECONDARY_ALL_MASK_PORT 0x00deu

#define X86_DMA_MODE_CASCADE 0xc0u

struct x86_legacy_chipset_config {
	uint16_t rtc_year;
	uint16_t pit_reload[X86_LEGACY_CHIPSET_PIT_CHANNEL_COUNT];
	uint8_t pic_vector_base[X86_LEGACY_CHIPSET_PIC_COUNT];
	uint8_t pic_mask[X86_LEGACY_CHIPSET_PIC_COUNT];
	uint8_t pit_access[X86_LEGACY_CHIPSET_PIT_CHANNEL_COUNT];
	uint8_t pit_mode[X86_LEGACY_CHIPSET_PIT_CHANNEL_COUNT];
	uint8_t pit_bcd[X86_LEGACY_CHIPSET_PIT_CHANNEL_COUNT];
	uint8_t rtc_second;
	uint8_t rtc_minute;
	uint8_t rtc_hour;
	uint8_t rtc_weekday;
	uint8_t rtc_day;
	uint8_t rtc_month;
	uint8_t rtc_status_a;
	uint8_t rtc_status_b;
	uint8_t rtc_valid;
	uint8_t pic_cascade_config[X86_LEGACY_CHIPSET_PIC_COUNT];
} __aligned(8);

struct x86_legacy_chipset_source_config {
	uint64_t pit_input_quantum;
	uint32_t capabilities;
	uint8_t pit_rate_calibrated;
	uint8_t reserved[3];
} __aligned(8);

enum x86_legacy_chipset_source_event_kind {
	X86_LEGACY_CHIPSET_EVENT_PIT_CLOCK = 1,
	X86_LEGACY_CHIPSET_EVENT_IRQ_EDGE
};

struct x86_legacy_chipset_source_event {
	uint64_t pit_input_ticks;
	uint8_t kind;
	uint8_t irq;
	uint8_t reserved[6];
} __aligned(8);

struct x86_legacy_interrupt_claim {
	uint64_t chipset_generation;
	uint64_t delivery_token;
	uint8_t irq;
	uint8_t vector;
	uint8_t primary_auto_eoi;
	uint8_t secondary_auto_eoi;
	uint8_t cascaded;
	uint8_t reserved[3];
} __aligned(8);

struct x86_legacy_pic_view {
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
	uint8_t reserved;
} __aligned(8);

struct x86_legacy_pit_view {
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
	uint8_t output;
	uint8_t write_phase;
	uint8_t read_phase;
	uint8_t interrupt_delivery_active;
	uint8_t reserved[6];
} __aligned(8);

struct x86_legacy_rtc_view {
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
	uint8_t time_source_active;
	uint8_t reserved;
} __aligned(8);

struct x86_legacy_dma_channel_view {
	uint16_t base_address;
	uint16_t current_address;
	uint16_t base_count;
	uint16_t current_count;
	uint8_t page;
	uint8_t mode;
	uint8_t request_active;
	uint8_t terminal_count;
	uint8_t reserved[4];
} __aligned(8);

struct x86_legacy_dma_controller_view {
	struct x86_legacy_dma_channel_view
		channel[X86_LEGACY_CHIPSET_DMA_CHANNELS_PER_CONTROLLER];
	uint8_t command;
	uint8_t status;
	uint8_t mask;
	uint8_t flip_flop;
	uint8_t temporary;
	uint8_t word_addressed;
	uint8_t reserved[2];
} __aligned(8);

struct x86_legacy_chipset_snapshot {
	uint64_t generation;
	kernel_object_handle_t context_identity;
	kernel_object_handle_t owner_identity;
	kernel_object_handle_t interrupt_source_identity;
	uint64_t interrupt_source_generation;
	uint64_t pit_input_quantum;
	uint64_t pit_irq0_edges;
	uint64_t pit_irq0_coalesced_edges;
	struct x86_legacy_pic_view pic[X86_LEGACY_CHIPSET_PIC_COUNT];
	struct x86_legacy_pit_view pit[X86_LEGACY_CHIPSET_PIT_CHANNEL_COUNT];
	struct x86_legacy_rtc_view rtc;
	struct x86_legacy_dma_controller_view
		dma[X86_LEGACY_CHIPSET_DMA_CONTROLLER_COUNT];
	uint8_t active;
	uint8_t poisoned;
	uint8_t interrupt_delivery_active;
	uint8_t pit_time_source_active;
	uint8_t pit_rate_calibrated;
	uint8_t reserved[3];
} __aligned(8);

enum x86_legacy_chipset_status {
	X86_LEGACY_CHIPSET_OK = 0,
	X86_LEGACY_CHIPSET_INVALID_ARGUMENT,
	X86_LEGACY_CHIPSET_INVALID_STATE,
	X86_LEGACY_CHIPSET_CAPACITY_EXHAUSTED,
	X86_LEGACY_CHIPSET_IDENTITY_MISMATCH,
	X86_LEGACY_CHIPSET_POISONED,
	X86_LEGACY_CHIPSET_NO_INTERRUPT,
	X86_LEGACY_CHIPSET_STALE_INTERRUPT
};

enum x86_legacy_chipset_status x86_legacy_chipset_policy_config(
	const struct x86_legacy_bios_snapshot *platform,
	struct x86_legacy_chipset_config *config) __must_check;
enum x86_legacy_chipset_status x86_legacy_chipset_prepare(
	kernel_object_handle_t context_identity,
	kernel_object_handle_t owner_identity,
	const struct x86_legacy_chipset_config *config,
	struct x86_io_resource_descriptor *descriptors,
	size_t descriptor_capacity) __must_check;
enum x86_legacy_chipset_status x86_legacy_chipset_publish(
	kernel_object_handle_t context_identity) __must_check;
enum x86_legacy_chipset_status x86_legacy_chipset_abort(
	kernel_object_handle_t context_identity) __must_check;
enum x86_legacy_chipset_status x86_legacy_chipset_poison(
	kernel_object_handle_t context_identity) __must_check;
enum x86_legacy_chipset_status x86_legacy_chipset_source_bind(
	kernel_object_handle_t context_identity,
	kernel_object_handle_t owner_identity,
	kernel_object_handle_t source_identity,
	const struct x86_legacy_chipset_source_config *config) __must_check;
enum x86_legacy_chipset_status x86_legacy_chipset_source_quiesce(
	kernel_object_handle_t context_identity,
	kernel_object_handle_t source_identity) __must_check;
enum x86_legacy_chipset_status x86_legacy_chipset_source_resume(
	kernel_object_handle_t context_identity,
	kernel_object_handle_t source_identity) __must_check;
enum x86_legacy_chipset_status x86_legacy_chipset_source_unbind(
	kernel_object_handle_t context_identity,
	kernel_object_handle_t source_identity) __must_check;
enum x86_legacy_chipset_status x86_legacy_chipset_source_submit(
	kernel_object_handle_t context_identity,
	kernel_object_handle_t source_identity,
	const struct x86_legacy_chipset_source_event *event) __must_check;
enum x86_legacy_chipset_status x86_legacy_chipset_interrupt_claim(
	kernel_object_handle_t context_identity,
	kernel_object_handle_t owner_identity,
	struct x86_legacy_interrupt_claim *claim) __must_check;
/*
 * Prepare samples one currently eligible PIC request without changing IRR or
 * ISR.  Commit acknowledges that exact generation-bound sample atomically;
 * failed commit leaves both PIC state and the reservation unchanged so cancel
 * remains available.  Cancel drops only the reservation.  At most one
 * reservation may exist per chipset.
 */
enum x86_legacy_chipset_status x86_legacy_chipset_interrupt_prepare(
	kernel_object_handle_t context_identity,
	kernel_object_handle_t owner_identity,
	struct x86_legacy_interrupt_claim *claim) __must_check;
enum x86_legacy_chipset_status x86_legacy_chipset_interrupt_commit(
	kernel_object_handle_t context_identity,
	kernel_object_handle_t owner_identity,
	const struct x86_legacy_interrupt_claim *claim) __must_check;
enum x86_legacy_chipset_status x86_legacy_chipset_interrupt_cancel(
	kernel_object_handle_t context_identity,
	kernel_object_handle_t owner_identity,
	const struct x86_legacy_interrupt_claim *claim) __must_check;
enum x86_legacy_chipset_status x86_legacy_chipset_snapshot(
	kernel_object_handle_t context_identity,
	struct x86_legacy_chipset_snapshot *snapshot) __must_check;

static_assert_expression(sizeof(struct x86_legacy_chipset_config) == 32u,
			 "legacy-chipset config layout changed");
static_assert_expression(
	sizeof(struct x86_legacy_chipset_source_config) == 16u,
	"legacy-chipset source config layout changed");
static_assert_expression(
	sizeof(struct x86_legacy_chipset_source_event) == 16u,
	"legacy-chipset source event layout changed");
static_assert_expression(sizeof(struct x86_legacy_interrupt_claim) == 24u,
			 "legacy interrupt claim layout changed");
static_assert_expression(sizeof(struct x86_legacy_pic_view) == 16u,
			 "legacy PIC view layout changed");
static_assert_expression(sizeof(struct x86_legacy_pit_view) == 24u,
			 "legacy PIT view layout changed");
static_assert_expression(sizeof(struct x86_legacy_rtc_view) == 16u,
			 "legacy RTC view layout changed");
static_assert_expression(sizeof(struct x86_legacy_dma_channel_view) == 16u,
			 "legacy DMA channel view layout changed");
static_assert_expression(
		sizeof(struct x86_legacy_dma_controller_view) == 72u,
		"legacy DMA controller view layout changed");
static_assert_expression(sizeof(struct x86_legacy_chipset_snapshot) == 336u,
			 "legacy-chipset snapshot layout changed");

#endif

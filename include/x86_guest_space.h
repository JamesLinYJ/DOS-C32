/* SPDX-License-Identifier: GPL-2.0-only */
/* Generation-bound low-memory space shared by compatible x86 guest backends. */
#ifndef DOSC32_X86_GUEST_SPACE_H
#define DOSC32_X86_GUEST_SPACE_H

#include "dos_machine.h"
#include "x86_i8042.h"
#include "x86_io_resource.h"
#include "x86_legacy_chipset.h"
#include "x86_paging.h"

enum x86_guest_space_status {
	X86_GUEST_SPACE_OK = 0,
	X86_GUEST_SPACE_INVALID_ARGUMENT,
	X86_GUEST_SPACE_INVALID_STATE,
	X86_GUEST_SPACE_PAGING_MISMATCH,
	X86_GUEST_SPACE_MACHINE_MISMATCH,
	X86_GUEST_SPACE_IO_DENIED,
	X86_GUEST_SPACE_IO_FAULT,
	X86_GUEST_SPACE_NO_INTERRUPT,
	X86_GUEST_SPACE_INTERRUPT_SOURCE_MISMATCH,
	X86_GUEST_SPACE_INTERRUPT_FAULT,
	X86_GUEST_SPACE_STALE_BINDING,
	X86_GUEST_SPACE_CAPACITY_EXHAUSTED,
	X86_GUEST_SPACE_INPUT_MODE_CHANGED,
	X86_GUEST_SPACE_INPUT_COMMITTED_DELIVERY_PENDING,
	X86_GUEST_SPACE_DEVICE_EVENT_RETRY,
	X86_GUEST_SPACE_DEVICE_FAULT
};

struct x86_guest_space_config {
	kernel_object_handle_t address_space_identity;
	kernel_object_handle_t machine_identity;
	kernel_object_handle_t irq_router_identity;
	kernel_object_handle_t i8042_irq_producer_identity;
	struct x86_i8042_config i8042;
	uint8_t reserved[8];
} __aligned(8);

struct x86_guest_space_pit_binding {
	kernel_object_handle_t source_identity;
	uint64_t guest_space_generation;
	uint64_t router_generation;
	uint64_t producer_generation;
	uint8_t reserved[8];
} __aligned(8);

struct x86_guest_space_binding {
	kernel_object_handle_t address_space_identity;
	uint64_t address_space_generation;
	kernel_object_handle_t machine_identity;
	kernel_object_handle_t machine_context;
	struct x86_paging_binding paging;
	uint8_t a20_enabled;
	uint8_t reserved[7];
} __aligned(8);

/* Opaque process-tree lifetime proof for private firmware runtime pages. */
struct x86_guest_space_firmware_binding {
	kernel_object_handle_t address_space_identity;
	uint64_t address_space_generation;
	kernel_object_handle_t machine_identity;
	uint64_t execution_generation;
	uint64_t client_generation;
	uint32_t client_slot;
	uint8_t reserved[4];
} __aligned(8);

enum x86_guest_space_status x86_guest_space_initialize(
	const struct x86_guest_space_config *config) __must_check;
const struct dos_machine *x86_guest_space_machine(void) __must_check;
kernel_object_handle_t x86_guest_space_machine_identity(void) __must_check;
/* Sticky isolation of the exact currently borrowed guest address space. */
enum x86_guest_space_status x86_guest_space_quarantine(
	kernel_object_handle_t machine_identity,
	const struct dos_machine *machine) __must_check;
/* Input must quiesce first and be unbound before the router's final unbind. */
enum x86_guest_space_status x86_guest_space_native_pit_bind(
	kernel_object_handle_t machine_identity,
	kernel_object_handle_t source_identity,
	uint64_t pit_input_quantum, bool pit_rate_calibrated,
	struct x86_guest_space_pit_binding *binding) __must_check;
enum x86_guest_space_status x86_guest_space_native_pit_submit(
	const struct x86_guest_space_pit_binding *binding,
	uint64_t pit_input_ticks) __must_check;
enum x86_guest_space_status x86_guest_space_native_pit_quiesce(
	const struct x86_guest_space_pit_binding *binding) __must_check;
enum x86_guest_space_status x86_guest_space_native_pit_resume(
	const struct x86_guest_space_pit_binding *binding) __must_check;
enum x86_guest_space_status x86_guest_space_native_pit_unbind(
	const struct x86_guest_space_pit_binding *binding) __must_check;
enum x86_guest_space_status x86_guest_space_interrupt_claim(
	kernel_object_handle_t machine_identity,
	struct x86_legacy_interrupt_claim *claim) __must_check;
enum x86_guest_space_status x86_guest_space_interrupt_prepare(
	kernel_object_handle_t machine_identity,
	struct x86_legacy_interrupt_claim *claim) __must_check;
enum x86_guest_space_status x86_guest_space_interrupt_commit(
	kernel_object_handle_t machine_identity,
	const struct x86_legacy_interrupt_claim *claim) __must_check;
enum x86_guest_space_status x86_guest_space_interrupt_cancel(
	kernel_object_handle_t machine_identity,
	const struct x86_legacy_interrupt_claim *claim) __must_check;
enum x86_guest_space_status x86_guest_space_device_events_pump(
	kernel_object_handle_t machine_identity, size_t budget,
	size_t *processed) __must_check;

/* Upstream input resumes only while its downstream guest router is active. */
enum x86_guest_space_status x86_guest_space_i8042_input_bind(
	kernel_object_handle_t machine_identity,
	kernel_object_handle_t source_identity,
	const struct x86_i8042_input_config *config,
	struct x86_i8042_input_binding *binding) __must_check;
enum x86_guest_space_status x86_guest_space_i8042_input_quiesce(
	kernel_object_handle_t machine_identity,
	const struct x86_i8042_input_binding *binding) __must_check;
enum x86_guest_space_status x86_guest_space_i8042_input_resume(
	kernel_object_handle_t machine_identity,
	const struct x86_i8042_input_binding *binding) __must_check;
enum x86_guest_space_status x86_guest_space_i8042_input_unbind(
	kernel_object_handle_t machine_identity,
	const struct x86_i8042_input_binding *binding) __must_check;
enum x86_guest_space_status x86_guest_space_i8042_input_keyboard_mode(
	const struct x86_i8042_input_binding *binding,
	struct x86_i8042_keyboard_mode *mode) __must_check;
enum x86_guest_space_status x86_guest_space_i8042_input_inject_keyboard(
	const struct x86_i8042_input_binding *binding,
	const struct x86_i8042_keyboard_mode *mode, uint8_t value) __must_check;
enum x86_guest_space_status
x86_guest_space_i8042_input_inject_keyboard_sequence(
	const struct x86_i8042_input_binding *binding,
	const struct x86_i8042_keyboard_mode *mode, const uint8_t *values,
	size_t values_capacity, size_t count) __must_check;
/*
 * INPUT_MODE_CHANGED commits no byte and permits re-encoding against a fresh
 * mode. INPUT_COMMITTED_DELIVERY_PENDING means the full sequence is already
 * committed; the caller must not resubmit it and may only advance the central
 * device-event pump.
 */
/* Auxiliary bytes only; keyboard injection requires an exact mode epoch. */
enum x86_guest_space_status x86_guest_space_i8042_input_inject(
	const struct x86_i8042_input_binding *binding,
	enum x86_i8042_input_kind kind, uint8_t value) __must_check;
enum x86_guest_space_status x86_guest_space_i8042_input_inject_sequence(
	const struct x86_i8042_input_binding *binding,
	enum x86_i8042_input_kind kind, const uint8_t *values,
	size_t values_capacity, size_t count) __must_check;
/* BIOS data-area conventional-memory size converted to an exclusive segment. */
enum x86_guest_space_status x86_guest_space_conventional_end_segment(
	uint16_t *end_segment) __must_check;

/*
 * Only the DCC-derived native display I/O and page ranges exist, and they
 * remain closed until the active execution frontend owns this explicit lease.
 * Fault recovery may revoke both without trusting the stopped frontend; a
 * released or revoked token is permanently stale.
 */
enum x86_guest_space_status x86_guest_space_display_foreground_acquire(
	kernel_object_handle_t machine_identity,
	kernel_object_handle_t requester_identity,
	x86_io_foreground_token_t *token) __must_check;
enum x86_guest_space_status x86_guest_space_display_foreground_release(
	kernel_object_handle_t requester_identity,
	x86_io_foreground_token_t token) __must_check;
enum x86_guest_space_status x86_guest_space_display_foreground_revoke(
	kernel_object_handle_t machine_identity) __must_check;

/* Borrowed machine plus fixed identity are both required; output is atomic. */
enum x86_guest_space_status x86_guest_space_pin(
	kernel_object_handle_t machine_identity,
	const struct dos_machine *machine,
	struct x86_guest_space_binding *binding) __must_check;
bool x86_guest_space_binding_is_active(
	const struct x86_guest_space_binding *binding,
	kernel_object_handle_t machine_identity,
	const struct dos_machine *machine) __must_check;

/*
 * VM frontends share one firmware shadow epoch across a parent/child process
 * tree. The final release restores all ROM PTEs before returning their private
 * pages. This lifetime is deliberately independent of display foreground.
 */
enum x86_guest_space_status x86_guest_space_firmware_execution_acquire(
	kernel_object_handle_t machine_identity,
	struct x86_guest_space_firmware_binding *binding) __must_check;
enum x86_guest_space_status x86_guest_space_firmware_execution_release(
	kernel_object_handle_t machine_identity,
	const struct x86_guest_space_firmware_binding *binding) __must_check;
/* Emergency fail-closed path: restore aliases and quarantine backing pages. */
enum x86_guest_space_status x86_guest_space_firmware_execution_quarantine(
	kernel_object_handle_t machine_identity,
	const struct x86_guest_space_firmware_binding *binding) __must_check;
enum x86_guest_space_status x86_guest_space_firmware_write_fault(
	kernel_object_handle_t machine_identity,
	const struct x86_guest_space_firmware_binding *binding,
	uint32_t page_fault_error, uint32_t fault_address) __must_check;

static_assert_expression(sizeof(struct x86_guest_space_binding) == 56u,
			 "guest-space bindings must remain fixed width");
static_assert_expression(sizeof(struct x86_guest_space_config) == 56u,
			 "guest-space config layout changed");
static_assert_expression(sizeof(struct x86_guest_space_pit_binding) == 40u,
			 "guest-space PIT binding layout changed");
static_assert_expression(
	sizeof(struct x86_guest_space_firmware_binding) == 48u,
	"guest-space firmware binding layout changed");

#endif

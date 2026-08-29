// SPDX-License-Identifier: GPL-2.0-only
/*
 * DOS-C32 direct i386 VM86 backend.
 *
 * This singleton kernel execution engine consumes DOS-C32 fixed-width CPU state and
 * emits the same typed events as future interpreter and 64-bit backends.
 * DOS-visible interrupt behavior remains outside this file.
 */
#include "x86_vm86.h"

#include "dos_control_instruction.h"
#include "dos_interrupt_reflection.h"
#include "dos_port_instruction.h"
#include "vm86_firmware.h"
#include "x86_guest_space.h"
#include "x86_legacy_irq.h"
#include "x86_paging.h"

#define X86_VM86_INT_OPCODE 0xcdu
#define X86_VM86_STI_OPCODE 0xfbu
#define X86_VM86_BACKEND_SLOT_COUNT 4u
#define X86_VM86_BACKEND_SLOT_BITS 3u
#define X86_VM86_BACKEND_SLOT_MASK 0x07ull
#define X86_VM86_BACKEND_GENERATION_MAX 0x1fffffffffffffffull
#if CONFIG_X86_VM_ACCEPTANCE_DIAGNOSTICS
#define X86_VM86_INTERRUPT_HISTORY_COUNT 32u
#endif

enum x86_vm86_prepare_detail {
	X86_VM86_PREPARE_NO_SLOT = 1u,
	X86_VM86_PREPARE_ADDRESS_SPACE,
	X86_VM86_PREPARE_HANDOFF
};

extern bool x86_vm86_enter(struct dos_cpu_state *state);
extern void x86_vm86_kernel_return(void);

struct x86_vm86_tail {
	uint32_t stack_pointer;
	uint32_t stack_segment;
	uint32_t es;
	uint32_t ds;
	uint32_t fs;
	uint32_t gs;
};

static const struct dos_machine *active_machine;
static kernel_object_handle_t active_machine_identity =
	KERNEL_OBJECT_HANDLE_INVALID;
static struct dos_cpu_state *active_state;
static struct dos_execution_event *active_event;
static struct x86_vm86_backend_slot *active_backend_slot;
static const struct x86_guest_space_firmware_binding
	*active_firmware_binding;
static uint32_t active_virtual_flags;
static bool active;
#if CONFIG_X86_VM_ACCEPTANCE_DIAGNOSTICS
static struct dos_cpu_state last_software_interrupt_state;
static uint8_t last_software_interrupt_vector;
static bool last_software_interrupt_valid;

struct x86_vm86_interrupt_receipt {
	struct dos_cpu_state state;
	uint8_t vector;
	uint8_t valid;
	uint8_t reserved[6];
} __aligned(8);

static struct x86_vm86_interrupt_receipt interrupt_history[
	X86_VM86_INTERRUPT_HISTORY_COUNT];
static uint32_t interrupt_history_cursor;
#endif

struct x86_vm86_backend_slot {
	uint64_t generation;
	struct x86_guest_space_binding binding;
	struct x86_guest_space_firmware_binding firmware_binding;
	struct dos_cpu_state shadow_port_state;
	uint8_t in_use;
	uint8_t halted;
	uint8_t interrupt_shadow;
	uint8_t shadow_port_pending;
	uint8_t reserved[4];
} __aligned(8);

struct x86_vm86_backend_owner {
	struct x86_vm86_backend_slot slots[X86_VM86_BACKEND_SLOT_COUNT];
	kernel_object_handle_t adapter_context;
	uint8_t reusable_mask;
	uint8_t initialized;
	uint8_t reserved[6];
} __aligned(8);

static struct x86_vm86_backend_owner backend_owner;

static enum dos_exec_backend_prepare_status vm86_backend_prepare(
	kernel_object_handle_t context, const struct dos_machine *machine,
	kernel_object_handle_t machine_identity,
	const struct dos_exec_handoff_plan *handoff,
	struct dos_exec_backend_prepare_result *result);
static enum dos_exec_backend_release_status vm86_backend_release(
	kernel_object_handle_t context, kernel_object_handle_t backend_context);
static enum dos_exec_backend_run_status vm86_backend_run(
	kernel_object_handle_t context, kernel_object_handle_t backend_context,
	kernel_object_handle_t machine_identity,
	const struct dos_machine *machine, struct dos_cpu_state *state,
	struct dos_execution_event *event);
static bool state_is_valid(const struct dos_cpu_state *state);
static enum dos_exec_backend_state_commit_status
vm86_backend_commit_state_replacement(
	kernel_object_handle_t context, kernel_object_handle_t backend_context,
	const struct dos_cpu_state *expected_state,
	const struct dos_cpu_state *replacement_state);
static bool vm86_run_identity_until_event(struct x86_vm86_backend_slot *slot,
					  const struct dos_machine *machine,
					  kernel_object_handle_t machine_identity,
					  struct dos_cpu_state *state,
					  struct dos_execution_event *event);

static struct dos_exec_backend_ops backend_ops = {
	.identity = KERNEL_OBJECT_HANDLE_INVALID,
	.capabilities = DOS_EXEC_CAP_VM86,
	.prepare = vm86_backend_prepare,
	.release = vm86_backend_release,
	.run_until_event = vm86_backend_run,
	.commit_state_replacement = vm86_backend_commit_state_replacement,
};

static_assert_expression(sizeof(struct x86_vm86_tail) == 24u,
			 "hardware VM86 tail must contain six dwords");
static_assert_expression(sizeof(struct x86_vm86_backend_slot) == 176u,
			 "VM86 backend slots must stay fixed width");

static bool identity_is_valid(kernel_object_handle_t identity)
{
	return identity != 0u && identity != KERNEL_OBJECT_HANDLE_INVALID;
}

static kernel_object_handle_t backend_handle(uint32_t slot_index,
					      uint64_t generation)
{
	return (generation << X86_VM86_BACKEND_SLOT_BITS) |
	       (kernel_object_handle_t)(slot_index + 1u);
}

static bool decode_backend_handle(kernel_object_handle_t handle,
				  uint32_t *slot_index)
{
	uint32_t encoded_slot;
	uint64_t generation;

	if (slot_index == NULL || handle == 0u ||
	    handle == KERNEL_OBJECT_HANDLE_INVALID)
		return false;
	encoded_slot = (uint32_t)(handle & X86_VM86_BACKEND_SLOT_MASK);
	generation = handle >> X86_VM86_BACKEND_SLOT_BITS;
	if (encoded_slot == 0u || encoded_slot > X86_VM86_BACKEND_SLOT_COUNT ||
	    generation == 0u || generation > X86_VM86_BACKEND_GENERATION_MAX ||
	    backend_owner.slots[encoded_slot - 1u].generation != generation ||
	    backend_owner.slots[encoded_slot - 1u].in_use != 1u)
		return false;
	*slot_index = encoded_slot - 1u;
	return true;
}

enum x86_vm86_backend_status x86_vm86_backend_initialize(
	kernel_object_handle_t adapter_identity,
	kernel_object_handle_t adapter_context)
{
	uint32_t index;

	if (!identity_is_valid(adapter_identity) ||
	    !identity_is_valid(adapter_context) ||
	    adapter_identity == adapter_context)
		return X86_VM86_BACKEND_INVALID_ARGUMENT;
	if (backend_owner.initialized != 0u ||
	    backend_ops.identity != KERNEL_OBJECT_HANDLE_INVALID)
		return X86_VM86_BACKEND_INVALID_STATE;
	for (index = 0u; index < X86_VM86_BACKEND_SLOT_COUNT; ++index)
		backend_owner.slots[index] =
			(struct x86_vm86_backend_slot){0};
	backend_owner.adapter_context = adapter_context;
	backend_owner.reusable_mask =
		(uint8_t)((1u << X86_VM86_BACKEND_SLOT_COUNT) - 1u);
	backend_owner.initialized = 1u;
	backend_ops.identity = adapter_identity;
	return X86_VM86_BACKEND_OK;
}

const struct dos_exec_backend_ops *x86_vm86_backend_ops(void)
{
	return backend_owner.initialized == 1u ? &backend_ops : NULL;
}

kernel_object_handle_t x86_vm86_backend_context(void)
{
	return backend_owner.initialized == 1u
		       ? backend_owner.adapter_context
		       : KERNEL_OBJECT_HANDLE_INVALID;
}

static bool reserve_backend_slot(uint32_t *slot_index)
{
	while (backend_owner.reusable_mask != 0u) {
		uint32_t index =
			(uint32_t)__builtin_ctz(backend_owner.reusable_mask);
		struct x86_vm86_backend_slot *slot =
			&backend_owner.slots[index];

		backend_owner.reusable_mask &= (uint8_t)~(1u << index);
		if (slot->generation >= X86_VM86_BACKEND_GENERATION_MAX)
			continue;
		++slot->generation;
		slot->in_use = 1u;
		slot->halted = 0u;
		slot->interrupt_shadow = 0u;
		slot->shadow_port_pending = 0u;
		slot->shadow_port_state = (struct dos_cpu_state){0};
		*slot_index = index;
		return true;
	}
	return false;
}

static void vacate_backend_slot(uint32_t slot_index)
{
	struct x86_vm86_backend_slot *slot = &backend_owner.slots[slot_index];

	slot->binding = (struct x86_guest_space_binding){0};
	slot->firmware_binding =
		(struct x86_guest_space_firmware_binding){0};
	slot->halted = 0u;
	slot->interrupt_shadow = 0u;
	slot->shadow_port_pending = 0u;
	slot->shadow_port_state = (struct dos_cpu_state){0};
	slot->in_use = 0u;
	if (slot->generation < X86_VM86_BACKEND_GENERATION_MAX)
		backend_owner.reusable_mask |= (uint8_t)(1u << slot_index);
}

static bool handoff_is_directly_addressable(
	const struct dos_exec_handoff_plan *handoff)
{
	uint32_t entry_linear;
	uint32_t stack_linear;

	if (!dos_exec_handoff_plan_has_valid_encoding(handoff) ||
	    (handoff->entry_state.eip & 0xffff0000u) != 0u ||
	    (handoff->entry_state.esp & 0xffff0000u) != 0u)
		return false;
	entry_linear = ((uint32_t)handoff->entry_state.cs << 4u) +
		       handoff->entry_state.eip;
	stack_linear = ((uint32_t)handoff->entry_state.ss << 4u) +
		       handoff->entry_state.esp;
	return entry_linear < X86_LEGACY_ROM_LIMIT &&
	       stack_linear < X86_LEGACY_ROM_LIMIT;
}

static enum dos_exec_backend_prepare_status vm86_backend_prepare(
	kernel_object_handle_t context, const struct dos_machine *machine,
	kernel_object_handle_t machine_identity,
	const struct dos_exec_handoff_plan *handoff,
	struct dos_exec_backend_prepare_result *result)
{
	struct x86_guest_space_binding binding;
	uint32_t slot_index;

	if (result == NULL)
		return DOS_EXEC_BACKEND_PREPARE_UNCERTAIN;
	*result = (struct dos_exec_backend_prepare_result){
		.backend_context = KERNEL_OBJECT_HANDLE_INVALID,
		.failure_detail = 0u,
		.reserved = {0u},
	};
	if (backend_owner.initialized != 1u ||
	    context != backend_owner.adapter_context)
		return DOS_EXEC_BACKEND_PREPARE_UNCERTAIN;
	if (!handoff_is_directly_addressable(handoff)) {
		result->failure_detail = X86_VM86_PREPARE_HANDOFF;
		return DOS_EXEC_BACKEND_REJECTED;
	}
	if (x86_guest_space_pin(machine_identity, machine, &binding) !=
	    X86_GUEST_SPACE_OK) {
		result->failure_detail = X86_VM86_PREPARE_ADDRESS_SPACE;
		return DOS_EXEC_BACKEND_REJECTED;
	}
	if (!reserve_backend_slot(&slot_index)) {
		result->failure_detail = X86_VM86_PREPARE_NO_SLOT;
		return DOS_EXEC_BACKEND_REJECTED;
	}
	backend_owner.slots[slot_index].binding = binding;
	if (x86_guest_space_firmware_execution_acquire(
		    machine_identity,
		    &backend_owner.slots[slot_index].firmware_binding) !=
	    X86_GUEST_SPACE_OK) {
		vacate_backend_slot(slot_index);
		result->failure_detail = X86_VM86_PREPARE_ADDRESS_SPACE;
		return DOS_EXEC_BACKEND_REJECTED;
	}
	result->backend_context = backend_handle(
		slot_index, backend_owner.slots[slot_index].generation);
	return DOS_EXEC_BACKEND_PREPARED;
}

static enum dos_exec_backend_release_status vm86_backend_release(
	kernel_object_handle_t context, kernel_object_handle_t backend_context)
{
	struct x86_vm86_backend_slot *slot;
	enum dos_exec_backend_release_status status;
	uint32_t slot_index;

	if (backend_owner.initialized != 1u ||
	    context != backend_owner.adapter_context ||
	    !decode_backend_handle(backend_context, &slot_index))
		return DOS_EXEC_BACKEND_RELEASE_UNCERTAIN;
	slot = &backend_owner.slots[slot_index];
	status = x86_vm86_firmware_execution_release(
		slot->binding.machine_identity, &slot->firmware_binding);
	vacate_backend_slot(slot_index);
	return status;
}

static enum dos_exec_backend_run_status vm86_backend_run(
	kernel_object_handle_t context, kernel_object_handle_t backend_context,
	kernel_object_handle_t machine_identity,
	const struct dos_machine *machine, struct dos_cpu_state *state,
	struct dos_execution_event *event)
{
	uint32_t slot_index;

	if (backend_owner.initialized != 1u ||
	    context != backend_owner.adapter_context || state == NULL ||
	    event == NULL || !x86_legacy_irq_is_initialized() ||
	    !decode_backend_handle(backend_context, &slot_index) ||
	    !x86_guest_space_binding_is_active(
		&backend_owner.slots[slot_index].binding, machine_identity,
		machine) ||
	    backend_owner.slots[slot_index].shadow_port_pending != 0u ||
	    (state->mode != (uint32_t)DOS_CPU_REAL16 &&
	     state->mode != (uint32_t)DOS_CPU_VM86))
		return DOS_EXEC_BACKEND_RUN_UNCERTAIN;
	state->mode = (uint32_t)DOS_CPU_VM86;
	return vm86_run_identity_until_event(
		       &backend_owner.slots[slot_index], machine, machine_identity,
		       state, event)
		       ? DOS_EXEC_BACKEND_EVENT
		       : DOS_EXEC_BACKEND_RUN_UNCERTAIN;
}

static bool cpu_states_equal(const struct dos_cpu_state *left,
			     const struct dos_cpu_state *right)
{
	return left->eax == right->eax && left->ebx == right->ebx &&
	       left->ecx == right->ecx && left->edx == right->edx &&
	       left->esi == right->esi && left->edi == right->edi &&
	       left->ebp == right->ebp && left->esp == right->esp &&
	       left->eip == right->eip && left->eflags == right->eflags &&
	       left->cs == right->cs && left->ss == right->ss &&
	       left->ds == right->ds && left->es == right->es &&
	       left->fs == right->fs && left->gs == right->gs &&
	       left->mode == right->mode;
}

static enum dos_exec_backend_state_commit_status
vm86_backend_commit_state_replacement(
	kernel_object_handle_t context, kernel_object_handle_t backend_context,
	const struct dos_cpu_state *expected_state,
	const struct dos_cpu_state *replacement_state)
{
	struct x86_vm86_backend_slot *slot;
	uint32_t slot_index;

	if (backend_owner.initialized != 1u ||
	    context != backend_owner.adapter_context || expected_state == NULL ||
	    replacement_state == NULL ||
	    !decode_backend_handle(backend_context, &slot_index))
		return DOS_EXEC_BACKEND_STATE_COMMIT_UNCERTAIN;
	slot = &backend_owner.slots[slot_index];
	if (slot->shadow_port_pending == 0u)
		return DOS_EXEC_BACKEND_STATE_COMMITTED;
	if (slot->interrupt_shadow != 1u ||
	    !cpu_states_equal(&slot->shadow_port_state, expected_state) ||
	    !state_is_valid(replacement_state))
		return DOS_EXEC_BACKEND_STATE_REJECTED;
	slot->shadow_port_state = (struct dos_cpu_state){0};
	slot->shadow_port_pending = 0u;
	slot->interrupt_shadow = 0u;
	return DOS_EXEC_BACKEND_STATE_COMMITTED;
}

static bool state_is_valid(const struct dos_cpu_state *state)
{
	return state != NULL && state->mode == (uint32_t)DOS_CPU_VM86 &&
	       (state->eip & 0xffff0000u) == 0u &&
	       (state->esp & 0xffff0000u) == 0u;
}

static struct x86_vm86_tail *vm86_tail(struct x86_trap_frame *frame)
{
	return (struct x86_vm86_tail *)(void *)
		((uint8_t *)frame + sizeof(*frame));
}

static void capture_state(const struct x86_trap_frame *frame,
			  const struct x86_vm86_tail *tail,
			  struct dos_cpu_state *state)
{
	uint32_t flags =
		(frame->flags &
		 ~(DOS_EFLAGS_TF | DOS_EFLAGS_IF | DOS_EFLAGS_IOPL |
		   DOS_EFLAGS_NT)) |
		(active_virtual_flags &
		 (DOS_EFLAGS_TF | DOS_EFLAGS_IF | DOS_EFLAGS_IOPL |
		  DOS_EFLAGS_NT));

	*state = (struct dos_cpu_state){
		.eax = frame->eax,
		.ebx = frame->ebx,
		.ecx = frame->ecx,
		.edx = frame->edx,
		.esi = frame->esi,
		.edi = frame->edi,
		.ebp = frame->ebp,
		.esp = tail->stack_pointer & 0xffffu,
		.eip = frame->instruction_pointer & 0xffffu,
		.eflags = flags,
		.cs = (uint16_t)frame->code_segment,
		.ss = (uint16_t)tail->stack_segment,
		.ds = (uint16_t)tail->ds,
		.es = (uint16_t)tail->es,
		.fs = (uint16_t)tail->fs,
		.gs = (uint16_t)tail->gs,
		.mode = (uint32_t)DOS_CPU_VM86,
	};
}

static void resume_emulated_control(
	struct x86_trap_frame *frame, struct x86_vm86_tail *tail,
	const struct dos_cpu_state *state)
{
	uint32_t physical_flags =
		state->eflags &
		~(DOS_EFLAGS_IF | DOS_EFLAGS_IOPL | DOS_EFLAGS_NT);

	if (active_backend_slot != NULL &&
	    active_backend_slot->interrupt_shadow != 0u)
		physical_flags |= DOS_EFLAGS_TF;

	active_virtual_flags = state->eflags;
	*active_state = *state;
	frame->instruction_pointer = state->eip;
	frame->code_segment = state->cs;
	/* The native timer must keep advancing while the guest has virtual IF
	 * clear.  Hardware IF therefore stays set; active_virtual_flags alone
	 * controls whether the modeled PIC may deliver a pending request. */
	frame->flags = physical_flags | DOS_EFLAGS_IF | DOS_EFLAGS_VM |
		       DOS_EFLAGS_RESERVED_ONE;
	tail->stack_pointer = state->esp;
	tail->stack_segment = state->ss;
}

static void leave_vm86(struct x86_trap_frame *frame)
{
	/* The assembly continuation restores its saved kernel stack explicitly. */
	frame->instruction_pointer = (uint32_t)(uintptr_t)&x86_vm86_kernel_return;
	frame->code_segment = 0x08u;
	frame->flags = DOS_EFLAGS_RESERVED_ONE;
	frame->ds = 0x10u;
	frame->es = 0x10u;
	frame->fs = 0x10u;
	frame->gs = 0x10u;
}

static uint32_t page_fault_linear_address(void)
{
#if defined(DOSC32_HOST_TEST)
	return 0u;
#else
	uint32_t address;

	__asm__ volatile("movl %%cr2, %0" : "=r"(address));
	return address;
#endif
}

enum pending_reflection_status {
	PENDING_REFLECTION_NONE = 0,
	PENDING_REFLECTION_APPLIED,
	PENDING_REFLECTION_SESSION_FAULT,
	PENDING_REFLECTION_SYSTEM_FAULT
};

static enum pending_reflection_status reflect_pending_interrupt(
	kernel_object_handle_t machine_identity,
	const struct dos_machine *machine, struct dos_cpu_state *state,
	struct dos_execution_event *event)
{
	struct x86_legacy_interrupt_claim claim;
	struct dos_interrupt_reflection_result reflection;
	enum dos_machine_status rollback_status;
	enum x86_guest_space_status cancel_status;
	enum x86_guest_space_status claim_status;
	enum x86_guest_space_status commit_status;
	enum x86_guest_space_status quarantine_status;
	bool transaction_uncertain;

	if ((state->eflags & DOS_EFLAGS_IF) == 0u)
		return PENDING_REFLECTION_NONE;
	claim_status = x86_guest_space_interrupt_prepare(machine_identity,
						      &claim);
	if (claim_status == X86_GUEST_SPACE_NO_INTERRUPT)
		return PENDING_REFLECTION_NONE;
	if (claim_status != X86_GUEST_SPACE_OK)
		return PENDING_REFLECTION_SYSTEM_FAULT;
	reflection = dos_interrupt_reflect(machine, claim.vector, state);
	if (reflection.status != (uint32_t)DOS_INTERRUPT_REFLECTION_OK) {
		cancel_status =
			x86_guest_space_interrupt_cancel(machine_identity, &claim);
		transaction_uncertain =
			reflection.status ==
				(uint32_t)DOS_INTERRUPT_REFLECTION_ROLLBACK_FAILED ||
			cancel_status != X86_GUEST_SPACE_OK;
		if (transaction_uncertain) {
			quarantine_status =
				x86_guest_space_quarantine(machine_identity, machine);
			if (quarantine_status != X86_GUEST_SPACE_OK)
				transaction_uncertain = true;
		}
		*event = (struct dos_execution_event){
			.kind = (uint32_t)DOS_EXEC_EVENT_FAULT,
			.value = transaction_uncertain
					 ? (uint32_t)DOS_MACHINE_ROLLBACK_FAILED
					 : reflection.machine_status,
			.vector = claim.vector,
		};
		return PENDING_REFLECTION_SESSION_FAULT;
	}
	commit_status =
		x86_guest_space_interrupt_commit(machine_identity, &claim);
	if (commit_status != X86_GUEST_SPACE_OK) {
		/* Stack publication followed reservation, so unwind it first.  A failed
		 * PIC commit leaves its reservation cancelable by contract. */
		rollback_status = dos_interrupt_reflection_rollback(
			machine, &reflection.receipt);
		cancel_status =
			x86_guest_space_interrupt_cancel(machine_identity, &claim);
		transaction_uncertain = rollback_status != DOS_MACHINE_OK ||
					cancel_status != X86_GUEST_SPACE_OK;
		if (transaction_uncertain) {
			quarantine_status =
				x86_guest_space_quarantine(machine_identity, machine);
			if (quarantine_status != X86_GUEST_SPACE_OK)
				transaction_uncertain = true;
		}
		*event = (struct dos_execution_event){
			.kind = (uint32_t)DOS_EXEC_EVENT_FAULT,
			.value = transaction_uncertain
					 ? (uint32_t)DOS_MACHINE_ROLLBACK_FAILED
					 : (uint32_t)DOS_MACHINE_IO_FAULT,
			.vector = claim.vector,
		};
		return PENDING_REFLECTION_SESSION_FAULT;
	}
	*state = reflection.state;
	return PENDING_REFLECTION_APPLIED;
}

static bool resume_with_pending_interrupt(
	struct x86_trap_frame *frame, struct x86_vm86_tail *tail,
	const struct dos_cpu_state *state)
{
	struct dos_cpu_state prepared = *state;
	enum pending_reflection_status status;

	status = reflect_pending_interrupt(active_machine_identity,
					   active_machine, &prepared,
					   active_event);
	if (status == PENDING_REFLECTION_SYSTEM_FAULT)
		return false;
	if (status == PENDING_REFLECTION_SESSION_FAULT) {
		*active_state = prepared;
		leave_vm86(frame);
		return true;
	}
	resume_emulated_control(frame, tail, &prepared);
	return true;
}

enum x86_vm86_interrupt_delivery_status
x86_vm86_deliver_pending_interrupt(struct x86_trap_frame *frame)
{
	enum pending_reflection_status status;
	struct x86_vm86_tail *tail;

	if (!active || frame == NULL || active_machine == NULL ||
	    active_state == NULL || active_event == NULL)
		return X86_VM86_INTERRUPT_INACTIVE;
	if ((frame->flags & DOS_EFLAGS_VM) == 0u ||
	    (active_backend_slot != NULL &&
	     active_backend_slot->interrupt_shadow != 0u) ||
	    (active_virtual_flags & DOS_EFLAGS_IF) == 0u)
		return X86_VM86_INTERRUPT_DEFERRED;
	tail = vm86_tail(frame);
	capture_state(frame, tail, active_state);
	status = reflect_pending_interrupt(active_machine_identity,
					   active_machine, active_state,
					   active_event);
	if (status == PENDING_REFLECTION_NONE)
		return X86_VM86_INTERRUPT_NONE;
	if (status == PENDING_REFLECTION_SYSTEM_FAULT)
		return X86_VM86_INTERRUPT_SYSTEM_FAULT;
	if (status == PENDING_REFLECTION_SESSION_FAULT) {
		leave_vm86(frame);
		return X86_VM86_INTERRUPT_SESSION_FAULT;
	}
	resume_emulated_control(frame, tail, active_state);
	return X86_VM86_INTERRUPT_DELIVERED;
}

bool x86_vm86_handle_trap(struct x86_trap_frame *frame)
{
	struct dos_control_instruction_result control;
	struct dos_port_instruction_result port_instruction;
	struct x86_vm86_tail *tail;
	enum pending_reflection_status pending_status;
	uint8_t interrupt_vector;
	uint8_t opcode;
	uint8_t effective_opcode;
	bool operand32;
	bool shadowed_instruction;
	bool virtual_interrupts_were_disabled;
	uint16_t instruction_pointer;

	if (!active || frame == NULL || active_machine == NULL ||
	    active_state == NULL || active_event == NULL ||
	    (frame->flags & DOS_EFLAGS_VM) == 0u)
		return false;

	tail = vm86_tail(frame);
	capture_state(frame, tail, active_state);
	shadowed_instruction = active_backend_slot != NULL &&
			       active_backend_slot->interrupt_shadow != 0u;
	virtual_interrupts_were_disabled =
		(active_state->eflags & DOS_EFLAGS_IF) == 0u;
	if (frame->vector == (uint32_t)X86_EXCEPTION_PAGE_FAULT &&
	    active_firmware_binding != NULL &&
	    x86_guest_space_firmware_write_fault(
		    active_machine_identity, active_firmware_binding,
		    frame->error_code, page_fault_linear_address()) ==
		    X86_GUEST_SPACE_OK) {
		/* The PTE is now writable private RAM; IRET retries the instruction. */
		return true;
	}
	if (frame->vector == (uint32_t)X86_EXCEPTION_DEBUG &&
	    shadowed_instruction &&
	    (active_virtual_flags & DOS_EFLAGS_TF) == 0u) {
		active_backend_slot->interrupt_shadow = 0u;
		return resume_with_pending_interrupt(frame, tail, active_state);
	}
	if (frame->vector != (uint32_t)X86_EXCEPTION_GENERAL_PROTECTION ||
	    frame->error_code != 0u ||
	    dos_machine_read_far(active_machine, active_state->cs,
				 active_state->eip, &opcode, sizeof(opcode),
				 sizeof(opcode)) !=
		DOS_MACHINE_OK) {
		if (shadowed_instruction)
			active_backend_slot->interrupt_shadow = 0u;
		*active_event = (struct dos_execution_event){
			.kind = (uint32_t)DOS_EXEC_EVENT_FAULT,
			.value = frame->error_code,
			.vector = (uint8_t)frame->vector,
		};
		leave_vm86(frame);
		return true;
	}

	if (opcode != X86_VM86_INT_OPCODE) {
		operand32 = opcode == 0x66u;
		effective_opcode = opcode;
		if (operand32 &&
		    dos_machine_read_far(
			    active_machine, active_state->cs,
			    (uint16_t)(active_state->eip + 1u),
			    &effective_opcode, sizeof(effective_opcode),
			    sizeof(effective_opcode)) != DOS_MACHINE_OK) {
			*active_event = (struct dos_execution_event){
				.kind = (uint32_t)DOS_EXEC_EVENT_FAULT,
				.vector = (uint8_t)frame->vector,
			};
			leave_vm86(frame);
			return true;
		}
		control = dos_control_instruction_emulate(
			active_machine, effective_opcode, operand32,
			active_state);
		if (control.status ==
		    (uint32_t)DOS_CONTROL_INSTRUCTION_HALTED) {
			if (active_backend_slot == NULL)
				return false;
			active_backend_slot->interrupt_shadow = 0u;
			active_backend_slot->halted = 1u;
			pending_status = reflect_pending_interrupt(
				active_machine_identity, active_machine, &control.state,
				active_event);
			if (pending_status == PENDING_REFLECTION_SYSTEM_FAULT)
				return false;
			if (pending_status == PENDING_REFLECTION_SESSION_FAULT) {
				*active_state = control.state;
				leave_vm86(frame);
				return true;
			}
			if (pending_status == PENDING_REFLECTION_APPLIED) {
				active_backend_slot->halted = 0u;
				resume_emulated_control(frame, tail, &control.state);
				return true;
			}
			*active_state = control.state;
			*active_event = (struct dos_execution_event){
				.kind = (uint32_t)DOS_EXEC_EVENT_HALTED,
			};
			leave_vm86(frame);
			return true;
		}
		if (control.status ==
		    (uint32_t)DOS_CONTROL_INSTRUCTION_EMULATED) {
			if (effective_opcode == X86_VM86_STI_OPCODE &&
			    virtual_interrupts_were_disabled &&
			    !shadowed_instruction) {
				active_backend_slot->interrupt_shadow = 1u;
				resume_emulated_control(frame, tail, &control.state);
				return true;
			}
			if (shadowed_instruction)
				active_backend_slot->interrupt_shadow = 0u;
			return resume_with_pending_interrupt(frame, tail,
						     &control.state);
		}
		if (control.status !=
		    (uint32_t)DOS_CONTROL_INSTRUCTION_NOT_HANDLED) {
			*active_event = (struct dos_execution_event){
				.kind = (uint32_t)DOS_EXEC_EVENT_FAULT,
				.value = control.machine_status,
				.vector = (uint8_t)frame->vector,
			};
			leave_vm86(frame);
			return true;
		}
		port_instruction = dos_port_instruction_decode(active_machine,
							       active_state);
		if (port_instruction.status ==
		    (uint32_t)DOS_PORT_INSTRUCTION_DECODED) {
			if (shadowed_instruction) {
				active_backend_slot->shadow_port_state =
					port_instruction.state;
				active_backend_slot->shadow_port_pending = 1u;
			}
			*active_state = port_instruction.state;
			*active_event = port_instruction.event;
			leave_vm86(frame);
			return true;
		}
		if (port_instruction.status !=
		    (uint32_t)DOS_PORT_INSTRUCTION_NOT_HANDLED) {
			*active_event = (struct dos_execution_event){
				.kind = (uint32_t)DOS_EXEC_EVENT_FAULT,
				.value = port_instruction.machine_status,
				.vector = (uint8_t)frame->vector,
			};
			leave_vm86(frame);
			return true;
		}
		if (shadowed_instruction)
			active_backend_slot->interrupt_shadow = 0u;
		*active_event = (struct dos_execution_event){
			.kind = (uint32_t)DOS_EXEC_EVENT_EXCEPTION,
			.value = opcode,
			.vector = (uint8_t)frame->vector,
		};
		leave_vm86(frame);
		return true;
	}

	instruction_pointer = (uint16_t)active_state->eip;
	instruction_pointer = (uint16_t)(instruction_pointer + 2u);
	if (dos_machine_read_far(active_machine, active_state->cs,
				 (uint16_t)(instruction_pointer - 1u),
				 &interrupt_vector, sizeof(interrupt_vector),
				 sizeof(interrupt_vector)) != DOS_MACHINE_OK) {
		*active_event = (struct dos_execution_event){
			.kind = (uint32_t)DOS_EXEC_EVENT_FAULT,
			.vector = (uint8_t)frame->vector,
		};
		leave_vm86(frame);
		return true;
	}
	active_state->eip = instruction_pointer;
	if (shadowed_instruction)
		active_backend_slot->interrupt_shadow = 0u;
#if CONFIG_X86_VM_ACCEPTANCE_DIAGNOSTICS
	last_software_interrupt_state = *active_state;
	last_software_interrupt_vector = interrupt_vector;
	last_software_interrupt_valid = true;
	interrupt_history[interrupt_history_cursor] =
		(struct x86_vm86_interrupt_receipt){
			.state = *active_state,
			.vector = interrupt_vector,
			.valid = 1u,
			.reserved = {0u},
		};
	interrupt_history_cursor =
		(interrupt_history_cursor + 1u) % X86_VM86_INTERRUPT_HISTORY_COUNT;
#endif
	*active_event = (struct dos_execution_event){
		.kind = (uint32_t)DOS_EXEC_EVENT_SOFTWARE_INTERRUPT,
		.vector = interrupt_vector,
	};
	leave_vm86(frame);
	return true;
}

#if CONFIG_X86_VM_ACCEPTANCE_DIAGNOSTICS
bool x86_vm86_recent_software_interrupt(uint32_t previous,
					 struct dos_cpu_state *state,
					 uint8_t *vector)
{
	uint32_t index;

	if (state == NULL || vector == NULL ||
	    previous >= X86_VM86_INTERRUPT_HISTORY_COUNT)
		return false;
	index = (interrupt_history_cursor + X86_VM86_INTERRUPT_HISTORY_COUNT -
		  1u - previous) % X86_VM86_INTERRUPT_HISTORY_COUNT;
	if (interrupt_history[index].valid == 0u)
		return false;
	*state = interrupt_history[index].state;
	*vector = interrupt_history[index].vector;
	return true;
}

bool x86_vm86_last_software_interrupt(struct dos_cpu_state *state,
				      uint8_t *vector)
{
	if (state == NULL || vector == NULL || !last_software_interrupt_valid)
		return false;
	*state = last_software_interrupt_state;
	*vector = last_software_interrupt_vector;
	return true;
}
#endif

static bool vm86_run_identity_until_event(struct x86_vm86_backend_slot *slot,
					  const struct dos_machine *machine,
					  kernel_object_handle_t machine_identity,
					  struct dos_cpu_state *state,
					  struct dos_execution_event *event)
{
	enum pending_reflection_status pending_status;
	bool entered;

	if (active || slot == NULL || machine == NULL || machine->ops == NULL ||
	    !state_is_valid(state) || event == NULL ||
	    !x86_paging_is_enabled())
		return false;
	*event = (struct dos_execution_event){0};
	pending_status = reflect_pending_interrupt(machine_identity, machine,
						   state, event);
	if (pending_status == PENDING_REFLECTION_SYSTEM_FAULT)
		return false;
	if (pending_status == PENDING_REFLECTION_SESSION_FAULT)
		return true;
	if (slot->halted != 0u &&
	    pending_status != PENDING_REFLECTION_APPLIED) {
		*event = (struct dos_execution_event){
			.kind = (uint32_t)DOS_EXEC_EVENT_HALTED,
		};
		return true;
	}
	if (pending_status == PENDING_REFLECTION_APPLIED)
		slot->halted = 0u;

	active_machine = machine;
	active_machine_identity = machine_identity;
	active_state = state;
	active_event = event;
	active_backend_slot = slot;
	active_firmware_binding = &slot->firmware_binding;
	active_virtual_flags = state->eflags;
	active = true;
	entered = x86_vm86_enter(state);
	active = false;
	active_event = NULL;
	active_backend_slot = NULL;
	active_firmware_binding = NULL;
	active_state = NULL;
	active_machine = NULL;
	active_machine_identity = KERNEL_OBJECT_HANDLE_INVALID;
	active_virtual_flags = 0u;
	return entered;
}

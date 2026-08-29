// SPDX-License-Identifier: GPL-2.0-only
/* Host-safe precise VM86 HLT, interrupt-shadow and delivery tests. */
#include "x86_vm86.h"

#include "dos_interrupt_reflection.h"
#include "test_entry.h"
#include "vm86_firmware.h"
#include "x86_guest_space.h"
#include "x86_legacy_irq.h"

#define TEST_ADAPTER_IDENTITY ((kernel_object_handle_t)0x564d383641444150ull)
#define TEST_ADAPTER_CONTEXT ((kernel_object_handle_t)0x564d3836434f4e54ull)
#define TEST_MACHINE_IDENTITY ((kernel_object_handle_t)0x564d38364d414348ull)
#define TEST_MACHINE_CONTEXT ((kernel_object_handle_t)0x564d38364d454d31ull)
#define TEST_ADDRESS_SPACE ((kernel_object_handle_t)0x564d383641444452ull)
#define TEST_MEMORY_BYTES 0x00100000u
#define TEST_PROGRAM_SEGMENT 0x1000u
#define TEST_HANDLER_SEGMENT 0x2000u
#define TEST_STACK_SEGMENT 0x3000u
#define TEST_STACK_POINTER 0x1000u
#define TEST_IRQ_VECTOR 0x20u

enum test_injection_point {
	TEST_INJECT_NONE = 0,
	TEST_INJECT_BEFORE_HLT,
	TEST_INJECT_AFTER_ORDINARY,
	TEST_INJECT_BEFORE_CLI,
	TEST_INJECT_BEFORE_SECOND_STI,
	TEST_INJECT_BEFORE_PORT
};

struct test_vm86_tail {
	uint32_t stack_pointer;
	uint32_t stack_segment;
	uint32_t es;
	uint32_t ds;
	uint32_t fs;
	uint32_t gs;
};

struct test_trap_frame {
	struct x86_trap_frame frame;
	struct test_vm86_tail tail;
};

static uint8_t guest_memory[TEST_MEMORY_BYTES];
static struct dos_machine machine;
static uint32_t write_calls;
static uint32_t fail_write_call;
static uint32_t fail_writes_from_call;
static bool fake_pending;
static bool fake_masked;
static bool fake_claim_active;
static struct x86_legacy_interrupt_claim fake_claim;
static enum x86_guest_space_status fake_commit_status;
static enum x86_guest_space_status fake_cancel_status;
static uint64_t fake_token;
static uint32_t prepare_calls;
static uint32_t commit_calls;
static uint32_t cancel_calls;
static uint32_t quarantine_calls;
static uint32_t enter_calls;
static enum test_injection_point injection_point;
static bool injection_fired;
static uint32_t observed_sti_count;

bool x86_vm86_enter(struct dos_cpu_state *state);
void x86_vm86_kernel_return(void);

static uint16_t read_le16(size_t address)
{
	return (uint16_t)guest_memory[address] |
	       ((uint16_t)guest_memory[address + 1u] << 8u);
}

static void write_le16(size_t address, uint16_t value)
{
	guest_memory[address] = (uint8_t)value;
	guest_memory[address + 1u] = (uint8_t)(value >> 8u);
}

static size_t state_linear(const struct test_trap_frame *trap)
{
	return ((size_t)(uint16_t)trap->frame.code_segment << 4u) +
	       (size_t)(uint16_t)trap->frame.instruction_pointer;
}

static enum dos_machine_status test_read_memory(
	kernel_object_handle_t context, dos_linear_address_t linear_address,
	void *destination, size_t destination_capacity, size_t count)
{
	uint8_t *output = (uint8_t *)destination;
	size_t index;

	if (context != TEST_MACHINE_CONTEXT || destination == NULL ||
	    count > destination_capacity ||
	    (size_t)linear_address > TEST_MEMORY_BYTES ||
	    count > TEST_MEMORY_BYTES - (size_t)linear_address)
		return DOS_MACHINE_IO_FAULT;
	for (index = 0u; index < count; ++index)
		output[index] = guest_memory[(size_t)linear_address + index];
	return DOS_MACHINE_OK;
}

static enum dos_machine_status test_write_memory(
	kernel_object_handle_t context, dos_linear_address_t linear_address,
	const void *source, size_t source_capacity, size_t count)
{
	const uint8_t *input = (const uint8_t *)source;
	size_t index;

	if (context != TEST_MACHINE_CONTEXT || source == NULL ||
	    count > source_capacity ||
	    (size_t)linear_address > TEST_MEMORY_BYTES ||
	    count > TEST_MEMORY_BYTES - (size_t)linear_address)
		return DOS_MACHINE_IO_FAULT;
	++write_calls;
	if (write_calls == fail_write_call)
		return DOS_MACHINE_IO_FAULT;
	if (fail_writes_from_call != 0u &&
	    write_calls >= fail_writes_from_call) {
		if (count != 0u)
			guest_memory[(size_t)linear_address] = 0xeeu;
		return DOS_MACHINE_IO_FAULT;
	}
	for (index = 0u; index < count; ++index)
		guest_memory[(size_t)linear_address + index] = input[index];
	return DOS_MACHINE_OK;
}

static const struct dos_machine_ops machine_ops = {
	.read_memory = test_read_memory,
	.write_memory = test_write_memory,
};

bool dos_exec_handoff_plan_has_valid_encoding(
	const struct dos_exec_handoff_plan *plan)
{
	return plan != NULL;
}

enum x86_guest_space_status x86_guest_space_pin(
	kernel_object_handle_t machine_identity,
	const struct dos_machine *candidate,
	struct x86_guest_space_binding *binding)
{
	if (machine_identity != TEST_MACHINE_IDENTITY || candidate != &machine ||
	    binding == NULL)
		return X86_GUEST_SPACE_MACHINE_MISMATCH;
	*binding = (struct x86_guest_space_binding){
		.address_space_identity = TEST_ADDRESS_SPACE,
		.address_space_generation = 1u,
		.machine_identity = TEST_MACHINE_IDENTITY,
		.machine_context = TEST_MACHINE_CONTEXT,
		.a20_enabled = 1u,
		.reserved = {0u},
	};
	return X86_GUEST_SPACE_OK;
}

bool x86_guest_space_binding_is_active(
	const struct x86_guest_space_binding *binding,
	kernel_object_handle_t machine_identity,
	const struct dos_machine *candidate)
{
	return binding != NULL && candidate == &machine &&
	       machine_identity == TEST_MACHINE_IDENTITY &&
	       binding->address_space_identity == TEST_ADDRESS_SPACE &&
	       binding->address_space_generation == 1u &&
	       binding->machine_identity == TEST_MACHINE_IDENTITY &&
	       binding->machine_context == TEST_MACHINE_CONTEXT;
}

enum x86_guest_space_status x86_guest_space_firmware_execution_acquire(
	kernel_object_handle_t machine_identity,
	struct x86_guest_space_firmware_binding *binding)
{
	if (machine_identity != TEST_MACHINE_IDENTITY || binding == NULL)
		return X86_GUEST_SPACE_MACHINE_MISMATCH;
	*binding = (struct x86_guest_space_firmware_binding){
		.address_space_identity = TEST_ADDRESS_SPACE,
		.address_space_generation = 1u,
		.machine_identity = TEST_MACHINE_IDENTITY,
		.execution_generation = 1u,
		.client_generation = 1u,
		.client_slot = 0u,
		.reserved = {0u},
	};
	return X86_GUEST_SPACE_OK;
}

enum dos_exec_backend_release_status x86_vm86_firmware_execution_release(
	kernel_object_handle_t machine_identity,
	const struct x86_guest_space_firmware_binding *binding)
{
	return machine_identity == TEST_MACHINE_IDENTITY && binding != NULL &&
	       binding->machine_identity == TEST_MACHINE_IDENTITY
		       ? DOS_EXEC_BACKEND_RELEASED
		       : DOS_EXEC_BACKEND_RELEASE_UNCERTAIN;
}

enum x86_guest_space_status x86_guest_space_firmware_write_fault(
	kernel_object_handle_t machine_identity,
	const struct x86_guest_space_firmware_binding *binding,
	uint32_t page_fault_error, uint32_t fault_address)
{
	(void)machine_identity;
	(void)binding;
	(void)page_fault_error;
	(void)fault_address;
	return X86_GUEST_SPACE_INVALID_STATE;
}

bool x86_legacy_irq_is_initialized(void)
{
	return true;
}

bool x86_paging_is_enabled(void)
{
	return true;
}

static bool claims_equal(const struct x86_legacy_interrupt_claim *left,
			 const struct x86_legacy_interrupt_claim *right)
{
	return left->chipset_generation == right->chipset_generation &&
	       left->delivery_token == right->delivery_token &&
	       left->irq == right->irq && left->vector == right->vector &&
	       left->cascaded == right->cascaded;
}

enum x86_guest_space_status x86_guest_space_interrupt_prepare(
	kernel_object_handle_t machine_identity,
	struct x86_legacy_interrupt_claim *claim)
{
	++prepare_calls;
	if (machine_identity != TEST_MACHINE_IDENTITY || claim == NULL)
		return X86_GUEST_SPACE_MACHINE_MISMATCH;
	if (fake_claim_active)
		return X86_GUEST_SPACE_INVALID_STATE;
	if (!fake_pending || fake_masked)
		return X86_GUEST_SPACE_NO_INTERRUPT;
	fake_claim = (struct x86_legacy_interrupt_claim){
		.chipset_generation = 1u,
		.delivery_token = ++fake_token,
		.irq = 0u,
		.vector = TEST_IRQ_VECTOR,
		.reserved = {0u},
	};
	fake_claim_active = true;
	*claim = fake_claim;
	return X86_GUEST_SPACE_OK;
}

enum x86_guest_space_status x86_guest_space_interrupt_commit(
	kernel_object_handle_t machine_identity,
	const struct x86_legacy_interrupt_claim *claim)
{
	if (machine_identity != TEST_MACHINE_IDENTITY || claim == NULL ||
	    !fake_claim_active || !claims_equal(claim, &fake_claim))
		return X86_GUEST_SPACE_STALE_BINDING;
	++commit_calls;
	if (fake_commit_status != X86_GUEST_SPACE_OK)
		return fake_commit_status;
	fake_pending = false;
	fake_claim_active = false;
	return X86_GUEST_SPACE_OK;
}

enum x86_guest_space_status x86_guest_space_interrupt_cancel(
	kernel_object_handle_t machine_identity,
	const struct x86_legacy_interrupt_claim *claim)
{
	if (machine_identity != TEST_MACHINE_IDENTITY || claim == NULL ||
	    !fake_claim_active || !claims_equal(claim, &fake_claim))
		return X86_GUEST_SPACE_STALE_BINDING;
	++cancel_calls;
	if (fake_cancel_status != X86_GUEST_SPACE_OK)
		return fake_cancel_status;
	fake_claim_active = false;
	return X86_GUEST_SPACE_OK;
}

enum x86_guest_space_status x86_guest_space_quarantine(
	kernel_object_handle_t machine_identity,
	const struct dos_machine *candidate)
{
	if (machine_identity != TEST_MACHINE_IDENTITY || candidate != &machine)
		return X86_GUEST_SPACE_MACHINE_MISMATCH;
	++quarantine_calls;
	machine.poisoned = 1u;
	return X86_GUEST_SPACE_OK;
}

void x86_vm86_kernel_return(void)
{
}

static void trap_from_state(struct test_trap_frame *trap,
			    const struct dos_cpu_state *state)
{
	*trap = (struct test_trap_frame){0};
	trap->frame.edi = state->edi;
	trap->frame.esi = state->esi;
	trap->frame.ebp = state->ebp;
	trap->frame.ebx = state->ebx;
	trap->frame.edx = state->edx;
	trap->frame.ecx = state->ecx;
	trap->frame.eax = state->eax;
	trap->frame.instruction_pointer = state->eip;
	trap->frame.code_segment = state->cs;
	trap->frame.flags =
		(state->eflags &
		 ~(DOS_EFLAGS_IF | DOS_EFLAGS_IOPL | DOS_EFLAGS_NT)) |
		DOS_EFLAGS_IF | DOS_EFLAGS_VM | DOS_EFLAGS_RESERVED_ONE;
	trap->tail.stack_pointer = state->esp;
	trap->tail.stack_segment = state->ss;
	trap->tail.es = state->es;
	trap->tail.ds = state->ds;
	trap->tail.fs = state->fs;
	trap->tail.gs = state->gs;
}

static void inject_for_opcode(uint8_t opcode)
{
	if (injection_fired)
		return;
	if ((injection_point == TEST_INJECT_BEFORE_HLT && opcode == 0xf4u) ||
	    (injection_point == TEST_INJECT_BEFORE_CLI && opcode == 0xfau) ||
	    (injection_point == TEST_INJECT_BEFORE_PORT && opcode == 0xe4u) ||
	    (injection_point == TEST_INJECT_BEFORE_SECOND_STI &&
	     opcode == 0xfbu && observed_sti_count == 1u)) {
		fake_pending = true;
		injection_fired = true;
	}
}

static int deliver_test_interrupt(struct test_trap_frame *trap)
{
	enum x86_vm86_interrupt_delivery_status status =
		x86_vm86_deliver_pending_interrupt(&trap->frame);

	if (status == X86_VM86_INTERRUPT_DEFERRED)
		return 0;
	if (status == X86_VM86_INTERRUPT_DELIVERED ||
	    status == X86_VM86_INTERRUPT_SESSION_FAULT)
		return 1;
	return -1;
}

bool x86_vm86_enter(struct dos_cpu_state *state)
{
	struct test_trap_frame trap;
	uint32_t instruction_budget;

	++enter_calls;
	trap_from_state(&trap, state);
	for (instruction_budget = 0u; instruction_budget < 16u;
	     ++instruction_budget) {
		bool injection_was_fired;
		int delivery_action;
		uint8_t opcode;
		size_t linear = state_linear(&trap);

		if (linear >= TEST_MEMORY_BYTES)
			return false;
		opcode = guest_memory[linear];
		injection_was_fired = injection_fired;
		inject_for_opcode(opcode);
		if (!injection_was_fired && injection_fired) {
			delivery_action = deliver_test_interrupt(&trap);
			if (delivery_action < 0)
				return false;
			if (delivery_action > 0) {
				if (trap.frame.code_segment == 0x08u &&
				    (trap.frame.flags & DOS_EFLAGS_VM) == 0u)
					return true;
				continue;
			}
		}
		if (opcode == 0x90u) {
			trap.frame.instruction_pointer =
				(uint16_t)(trap.frame.instruction_pointer + 1u);
			if (injection_point == TEST_INJECT_AFTER_ORDINARY &&
			    !injection_fired) {
				fake_pending = true;
				injection_fired = true;
				delivery_action = deliver_test_interrupt(&trap);
				if (delivery_action < 0)
					return false;
				if (delivery_action > 0) {
					if (trap.frame.code_segment == 0x08u &&
					    (trap.frame.flags & DOS_EFLAGS_VM) == 0u)
						return true;
					continue;
				}
			}
			if ((trap.frame.flags & DOS_EFLAGS_TF) == 0u)
				continue;
			trap.frame.vector = X86_EXCEPTION_DEBUG;
			trap.frame.error_code = 0u;
		} else {
			if (opcode == 0xfbu)
				++observed_sti_count;
			trap.frame.vector = X86_EXCEPTION_GENERAL_PROTECTION;
			trap.frame.error_code = 0u;
		}
		if (!x86_vm86_handle_trap(&trap.frame))
			return false;
		if (trap.frame.code_segment == 0x08u &&
		    (trap.frame.flags & DOS_EFLAGS_VM) == 0u)
			return true;
	}
	return false;
}

static struct dos_cpu_state initial_state(void)
{
	return (struct dos_cpu_state){
		.esp = TEST_STACK_POINTER,
		.eflags = DOS_EFLAGS_RESERVED_ONE | DOS_EFLAGS_IF,
		.cs = TEST_PROGRAM_SEGMENT,
		.ss = TEST_STACK_SEGMENT,
		.ds = TEST_PROGRAM_SEGMENT,
		.es = TEST_PROGRAM_SEGMENT,
		.mode = (uint32_t)DOS_CPU_REAL16,
	};
}

static struct dos_cpu_state initial_state_with_interrupts(bool enabled)
{
	struct dos_cpu_state state = initial_state();

	if (!enabled)
		state.eflags &= ~DOS_EFLAGS_IF;
	return state;
}

static void reset_fixture(const uint8_t *program, size_t count)
{
	size_t index;
	size_t program_linear = (size_t)TEST_PROGRAM_SEGMENT << 4u;
	size_t handler_linear = (size_t)TEST_HANDLER_SEGMENT << 4u;

	for (index = 0u; index < TEST_MEMORY_BYTES; ++index)
		guest_memory[index] = 0u;
	for (index = 0u; index < count; ++index)
		guest_memory[program_linear + index] = program[index];
	guest_memory[handler_linear] = 0xcdu;
	guest_memory[handler_linear + 1u] = 0x21u;
	write_le16((size_t)TEST_IRQ_VECTOR * 4u, 0u);
	write_le16((size_t)TEST_IRQ_VECTOR * 4u + 2u,
		   TEST_HANDLER_SEGMENT);
	machine.poisoned = 0u;
	write_calls = 0u;
	fail_write_call = 0u;
	fail_writes_from_call = 0u;
	fake_pending = false;
	fake_masked = false;
	fake_claim_active = false;
	fake_claim = (struct x86_legacy_interrupt_claim){0};
	fake_commit_status = X86_GUEST_SPACE_OK;
	fake_cancel_status = X86_GUEST_SPACE_OK;
	prepare_calls = 0u;
	commit_calls = 0u;
	cancel_calls = 0u;
	quarantine_calls = 0u;
	enter_calls = 0u;
	injection_point = TEST_INJECT_NONE;
	injection_fired = false;
	observed_sti_count = 0u;
}

static bool prepare_backend(
	const struct dos_exec_backend_ops *ops, struct dos_cpu_state *state,
	kernel_object_handle_t *backend_context)
{
	struct dos_exec_handoff_plan handoff = {0};
	struct dos_exec_backend_prepare_result result;

	handoff.entry_state = *state;
	if (ops->prepare(TEST_ADAPTER_CONTEXT, &machine, TEST_MACHINE_IDENTITY,
			 &handoff, &result) != DOS_EXEC_BACKEND_PREPARED)
		return false;
	*backend_context = result.backend_context;
	return result.backend_context != KERNEL_OBJECT_HANDLE_INVALID;
}

static bool release_backend(const struct dos_exec_backend_ops *ops,
			    kernel_object_handle_t backend_context)
{
	return ops->release(TEST_ADAPTER_CONTEXT, backend_context) ==
	       DOS_EXEC_BACKEND_RELEASED;
}

static enum dos_exec_backend_run_status run_backend(
	const struct dos_exec_backend_ops *ops,
	kernel_object_handle_t backend_context, struct dos_cpu_state *state,
	struct dos_execution_event *event)
{
	return ops->run_until_event(TEST_ADAPTER_CONTEXT, backend_context,
				    TEST_MACHINE_IDENTITY, &machine, state,
				    event);
}

static size_t reflected_stack_linear(const struct dos_cpu_state *state)
{
	return ((size_t)state->ss << 4u) + (size_t)(uint16_t)state->esp;
}

static size_t interrupt_frame_linear(const struct dos_cpu_state *state)
{
	return ((size_t)state->ss << 4u) +
	       (size_t)(uint16_t)(state->esp -
				 DOS_INTERRUPT_REFLECTION_FRAME_BYTES);
}

static void fill_interrupt_frame(size_t address, uint8_t value)
{
	size_t index;

	for (index = 0u; index < DOS_INTERRUPT_REFLECTION_FRAME_BYTES; ++index)
		guest_memory[address + index] = value;
}

static bool interrupt_frame_is(size_t address, uint8_t value)
{
	size_t index;

	for (index = 0u; index < DOS_INTERRUPT_REFLECTION_FRAME_BYTES; ++index) {
		if (guest_memory[address + index] != value)
			return false;
	}
	return true;
}

static int test_halt_wait_mask_and_retry(
	const struct dos_exec_backend_ops *ops)
{
	static const uint8_t program[] = {0xf4u};
	struct dos_execution_event event;
	struct dos_cpu_state state;
	kernel_object_handle_t backend_context;
	size_t stack_linear;

	reset_fixture(program, ARRAY_SIZE(program));
	state = initial_state();
	if (!prepare_backend(ops, &state, &backend_context) ||
	    run_backend(ops, backend_context, &state, &event) !=
		    DOS_EXEC_BACKEND_EVENT ||
	    event.kind != (uint32_t)DOS_EXEC_EVENT_HALTED || state.eip != 1u ||
	    enter_calls != 1u || prepare_calls != 2u || commit_calls != 0u)
		return 1;
	/* Empty and masked requests keep the same generation halted and never enter
	 * the CPU.  The owner may therefore use a real host wait without spinning. */
	if (run_backend(ops, backend_context, &state, &event) !=
		    DOS_EXEC_BACKEND_EVENT ||
	    event.kind != (uint32_t)DOS_EXEC_EVENT_HALTED || enter_calls != 1u)
		return 2;
	fake_pending = true;
	fake_masked = true;
	if (run_backend(ops, backend_context, &state, &event) !=
		    DOS_EXEC_BACKEND_EVENT ||
	    event.kind != (uint32_t)DOS_EXEC_EVENT_HALTED || enter_calls != 1u ||
	    commit_calls != 0u || !fake_pending)
		return 3;
	/* Once the exact request is eligible, reflection and intack complete before
	 * run returns; no intermediate HALTED result can make the owner sleep. */
	fake_masked = false;
	if (run_backend(ops, backend_context, &state, &event) !=
		    DOS_EXEC_BACKEND_EVENT ||
	    event.kind != (uint32_t)DOS_EXEC_EVENT_SOFTWARE_INTERRUPT ||
	    event.vector != 0x21u || enter_calls != 2u || commit_calls != 1u ||
	    fake_pending || state.cs != TEST_HANDLER_SEGMENT)
		return 4;
	stack_linear = reflected_stack_linear(&state);
	if (read_le16(stack_linear) != 1u ||
	    read_le16(stack_linear + 2u) != TEST_PROGRAM_SEGMENT ||
	    (read_le16(stack_linear + 4u) & DOS_EFLAGS_IF) == 0u)
		return 5;
	if (!release_backend(ops, backend_context))
		return 6;

	/* A failed guest-stack replacement cancels the reservation.  The request
	 * stays pending and a later retry intacks it exactly once. */
	reset_fixture(program, ARRAY_SIZE(program));
	state = initial_state();
	if (!prepare_backend(ops, &state, &backend_context) ||
	    run_backend(ops, backend_context, &state, &event) !=
		    DOS_EXEC_BACKEND_EVENT ||
	    event.kind != (uint32_t)DOS_EXEC_EVENT_HALTED)
		return 7;
	fake_pending = true;
	fail_write_call = 1u;
	if (run_backend(ops, backend_context, &state, &event) !=
		    DOS_EXEC_BACKEND_EVENT ||
	    event.kind != (uint32_t)DOS_EXEC_EVENT_FAULT || cancel_calls != 1u ||
	    commit_calls != 0u || !fake_pending || fake_claim_active ||
	    state.eip != 1u)
		return 8;
	if (run_backend(ops, backend_context, &state, &event) !=
		    DOS_EXEC_BACKEND_EVENT ||
	    event.kind != (uint32_t)DOS_EXEC_EVENT_SOFTWARE_INTERRUPT ||
	    commit_calls != 1u || fake_pending ||
	    !release_backend(ops, backend_context))
		return 9;
	return 0;
}

static int test_interrupt_commit_rollback(
	const struct dos_exec_backend_ops *ops)
{
	static const uint8_t program[] = {0xf4u};
	struct dos_execution_event event;
	struct dos_cpu_state state;
	kernel_object_handle_t backend_context;
	size_t stack_linear;

	/* A failed intack with an exact stack restore and cancel is a contained
	 * session fault.  The pending PIC request remains retryable and the guest
	 * address space must not be quarantined. */
	reset_fixture(program, ARRAY_SIZE(program));
	state = initial_state();
	stack_linear = interrupt_frame_linear(&state);
	fill_interrupt_frame(stack_linear, 0x5au);
	fake_pending = true;
	fake_commit_status = X86_GUEST_SPACE_INTERRUPT_FAULT;
	if (!prepare_backend(ops, &state, &backend_context) ||
	    run_backend(ops, backend_context, &state, &event) !=
		    DOS_EXEC_BACKEND_EVENT ||
	    event.kind != (uint32_t)DOS_EXEC_EVENT_FAULT ||
	    event.value != (uint32_t)DOS_MACHINE_IO_FAULT ||
	    event.vector != TEST_IRQ_VECTOR || state.eip != 0u ||
	    state.esp != TEST_STACK_POINTER || write_calls != 2u ||
	    commit_calls != 1u || cancel_calls != 1u || quarantine_calls != 0u ||
	    !fake_pending || fake_claim_active || machine.poisoned != 0u ||
	    !interrupt_frame_is(stack_linear, 0x5au) ||
	    !release_backend(ops, backend_context))
		return 1;

	/* If the transactional restore cannot preserve even its pre-restore frame,
	 * cancel is still attempted and only this guest address space is poisoned. */
	reset_fixture(program, ARRAY_SIZE(program));
	state = initial_state();
	stack_linear = interrupt_frame_linear(&state);
	fill_interrupt_frame(stack_linear, 0x5au);
	fake_pending = true;
	fake_commit_status = X86_GUEST_SPACE_INTERRUPT_FAULT;
	fail_writes_from_call = 2u;
	if (!prepare_backend(ops, &state, &backend_context) ||
	    run_backend(ops, backend_context, &state, &event) !=
		    DOS_EXEC_BACKEND_EVENT ||
	    event.kind != (uint32_t)DOS_EXEC_EVENT_FAULT ||
	    event.value != (uint32_t)DOS_MACHINE_ROLLBACK_FAILED ||
	    event.vector != TEST_IRQ_VECTOR || write_calls != 3u ||
	    commit_calls != 1u || cancel_calls != 1u || quarantine_calls != 1u ||
	    !fake_pending || fake_claim_active || machine.poisoned != 1u ||
	    interrupt_frame_is(stack_linear, 0x5au) ||
	    !release_backend(ops, backend_context))
		return 2;

	/* A successful stack restore does not make an uncertain cancel safe. */
	reset_fixture(program, ARRAY_SIZE(program));
	state = initial_state();
	stack_linear = interrupt_frame_linear(&state);
	fill_interrupt_frame(stack_linear, 0x5au);
	fake_pending = true;
	fake_commit_status = X86_GUEST_SPACE_INTERRUPT_FAULT;
	fake_cancel_status = X86_GUEST_SPACE_STALE_BINDING;
	if (!prepare_backend(ops, &state, &backend_context) ||
	    run_backend(ops, backend_context, &state, &event) !=
		    DOS_EXEC_BACKEND_EVENT ||
	    event.kind != (uint32_t)DOS_EXEC_EVENT_FAULT ||
	    event.value != (uint32_t)DOS_MACHINE_ROLLBACK_FAILED ||
	    write_calls != 2u || commit_calls != 1u || cancel_calls != 1u ||
	    quarantine_calls != 1u || !fake_pending || !fake_claim_active ||
	    machine.poisoned != 1u ||
	    !interrupt_frame_is(stack_linear, 0x5au) ||
	    !release_backend(ops, backend_context))
		return 3;
	return 0;
}

static int run_shadow_case(const struct dos_exec_backend_ops *ops,
			   const uint8_t *program, size_t program_count,
			   enum test_injection_point point,
			   bool initial_interrupts_enabled,
			   uint16_t expected_return_ip)
{
	struct dos_execution_event event;
	struct dos_cpu_state state;
	kernel_object_handle_t backend_context;
	size_t stack_linear;

	reset_fixture(program, program_count);
	injection_point = point;
	state = initial_state_with_interrupts(initial_interrupts_enabled);
	if (!prepare_backend(ops, &state, &backend_context) ||
	    run_backend(ops, backend_context, &state, &event) !=
		    DOS_EXEC_BACKEND_EVENT ||
	    event.kind != (uint32_t)DOS_EXEC_EVENT_SOFTWARE_INTERRUPT ||
	    event.vector != 0x21u || !injection_fired || commit_calls != 1u ||
	    fake_pending || enter_calls != 1u)
		return 1;
	stack_linear = reflected_stack_linear(&state);
	if (read_le16(stack_linear) != expected_return_ip ||
	    read_le16(stack_linear + 2u) != TEST_PROGRAM_SEGMENT ||
	    (read_le16(stack_linear + 4u) & DOS_EFLAGS_TF) != 0u ||
	    !release_backend(ops, backend_context))
		return 2;
	return 0;
}

static int test_interrupt_shadow(const struct dos_exec_backend_ops *ops)
{
	static const uint8_t sti_hlt[] = {0xfbu, 0xf4u};
	static const uint8_t sti_ordinary[] = {0xfbu, 0x90u};
	static const uint8_t sti_sti[] = {0xfbu, 0xfbu};
	static const uint8_t sti_cli_hlt[] = {0xfbu, 0xfau, 0xf4u};
	struct dos_execution_event event;
	struct dos_cpu_state state;
	kernel_object_handle_t backend_context;
	int status;

	status = run_shadow_case(ops, sti_hlt, ARRAY_SIZE(sti_hlt),
				 TEST_INJECT_BEFORE_HLT, false, 2u);
	if (status != 0)
		return status;
	/* A redundant STI with virtual IF already set creates no new inhibition:
	 * an IRQ recognized before HLT must reflect at the STI retirement IP. */
	status = run_shadow_case(ops, sti_hlt, ARRAY_SIZE(sti_hlt),
				 TEST_INJECT_BEFORE_HLT, true, 1u);
	if (status != 0)
		return 2 + status;
	status = run_shadow_case(ops, sti_ordinary, ARRAY_SIZE(sti_ordinary),
				 TEST_INJECT_AFTER_ORDINARY, false, 2u);
	if (status != 0)
		return 4 + status;
	status = run_shadow_case(ops, sti_sti, ARRAY_SIZE(sti_sti),
				 TEST_INJECT_BEFORE_SECOND_STI, false, 2u);
	if (status != 0)
		return 6 + status;

	/* CLI is the instruction after STI: it retires the shadow but leaves virtual
	 * IF clear, so the pending maskable request cannot wake the following HLT. */
	reset_fixture(sti_cli_hlt, ARRAY_SIZE(sti_cli_hlt));
	injection_point = TEST_INJECT_BEFORE_CLI;
	state = initial_state_with_interrupts(false);
	if (!prepare_backend(ops, &state, &backend_context) ||
	    run_backend(ops, backend_context, &state, &event) !=
		    DOS_EXEC_BACKEND_EVENT ||
	    event.kind != (uint32_t)DOS_EXEC_EVENT_HALTED || state.eip != 3u ||
	    (state.eflags & DOS_EFLAGS_IF) != 0u || !fake_pending ||
	    commit_calls != 0u || !release_backend(ops, backend_context))
		return 9;
	return 0;
}

static int test_port_retirement_boundary(
	const struct dos_exec_backend_ops *ops)
{
	static const uint8_t program[] = {0xfbu, 0xe4u, 0x60u};
	struct dos_execution_event event;
	struct dos_cpu_state replacement;
	struct dos_cpu_state stale;
	struct dos_cpu_state state;
	kernel_object_handle_t backend_context;

	reset_fixture(program, ARRAY_SIZE(program));
	injection_point = TEST_INJECT_BEFORE_PORT;
	state = initial_state_with_interrupts(false);
	if (!prepare_backend(ops, &state, &backend_context) ||
	    run_backend(ops, backend_context, &state, &event) !=
		    DOS_EXEC_BACKEND_EVENT ||
	    event.kind != (uint32_t)DOS_EXEC_EVENT_PORT_IO || event.port != 0x60u ||
		    state.eip != 3u || !fake_pending || commit_calls != 0u ||
		    prepare_calls != 0u)
		return 1;
	/* No caller may run past an uncommitted I/O stop.  A stale service snapshot
	 * rejects without retiring the shadow; only the exact replacement commits. */
	if (run_backend(ops, backend_context, &state, &event) !=
	    DOS_EXEC_BACKEND_RUN_UNCERTAIN)
		return 2;
	replacement = state;
	replacement.eax = 0x12345678u;
	stale = state;
	++stale.eip;
	if (ops->commit_state_replacement(
		    TEST_ADAPTER_CONTEXT, backend_context, &stale, &replacement) !=
		    DOS_EXEC_BACKEND_STATE_REJECTED ||
	    ops->commit_state_replacement(
		    TEST_ADAPTER_CONTEXT, backend_context, &state, &replacement) !=
		    DOS_EXEC_BACKEND_STATE_COMMITTED)
		return 3;
	state = replacement;
	if (run_backend(ops, backend_context, &state, &event) !=
		    DOS_EXEC_BACKEND_EVENT ||
	    event.kind != (uint32_t)DOS_EXEC_EVENT_SOFTWARE_INTERRUPT ||
		    commit_calls != 1u || prepare_calls != 1u || fake_pending ||
	    state.eax != replacement.eax ||
	    !release_backend(ops, backend_context))
		return 4;
	return 0;
}

static int run_tests(void)
{
	const struct dos_exec_backend_ops *ops;
	int status;

	if (dos_machine_configure(&machine, &machine_ops, TEST_MACHINE_CONTEXT,
				  TEST_MEMORY_BYTES, true) != DOS_MACHINE_OK ||
	    x86_vm86_backend_initialize(TEST_ADAPTER_IDENTITY,
					TEST_ADAPTER_CONTEXT) != X86_VM86_BACKEND_OK)
		return 1;
	ops = x86_vm86_backend_ops();
	if (ops == NULL || ops->commit_state_replacement == NULL ||
	    x86_vm86_backend_context() != TEST_ADAPTER_CONTEXT)
		return 2;
	status = test_halt_wait_mask_and_retry(ops);
	if (status != 0)
		return 10 + status;
	status = test_interrupt_commit_rollback(ops);
	if (status != 0)
		return 30 + status;
	status = test_interrupt_shadow(ops);
	if (status != 0)
		return 50 + status;
	status = test_port_retirement_boundary(ops);
	if (status != 0)
		return 70 + status;
	return 0;
}

DOSC32_TEST_ENTRY(run_tests)

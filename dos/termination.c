// SPDX-License-Identifier: GPL-2.0-only
/*
 * DOS process termination
 *
 * Compatibility contract: restore saved vectors, free child memory, close JFT entries
 *                 from the last handle to the first, then restore the parent
 * Safety changes: full-range JFT preflight, bounded streaming, fixed native
 *                 storage, typed callbacks, and sticky teardown poison
 */
#include "dos_termination.h"

#include "dos_abi.h"
#include "dos_vectors.h"

#define TERMINATION_VECTOR_BYTES 12u
#define TERMINATION_JFT_CHUNK_BYTES 64u

static bool identity_is_valid(kernel_object_handle_t identity)
{
	return identity != 0u && identity != KERNEL_OBJECT_HANDLE_INVALID;
}

static bool machine_is_usable(const struct dos_machine *machine)
{
	return machine != NULL && machine->ops != NULL &&
	       machine->ops->read_memory != NULL &&
	       machine->ops->write_memory != NULL && machine->address_limit != 0u &&
	       machine->address_limit <= DOS_GUEST_32_ADDRESS_LIMIT &&
	       !machine->poisoned;
}

static uint16_t read_le16(const uint8_t *bytes)
{
	return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static struct dos_far_pointer16 read_far(const uint8_t *bytes)
{
	struct dos_far_pointer16 address = {
		.offset = read_le16(bytes),
		.segment = read_le16(bytes + 2u),
	};

	return address;
}

static void write_le16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
}

static void write_far(uint8_t *bytes, struct dos_far_pointer16 address)
{
	write_le16(bytes, address.offset);
	write_le16(bytes + 2u, address.segment);
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

/*
 * Validate the whole range before the first backend read, then prove every
 * byte currently readable without retaining a guest-sized native buffer.
 */
static enum dos_machine_status termination_jft_preflight(
	const struct dos_machine *machine, struct dos_far_pointer16 jft,
	uint16_t jft_length)
{
	uint8_t scratch[TERMINATION_JFT_CHUNK_BYTES];
	size_t completed = 0u;
	enum dos_machine_status status;

	status = dos_machine_validate_far(machine, jft.segment, jft.offset,
					  jft_length);
	if (status != DOS_MACHINE_OK)
		return status;
	while (completed < jft_length) {
		size_t remaining = (size_t)jft_length - completed;
		size_t count = remaining < sizeof(scratch) ? remaining
							    : sizeof(scratch);
		uint16_t offset = (uint16_t)((size_t)jft.offset + completed);

		status = dos_machine_read_far(machine, jft.segment, offset,
					      scratch, sizeof(scratch), count);
		if (status != DOS_MACHINE_OK)
			return status;
		completed += count;
	}
	return DOS_MACHINE_OK;
}

static bool plan_matches_machine(const struct dos_termination_plan *plan,
				 const struct dos_machine *machine,
				 kernel_object_handle_t machine_identity)
{
	return plan != NULL && plan->captured == 1u &&
	       plan->a20_enabled <= 1u &&
	       bytes_are_zero(plan->reserved, sizeof(plan->reserved)) &&
	       plan->machine_identity == machine_identity &&
	       plan->machine_context == machine->context &&
	       plan->machine_address_limit == machine->address_limit &&
	       plan->a20_enabled == (machine->a20_enabled ? 1u : 0u);
}

enum dos_termination_status dos_termination_capture(
	const struct dos_machine *machine,
	kernel_object_handle_t machine_identity, uint16_t process_psp,
	struct dos_termination_plan *plan)
{
	uint8_t prefix[sizeof(struct dos_psp_prefix40)];
	struct dos_termination_plan prepared = {0};
	struct dos_far_pointer16 jft;
	uint16_t jft_length;

	if (!machine_is_usable(machine) || !identity_is_valid(machine_identity) ||
	    plan == NULL)
		return DOS_TERMINATION_INVALID_ARGUMENT;
	if (dos_machine_read_far(machine, process_psp, 0u, prefix,
				 sizeof(prefix), sizeof(prefix)) != DOS_MACHINE_OK)
		return DOS_TERMINATION_MACHINE_FAULT;
	jft_length = read_le16(
		prefix + __builtin_offsetof(struct dos_psp_prefix40, jft_length));
	jft = read_far(prefix +
		       __builtin_offsetof(struct dos_psp_prefix40, jft_pointer));
	if (termination_jft_preflight(machine, jft, jft_length) !=
	    DOS_MACHINE_OK)
		return DOS_TERMINATION_MACHINE_FAULT;
	prepared.machine_identity = machine_identity;
	prepared.machine_context = machine->context;
	prepared.machine_address_limit = machine->address_limit;
	prepared.terminate_vector = read_far(
		prefix + __builtin_offsetof(struct dos_psp_prefix40, exit_vector));
	prepared.control_c_vector = read_far(
		prefix +
		__builtin_offsetof(struct dos_psp_prefix40, control_c_vector));
	prepared.critical_error_vector = read_far(
		prefix +
		__builtin_offsetof(struct dos_psp_prefix40, fatal_abort_vector));
	prepared.parent_user_stack = read_far(
		prefix + __builtin_offsetof(struct dos_psp_prefix40, user_stack));
	prepared.jft_pointer = jft;
	prepared.process_psp = process_psp;
	prepared.parent_psp = read_le16(
		prefix + __builtin_offsetof(struct dos_psp_prefix40, parent_psp));
	prepared.jft_length = jft_length;
	prepared.a20_enabled = machine->a20_enabled ? 1u : 0u;
	prepared.captured = 1u;
	*plan = prepared;
	return DOS_TERMINATION_OK;
}

enum dos_termination_status dos_termination_restore_vectors(
	const struct dos_machine *machine,
	kernel_object_handle_t machine_identity,
	const struct dos_termination_plan *plan)
{
	uint8_t vectors[TERMINATION_VECTOR_BYTES];
	uint8_t rollback[TERMINATION_VECTOR_BYTES];
	enum dos_machine_status status;

	if (!machine_is_usable(machine) || !identity_is_valid(machine_identity) ||
	    plan == NULL)
		return DOS_TERMINATION_INVALID_ARGUMENT;
	if (!plan_matches_machine(plan, machine, machine_identity))
		return DOS_TERMINATION_STALE_PLAN;
	write_far(vectors, plan->terminate_vector);
	write_far(vectors + 4u, plan->control_c_vector);
	write_far(vectors + 8u, plan->critical_error_vector);
	status = dos_machine_replace(
		machine, (dos_linear_address_t)DOS_INTERRUPT_TERMINATE * 4u,
		vectors, sizeof(vectors), rollback, sizeof(rollback),
		sizeof(vectors));
	if (status == DOS_MACHINE_OK)
		return DOS_TERMINATION_OK;
	return status == DOS_MACHINE_ROLLBACK_FAILED
		       ? DOS_TERMINATION_MACHINE_POISONED
		       : DOS_TERMINATION_MACHINE_FAULT;
}

enum dos_termination_status dos_termination_close_handles(
	const struct dos_machine *machine,
	kernel_object_handle_t machine_identity,
	const struct dos_termination_plan *plan,
	const struct dos_sft_batch_ops *ops,
	kernel_object_handle_t sft_context)
{
	uint8_t chunk[TERMINATION_JFT_CHUNK_BYTES];
	bool irreversible = false;
	bool failed = false;
	size_t remaining;

	if (!machine_is_usable(machine) || !identity_is_valid(machine_identity) ||
	    plan == NULL || ops == NULL || !identity_is_valid(ops->identity) ||
	    ops->lookup == NULL || ops->reference_release == NULL ||
	    ops->device_close == NULL ||
	    sft_context == KERNEL_OBJECT_HANDLE_INVALID)
		return DOS_TERMINATION_INVALID_ARGUMENT;
	if (!plan_matches_machine(plan, machine, machine_identity))
		return DOS_TERMINATION_STALE_PLAN;
	if (termination_jft_preflight(machine, plan->jft_pointer,
				      plan->jft_length) != DOS_MACHINE_OK)
		return DOS_TERMINATION_MACHINE_FAULT;

	remaining = plan->jft_length;
	while (remaining != 0u) {
		size_t count = remaining < sizeof(chunk) ? remaining
							  : sizeof(chunk);
		size_t start = remaining - count;
		uint16_t offset =
			(uint16_t)((size_t)plan->jft_pointer.offset + start);
		size_t index;

		if (dos_machine_read_far(machine, plan->jft_pointer.segment,
					 offset, chunk, sizeof(chunk), count) !=
		    DOS_MACHINE_OK)
			return failed || irreversible
				       ? DOS_TERMINATION_SFT_POISONED
				       : DOS_TERMINATION_MACHINE_FAULT;
		for (index = count; index != 0u; --index) {
			struct dos_sft_view view = {
				.reference_handle =
					DOS_SFT_REFERENCE_HANDLE_INVALID,
				.flags = 0u,
				.mode = 0u,
			};
			enum dos_sft_adapter_status lookup;
			uint8_t sfn = chunk[index - 1u];

			if (sfn == DOS_JFT_ENTRY_UNUSED)
				continue;
			lookup = ops->lookup(sft_context, sfn, &view);
			if (lookup == DOS_SFT_ADAPTER_INVALID_SFT)
				continue;
			if (lookup != DOS_SFT_ADAPTER_OK ||
			    view.reference_handle ==
				    DOS_SFT_REFERENCE_HANDLE_INVALID) {
				failed = true;
				continue;
			}
			/* DOS_CLOSE/Free_SFT decrements the SFT reference before
			 * the local-device close corresponding to EXEC's DEV_OPEN.
			 */
			if (ops->reference_release(
				    sft_context, view.reference_handle) !=
			    DOS_SFT_ADAPTER_OK) {
				failed = true;
				continue;
			}
			irreversible = true;
			if ((view.flags & DOS_SFT_FLAG_IS_NETWORK) == 0u &&
			    ops->device_close(sft_context,
					      view.reference_handle) !=
				DOS_SFT_ADAPTER_OK)
				failed = true;
		}
		remaining = start;
	}
	return failed ? DOS_TERMINATION_SFT_POISONED : DOS_TERMINATION_OK;
}

static void poison_termination_domain(
	const struct dos_termination_services *services)
{
	enum dos_process_runtime_status status;

	services->memory_arena->machine_poisoned = 1u;
	status = dos_process_runtime_poison(services->runtime);
	if (status != DOS_PROCESS_RUNTIME_OK &&
	    status != DOS_PROCESS_RUNTIME_POISONED)
		services->runtime->poisoned = 1u;
}

static bool services_are_usable(
	const struct dos_termination_services *services)
{
	return services != NULL && services->runtime != NULL &&
	       services->memory_arena != NULL &&
	       machine_is_usable(services->machine) &&
	       identity_is_valid(services->machine_identity) &&
	       services->sft_ops != NULL &&
	       identity_is_valid(services->sft_ops->identity) &&
	       services->sft_ops->lookup != NULL &&
	       services->sft_ops->reference_release != NULL &&
	       services->sft_ops->device_close != NULL &&
	       services->sft_context != KERNEL_OBJECT_HANDLE_INVALID;
}

enum dos_termination_status dos_termination_execute(
	const struct dos_termination_services *services,
	const struct dos_process_runtime_snapshot *parent_runtime,
	uint16_t process_psp, struct dos_termination_result *result)
{
	struct dos_termination_result prepared = {0};
	enum dos_process_runtime_status runtime_status;
	enum dos_memory_status memory_status;
	enum dos_termination_status status;

	if (!services_are_usable(services) || parent_runtime == NULL ||
	    result == NULL || parent_runtime->reserved != 0u ||
	    !identity_is_valid(parent_runtime->runtime_identity))
		return DOS_TERMINATION_INVALID_ARGUMENT;
	runtime_status = dos_process_runtime_snapshot(
		services->runtime, &prepared.child_runtime);
	prepared.runtime_status = (uint32_t)runtime_status;
	if (runtime_status != DOS_PROCESS_RUNTIME_OK)
		return DOS_TERMINATION_RUNTIME_FAULT;
	status = dos_termination_capture(
		services->machine, services->machine_identity, process_psp,
		&prepared.plan);
	if (status != DOS_TERMINATION_OK)
		return status;
	if (prepared.child_runtime.current_psp != process_psp ||
	    prepared.plan.process_psp != process_psp ||
	    prepared.plan.parent_psp != parent_runtime->current_psp ||
	    prepared.child_runtime.runtime_identity !=
		parent_runtime->runtime_identity ||
	    prepared.child_runtime.generation <= parent_runtime->generation)
		return DOS_TERMINATION_STALE_PLAN;

	status = dos_termination_restore_vectors(
		services->machine, services->machine_identity, &prepared.plan);
	prepared.vector_status = (uint32_t)status;
	if (status != DOS_TERMINATION_OK) {
		if (status == DOS_TERMINATION_MACHINE_POISONED) {
			poison_termination_domain(services);
			*result = prepared;
			return DOS_TERMINATION_POISONED;
		}
		*result = prepared;
		return status;
	}

	/* Termination frees all child-owned arena blocks before
	 * DOS_ABORT closes the child's inherited JFT references. */
	memory_status = dos_memory_free_process_checked(
		services->memory_arena, services->machine, process_psp);
	prepared.memory_status = (uint32_t)memory_status;
	if (memory_status != DOS_MEMORY_OK) {
		poison_termination_domain(services);
		*result = prepared;
		return memory_status == DOS_MEMORY_MACHINE_POISONED
			       ? DOS_TERMINATION_POISONED
			       : DOS_TERMINATION_MEMORY_FAULT;
	}

	status = dos_termination_close_handles(
		services->machine, services->machine_identity, &prepared.plan,
		services->sft_ops, services->sft_context);
	prepared.sft_status = (uint32_t)status;
	if (status != DOS_TERMINATION_OK) {
		poison_termination_domain(services);
		*result = prepared;
		return DOS_TERMINATION_POISONED;
	}

	runtime_status = dos_process_runtime_restore_parent(
		services->runtime, &prepared.child_runtime, parent_runtime);
	prepared.runtime_status = (uint32_t)runtime_status;
	if (runtime_status != DOS_PROCESS_RUNTIME_OK) {
		poison_termination_domain(services);
		*result = prepared;
		return DOS_TERMINATION_POISONED;
	}
	*result = prepared;
	return DOS_TERMINATION_OK;
}

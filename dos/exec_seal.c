// SPDX-License-Identifier: GPL-2.0-only
/* Cross-object final publication for a prepared DOS EXEC1 transaction. */
#include "dos_exec_seal.h"

static bool handle_is_valid(kernel_object_handle_t handle)
{
	return handle != 0u && handle != KERNEL_OBJECT_HANDLE_INVALID;
}

static bool reserved_bytes_are_zero(const uint8_t *bytes, size_t count)
{
	size_t index;

	for (index = 0u; index < count; ++index) {
		if (bytes[index] != 0u)
			return false;
	}
	return true;
}

static bool rebind_plan_is_zero(
    const struct dos_memory_lease_rebind_plan *plan)
{
	if (plan->handle.value != 0u || plan->machine_context != 0u ||
	    plan->arena_identity != 0u || plan->arena_generation != 0u ||
	    plan->value.header_segment != 0u ||
	    plan->value.expected_owner != 0u || plan->value.new_owner != 0u ||
	    plan->guest_segment != 0u || plan->paragraphs != 0u ||
	    plan->arena_head_segment != 0u)
		return false;
	if (!reserved_bytes_are_zero(plan->value.reserved,
				    ARRAY_SIZE(plan->value.reserved)) ||
	    !reserved_bytes_are_zero(plan->value.replacement_bytes,
				    ARRAY_SIZE(plan->value.replacement_bytes)) ||
	    !reserved_bytes_are_zero(plan->reserved, ARRAY_SIZE(plan->reserved)))
		return false;
	return true;
}

static bool
plan_has_valid_encoding(const struct dos_exec_load_only_seal_plan *plan)
{
	if (plan == NULL || plan->expected_parent.reserved != 0u ||
	    plan->expected_parent.generation == 0u ||
	    !handle_is_valid(plan->expected_parent.runtime_identity) ||
	    plan->has_environment > 1u ||
	    !reserved_bytes_are_zero(plan->reserved,
				     ARRAY_SIZE(plan->reserved)) ||
	    !handle_is_valid(plan->load_rebind.handle.value) ||
	    !handle_is_valid(plan->sft_batch))
		return false;
	if (plan->load_rebind.value.expected_owner !=
		plan->expected_parent.current_psp ||
	    plan->load_rebind.value.new_owner != plan->child_psp ||
	    plan->load_rebind.guest_segment != plan->child_psp)
		return false;
	if (plan->has_environment != 0u) {
		return handle_is_valid(plan->environment_rebind.handle.value) &&
		       plan->environment_rebind.handle.value !=
			   plan->load_rebind.handle.value &&
		       plan->environment_rebind.value.expected_owner ==
			   plan->expected_parent.current_psp &&
		       plan->environment_rebind.value.new_owner ==
			   plan->child_psp;
	}
	return rebind_plan_is_zero(&plan->environment_rebind);
}

static bool services_are_present(const struct dos_exec_seal_services *services)
{
	return services != NULL && services->observer != NULL &&
	       services->observer_ops != NULL && services->journal != NULL &&
	       services->memory_leases != NULL && services->arena != NULL &&
	       services->machine != NULL && services->runtime != NULL;
}

static bool memory_status_is_poison(enum dos_memory_lease_status status)
{
	return status == DOS_MEMORY_LEASE_MACHINE_POISONED;
}

enum dos_exec_seal_status dos_exec_seal_preflight_load_only(
    const struct dos_exec_seal_services *services,
    const struct dos_exec_load_only_seal_plan *plan)
{
	enum dos_exec_observer_status observer_status;
	enum dos_exec_journal_status journal_status;
	enum dos_memory_lease_status memory_status;
	enum dos_sft_batch_status sft_status;
	enum dos_process_runtime_status runtime_status;

	if (!services_are_present(services) || !plan_has_valid_encoding(plan))
		return DOS_EXEC_SEAL_INVALID_ARGUMENT;
	observer_status = dos_exec_observer_validate_held(
	    services->observer, services->observer_ops,
	    services->observer_context);
	if (observer_status != DOS_EXEC_OBSERVER_OK)
		return observer_status == DOS_EXEC_OBSERVER_POISONED
			   ? DOS_EXEC_SEAL_POISONED
			   : DOS_EXEC_SEAL_OBSERVER_NOT_READY;
	journal_status = dos_exec_journal_preflight_seal(
	    services->journal, services->machine_identity, services->machine);
	if (journal_status != DOS_EXEC_JOURNAL_OK)
		return journal_status == DOS_EXEC_JOURNAL_POISONED
			   ? DOS_EXEC_SEAL_POISONED
			   : DOS_EXEC_SEAL_JOURNAL_NOT_READY;
	if (plan->has_environment != 0u) {
		memory_status = dos_memory_lease_preflight_rebind_publish(
		    services->memory_leases, services->arena, services->machine,
		    &plan->environment_rebind);
		if (memory_status != DOS_MEMORY_LEASE_OK)
			return memory_status_is_poison(memory_status)
				   ? DOS_EXEC_SEAL_POISONED
				   : DOS_EXEC_SEAL_ENVIRONMENT_NOT_READY;
	}
	memory_status = dos_memory_lease_preflight_rebind_publish(
	    services->memory_leases, services->arena, services->machine,
	    &plan->load_rebind);
	if (memory_status != DOS_MEMORY_LEASE_OK)
		return memory_status_is_poison(memory_status)
			   ? DOS_EXEC_SEAL_POISONED
			   : DOS_EXEC_SEAL_LOAD_NOT_READY;
	sft_status = dos_sft_batch_preflight_commit(plan->sft_batch);
	if (sft_status != DOS_SFT_BATCH_OK)
		return sft_status == DOS_SFT_BATCH_POISONED
			   ? DOS_EXEC_SEAL_POISONED
			   : DOS_EXEC_SEAL_SFT_NOT_READY;
	runtime_status = dos_process_runtime_preflight_exec(
	    services->runtime, &plan->expected_parent);
	if (runtime_status != DOS_PROCESS_RUNTIME_OK)
		return runtime_status == DOS_PROCESS_RUNTIME_POISONED
			   ? DOS_EXEC_SEAL_POISONED
			   : DOS_EXEC_SEAL_RUNTIME_NOT_READY;
	return DOS_EXEC_SEAL_OK;
}

static void quarantine_services(const struct dos_exec_seal_services *services)
{
	enum dos_exec_journal_status journal_status;
	enum dos_memory_status memory_status;
	enum dos_process_runtime_status runtime_status;
	enum dos_exec_observer_status observer_status;

	journal_status = dos_exec_journal_poison(
	    services->journal, services->machine_identity, services->machine);
	memory_status = dos_memory_arena_poison(services->arena);
	runtime_status = dos_process_runtime_poison(services->runtime);
	observer_status =
	    dos_exec_observer_poison(services->observer, services->observer_ops,
				     services->observer_context);
	/* Every component is sticky even if its adapter cannot confirm the
	 * quarantine callback.  The outer coordinator still receives POISONED.
	 */
	(void)journal_status;
	(void)memory_status;
	(void)runtime_status;
	(void)observer_status;
}

enum dos_exec_seal_status
dos_exec_seal_commit_load_only(const struct dos_exec_seal_services *services,
			       const struct dos_exec_load_only_seal_plan *plan)
{
	enum dos_exec_seal_status status;

	status = dos_exec_seal_preflight_load_only(services, plan);
	if (status != DOS_EXEC_SEAL_OK)
		return status;

	if ((plan->has_environment != 0u &&
	     dos_memory_lease_rebind_publish(
		 services->memory_leases, services->arena, services->machine,
		 &plan->environment_rebind) != DOS_MEMORY_LEASE_OK) ||
	    dos_memory_lease_rebind_publish(
		services->memory_leases, services->arena, services->machine,
		&plan->load_rebind) != DOS_MEMORY_LEASE_OK ||
	    dos_sft_batch_commit(plan->sft_batch) != DOS_SFT_BATCH_OK ||
	    dos_exec_journal_seal(services->journal, services->machine_identity,
				  services->machine) != DOS_EXEC_JOURNAL_OK ||
	    dos_process_runtime_publish_exec(
		services->runtime, &plan->expected_parent, plan->child_psp) !=
		DOS_PROCESS_RUNTIME_OK) {
		quarantine_services(services);
		return DOS_EXEC_SEAL_POISONED;
	}
	if (dos_exec_observer_release(
		services->observer, services->observer_ops,
		services->observer_context) != DOS_EXEC_OBSERVER_OK) {
		quarantine_services(services);
		return DOS_EXEC_SEAL_POISONED;
	}
	return DOS_EXEC_SEAL_OK;
}

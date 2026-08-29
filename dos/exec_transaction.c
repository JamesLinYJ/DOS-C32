// SPDX-License-Identifier: GPL-2.0-only
/*
 * Common DOS EXEC prefix ownership coordinator
 *
 * Compatibility contract: validate AL, OPEN, IOCTL, environment selection, later CLOSE
 * Safety changes: early observation exclusion, fixed generation slots, reverse
 *                 abort and adapter-wide fail-closed poison
 */
#include "dos_exec_transaction.h"

#include "dos_exec_seal.h"
#include "dos_vectors.h"
#include "overflow.h"

static bool identity_is_valid(kernel_object_handle_t identity)
{
	return identity != 0u && identity != KERNEL_OBJECT_HANDLE_INVALID;
}

static bool memory_lease_table_identity_is_valid(
    dos_memory_lease_table_identity_t identity)
{
	return identity != 0u &&
	       identity != DOS_MEMORY_LEASE_TABLE_IDENTITY_INVALID;
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

static uint16_t transaction_read_le16(const uint8_t *bytes)
{
	return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static bool
request_has_valid_encoding(const struct dos_exec_transaction_request *request)
{
	return request != NULL &&
	       dos_exec_subfunction_is_valid(request->subfunction) &&
	       bytes_are_zero(request->reserved, ARRAY_SIZE(request->reserved));
}

static bool machine_is_usable(const struct dos_machine *machine)
{
	return machine != NULL && machine->ops != NULL &&
	       machine->ops->read_memory != NULL &&
	       machine->ops->write_memory != NULL &&
	       machine->context != KERNEL_OBJECT_HANDLE_INVALID &&
	       machine->address_limit != 0u &&
	       machine->address_limit <= DOS_GUEST_32_ADDRESS_LIMIT &&
	       (machine->a20_enabled == false || machine->a20_enabled == true);
}

static bool file_ops_are_complete(const struct dos_exec_file_lease_ops *ops)
{
	return ops != NULL && identity_is_valid(ops->identity) &&
	       ops->open != NULL && ops->probe_device != NULL &&
	       ops->read != NULL && ops->close != NULL;
}

static bool observer_ops_are_complete(const struct dos_exec_observer_ops *ops)
{
	return ops != NULL && identity_is_valid(ops->identity) &&
	       ops->acquire != NULL && ops->release != NULL &&
	       ops->quarantine != NULL;
}

static bool sft_ops_are_complete(const struct dos_sft_batch_ops *ops)
{
	return ops != NULL && identity_is_valid(ops->identity) &&
	       ops->lookup != NULL && ops->device_open != NULL &&
	       ops->reference_acquire != NULL &&
	       ops->reference_release != NULL && ops->device_close != NULL;
}

static bool drive_ops_are_complete(
    const struct dos_exec_drive_visibility_ops *ops)
{
	return ops != NULL && identity_is_valid(ops->identity) &&
	       ops->resolve != NULL;
}

static bool backend_services_are_absent(
    const struct dos_exec_transaction_services *services)
{
	return services->backend_sessions == NULL &&
	       services->backend_ops == NULL &&
	       (services->backend_session_table_identity == 0u ||
		services->backend_session_table_identity ==
		    KERNEL_OBJECT_HANDLE_INVALID) &&
	       (services->backend_adapter_context == 0u ||
		services->backend_adapter_context == KERNEL_OBJECT_HANDLE_INVALID);
}

static bool backend_services_are_complete(
    const struct dos_exec_transaction_services *services)
{
	const struct dos_exec_backend_ops *ops = services->backend_ops;
	const struct dos_exec_backend_session_table *sessions =
	    services->backend_sessions;

	return sessions != NULL && ops != NULL &&
	       identity_is_valid(services->backend_session_table_identity) &&
	       sessions->identity == services->backend_session_table_identity &&
	       sessions->initialized == 1u && sessions->constructed == 1u &&
	       sessions->poisoned <= 1u && identity_is_valid(ops->identity) &&
	       ops->capabilities != 0u &&
	       (ops->capabilities & ~DOS_EXEC_CAPABILITY_MASK) == 0u &&
	       ops->prepare != NULL && ops->release != NULL &&
	       ops->run_until_event != NULL &&
	       services->backend_adapter_context !=
		   KERNEL_OBJECT_HANDLE_INVALID;
}

static bool memory_services_are_complete(
    const struct dos_exec_transaction_services *services)
{
	const struct dos_memory_lease_table *leases = services->memory_leases;
	const struct dos_memory_arena *arena = services->memory_arena;

	return leases != NULL && arena != NULL &&
	       memory_lease_table_identity_is_valid(
		   services->memory_lease_table_identity) &&
	       leases->lifetime_identity ==
		   services->memory_lease_table_identity &&
	       leases->initialized == 1u && leases->constructed == 1u &&
	       bytes_are_zero(leases->reserved, ARRAY_SIZE(leases->reserved)) &&
	       identity_is_valid(arena->identity) && arena->generation != 0u &&
	       arena->initialized == 1u && arena->machine_poisoned <= 1u &&
	       arena->constructed == 1u &&
	       bytes_are_zero(arena->reserved, ARRAY_SIZE(arena->reserved));
}

static bool state_is_implemented(uint8_t state)
{
	switch (state) {
	case DOS_EXEC_TRANSACTION_STATE_VACANT:
	case DOS_EXEC_TRANSACTION_STATE_OBSERVED:
	case DOS_EXEC_TRANSACTION_STATE_FILE_OPEN:
	case DOS_EXEC_TRANSACTION_STATE_FILE_PROBED:
	case DOS_EXEC_TRANSACTION_STATE_ENV_READY:
	case DOS_EXEC_TRANSACTION_STATE_IMAGE_READY:
	case DOS_EXEC_TRANSACTION_STATE_TARGET_READY:
	case DOS_EXEC_TRANSACTION_STATE_IMAGE_PRIVATE:
	case DOS_EXEC_TRANSACTION_STATE_SFT_RESERVED:
	case DOS_EXEC_TRANSACTION_STATE_GLOBAL_STAGED:
	case DOS_EXEC_TRANSACTION_STATE_FILE_CLOSED:
	case DOS_EXEC_TRANSACTION_STATE_COMMITTING:
	case DOS_EXEC_TRANSACTION_STATE_PUBLISHED:
	case DOS_EXEC_TRANSACTION_STATE_FAILED:
	case DOS_EXEC_TRANSACTION_STATE_ABORTING:
	case DOS_EXEC_TRANSACTION_STATE_ABORTED:
	case DOS_EXEC_TRANSACTION_STATE_POISONED:
	case DOS_EXEC_TRANSACTION_STATE_OBSERVER_ACQUIRING:
	case DOS_EXEC_TRANSACTION_STATE_FILE_OPENING:
	case DOS_EXEC_TRANSACTION_STATE_FILE_PROBING:
	case DOS_EXEC_TRANSACTION_STATE_FILE_CLOSING:
	case DOS_EXEC_TRANSACTION_STATE_ABORT_FILE_CLOSING:
	case DOS_EXEC_TRANSACTION_STATE_OBSERVER_RELEASING:
	case DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_READING:
	case DOS_EXEC_TRANSACTION_STATE_NAME_READING:
	case DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_SCANNING:
	case DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_PLANNED:
	case DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_ALLOCATING:
	case DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_LEASED:
	case DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_BUILDING:
	case DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_BLOCK_READY:
	case DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_LEASE_ABORTING:
	case DOS_EXEC_TRANSACTION_STATE_IMAGE_READING:
	case DOS_EXEC_TRANSACTION_STATE_LOAD_QUERYING:
	case DOS_EXEC_TRANSACTION_STATE_LOAD_PLANNED:
	case DOS_EXEC_TRANSACTION_STATE_LOAD_ALLOCATING:
	case DOS_EXEC_TRANSACTION_STATE_LOAD_LEASE_ABORTING:
	case DOS_EXEC_TRANSACTION_STATE_LOAD_LEASED:
	case DOS_EXEC_TRANSACTION_STATE_PROCESS_PLANNED:
	case DOS_EXEC_TRANSACTION_STATE_RESIDENT_LOADING:
	case DOS_EXEC_TRANSACTION_STATE_RESIDENT_READY:
	case DOS_EXEC_TRANSACTION_STATE_RELOCATION_PLANNED:
	case DOS_EXEC_TRANSACTION_STATE_RELOCATING:
	case DOS_EXEC_TRANSACTION_STATE_PROCESS_FILE_CLOSING:
	case DOS_EXEC_TRANSACTION_STATE_PROCESS_FILE_CLOSED:
	case DOS_EXEC_TRANSACTION_STATE_PARENT_SNAPSHOTTING:
	case DOS_EXEC_TRANSACTION_STATE_PARENT_SNAPSHOT_READY:
	case DOS_EXEC_TRANSACTION_STATE_SFT_PREPARING:
	case DOS_EXEC_TRANSACTION_STATE_SFT_ABORTING:
	case DOS_EXEC_TRANSACTION_STATE_PROCESS_PARAMETER_READING:
	case DOS_EXEC_TRANSACTION_STATE_PSP_PREPARING:
	case DOS_EXEC_TRANSACTION_STATE_PSP_PREPARED:
	case DOS_EXEC_TRANSACTION_STATE_DRIVE_RESOLVING:
	case DOS_EXEC_TRANSACTION_STATE_INITIAL_STATE_READY:
	case DOS_EXEC_TRANSACTION_STATE_PROCESS_MEMORY_STAGING:
	case DOS_EXEC_TRANSACTION_STATE_JOURNAL_ABORTING:
	case DOS_EXEC_TRANSACTION_STATE_GLOBAL_MEMORY_STAGING:
	case DOS_EXEC_TRANSACTION_STATE_GLOBAL_MEMORY_READY:
	case DOS_EXEC_TRANSACTION_STATE_HANDOFF_STAGING:
	case DOS_EXEC_TRANSACTION_STATE_HANDOFF_READY:
	case DOS_EXEC_TRANSACTION_STATE_BACKEND_PREPARING:
	case DOS_EXEC_TRANSACTION_STATE_BACKEND_DORMANT:
	case DOS_EXEC_TRANSACTION_STATE_BACKEND_RELEASING:
		return true;
	default:
		return false;
	}
}

static bool observer_reserved_is_zero(const struct dos_exec_observer *observer)
{
	return bytes_are_zero(observer->reserved,
			      ARRAY_SIZE(observer->reserved));
}

static bool
request_is_process(const struct dos_exec_transaction_request *request)
{
	return request->subfunction != DOS_EXEC_OVERLAY;
}

static bool
snapshot_is_unused(const struct dos_process_runtime_snapshot *snapshot)
{
	return snapshot->generation == 0u && snapshot->runtime_identity == 0u &&
	       snapshot->dta.offset == 0u && snapshot->dta.segment == 0u &&
	       snapshot->current_psp == 0u && snapshot->reserved == 0u;
}

static bool environment_source_is_unused(
    const struct dos_exec_environment_source_plan *source)
{
	return source->source.offset == 0u && source->source.segment == 0u &&
	       source->parent_psp == 0u && source->subfunction == 0u &&
	       source->kind == 0u;
}

static bool executable_name_is_unused(const struct dos_exec_name_plan *name)
{
	return name->source.offset == 0u && name->source.segment == 0u &&
	       name->bytes_including_nul == 0u && name->reserved == 0u;
}

static bool owner_name_patch_is_zero(
    const struct dos_memory_owner_name_patch *patch)
{
	return bytes_are_zero(patch->bytes, ARRAY_SIZE(patch->bytes)) &&
	       patch->count == 0u &&
	       bytes_are_zero(patch->reserved, ARRAY_SIZE(patch->reserved));
}

static bool owner_name_patch_has_valid_encoding(
    const struct dos_memory_owner_name_patch *patch)
{
	size_t index;

	if (patch->count == 0u || patch->count > ARRAY_SIZE(patch->bytes) ||
	    !bytes_are_zero(patch->reserved, ARRAY_SIZE(patch->reserved)))
		return false;
	for (index = patch->count; index < ARRAY_SIZE(patch->bytes); ++index) {
		if (patch->bytes[index] != 0u)
			return false;
	}
	return true;
}

static void copy_owner_name_patch(
    struct dos_memory_owner_name_patch *destination,
    const struct dos_memory_owner_name_patch *source)
{
	size_t index;

	for (index = 0u; index < ARRAY_SIZE(destination->bytes); ++index)
		destination->bytes[index] = source->bytes[index];
	destination->count = source->count;
	for (index = 0u; index < ARRAY_SIZE(destination->reserved); ++index)
		destination->reserved[index] = source->reserved[index];
}

static bool rebind_plan_is_zero(
    const struct dos_memory_lease_rebind_plan *plan)
{
	return plan->handle.value == 0u && plan->machine_context == 0u &&
	       plan->arena_identity == 0u && plan->arena_generation == 0u &&
	       plan->value.header_segment == 0u &&
	       plan->value.expected_owner == 0u && plan->value.new_owner == 0u &&
	       bytes_are_zero(plan->value.reserved,
			      ARRAY_SIZE(plan->value.reserved)) &&
	       bytes_are_zero(plan->value.replacement_bytes,
			      ARRAY_SIZE(plan->value.replacement_bytes)) &&
	       plan->guest_segment == 0u && plan->paragraphs == 0u &&
	       plan->arena_head_segment == 0u &&
	       bytes_are_zero(plan->reserved, ARRAY_SIZE(plan->reserved));
}

static void clear_backend_binding(
    struct dos_exec_transaction_publication *publication)
{
	publication->backend_session.value = 0u;
	publication->backend_session_table_identity =
	    KERNEL_OBJECT_HANDLE_INVALID;
	publication->backend_adapter_identity = KERNEL_OBJECT_HANDLE_INVALID;
	publication->backend_adapter_context = KERNEL_OBJECT_HANDLE_INVALID;
	publication->has_backend_session = 0u;
}

static void clear_publication_rebinds(
    struct dos_exec_transaction_publication *publication)
{
	publication->environment_rebind =
	    (struct dos_memory_lease_rebind_plan){0};
	publication->load_rebind = (struct dos_memory_lease_rebind_plan){0};
	publication->handoff = (struct dos_exec_handoff_plan){0};
	publication->has_environment_rebind = 0u;
	publication->has_load_rebind = 0u;
	publication->has_handoff = 0u;
	clear_backend_binding(publication);
}

static void clear_transaction_publication(
    struct dos_exec_transaction_publication *publication)
{
	size_t index;

	for (index = 0u; index < ARRAY_SIZE(publication->owner_name.bytes);
	     ++index)
		publication->owner_name.bytes[index] = 0u;
	publication->owner_name.count = 0u;
	for (index = 0u; index < ARRAY_SIZE(publication->owner_name.reserved);
	     ++index)
		publication->owner_name.reserved[index] = 0u;
	clear_publication_rebinds(publication);
	publication->has_owner_name = 0u;
	for (index = 0u; index < ARRAY_SIZE(publication->reserved); ++index)
		publication->reserved[index] = 0u;
}

static bool state_is_process_memory_interval(uint8_t state)
{
	return state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_PROCESS_MEMORY_STAGING ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_GLOBAL_STAGED ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_GLOBAL_MEMORY_STAGING ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_GLOBAL_MEMORY_READY ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_HANDOFF_STAGING ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_HANDOFF_READY ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_BACKEND_PREPARING ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_BACKEND_DORMANT ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_BACKEND_RELEASING ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_COMMITTING ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_PUBLISHED ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_JOURNAL_ABORTING;
}

static bool state_is_load_block_interval(uint8_t state)
{
	return state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_LOAD_QUERYING ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_LOAD_PLANNED ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_LOAD_ALLOCATING ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_LOAD_LEASED ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_TARGET_READY ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_PROCESS_PLANNED ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_RESIDENT_LOADING ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_RESIDENT_READY ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_RELOCATION_PLANNED ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_RELOCATING ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_IMAGE_PRIVATE ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_PROCESS_FILE_CLOSING ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_PROCESS_FILE_CLOSED ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_PARENT_SNAPSHOTTING ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_PARENT_SNAPSHOT_READY ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_SFT_PREPARING ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_SFT_RESERVED ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_SFT_ABORTING ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_PROCESS_PARAMETER_READING ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_PSP_PREPARING ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_PSP_PREPARED ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_DRIVE_RESOLVING ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_INITIAL_STATE_READY ||
	       state_is_process_memory_interval(state) ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_LOAD_LEASE_ABORTING;
}

static void copy_executable_name(struct dos_exec_name_plan *destination,
				 const struct dos_exec_name_plan *source)
{
	destination->source.offset = source->source.offset;
	destination->source.segment = source->source.segment;
	destination->bytes_including_nul = source->bytes_including_nul;
	destination->reserved = source->reserved;
}

static bool state_retains_name_with_file_lease(uint8_t state)
{
	return state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_FILE_OPEN ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_FILE_PROBING ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_FILE_PROBED ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_READING ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_ENV_READY ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_SCANNING ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_PLANNED ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_ALLOCATING ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_LEASED ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_BUILDING ||
	       state == (uint8_t)
			    DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_BLOCK_READY ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_IMAGE_READING ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_IMAGE_READY ||
	       state_is_load_block_interval(state) ||
	       state == (uint8_t)
			    DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_LEASE_ABORTING ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_FILE_CLOSING ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_FILE_CLOSED ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_FAILED ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_ABORTING ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_ABORT_FILE_CLOSING ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_POISONED;
}

static bool slot_executable_name_has_valid_encoding(
    const struct dos_exec_transaction_slot *slot)
{
	bool unused = executable_name_is_unused(&slot->executable_name);

	if (unused)
		return slot->has_file_lease == 0u;
	if (!dos_exec_name_plan_has_valid_encoding(&slot->executable_name) ||
	    slot->executable_name.source.offset !=
		slot->request.executable_name.offset ||
	    slot->executable_name.source.segment !=
		slot->request.executable_name.segment)
		return false;
	if (slot->has_file_lease != 0u)
		return state_retains_name_with_file_lease(slot->state);
	return slot->state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_OBSERVER_RELEASING ||
	       slot->state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_ABORTED ||
	       slot->state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_POISONED;
}

static void copy_environment_source(
    struct dos_exec_environment_source_plan *destination,
    const struct dos_exec_environment_source_plan *source)
{
	destination->source.offset = source->source.offset;
	destination->source.segment = source->source.segment;
	destination->parent_psp = source->parent_psp;
	destination->subfunction = source->subfunction;
	destination->kind = source->kind;
}

static bool state_may_retain_environment_source(uint8_t state)
{
	return state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_ENV_READY ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_SCANNING ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_PLANNED ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_ALLOCATING ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_LEASED ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_BUILDING ||
	       state == (uint8_t)
			    DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_BLOCK_READY ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_IMAGE_READING ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_IMAGE_READY ||
	       state_is_load_block_interval(state) ||
	       state == (uint8_t)
			    DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_LEASE_ABORTING ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_FAILED ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_ABORTING ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_ABORT_FILE_CLOSING ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_OBSERVER_RELEASING ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_ABORTED ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_POISONED;
}

static bool state_requires_selected_environment(uint8_t state)
{
	return state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_ENV_READY ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_SCANNING ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_PLANNED ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_ALLOCATING ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_LEASED ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_BUILDING ||
	       state == (uint8_t)
			    DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_BLOCK_READY ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_IMAGE_READING ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_IMAGE_READY ||
	       state_is_load_block_interval(state) ||
	       state == (uint8_t)
			    DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_LEASE_ABORTING;
}

static bool environment_source_matches_parent(
    const struct dos_exec_environment_source_plan *source,
    const struct dos_process_runtime_snapshot *parent)
{
	if (source->kind == DOS_EXEC_ENVIRONMENT_SOURCE_PARAMETER)
		return source->parent_psp == 0u;
	if (source->kind == DOS_EXEC_ENVIRONMENT_SOURCE_PARENT ||
	    source->kind == DOS_EXEC_ENVIRONMENT_SOURCE_NONE)
		return source->parent_psp == parent->current_psp;
	return false;
}

static bool slot_environment_source_has_valid_encoding(
    const struct dos_exec_transaction_slot *slot)
{
	bool unused = environment_source_is_unused(&slot->environment_source);

	if (!request_is_process(&slot->request))
		return unused;
	/* AL=0 plus parent PSP zero plus no environment is a legitimate
	 * all-zero plan.  ENV_READY, rather than a sentinel field, proves that
	 * the decoder published it. */
	if (state_requires_selected_environment(slot->state))
		return dos_exec_environment_source_plan_has_valid_encoding(
			   &slot->environment_source) &&
		       slot->environment_source.subfunction ==
			   slot->request.subfunction &&
		       environment_source_matches_parent(
			   &slot->environment_source, &slot->parent_runtime);
	if (unused)
		return true;
	return state_may_retain_environment_source(slot->state) &&
	       dos_exec_environment_source_plan_has_valid_encoding(
		   &slot->environment_source) &&
	       slot->environment_source.subfunction == slot->request.subfunction &&
	       environment_source_matches_parent(&slot->environment_source,
						 &slot->parent_runtime);
}

static bool environment_plan_is_zero(const struct dos_environment_plan *plan)
{
	return plan->source.offset == 0u && plan->source.segment == 0u &&
	       plan->environment_bytes == 0u &&
	       executable_name_is_unused(&plan->executable_name) &&
	       plan->payload_bytes == 0u && plan->allocation_bytes == 0u &&
	       plan->paragraphs == 0u && plan->reserved == 0u;
}

static bool memory_lease_receipt_is_zero(
    const struct dos_memory_lease_receipt *receipt)
{
	return receipt->handle.value == 0u && receipt->guest_segment == 0u &&
	       receipt->paragraphs == 0u && receipt->maximum_available == 0u &&
	       receipt->reserved == 0u;
}

static bool transaction_environment_is_zero(
    const struct dos_exec_transaction_environment *environment)
{
	return environment_plan_is_zero(&environment->plan) &&
	       memory_lease_receipt_is_zero(&environment->lease) &&
	       environment->has_block == 0u &&
	       bytes_are_zero(environment->reserved,
			      ARRAY_SIZE(environment->reserved));
}

static void clear_environment_plan(struct dos_environment_plan *plan)
{
	plan->source.offset = 0u;
	plan->source.segment = 0u;
	plan->environment_bytes = 0u;
	plan->executable_name.source.offset = 0u;
	plan->executable_name.source.segment = 0u;
	plan->executable_name.bytes_including_nul = 0u;
	plan->executable_name.reserved = 0u;
	plan->payload_bytes = 0u;
	plan->allocation_bytes = 0u;
	plan->paragraphs = 0u;
	plan->reserved = 0u;
}

static void clear_environment_lease(
    struct dos_exec_transaction_environment *environment)
{
	environment->lease.handle.value = 0u;
	environment->lease.guest_segment = 0u;
	environment->lease.paragraphs = 0u;
	environment->lease.maximum_available = 0u;
	environment->lease.reserved = 0u;
	environment->has_block = 0u;
}

static void clear_transaction_environment(
    struct dos_exec_transaction_environment *environment)
{
	size_t index;

	clear_environment_plan(&environment->plan);
	clear_environment_lease(environment);
	for (index = 0u; index < ARRAY_SIZE(environment->reserved); ++index)
		environment->reserved[index] = 0u;
}

static void copy_environment_plan(struct dos_environment_plan *destination,
				  const struct dos_environment_plan *source)
{
	destination->source.offset = source->source.offset;
	destination->source.segment = source->source.segment;
	destination->environment_bytes = source->environment_bytes;
	copy_executable_name(&destination->executable_name,
			     &source->executable_name);
	destination->payload_bytes = source->payload_bytes;
	destination->allocation_bytes = source->allocation_bytes;
	destination->paragraphs = source->paragraphs;
	destination->reserved = source->reserved;
}

static void copy_environment_receipt(
    struct dos_memory_lease_receipt *destination,
    const struct dos_memory_lease_receipt *source)
{
	destination->handle = source->handle;
	destination->guest_segment = source->guest_segment;
	destination->paragraphs = source->paragraphs;
	destination->maximum_available = source->maximum_available;
	destination->reserved = source->reserved;
}

static void copy_transaction_environment(
    struct dos_exec_transaction_environment *destination,
    const struct dos_exec_transaction_environment *source)
{
	size_t index;

	copy_environment_plan(&destination->plan, &source->plan);
	copy_environment_receipt(&destination->lease, &source->lease);
	destination->has_block = source->has_block;
	for (index = 0u; index < ARRAY_SIZE(destination->reserved); ++index)
		destination->reserved[index] = source->reserved[index];
}

static bool load_plan_is_zero(const struct dos_load_plan *plan)
{
	return plan->format == 0u && plan->old_mz_signature == 0u &&
	       plan->load_high == 0u && plan->target_kind == 0u &&
	       plan->reserved32 == 0u && plan->file_size == 0u &&
	       plan->image_file_offset == 0u && plan->image_size == 0u &&
	       plan->minimum_image_paragraphs == 0u &&
	       plan->minimum_extra_paragraphs == 0u &&
	       plan->maximum_extra_paragraphs == 0u && plan->initial_cs == 0u &&
	       plan->initial_ip == 0u && plan->initial_ss == 0u &&
	       plan->initial_sp == 0u && plan->relocation_count == 0u &&
	       plan->relocation_table_offset == 0u;
}

static void clear_load_plan(struct dos_load_plan *plan)
{
	plan->format = 0u;
	plan->old_mz_signature = 0u;
	plan->load_high = 0u;
	plan->target_kind = 0u;
	plan->reserved32 = 0u;
	plan->file_size = 0u;
	plan->image_file_offset = 0u;
	plan->image_size = 0u;
	plan->minimum_image_paragraphs = 0u;
	plan->minimum_extra_paragraphs = 0u;
	plan->maximum_extra_paragraphs = 0u;
	plan->initial_cs = 0u;
	plan->initial_ip = 0u;
	plan->initial_ss = 0u;
	plan->initial_sp = 0u;
	plan->relocation_count = 0u;
	plan->relocation_table_offset = 0u;
}

static void copy_load_plan(struct dos_load_plan *destination,
			   const struct dos_load_plan *source)
{
	destination->format = source->format;
	destination->old_mz_signature = source->old_mz_signature;
	destination->load_high = source->load_high;
	destination->target_kind = source->target_kind;
	destination->reserved32 = source->reserved32;
	destination->file_size = source->file_size;
	destination->image_file_offset = source->image_file_offset;
	destination->image_size = source->image_size;
	destination->minimum_image_paragraphs =
	    source->minimum_image_paragraphs;
	destination->minimum_extra_paragraphs =
	    source->minimum_extra_paragraphs;
	destination->maximum_extra_paragraphs =
	    source->maximum_extra_paragraphs;
	destination->initial_cs = source->initial_cs;
	destination->initial_ip = source->initial_ip;
	destination->initial_ss = source->initial_ss;
	destination->initial_sp = source->initial_sp;
	destination->relocation_count = source->relocation_count;
	destination->relocation_table_offset = source->relocation_table_offset;
}

static bool transaction_image_is_zero(
    const struct dos_exec_transaction_image *image)
{
	return load_plan_is_zero(&image->plan) && image->has_plan == 0u &&
	       bytes_are_zero(image->reserved, ARRAY_SIZE(image->reserved));
}

static void clear_transaction_image(struct dos_exec_transaction_image *image)
{
	size_t index;

	clear_load_plan(&image->plan);
	image->has_plan = 0u;
	for (index = 0u; index < ARRAY_SIZE(image->reserved); ++index)
		image->reserved[index] = 0u;
}

static bool state_may_retain_image_plan(uint8_t state)
{
	return state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_IMAGE_READY ||
	       state_is_load_block_interval(state) ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_FAILED ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_ABORTING ||
	       state == (uint8_t)
			    DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_LEASE_ABORTING ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_ABORT_FILE_CLOSING ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_OBSERVER_RELEASING ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_ABORTED ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_POISONED;
}

static bool slot_image_has_valid_encoding(
    const struct dos_exec_transaction_slot *slot)
{
	const struct dos_exec_transaction_image *image = &slot->image;
	uint8_t expected_target =
	    request_is_process(&slot->request)
		? (uint8_t)DOS_LOAD_TARGET_PROCESS
		: (uint8_t)DOS_LOAD_TARGET_OVERLAY;

	if (image->has_plan == 0u) {
		return transaction_image_is_zero(image) &&
		       slot->state !=
			   (uint8_t)DOS_EXEC_TRANSACTION_STATE_IMAGE_READY &&
		       !state_is_load_block_interval(slot->state);
	}
	return image->has_plan == 1u &&
	       bytes_are_zero(image->reserved, ARRAY_SIZE(image->reserved)) &&
	       state_may_retain_image_plan(slot->state) &&
	       dos_load_plan_has_inspected_encoding(&image->plan) &&
	       image->plan.target_kind == expected_target;
}

static bool allocation_plan_is_zero(
    const struct dos_process_allocation_plan *allocation)
{
	return allocation->format == 0u && allocation->load_high == 0u &&
	       allocation->reserved == 0u &&
	       allocation->available_paragraphs == 0u &&
	       allocation->block_paragraphs == 0u;
}

static void clear_allocation_plan(
    struct dos_process_allocation_plan *allocation)
{
	allocation->format = 0u;
	allocation->load_high = 0u;
	allocation->reserved = 0u;
	allocation->available_paragraphs = 0u;
	allocation->block_paragraphs = 0u;
}

static void copy_allocation_plan(
    struct dos_process_allocation_plan *destination,
    const struct dos_process_allocation_plan *source)
{
	destination->format = source->format;
	destination->load_high = source->load_high;
	destination->reserved = source->reserved;
	destination->available_paragraphs = source->available_paragraphs;
	destination->block_paragraphs = source->block_paragraphs;
}

static bool transaction_target_is_zero(
    const struct dos_exec_transaction_target *target)
{
	return allocation_plan_is_zero(&target->allocation) &&
	       memory_lease_receipt_is_zero(&target->lease) &&
	       target->has_load_block == 0u &&
	       bytes_are_zero(target->reserved, ARRAY_SIZE(target->reserved));
}

static void clear_target_lease(struct dos_exec_transaction_target *target)
{
	target->lease.handle.value = 0u;
	target->lease.guest_segment = 0u;
	target->lease.paragraphs = 0u;
	target->lease.maximum_available = 0u;
	target->lease.reserved = 0u;
	target->has_load_block = 0u;
}

static void clear_transaction_target(
    struct dos_exec_transaction_target *target)
{
	size_t index;

	clear_allocation_plan(&target->allocation);
	clear_target_lease(target);
	for (index = 0u; index < ARRAY_SIZE(target->reserved); ++index)
		target->reserved[index] = 0u;
}

static void copy_transaction_target(
    struct dos_exec_transaction_target *destination,
    const struct dos_exec_transaction_target *source)
{
	size_t index;

	copy_allocation_plan(&destination->allocation, &source->allocation);
	copy_environment_receipt(&destination->lease, &source->lease);
	destination->has_load_block = source->has_load_block;
	for (index = 0u; index < ARRAY_SIZE(destination->reserved); ++index)
		destination->reserved[index] = source->reserved[index];
}

static bool allocation_plan_matches_image(
    const struct dos_process_allocation_plan *allocation,
    const struct dos_load_plan *image)
{
	struct dos_process_allocation_plan expected;

	if (!dos_process_allocation_plan_has_valid_encoding(allocation) ||
	    allocation->available_paragraphs == 0u ||
	    allocation->block_paragraphs == 0u ||
	    dos_process_select_allocation(image,
				  allocation->available_paragraphs,
				  &expected) != DOS_PROCESS_OK)
		return false;
	return expected.format == allocation->format &&
	       expected.load_high == allocation->load_high &&
	       expected.reserved == allocation->reserved &&
	       expected.available_paragraphs ==
		   allocation->available_paragraphs &&
	       expected.block_paragraphs == allocation->block_paragraphs;
}

static bool state_may_retain_target_plan(uint8_t state)
{
	return state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_LOAD_PLANNED ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_LOAD_ALLOCATING ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_LOAD_LEASED ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_TARGET_READY ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_PROCESS_PLANNED ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_RESIDENT_LOADING ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_RESIDENT_READY ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_RELOCATION_PLANNED ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_RELOCATING ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_IMAGE_PRIVATE ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_PROCESS_FILE_CLOSING ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_PROCESS_FILE_CLOSED ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_PARENT_SNAPSHOTTING ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_PARENT_SNAPSHOT_READY ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_SFT_PREPARING ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_SFT_RESERVED ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_SFT_ABORTING ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_PROCESS_PARAMETER_READING ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_PSP_PREPARING ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_PSP_PREPARED ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_DRIVE_RESOLVING ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_INITIAL_STATE_READY ||
	       state_is_process_memory_interval(state) ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_LOAD_LEASE_ABORTING ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_FAILED ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_ABORTING ||
	       state == (uint8_t)
			    DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_LEASE_ABORTING ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_ABORT_FILE_CLOSING ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_OBSERVER_RELEASING ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_ABORTED ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_POISONED;
}

static bool slot_target_has_valid_encoding(
    const struct dos_exec_transaction_slot *slot)
{
	const struct dos_exec_transaction_target *target = &slot->target;
	bool zero = transaction_target_is_zero(target);

	if (!request_is_process(&slot->request))
		return zero;
	if (zero)
		return slot->state !=
			   (uint8_t)DOS_EXEC_TRANSACTION_STATE_LOAD_PLANNED &&
		       slot->state !=
			   (uint8_t)DOS_EXEC_TRANSACTION_STATE_LOAD_ALLOCATING &&
		       slot->state !=
			   (uint8_t)DOS_EXEC_TRANSACTION_STATE_LOAD_LEASED &&
		       slot->state !=
			   (uint8_t)DOS_EXEC_TRANSACTION_STATE_TARGET_READY &&
		       slot->state != (uint8_t)
			   DOS_EXEC_TRANSACTION_STATE_LOAD_LEASE_ABORTING;
	if (!bytes_are_zero(target->reserved, ARRAY_SIZE(target->reserved)) ||
	    !state_may_retain_target_plan(slot->state) ||
	    slot->image.has_plan != 1u ||
	    !allocation_plan_matches_image(&target->allocation,
					   &slot->image.plan))
		return false;
	if (target->has_load_block == 0u)
		return memory_lease_receipt_is_zero(&target->lease) &&
		       slot->state !=
			   (uint8_t)DOS_EXEC_TRANSACTION_STATE_TARGET_READY &&
		       slot->state != (uint8_t)
			   DOS_EXEC_TRANSACTION_STATE_LOAD_LEASE_ABORTING;
	if (target->has_load_block != 1u || target->lease.handle.value == 0u ||
	    target->lease.handle.value == KERNEL_OBJECT_HANDLE_INVALID ||
	    target->lease.paragraphs != target->allocation.block_paragraphs ||
	    target->lease.maximum_available !=
		target->allocation.available_paragraphs ||
	    target->lease.reserved != 0u ||
	    (dos_memory_lease_table_identity_t)(
		target->lease.handle.value >>
		DOS_MEMORY_LEASE_TABLE_IDENTITY_SHIFT) !=
		slot->memory_lease_table_identity)
		return false;
	return slot->state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_TARGET_READY ||
	       slot->state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_LOAD_LEASED ||
	       slot->state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_PROCESS_PLANNED ||
	       slot->state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_RESIDENT_LOADING ||
	       slot->state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_RESIDENT_READY ||
	       slot->state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_RELOCATION_PLANNED ||
	       slot->state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_RELOCATING ||
	       slot->state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_IMAGE_PRIVATE ||
	       slot->state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_PROCESS_FILE_CLOSING ||
	       slot->state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_PROCESS_FILE_CLOSED ||
	       slot->state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_PARENT_SNAPSHOTTING ||
	       slot->state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_PARENT_SNAPSHOT_READY ||
	       slot->state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_SFT_PREPARING ||
	       slot->state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_SFT_RESERVED ||
	       slot->state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_SFT_ABORTING ||
	       slot->state == (uint8_t)
			 DOS_EXEC_TRANSACTION_STATE_PROCESS_PARAMETER_READING ||
	       slot->state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_PSP_PREPARING ||
	       slot->state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_PSP_PREPARED ||
	       slot->state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_DRIVE_RESOLVING ||
	       slot->state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_INITIAL_STATE_READY ||
	       state_is_process_memory_interval(slot->state) ||
	       slot->state == (uint8_t)
			 DOS_EXEC_TRANSACTION_STATE_LOAD_LEASE_ABORTING ||
	       slot->state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_FAILED ||
	       slot->state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_ABORTING ||
	       slot->state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_POISONED;
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

static bool com_process_plans_equal(const struct dos_com_process_plan *left,
				    const struct dos_com_process_plan *right)
{
	return left->psp_segment == right->psp_segment &&
	       left->block_end_segment == right->block_end_segment &&
	       left->load_segment == right->load_segment &&
	       left->load_offset == right->load_offset &&
	       left->load_linear_address == right->load_linear_address &&
	       left->image_size == right->image_size &&
	       left->read_capacity == right->read_capacity &&
	       left->stack_sentinel_offset == right->stack_sentinel_offset &&
	       left->stack_sentinel_value == right->stack_sentinel_value &&
	       left->load_only_stack_pointer ==
		   right->load_only_stack_pointer &&
	       left->load_only_stack_value == right->load_only_stack_value &&
	       left->launch_mode == right->launch_mode &&
	       left->reserved8 == right->reserved8 &&
	       left->reserved16 == right->reserved16 &&
	       cpu_states_equal(&left->initial_state, &right->initial_state);
}

static bool mz_process_plans_equal(const struct dos_mz_process_plan *left,
				   const struct dos_mz_process_plan *right)
{
	return left->psp_segment == right->psp_segment &&
	       left->block_end_segment == right->block_end_segment &&
	       left->load_segment == right->load_segment &&
	       left->load_offset == right->load_offset &&
	       left->load_linear_address == right->load_linear_address &&
	       left->reserved0 == right->reserved0 &&
	       left->image_file_offset == right->image_file_offset &&
	       left->image_size == right->image_size &&
	       left->resident_paragraphs == right->resident_paragraphs &&
	       left->relocation_factor == right->relocation_factor &&
	       left->relocation_count == right->relocation_count &&
	       left->relocation_table_offset ==
		   right->relocation_table_offset &&
	       left->load_only_stack_pointer ==
		   right->load_only_stack_pointer &&
	       left->load_only_stack_value == right->load_only_stack_value &&
	       left->load_high == right->load_high &&
	       left->launch_mode == right->launch_mode &&
	       left->reserved1 == right->reserved1 &&
	       cpu_states_equal(&left->initial_state, &right->initial_state);
}

static bool image_load_result_is_zero(
    const struct dos_image_load_result *load)
{
	return load->lease_handle == 0u && load->file_bytes_written == 0u &&
	       load->resident_bytes == 0u && load->untouched_bytes == 0u &&
	       load->reserved == 0u;
}

static void clear_transaction_resident(
    struct dos_exec_transaction_resident *resident)
{
	size_t index;

	for (index = 0u; index < ARRAY_SIZE(resident->process.bytes); ++index)
		resident->process.bytes[index] = 0u;
	resident->load = (struct dos_image_load_result){0};
	resident->format = 0u;
	resident->has_process_plan = 0u;
	resident->has_resident = 0u;
	for (index = 0u; index < ARRAY_SIZE(resident->reserved); ++index)
		resident->reserved[index] = 0u;
}

static bool transaction_resident_is_zero(
    const struct dos_exec_transaction_resident *resident)
{
	return bytes_are_zero(resident->process.bytes,
			      ARRAY_SIZE(resident->process.bytes)) &&
	       image_load_result_is_zero(&resident->load) &&
	       resident->format == 0u && resident->has_process_plan == 0u &&
	       resident->has_resident == 0u &&
	       bytes_are_zero(resident->reserved,
			      ARRAY_SIZE(resident->reserved));
}

static enum dos_process_launch_mode
slot_launch_mode(const struct dos_exec_transaction_slot *slot)
{
	return slot->request.subfunction == DOS_EXEC_LOAD_ONLY
		   ? DOS_PROCESS_LAUNCH_LOAD_ONLY
		   : DOS_PROCESS_LAUNCH_EXECUTE;
}

static bool resident_process_plan_matches_slot(
    const struct dos_exec_transaction_slot *slot)
{
	const struct dos_exec_transaction_resident *resident = &slot->resident;
	enum dos_process_status status;

	if (resident->format == (uint8_t)DOS_IMAGE_COM) {
		struct dos_com_process_plan expected;
		uint16_t initial_ax = dos_register_low16(
		    resident->process.com.initial_state.eax);

		status = dos_process_plan_com(
		    &slot->image.plan, &slot->target.allocation,
		    slot->target.lease.guest_segment, slot_launch_mode(slot),
		    initial_ax, &expected);
		return status == DOS_PROCESS_OK &&
		       com_process_plans_equal(&resident->process.com, &expected);
	}
	if (resident->format == (uint8_t)DOS_IMAGE_MZ) {
		struct dos_mz_process_plan expected;
		uint16_t initial_ax = dos_register_low16(
		    resident->process.mz.initial_state.eax);

		status = dos_process_plan_mz(
		    &slot->image.plan, &slot->target.allocation,
		    slot->target.lease.guest_segment, slot_launch_mode(slot),
		    initial_ax, &expected);
		return status == DOS_PROCESS_OK &&
		       mz_process_plans_equal(&resident->process.mz, &expected);
	}
	return false;
}

static bool resident_load_result_matches_slot(
    const struct dos_exec_transaction_slot *slot)
{
	const struct dos_exec_transaction_resident *resident = &slot->resident;
	uint64_t available;
	uint32_t expected_written;
	uint32_t expected_resident;

	if (resident->load.lease_handle != slot->target.lease.handle.value ||
	    resident->load.reserved != 0u)
		return false;
	if (resident->format == (uint8_t)DOS_IMAGE_COM) {
		expected_written = resident->process.com.image_size;
		expected_resident = resident->process.com.read_capacity;
	} else if (resident->format == (uint8_t)DOS_IMAGE_MZ) {
		expected_resident =
		    resident->process.mz.resident_paragraphs * 16u;
		available = slot->image.plan.file_size >
				    slot->image.plan.image_file_offset
			? slot->image.plan.file_size -
			      slot->image.plan.image_file_offset
			: 0u;
		expected_written = available < expected_resident
				       ? (uint32_t)available
				       : expected_resident;
	} else {
		return false;
	}
	return resident->load.file_bytes_written == expected_written &&
	       resident->load.resident_bytes == expected_resident &&
	       resident->load.untouched_bytes ==
		   expected_resident - expected_written;
}

static bool state_may_retain_resident(uint8_t state)
{
	return state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_PROCESS_PLANNED ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_RESIDENT_LOADING ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_RESIDENT_READY ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_RELOCATION_PLANNED ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_RELOCATING ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_IMAGE_PRIVATE ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_PROCESS_FILE_CLOSING ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_PROCESS_FILE_CLOSED ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_PARENT_SNAPSHOTTING ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_PARENT_SNAPSHOT_READY ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_SFT_PREPARING ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_SFT_RESERVED ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_SFT_ABORTING ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_PROCESS_PARAMETER_READING ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_PSP_PREPARING ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_PSP_PREPARED ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_DRIVE_RESOLVING ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_INITIAL_STATE_READY ||
	       state_is_process_memory_interval(state) ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_LOAD_LEASE_ABORTING ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_FAILED ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_ABORTING ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_POISONED;
}

static bool slot_resident_has_valid_encoding(
    const struct dos_exec_transaction_slot *slot)
{
	const struct dos_exec_transaction_resident *resident = &slot->resident;
	bool zero = transaction_resident_is_zero(resident);

	if (!request_is_process(&slot->request))
		return zero;
	if (zero)
		return slot->state !=
			   (uint8_t)DOS_EXEC_TRANSACTION_STATE_PROCESS_PLANNED &&
		       slot->state !=
			   (uint8_t)DOS_EXEC_TRANSACTION_STATE_RESIDENT_LOADING &&
		       slot->state !=
			   (uint8_t)DOS_EXEC_TRANSACTION_STATE_RESIDENT_READY;
	if (resident->has_process_plan != 1u ||
	    resident->has_resident > 1u ||
	    !bytes_are_zero(resident->reserved,
			    ARRAY_SIZE(resident->reserved)) ||
	    !state_may_retain_resident(slot->state) ||
	    slot->target.has_load_block != 1u || slot->image.has_plan != 1u ||
	    resident->format != slot->image.plan.format ||
	    !resident_process_plan_matches_slot(slot))
		return false;
	if (resident->has_resident == 0u)
		return image_load_result_is_zero(&resident->load) &&
		       slot->state !=
			   (uint8_t)DOS_EXEC_TRANSACTION_STATE_RESIDENT_READY;
	return resident_load_result_matches_slot(slot) &&
	       slot->state !=
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_PROCESS_PLANNED;
}

static void copy_image_load_result(
    struct dos_image_load_result *destination,
    const struct dos_image_load_result *source)
{
	destination->lease_handle = source->lease_handle;
	destination->file_bytes_written = source->file_bytes_written;
	destination->resident_bytes = source->resident_bytes;
	destination->untouched_bytes = source->untouched_bytes;
	destination->reserved = source->reserved;
}

static void copy_transaction_resident(
    struct dos_exec_transaction_resident *destination,
    const struct dos_exec_transaction_resident *source)
{
	size_t index;

	clear_transaction_resident(destination);
	if (source->format == (uint8_t)DOS_IMAGE_COM)
		destination->process.com = source->process.com;
	else if (source->format == (uint8_t)DOS_IMAGE_MZ)
		destination->process.mz = source->process.mz;
	copy_image_load_result(&destination->load, &source->load);
	destination->format = source->format;
	destination->has_process_plan = source->has_process_plan;
	destination->has_resident = source->has_resident;
	for (index = 0u; index < ARRAY_SIZE(destination->reserved); ++index)
		destination->reserved[index] = source->reserved[index];
}

static bool relocation_request_is_zero(
    const struct dos_relocator_request *request)
{
	return request->relocation_table_offset == 0u &&
	       request->resident_size == 0u &&
	       request->resident_linear_address == 0u &&
	       request->relocation_count == 0u &&
	       request->relocation_factor == 0u;
}

static bool transaction_relocation_is_zero(
    const struct dos_exec_transaction_relocation *relocation)
{
	return relocation_request_is_zero(&relocation->request) &&
	       relocation->result.validated_entries == 0u &&
	       relocation->result.applied_entries == 0u &&
	       relocation->applicable == 0u && relocation->has_request == 0u &&
	       relocation->applied == 0u &&
	       bytes_are_zero(relocation->reserved,
			      ARRAY_SIZE(relocation->reserved));
}

static void clear_transaction_relocation(
    struct dos_exec_transaction_relocation *relocation)
{
	size_t index;

	relocation->request = (struct dos_relocator_request){0};
	relocation->result = (struct dos_relocator_result){0};
	relocation->applicable = 0u;
	relocation->has_request = 0u;
	relocation->applied = 0u;
	for (index = 0u; index < ARRAY_SIZE(relocation->reserved); ++index)
		relocation->reserved[index] = 0u;
}

static void copy_transaction_relocation(
    struct dos_exec_transaction_relocation *destination,
    const struct dos_exec_transaction_relocation *source)
{
	size_t index;

	destination->request.relocation_table_offset =
	    source->request.relocation_table_offset;
	destination->request.resident_size = source->request.resident_size;
	destination->request.resident_linear_address =
	    source->request.resident_linear_address;
	destination->request.relocation_count =
	    source->request.relocation_count;
	destination->request.relocation_factor =
	    source->request.relocation_factor;
	destination->result.validated_entries =
	    source->result.validated_entries;
	destination->result.applied_entries = source->result.applied_entries;
	destination->applicable = source->applicable;
	destination->has_request = source->has_request;
	destination->applied = source->applied;
	for (index = 0u; index < ARRAY_SIZE(destination->reserved); ++index)
		destination->reserved[index] = source->reserved[index];
}

static bool relocation_request_matches_slot(
    const struct dos_exec_transaction_slot *slot)
{
	const struct dos_relocator_request *request = &slot->relocation.request;

	return request->relocation_table_offset ==
		   slot->resident.process.mz.relocation_table_offset &&
	       request->resident_size == slot->resident.load.resident_bytes &&
	       request->resident_linear_address ==
		   slot->resident.process.mz.load_linear_address &&
	       request->relocation_count ==
		   slot->resident.process.mz.relocation_count &&
	       request->relocation_factor ==
		   slot->resident.process.mz.relocation_factor;
}

static bool state_may_retain_relocation(uint8_t state)
{
	return state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_RELOCATION_PLANNED ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_RELOCATING ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_IMAGE_PRIVATE ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_PROCESS_FILE_CLOSING ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_PROCESS_FILE_CLOSED ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_PARENT_SNAPSHOTTING ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_PARENT_SNAPSHOT_READY ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_SFT_PREPARING ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_SFT_RESERVED ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_SFT_ABORTING ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_PROCESS_PARAMETER_READING ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_PSP_PREPARING ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_PSP_PREPARED ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_DRIVE_RESOLVING ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_INITIAL_STATE_READY ||
	       state_is_process_memory_interval(state) ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_LOAD_LEASE_ABORTING ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_FAILED ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_ABORTING ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_POISONED;
}

static bool slot_relocation_has_valid_encoding(
    const struct dos_exec_transaction_slot *slot)
{
	const struct dos_exec_transaction_relocation *relocation =
	    &slot->relocation;
	bool zero = transaction_relocation_is_zero(relocation);

	if (!request_is_process(&slot->request))
		return zero;
	if (zero) {
		if (slot->state ==
				(uint8_t)
				    DOS_EXEC_TRANSACTION_STATE_RELOCATION_PLANNED ||
		    slot->state ==
				(uint8_t)DOS_EXEC_TRANSACTION_STATE_RELOCATING)
			return false;
		if (slot->state ==
		    (uint8_t)DOS_EXEC_TRANSACTION_STATE_IMAGE_PRIVATE)
			return slot->resident.has_resident == 1u &&
			       slot->resident.format == (uint8_t)DOS_IMAGE_COM;
		return true;
	}
	if (relocation->applicable != 1u || relocation->has_request != 1u ||
	    relocation->applied > 1u ||
	    !bytes_are_zero(relocation->reserved,
			    ARRAY_SIZE(relocation->reserved)) ||
	    !state_may_retain_relocation(slot->state) ||
	    slot->resident.has_resident != 1u ||
	    slot->resident.format != (uint8_t)DOS_IMAGE_MZ ||
	    !relocation_request_matches_slot(slot))
		return false;
	if (relocation->applied == 0u)
		return relocation->result.validated_entries == 0u &&
		       relocation->result.applied_entries == 0u &&
		       slot->state !=
			   (uint8_t)DOS_EXEC_TRANSACTION_STATE_IMAGE_PRIVATE;
	return relocation->result.validated_entries ==
		   relocation->request.relocation_count &&
	       relocation->result.applied_entries ==
		   relocation->request.relocation_count &&
	       slot->state !=
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_RELOCATION_PLANNED;
}

static void clear_transaction_parent(
    struct dos_exec_transaction_parent *parent)
{
	size_t index;

	parent->snapshot = (struct dos_process_parent_snapshot){0};
	parent->has_snapshot = 0u;
	for (index = 0u; index < ARRAY_SIZE(parent->reserved); ++index)
		parent->reserved[index] = 0u;
}

static void copy_transaction_parent(
    struct dos_exec_transaction_parent *destination,
    const struct dos_exec_transaction_parent *source)
{
	size_t index;

	destination->snapshot.machine_identity =
	    source->snapshot.machine_identity;
	destination->snapshot.machine_context = source->snapshot.machine_context;
	destination->snapshot.machine_address_limit =
	    source->snapshot.machine_address_limit;
	destination->snapshot.parent_psp_segment =
	    source->snapshot.parent_psp_segment;
	destination->snapshot.a20_enabled = source->snapshot.a20_enabled;
	destination->snapshot.captured = source->snapshot.captured;
	for (index = 0u; index < ARRAY_SIZE(destination->snapshot.reserved);
	     ++index)
		destination->snapshot.reserved[index] =
		    source->snapshot.reserved[index];
	for (index = 0u; index < ARRAY_SIZE(destination->snapshot.parent_psp);
	     ++index)
		destination->snapshot.parent_psp[index] =
		    source->snapshot.parent_psp[index];
	for (index = 0u;
	     index < ARRAY_SIZE(destination->snapshot.parent_jft.entries);
	     ++index)
		destination->snapshot.parent_jft.entries[index] =
		    source->snapshot.parent_jft.entries[index];
	for (index = 0u; index < ARRAY_SIZE(destination->snapshot.reserved_tail);
	     ++index)
		destination->snapshot.reserved_tail[index] =
		    source->snapshot.reserved_tail[index];
	destination->has_snapshot = source->has_snapshot;
	for (index = 0u; index < ARRAY_SIZE(destination->reserved); ++index)
		destination->reserved[index] = source->reserved[index];
}

static bool transaction_parent_is_zero(
    const struct dos_exec_transaction_parent *parent)
{
	return parent->snapshot.machine_identity == 0u &&
	       parent->snapshot.machine_context == 0u &&
	       parent->snapshot.machine_address_limit == 0u &&
	       parent->snapshot.parent_psp_segment == 0u &&
	       parent->snapshot.a20_enabled == 0u &&
	       parent->snapshot.captured == 0u &&
	       bytes_are_zero(parent->snapshot.reserved,
			      ARRAY_SIZE(parent->snapshot.reserved)) &&
	       bytes_are_zero(parent->snapshot.parent_psp,
			      ARRAY_SIZE(parent->snapshot.parent_psp)) &&
	       bytes_are_zero(parent->snapshot.parent_jft.entries,
			      ARRAY_SIZE(parent->snapshot.parent_jft.entries)) &&
	       bytes_are_zero(parent->snapshot.reserved_tail,
			      ARRAY_SIZE(parent->snapshot.reserved_tail)) &&
	       parent->has_snapshot == 0u &&
	       bytes_are_zero(parent->reserved, ARRAY_SIZE(parent->reserved));
}

static bool slot_parent_has_valid_encoding(
    const struct dos_exec_transaction_slot *slot)
{
	const struct dos_exec_transaction_parent *parent = &slot->parent;
	bool zero = transaction_parent_is_zero(parent);
	bool retained_state;

	if (!request_is_process(&slot->request))
		return zero;
	if (zero)
		return slot->state !=
		       (uint8_t)DOS_EXEC_TRANSACTION_STATE_PARENT_SNAPSHOT_READY;
	retained_state =
	    slot->state ==
		(uint8_t)DOS_EXEC_TRANSACTION_STATE_PARENT_SNAPSHOT_READY ||
	    slot->state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_SFT_PREPARING ||
	    slot->state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_SFT_RESERVED ||
	    slot->state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_SFT_ABORTING ||
	    slot->state == (uint8_t)
		DOS_EXEC_TRANSACTION_STATE_PROCESS_PARAMETER_READING ||
	    slot->state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_PSP_PREPARING ||
	    slot->state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_PSP_PREPARED ||
	    slot->state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_DRIVE_RESOLVING ||
	    slot->state ==
		(uint8_t)DOS_EXEC_TRANSACTION_STATE_INITIAL_STATE_READY ||
	    state_is_process_memory_interval(slot->state) ||
	    slot->state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_FAILED ||
	    slot->state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_ABORTING ||
	    slot->state ==
		(uint8_t)DOS_EXEC_TRANSACTION_STATE_LOAD_LEASE_ABORTING ||
	    slot->state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_POISONED;
	return parent->has_snapshot == 1u && retained_state &&
	       bytes_are_zero(parent->reserved, ARRAY_SIZE(parent->reserved)) &&
	       parent->snapshot.machine_identity == slot->machine_identity &&
	       parent->snapshot.machine_context == slot->machine_context &&
	       parent->snapshot.machine_address_limit ==
		   slot->machine_address_limit &&
	       parent->snapshot.parent_psp_segment ==
		   slot->parent_runtime.current_psp &&
	       parent->snapshot.a20_enabled == slot->machine_a20_enabled &&
	       parent->snapshot.captured == 1u &&
	       bytes_are_zero(parent->snapshot.reserved,
			      ARRAY_SIZE(parent->snapshot.reserved)) &&
	       bytes_are_zero(parent->snapshot.reserved_tail,
			      ARRAY_SIZE(parent->snapshot.reserved_tail));
}

static void clear_transaction_inheritance(
    struct dos_exec_transaction_inheritance *inheritance)
{
	size_t index;

	inheritance->batch = DOS_SFT_BATCH_HANDLE_INVALID;
	for (index = 0u; index < ARRAY_SIZE(inheritance->child_jft.entries);
	     ++index)
		inheritance->child_jft.entries[index] = 0u;
	inheritance->has_batch = 0u;
	inheritance->has_child_jft = 0u;
	for (index = 0u; index < ARRAY_SIZE(inheritance->reserved); ++index)
		inheritance->reserved[index] = 0u;
}

static void copy_transaction_inheritance(
    struct dos_exec_transaction_inheritance *destination,
    const struct dos_exec_transaction_inheritance *source)
{
	size_t index;

	destination->batch = source->batch;
	for (index = 0u; index < ARRAY_SIZE(destination->child_jft.entries);
	     ++index)
		destination->child_jft.entries[index] =
		    source->child_jft.entries[index];
	destination->has_batch = source->has_batch;
	destination->has_child_jft = source->has_child_jft;
	for (index = 0u; index < ARRAY_SIZE(destination->reserved); ++index)
		destination->reserved[index] = source->reserved[index];
}

static bool transaction_inheritance_is_zero(
    const struct dos_exec_transaction_inheritance *inheritance)
{
	return inheritance->batch == DOS_SFT_BATCH_HANDLE_INVALID &&
	       bytes_are_zero(inheritance->child_jft.entries,
			      ARRAY_SIZE(inheritance->child_jft.entries)) &&
	       inheritance->has_batch == 0u &&
	       inheritance->has_child_jft == 0u &&
	       bytes_are_zero(inheritance->reserved,
			      ARRAY_SIZE(inheritance->reserved));
}

static bool slot_inheritance_has_valid_encoding(
    const struct dos_exec_transaction_slot *slot)
{
	const struct dos_exec_transaction_inheritance *inheritance =
	    &slot->inheritance;
	bool zero = transaction_inheritance_is_zero(inheritance);

	if (!request_is_process(&slot->request))
		return zero;
	if (zero)
		return slot->state !=
		       (uint8_t)DOS_EXEC_TRANSACTION_STATE_SFT_RESERVED;
	if (inheritance->batch == DOS_SFT_BATCH_HANDLE_INVALID ||
	    inheritance->batch == KERNEL_OBJECT_HANDLE_INVALID ||
	    inheritance->has_batch != 1u || inheritance->has_child_jft > 1u ||
	    !bytes_are_zero(inheritance->reserved,
			    ARRAY_SIZE(inheritance->reserved)))
		return false;
	if (inheritance->has_child_jft == 0u)
		return slot->state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_POISONED;
	return slot->state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_SFT_RESERVED ||
	       slot->state == (uint8_t)
			 DOS_EXEC_TRANSACTION_STATE_PROCESS_PARAMETER_READING ||
	       slot->state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_PSP_PREPARING ||
	       slot->state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_PSP_PREPARED ||
	       slot->state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_DRIVE_RESOLVING ||
	       slot->state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_INITIAL_STATE_READY ||
	       state_is_process_memory_interval(slot->state) ||
	       slot->state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_SFT_ABORTING ||
	       slot->state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_ABORTING ||
	       slot->state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_FAILED ||
	       slot->state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_POISONED;
}

static void clear_transaction_psp(struct dos_exec_transaction_psp *psp)
{
	size_t index;

	psp->image.segment = 0u;
	for (index = 0u; index < ARRAY_SIZE(psp->image.bytes); ++index)
		psp->image.bytes[index] = 0u;
	psp->has_image = 0u;
	for (index = 0u; index < ARRAY_SIZE(psp->reserved); ++index)
		psp->reserved[index] = 0u;
}

static void copy_transaction_psp(struct dos_exec_transaction_psp *destination,
				 const struct dos_exec_transaction_psp *source)
{
	size_t index;

	destination->image.segment = source->image.segment;
	for (index = 0u; index < ARRAY_SIZE(destination->image.bytes); ++index)
		destination->image.bytes[index] = source->image.bytes[index];
	destination->has_image = source->has_image;
	for (index = 0u; index < ARRAY_SIZE(destination->reserved); ++index)
		destination->reserved[index] = source->reserved[index];
}

static bool transaction_psp_is_zero(const struct dos_exec_transaction_psp *psp)
{
	return psp->image.segment == 0u &&
	       bytes_are_zero(psp->image.bytes, ARRAY_SIZE(psp->image.bytes)) &&
	       psp->has_image == 0u &&
	       bytes_are_zero(psp->reserved, ARRAY_SIZE(psp->reserved));
}

static bool slot_psp_has_valid_encoding(
    const struct dos_exec_transaction_slot *slot)
{
	const struct dos_exec_transaction_psp *psp = &slot->psp;
	bool zero = transaction_psp_is_zero(psp);

	if (!request_is_process(&slot->request))
		return zero;
	if (zero)
		return slot->state !=
			   (uint8_t)DOS_EXEC_TRANSACTION_STATE_PSP_PREPARED &&
		       slot->state !=
			   (uint8_t)DOS_EXEC_TRANSACTION_STATE_DRIVE_RESOLVING &&
		       slot->state != (uint8_t)
			   DOS_EXEC_TRANSACTION_STATE_INITIAL_STATE_READY &&
		       !state_is_process_memory_interval(slot->state);
	return psp->has_image == 1u &&
	       bytes_are_zero(psp->reserved, ARRAY_SIZE(psp->reserved)) &&
	       psp->image.segment == slot->target.lease.guest_segment &&
	       (slot->state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_PSP_PREPARED ||
		slot->state ==
		    (uint8_t)DOS_EXEC_TRANSACTION_STATE_DRIVE_RESOLVING ||
		slot->state ==
		    (uint8_t)DOS_EXEC_TRANSACTION_STATE_INITIAL_STATE_READY ||
		state_is_process_memory_interval(slot->state) ||
		slot->state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_ABORTING ||
		slot->state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_SFT_ABORTING ||
		slot->state ==
		    (uint8_t)DOS_EXEC_TRANSACTION_STATE_LOAD_LEASE_ABORTING ||
		slot->state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_FAILED ||
		slot->state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_POISONED);
}

static bool slot_journal_has_valid_encoding(
    const struct dos_exec_transaction_slot *slot)
{
	const struct dos_exec_journal *journal = &slot->journal;

	if (!dos_exec_journal_has_valid_encoding(journal))
		return false;
	if (slot->state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_VACANT ||
	    !request_is_process(&slot->request))
		return journal->state ==
			   (uint8_t)DOS_EXEC_JOURNAL_STATE_UNINITIALIZED;
	return journal->state !=
		   (uint8_t)DOS_EXEC_JOURNAL_STATE_UNINITIALIZED &&
	       journal->machine_identity == slot->machine_identity &&
	       journal->machine_context == slot->machine_context &&
	       journal->machine_address_limit == slot->machine_address_limit &&
	       journal->a20_enabled == slot->machine_a20_enabled;
}

static bool environment_plan_matches_slot(
    const struct dos_exec_transaction_slot *slot,
    const struct dos_environment_plan *plan)
{
	return dos_environment_plan_has_valid_encoding(plan) &&
	       plan->source.offset == slot->environment_source.source.offset &&
	       plan->source.segment == slot->environment_source.source.segment &&
	       plan->executable_name.source.offset ==
		   slot->executable_name.source.offset &&
	       plan->executable_name.source.segment ==
		   slot->executable_name.source.segment &&
	       plan->executable_name.bytes_including_nul ==
		   slot->executable_name.bytes_including_nul &&
	       plan->executable_name.reserved == slot->executable_name.reserved;
}

static bool state_may_retain_environment_plan(uint8_t state)
{
	return state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_PLANNED ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_ALLOCATING ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_LEASED ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_BUILDING ||
	       state == (uint8_t)
			    DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_BLOCK_READY ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_IMAGE_READING ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_IMAGE_READY ||
	       state_is_load_block_interval(state) ||
	       state == (uint8_t)
			    DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_LEASE_ABORTING ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_FAILED ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_ABORTING ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_ABORT_FILE_CLOSING ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_OBSERVER_RELEASING ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_ABORTED ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_POISONED;
}

static bool state_requires_environment_lease(uint8_t state)
{
	return state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_LEASED ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_BUILDING ||
	       state == (uint8_t)
			    DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_BLOCK_READY ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_IMAGE_READING ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_IMAGE_READY ||
	       state_is_load_block_interval(state) ||
	       state == (uint8_t)
			    DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_LEASE_ABORTING;
}

static bool slot_environment_has_valid_encoding(
    const struct dos_exec_transaction_slot *slot)
{
	const struct dos_exec_transaction_environment *environment =
	    &slot->environment;
	bool zero = transaction_environment_is_zero(environment);

	if (!request_is_process(&slot->request))
		return zero;
	if (zero) {
		if (slot->state == (uint8_t)
				      DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_BLOCK_READY ||
		    slot->state ==
			(uint8_t)DOS_EXEC_TRANSACTION_STATE_IMAGE_READING ||
		    slot->state ==
			(uint8_t)DOS_EXEC_TRANSACTION_STATE_IMAGE_READY ||
		    state_is_load_block_interval(slot->state))
			return slot->environment_source.kind ==
			       DOS_EXEC_ENVIRONMENT_SOURCE_NONE;
		return !state_requires_environment_lease(slot->state) &&
		       slot->state != (uint8_t)
				  DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_PLANNED &&
		       slot->state != (uint8_t)
				  DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_ALLOCATING;
	}
	if (!bytes_are_zero(environment->reserved,
			    ARRAY_SIZE(environment->reserved)) ||
	    !state_may_retain_environment_plan(slot->state) ||
	    slot->environment_source.kind == DOS_EXEC_ENVIRONMENT_SOURCE_NONE ||
	    !environment_plan_matches_slot(slot, &environment->plan))
		return false;
	if (environment->has_block == 0u) {
		if (!memory_lease_receipt_is_zero(&environment->lease))
			return false;
		return !state_requires_environment_lease(slot->state);
	}
	if (environment->has_block != 1u ||
	    environment->lease.handle.value == 0u ||
	    environment->lease.handle.value == KERNEL_OBJECT_HANDLE_INVALID ||
	    environment->lease.paragraphs != environment->plan.paragraphs ||
	    environment->lease.reserved != 0u ||
	    (dos_memory_lease_table_identity_t)(
		environment->lease.handle.value >>
		DOS_MEMORY_LEASE_TABLE_IDENTITY_SHIFT) !=
		slot->memory_lease_table_identity)
		return false;
	return state_requires_environment_lease(slot->state) ||
	       slot->state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_FAILED ||
	       slot->state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_ABORTING ||
	       slot->state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_POISONED;
}

static bool observer_is_idle(const struct dos_exec_observer *observer)
{
	return observer->adapter_identity == KERNEL_OBJECT_HANDLE_INVALID &&
	       observer->context == KERNEL_OBJECT_HANDLE_INVALID &&
	       observer->generation == 0u &&
	       observer->state == (uint8_t)DOS_EXEC_OBSERVER_STATE_IDLE &&
	       observer->constructed == 1u &&
	       observer_reserved_is_zero(observer);
}

static bool request_is_zero(const struct dos_exec_transaction_request *request)
{
	return request->executable_name.offset == 0u &&
	       request->executable_name.segment == 0u &&
	       request->parameter_block.offset == 0u &&
	       request->parameter_block.segment == 0u &&
	       request->subfunction == 0u &&
	       bytes_are_zero(request->reserved, ARRAY_SIZE(request->reserved));
}

static bool publication_rebind_matches_slot(
    const struct dos_exec_transaction_slot *slot,
    const struct dos_memory_lease_rebind_plan *plan,
    const struct dos_memory_lease_receipt *receipt)
{
	return dos_memory_lease_rebind_plan_has_valid_encoding(plan) &&
	       plan->handle.value == receipt->handle.value &&
	       plan->machine_context == slot->machine_context &&
	       plan->arena_identity == slot->memory_arena_identity &&
	       plan->arena_generation == slot->memory_arena_generation &&
	       plan->guest_segment == receipt->guest_segment &&
	       plan->paragraphs == receipt->paragraphs &&
	       plan->arena_head_segment == slot->memory_arena_head_segment &&
	       plan->value.expected_owner == slot->parent_runtime.current_psp &&
	       plan->value.new_owner == slot->target.lease.guest_segment;
}

static bool handoff_plan_is_zero(const struct dos_exec_handoff_plan *plan)
{
	const struct dos_cpu_state zero_state = {0};

	return cpu_states_equal(&plan->entry_state, &zero_state) &&
	       plan->stack_image.segment == 0u &&
	       plan->stack_image.offset == 0u &&
	       bytes_are_zero(plan->stack_image.bytes,
			      ARRAY_SIZE(plan->stack_image.bytes)) &&
	       plan->child_psp == 0u && plan->format == 0u &&
	       plan->stack_word_count == 0u &&
	       bytes_are_zero(plan->reserved, ARRAY_SIZE(plan->reserved));
}

static bool handoff_matches_slot(
    const struct dos_exec_transaction_slot *slot,
    const struct dos_exec_handoff_plan *handoff)
{
	const struct dos_cpu_state *expected_state;

	if (!dos_exec_handoff_plan_has_valid_encoding(handoff) ||
	    handoff->child_psp != slot->target.lease.guest_segment ||
	    handoff->format != slot->resident.format)
		return false;
	if (slot->resident.format == (uint8_t)DOS_IMAGE_COM)
		expected_state = &slot->resident.process.com.initial_state;
	else if (slot->resident.format == (uint8_t)DOS_IMAGE_MZ)
		expected_state = &slot->resident.process.mz.initial_state;
	else
		return false;
	return cpu_states_equal(&handoff->entry_state, expected_state);
}

static bool backend_binding_is_clear(
    const struct dos_exec_transaction_publication *publication)
{
	return publication->backend_session.value == 0u &&
	       publication->backend_session_table_identity ==
		   KERNEL_OBJECT_HANDLE_INVALID &&
	       publication->backend_adapter_identity ==
		   KERNEL_OBJECT_HANDLE_INVALID &&
	       publication->backend_adapter_context ==
		   KERNEL_OBJECT_HANDLE_INVALID;
}

static bool backend_binding_is_valid(
    const struct dos_exec_transaction_publication *publication)
{
	return publication->backend_session.value != 0u &&
	       publication->backend_session.value !=
		   KERNEL_OBJECT_HANDLE_INVALID &&
	       identity_is_valid(publication->backend_session_table_identity) &&
	       identity_is_valid(publication->backend_adapter_identity) &&
	       publication->backend_adapter_context !=
		   KERNEL_OBJECT_HANDLE_INVALID;
}

static bool slot_publication_has_valid_encoding(
    const struct dos_exec_transaction_slot *slot)
{
	const struct dos_exec_transaction_publication *publication =
	    &slot->publication;
	bool has_rebinds;

	if (publication->has_owner_name > 1u ||
	    publication->has_environment_rebind > 1u ||
	    publication->has_load_rebind > 1u ||
	    publication->has_handoff > 1u ||
	    publication->has_backend_session > 1u ||
	    !bytes_are_zero(publication->reserved,
			    ARRAY_SIZE(publication->reserved)))
		return false;
	if (executable_name_is_unused(&slot->executable_name)) {
		if (publication->has_owner_name != 0u ||
		    !owner_name_patch_is_zero(&publication->owner_name))
			return false;
	} else if (publication->has_owner_name != 1u ||
		   !owner_name_patch_has_valid_encoding(
		       &publication->owner_name)) {
		return false;
	}

	has_rebinds = publication->has_load_rebind != 0u ||
		      publication->has_environment_rebind != 0u;
	if (!has_rebinds) {
		return rebind_plan_is_zero(&publication->environment_rebind) &&
		       rebind_plan_is_zero(&publication->load_rebind) &&
		       publication->has_handoff == 0u &&
		       handoff_plan_is_zero(&publication->handoff) &&
		       publication->has_backend_session == 0u &&
		       backend_binding_is_clear(publication) &&
		       slot->state != (uint8_t)
				  DOS_EXEC_TRANSACTION_STATE_GLOBAL_MEMORY_READY;
	}
	if (!request_is_process(&slot->request) ||
	    publication->has_load_rebind != 1u ||
	    slot->target.has_load_block != 1u ||
	    !publication_rebind_matches_slot(slot, &publication->load_rebind,
					     &slot->target.lease) ||
	    publication->load_rebind.guest_segment !=
		slot->target.lease.guest_segment)
		return false;
	if (slot->environment.has_block != 0u) {
		if (publication->has_environment_rebind != 1u ||
		    !publication_rebind_matches_slot(
			slot, &publication->environment_rebind,
			&slot->environment.lease))
			return false;
	} else if (publication->has_environment_rebind != 0u ||
		   !rebind_plan_is_zero(&publication->environment_rebind)) {
		return false;
	}
	if (slot->request.subfunction == DOS_EXEC_LOAD_ONLY) {
		if (publication->has_handoff != 0u ||
		    !handoff_plan_is_zero(&publication->handoff) ||
		    publication->has_backend_session != 0u ||
		    !backend_binding_is_clear(publication))
			return false;
	} else if (slot->request.subfunction == DOS_EXEC_LOAD_AND_EXECUTE) {
		if (publication->has_handoff == 0u) {
			if (!handoff_plan_is_zero(&publication->handoff) ||
			    publication->has_backend_session != 0u ||
			    !backend_binding_is_clear(publication) ||
			    (slot->state != (uint8_t)
				 DOS_EXEC_TRANSACTION_STATE_GLOBAL_MEMORY_READY &&
			     slot->state != (uint8_t)
				 DOS_EXEC_TRANSACTION_STATE_HANDOFF_STAGING &&
			     slot->state !=
				 (uint8_t)DOS_EXEC_TRANSACTION_STATE_POISONED))
				return false;
		} else if (!handoff_matches_slot(slot, &publication->handoff)) {
			return false;
		} else if (publication->has_backend_session == 0u) {
			if (!backend_binding_is_clear(publication) ||
			    (slot->state !=
				(uint8_t)DOS_EXEC_TRANSACTION_STATE_HANDOFF_READY &&
			     slot->state != (uint8_t)
				 DOS_EXEC_TRANSACTION_STATE_BACKEND_PREPARING &&
			     slot->state !=
				 (uint8_t)DOS_EXEC_TRANSACTION_STATE_POISONED))
				return false;
		} else if (!backend_binding_is_valid(publication) ||
			   (slot->state != (uint8_t)
				DOS_EXEC_TRANSACTION_STATE_BACKEND_DORMANT &&
			    slot->state != (uint8_t)
				DOS_EXEC_TRANSACTION_STATE_BACKEND_RELEASING &&
			    slot->state !=
				(uint8_t)DOS_EXEC_TRANSACTION_STATE_COMMITTING &&
			    slot->state !=
				(uint8_t)DOS_EXEC_TRANSACTION_STATE_PUBLISHED &&
			    slot->state !=
				(uint8_t)DOS_EXEC_TRANSACTION_STATE_POISONED)) {
			return false;
		}
	} else {
		return false;
	}
	return slot->state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_GLOBAL_MEMORY_READY ||
	       slot->state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_HANDOFF_STAGING ||
	       slot->state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_HANDOFF_READY ||
	       slot->state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_BACKEND_PREPARING ||
	       slot->state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_BACKEND_DORMANT ||
	       slot->state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_BACKEND_RELEASING ||
	       slot->state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_COMMITTING ||
	       slot->state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_PUBLISHED ||
	       slot->state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_POISONED;
}

static bool
slot_is_canonical_vacant(const struct dos_exec_transaction_slot *slot)
{
	return slot->coordinator_identity == KERNEL_OBJECT_HANDLE_INVALID &&
	       slot->machine_identity == KERNEL_OBJECT_HANDLE_INVALID &&
	       slot->machine_context == KERNEL_OBJECT_HANDLE_INVALID &&
	       slot->machine_address_limit == 0u &&
	       slot->file_adapter_identity == KERNEL_OBJECT_HANDLE_INVALID &&
	       slot->file_adapter_context == KERNEL_OBJECT_HANDLE_INVALID &&
	       slot->file_lease_table_identity ==
		   KERNEL_OBJECT_HANDLE_INVALID &&
	       slot->runtime_identity == KERNEL_OBJECT_HANDLE_INVALID &&
	       slot->sft_adapter_identity == KERNEL_OBJECT_HANDLE_INVALID &&
	       slot->sft_adapter_context == KERNEL_OBJECT_HANDLE_INVALID &&
	       slot->drive_adapter_identity == KERNEL_OBJECT_HANDLE_INVALID &&
	       slot->drive_adapter_context == KERNEL_OBJECT_HANDLE_INVALID &&
	       slot->observer_adapter_identity ==
		   KERNEL_OBJECT_HANDLE_INVALID &&
	       slot->observer_adapter_context == KERNEL_OBJECT_HANDLE_INVALID &&
	       snapshot_is_unused(&slot->parent_runtime) &&
	       environment_source_is_unused(&slot->environment_source) &&
	       executable_name_is_unused(&slot->executable_name) &&
	       slot->file_lease.value == 0u &&
	       request_is_zero(&slot->request) &&
	       observer_is_idle(&slot->observer) &&
	       slot->has_file_lease == 0u && slot->machine_a20_enabled == 0u &&
	       slot->memory_arena_identity == KERNEL_OBJECT_HANDLE_INVALID &&
	       slot->memory_arena_generation == 0u &&
	       transaction_environment_is_zero(&slot->environment) &&
	       slot->memory_lease_table_identity ==
		   DOS_MEMORY_LEASE_TABLE_IDENTITY_INVALID &&
	       slot->memory_arena_head_segment == 0u &&
	       bytes_are_zero(slot->reserved_extension,
			      ARRAY_SIZE(slot->reserved_extension)) &&
	       transaction_image_is_zero(&slot->image) &&
	       transaction_target_is_zero(&slot->target) &&
	       transaction_resident_is_zero(&slot->resident) &&
	       transaction_relocation_is_zero(&slot->relocation) &&
	       transaction_parent_is_zero(&slot->parent) &&
	       transaction_inheritance_is_zero(&slot->inheritance) &&
	       transaction_psp_is_zero(&slot->psp) &&
	       slot_publication_has_valid_encoding(slot) &&
	       dos_exec_journal_has_valid_encoding(&slot->journal) &&
	       slot->journal.state ==
		   (uint8_t)DOS_EXEC_JOURNAL_STATE_UNINITIALIZED;
}

static bool
slot_has_valid_encoding(const struct dos_exec_transaction_slot *slot)
{
	uint8_t observer_state;

	if (slot == NULL ||
	    slot->generation > DOS_EXEC_TRANSACTION_GENERATION_MAX ||
	    !state_is_implemented(slot->state) || slot->has_file_lease > 1u ||
	    slot->machine_a20_enabled > 1u ||
	    !bytes_are_zero(slot->reserved, ARRAY_SIZE(slot->reserved)) ||
	    !bytes_are_zero(slot->reserved_extension,
			    ARRAY_SIZE(slot->reserved_extension)))
		return false;
	if (slot->state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_VACANT)
		return slot_is_canonical_vacant(slot);
	if (slot->generation == 0u ||
	    !identity_is_valid(slot->coordinator_identity) ||
	    !identity_is_valid(slot->machine_identity) ||
	    !identity_is_valid(slot->file_adapter_identity) ||
	    !identity_is_valid(slot->file_lease_table_identity) ||
	    !identity_is_valid(slot->observer_adapter_identity) ||
	    slot->machine_context == KERNEL_OBJECT_HANDLE_INVALID ||
	    slot->file_adapter_context == KERNEL_OBJECT_HANDLE_INVALID ||
	    slot->observer_adapter_context == KERNEL_OBJECT_HANDLE_INVALID ||
	    slot->machine_address_limit == 0u ||
	    slot->machine_address_limit > DOS_GUEST_32_ADDRESS_LIMIT ||
	    !request_has_valid_encoding(&slot->request) ||
	    slot->observer.constructed != 1u ||
	    !observer_reserved_is_zero(&slot->observer))
		return false;
	if (request_is_process(&slot->request)) {
		if (!identity_is_valid(slot->runtime_identity) ||
		    !identity_is_valid(slot->sft_adapter_identity) ||
		    !identity_is_valid(slot->drive_adapter_identity) ||
		    slot->drive_adapter_context == KERNEL_OBJECT_HANDLE_INVALID ||
		    slot->sft_adapter_context == KERNEL_OBJECT_HANDLE_INVALID ||
		    !identity_is_valid(slot->memory_arena_identity) ||
		    slot->memory_arena_generation == 0u ||
		    !memory_lease_table_identity_is_valid(
			slot->memory_lease_table_identity))
			return false;
	} else if (slot->runtime_identity != KERNEL_OBJECT_HANDLE_INVALID ||
		   slot->sft_adapter_identity != KERNEL_OBJECT_HANDLE_INVALID ||
		   slot->sft_adapter_context != KERNEL_OBJECT_HANDLE_INVALID ||
		   slot->drive_adapter_identity != KERNEL_OBJECT_HANDLE_INVALID ||
		   slot->drive_adapter_context != KERNEL_OBJECT_HANDLE_INVALID ||
		   slot->memory_arena_identity != KERNEL_OBJECT_HANDLE_INVALID ||
		   slot->memory_arena_generation != 0u ||
		   slot->memory_lease_table_identity !=
		       DOS_MEMORY_LEASE_TABLE_IDENTITY_INVALID ||
		   slot->memory_arena_head_segment != 0u) {
		return false;
	}
	if (!slot_environment_source_has_valid_encoding(slot))
		return false;
	if (!slot_executable_name_has_valid_encoding(slot))
		return false;
	if (!slot_environment_has_valid_encoding(slot))
		return false;
	if (!slot_image_has_valid_encoding(slot))
		return false;
	if (!slot_target_has_valid_encoding(slot))
		return false;
	if (!slot_resident_has_valid_encoding(slot))
		return false;
	if (!slot_relocation_has_valid_encoding(slot))
		return false;
	if (!slot_parent_has_valid_encoding(slot))
		return false;
	if (!slot_inheritance_has_valid_encoding(slot))
		return false;
	if (!slot_psp_has_valid_encoding(slot))
		return false;
	if (!slot_publication_has_valid_encoding(slot))
		return false;
	if (!slot_journal_has_valid_encoding(slot))
		return false;
	if (slot->state ==
	    (uint8_t)DOS_EXEC_TRANSACTION_STATE_OBSERVER_ACQUIRING) {
		return snapshot_is_unused(&slot->parent_runtime) &&
		       slot->has_file_lease == 0u &&
		       slot->file_lease.value == 0u &&
		       slot->observer.state ==
			   (uint8_t)DOS_EXEC_OBSERVER_STATE_ACQUIRING &&
		       slot->observer.generation == 0u &&
		       slot->observer.adapter_identity ==
			   KERNEL_OBJECT_HANDLE_INVALID &&
		       slot->observer.context == KERNEL_OBJECT_HANDLE_INVALID;
	}
	if (request_is_process(&slot->request)) {
		bool snapshot_may_be_unused =
		    slot->state ==
			(uint8_t)
			    DOS_EXEC_TRANSACTION_STATE_OBSERVER_RELEASING ||
		    slot->state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_POISONED;

		if (!snapshot_is_unused(&slot->parent_runtime) &&
		    (slot->parent_runtime.generation == 0u ||
		     slot->parent_runtime.runtime_identity !=
			 slot->runtime_identity ||
		     slot->parent_runtime.reserved != 0u))
			return false;
		if (!snapshot_may_be_unused &&
		    snapshot_is_unused(&slot->parent_runtime))
			return false;
	} else if (!snapshot_is_unused(&slot->parent_runtime)) {
		return false;
	}
	if (slot->observer.adapter_identity !=
		slot->observer_adapter_identity ||
	    slot->observer.context != slot->observer_adapter_context ||
	    slot->observer.generation == 0u)
		return false;
	if (slot->has_file_lease != 0u &&
	    (slot->file_lease.value == 0u ||
	     slot->file_lease.value == KERNEL_OBJECT_HANDLE_INVALID))
		return false;
	if (slot->has_file_lease == 0u && slot->file_lease.value != 0u)
		return false;
	if ((slot->state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_OBSERVED ||
	     slot->state ==
		 (uint8_t)DOS_EXEC_TRANSACTION_STATE_NAME_READING ||
	     slot->state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_FILE_OPENING ||
	     slot->state ==
		 (uint8_t)DOS_EXEC_TRANSACTION_STATE_OBSERVER_RELEASING) &&
	    slot->has_file_lease != 0u)
		return false;
	if ((slot->state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_FILE_OPEN ||
	     slot->state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_FILE_PROBING ||
	     slot->state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_FILE_PROBED ||
	     slot->state ==
		 (uint8_t)DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_READING ||
	     slot->state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_ENV_READY ||
	     slot->state ==
		 (uint8_t)DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_SCANNING ||
	     slot->state ==
		 (uint8_t)DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_PLANNED ||
	     slot->state ==
		 (uint8_t)DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_ALLOCATING ||
	     slot->state ==
		 (uint8_t)DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_LEASED ||
	     slot->state ==
		 (uint8_t)DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_BUILDING ||
	     slot->state == (uint8_t)
			      DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_BLOCK_READY ||
	     slot->state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_IMAGE_READING ||
	     slot->state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_IMAGE_READY ||
	     state_is_load_block_interval(slot->state) ||
	     slot->state == (uint8_t)
			      DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_LEASE_ABORTING ||
	     slot->state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_FILE_CLOSING ||
	     slot->state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_FILE_CLOSED ||
	     slot->state ==
		 (uint8_t)DOS_EXEC_TRANSACTION_STATE_ABORT_FILE_CLOSING) &&
	    slot->has_file_lease == 0u)
		return false;
	if (slot->state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_ABORTING &&
	    slot->has_file_lease == 0u)
		return false;
	observer_state = slot->observer.state;
	if (slot->state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_ABORTED) {
		if (slot->has_file_lease != 0u || slot->file_lease.value != 0u)
			return false;
		return observer_state ==
		       (uint8_t)DOS_EXEC_OBSERVER_STATE_RELEASED;
	}
	if (slot->state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_POISONED)
		return observer_state ==
		       (uint8_t)DOS_EXEC_OBSERVER_STATE_POISONED;
	if (slot->state ==
	    (uint8_t)DOS_EXEC_TRANSACTION_STATE_OBSERVER_RELEASING)
		return observer_state ==
		       (uint8_t)DOS_EXEC_OBSERVER_STATE_RELEASING;
	if (slot->state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_PUBLISHED)
		return observer_state ==
		       (uint8_t)DOS_EXEC_OBSERVER_STATE_RELEASED;
	return observer_state == (uint8_t)DOS_EXEC_OBSERVER_STATE_HELD;
}

static enum dos_exec_transaction_status
validate_table(const struct dos_exec_transaction_table *table)
{
	size_t live_slots = 0u;
	size_t index;

	if (table == NULL || table->constructed != 1u ||
	    table->initialized > 1u || table->poisoned > 1u ||
	    !bytes_are_zero(table->reserved, ARRAY_SIZE(table->reserved)))
		return DOS_EXEC_TRANSACTION_INVALID_ARGUMENT;
	if (table->poisoned != 0u)
		return DOS_EXEC_TRANSACTION_POISONED;
	if (table->initialized == 0u)
		return DOS_EXEC_TRANSACTION_NOT_INITIALIZED;
	if (!identity_is_valid(table->coordinator_identity))
		return DOS_EXEC_TRANSACTION_INVALID_STATE;
	for (index = 0u; index < DOS_EXEC_TRANSACTION_SLOT_COUNT; ++index) {
		if (table->slots[index].state ==
			(uint8_t)DOS_EXEC_TRANSACTION_STATE_POISONED ||
		    table->slots[index].observer.state ==
			(uint8_t)DOS_EXEC_OBSERVER_STATE_POISONED)
			return DOS_EXEC_TRANSACTION_POISONED;
		if (!slot_has_valid_encoding(&table->slots[index]))
			return DOS_EXEC_TRANSACTION_INVALID_STATE;
		if (table->slots[index].state !=
			(uint8_t)DOS_EXEC_TRANSACTION_STATE_VACANT &&
		    table->slots[index].coordinator_identity !=
			table->coordinator_identity)
			return DOS_EXEC_TRANSACTION_INVALID_STATE;
		if (table->slots[index].state !=
		    (uint8_t)DOS_EXEC_TRANSACTION_STATE_VACANT) {
			++live_slots;
			if (live_slots > 1u)
				return DOS_EXEC_TRANSACTION_INVALID_STATE;
		}
	}
	return DOS_EXEC_TRANSACTION_OK;
}

static bool
services_are_complete(const struct dos_exec_transaction_services *services,
		      uint8_t subfunction)
{
	if (services == NULL || services->file_leases == NULL ||
	    !file_ops_are_complete(services->file_ops) ||
	    !observer_ops_are_complete(services->observer_ops) ||
	    !machine_is_usable(services->machine) ||
	    !identity_is_valid(services->coordinator_identity) ||
	    !identity_is_valid(services->machine_identity) ||
	    !identity_is_valid(services->file_lease_table_identity) ||
	    services->file_leases->identity !=
		services->file_lease_table_identity ||
	    services->file_adapter_context == KERNEL_OBJECT_HANDLE_INVALID ||
	    services->observer_adapter_context == KERNEL_OBJECT_HANDLE_INVALID ||
	    (!backend_services_are_absent(services) &&
	     !backend_services_are_complete(services)))
		return false;
	if (subfunction == DOS_EXEC_OVERLAY)
		return true;
	return services->runtime != NULL &&
	       identity_is_valid(services->runtime->identity) &&
	       sft_ops_are_complete(services->sft_ops) &&
	       drive_ops_are_complete(services->drive_ops) &&
	       identity_is_valid(services->sft_adapter_identity) &&
	       services->sft_ops->identity == services->sft_adapter_identity &&
	       services->sft_adapter_context != KERNEL_OBJECT_HANDLE_INVALID &&
	       identity_is_valid(services->drive_adapter_identity) &&
	       services->drive_ops->identity == services->drive_adapter_identity &&
	       services->drive_adapter_context != KERNEL_OBJECT_HANDLE_INVALID &&
	       memory_services_are_complete(services);
}

static enum dos_exec_transaction_status
validate_services(const struct dos_exec_transaction_table *table,
		  const struct dos_exec_transaction_services *services,
		  uint8_t subfunction)
{
	if (!services_are_complete(services, subfunction))
		return DOS_EXEC_TRANSACTION_INVALID_ARGUMENT;
	if (services->coordinator_identity != table->coordinator_identity)
		return DOS_EXEC_TRANSACTION_BINDING_MISMATCH;
	return DOS_EXEC_TRANSACTION_OK;
}

static void clear_slot_binding(struct dos_exec_transaction_slot *slot)
{
	size_t index;

	slot->coordinator_identity = KERNEL_OBJECT_HANDLE_INVALID;
	slot->machine_identity = KERNEL_OBJECT_HANDLE_INVALID;
	slot->machine_context = KERNEL_OBJECT_HANDLE_INVALID;
	slot->machine_address_limit = 0u;
	slot->file_adapter_identity = KERNEL_OBJECT_HANDLE_INVALID;
	slot->file_adapter_context = KERNEL_OBJECT_HANDLE_INVALID;
	slot->file_lease_table_identity = KERNEL_OBJECT_HANDLE_INVALID;
	slot->runtime_identity = KERNEL_OBJECT_HANDLE_INVALID;
	slot->sft_adapter_identity = KERNEL_OBJECT_HANDLE_INVALID;
	slot->sft_adapter_context = KERNEL_OBJECT_HANDLE_INVALID;
	slot->drive_adapter_identity = KERNEL_OBJECT_HANDLE_INVALID;
	slot->drive_adapter_context = KERNEL_OBJECT_HANDLE_INVALID;
	slot->observer_adapter_identity = KERNEL_OBJECT_HANDLE_INVALID;
	slot->observer_adapter_context = KERNEL_OBJECT_HANDLE_INVALID;
	slot->memory_arena_identity = KERNEL_OBJECT_HANDLE_INVALID;
	slot->memory_arena_generation = 0u;
	slot->memory_lease_table_identity =
	    DOS_MEMORY_LEASE_TABLE_IDENTITY_INVALID;
	slot->memory_arena_head_segment = 0u;
	clear_transaction_environment(&slot->environment);
	clear_transaction_image(&slot->image);
	clear_transaction_target(&slot->target);
	clear_transaction_resident(&slot->resident);
	clear_transaction_relocation(&slot->relocation);
	clear_transaction_parent(&slot->parent);
	clear_transaction_inheritance(&slot->inheritance);
	clear_transaction_psp(&slot->psp);
	clear_transaction_publication(&slot->publication);
	slot->journal =
	    (struct dos_exec_journal)DOS_EXEC_JOURNAL_INITIALIZER;
	slot->parent_runtime.generation = 0u;
	slot->parent_runtime.runtime_identity = 0u;
	slot->parent_runtime.dta.offset = 0u;
	slot->parent_runtime.dta.segment = 0u;
	slot->parent_runtime.current_psp = 0u;
	slot->parent_runtime.reserved = 0u;
	slot->environment_source.source.offset = 0u;
	slot->environment_source.source.segment = 0u;
	slot->environment_source.parent_psp = 0u;
	slot->environment_source.subfunction = 0u;
	slot->environment_source.kind = 0u;
	slot->executable_name.source.offset = 0u;
	slot->executable_name.source.segment = 0u;
	slot->executable_name.bytes_including_nul = 0u;
	slot->executable_name.reserved = 0u;
	slot->file_lease.value = 0u;
	slot->request.executable_name.offset = 0u;
	slot->request.executable_name.segment = 0u;
	slot->request.parameter_block.offset = 0u;
	slot->request.parameter_block.segment = 0u;
	slot->request.subfunction = 0u;
	for (index = 0u; index < ARRAY_SIZE(slot->request.reserved); ++index)
		slot->request.reserved[index] = 0u;
	slot->observer =
	    (struct dos_exec_observer)DOS_EXEC_OBSERVER_INITIALIZER;
	slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_VACANT;
	slot->has_file_lease = 0u;
	slot->machine_a20_enabled = 0u;
	for (index = 0u; index < ARRAY_SIZE(slot->reserved); ++index)
		slot->reserved[index] = 0u;
	for (index = 0u; index < ARRAY_SIZE(slot->reserved_extension); ++index)
		slot->reserved_extension[index] = 0u;
}

static void initialize_slot(struct dos_exec_transaction_slot *slot)
{
	slot->generation = 0u;
	clear_slot_binding(slot);
}

static void vacate_slot(struct dos_exec_transaction_slot *slot)
{
	uint64_t generation = slot->generation;

	clear_slot_binding(slot);
	slot->generation = generation;
}

static struct dos_exec_transaction_handle make_handle(uint32_t slot_index,
						      uint64_t generation)
{
	struct dos_exec_transaction_handle handle;

	handle.value = (generation << DOS_EXEC_TRANSACTION_SLOT_BITS) |
		       ((uint64_t)slot_index + 1u);
	return handle;
}

static enum dos_exec_transaction_status
decode_handle(struct dos_exec_transaction_handle handle, uint32_t *slot_index,
	      uint64_t *generation)
{
	uint64_t encoded_slot;
	uint64_t decoded_generation;

	if (slot_index == NULL || generation == NULL || handle.value == 0u ||
	    handle.value == KERNEL_OBJECT_HANDLE_INVALID)
		return DOS_EXEC_TRANSACTION_STALE_HANDLE;
	encoded_slot = handle.value & DOS_EXEC_TRANSACTION_SLOT_MASK;
	decoded_generation = handle.value >> DOS_EXEC_TRANSACTION_SLOT_BITS;
	if (encoded_slot == 0u ||
	    encoded_slot > DOS_EXEC_TRANSACTION_SLOT_COUNT ||
	    decoded_generation == 0u ||
	    decoded_generation > DOS_EXEC_TRANSACTION_GENERATION_MAX)
		return DOS_EXEC_TRANSACTION_STALE_HANDLE;
	*slot_index = (uint32_t)(encoded_slot - 1u);
	*generation = decoded_generation;
	return DOS_EXEC_TRANSACTION_OK;
}

static enum dos_exec_transaction_status
find_slot(struct dos_exec_transaction_table *table,
	  struct dos_exec_transaction_handle handle, uint32_t *slot_index)
{
	uint64_t generation;
	enum dos_exec_transaction_status status;

	if (slot_index == NULL)
		return DOS_EXEC_TRANSACTION_INVALID_ARGUMENT;
	status = validate_table(table);
	if (status != DOS_EXEC_TRANSACTION_OK)
		return status;
	status = decode_handle(handle, slot_index, &generation);
	if (status != DOS_EXEC_TRANSACTION_OK)
		return status;
	if (table->slots[*slot_index].generation != generation ||
	    table->slots[*slot_index].state ==
		(uint8_t)DOS_EXEC_TRANSACTION_STATE_VACANT)
		return DOS_EXEC_TRANSACTION_STALE_HANDLE;
	return DOS_EXEC_TRANSACTION_OK;
}

static bool
slot_matches_services(const struct dos_exec_transaction_slot *slot,
		      const struct dos_exec_transaction_services *services)
{
	if (slot->coordinator_identity != services->coordinator_identity ||
	    slot->machine_identity != services->machine_identity ||
	    slot->machine_context != services->machine->context ||
	    slot->machine_address_limit != services->machine->address_limit ||
	    slot->machine_a20_enabled !=
		(uint8_t)services->machine->a20_enabled ||
	    slot->file_adapter_identity != services->file_ops->identity ||
	    slot->file_adapter_context != services->file_adapter_context ||
	    slot->file_lease_table_identity !=
		services->file_lease_table_identity ||
	    services->file_leases->identity !=
		services->file_lease_table_identity ||
	    slot->observer_adapter_identity !=
		services->observer_ops->identity ||
	    slot->observer_adapter_context !=
		services->observer_adapter_context)
		return false;
	if (!request_is_process(&slot->request))
		return true;
	if (slot->publication.has_backend_session != 0u &&
	    (!backend_services_are_complete(services) ||
	     slot->publication.backend_session_table_identity !=
		 services->backend_session_table_identity ||
	     slot->publication.backend_adapter_identity !=
		 services->backend_ops->identity ||
	     slot->publication.backend_adapter_context !=
		 services->backend_adapter_context))
		return false;
	return slot->runtime_identity == services->runtime->identity &&
	       services->sft_ops->identity == slot->sft_adapter_identity &&
	       slot->sft_adapter_identity == services->sft_adapter_identity &&
	       slot->sft_adapter_context == services->sft_adapter_context &&
	       services->drive_ops->identity == slot->drive_adapter_identity &&
	       slot->drive_adapter_identity == services->drive_adapter_identity &&
	       slot->drive_adapter_context == services->drive_adapter_context &&
	       slot->memory_arena_identity == services->memory_arena->identity &&
	       slot->memory_arena_generation == services->memory_arena->generation &&
	       slot->memory_arena_head_segment ==
		   services->memory_arena->head_segment &&
	       slot->memory_lease_table_identity ==
		   services->memory_lease_table_identity &&
	       services->memory_leases->lifetime_identity ==
		   services->memory_lease_table_identity;
}

static enum dos_exec_transaction_status
find_bound_slot(struct dos_exec_transaction_table *table,
		struct dos_exec_transaction_handle handle,
		const struct dos_exec_transaction_services *services,
		uint32_t *slot_index)
{
	enum dos_exec_transaction_status status;

	status = find_slot(table, handle, slot_index);
	if (status != DOS_EXEC_TRANSACTION_OK)
		return status;
	status = validate_services(
	    table, services, table->slots[*slot_index].request.subfunction);
	if (status != DOS_EXEC_TRANSACTION_OK)
		return status;
	if (!slot_matches_services(&table->slots[*slot_index], services))
		return DOS_EXEC_TRANSACTION_BINDING_MISMATCH;
	return DOS_EXEC_TRANSACTION_OK;
}

static enum dos_exec_transaction_status
reserve_slot(struct dos_exec_transaction_table *table,
	     const struct dos_exec_transaction_services *services,
	     const struct dos_exec_transaction_request *request,
	     uint32_t *slot_index)
{
	bool every_generation_exhausted = true;
	enum dos_exec_journal_status journal_status;
	uint32_t index;

	/* DOS $Exec is serialized: one table owns at most one live request. */
	for (index = 0u; index < DOS_EXEC_TRANSACTION_SLOT_COUNT; ++index) {
		if (table->slots[index].state !=
		    (uint8_t)DOS_EXEC_TRANSACTION_STATE_VACANT)
			return DOS_EXEC_TRANSACTION_BUSY;
	}
	for (index = 0u; index < DOS_EXEC_TRANSACTION_SLOT_COUNT; ++index) {
		struct dos_exec_transaction_slot *slot = &table->slots[index];

		if (slot->generation < DOS_EXEC_TRANSACTION_GENERATION_MAX)
			every_generation_exhausted = false;
		if (slot->state != (uint8_t)DOS_EXEC_TRANSACTION_STATE_VACANT ||
		    slot->generation >= DOS_EXEC_TRANSACTION_GENERATION_MAX)
			continue;
		vacate_slot(slot);
		slot->generation++;
		slot->coordinator_identity = services->coordinator_identity;
		slot->machine_identity = services->machine_identity;
		slot->machine_context = services->machine->context;
		slot->machine_address_limit = services->machine->address_limit;
		slot->file_adapter_identity = services->file_ops->identity;
		slot->file_adapter_context = services->file_adapter_context;
		slot->file_lease_table_identity =
		    services->file_lease_table_identity;
		if (request_is_process(request)) {
			slot->runtime_identity = services->runtime->identity;
			slot->sft_adapter_identity =
			    services->sft_adapter_identity;
			slot->sft_adapter_context =
			    services->sft_adapter_context;
			slot->drive_adapter_identity =
			    services->drive_adapter_identity;
			slot->drive_adapter_context =
			    services->drive_adapter_context;
			slot->memory_arena_identity =
			    services->memory_arena->identity;
			slot->memory_arena_generation =
			    services->memory_arena->generation;
			slot->memory_arena_head_segment =
			    services->memory_arena->head_segment;
			slot->memory_lease_table_identity =
			    services->memory_lease_table_identity;
			journal_status = dos_exec_journal_initialize(
			    &slot->journal, services->machine_identity,
			    services->machine);
			if (journal_status != DOS_EXEC_JOURNAL_OK) {
				vacate_slot(slot);
				return DOS_EXEC_TRANSACTION_INVALID_STATE;
			}
		}
		slot->observer_adapter_identity =
		    services->observer_ops->identity;
		slot->observer_adapter_context =
		    services->observer_adapter_context;
		slot->request = *request;
		slot->machine_a20_enabled =
		    (uint8_t)services->machine->a20_enabled;
		*slot_index = index;
		return DOS_EXEC_TRANSACTION_OK;
	}
	return every_generation_exhausted
		   ? DOS_EXEC_TRANSACTION_GENERATION_EXHAUSTED
		   : DOS_EXEC_TRANSACTION_NO_SLOT;
}

static enum dos_exec_transaction_status
poison_transaction(struct dos_exec_transaction_table *table,
		   struct dos_exec_transaction_slot *slot,
		   const struct dos_exec_transaction_services *services)
{
	enum dos_exec_observer_status observer_status = DOS_EXEC_OBSERVER_OK;

	/* Publish fail-closed state before quarantine, whose callback may
	 * reenter. */
	table->poisoned = 1u;
	slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_POISONED;
	if (slot->observer.state == (uint8_t)DOS_EXEC_OBSERVER_STATE_HELD) {
		observer_status = dos_exec_observer_poison(
		    &slot->observer, services->observer_ops,
		    services->observer_adapter_context);
	}
	(void)observer_status;
	return DOS_EXEC_TRANSACTION_POISONED;
}

enum dos_exec_transaction_status
dos_exec_transaction_table_construct(struct dos_exec_transaction_table *table)
{
	uint32_t index;

	if (table == NULL)
		return DOS_EXEC_TRANSACTION_INVALID_ARGUMENT;
	for (index = 0u; index < DOS_EXEC_TRANSACTION_SLOT_COUNT; ++index)
		initialize_slot(&table->slots[index]);
	table->coordinator_identity = KERNEL_OBJECT_HANDLE_INVALID;
	table->initialized = 0u;
	table->constructed = 1u;
	table->poisoned = 0u;
	for (index = 0u; index < ARRAY_SIZE(table->reserved); ++index)
		table->reserved[index] = 0u;
	return DOS_EXEC_TRANSACTION_OK;
}

enum dos_exec_transaction_status dos_exec_transaction_table_initialize(
    struct dos_exec_transaction_table *table,
    kernel_object_handle_t coordinator_identity)
{
	uint32_t index;

	if (table == NULL || table->constructed != 1u ||
	    !identity_is_valid(coordinator_identity) ||
	    !bytes_are_zero(table->reserved, ARRAY_SIZE(table->reserved)))
		return DOS_EXEC_TRANSACTION_INVALID_ARGUMENT;
	if (table->poisoned != 0u)
		return DOS_EXEC_TRANSACTION_POISONED;
	if (table->initialized != 0u)
		return DOS_EXEC_TRANSACTION_INVALID_STATE;
	for (index = 0u; index < DOS_EXEC_TRANSACTION_SLOT_COUNT; ++index)
		initialize_slot(&table->slots[index]);
	table->coordinator_identity = coordinator_identity;
	table->initialized = 1u;
	return DOS_EXEC_TRANSACTION_OK;
}

bool dos_exec_transaction_table_is_drained(
    const struct dos_exec_transaction_table *table)
{
	uint32_t index;

	if (validate_table(table) != DOS_EXEC_TRANSACTION_OK)
		return false;
	for (index = 0u; index < DOS_EXEC_TRANSACTION_SLOT_COUNT; ++index) {
		if (table->slots[index].state !=
		    (uint8_t)DOS_EXEC_TRANSACTION_STATE_VACANT)
			return false;
	}
	return true;
}

enum dos_exec_transaction_status
dos_exec_transaction_begin(struct dos_exec_transaction_table *table,
			   const struct dos_exec_transaction_services *services,
			   const struct dos_exec_transaction_request *request,
			   struct dos_exec_transaction_handle *handle)
{
	struct dos_process_runtime_snapshot snapshot;
	struct dos_exec_transaction_handle prepared_handle;
	struct dos_exec_transaction_slot *slot;
	enum dos_exec_observer_status observer_status;
	enum dos_process_runtime_status runtime_status;
	enum dos_exec_transaction_status status;
	uint32_t slot_index;

	if (handle == NULL || !request_has_valid_encoding(request))
		return DOS_EXEC_TRANSACTION_INVALID_ARGUMENT;
	status = validate_table(table);
	if (status != DOS_EXEC_TRANSACTION_OK)
		return status;
	status = validate_services(table, services, request->subfunction);
	if (status != DOS_EXEC_TRANSACTION_OK)
		return status;
	if (request_is_process(request) &&
	    services->memory_arena->machine_poisoned != 0u)
		return DOS_EXEC_TRANSACTION_POISONED;
	status = reserve_slot(table, services, request, &slot_index);
	if (status != DOS_EXEC_TRANSACTION_OK)
		return status;
	slot = &table->slots[slot_index];
	prepared_handle = make_handle(slot_index, slot->generation);
	if (dos_exec_observer_construct(&slot->observer) !=
	    DOS_EXEC_OBSERVER_OK) {
		vacate_slot(slot);
		return DOS_EXEC_TRANSACTION_INVALID_STATE;
	}
	slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_OBSERVER_ACQUIRING;
	observer_status =
	    dos_exec_observer_acquire(&slot->observer, services->observer_ops,
				      services->observer_adapter_context);
	if (observer_status == DOS_EXEC_OBSERVER_BUSY) {
		vacate_slot(slot);
		return DOS_EXEC_TRANSACTION_OBSERVER_BUSY;
	}
	if (observer_status == DOS_EXEC_OBSERVER_POISONED)
		return poison_transaction(table, slot, services);
	if (observer_status != DOS_EXEC_OBSERVER_OK) {
		vacate_slot(slot);
		return DOS_EXEC_TRANSACTION_OBSERVER_FAULT;
	}
	if (!request_is_process(request)) {
		slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_OBSERVED;
		*handle = prepared_handle;
		return DOS_EXEC_TRANSACTION_OK;
	}
	runtime_status =
	    dos_process_runtime_snapshot(services->runtime, &snapshot);
	if (runtime_status != DOS_PROCESS_RUNTIME_OK) {
		if (runtime_status == DOS_PROCESS_RUNTIME_POISONED)
			return poison_transaction(table, slot, services);
		slot->state =
		    (uint8_t)DOS_EXEC_TRANSACTION_STATE_OBSERVER_RELEASING;
		observer_status = dos_exec_observer_release(
		    &slot->observer, services->observer_ops,
		    services->observer_adapter_context);
		if (observer_status != DOS_EXEC_OBSERVER_OK)
			return poison_transaction(table, slot, services);
		vacate_slot(slot);
		return DOS_EXEC_TRANSACTION_RUNTIME_NOT_READY;
	}
	slot->parent_runtime = snapshot;
	slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_OBSERVED;
	*handle = prepared_handle;
	return DOS_EXEC_TRANSACTION_OK;
}

enum dos_exec_transaction_status
dos_exec_transaction_open(struct dos_exec_transaction_table *table,
			  struct dos_exec_transaction_handle handle,
			  const struct dos_exec_transaction_services *services,
			  uint8_t *executable_name_scratch,
			  size_t executable_name_scratch_capacity,
			  uint32_t *failure_detail)
{
	struct dos_exec_name_plan prepared_name;
	struct dos_memory_owner_name_patch prepared_owner_name;
	struct dos_exec_file_lease_handle file_handle = {0u};
	struct dos_exec_transaction_slot *slot;
	enum dos_exec_file_lease_status file_status;
	enum dos_exec_name_status name_status;
	enum dos_exec_transaction_status status;
	uintptr_t scratch_end;
	size_t accessed_capacity;
	uint32_t detail = 0u;
	uint32_t slot_index;

	if (executable_name_scratch == NULL ||
	    executable_name_scratch_capacity == 0u ||
	    failure_detail == NULL)
		return DOS_EXEC_TRANSACTION_INVALID_ARGUMENT;
	accessed_capacity =
	    executable_name_scratch_capacity < DOS_EXEC_NAME_SCAN_LIMIT
		? executable_name_scratch_capacity
		: DOS_EXEC_NAME_SCAN_LIMIT;
	if (check_add_overflow((uintptr_t)executable_name_scratch,
			       (uintptr_t)accessed_capacity, &scratch_end))
		return DOS_EXEC_TRANSACTION_INVALID_ARGUMENT;
	(void)scratch_end;
	status = find_bound_slot(table, handle, services, &slot_index);
	if (status != DOS_EXEC_TRANSACTION_OK)
		return status;
	slot = &table->slots[slot_index];
	if (slot->state != (uint8_t)DOS_EXEC_TRANSACTION_STATE_OBSERVED)
		return DOS_EXEC_TRANSACTION_INVALID_STATE;

	slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_NAME_READING;
	name_status = dos_exec_name_read_guest(
	    services->machine, slot->request.executable_name,
	    executable_name_scratch, executable_name_scratch_capacity,
	    &prepared_name);
	if (name_status != DOS_EXEC_NAME_OK) {
		slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_FAILED;
		return DOS_EXEC_TRANSACTION_NAME_FAULT;
	}
	name_status = dos_exec_name_build_owner_patch(
	    executable_name_scratch, executable_name_scratch_capacity,
	    (size_t)prepared_name.bytes_including_nul, &prepared_owner_name);
	if (name_status != DOS_EXEC_NAME_OK) {
		slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_FAILED;
		return DOS_EXEC_TRANSACTION_NAME_FAULT;
	}

	slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_FILE_OPENING;
	file_status = dos_exec_file_lease_acquire(
	    services->file_leases, services->file_ops,
	    services->file_adapter_context, executable_name_scratch,
	    (size_t)prepared_name.bytes_including_nul, &file_handle, &detail);
	if (file_status == DOS_EXEC_FILE_LEASE_OK) {
		/* No callback remains: publish the pair, then publish its state. */
		copy_executable_name(&slot->executable_name, &prepared_name);
		copy_owner_name_patch(&slot->publication.owner_name,
				      &prepared_owner_name);
		slot->publication.has_owner_name = 1u;
		slot->file_lease = file_handle;
		slot->has_file_lease = 1u;
		slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_FILE_OPEN;
		*failure_detail = detail;
		return DOS_EXEC_TRANSACTION_OK;
	}
	if (file_status == DOS_EXEC_FILE_LEASE_OPEN_FAILED) {
		slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_FAILED;
		*failure_detail = detail;
		return DOS_EXEC_TRANSACTION_OPEN_FAILED;
	}
	if (file_status == DOS_EXEC_FILE_LEASE_POISONED)
		return poison_transaction(table, slot, services);
	slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_FAILED;
	if (file_status == DOS_EXEC_FILE_LEASE_NO_SLOT ||
	    file_status == DOS_EXEC_FILE_LEASE_GENERATION_EXHAUSTED ||
	    file_status == DOS_EXEC_FILE_LEASE_NOT_INITIALIZED)
		return DOS_EXEC_TRANSACTION_FILE_LEASE_UNAVAILABLE;
	return DOS_EXEC_TRANSACTION_FILE_LEASE_FAULT;
}

enum dos_exec_transaction_status
dos_exec_transaction_probe(struct dos_exec_transaction_table *table,
			   struct dos_exec_transaction_handle handle,
			   const struct dos_exec_transaction_services *services,
			   uint8_t *is_device, uint32_t *failure_detail)
{
	struct dos_exec_transaction_slot *slot;
	enum dos_exec_file_lease_status file_status;
	enum dos_exec_transaction_status status;
	uint32_t detail = 0u;
	uint32_t slot_index;
	uint8_t device = 0xffu;

	if (is_device == NULL || failure_detail == NULL)
		return DOS_EXEC_TRANSACTION_INVALID_ARGUMENT;
	status = find_bound_slot(table, handle, services, &slot_index);
	if (status != DOS_EXEC_TRANSACTION_OK)
		return status;
	slot = &table->slots[slot_index];
	if (slot->state != (uint8_t)DOS_EXEC_TRANSACTION_STATE_FILE_OPEN ||
	    slot->has_file_lease == 0u)
		return DOS_EXEC_TRANSACTION_INVALID_STATE;

	slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_FILE_PROBING;
	file_status = dos_exec_file_lease_probe_device(
	    services->file_leases, slot->file_lease, services->file_ops,
	    services->file_adapter_context, &device, &detail);
	if (file_status == DOS_EXEC_FILE_LEASE_OK) {
		*is_device = device;
		*failure_detail = detail;
		if (device != 0u) {
			slot->state =
			    (uint8_t)DOS_EXEC_TRANSACTION_STATE_FAILED;
			return DOS_EXEC_TRANSACTION_IS_DEVICE;
		}
		slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_FILE_PROBED;
		return DOS_EXEC_TRANSACTION_OK;
	}
	if (file_status == DOS_EXEC_FILE_LEASE_PROBE_FAILED) {
		slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_FAILED;
		*failure_detail = detail;
		return DOS_EXEC_TRANSACTION_PROBE_FAILED;
	}
	if (file_status == DOS_EXEC_FILE_LEASE_POISONED)
		return poison_transaction(table, slot, services);
	slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_FAILED;
	return DOS_EXEC_TRANSACTION_FILE_LEASE_FAULT;
}

enum dos_exec_transaction_status dos_exec_transaction_select_environment(
    struct dos_exec_transaction_table *table,
    struct dos_exec_transaction_handle handle,
    const struct dos_exec_transaction_services *services,
    struct dos_exec_environment_source_plan *environment_source)
{
	struct dos_exec_environment_source_plan prepared;
	struct dos_exec_transaction_slot *slot;
	struct dos_process_far_address parameter_block;
	enum dos_exec_parameter_status parameter_status;
	enum dos_process_runtime_status runtime_status;
	enum dos_exec_transaction_status status;
	uint32_t slot_index;

	if (environment_source == NULL)
		return DOS_EXEC_TRANSACTION_INVALID_ARGUMENT;
	status = find_bound_slot(table, handle, services, &slot_index);
	if (status != DOS_EXEC_TRANSACTION_OK)
		return status;
	slot = &table->slots[slot_index];
	if (slot->state !=
		(uint8_t)DOS_EXEC_TRANSACTION_STATE_FILE_PROBED ||
	    !request_is_process(&slot->request))
		return DOS_EXEC_TRANSACTION_INVALID_STATE;

	/* No guest callback may run before this expected-parent check. */
	runtime_status = dos_process_runtime_preflight_exec(
	    services->runtime, &slot->parent_runtime);
	if (runtime_status == DOS_PROCESS_RUNTIME_POISONED)
		return poison_transaction(table, slot, services);
	if (runtime_status != DOS_PROCESS_RUNTIME_OK) {
		slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_FAILED;
		return DOS_EXEC_TRANSACTION_RUNTIME_NOT_READY;
	}

	parameter_block.segment = slot->request.parameter_block.segment;
	parameter_block.offset = slot->request.parameter_block.offset;
	slot->state =
	    (uint8_t)DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_READING;
	parameter_status = dos_exec_parameter_decode_environment_source(
	    services->machine, slot->request.subfunction, parameter_block,
	    slot->parent_runtime.current_psp, &prepared);
	if (parameter_status != DOS_EXEC_PARAMETER_OK) {
		if (parameter_status == DOS_EXEC_PARAMETER_MACHINE_FAULT) {
			slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_FAILED;
			return DOS_EXEC_TRANSACTION_ENVIRONMENT_FAULT;
		}
		return poison_transaction(table, slot, services);
	}
	if (!dos_exec_environment_source_plan_has_valid_encoding(&prepared) ||
	    prepared.subfunction != slot->request.subfunction ||
	    !environment_source_matches_parent(&prepared,
				       &slot->parent_runtime))
		return poison_transaction(table, slot, services);

	copy_environment_source(&slot->environment_source, &prepared);
	slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_ENV_READY;
	copy_environment_source(environment_source, &prepared);
	return DOS_EXEC_TRANSACTION_OK;
}

static enum dos_exec_transaction_status poison_memory_domain(
    struct dos_exec_transaction_table *table,
    struct dos_exec_transaction_slot *slot,
    const struct dos_exec_transaction_services *services)
{
	enum dos_memory_status memory_status;

	memory_status = dos_memory_arena_poison(services->memory_arena);
	(void)memory_status;
	return poison_transaction(table, slot, services);
}

static enum dos_exec_transaction_status environment_failure(
    struct dos_exec_transaction_table *table,
    struct dos_exec_transaction_slot *slot,
    const struct dos_exec_transaction_services *services,
    enum dos_environment_status environment_status)
{
	slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_FAILED;
	switch (environment_status) {
	case DOS_ENVIRONMENT_BAD_SOURCE:
	case DOS_ENVIRONMENT_NAME_NOT_TERMINATED:
	case DOS_ENVIRONMENT_STALE_PLAN:
		return DOS_EXEC_TRANSACTION_BAD_ENVIRONMENT;
	case DOS_ENVIRONMENT_RANGE_OVERFLOW:
		return DOS_EXEC_TRANSACTION_ENVIRONMENT_RANGE_OVERFLOW;
	case DOS_ENVIRONMENT_SOURCE_FAULT:
	case DOS_ENVIRONMENT_TARGET_FAULT:
		return DOS_EXEC_TRANSACTION_ENVIRONMENT_FAULT;
	case DOS_ENVIRONMENT_TARGET_POISONED:
		return poison_memory_domain(table, slot, services);
	case DOS_ENVIRONMENT_OK:
	case DOS_ENVIRONMENT_INVALID_ARGUMENT:
	default:
		return poison_memory_domain(table, slot, services);
	}
}

static enum dos_exec_transaction_status memory_acquire_failure(
    struct dos_exec_transaction_table *table,
    struct dos_exec_transaction_slot *slot,
    const struct dos_exec_transaction_services *services,
    enum dos_memory_lease_status memory_status)
{
	slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_FAILED;
	switch (memory_status) {
	case DOS_MEMORY_LEASE_NO_SLOT:
	case DOS_MEMORY_LEASE_GENERATION_EXHAUSTED:
		return DOS_EXEC_TRANSACTION_MEMORY_LEASE_UNAVAILABLE;
	case DOS_MEMORY_LEASE_NOT_ENOUGH_MEMORY:
		return DOS_EXEC_TRANSACTION_NOT_ENOUGH_MEMORY;
	case DOS_MEMORY_LEASE_MACHINE_FAULT:
		return DOS_EXEC_TRANSACTION_MEMORY_FAULT;
	case DOS_MEMORY_LEASE_MACHINE_POISONED:
	case DOS_MEMORY_LEASE_INVALID_BLOCK:
	case DOS_MEMORY_LEASE_ARENA_DAMAGED:
	case DOS_MEMORY_LEASE_INVALID_ARGUMENT:
	case DOS_MEMORY_LEASE_STALE_HANDLE:
	case DOS_MEMORY_LEASE_OWNER_MISMATCH:
	case DOS_MEMORY_LEASE_CONTEXT_MISMATCH:
	case DOS_MEMORY_LEASE_IDENTITY_MISMATCH:
	case DOS_MEMORY_LEASE_INVALID_STATE:
	case DOS_MEMORY_LEASE_OK:
	default:
		return poison_memory_domain(table, slot, services);
	}
}

static bool memory_view_matches_environment(
    const struct dos_exec_transaction_slot *slot,
    const struct dos_memory_lease_view *view)
{
	return view->handle.value == slot->environment.lease.handle.value &&
	       view->machine_context == slot->machine_context &&
	       view->arena_identity == slot->memory_arena_identity &&
	       view->arena_generation == slot->memory_arena_generation &&
	       view->guest_segment == slot->environment.lease.guest_segment &&
	       view->paragraphs == slot->environment.plan.paragraphs &&
	       view->owner == slot->parent_runtime.current_psp &&
	       view->reserved == 0u;
}

static bool memory_view_matches_target(
    const struct dos_exec_transaction_slot *slot,
    const struct dos_memory_lease_view *view)
{
	return view->handle.value == slot->target.lease.handle.value &&
	       view->machine_context == slot->machine_context &&
	       view->arena_identity == slot->memory_arena_identity &&
	       view->arena_generation == slot->memory_arena_generation &&
	       view->guest_segment == slot->target.lease.guest_segment &&
	       view->paragraphs == slot->target.allocation.block_paragraphs &&
	       view->owner == slot->parent_runtime.current_psp &&
	       view->reserved == 0u;
}

enum dos_exec_transaction_status dos_exec_transaction_prepare_environment(
    struct dos_exec_transaction_table *table,
    struct dos_exec_transaction_handle handle,
    const struct dos_exec_transaction_services *services,
    struct dos_exec_transaction_environment *result)
{
	struct dos_exec_transaction_environment prepared;
	struct dos_exec_transaction_slot *slot;
	struct dos_memory_lease_receipt receipt;
	struct dos_memory_lease_view view;
	struct dos_far_pointer16 destination;
	enum dos_process_runtime_status runtime_status;
	enum dos_exec_transaction_status status;
	enum dos_environment_status environment_status;
	enum dos_memory_lease_status memory_status;
	uint32_t slot_index;

	if (result == NULL)
		return DOS_EXEC_TRANSACTION_INVALID_ARGUMENT;
	status = find_bound_slot(table, handle, services, &slot_index);
	if (status != DOS_EXEC_TRANSACTION_OK)
		return status;
	slot = &table->slots[slot_index];
	if (slot->state != (uint8_t)DOS_EXEC_TRANSACTION_STATE_ENV_READY ||
	    !request_is_process(&slot->request))
		return DOS_EXEC_TRANSACTION_INVALID_STATE;

	/* The expected-parent check is pure and precedes every guest callback. */
	runtime_status = dos_process_runtime_preflight_exec(
	    services->runtime, &slot->parent_runtime);
	if (runtime_status == DOS_PROCESS_RUNTIME_POISONED)
		return poison_transaction(table, slot, services);
	if (runtime_status != DOS_PROCESS_RUNTIME_OK) {
		slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_FAILED;
		return DOS_EXEC_TRANSACTION_RUNTIME_NOT_READY;
	}
	if (services->memory_arena->machine_poisoned != 0u)
		return poison_memory_domain(table, slot, services);

	clear_transaction_environment(&prepared);
	if (slot->environment_source.kind == DOS_EXEC_ENVIRONMENT_SOURCE_NONE) {
		slot->state = (uint8_t)
		    DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_BLOCK_READY;
		copy_transaction_environment(result, &prepared);
		return DOS_EXEC_TRANSACTION_OK;
	}
	if (slot->environment_source.kind !=
		DOS_EXEC_ENVIRONMENT_SOURCE_PARAMETER &&
	    slot->environment_source.kind != DOS_EXEC_ENVIRONMENT_SOURCE_PARENT)
		return poison_memory_domain(table, slot, services);

	/*
	 * MCB owner zero means free.  Reproducing an internal CurrentPDB=0
	 * allocation would publish an apparently free live block, so fail before
	 * scanning or allocation.  Normal DOS process execution always has a PSP.
	 */
	if (slot->parent_runtime.current_psp == 0u) {
		slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_FAILED;
		return DOS_EXEC_TRANSACTION_RUNTIME_NOT_READY;
	}

	slot->state =
	    (uint8_t)DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_SCANNING;
	environment_status = dos_environment_plan_create(
	    services->machine, slot->environment_source.source,
	    &slot->executable_name, &prepared.plan);
	if (environment_status != DOS_ENVIRONMENT_OK)
		return environment_failure(table, slot, services,
				   environment_status);
	copy_environment_plan(&slot->environment.plan, &prepared.plan);
	slot->state =
	    (uint8_t)DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_PLANNED;

	slot->state =
	    (uint8_t)DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_ALLOCATING;
	memory_status = dos_memory_lease_acquire_unnamed(
	    services->memory_leases, services->memory_arena, services->machine,
	    slot->parent_runtime.current_psp, slot->environment.plan.paragraphs,
	    &receipt);
	if (memory_status != DOS_MEMORY_LEASE_OK)
		return memory_acquire_failure(table, slot, services, memory_status);

	copy_environment_receipt(&slot->environment.lease, &receipt);
	slot->environment.has_block = 1u;
	slot->state =
	    (uint8_t)DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_LEASED;
	memory_status = dos_memory_lease_resolve_active(
	    services->memory_leases, services->memory_arena, services->machine,
	    slot->environment.lease.handle, slot->parent_runtime.current_psp,
	    &view);
	if (memory_status != DOS_MEMORY_LEASE_OK ||
	    !memory_view_matches_environment(slot, &view))
		return poison_memory_domain(table, slot, services);

	destination.offset = 0u;
	destination.segment = slot->environment.lease.guest_segment;
	slot->state =
	    (uint8_t)DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_BUILDING;
	environment_status = dos_environment_build(
	    services->machine, &slot->environment.plan, destination);
	if (environment_status != DOS_ENVIRONMENT_OK)
		return environment_failure(table, slot, services,
				   environment_status);

	slot->state = (uint8_t)
	    DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_BLOCK_READY;
	copy_transaction_environment(result, &slot->environment);
	return DOS_EXEC_TRANSACTION_OK;
}

static enum dos_exec_transaction_status image_inspection_failure(
    struct dos_exec_transaction_table *table,
    struct dos_exec_transaction_slot *slot,
    const struct dos_exec_transaction_services *services,
    enum dos_loader_status loader_status)
{
	slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_FAILED;
	switch (loader_status) {
	case DOS_LOADER_IMAGE_TOO_LARGE:
		return DOS_EXEC_TRANSACTION_IMAGE_TOO_LARGE;
	case DOS_LOADER_EMPTY_IMAGE:
	case DOS_LOADER_IO_ERROR:
	case DOS_LOADER_SHORT_READ:
	case DOS_LOADER_BAD_FORMAT:
	case DOS_LOADER_RANGE_OVERFLOW:
		return DOS_EXEC_TRANSACTION_BAD_IMAGE;
	case DOS_LOADER_INVALID_ARGUMENT:
	case DOS_LOADER_OK:
	default:
		return poison_transaction(table, slot, services);
	}
}

enum dos_exec_transaction_status dos_exec_transaction_inspect_image(
    struct dos_exec_transaction_table *table,
    struct dos_exec_transaction_handle handle,
    const struct dos_exec_transaction_services *services,
    struct dos_load_plan *result)
{
	struct dos_load_plan prepared;
	struct dos_exec_transaction_slot *slot;
	struct dos_image_reader reader;
	enum dos_exec_file_lease_status file_status;
	enum dos_process_runtime_status runtime_status;
	enum dos_exec_transaction_status status;
	enum dos_loader_status loader_status;
	enum dos_load_target_kind target;
	uint32_t slot_index;

	if (result == NULL)
		return DOS_EXEC_TRANSACTION_INVALID_ARGUMENT;
	status = find_bound_slot(table, handle, services, &slot_index);
	if (status != DOS_EXEC_TRANSACTION_OK)
		return status;
	slot = &table->slots[slot_index];
	if (request_is_process(&slot->request)) {
		if (slot->state != (uint8_t)
				      DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_BLOCK_READY)
			return DOS_EXEC_TRANSACTION_INVALID_STATE;
		runtime_status = dos_process_runtime_preflight_exec(
		    services->runtime, &slot->parent_runtime);
		if (runtime_status == DOS_PROCESS_RUNTIME_POISONED)
			return poison_transaction(table, slot, services);
		if (runtime_status != DOS_PROCESS_RUNTIME_OK) {
			slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_FAILED;
			return DOS_EXEC_TRANSACTION_RUNTIME_NOT_READY;
		}
		if (services->memory_arena->machine_poisoned != 0u)
			return poison_memory_domain(table, slot, services);
		target = DOS_LOAD_TARGET_PROCESS;
	} else {
		if (slot->state !=
		    (uint8_t)DOS_EXEC_TRANSACTION_STATE_FILE_PROBED)
			return DOS_EXEC_TRANSACTION_INVALID_STATE;
		target = DOS_LOAD_TARGET_OVERLAY;
	}

	file_status = dos_exec_file_lease_resolve_reader(
	    services->file_leases, slot->file_lease, services->file_ops,
	    services->file_adapter_context, &reader);
	if (file_status != DOS_EXEC_FILE_LEASE_OK)
		return poison_transaction(table, slot, services);

	slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_IMAGE_READING;
	loader_status = dos_loader_inspect_target(&reader, target, &prepared);
	if (loader_status != DOS_LOADER_OK)
		return image_inspection_failure(table, slot, services,
					loader_status);
	if (!dos_load_plan_has_inspected_encoding(&prepared) ||
	    prepared.file_size != reader.size ||
	    prepared.target_kind != (uint8_t)target)
		return poison_transaction(table, slot, services);

	copy_load_plan(&slot->image.plan, &prepared);
	slot->image.has_plan = 1u;
	slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_IMAGE_READY;
	copy_load_plan(result, &prepared);
	return DOS_EXEC_TRANSACTION_OK;
}

static enum dos_exec_transaction_status load_query_failure(
    struct dos_exec_transaction_table *table,
    struct dos_exec_transaction_slot *slot,
    const struct dos_exec_transaction_services *services,
    enum dos_memory_status memory_status)
{
	slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_FAILED;
	switch (memory_status) {
	case DOS_MEMORY_MACHINE_FAULT:
		return DOS_EXEC_TRANSACTION_MEMORY_FAULT;
	case DOS_MEMORY_MACHINE_POISONED:
	case DOS_MEMORY_ARENA_DAMAGED:
	case DOS_MEMORY_INVALID_ARGUMENT:
	case DOS_MEMORY_INVALID_BLOCK:
	case DOS_MEMORY_NOT_ENOUGH_MEMORY:
	case DOS_MEMORY_OWNER_MISMATCH:
	case DOS_MEMORY_IDENTITY_MISMATCH:
	case DOS_MEMORY_GENERATION_EXHAUSTED:
	case DOS_MEMORY_OK:
	default:
		return poison_memory_domain(table, slot, services);
	}
}

static enum dos_exec_transaction_status allocation_plan_failure(
    struct dos_exec_transaction_table *table,
    struct dos_exec_transaction_slot *slot,
    const struct dos_exec_transaction_services *services,
    enum dos_process_status process_status)
{
	slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_FAILED;
	switch (process_status) {
	case DOS_PROCESS_NOT_ENOUGH_MEMORY:
		return DOS_EXEC_TRANSACTION_NOT_ENOUGH_MEMORY;
	case DOS_PROCESS_WRONG_IMAGE_FORMAT:
	case DOS_PROCESS_RANGE_OVERFLOW:
	case DOS_PROCESS_BAD_IMAGE_RANGE:
		return DOS_EXEC_TRANSACTION_BAD_IMAGE;
	case DOS_PROCESS_MACHINE_FAULT:
		return DOS_EXEC_TRANSACTION_MEMORY_FAULT;
	case DOS_PROCESS_MACHINE_POISONED:
		return poison_memory_domain(table, slot, services);
	case DOS_PROCESS_INVALID_ARGUMENT:
	case DOS_PROCESS_INVALID_PSP:
	case DOS_PROCESS_BAD_COMMAND_TAIL:
	case DOS_PROCESS_COMMAND_TAIL_TOO_LONG:
	case DOS_PROCESS_STALE_SNAPSHOT:
	case DOS_PROCESS_OK:
	default:
		return poison_transaction(table, slot, services);
	}
}

enum dos_exec_transaction_status dos_exec_transaction_prepare_target(
    struct dos_exec_transaction_table *table,
    struct dos_exec_transaction_handle handle,
    const struct dos_exec_transaction_services *services,
    struct dos_exec_transaction_target *result)
{
	struct dos_exec_transaction_target prepared;
	struct dos_exec_transaction_slot *slot;
	struct dos_memory_lease_receipt receipt;
	struct dos_memory_lease_view view;
	enum dos_process_runtime_status runtime_status;
	enum dos_exec_transaction_status status;
	enum dos_memory_lease_status lease_status;
	enum dos_memory_status memory_status;
	enum dos_process_status process_status;
	uint16_t maximum_available;
	uint32_t slot_index;

	if (result == NULL)
		return DOS_EXEC_TRANSACTION_INVALID_ARGUMENT;
	status = find_bound_slot(table, handle, services, &slot_index);
	if (status != DOS_EXEC_TRANSACTION_OK)
		return status;
	slot = &table->slots[slot_index];
	if (slot->state != (uint8_t)DOS_EXEC_TRANSACTION_STATE_IMAGE_READY ||
	    !request_is_process(&slot->request))
		return DOS_EXEC_TRANSACTION_INVALID_STATE;

	runtime_status = dos_process_runtime_preflight_exec(
	    services->runtime, &slot->parent_runtime);
	if (runtime_status == DOS_PROCESS_RUNTIME_POISONED)
		return poison_transaction(table, slot, services);
	if (runtime_status != DOS_PROCESS_RUNTIME_OK ||
	    slot->parent_runtime.current_psp == 0u) {
		slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_FAILED;
		return DOS_EXEC_TRANSACTION_RUNTIME_NOT_READY;
	}
	if (services->memory_arena->machine_poisoned != 0u)
		return poison_memory_domain(table, slot, services);

	clear_transaction_target(&prepared);
	maximum_available = 0u;
	slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_LOAD_QUERYING;
	memory_status = dos_memory_query_maximum_checked(
	    services->memory_arena, services->machine, &maximum_available);
	if (memory_status != DOS_MEMORY_OK)
		return load_query_failure(table, slot, services, memory_status);

	process_status = dos_process_select_allocation(
	    &slot->image.plan, maximum_available, &prepared.allocation);
	if (process_status != DOS_PROCESS_OK)
		return allocation_plan_failure(table, slot, services,
				       process_status);
	if (!allocation_plan_matches_image(&prepared.allocation,
					   &slot->image.plan))
		return poison_transaction(table, slot, services);
	copy_allocation_plan(&slot->target.allocation, &prepared.allocation);
	slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_LOAD_PLANNED;

	slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_LOAD_ALLOCATING;
	lease_status = dos_memory_lease_acquire_unnamed(
	    services->memory_leases, services->memory_arena, services->machine,
	    slot->parent_runtime.current_psp,
	    slot->target.allocation.block_paragraphs, &receipt);
	if (lease_status != DOS_MEMORY_LEASE_OK)
		return memory_acquire_failure(table, slot, services, lease_status);
	copy_environment_receipt(&slot->target.lease, &receipt);
	slot->target.has_load_block = 1u;
	slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_LOAD_LEASED;
	if (receipt.paragraphs != slot->target.allocation.block_paragraphs ||
	    receipt.maximum_available !=
		slot->target.allocation.available_paragraphs ||
	    receipt.reserved != 0u)
		return poison_memory_domain(table, slot, services);
	lease_status = dos_memory_lease_resolve_active(
	    services->memory_leases, services->memory_arena, services->machine,
	    slot->target.lease.handle, slot->parent_runtime.current_psp, &view);
	if (lease_status != DOS_MEMORY_LEASE_OK ||
	    !memory_view_matches_target(slot, &view))
		return poison_memory_domain(table, slot, services);

	slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_TARGET_READY;
	copy_transaction_target(result, &slot->target);
	return DOS_EXEC_TRANSACTION_OK;
}

static enum dos_exec_transaction_status resident_load_failure(
    struct dos_exec_transaction_table *table,
    struct dos_exec_transaction_slot *slot,
    const struct dos_exec_transaction_services *services,
    enum dos_image_load_status image_status)
{
	slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_FAILED;
	switch (image_status) {
	case DOS_IMAGE_LOAD_FILE_RANGE_OVERFLOW:
	case DOS_IMAGE_LOAD_BAD_FILE_RANGE:
	case DOS_IMAGE_LOAD_IMAGE_IO_ERROR:
	case DOS_IMAGE_LOAD_IMAGE_SHORT_READ:
	case DOS_IMAGE_LOAD_WRONG_IMAGE_FORMAT:
		return DOS_EXEC_TRANSACTION_BAD_IMAGE;
	case DOS_IMAGE_LOAD_MACHINE_FAULT:
		return DOS_EXEC_TRANSACTION_MEMORY_FAULT;
	case DOS_IMAGE_LOAD_MACHINE_POISONED:
		return poison_memory_domain(table, slot, services);
	case DOS_IMAGE_LOAD_INVALID_ARGUMENT:
	case DOS_IMAGE_LOAD_STALE_PLAN:
	case DOS_IMAGE_LOAD_BAD_LEASE:
	case DOS_IMAGE_LOAD_BAD_RESIDENT_RANGE:
	case DOS_IMAGE_LOAD_OK:
	default:
		return poison_memory_domain(table, slot, services);
	}
}

enum dos_exec_transaction_status dos_exec_transaction_load_resident(
    struct dos_exec_transaction_table *table,
    struct dos_exec_transaction_handle handle,
    const struct dos_exec_transaction_services *services,
    struct dos_exec_transaction_resident *result)
{
	struct dos_exec_transaction_resident prepared;
	struct dos_exec_transaction_slot *slot;
	struct dos_image_reader reader;
	struct dos_memory_lease_view view;
	enum dos_process_launch_mode launch_mode;
	enum dos_process_runtime_status runtime_status;
	enum dos_exec_transaction_status status;
	enum dos_exec_file_lease_status file_status;
	enum dos_memory_lease_status lease_status;
	enum dos_process_status process_status;
	enum dos_image_load_status image_status;
	uint32_t slot_index;

	if (result == NULL)
		return DOS_EXEC_TRANSACTION_INVALID_ARGUMENT;
	status = find_bound_slot(table, handle, services, &slot_index);
	if (status != DOS_EXEC_TRANSACTION_OK)
		return status;
	slot = &table->slots[slot_index];
	if (slot->state != (uint8_t)DOS_EXEC_TRANSACTION_STATE_TARGET_READY ||
	    !request_is_process(&slot->request))
		return DOS_EXEC_TRANSACTION_INVALID_STATE;

	/* Pure lifetime checks precede every arena, file or guest callback. */
	runtime_status = dos_process_runtime_preflight_exec(
	    services->runtime, &slot->parent_runtime);
	if (runtime_status == DOS_PROCESS_RUNTIME_POISONED)
		return poison_transaction(table, slot, services);
	if (runtime_status != DOS_PROCESS_RUNTIME_OK ||
	    slot->parent_runtime.current_psp == 0u) {
		slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_FAILED;
		return DOS_EXEC_TRANSACTION_RUNTIME_NOT_READY;
	}
	if (services->memory_arena->machine_poisoned != 0u)
		return poison_memory_domain(table, slot, services);

	clear_transaction_resident(&prepared);
	launch_mode = slot_launch_mode(slot);
	prepared.format = slot->image.plan.format;
	if (prepared.format == (uint8_t)DOS_IMAGE_COM) {
		process_status = dos_process_plan_com(
		    &slot->image.plan, &slot->target.allocation,
		    slot->target.lease.guest_segment, launch_mode, 0u,
		    &prepared.process.com);
	} else if (prepared.format == (uint8_t)DOS_IMAGE_MZ) {
		process_status = dos_process_plan_mz(
		    &slot->image.plan, &slot->target.allocation,
		    slot->target.lease.guest_segment, launch_mode, 0u,
		    &prepared.process.mz);
	} else {
		return poison_transaction(table, slot, services);
	}
	if (process_status != DOS_PROCESS_OK)
		return allocation_plan_failure(table, slot, services,
				       process_status);
	prepared.has_process_plan = 1u;
	copy_transaction_resident(&slot->resident, &prepared);
	slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_PROCESS_PLANNED;
	if (!resident_process_plan_matches_slot(slot))
		return poison_transaction(table, slot, services);

	file_status = dos_exec_file_lease_resolve_reader(
	    services->file_leases, slot->file_lease, services->file_ops,
	    services->file_adapter_context, &reader);
	if (file_status != DOS_EXEC_FILE_LEASE_OK)
		return poison_transaction(table, slot, services);
	slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_RESIDENT_LOADING;
	lease_status = dos_memory_lease_resolve_active(
	    services->memory_leases, services->memory_arena, services->machine,
	    slot->target.lease.handle, slot->parent_runtime.current_psp, &view);
	if (lease_status != DOS_MEMORY_LEASE_OK ||
	    !memory_view_matches_target(slot, &view))
		return poison_memory_domain(table, slot, services);

	if (slot->resident.format == (uint8_t)DOS_IMAGE_COM)
		image_status = dos_image_load_com_resident(
		    &reader, services->machine, &slot->image.plan,
		    &slot->resident.process.com, &view, &prepared.load);
	else
		image_status = dos_image_load_mz_resident(
		    &reader, services->machine, &slot->image.plan,
		    &slot->resident.process.mz, &view, &prepared.load);
	if (image_status != DOS_IMAGE_LOAD_OK)
		return resident_load_failure(table, slot, services,
				     image_status);

	copy_image_load_result(&slot->resident.load, &prepared.load);
	slot->resident.has_resident = 1u;
	if (!resident_load_result_matches_slot(slot))
		return poison_memory_domain(table, slot, services);
	slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_RESIDENT_READY;
	copy_transaction_resident(result, &slot->resident);
	return DOS_EXEC_TRANSACTION_OK;
}

static enum dos_exec_transaction_status relocation_failure(
    struct dos_exec_transaction_table *table,
    struct dos_exec_transaction_slot *slot,
    const struct dos_exec_transaction_services *services,
    enum dos_relocator_status relocator_status)
{
	slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_FAILED;
	switch (relocator_status) {
	case DOS_RELOCATOR_FILE_RANGE_OVERFLOW:
	case DOS_RELOCATOR_BAD_FILE_RANGE:
	case DOS_RELOCATOR_IMAGE_IO_ERROR:
	case DOS_RELOCATOR_IMAGE_SHORT_READ:
	case DOS_RELOCATOR_BAD_TARGET_OFFSET:
	case DOS_RELOCATOR_TARGET_OUTSIDE_RESIDENT:
		return DOS_EXEC_TRANSACTION_BAD_IMAGE;
	case DOS_RELOCATOR_MACHINE_FAULT:
		return DOS_EXEC_TRANSACTION_MEMORY_FAULT;
	case DOS_RELOCATOR_MACHINE_POISONED:
		return poison_memory_domain(table, slot, services);
	case DOS_RELOCATOR_INVALID_ARGUMENT:
	case DOS_RELOCATOR_BAD_RESIDENT_RANGE:
	case DOS_RELOCATOR_OK:
	default:
		return poison_memory_domain(table, slot, services);
	}
}

enum dos_exec_transaction_status dos_exec_transaction_relocate_resident(
    struct dos_exec_transaction_table *table,
    struct dos_exec_transaction_handle handle,
    const struct dos_exec_transaction_services *services,
    struct dos_exec_transaction_relocation *result)
{
	struct dos_exec_transaction_relocation prepared;
	struct dos_exec_transaction_slot *slot;
	struct dos_image_reader reader;
	struct dos_memory_lease_view view;
	enum dos_process_runtime_status runtime_status;
	enum dos_exec_transaction_status status;
	enum dos_exec_file_lease_status file_status;
	enum dos_memory_lease_status lease_status;
	enum dos_relocator_status relocator_status;
	uint32_t slot_index;

	if (result == NULL)
		return DOS_EXEC_TRANSACTION_INVALID_ARGUMENT;
	status = find_bound_slot(table, handle, services, &slot_index);
	if (status != DOS_EXEC_TRANSACTION_OK)
		return status;
	slot = &table->slots[slot_index];
	if (slot->state !=
		(uint8_t)DOS_EXEC_TRANSACTION_STATE_RESIDENT_READY ||
	    !request_is_process(&slot->request))
		return DOS_EXEC_TRANSACTION_INVALID_STATE;

	runtime_status = dos_process_runtime_preflight_exec(
	    services->runtime, &slot->parent_runtime);
	if (runtime_status == DOS_PROCESS_RUNTIME_POISONED)
		return poison_transaction(table, slot, services);
	if (runtime_status != DOS_PROCESS_RUNTIME_OK) {
		slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_FAILED;
		return DOS_EXEC_TRANSACTION_RUNTIME_NOT_READY;
	}
	if (services->memory_arena->machine_poisoned != 0u)
		return poison_memory_domain(table, slot, services);

	clear_transaction_relocation(&prepared);
	if (slot->resident.format == (uint8_t)DOS_IMAGE_COM) {
		slot->state =
		    (uint8_t)DOS_EXEC_TRANSACTION_STATE_IMAGE_PRIVATE;
		copy_transaction_relocation(result, &prepared);
		return DOS_EXEC_TRANSACTION_OK;
	}
	if (slot->resident.format != (uint8_t)DOS_IMAGE_MZ)
		return poison_transaction(table, slot, services);

	prepared.request.relocation_table_offset =
	    slot->resident.process.mz.relocation_table_offset;
	prepared.request.resident_size = slot->resident.load.resident_bytes;
	prepared.request.resident_linear_address =
	    slot->resident.process.mz.load_linear_address;
	prepared.request.relocation_count =
	    slot->resident.process.mz.relocation_count;
	prepared.request.relocation_factor =
	    slot->resident.process.mz.relocation_factor;
	prepared.applicable = 1u;
	prepared.has_request = 1u;
	copy_transaction_relocation(&slot->relocation, &prepared);
	slot->state =
	    (uint8_t)DOS_EXEC_TRANSACTION_STATE_RELOCATION_PLANNED;
	if (!relocation_request_matches_slot(slot))
		return poison_transaction(table, slot, services);

	file_status = dos_exec_file_lease_resolve_reader(
	    services->file_leases, slot->file_lease, services->file_ops,
	    services->file_adapter_context, &reader);
	if (file_status != DOS_EXEC_FILE_LEASE_OK)
		return poison_transaction(table, slot, services);
	slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_RELOCATING;
	lease_status = dos_memory_lease_resolve_active(
	    services->memory_leases, services->memory_arena, services->machine,
	    slot->target.lease.handle, slot->parent_runtime.current_psp, &view);
	if (lease_status != DOS_MEMORY_LEASE_OK ||
	    !memory_view_matches_target(slot, &view))
		return poison_memory_domain(table, slot, services);
	relocator_status = dos_relocator_apply(
	    &reader, services->machine, &slot->relocation.request,
	    &prepared.result);
	if (relocator_status != DOS_RELOCATOR_OK)
		return relocation_failure(table, slot, services,
				  relocator_status);

	slot->relocation.result.validated_entries =
	    prepared.result.validated_entries;
	slot->relocation.result.applied_entries =
	    prepared.result.applied_entries;
	slot->relocation.applied = 1u;
	slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_IMAGE_PRIVATE;
	copy_transaction_relocation(result, &slot->relocation);
	return DOS_EXEC_TRANSACTION_OK;
}

enum dos_exec_transaction_status
dos_exec_transaction_close(struct dos_exec_transaction_table *table,
			   struct dos_exec_transaction_handle handle,
			   const struct dos_exec_transaction_services *services)
{
	struct dos_exec_transaction_slot *slot;
	enum dos_exec_file_lease_status file_status;
	enum dos_exec_transaction_status status;
	bool process_private;
	uint32_t slot_index;

	status = find_bound_slot(table, handle, services, &slot_index);
	if (status != DOS_EXEC_TRANSACTION_OK)
		return status;
	slot = &table->slots[slot_index];
	process_private =
	    request_is_process(&slot->request) &&
	    slot->state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_IMAGE_PRIVATE;
	if ((slot->state !=
		 (uint8_t)DOS_EXEC_TRANSACTION_STATE_FILE_PROBED &&
	     !process_private) ||
	    slot->has_file_lease == 0u)
		return DOS_EXEC_TRANSACTION_INVALID_STATE;
	slot->state =
	    process_private
		? (uint8_t)DOS_EXEC_TRANSACTION_STATE_PROCESS_FILE_CLOSING
		: (uint8_t)DOS_EXEC_TRANSACTION_STATE_FILE_CLOSING;
	file_status = dos_exec_file_lease_close(
	    services->file_leases, slot->file_lease, services->file_ops,
	    services->file_adapter_context);
	if (file_status == DOS_EXEC_FILE_LEASE_OK) {
		slot->state =
		    process_private
			? (uint8_t)
			      DOS_EXEC_TRANSACTION_STATE_PROCESS_FILE_CLOSED
			: (uint8_t)DOS_EXEC_TRANSACTION_STATE_FILE_CLOSED;
		return DOS_EXEC_TRANSACTION_OK;
	}
	if (file_status == DOS_EXEC_FILE_LEASE_CLOSE_RETAINED) {
		slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_FAILED;
		return DOS_EXEC_TRANSACTION_CLOSE_RETAINED;
	}
	if (file_status == DOS_EXEC_FILE_LEASE_POISONED)
		return poison_transaction(table, slot, services);
	slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_FAILED;
	return DOS_EXEC_TRANSACTION_FILE_LEASE_FAULT;
}

enum dos_exec_transaction_status dos_exec_transaction_capture_parent(
    struct dos_exec_transaction_table *table,
    struct dos_exec_transaction_handle handle,
    const struct dos_exec_transaction_services *services,
    struct dos_exec_transaction_parent *result)
{
	struct dos_exec_transaction_parent prepared;
	struct dos_exec_transaction_slot *slot;
	enum dos_process_runtime_status runtime_status;
	enum dos_process_status process_status;
	enum dos_exec_transaction_status status;
	uint32_t slot_index;

	if (result == NULL)
		return DOS_EXEC_TRANSACTION_INVALID_ARGUMENT;
	status = find_bound_slot(table, handle, services, &slot_index);
	if (status != DOS_EXEC_TRANSACTION_OK)
		return status;
	slot = &table->slots[slot_index];
	if (slot->state !=
		(uint8_t)DOS_EXEC_TRANSACTION_STATE_PROCESS_FILE_CLOSED ||
	    !request_is_process(&slot->request))
		return DOS_EXEC_TRANSACTION_INVALID_STATE;
	runtime_status = dos_process_runtime_preflight_exec(
	    services->runtime, &slot->parent_runtime);
	if (runtime_status == DOS_PROCESS_RUNTIME_POISONED)
		return poison_transaction(table, slot, services);
	if (runtime_status != DOS_PROCESS_RUNTIME_OK) {
		slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_FAILED;
		return DOS_EXEC_TRANSACTION_RUNTIME_NOT_READY;
	}

	clear_transaction_parent(&prepared);
	slot->state =
	    (uint8_t)DOS_EXEC_TRANSACTION_STATE_PARENT_SNAPSHOTTING;
	process_status = dos_process_capture_parent_snapshot(
	    services->machine, slot->machine_identity,
	    slot->parent_runtime.current_psp, &prepared.snapshot);
	if (process_status != DOS_PROCESS_OK) {
		slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_FAILED;
		if (process_status == DOS_PROCESS_MACHINE_FAULT ||
		    process_status == DOS_PROCESS_RANGE_OVERFLOW ||
		    process_status == DOS_PROCESS_INVALID_PSP)
			return DOS_EXEC_TRANSACTION_MEMORY_FAULT;
		return poison_transaction(table, slot, services);
	}
	prepared.has_snapshot = 1u;
	copy_transaction_parent(&slot->parent, &prepared);
	slot->state =
	    (uint8_t)DOS_EXEC_TRANSACTION_STATE_PARENT_SNAPSHOT_READY;
	if (!slot_parent_has_valid_encoding(slot))
		return poison_transaction(table, slot, services);
	copy_transaction_parent(result, &slot->parent);
	return DOS_EXEC_TRANSACTION_OK;
}

enum dos_exec_transaction_status dos_exec_transaction_prepare_inheritance(
    struct dos_exec_transaction_table *table,
    struct dos_exec_transaction_handle handle,
    const struct dos_exec_transaction_services *services,
    struct dos_exec_transaction_inheritance *result)
{
	struct dos_exec_transaction_inheritance prepared;
	struct dos_exec_transaction_slot *slot;
	enum dos_process_runtime_status runtime_status;
	enum dos_sft_batch_status sft_status;
	enum dos_exec_transaction_status status;
	uint32_t slot_index;

	if (result == NULL)
		return DOS_EXEC_TRANSACTION_INVALID_ARGUMENT;
	status = find_bound_slot(table, handle, services, &slot_index);
	if (status != DOS_EXEC_TRANSACTION_OK)
		return status;
	slot = &table->slots[slot_index];
	if (slot->state !=
		(uint8_t)DOS_EXEC_TRANSACTION_STATE_PARENT_SNAPSHOT_READY ||
	    slot->parent.has_snapshot != 1u)
		return DOS_EXEC_TRANSACTION_INVALID_STATE;
	runtime_status = dos_process_runtime_preflight_exec(
	    services->runtime, &slot->parent_runtime);
	if (runtime_status == DOS_PROCESS_RUNTIME_POISONED)
		return poison_transaction(table, slot, services);
	if (runtime_status != DOS_PROCESS_RUNTIME_OK) {
		slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_FAILED;
		return DOS_EXEC_TRANSACTION_RUNTIME_NOT_READY;
	}

	clear_transaction_inheritance(&prepared);
	slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_SFT_PREPARING;
	sft_status = dos_sft_batch_prepare(
	    services->sft_ops, services->sft_adapter_context,
	    &slot->parent.snapshot.parent_jft, &prepared.batch);
	if (sft_status != DOS_SFT_BATCH_OK) {
		if (sft_status == DOS_SFT_BATCH_POISONED) {
			prepared.has_batch = 1u;
			copy_transaction_inheritance(&slot->inheritance,
					     &prepared);
			return poison_memory_domain(table, slot, services);
		}
		slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_FAILED;
		if (sft_status == DOS_SFT_BATCH_NO_SLOT)
			return DOS_EXEC_TRANSACTION_SFT_UNAVAILABLE;
		if (sft_status == DOS_SFT_BATCH_ADAPTER_FAULT)
			return DOS_EXEC_TRANSACTION_SFT_FAULT;
		return poison_memory_domain(table, slot, services);
	}
	prepared.has_batch = 1u;
	sft_status = dos_sft_batch_copy_child_jft(prepared.batch,
					  &prepared.child_jft);
	if (sft_status != DOS_SFT_BATCH_OK) {
		copy_transaction_inheritance(&slot->inheritance, &prepared);
		return poison_memory_domain(table, slot, services);
	}
	prepared.has_child_jft = 1u;
	copy_transaction_inheritance(&slot->inheritance, &prepared);
	slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_SFT_RESERVED;
	copy_transaction_inheritance(result, &slot->inheritance);
	return DOS_EXEC_TRANSACTION_OK;
}

enum dos_exec_transaction_status dos_exec_transaction_prepare_psp(
    struct dos_exec_transaction_table *table,
    struct dos_exec_transaction_handle handle,
    const struct dos_exec_transaction_services *services,
    struct dos_process_far_address terminate_vector,
    struct dos_exec_transaction_psp *result)
{
	struct dos_exec_transaction_psp prepared;
	struct dos_exec_transaction_slot *slot;
	struct dos_process_psp_request request = {0};
	struct dos_process_far_address parameter_block;
	const uint8_t *parent_bytes;
	enum dos_process_runtime_status runtime_status;
	enum dos_exec_parameter_status parameter_status;
	enum dos_process_status process_status;
	enum dos_exec_transaction_status status;
	uint32_t slot_index;
	size_t control_offset =
	    __builtin_offsetof(struct dos_psp_prefix40, control_c_vector);
	size_t critical_offset =
	    __builtin_offsetof(struct dos_psp_prefix40, fatal_abort_vector);

	if (result == NULL)
		return DOS_EXEC_TRANSACTION_INVALID_ARGUMENT;
	status = find_bound_slot(table, handle, services, &slot_index);
	if (status != DOS_EXEC_TRANSACTION_OK)
		return status;
	slot = &table->slots[slot_index];
	if (slot->state != (uint8_t)DOS_EXEC_TRANSACTION_STATE_SFT_RESERVED ||
	    slot->inheritance.has_child_jft != 1u)
		return DOS_EXEC_TRANSACTION_INVALID_STATE;
	runtime_status = dos_process_runtime_preflight_exec(
	    services->runtime, &slot->parent_runtime);
	if (runtime_status == DOS_PROCESS_RUNTIME_POISONED)
		return poison_transaction(table, slot, services);
	if (runtime_status != DOS_PROCESS_RUNTIME_OK) {
		slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_FAILED;
		return DOS_EXEC_TRANSACTION_RUNTIME_NOT_READY;
	}

	parameter_block.segment = slot->request.parameter_block.segment;
	parameter_block.offset = slot->request.parameter_block.offset;
	slot->state =
	    (uint8_t)DOS_EXEC_TRANSACTION_STATE_PROCESS_PARAMETER_READING;
	parameter_status = dos_exec_parameter_read_first_fcb(
	    services->machine, parameter_block, &request.first_fcb_source);
	if (parameter_status == DOS_EXEC_PARAMETER_OK)
		parameter_status = dos_exec_parameter_read_second_fcb(
		    services->machine, parameter_block,
		    &request.second_fcb_source);
	if (parameter_status == DOS_EXEC_PARAMETER_OK)
		parameter_status = dos_exec_parameter_read_command_tail(
		    services->machine, parameter_block,
		    &request.command_tail_source);
	if (parameter_status != DOS_EXEC_PARAMETER_OK) {
		slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_FAILED;
		return parameter_status == DOS_EXEC_PARAMETER_MACHINE_FAULT
			   ? DOS_EXEC_TRANSACTION_MEMORY_FAULT
			   : poison_memory_domain(table, slot, services);
	}

	request.psp_segment = slot->target.lease.guest_segment;
	request.block_end_segment =
	    (uint16_t)(slot->target.lease.guest_segment +
		       slot->target.allocation.block_paragraphs);
	request.parent_psp_segment = slot->parent_runtime.current_psp;
	request.environment_segment = slot->environment.has_block != 0u
					  ? slot->environment.lease.guest_segment
					  : 0u;
	request.terminate_vector = terminate_vector;
	parent_bytes = slot->parent.snapshot.parent_psp;
	request.control_c_vector.offset =
	    transaction_read_le16(parent_bytes + control_offset);
	request.control_c_vector.segment =
	    transaction_read_le16(parent_bytes + control_offset + 2u);
	request.critical_error_vector.offset =
	    transaction_read_le16(parent_bytes + critical_offset);
	request.critical_error_vector.segment =
	    transaction_read_le16(parent_bytes + critical_offset + 2u);
	clear_transaction_psp(&prepared);
	slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_PSP_PREPARING;
	process_status = dos_process_prepare_psp_from_snapshot(
	    services->machine, slot->machine_identity, &slot->parent.snapshot,
	    &request, &slot->inheritance.child_jft, &prepared.image);
	if (process_status != DOS_PROCESS_OK) {
		slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_FAILED;
		if (process_status == DOS_PROCESS_MACHINE_FAULT ||
		    process_status == DOS_PROCESS_RANGE_OVERFLOW ||
		    process_status == DOS_PROCESS_BAD_IMAGE_RANGE)
			return DOS_EXEC_TRANSACTION_MEMORY_FAULT;
		return poison_memory_domain(table, slot, services);
	}
	prepared.has_image = 1u;
	copy_transaction_psp(&slot->psp, &prepared);
	slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_PSP_PREPARED;
	copy_transaction_psp(result, &slot->psp);
	return DOS_EXEC_TRANSACTION_OK;
}

static bool drive_visibility_status_is_valid(
    enum dos_exec_drive_visibility_status status)
{
	return status == DOS_EXEC_DRIVE_VISIBLE ||
	       status == DOS_EXEC_DRIVE_INVALID ||
	       status == DOS_EXEC_DRIVE_FAULT;
}

enum dos_exec_transaction_status dos_exec_transaction_finalize_initial_state(
    struct dos_exec_transaction_table *table,
    struct dos_exec_transaction_handle handle,
    const struct dos_exec_transaction_services *services,
    struct dos_exec_transaction_resident *result)
{
	struct dos_exec_transaction_resident prepared;
	struct dos_exec_transaction_slot *slot;
	enum dos_exec_drive_visibility_status first_status;
	enum dos_exec_drive_visibility_status second_status;
	enum dos_exec_transaction_status status;
	enum dos_process_status process_status;
	uint16_t initial_ax;
	uint32_t slot_index;
	uint8_t first_drive;
	uint8_t second_drive;

	if (result == NULL)
		return DOS_EXEC_TRANSACTION_INVALID_ARGUMENT;
	status = find_bound_slot(table, handle, services, &slot_index);
	if (status != DOS_EXEC_TRANSACTION_OK)
		return status;
	slot = &table->slots[slot_index];
	if (slot->state != (uint8_t)DOS_EXEC_TRANSACTION_STATE_PSP_PREPARED ||
	    slot->psp.has_image != 1u || slot->resident.has_process_plan != 1u ||
	    slot->resident.has_resident != 1u)
		return DOS_EXEC_TRANSACTION_INVALID_STATE;

	first_drive = slot->psp.image.bytes[DOS_PSP_FIRST_FCB_OFFSET];
	second_drive = slot->psp.image.bytes[DOS_PSP_SECOND_FCB_OFFSET];
	copy_transaction_resident(&prepared, &slot->resident);
	slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_DRIVE_RESOLVING;

	/* MS-DOS resolves BH (FCB2) before BL (FCB1). */
	second_status = services->drive_ops->resolve(
	    services->drive_adapter_context, second_drive);
	if (!drive_visibility_status_is_valid(second_status) ||
	    second_status == DOS_EXEC_DRIVE_FAULT) {
		slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_FAILED;
		return DOS_EXEC_TRANSACTION_DRIVE_FAULT;
	}
	first_status = services->drive_ops->resolve(
	    services->drive_adapter_context, first_drive);
	if (!drive_visibility_status_is_valid(first_status) ||
	    first_status == DOS_EXEC_DRIVE_FAULT) {
		slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_FAILED;
		return DOS_EXEC_TRANSACTION_DRIVE_FAULT;
	}

	initial_ax = second_status == DOS_EXEC_DRIVE_INVALID ? 0xff00u : 0u;
	if (first_status == DOS_EXEC_DRIVE_INVALID)
		initial_ax = (uint16_t)(initial_ax | 0x00ffu);
	if (prepared.format == (uint8_t)DOS_IMAGE_COM)
		process_status = dos_process_finalize_com_initial_ax(
		    &prepared.process.com, initial_ax);
	else if (prepared.format == (uint8_t)DOS_IMAGE_MZ)
		process_status = dos_process_finalize_mz_initial_ax(
		    &prepared.process.mz, initial_ax);
	else
		return poison_transaction(table, slot, services);
	if (process_status != DOS_PROCESS_OK)
		return poison_transaction(table, slot, services);

	copy_transaction_resident(&slot->resident, &prepared);
	slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_INITIAL_STATE_READY;
	copy_transaction_resident(result, &slot->resident);
	return DOS_EXEC_TRANSACTION_OK;
}

static enum dos_exec_transaction_status process_memory_stage_failure(
    struct dos_exec_transaction_table *table,
    struct dos_exec_transaction_slot *slot,
    const struct dos_exec_transaction_services *services,
    enum dos_exec_journal_status journal_status)
{
	if (journal_status == DOS_EXEC_JOURNAL_POISONED)
		return poison_memory_domain(table, slot, services);
	if (journal_status != DOS_EXEC_JOURNAL_MACHINE_FAULT)
		return poison_transaction(table, slot, services);

	slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_JOURNAL_ABORTING;
	journal_status = dos_exec_journal_abort(
	    &slot->journal, slot->machine_identity, services->machine);
	if (journal_status != DOS_EXEC_JOURNAL_OK)
		return poison_memory_domain(table, slot, services);
	slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_FAILED;
	return DOS_EXEC_TRANSACTION_MEMORY_FAULT;
}

static enum dos_exec_journal_status stage_process_stack_word(
    struct dos_exec_transaction_slot *slot,
    const struct dos_exec_transaction_services *services, uint16_t segment,
    uint16_t offset, uint16_t value)
{
	uint8_t bytes[2];

	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
	return dos_exec_journal_stage_replace_far(
	    &slot->journal, slot->machine_identity, services->machine, segment,
	    offset, bytes, sizeof(bytes), sizeof(bytes));
}

enum dos_exec_transaction_status dos_exec_transaction_stage_process_memory(
    struct dos_exec_transaction_table *table,
    struct dos_exec_transaction_handle handle,
    const struct dos_exec_transaction_services *services)
{
	struct dos_exec_transaction_slot *slot;
	enum dos_exec_journal_status journal_status;
	enum dos_exec_transaction_status status;
	uint32_t slot_index;

	status = find_bound_slot(table, handle, services, &slot_index);
	if (status != DOS_EXEC_TRANSACTION_OK)
		return status;
	slot = &table->slots[slot_index];
	if (slot->state !=
		(uint8_t)DOS_EXEC_TRANSACTION_STATE_INITIAL_STATE_READY ||
	    slot->psp.has_image != 1u || slot->resident.has_process_plan != 1u ||
	    slot->resident.has_resident != 1u ||
	    slot->journal.state != (uint8_t)DOS_EXEC_JOURNAL_STATE_STAGING ||
	    slot->journal.record_count != 0u)
		return DOS_EXEC_TRANSACTION_INVALID_STATE;

	slot->state =
	    (uint8_t)DOS_EXEC_TRANSACTION_STATE_PROCESS_MEMORY_STAGING;
	journal_status = dos_exec_journal_stage_replace_far_span(
	    &slot->journal, slot->machine_identity, services->machine,
	    slot->psp.image.segment, 0u, slot->psp.image.bytes,
	    sizeof(slot->psp.image.bytes), sizeof(slot->psp.image.bytes));
	if (journal_status != DOS_EXEC_JOURNAL_OK)
		return process_memory_stage_failure(table, slot, services,
					    journal_status);

	if (slot->resident.format == (uint8_t)DOS_IMAGE_COM) {
		const struct dos_com_process_plan *process =
		    &slot->resident.process.com;

		journal_status = stage_process_stack_word(
		    slot, services, process->psp_segment,
		    process->stack_sentinel_offset,
		    process->stack_sentinel_value);
		if (journal_status == DOS_EXEC_JOURNAL_OK &&
		    process->launch_mode ==
			(uint8_t)DOS_PROCESS_LAUNCH_LOAD_ONLY)
			journal_status = stage_process_stack_word(
			    slot, services, process->initial_state.ss,
			    process->load_only_stack_pointer,
			    process->load_only_stack_value);
	} else if (slot->resident.format == (uint8_t)DOS_IMAGE_MZ) {
		const struct dos_mz_process_plan *process =
		    &slot->resident.process.mz;

		journal_status = DOS_EXEC_JOURNAL_OK;
		if (process->launch_mode ==
		    (uint8_t)DOS_PROCESS_LAUNCH_LOAD_ONLY)
			journal_status = stage_process_stack_word(
			    slot, services, process->initial_state.ss,
			    process->load_only_stack_pointer,
			    process->load_only_stack_value);
	} else {
		return poison_transaction(table, slot, services);
	}
	if (journal_status != DOS_EXEC_JOURNAL_OK)
		return process_memory_stage_failure(table, slot, services,
					    journal_status);

	slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_GLOBAL_STAGED;
	return DOS_EXEC_TRANSACTION_OK;
}

static enum dos_exec_transaction_status global_memory_lease_failure(
    struct dos_exec_transaction_table *table,
    struct dos_exec_transaction_slot *slot,
    const struct dos_exec_transaction_services *services,
    enum dos_memory_lease_status memory_status)
{
	if (memory_status == DOS_MEMORY_LEASE_MACHINE_FAULT)
		return process_memory_stage_failure(
		    table, slot, services, DOS_EXEC_JOURNAL_MACHINE_FAULT);
	return poison_memory_domain(table, slot, services);
}

static enum dos_exec_journal_status stage_rebind_value(
    struct dos_exec_transaction_slot *slot,
    const struct dos_exec_transaction_services *services,
    const struct dos_memory_lease_rebind_plan *plan)
{
	return dos_exec_journal_stage_replace_far(
	    &slot->journal, slot->machine_identity, services->machine,
	    plan->value.header_segment, 0u, plan->value.replacement_bytes,
	    sizeof(plan->value.replacement_bytes),
	    sizeof(plan->value.replacement_bytes));
}

static bool prepare_load_only_result(
    const struct dos_exec_transaction_slot *slot,
    struct dos_exec_load_result_value *result)
{
	const struct dos_cpu_state *initial_state;

	if (slot->resident.format == (uint8_t)DOS_IMAGE_COM) {
		initial_state = &slot->resident.process.com.initial_state;
		result->initial_sp =
		    slot->resident.process.com.load_only_stack_pointer;
	} else if (slot->resident.format == (uint8_t)DOS_IMAGE_MZ) {
		initial_state = &slot->resident.process.mz.initial_state;
		result->initial_sp =
		    slot->resident.process.mz.load_only_stack_pointer;
	} else {
		return false;
	}
	result->initial_ss = initial_state->ss;
	result->initial_ip = dos_register_low16(initial_state->eip);
	result->initial_cs = initial_state->cs;
	return true;
}

enum dos_exec_transaction_status dos_exec_transaction_stage_global_memory(
    struct dos_exec_transaction_table *table,
    struct dos_exec_transaction_handle handle,
    const struct dos_exec_transaction_services *services)
{
	struct dos_memory_lease_rebind_plan environment_rebind = {0};
	struct dos_memory_lease_rebind_plan load_rebind;
	struct dos_exec_load_result_value load_result;
	struct dos_process_far_address parameter_block;
	struct dos_exec_transaction_slot *slot;
	enum dos_process_runtime_status runtime_status;
	enum dos_memory_lease_status memory_status;
	enum dos_exec_journal_status journal_status;
	enum dos_exec_transaction_status status;
	uint32_t slot_index;
	size_t exit_vector_offset =
	    __builtin_offsetof(struct dos_psp_prefix40, exit_vector);

	status = find_bound_slot(table, handle, services, &slot_index);
	if (status != DOS_EXEC_TRANSACTION_OK)
		return status;
	slot = &table->slots[slot_index];
	if (slot->state != (uint8_t)DOS_EXEC_TRANSACTION_STATE_GLOBAL_STAGED ||
	    slot->publication.has_owner_name != 1u ||
	    slot->target.has_load_block != 1u ||
	    slot->journal.state != (uint8_t)DOS_EXEC_JOURNAL_STATE_STAGING)
		return DOS_EXEC_TRANSACTION_INVALID_STATE;
	runtime_status = dos_process_runtime_preflight_exec(
	    services->runtime, &slot->parent_runtime);
	if (runtime_status == DOS_PROCESS_RUNTIME_POISONED)
		return poison_transaction(table, slot, services);
	if (runtime_status != DOS_PROCESS_RUNTIME_OK) {
		slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_FAILED;
		return DOS_EXEC_TRANSACTION_RUNTIME_NOT_READY;
	}

	slot->state =
	    (uint8_t)DOS_EXEC_TRANSACTION_STATE_GLOBAL_MEMORY_STAGING;
	if (slot->environment.has_block != 0u) {
		memory_status = dos_memory_lease_prepare_owner_rebind(
		    services->memory_leases, services->memory_arena,
		    services->machine, slot->environment.lease.handle,
		    slot->parent_runtime.current_psp,
		    slot->target.lease.guest_segment, &environment_rebind);
		if (memory_status != DOS_MEMORY_LEASE_OK)
			return global_memory_lease_failure(table, slot, services,
						   memory_status);
	}
	memory_status = dos_memory_lease_prepare_owner_name_patch_rebind(
	    services->memory_leases, services->memory_arena, services->machine,
	    slot->target.lease.handle, slot->parent_runtime.current_psp,
	    slot->target.lease.guest_segment, &slot->publication.owner_name,
	    &load_rebind);
	if (memory_status != DOS_MEMORY_LEASE_OK)
		return global_memory_lease_failure(table, slot, services,
						   memory_status);

	journal_status = DOS_EXEC_JOURNAL_OK;
	if (slot->environment.has_block != 0u)
		journal_status =
		    stage_rebind_value(slot, services, &environment_rebind);
	if (journal_status == DOS_EXEC_JOURNAL_OK)
		journal_status = stage_rebind_value(slot, services, &load_rebind);
	if (journal_status == DOS_EXEC_JOURNAL_OK)
		journal_status = dos_exec_journal_stage_replace_far(
		    &slot->journal, slot->machine_identity, services->machine, 0u,
		    (uint16_t)(DOS_INTERRUPT_TERMINATE *
			       DOS_INTERRUPT_VECTOR_BYTES),
		    &slot->psp.image.bytes[exit_vector_offset],
		    sizeof(slot->psp.image.bytes) - exit_vector_offset,
		    DOS_INTERRUPT_VECTOR_BYTES);
	if (journal_status == DOS_EXEC_JOURNAL_OK &&
	    slot->request.subfunction == DOS_EXEC_LOAD_ONLY) {
		if (!prepare_load_only_result(slot, &load_result))
			return poison_transaction(table, slot, services);
		parameter_block.segment = slot->request.parameter_block.segment;
		parameter_block.offset = slot->request.parameter_block.offset;
		journal_status = dos_exec_parameter_stage_load_result(
		    &slot->journal, slot->machine_identity, services->machine,
		    parameter_block, &load_result);
	}
	if (journal_status != DOS_EXEC_JOURNAL_OK)
		return process_memory_stage_failure(table, slot, services,
						    journal_status);

	if (slot->environment.has_block != 0u) {
		slot->publication.environment_rebind = environment_rebind;
		slot->publication.has_environment_rebind = 1u;
	}
	slot->publication.load_rebind = load_rebind;
	slot->publication.has_load_rebind = 1u;
	slot->state =
	    (uint8_t)DOS_EXEC_TRANSACTION_STATE_GLOBAL_MEMORY_READY;
	if (!slot_publication_has_valid_encoding(slot))
		return poison_transaction(table, slot, services);
	return DOS_EXEC_TRANSACTION_OK;
}

enum dos_exec_transaction_status dos_exec_transaction_prepare_handoff(
    struct dos_exec_transaction_table *table,
    struct dos_exec_transaction_handle handle,
    const struct dos_exec_transaction_services *services,
    struct dos_exec_handoff_plan *result)
{
	struct dos_exec_handoff_plan prepared;
	struct dos_exec_transaction_slot *slot;
	enum dos_exec_handoff_status handoff_status;
	enum dos_exec_transaction_status status;
	uint32_t slot_index;

	if (result == NULL)
		return DOS_EXEC_TRANSACTION_INVALID_ARGUMENT;
	status = find_bound_slot(table, handle, services, &slot_index);
	if (status != DOS_EXEC_TRANSACTION_OK)
		return status;
	slot = &table->slots[slot_index];
	if (slot->request.subfunction != DOS_EXEC_LOAD_AND_EXECUTE ||
	    slot->state !=
		(uint8_t)DOS_EXEC_TRANSACTION_STATE_GLOBAL_MEMORY_READY ||
	    slot->publication.has_load_rebind != 1u ||
	    slot->publication.has_handoff != 0u ||
	    !handoff_plan_is_zero(&slot->publication.handoff))
		return DOS_EXEC_TRANSACTION_INVALID_STATE;

	if (slot->resident.format == (uint8_t)DOS_IMAGE_COM)
		handoff_status = dos_exec_handoff_prepare_com(
		    &slot->resident.process.com, &prepared);
	else if (slot->resident.format == (uint8_t)DOS_IMAGE_MZ)
		handoff_status = dos_exec_handoff_prepare_mz(
		    &slot->resident.process.mz, &prepared);
	else
		return poison_transaction(table, slot, services);
	if (handoff_status != DOS_EXEC_HANDOFF_OK)
		return poison_transaction(table, slot, services);

	slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_HANDOFF_STAGING;
	handoff_status = dos_exec_handoff_stage_stack(
	    &prepared, &slot->journal, slot->machine_identity,
	    services->machine);
	if (handoff_status != DOS_EXEC_HANDOFF_OK) {
		clear_publication_rebinds(&slot->publication);
		if (handoff_status == DOS_EXEC_HANDOFF_MACHINE_FAULT)
			return process_memory_stage_failure(
			    table, slot, services,
			    DOS_EXEC_JOURNAL_MACHINE_FAULT);
		if (handoff_status == DOS_EXEC_HANDOFF_POISONED)
			return poison_memory_domain(table, slot, services);
		return poison_transaction(table, slot, services);
	}

	slot->publication.handoff = prepared;
	slot->publication.has_handoff = 1u;
	slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_HANDOFF_READY;
	if (!slot_publication_has_valid_encoding(slot))
		return poison_transaction(table, slot, services);
	*result = prepared;
	return DOS_EXEC_TRANSACTION_OK;
}

enum dos_exec_transaction_status dos_exec_transaction_prepare_backend(
    struct dos_exec_transaction_table *table,
    struct dos_exec_transaction_handle handle,
    const struct dos_exec_transaction_services *services,
    uint32_t *failure_detail)
{
	struct dos_exec_backend_session_handle session;
	struct dos_exec_transaction_slot *slot;
	enum dos_exec_backend_session_status backend_status;
	enum dos_exec_transaction_status status;
	uint32_t detail;
	uint32_t slot_index;

	if (failure_detail == NULL)
		return DOS_EXEC_TRANSACTION_INVALID_ARGUMENT;
	status = find_bound_slot(table, handle, services, &slot_index);
	if (status != DOS_EXEC_TRANSACTION_OK)
		return status;
	slot = &table->slots[slot_index];
	if (slot->request.subfunction != DOS_EXEC_LOAD_AND_EXECUTE ||
	    slot->state != (uint8_t)DOS_EXEC_TRANSACTION_STATE_HANDOFF_READY ||
	    slot->publication.has_handoff != 1u ||
	    slot->publication.has_backend_session != 0u ||
	    !backend_binding_is_clear(&slot->publication) ||
	    !backend_services_are_complete(services) ||
	    services->backend_sessions->poisoned != 0u)
		return DOS_EXEC_TRANSACTION_INVALID_STATE;

	slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_BACKEND_PREPARING;
	backend_status = dos_exec_backend_session_prepare(
	    services->backend_sessions, services->backend_ops,
	    services->backend_adapter_context, slot->machine_identity,
	    services->machine, &slot->publication.handoff, &session, &detail);
	if (backend_status == DOS_EXEC_BACKEND_SESSION_PREPARE_REJECTED) {
		slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_HANDOFF_READY;
		*failure_detail = detail;
		return DOS_EXEC_TRANSACTION_BACKEND_UNAVAILABLE;
	}
	if (backend_status == DOS_EXEC_BACKEND_SESSION_POISONED) {
		slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_FAILED;
		return DOS_EXEC_TRANSACTION_BACKEND_POISONED;
	}
	if (backend_status != DOS_EXEC_BACKEND_SESSION_OK) {
		slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_HANDOFF_READY;
		return DOS_EXEC_TRANSACTION_BACKEND_UNAVAILABLE;
	}
	slot->publication.backend_session = session;
	slot->publication.backend_session_table_identity =
	    services->backend_session_table_identity;
	slot->publication.backend_adapter_identity =
	    services->backend_ops->identity;
	slot->publication.backend_adapter_context =
	    services->backend_adapter_context;
	slot->publication.has_backend_session = 1u;
	slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_BACKEND_DORMANT;
	if (!slot_publication_has_valid_encoding(slot))
		return poison_transaction(table, slot, services);
	*failure_detail = 0u;
	return DOS_EXEC_TRANSACTION_OK;
}

static void build_process_seal_plan(
    const struct dos_exec_transaction_slot *slot,
    struct dos_exec_load_only_seal_plan *plan)
{
	*plan = (struct dos_exec_load_only_seal_plan){0};
	plan->expected_parent = slot->parent_runtime;
	plan->environment_rebind = slot->publication.environment_rebind;
	plan->load_rebind = slot->publication.load_rebind;
	plan->sft_batch = slot->inheritance.batch;
	plan->child_psp = slot->target.lease.guest_segment;
	plan->has_environment = slot->environment.has_block != 0u ? 1u : 0u;
}

static struct dos_exec_seal_services make_process_seal_services(
    struct dos_exec_transaction_slot *slot,
    const struct dos_exec_transaction_services *services)
{
	return (struct dos_exec_seal_services){
	    .observer = &slot->observer,
	    .observer_ops = services->observer_ops,
	    .observer_context = services->observer_adapter_context,
	    .journal = &slot->journal,
	    .machine_identity = slot->machine_identity,
	    .memory_leases = services->memory_leases,
	    .arena = services->memory_arena,
	    .machine = services->machine,
	    .runtime = services->runtime,
	};
}

enum dos_exec_transaction_status dos_exec_transaction_seal_execute(
    struct dos_exec_transaction_table *table,
    struct dos_exec_transaction_handle handle,
    const struct dos_exec_transaction_services *services,
    struct dos_exec_backend_session_handle *session)
{
	struct dos_exec_load_only_seal_plan plan;
	struct dos_exec_seal_services seal_services;
	struct dos_exec_transaction_slot *slot;
	enum dos_exec_backend_session_status backend_status;
	enum dos_exec_seal_status seal_status;
	enum dos_exec_transaction_status status;
	uint32_t slot_index;

	if (session == NULL)
		return DOS_EXEC_TRANSACTION_INVALID_ARGUMENT;
	status = find_bound_slot(table, handle, services, &slot_index);
	if (status != DOS_EXEC_TRANSACTION_OK)
		return status;
	slot = &table->slots[slot_index];
	if (slot->request.subfunction != DOS_EXEC_LOAD_AND_EXECUTE ||
	    slot->state !=
		(uint8_t)DOS_EXEC_TRANSACTION_STATE_BACKEND_DORMANT ||
	    slot->publication.has_backend_session != 1u ||
	    slot->inheritance.has_batch != 1u)
		return DOS_EXEC_TRANSACTION_INVALID_STATE;
	backend_status = dos_exec_backend_session_preflight_publish(
	    services->backend_sessions, slot->publication.backend_session,
	    services->backend_ops, services->backend_adapter_context,
	    slot->machine_identity, services->machine,
	    &slot->publication.handoff);
	if (backend_status == DOS_EXEC_BACKEND_SESSION_POISONED)
		return DOS_EXEC_TRANSACTION_BACKEND_POISONED;
	if (backend_status != DOS_EXEC_BACKEND_SESSION_OK)
		return DOS_EXEC_TRANSACTION_PUBLICATION_NOT_READY;

	build_process_seal_plan(slot, &plan);
	seal_services = make_process_seal_services(slot, services);
	slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_COMMITTING;
	seal_status = dos_exec_seal_commit_load_only(&seal_services, &plan);
	if (seal_status != DOS_EXEC_SEAL_OK) {
		if (seal_status == DOS_EXEC_SEAL_POISONED)
			return poison_memory_domain(table, slot, services);
		slot->state =
		    (uint8_t)DOS_EXEC_TRANSACTION_STATE_BACKEND_DORMANT;
		return DOS_EXEC_TRANSACTION_PUBLICATION_NOT_READY;
	}
	backend_status = dos_exec_backend_session_publish(
	    services->backend_sessions, slot->publication.backend_session,
	    services->backend_ops, services->backend_adapter_context,
	    slot->machine_identity, services->machine,
	    &slot->publication.handoff);
	if (backend_status != DOS_EXEC_BACKEND_SESSION_OK)
		return poison_memory_domain(table, slot, services);
	slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_PUBLISHED;
	if (!slot_has_valid_encoding(slot)) {
		table->poisoned = 1u;
		return DOS_EXEC_TRANSACTION_POISONED;
	}
	*session = slot->publication.backend_session;
	return DOS_EXEC_TRANSACTION_OK;
}

enum dos_exec_transaction_status dos_exec_transaction_seal_load_only(
    struct dos_exec_transaction_table *table,
    struct dos_exec_transaction_handle handle,
    const struct dos_exec_transaction_services *services)
{
	struct dos_exec_load_only_seal_plan plan = {0};
	struct dos_exec_seal_services seal_services;
	struct dos_exec_transaction_slot *slot;
	enum dos_exec_seal_status seal_status;
	enum dos_exec_transaction_status status;
	uint32_t slot_index;

	status = find_bound_slot(table, handle, services, &slot_index);
	if (status != DOS_EXEC_TRANSACTION_OK)
		return status;
	slot = &table->slots[slot_index];
	if (slot->request.subfunction != DOS_EXEC_LOAD_ONLY ||
	    slot->state !=
		(uint8_t)DOS_EXEC_TRANSACTION_STATE_GLOBAL_MEMORY_READY ||
	    slot->inheritance.has_batch != 1u ||
	    slot->publication.has_load_rebind != 1u)
		return DOS_EXEC_TRANSACTION_INVALID_STATE;

	build_process_seal_plan(slot, &plan);
	seal_services = make_process_seal_services(slot, services);
	slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_COMMITTING;
	seal_status =
	    dos_exec_seal_commit_load_only(&seal_services, &plan);
	if (seal_status != DOS_EXEC_SEAL_OK) {
		if (seal_status == DOS_EXEC_SEAL_POISONED)
			return poison_memory_domain(table, slot, services);
		slot->state =
		    (uint8_t)DOS_EXEC_TRANSACTION_STATE_GLOBAL_MEMORY_READY;
		return DOS_EXEC_TRANSACTION_PUBLICATION_NOT_READY;
	}
	slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_PUBLISHED;
	if (!slot_has_valid_encoding(slot)) {
		table->poisoned = 1u;
		return DOS_EXEC_TRANSACTION_POISONED;
	}
	return DOS_EXEC_TRANSACTION_OK;
}

enum dos_exec_transaction_status dos_exec_transaction_retire_published(
    struct dos_exec_transaction_table *table,
    struct dos_exec_transaction_handle handle,
    const struct dos_exec_transaction_services *services)
{
	struct dos_exec_transaction_slot *slot;
	enum dos_exec_file_lease_status file_status;
	enum dos_sft_batch_status sft_status;
	enum dos_exec_transaction_status status;
	uint32_t slot_index;

	status = find_bound_slot(table, handle, services, &slot_index);
	if (status != DOS_EXEC_TRANSACTION_OK)
		return status;
	slot = &table->slots[slot_index];
	if (slot->state != (uint8_t)DOS_EXEC_TRANSACTION_STATE_PUBLISHED ||
	    slot->has_file_lease != 1u || slot->inheritance.has_batch != 1u)
		return DOS_EXEC_TRANSACTION_INVALID_STATE;
	file_status = dos_exec_file_lease_preflight_retire(
	    services->file_leases, slot->file_lease);
	sft_status = dos_sft_batch_preflight_retire(slot->inheritance.batch);
	if (file_status != DOS_EXEC_FILE_LEASE_OK ||
	    sft_status != DOS_SFT_BATCH_OK)
		return DOS_EXEC_TRANSACTION_PUBLICATION_NOT_READY;

	/* Both generations are terminal and all checks above are pure.  Under
	 * the coordinator's serialization, neither no-callback retire can now
	 * acquire a new failure condition.
	 */
	if (dos_sft_batch_retire(slot->inheritance.batch) !=
		DOS_SFT_BATCH_OK ||
	    dos_exec_file_lease_retire(services->file_leases,
				       slot->file_lease) !=
		DOS_EXEC_FILE_LEASE_OK) {
		table->poisoned = 1u;
		return DOS_EXEC_TRANSACTION_POISONED;
	}
	vacate_slot(slot);
	return DOS_EXEC_TRANSACTION_OK;
}

static bool state_can_abort(uint8_t state)
{
	return state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_OBSERVED ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_FILE_OPEN ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_FILE_PROBED ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_ENV_READY ||
	       state == (uint8_t)
			    DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_BLOCK_READY ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_IMAGE_READY ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_TARGET_READY ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_RESIDENT_READY ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_IMAGE_PRIVATE ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_PROCESS_FILE_CLOSED ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_PARENT_SNAPSHOT_READY ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_SFT_RESERVED ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_PSP_PREPARED ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_INITIAL_STATE_READY ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_GLOBAL_STAGED ||
	       state ==
		   (uint8_t)DOS_EXEC_TRANSACTION_STATE_GLOBAL_MEMORY_READY ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_HANDOFF_READY ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_BACKEND_DORMANT ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_FILE_CLOSED ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_FAILED ||
	       state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_ABORTING;
}

enum dos_exec_transaction_status
dos_exec_transaction_abort(struct dos_exec_transaction_table *table,
			   struct dos_exec_transaction_handle handle,
			   const struct dos_exec_transaction_services *services)
{
	struct dos_exec_transaction_slot *slot;
	enum dos_exec_backend_session_status backend_status;
	enum dos_exec_file_lease_status file_status;
	enum dos_memory_lease_status memory_status;
	enum dos_exec_observer_status observer_status;
	enum dos_sft_batch_status sft_status;
	enum dos_exec_journal_status journal_status;
	enum dos_exec_transaction_status status;
	bool backend_poisoned = false;
	uint32_t slot_index;

	status = find_bound_slot(table, handle, services, &slot_index);
	if (status != DOS_EXEC_TRANSACTION_OK)
		return status;
	slot = &table->slots[slot_index];
	if (slot->state == (uint8_t)DOS_EXEC_TRANSACTION_STATE_ABORTED)
		return DOS_EXEC_TRANSACTION_OK;
	if (!state_can_abort(slot->state))
		return DOS_EXEC_TRANSACTION_INVALID_STATE;
	if (slot->publication.has_backend_session != 0u) {
		slot->state =
		    (uint8_t)DOS_EXEC_TRANSACTION_STATE_BACKEND_RELEASING;
		backend_status = dos_exec_backend_session_stop(
		    services->backend_sessions,
		    slot->publication.backend_session, services->backend_ops,
		    services->backend_adapter_context);
		if (backend_status ==
		    DOS_EXEC_BACKEND_SESSION_RELEASE_RETAINED) {
			slot->state =
			    (uint8_t)DOS_EXEC_TRANSACTION_STATE_BACKEND_DORMANT;
			return DOS_EXEC_TRANSACTION_BACKEND_RETAINED;
		}
		if (backend_status == DOS_EXEC_BACKEND_SESSION_OK) {
			backend_status = dos_exec_backend_session_retire(
			    services->backend_sessions,
			    slot->publication.backend_session);
			if (backend_status != DOS_EXEC_BACKEND_SESSION_OK)
				return poison_transaction(table, slot, services);
		} else if (backend_status ==
			   DOS_EXEC_BACKEND_SESSION_POISONED) {
			backend_poisoned = true;
		} else {
			return poison_transaction(table, slot, services);
		}
		clear_backend_binding(&slot->publication);
	}
	slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_ABORTING;
	if (request_is_process(&slot->request) &&
	    slot->journal.state == (uint8_t)DOS_EXEC_JOURNAL_STATE_STAGING) {
		if (slot->journal.record_count != 0u)
			slot->state =
			    (uint8_t)DOS_EXEC_TRANSACTION_STATE_JOURNAL_ABORTING;
		journal_status = dos_exec_journal_abort(
		    &slot->journal, slot->machine_identity, services->machine);
		if (journal_status != DOS_EXEC_JOURNAL_OK)
			return poison_memory_domain(table, slot, services);
		clear_publication_rebinds(&slot->publication);
		slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_ABORTING;
	}
	clear_transaction_psp(&slot->psp);
	if (slot->inheritance.has_batch != 0u) {
		slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_SFT_ABORTING;
		sft_status = dos_sft_batch_abort(
		    slot->inheritance.batch, services->sft_ops,
		    services->sft_adapter_context);
		if (sft_status != DOS_SFT_BATCH_OK)
			return poison_memory_domain(table, slot, services);
		sft_status = dos_sft_batch_retire(slot->inheritance.batch);
		if (sft_status != DOS_SFT_BATCH_OK)
			return poison_memory_domain(table, slot, services);
		clear_transaction_inheritance(&slot->inheritance);
		slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_ABORTING;
	}
	if (slot->target.has_load_block != 0u) {
		slot->state =
		    (uint8_t)DOS_EXEC_TRANSACTION_STATE_LOAD_LEASE_ABORTING;
		memory_status = dos_memory_lease_abort(
		    services->memory_leases, services->memory_arena,
		    services->machine, slot->target.lease.handle,
		    slot->parent_runtime.current_psp);
		if (memory_status == DOS_MEMORY_LEASE_MACHINE_FAULT) {
			slot->state =
			    (uint8_t)DOS_EXEC_TRANSACTION_STATE_ABORTING;
			return DOS_EXEC_TRANSACTION_MEMORY_LEASE_RETAINED;
		}
		if (memory_status != DOS_MEMORY_LEASE_OK)
			return poison_memory_domain(table, slot, services);
		clear_transaction_parent(&slot->parent);
		clear_transaction_relocation(&slot->relocation);
		clear_transaction_resident(&slot->resident);
		clear_target_lease(&slot->target);
		slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_ABORTING;
	}
	if (slot->environment.has_block != 0u) {
		slot->state = (uint8_t)
		    DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_LEASE_ABORTING;
		memory_status = dos_memory_lease_abort(
		    services->memory_leases, services->memory_arena,
		    services->machine, slot->environment.lease.handle,
		    slot->parent_runtime.current_psp);
		if (memory_status == DOS_MEMORY_LEASE_MACHINE_FAULT) {
			slot->state =
			    (uint8_t)DOS_EXEC_TRANSACTION_STATE_ABORTING;
			return DOS_EXEC_TRANSACTION_MEMORY_LEASE_RETAINED;
		}
		if (memory_status != DOS_MEMORY_LEASE_OK)
			return poison_memory_domain(table, slot, services);
		clear_environment_lease(&slot->environment);
		slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_ABORTING;
	}

	if (slot->has_file_lease != 0u) {
		slot->state =
		    (uint8_t)DOS_EXEC_TRANSACTION_STATE_ABORT_FILE_CLOSING;
		file_status = dos_exec_file_lease_abort(
		    services->file_leases, slot->file_lease, services->file_ops,
		    services->file_adapter_context);
		if (file_status == DOS_EXEC_FILE_LEASE_CLOSE_RETAINED) {
			slot->state =
			    (uint8_t)DOS_EXEC_TRANSACTION_STATE_ABORTING;
			return DOS_EXEC_TRANSACTION_CLOSE_RETAINED;
		}
		if (file_status == DOS_EXEC_FILE_LEASE_POISONED)
			return poison_transaction(table, slot, services);
		if (file_status != DOS_EXEC_FILE_LEASE_OK)
			return poison_transaction(table, slot, services);
		file_status = dos_exec_file_lease_retire(services->file_leases,
							 slot->file_lease);
		if (file_status != DOS_EXEC_FILE_LEASE_OK)
			return poison_transaction(table, slot, services);
		slot->file_lease.value = 0u;
		slot->has_file_lease = 0u;
	}

	slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_OBSERVER_RELEASING;
	observer_status =
	    dos_exec_observer_release(&slot->observer, services->observer_ops,
				      services->observer_adapter_context);
	if (observer_status != DOS_EXEC_OBSERVER_OK)
		return poison_transaction(table, slot, services);
	slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_ABORTED;
	return backend_poisoned ? DOS_EXEC_TRANSACTION_BACKEND_POISONED
				: DOS_EXEC_TRANSACTION_OK;
}

enum dos_exec_transaction_status
dos_exec_transaction_retire(struct dos_exec_transaction_table *table,
			    kernel_object_handle_t coordinator_identity,
			    struct dos_exec_transaction_handle handle)
{
	struct dos_exec_transaction_slot *slot;
	enum dos_exec_transaction_status status;
	uint32_t slot_index;

	if (!identity_is_valid(coordinator_identity))
		return DOS_EXEC_TRANSACTION_INVALID_ARGUMENT;
	status = validate_table(table);
	if (status != DOS_EXEC_TRANSACTION_OK)
		return status;
	if (coordinator_identity != table->coordinator_identity)
		return DOS_EXEC_TRANSACTION_BINDING_MISMATCH;
	status = find_slot(table, handle, &slot_index);
	if (status != DOS_EXEC_TRANSACTION_OK)
		return status;
	slot = &table->slots[slot_index];
	if (slot->coordinator_identity != coordinator_identity)
		return DOS_EXEC_TRANSACTION_BINDING_MISMATCH;
	if (slot->state != (uint8_t)DOS_EXEC_TRANSACTION_STATE_ABORTED)
		return DOS_EXEC_TRANSACTION_INVALID_STATE;
	vacate_slot(slot);
	return DOS_EXEC_TRANSACTION_OK;
}

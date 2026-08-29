// SPDX-License-Identifier: GPL-2.0-only
/*
 * XMS High Memory Area ownership
 *
 * XMS 3.0 compatibility uses global single-owner ordering and typed error
 * values.
 * Safety changes: active guest mapping proof, manager-bound generations,
 * acquire-before-publish and sticky quarantine of uncertain mapping state.
 */
#include "hma.h"

#define XMS_ERROR_HMA_NOT_PRESENT 0x90u
#define XMS_ERROR_HMA_IN_USE 0x91u
#define XMS_ERROR_HMA_REQUEST_TOO_SMALL 0x92u
#define XMS_ERROR_HMA_NOT_USED 0x93u

static bool valid_identity(kernel_object_handle_t identity)
{
	return identity != 0u && identity != KERNEL_OBJECT_HANDLE_INVALID;
}

static bool lease_encoding_is_valid(const struct dos_xms_hma_lease *lease)
{
	size_t index;

	for (index = 0u; index < ARRAY_SIZE(lease->reserved); ++index) {
		if (lease->reserved[index] != 0u)
			return false;
	}
	return true;
}

static void return_failure(struct dos_cpu_state *state, uint8_t error)
{
	dos_register_set_low16(&state->eax, 0u);
	dos_register_set_low8(&state->ebx, error);
}

static void return_success(struct dos_cpu_state *state)
{
	dos_register_set_low16(&state->eax, 1u);
	dos_register_set_low8(&state->ebx, 0u);
}

static bool snapshot_is_valid(const struct dos_xms_hma_snapshot *snapshot,
			      const struct dos_machine *machine)
{
	return snapshot != NULL && machine != NULL &&
	       valid_identity(snapshot->address_space_identity) &&
	       snapshot->address_space_generation != 0u &&
	       snapshot->machine_context == machine->context &&
	       snapshot->base_address == DOS_XMS_HMA_BASE &&
	       snapshot->byte_count == DOS_XMS_HMA_BYTES &&
	       machine->address_limit >= DOS_XMS_HMA_LIMIT;
}

static enum dos_xms_memory_status query_mapping(
	const struct dos_xms_manager *manager, const struct dos_machine *machine,
	struct dos_xms_hma_snapshot *snapshot)
{
	enum dos_xms_memory_status status;

	if (manager->ops->query_hma == NULL)
		return DOS_XMS_MEMORY_NO_MEMORY;
	status = manager->ops->query_hma(manager->memory_context, machine,
					 snapshot);
	if (status != DOS_XMS_MEMORY_OK)
		return status;
	return snapshot_is_valid(snapshot, machine) ? DOS_XMS_MEMORY_OK
						    : DOS_XMS_MEMORY_UNCERTAIN;
}

static enum dos_xms_status mapping_fault(struct dos_xms_manager *manager,
					 enum dos_xms_memory_status status)
{
	if (status == DOS_XMS_MEMORY_UNCERTAIN)
		manager->poisoned = 1u;
	return DOS_XMS_MACHINE_FAULT;
}

static bool same_mapping(const struct dos_xms_hma_snapshot *left,
			 const struct dos_xms_hma_snapshot *right)
{
	return left->address_space_identity == right->address_space_identity &&
	       left->address_space_generation ==
		       right->address_space_generation &&
	       left->machine_context == right->machine_context &&
	       left->base_address == right->base_address &&
	       left->byte_count == right->byte_count;
}

static bool active_lease_is_valid(const struct dos_xms_manager *manager,
				  const struct dos_machine *machine)
{
	return manager->hma.active == 1u &&
	       lease_encoding_is_valid(&manager->hma) &&
	       manager->hma.manager_identity == manager->identity &&
	       manager->hma.manager_generation == manager->generation &&
	       manager->hma.lease_generation == manager->hma_generation &&
	       snapshot_is_valid(&manager->hma.mapping, machine);
}

enum dos_xms_status dos_xms_hma_report_version(
	struct dos_xms_manager *manager, const struct dos_machine *machine,
	struct dos_cpu_state *state)
{
	struct dos_xms_hma_snapshot snapshot;
	enum dos_xms_memory_status status;

	status = query_mapping(manager, machine, &snapshot);
	if (status == DOS_XMS_MEMORY_NO_MEMORY) {
		dos_register_set_low16(&state->edx, 0u);
		return DOS_XMS_READY;
	}
	if (status != DOS_XMS_MEMORY_OK)
		return mapping_fault(manager, status);
	dos_register_set_low16(&state->edx, 1u);
	return DOS_XMS_READY;
}

enum dos_xms_status dos_xms_hma_request(
	struct dos_xms_manager *manager, const struct dos_machine *machine,
	struct dos_cpu_state *state)
{
	struct dos_xms_hma_snapshot snapshot;
	struct dos_xms_hma_lease prepared;
	enum dos_xms_memory_status status;
	uint16_t requested = dos_register_low16(state->edx);

	if (manager->hma.active != 0u) {
		if (!active_lease_is_valid(manager, machine)) {
			manager->poisoned = 1u;
			return DOS_XMS_MACHINE_FAULT;
		}
		return_failure(state, XMS_ERROR_HMA_IN_USE);
		return DOS_XMS_READY;
	}
	if (requested < manager->hma_minimum_bytes) {
		return_failure(state, XMS_ERROR_HMA_REQUEST_TOO_SMALL);
		return DOS_XMS_READY;
	}
	status = query_mapping(manager, machine, &snapshot);
	if (status == DOS_XMS_MEMORY_NO_MEMORY) {
		return_failure(state, XMS_ERROR_HMA_NOT_PRESENT);
		return DOS_XMS_READY;
	}
	if (status != DOS_XMS_MEMORY_OK)
		return mapping_fault(manager, status);
	if (manager->hma_generation == DOS_XMS_MANAGER_GENERATION_MAX) {
		manager->poisoned = 1u;
		return DOS_XMS_MACHINE_FAULT;
	}
	prepared = (struct dos_xms_hma_lease){
		.manager_identity = manager->identity,
		.manager_generation = manager->generation,
		.lease_generation = manager->hma_generation + 1u,
		.mapping = snapshot,
		.active = 1u,
		.reserved = {0u},
	};
	manager->hma_generation = prepared.lease_generation;
	manager->hma = prepared;
	return_success(state);
	return DOS_XMS_READY;
}

enum dos_xms_status dos_xms_hma_release(
	struct dos_xms_manager *manager, const struct dos_machine *machine,
	struct dos_cpu_state *state)
{
	struct dos_xms_hma_snapshot snapshot;
	enum dos_xms_memory_status status;

	if (manager->hma.active == 0u) {
		return_failure(state, XMS_ERROR_HMA_NOT_USED);
		return DOS_XMS_READY;
	}
	if (!active_lease_is_valid(manager, machine)) {
		manager->poisoned = 1u;
		return DOS_XMS_MACHINE_FAULT;
	}
	status = query_mapping(manager, machine, &snapshot);
	if (status == DOS_XMS_MEMORY_NO_MEMORY) {
		return_failure(state, XMS_ERROR_HMA_NOT_PRESENT);
		return DOS_XMS_READY;
	}
	if (status != DOS_XMS_MEMORY_OK)
		return mapping_fault(manager, status);
	if (!same_mapping(&manager->hma.mapping, &snapshot)) {
		manager->poisoned = 1u;
		return DOS_XMS_MACHINE_FAULT;
	}
	manager->hma = (struct dos_xms_hma_lease){0};
	return_success(state);
	return DOS_XMS_READY;
}

// SPDX-License-Identifier: GPL-2.0-only
/*
 * Exclusive owner for the process-global EXEC interval.
 * DOS-visible execution relies on non-reentrant process state.  This gate makes
 * that lifetime explicit and generation checked without adding a new visible
 * scheduling rule.
 */
#include "dos_exec_gate.h"

#define DOS_EXEC_GATE_GENERATION_MAX (~(uint64_t)0u)

struct dos_exec_gate_owner {
	kernel_object_handle_t context;
	uint64_t generation;
	uint64_t held_generation;
	uint8_t initialized;
	uint8_t quarantined;
	uint8_t reserved[6];
} __aligned(8);

static struct dos_exec_gate_owner owner;

static enum dos_exec_observer_adapter_status
gate_acquire(kernel_object_handle_t context, uint64_t *generation);
static enum dos_exec_observer_adapter_status
gate_release(kernel_object_handle_t context, uint64_t generation);
static enum dos_exec_observer_adapter_status
gate_quarantine(kernel_object_handle_t context, uint64_t generation);

static struct dos_exec_observer_ops operations = {
	.identity = KERNEL_OBJECT_HANDLE_INVALID,
	.acquire = gate_acquire,
	.release = gate_release,
	.quarantine = gate_quarantine,
};

static bool identity_is_valid(kernel_object_handle_t identity)
{
	return identity != 0u && identity != KERNEL_OBJECT_HANDLE_INVALID;
}

enum dos_exec_gate_status dos_exec_gate_initialize(
	kernel_object_handle_t adapter_identity,
	kernel_object_handle_t adapter_context)
{
	if (!identity_is_valid(adapter_identity) ||
	    !identity_is_valid(adapter_context))
		return DOS_EXEC_GATE_INVALID_ARGUMENT;
	if (owner.initialized != 0u)
		return DOS_EXEC_GATE_INVALID_STATE;
	owner = (struct dos_exec_gate_owner){
		.context = adapter_context,
		.generation = 0u,
		.held_generation = 0u,
		.initialized = 1u,
		.quarantined = 0u,
		.reserved = {0u},
	};
	operations.identity = adapter_identity;
	return DOS_EXEC_GATE_READY;
}

const struct dos_exec_observer_ops *dos_exec_gate_ops(void)
{
	return owner.initialized == 1u ? &operations : NULL;
}

kernel_object_handle_t dos_exec_gate_context(void)
{
	return owner.initialized == 1u ? owner.context
				      : KERNEL_OBJECT_HANDLE_INVALID;
}

bool dos_exec_gate_is_quarantined(void)
{
	return owner.initialized == 1u && owner.quarantined == 1u;
}

static enum dos_exec_observer_adapter_status
gate_acquire(kernel_object_handle_t context, uint64_t *generation)
{
	if (generation == NULL || owner.initialized != 1u ||
	    context != owner.context || owner.quarantined != 0u)
		return DOS_EXEC_OBSERVER_ADAPTER_FAULT;
	if (owner.held_generation != 0u)
		return DOS_EXEC_OBSERVER_ADAPTER_BUSY;
	if (owner.generation == DOS_EXEC_GATE_GENERATION_MAX)
		return DOS_EXEC_OBSERVER_ADAPTER_FAULT;
	++owner.generation;
	owner.held_generation = owner.generation;
	*generation = owner.generation;
	return DOS_EXEC_OBSERVER_ADAPTER_OK;
}

static enum dos_exec_observer_adapter_status
gate_release(kernel_object_handle_t context, uint64_t generation)
{
	if (owner.initialized != 1u || context != owner.context ||
	    owner.quarantined != 0u || generation == 0u ||
	    generation != owner.held_generation)
		return DOS_EXEC_OBSERVER_ADAPTER_FAULT;
	owner.held_generation = 0u;
	return DOS_EXEC_OBSERVER_ADAPTER_OK;
}

static enum dos_exec_observer_adapter_status
gate_quarantine(kernel_object_handle_t context, uint64_t generation)
{
	if (owner.initialized != 1u || context != owner.context)
		return DOS_EXEC_OBSERVER_ADAPTER_FAULT;
	/* A zero, stale, or unexpected generation is precisely why this fail-closed
	 * path exists.  Once the namespace identity matches, never refuse to make
	 * the quarantine sticky. */
	(void)generation;
	owner.quarantined = 1u;
	owner.held_generation = 0u;
	return DOS_EXEC_OBSERVER_ADAPTER_OK;
}

static_assert_expression(sizeof(struct dos_exec_gate_owner) == 32u,
			 "EXEC gate state must stay fixed width");

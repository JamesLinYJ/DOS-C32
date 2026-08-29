// SPDX-License-Identifier: GPL-2.0-only
/* Checked publication of CurrentPDB and DMAADD. */
#include "dos_process_runtime.h"

#define DOS_PROCESS_DEFAULT_DTA_OFFSET 0x0080u

static bool identity_is_valid(kernel_object_handle_t identity)
{
	return identity != 0u && identity != KERNEL_OBJECT_HANDLE_INVALID;
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

static bool
runtime_has_valid_encoding(const struct dos_process_runtime *runtime)
{
	if (runtime == NULL || runtime->initialized > 1u ||
	    runtime->poisoned > 1u || runtime->constructed > 1u ||
	    !bytes_are_zero(runtime->reserved, ARRAY_SIZE(runtime->reserved)))
		return false;
	if (runtime->initialized == 0u)
		return runtime->identity == KERNEL_OBJECT_HANDLE_INVALID &&
		       runtime->generation == 0u && runtime->poisoned == 0u &&
		       runtime->dta.offset == 0u &&
		       runtime->dta.segment == 0u && runtime->current_psp == 0u;
	return runtime->constructed == 1u && runtime->generation != 0u &&
	       identity_is_valid(runtime->identity);
}

static enum dos_process_runtime_status
runtime_ready_status(const struct dos_process_runtime *runtime)
{
	if (!runtime_has_valid_encoding(runtime))
		return DOS_PROCESS_RUNTIME_INVALID_ARGUMENT;
	if (runtime->constructed != 1u || runtime->initialized != 1u ||
	    runtime->generation == 0u)
		return DOS_PROCESS_RUNTIME_NOT_INITIALIZED;
	if (runtime->poisoned == 1u)
		return DOS_PROCESS_RUNTIME_POISONED;
	return DOS_PROCESS_RUNTIME_OK;
}

static enum dos_process_runtime_status
runtime_can_advance(const struct dos_process_runtime *runtime)
{
	enum dos_process_runtime_status status;

	status = runtime_ready_status(runtime);
	if (status != DOS_PROCESS_RUNTIME_OK)
		return status;
	if (runtime->generation == DOS_PROCESS_RUNTIME_GENERATION_MAX)
		return DOS_PROCESS_RUNTIME_GENERATION_EXHAUSTED;
	return DOS_PROCESS_RUNTIME_OK;
}

enum dos_process_runtime_status
dos_process_runtime_construct(struct dos_process_runtime *runtime)
{
	if (runtime == NULL)
		return DOS_PROCESS_RUNTIME_INVALID_ARGUMENT;
	*runtime = (struct dos_process_runtime)DOS_PROCESS_RUNTIME_INITIALIZER;
	return DOS_PROCESS_RUNTIME_OK;
}

enum dos_process_runtime_status dos_process_runtime_initialize(
    struct dos_process_runtime *runtime, kernel_object_handle_t identity,
    uint16_t current_psp, struct dos_far_pointer16 dta)
{
	struct dos_process_runtime initialized;

	if (!runtime_has_valid_encoding(runtime) ||
	    runtime->constructed != 1u || !identity_is_valid(identity))
		return DOS_PROCESS_RUNTIME_INVALID_ARGUMENT;
	if (runtime->poisoned == 1u)
		return DOS_PROCESS_RUNTIME_POISONED;
	if (runtime->initialized == 1u)
		return DOS_PROCESS_RUNTIME_INVALID_STATE;
	initialized = (struct dos_process_runtime){
	    .generation = 1u,
	    .identity = identity,
	    .dta = dta,
	    .current_psp = current_psp,
	    .initialized = 1u,
	    .poisoned = 0u,
	    .constructed = 1u,
	    .reserved = {0u},
	};
	*runtime = initialized;
	return DOS_PROCESS_RUNTIME_OK;
}

enum dos_process_runtime_status
dos_process_runtime_snapshot(const struct dos_process_runtime *runtime,
			     struct dos_process_runtime_snapshot *snapshot)
{
	struct dos_process_runtime_snapshot captured;
	enum dos_process_runtime_status status;

	if (snapshot == NULL)
		return DOS_PROCESS_RUNTIME_INVALID_ARGUMENT;
	status = runtime_ready_status(runtime);
	if (status != DOS_PROCESS_RUNTIME_OK)
		return status;
	captured = (struct dos_process_runtime_snapshot){
	    .generation = runtime->generation,
	    .runtime_identity = runtime->identity,
	    .dta = runtime->dta,
	    .current_psp = runtime->current_psp,
	    .reserved = 0u,
	};
	*snapshot = captured;
	return DOS_PROCESS_RUNTIME_OK;
}

enum dos_process_runtime_status
dos_process_runtime_set_current_psp(struct dos_process_runtime *runtime,
				    uint16_t current_psp)
{
	enum dos_process_runtime_status status;

	status = runtime_can_advance(runtime);
	if (status != DOS_PROCESS_RUNTIME_OK)
		return status;
	runtime->current_psp = current_psp;
	runtime->generation++;
	return DOS_PROCESS_RUNTIME_OK;
}

enum dos_process_runtime_status
dos_process_runtime_set_dta(struct dos_process_runtime *runtime,
			    struct dos_far_pointer16 dta)
{
	enum dos_process_runtime_status status;

	status = runtime_can_advance(runtime);
	if (status != DOS_PROCESS_RUNTIME_OK)
		return status;
	runtime->dta = dta;
	runtime->generation++;
	return DOS_PROCESS_RUNTIME_OK;
}

enum dos_process_runtime_status dos_process_runtime_preflight_exec(
    const struct dos_process_runtime *runtime,
    const struct dos_process_runtime_snapshot *expected)
{
	enum dos_process_runtime_status status;

	if (expected == NULL || expected->reserved != 0u ||
	    !identity_is_valid(expected->runtime_identity))
		return DOS_PROCESS_RUNTIME_INVALID_ARGUMENT;
	status = runtime_can_advance(runtime);
	if (status != DOS_PROCESS_RUNTIME_OK)
		return status;
	if (expected->runtime_identity != runtime->identity ||
	    expected->generation != runtime->generation ||
	    expected->current_psp != runtime->current_psp ||
	    expected->dta.offset != runtime->dta.offset ||
	    expected->dta.segment != runtime->dta.segment)
		return DOS_PROCESS_RUNTIME_STALE_SNAPSHOT;
	return DOS_PROCESS_RUNTIME_OK;
}

enum dos_process_runtime_status dos_process_runtime_publish_exec(
    struct dos_process_runtime *runtime,
    const struct dos_process_runtime_snapshot *expected, uint16_t child_psp)
{
	enum dos_process_runtime_status status;

	status = dos_process_runtime_preflight_exec(runtime, expected);
	if (status != DOS_PROCESS_RUNTIME_OK)
		return status;

	/* EXEC sets DMAADD before returning with child CurrentPDB.
	 */
	runtime->dta.offset = DOS_PROCESS_DEFAULT_DTA_OFFSET;
	runtime->dta.segment = child_psp;
	runtime->current_psp = child_psp;
	runtime->generation++;
	return DOS_PROCESS_RUNTIME_OK;
}

enum dos_process_runtime_status dos_process_runtime_restore_parent(
	struct dos_process_runtime *runtime,
	const struct dos_process_runtime_snapshot *expected_child,
	const struct dos_process_runtime_snapshot *parent)
{
	enum dos_process_runtime_status status;

	if (parent == NULL || parent->reserved != 0u ||
	    !identity_is_valid(parent->runtime_identity))
		return DOS_PROCESS_RUNTIME_INVALID_ARGUMENT;
	status = dos_process_runtime_preflight_exec(runtime, expected_child);
	if (status != DOS_PROCESS_RUNTIME_OK)
		return status;
	if (parent->runtime_identity != runtime->identity)
		return DOS_PROCESS_RUNTIME_STALE_SNAPSHOT;
	/* The parent snapshot predates child publication and therefore has an
	 * older generation by design; its guest values remain the authority. */
	runtime->dta = parent->dta;
	runtime->current_psp = parent->current_psp;
	runtime->generation++;
	return DOS_PROCESS_RUNTIME_OK;
}

enum dos_process_runtime_status
dos_process_runtime_poison(struct dos_process_runtime *runtime)
{
	enum dos_process_runtime_status status;

	status = runtime_ready_status(runtime);
	if (status != DOS_PROCESS_RUNTIME_OK &&
	    status != DOS_PROCESS_RUNTIME_POISONED)
		return status;
	runtime->poisoned = 1u;
	return DOS_PROCESS_RUNTIME_OK;
}

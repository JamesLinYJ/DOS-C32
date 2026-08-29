// SPDX-License-Identifier: GPL-2.0-only
/* Generation-bound dormant/runnable backend ownership for DOS EXEC. */
#include "dos_exec_backend_session.h"

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

static bool capabilities_are_valid(uint32_t capabilities)
{
	return capabilities != 0u &&
	       (capabilities & ~DOS_EXEC_CAPABILITY_MASK) == 0u;
}

static bool ops_are_complete(const struct dos_exec_backend_ops *ops)
{
	return ops != NULL && identity_is_valid(ops->identity) &&
	       capabilities_are_valid(ops->capabilities) &&
	       ops->prepare != NULL && ops->release != NULL &&
	       ops->run_until_event != NULL;
}

static bool machine_is_usable(const struct dos_machine *machine)
{
	return machine != NULL && machine->ops != NULL &&
	       machine->ops->read_memory != NULL &&
	       machine->ops->write_memory != NULL &&
	       machine->context != KERNEL_OBJECT_HANDLE_INVALID &&
	       machine->address_limit != 0u &&
	       machine->address_limit <= DOS_GUEST_32_ADDRESS_LIMIT;
}

static bool cpu_state_is_zero(const struct dos_cpu_state *state)
{
	return state->eax == 0u && state->ebx == 0u && state->ecx == 0u &&
	       state->edx == 0u && state->esi == 0u && state->edi == 0u &&
	       state->ebp == 0u && state->esp == 0u && state->eip == 0u &&
	       state->eflags == 0u && state->cs == 0u && state->ss == 0u &&
	       state->ds == 0u && state->es == 0u && state->fs == 0u &&
	       state->gs == 0u && state->mode == 0u;
}

static bool handoff_is_zero(const struct dos_exec_handoff_plan *handoff)
{
	return cpu_state_is_zero(&handoff->entry_state) &&
	       handoff->stack_image.segment == 0u &&
	       handoff->stack_image.offset == 0u &&
	       bytes_are_zero(handoff->stack_image.bytes,
			      ARRAY_SIZE(handoff->stack_image.bytes)) &&
	       handoff->child_psp == 0u && handoff->format == 0u &&
	       handoff->stack_word_count == 0u &&
	       bytes_are_zero(handoff->reserved,
			      ARRAY_SIZE(handoff->reserved));
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

static bool state_is_valid(uint8_t state)
{
	return state <= (uint8_t)DOS_EXEC_BACKEND_SESSION_STATE_POISONED;
}

static bool slot_has_valid_encoding(
    const struct dos_exec_backend_session_slot *slot)
{
	bool has_adapter;
	bool has_backend;
	bool has_machine;

	if (slot == NULL ||
	    slot->generation > DOS_EXEC_BACKEND_SESSION_GENERATION_MAX ||
	    !state_is_valid(slot->state) || slot->a20_enabled > 1u ||
	    slot->has_current_state > 1u ||
	    !bytes_are_zero(slot->reserved, ARRAY_SIZE(slot->reserved)))
		return false;
	has_adapter = identity_is_valid(slot->adapter_identity) &&
		      slot->adapter_context != KERNEL_OBJECT_HANDLE_INVALID;
	has_backend = identity_is_valid(slot->backend_context);
	has_machine = identity_is_valid(slot->machine_identity) &&
		      slot->machine_context != KERNEL_OBJECT_HANDLE_INVALID &&
		      slot->machine_address_limit != 0u &&
		      slot->machine_address_limit <= DOS_GUEST_32_ADDRESS_LIMIT;
	if (slot->state == (uint8_t)DOS_EXEC_BACKEND_SESSION_VACANT)
		return slot->adapter_identity == KERNEL_OBJECT_HANDLE_INVALID &&
		       slot->adapter_context == KERNEL_OBJECT_HANDLE_INVALID &&
		       slot->backend_context == KERNEL_OBJECT_HANDLE_INVALID &&
		       slot->machine_identity == KERNEL_OBJECT_HANDLE_INVALID &&
		       slot->machine_context == KERNEL_OBJECT_HANDLE_INVALID &&
		       slot->machine_address_limit == 0u &&
		       handoff_is_zero(&slot->handoff) &&
		       cpu_state_is_zero(&slot->current_state) &&
		       slot->capabilities == 0u && slot->a20_enabled == 0u &&
		       slot->has_current_state == 0u;
	if (slot->generation == 0u || !has_adapter || !has_machine ||
	    !capabilities_are_valid(slot->capabilities) ||
	    !dos_exec_handoff_plan_has_valid_encoding(&slot->handoff) ||
	    slot->has_current_state != 1u ||
	    !dos_cpu_mode_value_is_valid(slot->current_state.mode))
		return false;
	if ((slot->state == (uint8_t)DOS_EXEC_BACKEND_SESSION_PREPARING ||
	     slot->state == (uint8_t)DOS_EXEC_BACKEND_SESSION_DORMANT) &&
	    !cpu_states_equal(&slot->current_state,
			      &slot->handoff.entry_state))
		return false;
	switch (slot->state) {
	case DOS_EXEC_BACKEND_SESSION_PREPARING:
	case DOS_EXEC_BACKEND_SESSION_STOPPED:
		return slot->backend_context == KERNEL_OBJECT_HANDLE_INVALID;
	case DOS_EXEC_BACKEND_SESSION_DORMANT:
	case DOS_EXEC_BACKEND_SESSION_RUNNABLE:
	case DOS_EXEC_BACKEND_SESSION_RUNNING:
	case DOS_EXEC_BACKEND_SESSION_EXITED:
	case DOS_EXEC_BACKEND_SESSION_RELEASING:
		return has_backend;
	case DOS_EXEC_BACKEND_SESSION_STATE_POISONED:
		return true;
	default:
		return false;
	}
}

static void clear_slot_binding(struct dos_exec_backend_session_slot *slot)
{
	size_t index;

	slot->adapter_identity = KERNEL_OBJECT_HANDLE_INVALID;
	slot->adapter_context = KERNEL_OBJECT_HANDLE_INVALID;
	slot->backend_context = KERNEL_OBJECT_HANDLE_INVALID;
	slot->machine_identity = KERNEL_OBJECT_HANDLE_INVALID;
	slot->machine_context = KERNEL_OBJECT_HANDLE_INVALID;
	slot->machine_address_limit = 0u;
	slot->handoff = (struct dos_exec_handoff_plan){0};
	slot->current_state = (struct dos_cpu_state){0};
	slot->capabilities = 0u;
	slot->a20_enabled = 0u;
	slot->has_current_state = 0u;
	for (index = 0u; index < ARRAY_SIZE(slot->reserved); ++index)
		slot->reserved[index] = 0u;
}

static void initialize_slot(struct dos_exec_backend_session_slot *slot)
{
	slot->generation = 0u;
	clear_slot_binding(slot);
	slot->state = (uint8_t)DOS_EXEC_BACKEND_SESSION_VACANT;
}

static void vacate_slot(struct dos_exec_backend_session_slot *slot)
{
	clear_slot_binding(slot);
	slot->state = (uint8_t)DOS_EXEC_BACKEND_SESSION_VACANT;
}

static enum dos_exec_backend_session_status
validate_table(const struct dos_exec_backend_session_table *table)
{
	size_t index;

	if (table == NULL)
		return DOS_EXEC_BACKEND_SESSION_INVALID_ARGUMENT;
	if (table->constructed != 1u || table->initialized > 1u ||
	    table->poisoned > 1u ||
	    !bytes_are_zero(table->reserved, ARRAY_SIZE(table->reserved)))
		return DOS_EXEC_BACKEND_SESSION_INVALID_ARGUMENT;
	if (table->initialized == 0u)
		return DOS_EXEC_BACKEND_SESSION_NOT_INITIALIZED;
	if (!identity_is_valid(table->identity))
		return DOS_EXEC_BACKEND_SESSION_POISONED;
	for (index = 0u; index < ARRAY_SIZE(table->slots); ++index) {
		if (!slot_has_valid_encoding(&table->slots[index]))
			return DOS_EXEC_BACKEND_SESSION_POISONED;
	}
	return table->poisoned != 0u ? DOS_EXEC_BACKEND_SESSION_POISONED
				     : DOS_EXEC_BACKEND_SESSION_OK;
}

static struct dos_exec_backend_session_handle
make_handle(uint32_t slot_index, uint64_t generation)
{
	struct dos_exec_backend_session_handle handle;

	handle.value = (generation << DOS_EXEC_BACKEND_SESSION_SLOT_BITS) |
		       (uint64_t)(slot_index + 1u);
	return handle;
}

static enum dos_exec_backend_session_status decode_handle(
    struct dos_exec_backend_session_handle handle, uint32_t *slot_index,
    uint64_t *generation)
{
	uint32_t encoded_slot;
	uint64_t decoded_generation;

	if (slot_index == NULL || generation == NULL || handle.value == 0u ||
	    handle.value == KERNEL_OBJECT_HANDLE_INVALID)
		return DOS_EXEC_BACKEND_SESSION_STALE_HANDLE;
	encoded_slot =
	    (uint32_t)(handle.value & DOS_EXEC_BACKEND_SESSION_SLOT_MASK);
	decoded_generation =
	    handle.value >> DOS_EXEC_BACKEND_SESSION_SLOT_BITS;
	if (encoded_slot == 0u ||
	    encoded_slot > DOS_EXEC_BACKEND_SESSION_SLOT_COUNT ||
	    decoded_generation == 0u ||
	    decoded_generation > DOS_EXEC_BACKEND_SESSION_GENERATION_MAX)
		return DOS_EXEC_BACKEND_SESSION_STALE_HANDLE;
	*slot_index = encoded_slot - 1u;
	*generation = decoded_generation;
	return DOS_EXEC_BACKEND_SESSION_OK;
}

static enum dos_exec_backend_session_status find_slot(
    const struct dos_exec_backend_session_table *table,
    struct dos_exec_backend_session_handle handle, uint32_t *slot_index_out)
{
	const struct dos_exec_backend_session_slot *slot;
	enum dos_exec_backend_session_status status;
	uint64_t generation;
	uint32_t slot_index;

	if (slot_index_out == NULL)
		return DOS_EXEC_BACKEND_SESSION_INVALID_ARGUMENT;
	status = validate_table(table);
	if (status != DOS_EXEC_BACKEND_SESSION_OK)
		return status;
	status = decode_handle(handle, &slot_index, &generation);
	if (status != DOS_EXEC_BACKEND_SESSION_OK)
		return status;
	slot = &table->slots[slot_index];
	if (slot->generation != generation ||
	    slot->state == (uint8_t)DOS_EXEC_BACKEND_SESSION_VACANT)
		return DOS_EXEC_BACKEND_SESSION_STALE_HANDLE;
	*slot_index_out = slot_index;
	return DOS_EXEC_BACKEND_SESSION_OK;
}

static enum dos_exec_backend_session_status find_bound_slot(
    const struct dos_exec_backend_session_table *table,
    struct dos_exec_backend_session_handle handle,
    const struct dos_exec_backend_ops *ops,
    kernel_object_handle_t adapter_context, uint32_t *slot_index_out)
{
	const struct dos_exec_backend_session_slot *slot;
	enum dos_exec_backend_session_status status;
	uint32_t slot_index;

	if (!ops_are_complete(ops) ||
	    adapter_context == KERNEL_OBJECT_HANDLE_INVALID)
		return DOS_EXEC_BACKEND_SESSION_INVALID_ARGUMENT;
	status = find_slot(table, handle, &slot_index);
	if (status != DOS_EXEC_BACKEND_SESSION_OK)
		return status;
	slot = &table->slots[slot_index];
	if (slot->adapter_identity != ops->identity)
		return DOS_EXEC_BACKEND_SESSION_IDENTITY_MISMATCH;
	if (slot->adapter_context != adapter_context)
		return DOS_EXEC_BACKEND_SESSION_CONTEXT_MISMATCH;
	if (slot->capabilities != ops->capabilities)
		return DOS_EXEC_BACKEND_SESSION_IDENTITY_MISMATCH;
	if (slot->state == (uint8_t)DOS_EXEC_BACKEND_SESSION_STATE_POISONED)
		return DOS_EXEC_BACKEND_SESSION_POISONED;
	*slot_index_out = slot_index;
	return DOS_EXEC_BACKEND_SESSION_OK;
}

enum dos_exec_backend_session_status dos_exec_backend_session_table_construct(
    struct dos_exec_backend_session_table *table)
{
	size_t index;

	if (table == NULL)
		return DOS_EXEC_BACKEND_SESSION_INVALID_ARGUMENT;
	for (index = 0u; index < ARRAY_SIZE(table->slots); ++index)
		initialize_slot(&table->slots[index]);
	table->identity = KERNEL_OBJECT_HANDLE_INVALID;
	table->initialized = 0u;
	table->constructed = 1u;
	table->poisoned = 0u;
	for (index = 0u; index < ARRAY_SIZE(table->reserved); ++index)
		table->reserved[index] = 0u;
	return DOS_EXEC_BACKEND_SESSION_OK;
}

enum dos_exec_backend_session_status dos_exec_backend_session_table_initialize(
    struct dos_exec_backend_session_table *table,
    kernel_object_handle_t identity)
{
	size_t index;

	if (table == NULL || table->constructed != 1u ||
	    table->initialized > 1u || table->poisoned != 0u ||
	    !identity_is_valid(identity) ||
	    !bytes_are_zero(table->reserved, ARRAY_SIZE(table->reserved)))
		return DOS_EXEC_BACKEND_SESSION_INVALID_ARGUMENT;
	if (table->initialized != 0u)
		return DOS_EXEC_BACKEND_SESSION_INVALID_STATE;
	if (table->identity != KERNEL_OBJECT_HANDLE_INVALID)
		return DOS_EXEC_BACKEND_SESSION_INVALID_ARGUMENT;
	for (index = 0u; index < ARRAY_SIZE(table->slots); ++index)
		initialize_slot(&table->slots[index]);
	table->identity = identity;
	table->initialized = 1u;
	return DOS_EXEC_BACKEND_SESSION_OK;
}

bool dos_exec_backend_session_table_is_drained(
    const struct dos_exec_backend_session_table *table)
{
	size_t index;

	if (validate_table(table) != DOS_EXEC_BACKEND_SESSION_OK)
		return false;
	for (index = 0u; index < ARRAY_SIZE(table->slots); ++index) {
		uint8_t state = table->slots[index].state;

		if (state != (uint8_t)DOS_EXEC_BACKEND_SESSION_VACANT &&
		    state != (uint8_t)DOS_EXEC_BACKEND_SESSION_STOPPED)
			return false;
	}
	return true;
}

static enum dos_exec_backend_session_status reserve_slot(
    struct dos_exec_backend_session_table *table,
    const struct dos_exec_backend_ops *ops,
    kernel_object_handle_t adapter_context,
    kernel_object_handle_t machine_identity,
    const struct dos_machine *machine,
    const struct dos_exec_handoff_plan *handoff, uint32_t *slot_index)
{
	bool every_generation_exhausted = true;
	uint32_t index;

	for (index = 0u; index < DOS_EXEC_BACKEND_SESSION_SLOT_COUNT; ++index) {
		struct dos_exec_backend_session_slot *slot = &table->slots[index];

		if (slot->generation <
		    DOS_EXEC_BACKEND_SESSION_GENERATION_MAX)
			every_generation_exhausted = false;
		if (slot->state !=
			(uint8_t)DOS_EXEC_BACKEND_SESSION_VACANT ||
		    slot->generation >=
			DOS_EXEC_BACKEND_SESSION_GENERATION_MAX)
			continue;
		++slot->generation;
		slot->adapter_identity = ops->identity;
		slot->adapter_context = adapter_context;
		slot->backend_context = KERNEL_OBJECT_HANDLE_INVALID;
		slot->machine_identity = machine_identity;
		slot->machine_context = machine->context;
		slot->machine_address_limit = machine->address_limit;
		slot->handoff = *handoff;
		slot->current_state = handoff->entry_state;
		slot->capabilities = ops->capabilities;
		slot->a20_enabled = (uint8_t)(machine->a20_enabled ? 1u : 0u);
		slot->has_current_state = 1u;
		slot->state = (uint8_t)DOS_EXEC_BACKEND_SESSION_PREPARING;
		*slot_index = index;
		return DOS_EXEC_BACKEND_SESSION_OK;
	}
	return every_generation_exhausted
		   ? DOS_EXEC_BACKEND_SESSION_GENERATION_EXHAUSTED
		   : DOS_EXEC_BACKEND_SESSION_NO_SLOT;
}

static void poison_slot(struct dos_exec_backend_session_table *table,
			struct dos_exec_backend_session_slot *slot,
			kernel_object_handle_t possible_backend_context)
{
	if (possible_backend_context != 0u)
		slot->backend_context = possible_backend_context;
	slot->state = (uint8_t)DOS_EXEC_BACKEND_SESSION_STATE_POISONED;
	table->poisoned = 1u;
}

enum dos_exec_backend_session_status dos_exec_backend_session_prepare(
    struct dos_exec_backend_session_table *table,
    const struct dos_exec_backend_ops *ops,
    kernel_object_handle_t adapter_context,
    kernel_object_handle_t machine_identity,
    const struct dos_machine *machine,
    const struct dos_exec_handoff_plan *handoff,
    struct dos_exec_backend_session_handle *handle, uint32_t *failure_detail)
{
	struct dos_exec_backend_prepare_result prepared = {
	    .backend_context = KERNEL_OBJECT_HANDLE_INVALID,
	    .failure_detail = 0u,
	    .reserved = {0u},
	};
	struct dos_exec_backend_session_handle prepared_handle;
	struct dos_exec_backend_session_slot *slot;
	enum dos_exec_backend_prepare_status adapter_status;
	enum dos_exec_backend_session_status status;
	uint32_t slot_index;

	if (!ops_are_complete(ops) ||
	    adapter_context == KERNEL_OBJECT_HANDLE_INVALID ||
	    !identity_is_valid(machine_identity) || !machine_is_usable(machine) ||
	    !dos_exec_handoff_plan_has_valid_encoding(handoff) || handle == NULL ||
	    failure_detail == NULL)
		return DOS_EXEC_BACKEND_SESSION_INVALID_ARGUMENT;
	status = validate_table(table);
	if (status != DOS_EXEC_BACKEND_SESSION_OK)
		return status;
	status = reserve_slot(table, ops, adapter_context, machine_identity,
			      machine, handoff, &slot_index);
	if (status != DOS_EXEC_BACKEND_SESSION_OK)
		return status;
	slot = &table->slots[slot_index];
	prepared_handle = make_handle(slot_index, slot->generation);

	adapter_status =
	    ops->prepare(adapter_context, machine, machine_identity, handoff,
			 &prepared);
	if (adapter_status == DOS_EXEC_BACKEND_REJECTED &&
	    (prepared.backend_context == KERNEL_OBJECT_HANDLE_INVALID ||
	     prepared.backend_context == 0u) &&
	    bytes_are_zero(prepared.reserved, ARRAY_SIZE(prepared.reserved))) {
		uint32_t detail = prepared.failure_detail;

		vacate_slot(slot);
		*failure_detail = detail;
		return DOS_EXEC_BACKEND_SESSION_PREPARE_REJECTED;
	}
	if (adapter_status != DOS_EXEC_BACKEND_PREPARED ||
	    !identity_is_valid(prepared.backend_context) ||
	    prepared.failure_detail != 0u ||
	    !bytes_are_zero(prepared.reserved, ARRAY_SIZE(prepared.reserved))) {
		poison_slot(table, slot, prepared.backend_context);
		return DOS_EXEC_BACKEND_SESSION_POISONED;
	}
	slot->backend_context = prepared.backend_context;
	slot->state = (uint8_t)DOS_EXEC_BACKEND_SESSION_DORMANT;
	*handle = prepared_handle;
	*failure_detail = 0u;
	return DOS_EXEC_BACKEND_SESSION_OK;
}

static enum dos_exec_backend_session_status validate_machine_binding_state(
    const struct dos_exec_backend_session_slot *slot,
    kernel_object_handle_t machine_identity,
    const struct dos_machine *machine, bool allow_a20_transition)
{
	if (!identity_is_valid(machine_identity) || !machine_is_usable(machine))
		return DOS_EXEC_BACKEND_SESSION_INVALID_ARGUMENT;
	if (slot->machine_identity != machine_identity ||
	    slot->machine_context != machine->context ||
	    slot->machine_address_limit != machine->address_limit ||
	    (!allow_a20_transition &&
	     slot->a20_enabled !=
		     (uint8_t)(machine->a20_enabled ? 1u : 0u)))
		return DOS_EXEC_BACKEND_SESSION_MACHINE_MISMATCH;
	return DOS_EXEC_BACKEND_SESSION_OK;
}

static enum dos_exec_backend_session_status validate_machine_binding(
    const struct dos_exec_backend_session_slot *slot,
    kernel_object_handle_t machine_identity,
    const struct dos_machine *machine)
{
	return validate_machine_binding_state(slot, machine_identity, machine,
					      false);
}

enum dos_exec_backend_session_status dos_exec_backend_session_preflight_publish(
    const struct dos_exec_backend_session_table *table,
    struct dos_exec_backend_session_handle handle,
    const struct dos_exec_backend_ops *ops,
    kernel_object_handle_t adapter_context,
    kernel_object_handle_t machine_identity,
    const struct dos_machine *machine,
    const struct dos_exec_handoff_plan *expected_handoff)
{
	const struct dos_exec_backend_session_slot *slot;
	enum dos_exec_backend_session_status status;
	uint32_t slot_index;

	if (!dos_exec_handoff_plan_has_valid_encoding(expected_handoff))
		return DOS_EXEC_BACKEND_SESSION_INVALID_ARGUMENT;
	status = find_bound_slot(table, handle, ops, adapter_context,
				 &slot_index);
	if (status != DOS_EXEC_BACKEND_SESSION_OK)
		return status;
	slot = &table->slots[slot_index];
	status = validate_machine_binding(slot, machine_identity, machine);
	if (status != DOS_EXEC_BACKEND_SESSION_OK)
		return status;
	if (slot->state != (uint8_t)DOS_EXEC_BACKEND_SESSION_DORMANT)
		return DOS_EXEC_BACKEND_SESSION_INVALID_STATE;
	return dos_exec_handoff_plans_equal(&slot->handoff, expected_handoff)
		   ? DOS_EXEC_BACKEND_SESSION_OK
		   : DOS_EXEC_BACKEND_SESSION_IDENTITY_MISMATCH;
}

enum dos_exec_backend_session_status dos_exec_backend_session_publish(
    struct dos_exec_backend_session_table *table,
    struct dos_exec_backend_session_handle handle,
    const struct dos_exec_backend_ops *ops,
    kernel_object_handle_t adapter_context,
    kernel_object_handle_t machine_identity,
    const struct dos_machine *machine,
    const struct dos_exec_handoff_plan *expected_handoff)
{
	enum dos_exec_backend_session_status status;
	uint64_t generation;
	uint32_t slot_index;

	status = dos_exec_backend_session_preflight_publish(
	    table, handle, ops, adapter_context, machine_identity, machine,
	    expected_handoff);
	if (status != DOS_EXEC_BACKEND_SESSION_OK)
		return status;
	status = decode_handle(handle, &slot_index, &generation);
	/* The pure preflight already decoded the same stable handle. */
	if (status != DOS_EXEC_BACKEND_SESSION_OK ||
	    table->slots[slot_index].generation != generation)
		return DOS_EXEC_BACKEND_SESSION_POISONED;
	table->slots[slot_index].state =
	    (uint8_t)DOS_EXEC_BACKEND_SESSION_RUNNABLE;
	return DOS_EXEC_BACKEND_SESSION_OK;
}

static bool io_width_value_is_valid(uint8_t width)
{
	return width == (uint8_t)DOS_IO_WIDTH_8 ||
	       width == (uint8_t)DOS_IO_WIDTH_16 ||
	       width == (uint8_t)DOS_IO_WIDTH_32;
}

static bool event_has_valid_encoding(const struct dos_execution_event *event)
{
	if (event == NULL ||
	    event->kind > (uint32_t)DOS_EXEC_EVENT_FAULT ||
	    event->io_write > 1u ||
	    !bytes_are_zero(event->reserved, ARRAY_SIZE(event->reserved)))
		return false;
	if (event->kind == (uint32_t)DOS_EXEC_EVENT_PORT_IO)
		return io_width_value_is_valid(event->io_width);
	if (event->port != 0u || event->io_width != 0u ||
	    event->io_write != 0u)
		return false;
	if (event->kind == (uint32_t)DOS_EXEC_EVENT_MODE_CHANGE ||
	    event->kind == (uint32_t)DOS_EXEC_EVENT_HALTED ||
	    event->kind == (uint32_t)DOS_EXEC_EVENT_EXITED)
		return event->vector == 0u;
	return true;
}

enum dos_exec_backend_session_status dos_exec_backend_session_run_until_event(
    struct dos_exec_backend_session_table *table,
    struct dos_exec_backend_session_handle handle,
    const struct dos_exec_backend_ops *ops,
    kernel_object_handle_t adapter_context,
    kernel_object_handle_t machine_identity,
    const struct dos_machine *machine, struct dos_cpu_state *state,
    struct dos_execution_event *event)
{
	struct dos_exec_backend_session_slot *slot;
	struct dos_cpu_state prepared_state;
	struct dos_execution_event prepared_event = {0};
	enum dos_exec_backend_run_status adapter_status;
	enum dos_exec_backend_session_status status;
	uint32_t slot_index;

	if (state == NULL || event == NULL)
		return DOS_EXEC_BACKEND_SESSION_INVALID_ARGUMENT;
	status = find_bound_slot(table, handle, ops, adapter_context,
				 &slot_index);
	if (status != DOS_EXEC_BACKEND_SESSION_OK)
		return status;
	slot = &table->slots[slot_index];
	status = validate_machine_binding(slot, machine_identity, machine);
	if (status != DOS_EXEC_BACKEND_SESSION_OK)
		return status;
	if (slot->state != (uint8_t)DOS_EXEC_BACKEND_SESSION_RUNNABLE)
		return DOS_EXEC_BACKEND_SESSION_INVALID_STATE;
	prepared_state = slot->current_state;
	slot->state = (uint8_t)DOS_EXEC_BACKEND_SESSION_RUNNING;
	adapter_status = ops->run_until_event(
	    adapter_context, slot->backend_context, machine_identity, machine,
	    &prepared_state, &prepared_event);
	if (adapter_status != DOS_EXEC_BACKEND_EVENT ||
	    !dos_cpu_mode_value_is_valid(prepared_state.mode) ||
	    !event_has_valid_encoding(&prepared_event)) {
		poison_slot(table, slot, slot->backend_context);
		return DOS_EXEC_BACKEND_SESSION_POISONED;
	}
	slot->current_state = prepared_state;
	slot->state = prepared_event.kind == (uint32_t)DOS_EXEC_EVENT_EXITED
			  ? (uint8_t)DOS_EXEC_BACKEND_SESSION_EXITED
			  : (uint8_t)DOS_EXEC_BACKEND_SESSION_RUNNABLE;
	*state = prepared_state;
	*event = prepared_event;
	return DOS_EXEC_BACKEND_SESSION_OK;
}

enum dos_exec_backend_session_status dos_exec_backend_session_replace_state(
    struct dos_exec_backend_session_table *table,
    struct dos_exec_backend_session_handle handle,
    const struct dos_exec_backend_ops *ops,
    kernel_object_handle_t adapter_context,
    kernel_object_handle_t machine_identity,
    const struct dos_machine *machine,
    const struct dos_cpu_state *expected_state,
    const struct dos_cpu_state *replacement_state)
{
	struct dos_exec_backend_session_slot *slot;
	enum dos_exec_backend_state_commit_status adapter_status;
	enum dos_exec_backend_session_status status;
	uint32_t slot_index;

	if (expected_state == NULL || replacement_state == NULL ||
	    !dos_cpu_mode_value_is_valid(expected_state->mode) ||
	    !dos_cpu_mode_value_is_valid(replacement_state->mode))
		return DOS_EXEC_BACKEND_SESSION_INVALID_ARGUMENT;
	status = find_bound_slot(table, handle, ops, adapter_context,
				 &slot_index);
	if (status != DOS_EXEC_BACKEND_SESSION_OK)
		return status;
	slot = &table->slots[slot_index];
	/* DOS services such as XMS may commit a virtual A20 transition while the
	 * backend is stopped at this precise event.  Identity, context and address
	 * space remain immutable; the new transform is published with the CPU
	 * replacement state below. */
	status = validate_machine_binding_state(slot, machine_identity, machine,
						true);
	if (status != DOS_EXEC_BACKEND_SESSION_OK)
		return status;
	if (slot->state != (uint8_t)DOS_EXEC_BACKEND_SESSION_RUNNABLE)
		return DOS_EXEC_BACKEND_SESSION_INVALID_STATE;
	if (!cpu_states_equal(&slot->current_state, expected_state))
		return DOS_EXEC_BACKEND_SESSION_STATE_MISMATCH;
	if (ops->commit_state_replacement != NULL) {
		adapter_status = ops->commit_state_replacement(
			adapter_context, slot->backend_context, expected_state,
			replacement_state);
		if (adapter_status == DOS_EXEC_BACKEND_STATE_REJECTED)
			return DOS_EXEC_BACKEND_SESSION_INVALID_STATE;
		if (adapter_status != DOS_EXEC_BACKEND_STATE_COMMITTED) {
			poison_slot(table, slot, slot->backend_context);
			return DOS_EXEC_BACKEND_SESSION_POISONED;
		}
	}
	slot->current_state = *replacement_state;
	slot->a20_enabled = (uint8_t)(machine->a20_enabled ? 1u : 0u);
	return DOS_EXEC_BACKEND_SESSION_OK;
}

enum dos_exec_backend_session_status dos_exec_backend_session_stop(
    struct dos_exec_backend_session_table *table,
    struct dos_exec_backend_session_handle handle,
    const struct dos_exec_backend_ops *ops,
    kernel_object_handle_t adapter_context)
{
	struct dos_exec_backend_session_slot *slot;
	enum dos_exec_backend_release_status adapter_status;
	enum dos_exec_backend_session_status status;
	uint8_t previous_state;
	uint32_t slot_index;

	status = find_bound_slot(table, handle, ops, adapter_context,
				 &slot_index);
	if (status != DOS_EXEC_BACKEND_SESSION_OK)
		return status;
	slot = &table->slots[slot_index];
	if (slot->state != (uint8_t)DOS_EXEC_BACKEND_SESSION_DORMANT &&
	    slot->state != (uint8_t)DOS_EXEC_BACKEND_SESSION_RUNNABLE &&
	    slot->state != (uint8_t)DOS_EXEC_BACKEND_SESSION_EXITED)
		return DOS_EXEC_BACKEND_SESSION_INVALID_STATE;
	previous_state = slot->state;
	slot->state = (uint8_t)DOS_EXEC_BACKEND_SESSION_RELEASING;
	adapter_status = ops->release(adapter_context, slot->backend_context);
	if (adapter_status == DOS_EXEC_BACKEND_RETAINED) {
		slot->state = previous_state;
		return DOS_EXEC_BACKEND_SESSION_RELEASE_RETAINED;
	}
	if (adapter_status != DOS_EXEC_BACKEND_RELEASED) {
		poison_slot(table, slot, slot->backend_context);
		return DOS_EXEC_BACKEND_SESSION_POISONED;
	}
	slot->backend_context = KERNEL_OBJECT_HANDLE_INVALID;
	slot->state = (uint8_t)DOS_EXEC_BACKEND_SESSION_STOPPED;
	return DOS_EXEC_BACKEND_SESSION_OK;
}

enum dos_exec_backend_session_status dos_exec_backend_session_retire(
    struct dos_exec_backend_session_table *table,
    struct dos_exec_backend_session_handle handle)
{
	struct dos_exec_backend_session_slot *slot;
	enum dos_exec_backend_session_status status;
	uint32_t slot_index;

	status = find_slot(table, handle, &slot_index);
	if (status != DOS_EXEC_BACKEND_SESSION_OK)
		return status;
	slot = &table->slots[slot_index];
	if (slot->state != (uint8_t)DOS_EXEC_BACKEND_SESSION_STOPPED)
		return DOS_EXEC_BACKEND_SESSION_INVALID_STATE;
	vacate_slot(slot);
	return DOS_EXEC_BACKEND_SESSION_OK;
}

enum dos_exec_backend_session_status dos_exec_backend_session_get_state(
    const struct dos_exec_backend_session_table *table,
    struct dos_exec_backend_session_handle handle,
    enum dos_exec_backend_session_state *state)
{
	enum dos_exec_backend_session_status status;
	uint32_t slot_index;

	if (state == NULL)
		return DOS_EXEC_BACKEND_SESSION_INVALID_ARGUMENT;
	status = find_slot(table, handle, &slot_index);
	if (status != DOS_EXEC_BACKEND_SESSION_OK)
		return status;
	*state = (enum dos_exec_backend_session_state)table->slots[slot_index].state;
	return DOS_EXEC_BACKEND_SESSION_OK;
}

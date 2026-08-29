// SPDX-License-Identifier: GPL-2.0-only
/*
 * LIFO undo for bounded guest writes made by one serialized DOS EXEC.
 * Far-address splitting and A20 behavior stay exclusively in dos_machine.
 */
#include "dos_exec_journal.h"

static bool identity_is_valid(kernel_object_handle_t identity)
{
	return identity != 0u && identity != KERNEL_OBJECT_HANDLE_INVALID;
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

static void clear_record(struct dos_exec_journal_record *record)
{
	size_t index;

	record->segment = 0u;
	record->offset = 0u;
	record->count = 0u;
	for (index = 0u; index < ARRAY_SIZE(record->reserved); ++index)
		record->reserved[index] = 0u;
	for (index = 0u; index < ARRAY_SIZE(record->old_bytes); ++index)
		record->old_bytes[index] = 0u;
}

static void clear_records(struct dos_exec_journal *journal)
{
	size_t index;

	for (index = 0u; index < ARRAY_SIZE(journal->records); ++index)
		clear_record(&journal->records[index]);
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

static bool record_is_clear(const struct dos_exec_journal_record *record)
{
	return record->segment == 0u && record->offset == 0u &&
	       record->count == 0u &&
	       bytes_are_zero(record->reserved, ARRAY_SIZE(record->reserved)) &&
	       bytes_are_zero(record->old_bytes, ARRAY_SIZE(record->old_bytes));
}

static bool record_is_valid(const struct dos_exec_journal_record *record)
{
	if (record->count == 0u ||
	    record->count > DOS_EXEC_JOURNAL_RECORD_BYTES ||
	    !bytes_are_zero(record->reserved, ARRAY_SIZE(record->reserved)))
		return false;
	return bytes_are_zero(&record->old_bytes[record->count],
			      DOS_EXEC_JOURNAL_RECORD_BYTES - record->count);
}

static bool state_is_valid(uint8_t state)
{
	return state == (uint8_t)DOS_EXEC_JOURNAL_STATE_UNINITIALIZED ||
	       state == (uint8_t)DOS_EXEC_JOURNAL_STATE_STAGING ||
	       state == (uint8_t)DOS_EXEC_JOURNAL_STATE_SEALED ||
	       state == (uint8_t)DOS_EXEC_JOURNAL_STATE_ABORTED ||
	       state == (uint8_t)DOS_EXEC_JOURNAL_STATE_POISONED;
}

bool dos_exec_journal_has_valid_encoding(const struct dos_exec_journal *journal)
{
	size_t index;

	if (journal == NULL || journal->constructed != 1u ||
	    journal->reserved != 0u || journal->a20_enabled > 1u ||
	    !state_is_valid(journal->state) ||
	    journal->record_count > DOS_EXEC_JOURNAL_RECORD_CAPACITY)
		return false;
	if (journal->state == DOS_EXEC_JOURNAL_STATE_UNINITIALIZED) {
		if (journal->machine_identity != KERNEL_OBJECT_HANDLE_INVALID ||
		    journal->machine_context != KERNEL_OBJECT_HANDLE_INVALID ||
		    journal->machine_address_limit != 0u ||
		    journal->record_count != 0u || journal->a20_enabled != 0u)
			return false;
	} else if (!identity_is_valid(journal->machine_identity) ||
		   journal->machine_context == KERNEL_OBJECT_HANDLE_INVALID ||
		   journal->machine_address_limit == 0u ||
		   journal->machine_address_limit >
		       DOS_GUEST_32_ADDRESS_LIMIT) {
		return false;
	}
	if ((journal->state == DOS_EXEC_JOURNAL_STATE_SEALED ||
	     journal->state == DOS_EXEC_JOURNAL_STATE_ABORTED) &&
	    journal->record_count != 0u)
		return false;
	for (index = 0u; index < ARRAY_SIZE(journal->records); ++index) {
		if (index < journal->record_count) {
			if (!record_is_valid(&journal->records[index]))
				return false;
		} else if (!record_is_clear(&journal->records[index])) {
			return false;
		}
	}
	return true;
}

static enum dos_exec_journal_status
validate_binding(const struct dos_exec_journal *journal,
		 kernel_object_handle_t machine_identity,
		 const struct dos_machine *machine)
{
	if (!dos_exec_journal_has_valid_encoding(journal) ||
	    !identity_is_valid(machine_identity) || !machine_is_usable(machine))
		return DOS_EXEC_JOURNAL_INVALID_ARGUMENT;
	if (journal->state == DOS_EXEC_JOURNAL_STATE_POISONED)
		return DOS_EXEC_JOURNAL_POISONED;
	if (journal->state == DOS_EXEC_JOURNAL_STATE_UNINITIALIZED)
		return DOS_EXEC_JOURNAL_INVALID_STATE;
	if (machine_identity != journal->machine_identity)
		return DOS_EXEC_JOURNAL_IDENTITY_MISMATCH;
	if (machine->context != journal->machine_context)
		return DOS_EXEC_JOURNAL_CONTEXT_MISMATCH;
	if (machine->address_limit != journal->machine_address_limit ||
	    (uint8_t)(machine->a20_enabled ? 1u : 0u) != journal->a20_enabled)
		return DOS_EXEC_JOURNAL_MACHINE_MISMATCH;
	return DOS_EXEC_JOURNAL_OK;
}

enum dos_exec_journal_status
dos_exec_journal_construct(struct dos_exec_journal *journal)
{
	if (journal == NULL)
		return DOS_EXEC_JOURNAL_INVALID_ARGUMENT;
	journal->machine_identity = KERNEL_OBJECT_HANDLE_INVALID;
	journal->machine_context = KERNEL_OBJECT_HANDLE_INVALID;
	journal->machine_address_limit = 0u;
	journal->record_count = 0u;
	journal->state = DOS_EXEC_JOURNAL_STATE_UNINITIALIZED;
	journal->constructed = 1u;
	journal->a20_enabled = 0u;
	journal->reserved = 0u;
	clear_records(journal);
	return DOS_EXEC_JOURNAL_OK;
}

enum dos_exec_journal_status
dos_exec_journal_initialize(struct dos_exec_journal *journal,
			    kernel_object_handle_t machine_identity,
			    const struct dos_machine *machine)
{
	if (!dos_exec_journal_has_valid_encoding(journal) ||
	    !identity_is_valid(machine_identity) || !machine_is_usable(machine))
		return DOS_EXEC_JOURNAL_INVALID_ARGUMENT;
	if (journal->state == DOS_EXEC_JOURNAL_STATE_POISONED)
		return DOS_EXEC_JOURNAL_POISONED;
	if (journal->state != DOS_EXEC_JOURNAL_STATE_UNINITIALIZED)
		return DOS_EXEC_JOURNAL_INVALID_STATE;

	journal->machine_identity = machine_identity;
	journal->machine_context = machine->context;
	journal->machine_address_limit = machine->address_limit;
	journal->a20_enabled = (uint8_t)(machine->a20_enabled ? 1u : 0u);
	journal->state = DOS_EXEC_JOURNAL_STATE_STAGING;
	return DOS_EXEC_JOURNAL_OK;
}

enum dos_exec_journal_status dos_exec_journal_stage_replace_far(
    struct dos_exec_journal *journal, kernel_object_handle_t machine_identity,
    const struct dos_machine *machine, uint16_t segment, uint16_t offset,
    const void *source, size_t source_capacity, size_t count)
{
	struct dos_exec_journal_record prepared;
	enum dos_exec_journal_status journal_status;
	enum dos_machine_status machine_status;
	size_t index;

	journal_status = validate_binding(journal, machine_identity, machine);
	if (journal_status != DOS_EXEC_JOURNAL_OK)
		return journal_status;
	if (journal->state != DOS_EXEC_JOURNAL_STATE_STAGING)
		return DOS_EXEC_JOURNAL_INVALID_STATE;
	if (source == NULL || count == 0u || count > source_capacity)
		return DOS_EXEC_JOURNAL_INVALID_ARGUMENT;
	if (count > DOS_EXEC_JOURNAL_RECORD_BYTES)
		return DOS_EXEC_JOURNAL_RECORD_TOO_LARGE;
	if (journal->record_count == DOS_EXEC_JOURNAL_RECORD_CAPACITY)
		return DOS_EXEC_JOURNAL_FULL;

	clear_record(&prepared);
	prepared.segment = segment;
	prepared.offset = offset;
	prepared.count = (uint8_t)count;
	machine_status = dos_machine_replace_far(
	    machine, segment, offset, source, source_capacity,
	    prepared.old_bytes, sizeof(prepared.old_bytes), count);
	if (machine_status == DOS_MACHINE_ROLLBACK_FAILED) {
		journal->state = DOS_EXEC_JOURNAL_STATE_POISONED;
		return DOS_EXEC_JOURNAL_POISONED;
	}
	if (machine_status == DOS_MACHINE_INVALID_ARGUMENT)
		return DOS_EXEC_JOURNAL_INVALID_ARGUMENT;
	if (machine_status != DOS_MACHINE_OK)
		return DOS_EXEC_JOURNAL_MACHINE_FAULT;

	index = (size_t)journal->record_count;
	journal->records[index].segment = prepared.segment;
	journal->records[index].offset = prepared.offset;
	journal->records[index].count = prepared.count;
	journal->records[index].reserved[0] = 0u;
	journal->records[index].reserved[1] = 0u;
	journal->records[index].reserved[2] = 0u;
	for (index = 0u; index < ARRAY_SIZE(prepared.old_bytes); ++index)
		journal->records[journal->record_count].old_bytes[index] =
		    prepared.old_bytes[index];
	++journal->record_count;
	return DOS_EXEC_JOURNAL_OK;
}

static enum dos_exec_journal_status restore_to_record_count(
    struct dos_exec_journal *journal, const struct dos_machine *machine,
    uint32_t target_count)
{
	uint8_t displaced[DOS_EXEC_JOURNAL_RECORD_BYTES];

	while (journal->record_count > target_count) {
		struct dos_exec_journal_record *record =
		    &journal->records[journal->record_count - 1u];
		enum dos_machine_status machine_status;

		machine_status = dos_machine_replace_far(
		    machine, record->segment, record->offset, record->old_bytes,
		    sizeof(record->old_bytes), displaced, sizeof(displaced),
		    record->count);
		if (machine_status != DOS_MACHINE_OK) {
			journal->state = DOS_EXEC_JOURNAL_STATE_POISONED;
			return DOS_EXEC_JOURNAL_POISONED;
		}
		--journal->record_count;
		clear_record(record);
	}
	return DOS_EXEC_JOURNAL_OK;
}

enum dos_exec_journal_status dos_exec_journal_stage_replace_far_span(
    struct dos_exec_journal *journal, kernel_object_handle_t machine_identity,
    const struct dos_machine *machine, uint16_t segment, uint16_t offset,
    const void *source, size_t source_capacity, size_t count)
{
	const uint8_t *bytes = (const uint8_t *)source;
	enum dos_exec_journal_status status;
	uint32_t initial_count;
	size_t required_records;
	size_t completed = 0u;

	status = validate_binding(journal, machine_identity, machine);
	if (status != DOS_EXEC_JOURNAL_OK)
		return status;
	if (journal->state != DOS_EXEC_JOURNAL_STATE_STAGING)
		return DOS_EXEC_JOURNAL_INVALID_STATE;
	if (source == NULL || count == 0u || count > source_capacity)
		return DOS_EXEC_JOURNAL_INVALID_ARGUMENT;
	required_records = count / DOS_EXEC_JOURNAL_RECORD_BYTES;
	if (count % DOS_EXEC_JOURNAL_RECORD_BYTES != 0u)
		++required_records;
	if (required_records > DOS_EXEC_JOURNAL_RECORD_CAPACITY -
				   journal->record_count)
		return DOS_EXEC_JOURNAL_FULL;

	initial_count = journal->record_count;
	while (completed < count) {
		size_t chunk = count - completed;

		if (chunk > DOS_EXEC_JOURNAL_RECORD_BYTES)
			chunk = DOS_EXEC_JOURNAL_RECORD_BYTES;
		status = dos_exec_journal_stage_replace_far(
		    journal, machine_identity, machine, segment,
		    (uint16_t)((uint32_t)offset + (uint32_t)completed),
		    bytes + completed, source_capacity - completed, chunk);
		if (status != DOS_EXEC_JOURNAL_OK) {
			if (journal->state == DOS_EXEC_JOURNAL_STATE_POISONED)
				return DOS_EXEC_JOURNAL_POISONED;
			if (restore_to_record_count(journal, machine,
						    initial_count) !=
			    DOS_EXEC_JOURNAL_OK)
				return DOS_EXEC_JOURNAL_POISONED;
			return status;
		}
		completed += chunk;
	}
	return DOS_EXEC_JOURNAL_OK;
}

enum dos_exec_journal_status
dos_exec_journal_abort(struct dos_exec_journal *journal,
		       kernel_object_handle_t machine_identity,
		       const struct dos_machine *machine)
{
	enum dos_exec_journal_status journal_status;

	journal_status = validate_binding(journal, machine_identity, machine);
	if (journal_status != DOS_EXEC_JOURNAL_OK)
		return journal_status;
	if (journal->state == DOS_EXEC_JOURNAL_STATE_ABORTED)
		return DOS_EXEC_JOURNAL_OK;
	if (journal->state != DOS_EXEC_JOURNAL_STATE_STAGING)
		return DOS_EXEC_JOURNAL_INVALID_STATE;

	if (restore_to_record_count(journal, machine, 0u) !=
	    DOS_EXEC_JOURNAL_OK)
		return DOS_EXEC_JOURNAL_POISONED;
	journal->state = DOS_EXEC_JOURNAL_STATE_ABORTED;
	return DOS_EXEC_JOURNAL_OK;
}

enum dos_exec_journal_status
dos_exec_journal_preflight_seal(const struct dos_exec_journal *journal,
				kernel_object_handle_t machine_identity,
				const struct dos_machine *machine)
{
	enum dos_exec_journal_status status =
	    validate_binding(journal, machine_identity, machine);

	if (status != DOS_EXEC_JOURNAL_OK)
		return status;
	return journal->state == DOS_EXEC_JOURNAL_STATE_STAGING
		   ? DOS_EXEC_JOURNAL_OK
		   : DOS_EXEC_JOURNAL_INVALID_STATE;
}

enum dos_exec_journal_status
dos_exec_journal_poison(struct dos_exec_journal *journal,
			kernel_object_handle_t machine_identity,
			const struct dos_machine *machine)
{
	if (!dos_exec_journal_has_valid_encoding(journal) ||
	    !identity_is_valid(machine_identity) || !machine_is_usable(machine))
		return DOS_EXEC_JOURNAL_INVALID_ARGUMENT;
	if (machine_identity != journal->machine_identity)
		return DOS_EXEC_JOURNAL_IDENTITY_MISMATCH;
	if (machine->context != journal->machine_context)
		return DOS_EXEC_JOURNAL_CONTEXT_MISMATCH;
	if (machine->address_limit != journal->machine_address_limit ||
	    (uint8_t)(machine->a20_enabled ? 1u : 0u) != journal->a20_enabled)
		return DOS_EXEC_JOURNAL_MACHINE_MISMATCH;
	if (journal->state == DOS_EXEC_JOURNAL_STATE_POISONED)
		return DOS_EXEC_JOURNAL_OK;
	if (journal->state != DOS_EXEC_JOURNAL_STATE_STAGING &&
	    journal->state != DOS_EXEC_JOURNAL_STATE_SEALED)
		return DOS_EXEC_JOURNAL_INVALID_STATE;
	journal->state = DOS_EXEC_JOURNAL_STATE_POISONED;
	return DOS_EXEC_JOURNAL_OK;
}

enum dos_exec_journal_status
dos_exec_journal_seal(struct dos_exec_journal *journal,
		      kernel_object_handle_t machine_identity,
		      const struct dos_machine *machine)
{
	enum dos_exec_journal_status status =
	    validate_binding(journal, machine_identity, machine);

	if (status != DOS_EXEC_JOURNAL_OK)
		return status;
	if (journal->state == DOS_EXEC_JOURNAL_STATE_SEALED)
		return DOS_EXEC_JOURNAL_OK;
	if (journal->state != DOS_EXEC_JOURNAL_STATE_STAGING)
		return DOS_EXEC_JOURNAL_INVALID_STATE;
	clear_records(journal);
	journal->record_count = 0u;
	journal->state = DOS_EXEC_JOURNAL_STATE_SEALED;
	return DOS_EXEC_JOURNAL_OK;
}

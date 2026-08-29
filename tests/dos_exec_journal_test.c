// SPDX-License-Identifier: GPL-2.0-only
/* Freestanding tests for the fixed DOS EXEC guest-write undo journal. */
#include "dos_exec_journal.h"
#include "test_entry.h"

#define GUEST_CAPACITY 0x110000u
#define MACHINE_IDENTITY ((kernel_object_handle_t)0x4d414348494e4531ull)
#define MACHINE_CONTEXT ((kernel_object_handle_t)0x4d454d4f52593031ull)
#define MAX_WRITE_LOG 128u

static uint8_t guest_memory[GUEST_CAPACITY];
static uint32_t read_calls;
static uint32_t write_calls;
static uint32_t fail_read_call;
static uint32_t fail_write_call;
static bool fail_all_writes;
static uint8_t write_value_log[MAX_WRITE_LOG];
static dos_linear_address_t write_address_log[MAX_WRITE_LOG];
static uint32_t write_log_count;

static void fill_bytes(uint8_t *bytes, size_t count, uint8_t value)
{
	size_t index;

	for (index = 0u; index < count; ++index)
		bytes[index] = value;
}

static void reset_callbacks(void)
{
	read_calls = 0u;
	write_calls = 0u;
	fail_read_call = 0u;
	fail_write_call = 0u;
	fail_all_writes = false;
	write_log_count = 0u;
}

static void reset_fixture(void)
{
	fill_bytes(guest_memory, sizeof(guest_memory), 0u);
	reset_callbacks();
}

static enum dos_machine_status backend_read(kernel_object_handle_t context,
					    dos_linear_address_t linear_address,
					    void *destination,
					    size_t destination_capacity,
					    size_t count)
{
	uint8_t *output = (uint8_t *)destination;
	size_t index;

	++read_calls;
	if (context != MACHINE_CONTEXT || destination == NULL ||
	    count > destination_capacity || linear_address > GUEST_CAPACITY ||
	    count > GUEST_CAPACITY - (size_t)linear_address ||
	    read_calls == fail_read_call)
		return DOS_MACHINE_IO_FAULT;
	for (index = 0u; index < count; ++index)
		output[index] = guest_memory[(size_t)linear_address + index];
	return DOS_MACHINE_OK;
}

static enum dos_machine_status
backend_write(kernel_object_handle_t context,
	      dos_linear_address_t linear_address, const void *source,
	      size_t source_capacity, size_t count)
{
	const uint8_t *input = (const uint8_t *)source;
	size_t index;

	++write_calls;
	if (context != MACHINE_CONTEXT || source == NULL ||
	    count > source_capacity || linear_address > GUEST_CAPACITY ||
	    count > GUEST_CAPACITY - (size_t)linear_address)
		return DOS_MACHINE_IO_FAULT;
	if (write_log_count < MAX_WRITE_LOG && count != 0u) {
		write_value_log[write_log_count] = input[0];
		write_address_log[write_log_count] = linear_address;
		++write_log_count;
	}
	if (fail_all_writes || write_calls == fail_write_call) {
		if (count != 0u)
			guest_memory[(size_t)linear_address] = 0xeeu;
		return DOS_MACHINE_IO_FAULT;
	}
	for (index = 0u; index < count; ++index)
		guest_memory[(size_t)linear_address + index] = input[index];
	return DOS_MACHINE_OK;
}

static const struct dos_machine_ops machine_ops = {
    .read_memory = backend_read,
    .write_memory = backend_write,
    .read_port = NULL,
    .write_port = NULL,
    .set_a20 = NULL,
};

static bool initialize_fixture(struct dos_exec_journal *journal,
			       struct dos_machine *machine, bool a20_enabled)
{
	return dos_machine_configure(machine, &machine_ops, MACHINE_CONTEXT,
				     GUEST_CAPACITY,
				     a20_enabled) == DOS_MACHINE_OK &&
	       dos_exec_journal_construct(journal) == DOS_EXEC_JOURNAL_OK &&
	       dos_exec_journal_initialize(journal, MACHINE_IDENTITY,
					   machine) == DOS_EXEC_JOURNAL_OK;
}

static bool range_equals(size_t address, const uint8_t *expected, size_t count)
{
	size_t index;

	for (index = 0u; index < count; ++index) {
		if (guest_memory[address + index] != expected[index])
			return false;
	}
	return true;
}

static int test_lifecycle_and_segment_zero(void)
{
	struct dos_exec_journal journal;
	struct dos_machine machine;
	uint8_t replacement = 0x5au;
	uint32_t prior_reads;
	uint32_t prior_writes;

	reset_fixture();
	if (dos_exec_journal_construct(&journal) != DOS_EXEC_JOURNAL_OK ||
	    dos_exec_journal_initialize(&journal, 0u, NULL) !=
		DOS_EXEC_JOURNAL_INVALID_ARGUMENT ||
	    journal.state != DOS_EXEC_JOURNAL_STATE_UNINITIALIZED ||
	    journal.record_count != 0u)
		return 1;
	if (!initialize_fixture(&journal, &machine, false))
		return 2;
	guest_memory[0x20u] = 0x11u;
	if (dos_exec_journal_stage_replace_far(
		&journal, MACHINE_IDENTITY, &machine, 0u, 0x20u, &replacement,
		sizeof(replacement),
		sizeof(replacement)) != DOS_EXEC_JOURNAL_OK ||
	    journal.record_count != 1u || guest_memory[0x20u] != replacement ||
	    journal.records[0].segment != 0u ||
	    journal.records[0].old_bytes[0] != 0x11u)
		return 3;
	if (dos_exec_journal_abort(&journal, MACHINE_IDENTITY, &machine) !=
		DOS_EXEC_JOURNAL_OK ||
	    journal.state != DOS_EXEC_JOURNAL_STATE_ABORTED ||
	    journal.record_count != 0u || guest_memory[0x20u] != 0x11u)
		return 4;
	prior_reads = read_calls;
	prior_writes = write_calls;
	if (dos_exec_journal_abort(&journal, MACHINE_IDENTITY, &machine) !=
		DOS_EXEC_JOURNAL_OK ||
	    read_calls != prior_reads || write_calls != prior_writes)
		return 5;
	return 0;
}

static int test_far_offset_and_a20_wraps(void)
{
	struct dos_exec_journal journal;
	struct dos_machine machine;
	uint8_t original[16];
	uint8_t replacement[16];
	size_t index;

	reset_fixture();
	if (!initialize_fixture(&journal, &machine, false))
		return 1;
	for (index = 0u; index < sizeof(original); ++index) {
		original[index] = (uint8_t)(0x20u + index);
		replacement[index] = (uint8_t)(0x80u + index);
		if (index < 8u)
			guest_memory[0x1fff8u + index] = original[index];
		else
			guest_memory[0x10000u + index - 8u] = original[index];
	}
	if (dos_exec_journal_stage_replace_far(
		&journal, MACHINE_IDENTITY, &machine, 0x1000u, 0xfff8u,
		replacement, sizeof(replacement),
		sizeof(replacement)) != DOS_EXEC_JOURNAL_OK ||
	    !range_equals(0x1fff8u, replacement, 8u) ||
	    !range_equals(0x10000u, replacement + 8u, 8u) ||
	    dos_exec_journal_abort(&journal, MACHINE_IDENTITY, &machine) !=
		DOS_EXEC_JOURNAL_OK ||
	    !range_equals(0x1fff8u, original, 8u) ||
	    !range_equals(0x10000u, original + 8u, 8u))
		return 2;

	reset_fixture();
	if (!initialize_fixture(&journal, &machine, false))
		return 3;
	for (index = 0u; index < sizeof(original); ++index) {
		original[index] = (uint8_t)(0x30u + index);
		replacement[index] = (uint8_t)(0x90u + index);
		if (index < 8u)
			guest_memory[0xffff8u + index] = original[index];
		else
			guest_memory[index - 8u] = original[index];
	}
	if (dos_exec_journal_stage_replace_far(
		&journal, MACHINE_IDENTITY, &machine, 0xffffu, 0x0008u,
		replacement, sizeof(replacement),
		sizeof(replacement)) != DOS_EXEC_JOURNAL_OK ||
	    !range_equals(0xffff8u, replacement, 8u) ||
	    !range_equals(0u, replacement + 8u, 8u) ||
	    dos_exec_journal_abort(&journal, MACHINE_IDENTITY, &machine) !=
		DOS_EXEC_JOURNAL_OK ||
	    !range_equals(0xffff8u, original, 8u) ||
	    !range_equals(0u, original + 8u, 8u))
		return 4;

	reset_fixture();
	if (!initialize_fixture(&journal, &machine, true))
		return 5;
	for (index = 0u; index < sizeof(original); ++index) {
		original[index] = (uint8_t)(0x40u + index);
		replacement[index] = (uint8_t)(0xa0u + index);
		guest_memory[0xffff8u + index] = original[index];
	}
	if (dos_exec_journal_stage_replace_far(
		&journal, MACHINE_IDENTITY, &machine, 0xffffu, 0x0008u,
		replacement, sizeof(replacement),
		sizeof(replacement)) != DOS_EXEC_JOURNAL_OK ||
	    !range_equals(0xffff8u, replacement, sizeof(replacement)) ||
	    guest_memory[0u] != 0u ||
	    dos_exec_journal_abort(&journal, MACHINE_IDENTITY, &machine) !=
		DOS_EXEC_JOURNAL_OK ||
	    !range_equals(0xffff8u, original, sizeof(original)))
		return 6;
	return 0;
}

static int test_capacity_and_checked_count(void)
{
	struct dos_exec_journal journal;
	struct dos_machine machine;
	uint8_t replacement[DOS_EXEC_JOURNAL_RECORD_BYTES + 1u];
	uint32_t prior_reads;
	uint32_t prior_writes;
	size_t index;

	reset_fixture();
	if (!initialize_fixture(&journal, &machine, false))
		return 1;
	fill_bytes(replacement, sizeof(replacement), 0x6au);
	if (dos_exec_journal_stage_replace_far(
		&journal, MACHINE_IDENTITY, &machine, 0u, 0u, replacement,
		sizeof(replacement),
		sizeof(replacement)) != DOS_EXEC_JOURNAL_RECORD_TOO_LARGE ||
	    dos_exec_journal_stage_replace_far(
		&journal, MACHINE_IDENTITY, &machine, 0u, 0u, replacement,
		sizeof(replacement), 0u) != DOS_EXEC_JOURNAL_INVALID_ARGUMENT ||
	    read_calls != 0u || write_calls != 0u || journal.record_count != 0u)
		return 2;
	for (index = 0u; index < DOS_EXEC_JOURNAL_RECORD_CAPACITY; ++index) {
		replacement[0] = (uint8_t)(index + 1u);
		if (dos_exec_journal_stage_replace_far(
			&journal, MACHINE_IDENTITY, &machine, 0u,
			(uint16_t)(0x100u + index), replacement,
			sizeof(replacement), 1u) != DOS_EXEC_JOURNAL_OK)
			return 3;
	}
	prior_reads = read_calls;
	prior_writes = write_calls;
	if (journal.record_count != DOS_EXEC_JOURNAL_RECORD_CAPACITY ||
	    dos_exec_journal_stage_replace_far(
		&journal, MACHINE_IDENTITY, &machine, 0u, 0x200u, replacement,
		sizeof(replacement), 1u) != DOS_EXEC_JOURNAL_FULL ||
	    read_calls != prior_reads || write_calls != prior_writes ||
	    journal.record_count != DOS_EXEC_JOURNAL_RECORD_CAPACITY)
		return 4;
	if (dos_exec_journal_abort(&journal, MACHINE_IDENTITY, &machine) !=
	    DOS_EXEC_JOURNAL_OK)
		return 5;
	for (index = 0u; index < DOS_EXEC_JOURNAL_RECORD_CAPACITY; ++index) {
		if (guest_memory[0x100u + index] != 0u)
			return 6;
	}
	return 0;
}

static int test_stage_failures_and_sticky_poison(void)
{
	struct dos_exec_journal journal;
	struct dos_machine machine;
	uint8_t replacement = 0x77u;
	uint32_t prior_reads;
	uint32_t prior_writes;

	reset_fixture();
	if (!initialize_fixture(&journal, &machine, false))
		return 1;
	guest_memory[0x300u] = 0x22u;
	fail_write_call = 1u;
	if (dos_exec_journal_stage_replace_far(
		&journal, MACHINE_IDENTITY, &machine, 0u, 0x300u, &replacement,
		sizeof(replacement), 1u) != DOS_EXEC_JOURNAL_MACHINE_FAULT ||
	    journal.state != DOS_EXEC_JOURNAL_STATE_STAGING ||
	    journal.record_count != 0u || guest_memory[0x300u] != 0x22u ||
	    write_calls != 2u)
		return 2;
	fail_write_call = 0u;
	if (dos_exec_journal_stage_replace_far(
		&journal, MACHINE_IDENTITY, &machine, 0u, 0x300u, &replacement,
		sizeof(replacement), 1u) != DOS_EXEC_JOURNAL_OK ||
	    dos_exec_journal_abort(&journal, MACHINE_IDENTITY, &machine) !=
		DOS_EXEC_JOURNAL_OK ||
	    guest_memory[0x300u] != 0x22u)
		return 3;

	reset_fixture();
	if (!initialize_fixture(&journal, &machine, false))
		return 4;
	guest_memory[0x300u] = 0x22u;
	fail_all_writes = true;
	if (dos_exec_journal_stage_replace_far(
		&journal, MACHINE_IDENTITY, &machine, 0u, 0x300u, &replacement,
		sizeof(replacement), 1u) != DOS_EXEC_JOURNAL_POISONED ||
	    journal.state != DOS_EXEC_JOURNAL_STATE_POISONED ||
	    journal.record_count != 0u)
		return 5;
	fail_all_writes = false;
	prior_reads = read_calls;
	prior_writes = write_calls;
	if (dos_exec_journal_stage_replace_far(
		&journal, MACHINE_IDENTITY, &machine, 0u, 0x301u, &replacement,
		sizeof(replacement), 1u) != DOS_EXEC_JOURNAL_POISONED ||
	    dos_exec_journal_abort(&journal, MACHINE_IDENTITY, &machine) !=
		DOS_EXEC_JOURNAL_POISONED ||
	    dos_exec_journal_seal(&journal, MACHINE_IDENTITY, &machine) !=
		DOS_EXEC_JOURNAL_POISONED ||
	    read_calls != prior_reads || write_calls != prior_writes)
		return 6;
	return 0;
}

static int test_reverse_abort_and_abort_failure(void)
{
	struct dos_exec_journal journal;
	struct dos_machine machine;
	uint8_t first = 0x22u;
	uint8_t second = 0x33u;
	uint32_t prior_reads;
	uint32_t prior_writes;

	reset_fixture();
	if (!initialize_fixture(&journal, &machine, false))
		return 1;
	guest_memory[0x400u] = 0x11u;
	if (dos_exec_journal_stage_replace_far(
		&journal, MACHINE_IDENTITY, &machine, 0u, 0x400u, &first,
		sizeof(first), 1u) != DOS_EXEC_JOURNAL_OK ||
	    dos_exec_journal_stage_replace_far(
		&journal, MACHINE_IDENTITY, &machine, 0u, 0x400u, &second,
		sizeof(second), 1u) != DOS_EXEC_JOURNAL_OK)
		return 2;
	reset_callbacks();
	if (dos_exec_journal_abort(&journal, MACHINE_IDENTITY, &machine) !=
		DOS_EXEC_JOURNAL_OK ||
	    guest_memory[0x400u] != 0x11u || write_log_count != 2u ||
	    write_value_log[0] != 0x22u || write_value_log[1] != 0x11u ||
	    write_address_log[0] != 0x400u || write_address_log[1] != 0x400u)
		return 3;
	prior_reads = read_calls;
	prior_writes = write_calls;
	if (dos_exec_journal_abort(&journal, MACHINE_IDENTITY, &machine) !=
		DOS_EXEC_JOURNAL_OK ||
	    read_calls != prior_reads || write_calls != prior_writes)
		return 4;

	reset_fixture();
	if (!initialize_fixture(&journal, &machine, false))
		return 5;
	guest_memory[0x400u] = 0x11u;
	if (dos_exec_journal_stage_replace_far(
		&journal, MACHINE_IDENTITY, &machine, 0u, 0x400u, &first,
		sizeof(first), 1u) != DOS_EXEC_JOURNAL_OK ||
	    dos_exec_journal_stage_replace_far(
		&journal, MACHINE_IDENTITY, &machine, 0u, 0x400u, &second,
		sizeof(second), 1u) != DOS_EXEC_JOURNAL_OK)
		return 6;
	reset_callbacks();
	fail_write_call = 1u;
	if (dos_exec_journal_abort(&journal, MACHINE_IDENTITY, &machine) !=
		DOS_EXEC_JOURNAL_POISONED ||
	    journal.state != DOS_EXEC_JOURNAL_STATE_POISONED ||
	    journal.record_count != 2u || guest_memory[0x400u] != 0x33u ||
	    write_calls != 2u)
		return 7;
	return 0;
}

static int test_binding_and_callback_free_seal(void)
{
	struct dos_exec_journal journal;
	struct dos_machine machine;
	struct dos_machine wrong_machine;
	uint8_t replacement = 0x99u;
	uint32_t prior_reads;
	uint32_t prior_writes;

	reset_fixture();
	if (!initialize_fixture(&journal, &machine, false))
		return 1;
	if (dos_exec_journal_stage_replace_far(
		&journal, MACHINE_IDENTITY + 1u, &machine, 0u, 0x500u,
		&replacement, sizeof(replacement),
		1u) != DOS_EXEC_JOURNAL_IDENTITY_MISMATCH ||
	    read_calls != 0u || write_calls != 0u)
		return 2;
	wrong_machine = machine;
	wrong_machine.context = MACHINE_CONTEXT + 1u;
	if (dos_exec_journal_stage_replace_far(
		&journal, MACHINE_IDENTITY, &wrong_machine, 0u, 0x500u,
		&replacement, sizeof(replacement),
		1u) != DOS_EXEC_JOURNAL_CONTEXT_MISMATCH ||
	    read_calls != 0u || write_calls != 0u)
		return 3;
	wrong_machine = machine;
	wrong_machine.a20_enabled = true;
	if (dos_exec_journal_stage_replace_far(
		&journal, MACHINE_IDENTITY, &wrong_machine, 0u, 0x500u,
		&replacement, sizeof(replacement),
		1u) != DOS_EXEC_JOURNAL_MACHINE_MISMATCH ||
	    read_calls != 0u || write_calls != 0u)
		return 4;
	guest_memory[0x500u] = 0x44u;
	if (dos_exec_journal_stage_replace_far(
		&journal, MACHINE_IDENTITY, &machine, 0u, 0x500u, &replacement,
		sizeof(replacement), 1u) != DOS_EXEC_JOURNAL_OK ||
	    guest_memory[0x500u] != replacement)
		return 5;
	prior_reads = read_calls;
	prior_writes = write_calls;
	if (dos_exec_journal_preflight_seal(&journal, MACHINE_IDENTITY,
					    &machine) != DOS_EXEC_JOURNAL_OK ||
	    read_calls != prior_reads || write_calls != prior_writes ||
	    dos_exec_journal_seal(&journal, MACHINE_IDENTITY, &machine) !=
		DOS_EXEC_JOURNAL_OK ||
	    journal.state != DOS_EXEC_JOURNAL_STATE_SEALED ||
	    journal.record_count != 0u || guest_memory[0x500u] != replacement ||
	    read_calls != prior_reads || write_calls != prior_writes ||
	    dos_exec_journal_seal(&journal, MACHINE_IDENTITY, &machine) !=
		DOS_EXEC_JOURNAL_OK ||
	    dos_exec_journal_preflight_seal(&journal, MACHINE_IDENTITY,
					    &machine) !=
		DOS_EXEC_JOURNAL_INVALID_STATE ||
	    read_calls != prior_reads || write_calls != prior_writes ||
	    dos_exec_journal_abort(&journal, MACHINE_IDENTITY, &machine) !=
		DOS_EXEC_JOURNAL_INVALID_STATE ||
	    read_calls != prior_reads || write_calls != prior_writes)
		return 6;
	if (dos_exec_journal_poison(&journal, MACHINE_IDENTITY, &machine) !=
		DOS_EXEC_JOURNAL_OK ||
	    dos_exec_journal_poison(&journal, MACHINE_IDENTITY, &machine) !=
		DOS_EXEC_JOURNAL_OK ||
	    journal.state != DOS_EXEC_JOURNAL_STATE_POISONED ||
	    read_calls != prior_reads || write_calls != prior_writes)
		return 7;
	return 0;
}

static int run_tests(void)
{
	int status = test_lifecycle_and_segment_zero();

	if (status != 0)
		return 10 + status;
	status = test_far_offset_and_a20_wraps();
	if (status != 0)
		return 20 + status;
	status = test_capacity_and_checked_count();
	if (status != 0)
		return 30 + status;
	status = test_stage_failures_and_sticky_poison();
	if (status != 0)
		return 40 + status;
	status = test_reverse_abort_and_abort_failure();
	if (status != 0)
		return 50 + status;
	status = test_binding_and_callback_free_seal();
	if (status != 0)
		return 60 + status;
	return 0;
}

DOSC32_TEST_ENTRY(run_tests)

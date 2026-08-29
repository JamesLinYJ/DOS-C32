// SPDX-License-Identifier: GPL-2.0-only
/* Freestanding tests for the cross-object EXEC1 publication seal. */
#include "dos_exec_seal.h"
#include "test_entry.h"

#define TEST_MEMORY_SIZE DOS_A20_WRAP_ADDRESS
#define TEST_MACHINE_CONTEXT ((kernel_object_handle_t)0x4d41434843545831ull)
#define TEST_MACHINE_IDENTITY ((kernel_object_handle_t)0x4d41434849443131ull)
#define TEST_ARENA_IDENTITY ((kernel_object_handle_t)0x4152454e41494431ull)
#define TEST_MEMORY_LEASE_TABLE_IDENTITY                                      \
	((dos_memory_lease_table_identity_t)0x5345414cu)
#define TEST_RUNTIME_IDENTITY ((kernel_object_handle_t)0x52554e4944313131ull)
#define TEST_OBSERVER_CONTEXT ((kernel_object_handle_t)0x4f42534354583131ull)
#define TEST_OBSERVER_IDENTITY ((kernel_object_handle_t)0x4f42534944313131ull)
#define TEST_SFT_CONTEXT ((kernel_object_handle_t)0x5346544354583131ull)
#define TEST_SFT_IDENTITY ((kernel_object_handle_t)0x5346544944313131ull)
#define TEST_PARENT_PSP 0x1111u

static uint8_t guest_memory[TEST_MEMORY_SIZE];
static uint32_t read_calls;
static uint32_t write_calls;
static uint32_t observer_acquire_calls;
static uint32_t observer_release_calls;
static uint32_t observer_quarantine_calls;
static uint32_t sft_callback_calls;
static bool fail_observer_release;

struct seal_fixture {
	struct dos_machine machine;
	struct dos_memory_arena arena;
	struct dos_memory_lease_table memory_leases;
	struct dos_memory_lease_receipt environment;
	struct dos_memory_lease_receipt load;
	struct dos_process_runtime runtime;
	struct dos_exec_observer observer;
	struct dos_exec_journal journal;
	dos_sft_batch_handle_t sft_batch;
	struct dos_exec_seal_services services;
	struct dos_exec_load_only_seal_plan plan;
};

static void clear_guest(void)
{
	size_t index;

	for (index = 0u; index < sizeof(guest_memory); ++index)
		guest_memory[index] = 0u;
}

static enum dos_machine_status
guest_read(kernel_object_handle_t context, dos_linear_address_t address,
	   void *destination, size_t destination_capacity, size_t count)
{
	uint8_t *bytes = (uint8_t *)destination;
	size_t index;

	++read_calls;
	if (context != TEST_MACHINE_CONTEXT || destination == NULL ||
	    count > destination_capacity ||
	    (uint64_t)address >= TEST_MEMORY_SIZE ||
	    (uint64_t)count > TEST_MEMORY_SIZE - (uint64_t)address)
		return DOS_MACHINE_ADDRESS_FAULT;
	for (index = 0u; index < count; ++index)
		bytes[index] = guest_memory[address + index];
	return DOS_MACHINE_OK;
}

static enum dos_machine_status guest_write(kernel_object_handle_t context,
					   dos_linear_address_t address,
					   const void *source,
					   size_t source_capacity, size_t count)
{
	const uint8_t *bytes = (const uint8_t *)source;
	size_t index;

	++write_calls;
	if (context != TEST_MACHINE_CONTEXT || source == NULL ||
	    count > source_capacity || (uint64_t)address >= TEST_MEMORY_SIZE ||
	    (uint64_t)count > TEST_MEMORY_SIZE - (uint64_t)address)
		return DOS_MACHINE_ADDRESS_FAULT;
	for (index = 0u; index < count; ++index)
		guest_memory[address + index] = bytes[index];
	return DOS_MACHINE_OK;
}

static enum dos_exec_observer_adapter_status
observer_acquire(kernel_object_handle_t context, uint64_t *generation)
{
	++observer_acquire_calls;
	if (context != TEST_OBSERVER_CONTEXT || generation == NULL)
		return DOS_EXEC_OBSERVER_ADAPTER_FAULT;
	*generation = 9u;
	return DOS_EXEC_OBSERVER_ADAPTER_OK;
}

static enum dos_exec_observer_adapter_status
observer_release(kernel_object_handle_t context, uint64_t generation)
{
	++observer_release_calls;
	if (context != TEST_OBSERVER_CONTEXT || generation != 9u ||
	    fail_observer_release)
		return DOS_EXEC_OBSERVER_ADAPTER_FAULT;
	return DOS_EXEC_OBSERVER_ADAPTER_OK;
}

static enum dos_exec_observer_adapter_status
observer_quarantine(kernel_object_handle_t context, uint64_t generation)
{
	++observer_quarantine_calls;
	return context == TEST_OBSERVER_CONTEXT && generation == 9u
		   ? DOS_EXEC_OBSERVER_ADAPTER_OK
		   : DOS_EXEC_OBSERVER_ADAPTER_FAULT;
}

static enum dos_sft_adapter_status sft_lookup(kernel_object_handle_t context,
					      uint8_t sfn,
					      struct dos_sft_view *view)
{
	(void)context;
	(void)sfn;
	(void)view;
	++sft_callback_calls;
	return DOS_SFT_ADAPTER_FAULT;
}

static enum dos_sft_adapter_status
sft_operation(kernel_object_handle_t context,
	      dos_sft_reference_handle_t reference_handle)
{
	(void)context;
	(void)reference_handle;
	++sft_callback_calls;
	return DOS_SFT_ADAPTER_FAULT;
}

static const struct dos_machine_ops machine_ops = {
    .read_memory = guest_read,
    .write_memory = guest_write,
};

static const struct dos_exec_observer_ops observer_ops = {
    .identity = TEST_OBSERVER_IDENTITY,
    .acquire = observer_acquire,
    .release = observer_release,
    .quarantine = observer_quarantine,
};

static const struct dos_sft_batch_ops sft_ops = {
    .identity = TEST_SFT_IDENTITY,
    .lookup = sft_lookup,
    .device_open = sft_operation,
    .reference_acquire = sft_operation,
    .reference_release = sft_operation,
    .device_close = sft_operation,
};

static bool setup_fixture(struct seal_fixture *fixture, bool has_environment)
{
	struct dos_sft_jft20 parent_jft;
	struct dos_far_pointer16 dta = {
	    .offset = 0x0080u,
	    .segment = TEST_PARENT_PSP,
	};
	uint8_t journal_value = 0x5au;
	uint16_t child_psp;
	size_t index;

	clear_guest();
	read_calls = 0u;
	write_calls = 0u;
	observer_acquire_calls = 0u;
	observer_release_calls = 0u;
	observer_quarantine_calls = 0u;
	sft_callback_calls = 0u;
	fail_observer_release = false;
	fixture->environment = (struct dos_memory_lease_receipt){0};
	fixture->load = (struct dos_memory_lease_receipt){0};
	fixture->sft_batch = DOS_SFT_BATCH_HANDLE_INVALID;

	if (dos_machine_configure(&fixture->machine, &machine_ops,
				  TEST_MACHINE_CONTEXT, TEST_MEMORY_SIZE,
				  false) != DOS_MACHINE_OK ||
	    dos_memory_arena_construct(&fixture->arena, TEST_ARENA_IDENTITY) !=
		DOS_MEMORY_OK ||
	    dos_memory_arena_initialize_checked(&fixture->arena,
						&fixture->machine, 0x4000u,
						0x4100u) != DOS_MEMORY_OK ||
	    dos_memory_lease_table_construct(
		&fixture->memory_leases, TEST_MEMORY_LEASE_TABLE_IDENTITY) !=
		DOS_MEMORY_LEASE_OK ||
	    dos_memory_lease_table_initialize(&fixture->memory_leases) !=
		DOS_MEMORY_LEASE_OK)
		return false;
	if (has_environment &&
	    (dos_memory_lease_acquire_unnamed(
		 &fixture->memory_leases, &fixture->arena, &fixture->machine,
		 TEST_PARENT_PSP, 2u,
		 &fixture->environment) != DOS_MEMORY_LEASE_OK))
		return false;
	if (dos_memory_lease_acquire_unnamed(
		&fixture->memory_leases, &fixture->arena, &fixture->machine,
		TEST_PARENT_PSP, 0x20u,
		&fixture->load) != DOS_MEMORY_LEASE_OK)
		return false;
	child_psp = fixture->load.guest_segment;
	for (index = 0u; index < ARRAY_SIZE(parent_jft.entries); ++index)
		parent_jft.entries[index] = DOS_JFT_ENTRY_UNUSED;
	if (dos_sft_batch_prepare(&sft_ops, TEST_SFT_CONTEXT, &parent_jft,
				  &fixture->sft_batch) != DOS_SFT_BATCH_OK ||
	    sft_callback_calls != 0u ||
	    dos_process_runtime_construct(&fixture->runtime) !=
		DOS_PROCESS_RUNTIME_OK ||
	    dos_process_runtime_initialize(
		&fixture->runtime, TEST_RUNTIME_IDENTITY, TEST_PARENT_PSP,
		dta) != DOS_PROCESS_RUNTIME_OK ||
	    dos_exec_observer_construct(&fixture->observer) !=
		DOS_EXEC_OBSERVER_OK ||
	    dos_exec_observer_acquire(&fixture->observer, &observer_ops,
				      TEST_OBSERVER_CONTEXT) !=
		DOS_EXEC_OBSERVER_OK ||
	    dos_exec_journal_construct(&fixture->journal) !=
		DOS_EXEC_JOURNAL_OK ||
	    dos_exec_journal_initialize(
		&fixture->journal, TEST_MACHINE_IDENTITY, &fixture->machine) !=
		DOS_EXEC_JOURNAL_OK ||
	    dos_exec_journal_stage_replace_far(
		&fixture->journal, TEST_MACHINE_IDENTITY, &fixture->machine, 0u,
		0x0200u, &journal_value, sizeof(journal_value),
		sizeof(journal_value)) != DOS_EXEC_JOURNAL_OK)
		return false;

	fixture->plan = (struct dos_exec_load_only_seal_plan){0};
	if (dos_process_runtime_snapshot(&fixture->runtime,
					 &fixture->plan.expected_parent) !=
	    DOS_PROCESS_RUNTIME_OK)
		return false;
	if (has_environment &&
	    dos_memory_lease_prepare_owner_rebind(
		&fixture->memory_leases, &fixture->arena, &fixture->machine,
		fixture->environment.handle, TEST_PARENT_PSP, child_psp,
		&fixture->plan.environment_rebind) != DOS_MEMORY_LEASE_OK)
		return false;
	if (dos_memory_lease_prepare_owner_rebind(
		&fixture->memory_leases, &fixture->arena, &fixture->machine,
		fixture->load.handle, TEST_PARENT_PSP, child_psp,
		&fixture->plan.load_rebind) != DOS_MEMORY_LEASE_OK)
		return false;
	if (has_environment &&
	    dos_exec_journal_stage_replace_far(
		&fixture->journal, TEST_MACHINE_IDENTITY, &fixture->machine,
		fixture->plan.environment_rebind.value.header_segment, 0u,
		fixture->plan.environment_rebind.value.replacement_bytes,
		sizeof(fixture->plan.environment_rebind.value.replacement_bytes),
		sizeof(fixture->plan.environment_rebind.value.replacement_bytes)) !=
		DOS_EXEC_JOURNAL_OK)
		return false;
	if (dos_exec_journal_stage_replace_far(
		&fixture->journal, TEST_MACHINE_IDENTITY, &fixture->machine,
		fixture->plan.load_rebind.value.header_segment, 0u,
		fixture->plan.load_rebind.value.replacement_bytes,
		sizeof(fixture->plan.load_rebind.value.replacement_bytes),
		sizeof(fixture->plan.load_rebind.value.replacement_bytes)) !=
	    DOS_EXEC_JOURNAL_OK)
		return false;
	fixture->plan.sft_batch = fixture->sft_batch;
	fixture->plan.child_psp = child_psp;
	fixture->plan.has_environment = has_environment ? 1u : 0u;
	fixture->services = (struct dos_exec_seal_services){
	    .observer = &fixture->observer,
	    .observer_ops = &observer_ops,
	    .observer_context = TEST_OBSERVER_CONTEXT,
	    .journal = &fixture->journal,
	    .machine_identity = TEST_MACHINE_IDENTITY,
	    .memory_leases = &fixture->memory_leases,
	    .arena = &fixture->arena,
	    .machine = &fixture->machine,
	    .runtime = &fixture->runtime,
	};
	read_calls = 0u;
	write_calls = 0u;
	return true;
}

static bool cleanup_prepared(struct seal_fixture *fixture)
{
	bool ok = true;

	fail_observer_release = false;
	if (fixture->journal.state == DOS_EXEC_JOURNAL_STATE_STAGING &&
	    dos_exec_journal_abort(&fixture->journal, TEST_MACHINE_IDENTITY,
				   &fixture->machine) != DOS_EXEC_JOURNAL_OK)
		ok = false;
	if (dos_sft_batch_abort(fixture->sft_batch, &sft_ops,
				TEST_SFT_CONTEXT) != DOS_SFT_BATCH_OK ||
	    dos_sft_batch_retire(fixture->sft_batch) != DOS_SFT_BATCH_OK)
		ok = false;
	if (dos_memory_lease_abort(&fixture->memory_leases, &fixture->arena,
				   &fixture->machine, fixture->load.handle,
				   TEST_PARENT_PSP) != DOS_MEMORY_LEASE_OK)
		ok = false;
	if (fixture->plan.has_environment != 0u &&
	    dos_memory_lease_abort(&fixture->memory_leases, &fixture->arena,
				   &fixture->machine,
				   fixture->environment.handle,
				   TEST_PARENT_PSP) != DOS_MEMORY_LEASE_OK)
		ok = false;
	if (dos_exec_observer_release(&fixture->observer, &observer_ops,
				      TEST_OBSERVER_CONTEXT) !=
	    DOS_EXEC_OBSERVER_OK)
		ok = false;
	return ok;
}

static int test_preflight_is_pure(void)
{
	struct seal_fixture fixture;
	enum dos_exec_seal_status seal_status;
	uint32_t expected_generation;
	uint16_t child_psp;

	if (!setup_fixture(&fixture, true))
		return 1;
	expected_generation = (uint32_t)fixture.plan.expected_parent.generation;
	child_psp = fixture.plan.child_psp;
	fixture.plan.expected_parent.generation++;
	seal_status = dos_exec_seal_preflight_load_only(&fixture.services,
						       &fixture.plan);
	if (seal_status != DOS_EXEC_SEAL_RUNTIME_NOT_READY)
		return 2;
	if (read_calls != 0u || write_calls != 0u)
		return 3;
	if (fixture.observer.state != DOS_EXEC_OBSERVER_STATE_HELD ||
	    fixture.journal.state != DOS_EXEC_JOURNAL_STATE_STAGING)
		return 4;
	fixture.plan.expected_parent.generation = expected_generation;
	/* The child PSP is cross-checked against both exact rebind values. */
	fixture.plan.child_psp = 0u;
	if (dos_exec_seal_preflight_load_only(&fixture.services,
					      &fixture.plan) !=
		DOS_EXEC_SEAL_INVALID_ARGUMENT ||
	    read_calls != 0u || write_calls != 0u)
		return 5;
	fixture.plan.child_psp = child_psp;
	if (dos_exec_seal_preflight_load_only(
		&fixture.services, &fixture.plan) != DOS_EXEC_SEAL_OK ||
	    read_calls != 0u || write_calls != 0u ||
	    !cleanup_prepared(&fixture))
		return 6;
	return 0;
}

static int test_successful_load_only_seal(void)
{
	struct seal_fixture fixture;
	enum dos_sft_batch_state sft_state = DOS_SFT_BATCH_STATE_ABORTED;

	if (!setup_fixture(&fixture, true))
		return 1;
	if (dos_exec_seal_preflight_load_only(
		&fixture.services, &fixture.plan) != DOS_EXEC_SEAL_OK ||
	    dos_exec_seal_commit_load_only(&fixture.services, &fixture.plan) !=
		DOS_EXEC_SEAL_OK ||
	    read_calls != 0u || write_calls != 0u ||
	    observer_acquire_calls != 1u || observer_release_calls != 1u ||
	    observer_quarantine_calls != 0u ||
	    fixture.observer.state != DOS_EXEC_OBSERVER_STATE_RELEASED ||
	    fixture.journal.state != DOS_EXEC_JOURNAL_STATE_SEALED ||
	    fixture.runtime.poisoned || fixture.arena.machine_poisoned ||
	    fixture.runtime.current_psp != fixture.plan.child_psp ||
	    fixture.runtime.dta.segment != fixture.plan.child_psp ||
	    fixture.runtime.dta.offset != 0x0080u ||
	    !dos_memory_lease_table_is_drained(&fixture.memory_leases) ||
	    dos_sft_batch_get_state(fixture.sft_batch, &sft_state) !=
		DOS_SFT_BATCH_OK ||
	    sft_state != DOS_SFT_BATCH_STATE_COMMITTED)
		return 2;
	if (dos_memory_lease_preflight_publish(
		&fixture.memory_leases, &fixture.arena, &fixture.machine,
		fixture.load.handle,
		fixture.plan.child_psp) != DOS_MEMORY_LEASE_INVALID_STATE ||
	    dos_sft_batch_retire(fixture.sft_batch) != DOS_SFT_BATCH_OK)
		return 3;
	return 0;
}

static int test_release_uncertainty_poison(void)
{
	struct seal_fixture fixture;
	enum dos_sft_batch_state sft_state = DOS_SFT_BATCH_STATE_ABORTED;

	if (!setup_fixture(&fixture, false))
		return 1;
	fail_observer_release = true;
	if (dos_exec_seal_commit_load_only(&fixture.services, &fixture.plan) !=
		DOS_EXEC_SEAL_POISONED ||
	    read_calls != 0u || write_calls != 0u ||
	    observer_release_calls != 1u || observer_quarantine_calls != 1u ||
	    fixture.observer.state != DOS_EXEC_OBSERVER_STATE_POISONED ||
	    fixture.journal.state != DOS_EXEC_JOURNAL_STATE_POISONED ||
	    !fixture.runtime.poisoned || !fixture.arena.machine_poisoned ||
	    fixture.runtime.current_psp != fixture.plan.child_psp ||
	    dos_sft_batch_get_state(fixture.sft_batch, &sft_state) !=
		DOS_SFT_BATCH_OK ||
	    sft_state != DOS_SFT_BATCH_STATE_COMMITTED ||
	    dos_sft_batch_retire(fixture.sft_batch) != DOS_SFT_BATCH_OK)
		return 2;
	return 0;
}

static int run_tests(void)
{
	int status = test_preflight_is_pure();

	if (status != 0)
		return 10 + status;
	status = test_successful_load_only_seal();
	if (status != 0)
		return 20 + status;
	status = test_release_uncertainty_poison();
	if (status != 0)
		return 30 + status;
	return 0;
}

DOSC32_TEST_ENTRY(run_tests)

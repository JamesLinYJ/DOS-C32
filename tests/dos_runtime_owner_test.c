// SPDX-License-Identifier: GPL-2.0-only
#include "dos_runtime_owner.h"
#include "test_entry.h"

#define TEST_MEMORY_BYTES 0x30000u
#define TEST_MACHINE_CONTEXT 0x101u
#define TEST_MACHINE_IDENTITY 0x102u
#define TEST_FILE_IDENTITY 0x201u
#define TEST_FILE_CONTEXT 0x202u
#define TEST_OBSERVER_IDENTITY 0x301u
#define TEST_OBSERVER_CONTEXT 0x302u
#define TEST_SFT_IDENTITY 0x401u
#define TEST_SFT_CONTEXT 0x402u
#define TEST_DRIVE_IDENTITY 0x501u
#define TEST_DRIVE_CONTEXT 0x502u
#define TEST_BACKEND_IDENTITY 0x601u
#define TEST_BACKEND_CONTEXT 0x602u
#define TEST_ARENA_HEAD 0x1000u
#define TEST_ARENA_END 0x2000u
#define TEST_INITIAL_PSP 0x1001u
#define TEST_INITIAL_ENVIRONMENT 0x1012u

static uint8_t guest_memory[TEST_MEMORY_BYTES];

static uint16_t read_le16(const uint8_t *bytes)
{
	return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static enum dos_machine_status machine_read(
	kernel_object_handle_t context, dos_linear_address_t address,
	void *destination, size_t destination_capacity, size_t count)
{
	uint8_t *output = (uint8_t *)destination;
	size_t index;

	if (context != TEST_MACHINE_CONTEXT || destination == NULL ||
	    count > destination_capacity || address > TEST_MEMORY_BYTES ||
	    count > TEST_MEMORY_BYTES - (size_t)address)
		return DOS_MACHINE_ADDRESS_FAULT;
	for (index = 0u; index < count; ++index)
		output[index] = guest_memory[(size_t)address + index];
	return DOS_MACHINE_OK;
}

static enum dos_machine_status machine_write(
	kernel_object_handle_t context, dos_linear_address_t address,
	const void *source, size_t source_capacity, size_t count)
{
	const uint8_t *input = (const uint8_t *)source;
	size_t index;

	if (context != TEST_MACHINE_CONTEXT || source == NULL ||
	    count > source_capacity || address > TEST_MEMORY_BYTES ||
	    count > TEST_MEMORY_BYTES - (size_t)address)
		return DOS_MACHINE_ADDRESS_FAULT;
	for (index = 0u; index < count; ++index)
		guest_memory[(size_t)address + index] = input[index];
	return DOS_MACHINE_OK;
}

static enum dos_exec_file_adapter_status file_open(
	kernel_object_handle_t context, const uint8_t *path, size_t path_length,
	struct dos_exec_file_open_result *result)
{
	(void)context;
	(void)path;
	(void)path_length;
	(void)result;
	return DOS_EXEC_FILE_ADAPTER_FAULT;
}

static enum dos_exec_file_adapter_status file_probe(
	kernel_object_handle_t context, kernel_object_handle_t reader_context,
	struct dos_exec_file_probe_result *result)
{
	(void)context;
	(void)reader_context;
	(void)result;
	return DOS_EXEC_FILE_ADAPTER_FAULT;
}

static enum dos_image_read_status file_read(
	kernel_object_handle_t reader_context, file_offset_t offset,
	void *destination, size_t destination_capacity, size_t count,
	size_t *bytes_read)
{
	(void)reader_context;
	(void)offset;
	(void)destination;
	(void)destination_capacity;
	(void)count;
	(void)bytes_read;
	return DOS_IMAGE_READ_IO_ERROR;
}

static enum dos_exec_file_close_result file_close(
	kernel_object_handle_t context, kernel_object_handle_t reader_context)
{
	(void)context;
	(void)reader_context;
	return DOS_EXEC_FILE_CLOSE_CLOSED;
}

static enum dos_exec_observer_adapter_status observer_acquire(
	kernel_object_handle_t context, uint64_t *generation)
{
	(void)context;
	(void)generation;
	return DOS_EXEC_OBSERVER_ADAPTER_FAULT;
}

static enum dos_exec_observer_adapter_status observer_release(
	kernel_object_handle_t context, uint64_t generation)
{
	(void)context;
	(void)generation;
	return DOS_EXEC_OBSERVER_ADAPTER_FAULT;
}

static enum dos_exec_observer_adapter_status observer_quarantine(
	kernel_object_handle_t context, uint64_t generation)
{
	(void)context;
	(void)generation;
	return DOS_EXEC_OBSERVER_ADAPTER_OK;
}

static enum dos_sft_adapter_status sft_lookup(
	kernel_object_handle_t context, uint8_t sfn, struct dos_sft_view *view)
{
	(void)context;
	(void)sfn;
	(void)view;
	return DOS_SFT_ADAPTER_INVALID_SFT;
}

static enum dos_sft_adapter_status sft_reference(
	kernel_object_handle_t context, dos_sft_reference_handle_t reference)
{
	(void)context;
	(void)reference;
	return DOS_SFT_ADAPTER_FAULT;
}

static enum dos_exec_drive_visibility_status drive_resolve(
	kernel_object_handle_t context, uint8_t drive_designator)
{
	(void)context;
	(void)drive_designator;
	return DOS_EXEC_DRIVE_INVALID;
}

static enum dos_exec_backend_prepare_status backend_prepare(
	kernel_object_handle_t context, const struct dos_machine *machine,
	kernel_object_handle_t machine_identity,
	const struct dos_exec_handoff_plan *handoff,
	struct dos_exec_backend_prepare_result *result)
{
	(void)context;
	(void)machine;
	(void)machine_identity;
	(void)handoff;
	(void)result;
	return DOS_EXEC_BACKEND_REJECTED;
}

static enum dos_exec_backend_release_status backend_release(
	kernel_object_handle_t context, kernel_object_handle_t backend_context)
{
	(void)context;
	(void)backend_context;
	return DOS_EXEC_BACKEND_RELEASED;
}

static enum dos_exec_backend_run_status backend_run(
	kernel_object_handle_t context, kernel_object_handle_t backend_context,
	kernel_object_handle_t machine_identity, const struct dos_machine *machine,
	struct dos_cpu_state *state, struct dos_execution_event *event)
{
	(void)context;
	(void)backend_context;
	(void)machine_identity;
	(void)machine;
	(void)state;
	(void)event;
	return DOS_EXEC_BACKEND_RUN_UNCERTAIN;
}

static const struct dos_machine_ops machine_ops = {
	.read_memory = machine_read,
	.write_memory = machine_write,
};

static const struct dos_exec_file_lease_ops file_ops = {
	.identity = TEST_FILE_IDENTITY,
	.open = file_open,
	.probe_device = file_probe,
	.read = file_read,
	.close = file_close,
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
	.device_open = sft_reference,
	.reference_acquire = sft_reference,
	.reference_release = sft_reference,
	.device_close = sft_reference,
};

static const struct dos_exec_drive_visibility_ops drive_ops = {
	.identity = TEST_DRIVE_IDENTITY,
	.resolve = drive_resolve,
};

static const struct dos_exec_backend_ops backend_ops = {
	.identity = TEST_BACKEND_IDENTITY,
	.capabilities = DOS_EXEC_CAP_VM86,
	.prepare = backend_prepare,
	.release = backend_release,
	.run_until_event = backend_run,
};

static int run_tests(void)
{
	struct dos_machine machine;
	struct dos_runtime_owner_config config = {
		.coordinator_identity = 0x701u,
		.file_lease_table_identity = 0x702u,
		.memory_arena_identity = 0x703u,
		.backend_session_table_identity = 0x704u,
		.runtime_identity = 0x705u,
		.personality_identity = 0x706u,
		.memory_lease_table_identity = 0x707u,
		.arena_head_segment = TEST_ARENA_HEAD,
		.arena_end_segment = TEST_ARENA_END,
		.drives = {
			.available_drive_mask = (uint32_t)1u << 3u,
			.current_drive = 3u,
			.boot_drive = 4u,
			.last_drive = 4u,
			.reserved = 0u,
		},
	};
	struct dos_runtime_owner_bindings bindings;
	struct dos_exec_transaction_services services;
	struct dos_exec_transaction_services untouched;
	struct dos_process_runtime_snapshot runtime;
	const uint8_t *mcb = guest_memory + ((size_t)TEST_ARENA_HEAD << 4);
	const uint8_t *environment_mcb =
		guest_memory + ((size_t)0x1011u << 4);
	const uint8_t *environment =
		guest_memory + ((size_t)TEST_INITIAL_ENVIRONMENT << 4);
	const uint8_t *free_mcb =
		guest_memory + ((size_t)0x101cu << 4);
	const uint8_t *psp = guest_memory + ((size_t)TEST_INITIAL_PSP << 4);

	guest_memory[0x22u * 4u] = 0x22u;
	guest_memory[0x22u * 4u + 1u] = 0x12u;
	guest_memory[0x22u * 4u + 2u] = 0x22u;
	guest_memory[0x22u * 4u + 3u] = 0x22u;
	guest_memory[0x23u * 4u] = 0x23u;
	guest_memory[0x23u * 4u + 1u] = 0x12u;
	guest_memory[0x23u * 4u + 2u] = 0x23u;
	guest_memory[0x23u * 4u + 3u] = 0x23u;
	guest_memory[0x24u * 4u] = 0x24u;
	guest_memory[0x24u * 4u + 1u] = 0x12u;
	guest_memory[0x24u * 4u + 2u] = 0x24u;
	guest_memory[0x24u * 4u + 3u] = 0x24u;
	if (dos_machine_configure(&machine, &machine_ops, TEST_MACHINE_CONTEXT,
				  TEST_MEMORY_BYTES, false) != DOS_MACHINE_OK)
		return 1;
	bindings = (struct dos_runtime_owner_bindings){
		.machine = &machine,
		.file_ops = &file_ops,
		.observer_ops = &observer_ops,
		.sft_ops = &sft_ops,
		.drive_ops = &drive_ops,
		.backend_ops = &backend_ops,
		.machine_identity = TEST_MACHINE_IDENTITY,
		.file_adapter_context = TEST_FILE_CONTEXT,
		.observer_adapter_context = TEST_OBSERVER_CONTEXT,
		.sft_adapter_context = TEST_SFT_CONTEXT,
		.drive_adapter_context = TEST_DRIVE_CONTEXT,
		.backend_adapter_context = TEST_BACKEND_CONTEXT,
	};
	if (dos_runtime_owner_initialize(&config, &bindings) !=
		DOS_RUNTIME_OWNER_READY ||
	    dos_runtime_owner_initial_psp() != TEST_INITIAL_PSP ||
	    dos_runtime_owner_transactions() == NULL ||
	    dos_runtime_owner_sessions() == NULL ||
	    dos_runtime_owner_personality() == NULL)
		return 2;
	if (mcb[0] != (uint8_t)'M' || read_le16(mcb + 1u) != TEST_INITIAL_PSP ||
	    read_le16(mcb + 3u) != 0x10u || mcb[8u] != (uint8_t)'C' ||
	    mcb[9u] != (uint8_t)'O' || mcb[10u] != (uint8_t)'M' ||
	    mcb[11u] != (uint8_t)'M' || mcb[12u] != (uint8_t)'A' ||
	    mcb[13u] != (uint8_t)'N' || mcb[14u] != (uint8_t)'D' ||
	    mcb[15u] != 0u || environment_mcb[0] != (uint8_t)'M' ||
	    read_le16(environment_mcb + 1u) != TEST_INITIAL_PSP ||
	    read_le16(environment_mcb + 3u) != 10u ||
	    free_mcb[0] != (uint8_t)'Z' ||
	    read_le16(free_mcb + 1u) != 0u ||
	    read_le16(free_mcb + 3u) != 0x0fe3u)
		return 3;
	if (read_le16(psp) != 0x20cdu || read_le16(psp + 2u) != 0x1011u ||
	    read_le16(psp + 0x0au) != 0x1222u ||
	    read_le16(psp + 0x0cu) != 0x2222u ||
	    read_le16(psp + 0x0eu) != 0x1223u ||
	    read_le16(psp + 0x10u) != 0x2323u ||
	    read_le16(psp + 0x12u) != 0x1224u ||
	    read_le16(psp + 0x14u) != 0x2424u ||
	    read_le16(psp + 0x16u) != TEST_INITIAL_PSP ||
	    psp[0x18u] != 0u || psp[0x19u] != 0u || psp[0x1au] != 0u ||
	    psp[0x1bu] != DOS_JFT_ENTRY_UNUSED ||
	    read_le16(psp + 0x2cu) != TEST_INITIAL_ENVIRONMENT ||
	    read_le16(psp + 0x32u) != DOS_PSP_DEFAULT_HANDLES ||
	    read_le16(psp + 0x34u) != 0x18u ||
	    read_le16(psp + 0x36u) != TEST_INITIAL_PSP || psp[0x80u] != 0u ||
	    psp[0x81u] != 0x0du)
		return 4;
	if (environment[0] != (uint8_t)'P' ||
	    environment[4] != (uint8_t)'=' || environment[5] != 0u ||
	    environment[6] != (uint8_t)'C' ||
	    environment[13] != (uint8_t)'=' ||
	    environment[14] != (uint8_t)'D' ||
	    environment[28] != 0u || environment[29] != 0u ||
	    read_le16(environment + 30u) != DOS_ENVIRONMENT_TRAILER_VALUE ||
	    environment[32] != (uint8_t)'D' || environment[46] != 0u)
		return 5;
	if (dos_process_runtime_snapshot(
		&dos_runtime_owner_personality()->int21.process_runtime,
		&runtime) != DOS_PROCESS_RUNTIME_OK ||
	    runtime.current_psp != TEST_INITIAL_PSP ||
	    runtime.dta.segment != TEST_INITIAL_PSP || runtime.dta.offset != 0x80u)
		return 6;
	if (dos_runtime_owner_borrow_exec_services(&bindings, &services) !=
		DOS_RUNTIME_OWNER_READY ||
	    services.runtime !=
		&dos_runtime_owner_personality()->int21.process_runtime ||
	    services.memory_arena !=
		&dos_runtime_owner_personality()->int21.memory_arena ||
	    services.file_leases == NULL || services.memory_leases == NULL ||
	    services.backend_sessions != dos_runtime_owner_sessions() ||
	    services.coordinator_identity != config.coordinator_identity ||
	    services.memory_lease_table_identity !=
		config.memory_lease_table_identity)
		return 7;
	untouched = services;
	bindings.machine_identity++;
	if (dos_runtime_owner_borrow_exec_services(&bindings, &services) !=
		DOS_RUNTIME_OWNER_INVALID_ARGUMENT ||
	    services.coordinator_identity != untouched.coordinator_identity ||
	    services.runtime != untouched.runtime)
		return 8;
	bindings.machine_identity--;
	if (dos_runtime_owner_initialize(&config, &bindings) !=
		DOS_RUNTIME_OWNER_INVALID_STATE)
		return 8;
	return 0;
}

DOSC32_TEST_ENTRY(run_tests)

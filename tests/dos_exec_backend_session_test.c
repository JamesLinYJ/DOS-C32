// SPDX-License-Identifier: GPL-2.0-only
/* Freestanding lifecycle tests for dormant/runnable execution backends. */
#include "dos_exec_backend_session.h"
#include "test_entry.h"
#include "dos_execution_loop.h"

#define TABLE_IDENTITY ((kernel_object_handle_t)0x53455353494f4e31ull)
#define ADAPTER_IDENTITY ((kernel_object_handle_t)0x4241434b454e4431ull)
#define ADAPTER_CONTEXT ((kernel_object_handle_t)0x4241434b43545831ull)
#define MACHINE_IDENTITY ((kernel_object_handle_t)0x4d414348494e4531ull)
#define MACHINE_CONTEXT ((kernel_object_handle_t)0x4d41434843545831ull)
#define BACKEND_CONTEXT ((kernel_object_handle_t)0x564d383653455331ull)
#define GUEST_CAPACITY 0x110000u
#define HANDLE_SENTINEL 0xa5a55a5af00ff00full
#define DETAIL_SENTINEL 0xa5a55a5au
#define ARENA_IDENTITY ((kernel_object_handle_t)0x4152454e41303031ull)
#define PERSONALITY_IDENTITY ((kernel_object_handle_t)0x504552534f4e4131ull)
#define RUNTIME_IDENTITY ((kernel_object_handle_t)0x52554e54494d4531ull)
#define TEST_ARENA_HEAD 0x2000u
#define TEST_ARENA_END 0x2020u

static const struct dos_int21_drive_config test_drive_config = {
	.available_drive_mask = (uint32_t)1u << 2u,
	.current_drive = 2u,
	.boot_drive = 3u,
	.last_drive = 3u,
	.reserved = 0u,
};

static uint8_t guest_memory[GUEST_CAPACITY];
static enum dos_exec_backend_prepare_status configured_prepare_status;
static enum dos_exec_backend_release_status configured_release_status;
static enum dos_exec_backend_run_status configured_run_status;
static struct dos_execution_event configured_event;
static struct dos_exec_backend_prepare_result configured_prepare_result;
static uint32_t prepare_calls;
static uint32_t release_calls;
static uint32_t run_calls;
static uint32_t port_read_calls;
static uint32_t port_write_calls;
static uint32_t configured_port_read_value;
static uint32_t observed_port_write_value;
static enum dos_machine_status configured_port_read_status;
static enum dos_machine_status configured_port_write_status;
static uint8_t arguments_valid;
static struct dos_exec_handoff_plan expected_handoff;

static enum dos_machine_status machine_read(kernel_object_handle_t context,
					    dos_linear_address_t address,
					    void *destination,
					    size_t destination_capacity,
					    size_t count)
{
	uint8_t *output = (uint8_t *)destination;
	size_t index;

	if (context != MACHINE_CONTEXT || destination == NULL ||
	    count > destination_capacity || address > GUEST_CAPACITY ||
	    count > GUEST_CAPACITY - (size_t)address)
		return DOS_MACHINE_IO_FAULT;
	for (index = 0u; index < count; ++index)
		output[index] = guest_memory[(size_t)address + index];
	return DOS_MACHINE_OK;
}

static enum dos_machine_status machine_write(kernel_object_handle_t context,
					     dos_linear_address_t address,
					     const void *source,
					     size_t source_capacity,
					     size_t count)
{
	const uint8_t *input = (const uint8_t *)source;
	size_t index;

	if (context != MACHINE_CONTEXT || source == NULL ||
	    count > source_capacity || address > GUEST_CAPACITY ||
	    count > GUEST_CAPACITY - (size_t)address)
		return DOS_MACHINE_IO_FAULT;
	for (index = 0u; index < count; ++index)
		guest_memory[(size_t)address + index] = input[index];
	return DOS_MACHINE_OK;
}

static enum dos_machine_status machine_read_port(
	kernel_object_handle_t context, uint16_t port, enum dos_io_width width,
	uint32_t *value)
{
	++port_read_calls;
	if (context != MACHINE_CONTEXT || port != 0x1234u ||
	    width != DOS_IO_WIDTH_16 || value == NULL)
		arguments_valid = 0u;
	if (configured_port_read_status == DOS_MACHINE_OK)
		*value = configured_port_read_value;
	return configured_port_read_status;
}

static enum dos_machine_status machine_write_port(
	kernel_object_handle_t context, uint16_t port, enum dos_io_width width,
	uint32_t value)
{
	++port_write_calls;
	if (context != MACHINE_CONTEXT || port != 0x1234u ||
	    width != DOS_IO_WIDTH_8)
		arguments_valid = 0u;
	observed_port_write_value = value;
	return configured_port_write_status;
}

static const struct dos_machine_ops machine_ops = {
	.read_memory = machine_read,
	.write_memory = machine_write,
	.read_port = machine_read_port,
	.write_port = machine_write_port,
	.set_a20 = NULL,
};

static enum dos_exec_backend_prepare_status adapter_prepare(
    kernel_object_handle_t context, const struct dos_machine *machine,
    kernel_object_handle_t machine_identity,
    const struct dos_exec_handoff_plan *handoff,
    struct dos_exec_backend_prepare_result *result)
{
	++prepare_calls;
	if (context != ADAPTER_CONTEXT || machine_identity != MACHINE_IDENTITY ||
	    machine == NULL ||
	    machine->context != MACHINE_CONTEXT ||
	    !dos_exec_handoff_plans_equal(handoff, &expected_handoff) ||
	    result == NULL)
		arguments_valid = 0u;
	*result = configured_prepare_result;
	return configured_prepare_status;
}

static enum dos_exec_backend_release_status adapter_release(
    kernel_object_handle_t context, kernel_object_handle_t backend_context)
{
	++release_calls;
	if (context != ADAPTER_CONTEXT || backend_context != BACKEND_CONTEXT)
		arguments_valid = 0u;
	return configured_release_status;
}

static enum dos_exec_backend_run_status adapter_run(
    kernel_object_handle_t context, kernel_object_handle_t backend_context,
    kernel_object_handle_t machine_identity,
    const struct dos_machine *machine, struct dos_cpu_state *state,
    struct dos_execution_event *event)
{
	++run_calls;
	if (context != ADAPTER_CONTEXT || backend_context != BACKEND_CONTEXT ||
	    machine_identity != MACHINE_IDENTITY ||
	    machine == NULL || machine->context != MACHINE_CONTEXT ||
	    state == NULL || event == NULL)
		arguments_valid = 0u;
	if (configured_run_status == DOS_EXEC_BACKEND_EVENT) {
		state->eip = (uint16_t)(dos_register_low16(state->eip) + 1u);
		*event = configured_event;
	}
	return configured_run_status;
}

static const struct dos_exec_backend_ops backend_ops = {
	.identity = ADAPTER_IDENTITY,
	.capabilities = DOS_EXEC_CAP_VM86,
	.prepare = adapter_prepare,
	.release = adapter_release,
	.run_until_event = adapter_run,
};

static enum dos_ems_page_status ems_page_query(
	kernel_object_handle_t context, struct dos_ems_page_snapshot *snapshot)
{
	if (context != MACHINE_IDENTITY || snapshot == NULL)
		return DOS_EMS_PAGE_FAULT;
	*snapshot = (struct dos_ems_page_snapshot){
		.largest_free_pages = 256u,
		.total_free_pages = 256u,
		.managed_pages = 256u,
		.highest_address = 0x001fffffu,
	};
	return DOS_EMS_PAGE_OK;
}

static enum dos_ems_page_status ems_page_allocate(
	kernel_object_handle_t context, uint64_t requested_pages,
	dos_ems_page_block_t *block, uint64_t *physical_address,
	uint64_t *capacity_pages)
{
	(void)context;
	(void)requested_pages;
	(void)block;
	(void)physical_address;
	(void)capacity_pages;
	return DOS_EMS_PAGE_NO_MEMORY;
}

static enum dos_ems_page_status ems_page_release(
	kernel_object_handle_t context, dos_ems_page_block_t block)
{
	(void)context;
	(void)block;
	return DOS_EMS_PAGE_INVALID_BLOCK;
}

static enum dos_vcpi_platform_status ems_translate_low_page(
	kernel_object_handle_t context, uint16_t page,
	uint64_t *physical_address)
{
	if (context != BACKEND_CONTEXT || physical_address == NULL)
		return DOS_VCPI_PLATFORM_FAULT;
	*physical_address = (uint64_t)page * DOS_EMS_NATIVE_PAGE_BYTES;
	return DOS_VCPI_PLATFORM_OK;
}

static enum dos_vcpi_platform_status ems_read_virtual_cr0(
	kernel_object_handle_t context, uint32_t *virtual_cr0)
{
	if (context != BACKEND_CONTEXT || virtual_cr0 == NULL)
		return DOS_VCPI_PLATFORM_FAULT;
	*virtual_cr0 = 0x11u;
	return DOS_VCPI_PLATFORM_OK;
}

static enum dos_vcpi_platform_status ems_query_pic_mappings(
	kernel_object_handle_t context, uint8_t *master_base,
	uint8_t *slave_base)
{
	if (context != BACKEND_CONTEXT || master_base == NULL ||
	    slave_base == NULL)
		return DOS_VCPI_PLATFORM_FAULT;
	*master_base = 0x08u;
	*slave_base = 0x70u;
	return DOS_VCPI_PLATFORM_OK;
}

static enum dos_vcpi_platform_status ems_set_pic_mappings(
	kernel_object_handle_t context, uint8_t master_base,
	uint8_t slave_base)
{
	return context == BACKEND_CONTEXT && master_base != slave_base
		       ? DOS_VCPI_PLATFORM_OK
		       : DOS_VCPI_PLATFORM_FAULT;
}

static enum dos_ems_page_frame_status ems_frame_acquire(
	kernel_object_handle_t context, uint64_t linear_address,
	uint64_t byte_count, dos_ems_page_frame_lease_t *lease)
{
	if (context != BACKEND_CONTEXT || linear_address != 0xe0000u ||
	    byte_count != 0x10000u || lease == NULL)
		return DOS_EMS_PAGE_FRAME_FAULT;
	*lease = 0x2001u;
	return DOS_EMS_PAGE_FRAME_OK;
}

static enum dos_ems_page_frame_status ems_frame_release(
	kernel_object_handle_t context, dos_ems_page_frame_lease_t lease)
{
	return context == BACKEND_CONTEXT && lease == 0x2001u
		       ? DOS_EMS_PAGE_FRAME_OK
		       : DOS_EMS_PAGE_FRAME_FAULT;
}

static enum dos_ems_page_frame_status ems_frame_map(
	kernel_object_handle_t context, dos_ems_page_frame_lease_t lease,
	uint8_t physical_page, uint64_t source_physical_address)
{
	return context == BACKEND_CONTEXT && lease == 0x2001u &&
		       physical_page < DOS_EMS_PAGE_FRAME_SLOTS &&
		       source_physical_address < DOS_GUEST_32_ADDRESS_LIMIT
		       ? DOS_EMS_PAGE_FRAME_OK
		       : DOS_EMS_PAGE_FRAME_FAULT;
}

static enum dos_ems_page_frame_status ems_frame_unmap(
	kernel_object_handle_t context, dos_ems_page_frame_lease_t lease,
	uint8_t physical_page)
{
	return context == BACKEND_CONTEXT && lease == 0x2001u &&
		       physical_page < DOS_EMS_PAGE_FRAME_SLOTS
		       ? DOS_EMS_PAGE_FRAME_OK
		       : DOS_EMS_PAGE_FRAME_FAULT;
}

static enum dos_vcpi_handoff_status ems_transfer_handoff(
	kernel_object_handle_t context,
	const struct dos_vcpi_handoff_request *request,
	struct dos_cpu_state *state)
{
	if (context != BACKEND_CONTEXT || request == NULL || state == NULL ||
	    request->kind != DOS_VCPI_HANDOFF_ENTER_PROTECTED)
		return DOS_VCPI_HANDOFF_FAULT;
	state->eax = 0xfeedbeefu;
	state->mode = (uint32_t)DOS_CPU_PROTECTED32;
	return DOS_VCPI_HANDOFF_TRANSFERRED;
}

static const struct dos_ems_page_ops ems_page_ops = {
	.query = ems_page_query,
	.allocate = ems_page_allocate,
	.release = ems_page_release,
};

static const struct dos_vcpi_platform_ops ems_vcpi_ops = {
	.translate_low_page = ems_translate_low_page,
	.read_virtual_cr0 = ems_read_virtual_cr0,
	.query_pic_mappings = ems_query_pic_mappings,
	.set_pic_mappings = ems_set_pic_mappings,
	.handoff = ems_transfer_handoff,
};

static const struct dos_ems_page_frame_ops ems_frame_ops = {
	.acquire = ems_frame_acquire,
	.release = ems_frame_release,
	.map = ems_frame_map,
	.unmap = ems_frame_unmap,
};

static const struct dos_ems_page_frame_binding ems_frame_binding = {
	.ops = &ems_frame_ops,
	.context = BACKEND_CONTEXT,
	.lease = 0x2001u,
	.linear_address = 0xe0000u,
	.byte_count = 0x10000u,
};

static const struct dos_ems_runtime_config ems_runtime_config = {
	.service = {
		.page_frame_segment = 0xe000u,
		.reserved = 0u,
		.reserved2 = 0u,
	},
	.device_name = {'E', 'M', 'M', 'X', 'X', 'X', 'X', '0'},
	.reserved = {0u},
};

static struct dos_exec_handoff_plan make_handoff(void)
{
	struct dos_exec_handoff_plan handoff = {0};
	uint16_t child_psp = 0x2345u;
	uint16_t entry_cs = 0x3456u;
	uint16_t entry_ip = 0x789au;
	uint16_t stack_segment = 0x4567u;
	uint16_t initial_sp = 0x1000u;

	handoff.entry_state.eax = 0xff00u;
	handoff.entry_state.ebx = 0xff00u;
	handoff.entry_state.edx = child_psp;
	handoff.entry_state.esi = entry_ip;
	handoff.entry_state.edi = initial_sp;
	handoff.entry_state.esp = initial_sp;
	handoff.entry_state.eip = entry_ip;
	handoff.entry_state.eflags = DOS_EFLAGS_IF | 2u;
	handoff.entry_state.cs = entry_cs;
	handoff.entry_state.ss = stack_segment;
	handoff.entry_state.ds = child_psp;
	handoff.entry_state.es = child_psp;
	handoff.entry_state.mode = (uint32_t)DOS_CPU_REAL16;
	handoff.stack_image.segment = stack_segment;
	handoff.stack_image.offset =
	    (uint16_t)(initial_sp - DOS_EXEC_HANDOFF_STACK_BYTES);
	handoff.stack_image.bytes[0] = (uint8_t)entry_ip;
	handoff.stack_image.bytes[1] = (uint8_t)(entry_ip >> 8);
	handoff.stack_image.bytes[2] = (uint8_t)entry_cs;
	handoff.stack_image.bytes[3] = (uint8_t)(entry_cs >> 8);
	handoff.child_psp = child_psp;
	handoff.format = (uint8_t)DOS_IMAGE_MZ;
	handoff.stack_word_count = DOS_EXEC_HANDOFF_STACK_WORDS;
	return handoff;
}

static bool initialize_fixture(struct dos_exec_backend_session_table *table,
			       struct dos_machine *machine)
{
	return dos_machine_configure(machine, &machine_ops, MACHINE_CONTEXT,
				     GUEST_CAPACITY, false) == DOS_MACHINE_OK &&
	       dos_exec_backend_session_table_construct(table) ==
		   DOS_EXEC_BACKEND_SESSION_OK &&
	       dos_exec_backend_session_table_initialize(table, TABLE_IDENTITY) ==
		   DOS_EXEC_BACKEND_SESSION_OK;
}

static void reset_adapter(void)
{
	configured_prepare_status = DOS_EXEC_BACKEND_PREPARED;
	configured_release_status = DOS_EXEC_BACKEND_RELEASED;
	configured_run_status = DOS_EXEC_BACKEND_EVENT;
	configured_event = (struct dos_execution_event){
	    .kind = (uint32_t)DOS_EXEC_EVENT_SOFTWARE_INTERRUPT,
	    .value = 0u,
	    .port = 0u,
	    .vector = 0x21u,
	    .io_width = 0u,
	    .io_write = 0u,
	    .reserved = {0u},
	};
	configured_prepare_result = (struct dos_exec_backend_prepare_result){
	    .backend_context = BACKEND_CONTEXT,
	    .failure_detail = 0u,
	    .reserved = {0u},
	};
	prepare_calls = 0u;
	release_calls = 0u;
	run_calls = 0u;
	port_read_calls = 0u;
	port_write_calls = 0u;
	configured_port_read_value = 0xabcdu;
	observed_port_write_value = 0u;
	configured_port_read_status = DOS_MACHINE_OK;
	configured_port_write_status = DOS_MACHINE_OK;
	arguments_valid = 1u;
	expected_handoff = make_handoff();
}

static int test_dormant_publish_stop_and_aba(void)
{
	struct dos_exec_backend_session_table table =
	    DOS_EXEC_BACKEND_SESSION_TABLE_INITIALIZER;
	struct dos_exec_backend_session_handle first = {
	    .value = HANDLE_SENTINEL,
	};
	struct dos_exec_backend_session_handle second = {
	    .value = HANDLE_SENTINEL,
	};
	struct dos_exec_handoff_plan tampered;
	struct dos_machine machine;
	enum dos_exec_backend_session_state state =
	    DOS_EXEC_BACKEND_SESSION_VACANT;
	uint32_t detail = DETAIL_SENTINEL;

	reset_adapter();
	if (!initialize_fixture(&table, &machine) ||
	    dos_exec_backend_session_prepare(
		&table, &backend_ops, ADAPTER_CONTEXT, MACHINE_IDENTITY,
		&machine, &expected_handoff, &first, &detail) !=
		DOS_EXEC_BACKEND_SESSION_OK ||
	    first.value == HANDLE_SENTINEL || detail != 0u ||
	    prepare_calls != 1u || run_calls != 0u || !arguments_valid ||
	    dos_exec_backend_session_get_state(&table, first, &state) !=
		DOS_EXEC_BACKEND_SESSION_OK ||
	    state != DOS_EXEC_BACKEND_SESSION_DORMANT)
		return 1;
	tampered = expected_handoff;
	tampered.entry_state.eax++;
	tampered.entry_state.ebx++;
	if (dos_exec_backend_session_preflight_publish(
		&table, first, &backend_ops, ADAPTER_CONTEXT,
		MACHINE_IDENTITY, &machine, &tampered) !=
		DOS_EXEC_BACKEND_SESSION_IDENTITY_MISMATCH ||
	    prepare_calls != 1u || release_calls != 0u || run_calls != 0u)
		return 2;
	if (dos_exec_backend_session_preflight_publish(
		&table, first, &backend_ops, ADAPTER_CONTEXT,
		MACHINE_IDENTITY, &machine, &expected_handoff) !=
		DOS_EXEC_BACKEND_SESSION_OK ||
	    dos_exec_backend_session_publish(
		&table, first, &backend_ops, ADAPTER_CONTEXT,
		MACHINE_IDENTITY, &machine, &expected_handoff) !=
		DOS_EXEC_BACKEND_SESSION_OK ||
	    dos_exec_backend_session_get_state(&table, first, &state) !=
		DOS_EXEC_BACKEND_SESSION_OK ||
	    state != DOS_EXEC_BACKEND_SESSION_RUNNABLE || run_calls != 0u)
		return 3;
	configured_release_status = DOS_EXEC_BACKEND_RETAINED;
	if (dos_exec_backend_session_stop(&table, first, &backend_ops,
					  ADAPTER_CONTEXT) !=
		DOS_EXEC_BACKEND_SESSION_RELEASE_RETAINED ||
	    dos_exec_backend_session_get_state(&table, first, &state) !=
		DOS_EXEC_BACKEND_SESSION_OK ||
	    state != DOS_EXEC_BACKEND_SESSION_RUNNABLE || release_calls != 1u)
		return 4;
	configured_release_status = DOS_EXEC_BACKEND_RELEASED;
	if (dos_exec_backend_session_stop(&table, first, &backend_ops,
					  ADAPTER_CONTEXT) !=
		DOS_EXEC_BACKEND_SESSION_OK ||
	    !dos_exec_backend_session_table_is_drained(&table) ||
	    dos_exec_backend_session_retire(&table, first) !=
		DOS_EXEC_BACKEND_SESSION_OK ||
	    dos_exec_backend_session_get_state(&table, first, &state) !=
		DOS_EXEC_BACKEND_SESSION_STALE_HANDLE)
		return 5;
	if (dos_exec_backend_session_prepare(
		&table, &backend_ops, ADAPTER_CONTEXT, MACHINE_IDENTITY,
		&machine, &expected_handoff, &second, &detail) !=
		DOS_EXEC_BACKEND_SESSION_OK ||
	    second.value == first.value ||
	    dos_exec_backend_session_get_state(&table, first, &state) !=
		DOS_EXEC_BACKEND_SESSION_STALE_HANDLE ||
	    dos_exec_backend_session_stop(&table, second, &backend_ops,
					  ADAPTER_CONTEXT) !=
		DOS_EXEC_BACKEND_SESSION_OK ||
	    dos_exec_backend_session_retire(&table, second) !=
		DOS_EXEC_BACKEND_SESSION_OK)
		return 6;
	return arguments_valid ? 0 : 7;
}

static int test_rejection_and_uncertainty(void)
{
	struct dos_exec_backend_session_table table =
	    DOS_EXEC_BACKEND_SESSION_TABLE_INITIALIZER;
	struct dos_exec_backend_session_handle handle = {
	    .value = HANDLE_SENTINEL,
	};
	struct dos_machine machine;
	uint32_t detail = DETAIL_SENTINEL;

	reset_adapter();
	if (!initialize_fixture(&table, &machine))
		return 1;
	configured_prepare_status = DOS_EXEC_BACKEND_REJECTED;
	configured_prepare_result.backend_context = KERNEL_OBJECT_HANDLE_INVALID;
	configured_prepare_result.failure_detail = 0x12345678u;
	if (dos_exec_backend_session_prepare(
		&table, &backend_ops, ADAPTER_CONTEXT, MACHINE_IDENTITY,
		&machine, &expected_handoff, &handle, &detail) !=
		DOS_EXEC_BACKEND_SESSION_PREPARE_REJECTED ||
	    handle.value != HANDLE_SENTINEL || detail != 0x12345678u ||
	    !dos_exec_backend_session_table_is_drained(&table))
		return 2;
	configured_prepare_status = DOS_EXEC_BACKEND_PREPARE_UNCERTAIN;
	configured_prepare_result.backend_context = BACKEND_CONTEXT;
	configured_prepare_result.failure_detail = 0u;
	detail = DETAIL_SENTINEL;
	if (dos_exec_backend_session_prepare(
		&table, &backend_ops, ADAPTER_CONTEXT, MACHINE_IDENTITY,
		&machine, &expected_handoff, &handle, &detail) !=
		DOS_EXEC_BACKEND_SESSION_POISONED ||
	    handle.value != HANDLE_SENTINEL || detail != DETAIL_SENTINEL ||
	    table.poisoned != 1u || prepare_calls != 2u ||
	    dos_exec_backend_session_prepare(
		&table, &backend_ops, ADAPTER_CONTEXT, MACHINE_IDENTITY,
		&machine, &expected_handoff, &handle, &detail) !=
		DOS_EXEC_BACKEND_SESSION_POISONED ||
	    prepare_calls != 2u)
		return 3;
	return arguments_valid ? 0 : 4;
}

static int test_precise_run_and_imprecise_poison(void)
{
	struct dos_exec_backend_session_table table =
	    DOS_EXEC_BACKEND_SESSION_TABLE_INITIALIZER;
	struct dos_exec_backend_session_handle handle;
	struct dos_cpu_state state = {.eax = 0xa5a5a5a5u};
	struct dos_cpu_state observed;
	struct dos_cpu_state replacement;
	struct dos_execution_event event = {.value = 0x5a5a5a5au};
	struct dos_machine machine;
	uint32_t detail = DETAIL_SENTINEL;
	uint32_t original_ip;

	reset_adapter();
	if (!initialize_fixture(&table, &machine) ||
	    dos_exec_backend_session_prepare(
		&table, &backend_ops, ADAPTER_CONTEXT, MACHINE_IDENTITY,
		&machine, &expected_handoff, &handle, &detail) !=
		DOS_EXEC_BACKEND_SESSION_OK ||
	    dos_exec_backend_session_publish(
		&table, handle, &backend_ops, ADAPTER_CONTEXT,
		MACHINE_IDENTITY, &machine, &expected_handoff) !=
		DOS_EXEC_BACKEND_SESSION_OK)
		return 1;
	original_ip = expected_handoff.entry_state.eip;
	if (dos_exec_backend_session_run_until_event(
		&table, handle, &backend_ops, ADAPTER_CONTEXT,
		MACHINE_IDENTITY, &machine, &state, &event) !=
		DOS_EXEC_BACKEND_SESSION_OK ||
	    state.eip != (uint16_t)(dos_register_low16(original_ip) + 1u) ||
	    event.kind != (uint32_t)DOS_EXEC_EVENT_SOFTWARE_INTERRUPT ||
	    event.vector != 0x21u || run_calls != 1u)
		return 2;
	observed = state;
	replacement = observed;
	replacement.eax = 0x12345678u;
	/* A DOS service may commit virtual A20 only while execution is stopped
	 * at this exact event.  A stale CPU snapshot must still be rejected; the
	 * matching replacement publishes both state changes atomically. */
	machine.a20_enabled = true;
	if (dos_exec_backend_session_replace_state(
		&table, handle, &backend_ops, ADAPTER_CONTEXT,
		MACHINE_IDENTITY, &machine, &expected_handoff.entry_state,
		&replacement) != DOS_EXEC_BACKEND_SESSION_STATE_MISMATCH ||
	    dos_exec_backend_session_replace_state(
		&table, handle, &backend_ops, ADAPTER_CONTEXT,
		MACHINE_IDENTITY, &machine, &observed, &replacement) !=
		DOS_EXEC_BACKEND_SESSION_OK)
		return 3;
	if (dos_exec_backend_session_run_until_event(
		&table, handle, &backend_ops, ADAPTER_CONTEXT,
		MACHINE_IDENTITY, &machine, &state, &event) !=
		DOS_EXEC_BACKEND_SESSION_OK || state.eax != 0x12345678u ||
	    state.eip != (uint16_t)(dos_register_low16(observed.eip) + 1u) ||
	    run_calls != 2u)
		return 4;
	configured_run_status = DOS_EXEC_BACKEND_RUN_UNCERTAIN;
	state.eax = 0xa5a5a5a5u;
	event.value = 0x5a5a5a5au;
	if (dos_exec_backend_session_run_until_event(
		&table, handle, &backend_ops, ADAPTER_CONTEXT,
		MACHINE_IDENTITY, &machine, &state, &event) !=
		DOS_EXEC_BACKEND_SESSION_POISONED ||
	    state.eax != 0xa5a5a5a5u || event.value != 0x5a5a5a5au ||
	    table.poisoned != 1u || run_calls != 3u)
		return 5;
	return arguments_valid ? 0 : 6;
}

static int test_shared_int21_service_resume(void)
{
	struct dos_exec_backend_session_table table =
		DOS_EXEC_BACKEND_SESSION_TABLE_INITIALIZER;
	struct dos_exec_backend_session_handle handle;
	struct dos_memory_arena arena =
		DOS_MEMORY_ARENA_INITIALIZER(ARENA_IDENTITY);
	struct dos_personality personality = {0};
	struct dos_execution_step_result step;
	struct dos_machine machine;
	enum dos_exec_backend_session_status session_status;
	uint32_t detail = DETAIL_SENTINEL;

	reset_adapter();
	expected_handoff.entry_state.eax = 0x3000u;
	expected_handoff.entry_state.ebx = 0x3000u;
	if (!initialize_fixture(&table, &machine))
		return 1;
	if (dos_memory_arena_initialize(&arena, &machine, TEST_ARENA_HEAD,
					TEST_ARENA_END) != DOS_SUCCESS)
		return 2;
	if (dos_personality_initialize(
		&personality, PERSONALITY_IDENTITY, MACHINE_IDENTITY, &machine,
		&arena, RUNTIME_IDENTITY, expected_handoff.child_psp,
		&test_drive_config) !=
	    DOS_PERSONALITY_READY)
		return 3;
	session_status = dos_exec_backend_session_prepare(
		&table, &backend_ops, ADAPTER_CONTEXT, MACHINE_IDENTITY,
		&machine, &expected_handoff, &handle, &detail);
	if (session_status != DOS_EXEC_BACKEND_SESSION_OK)
		return 10 + (int)session_status;
	if (dos_exec_backend_session_publish(
		&table, handle, &backend_ops, ADAPTER_CONTEXT,
		MACHINE_IDENTITY, &machine, &expected_handoff) !=
	    DOS_EXEC_BACKEND_SESSION_OK)
		return 5;

	step = dos_execution_step(&table, handle, &backend_ops,
				  ADAPTER_CONTEXT, MACHINE_IDENTITY, &machine,
				  &personality);
	if (step.status !=
			(uint32_t)DOS_EXECUTION_STEP_SERVICE_RESUMED ||
	    step.session_status !=
			(uint32_t)DOS_EXEC_BACKEND_SESSION_OK ||
	    step.interrupt.disposition != DOS_INTERRUPT_HANDLED ||
	    step.event.kind !=
			(uint32_t)DOS_EXEC_EVENT_SOFTWARE_INTERRUPT ||
	    dos_register_low16(step.state.eax) != 0x1706u || run_calls != 1u)
		return 6;

	configured_event.kind = (uint32_t)DOS_EXEC_EVENT_HALTED;
	configured_event.vector = 0u;
	step = dos_execution_step(&table, handle, &backend_ops,
				  ADAPTER_CONTEXT, MACHINE_IDENTITY, &machine,
				  &personality);
	if (step.status != (uint32_t)DOS_EXECUTION_STEP_HALTED ||
	    step.event.kind != (uint32_t)DOS_EXEC_EVENT_HALTED ||
	    dos_register_low16(step.state.eax) != 0x1706u || run_calls != 2u ||
	    dos_exec_backend_session_stop(&table, handle, &backend_ops,
					  ADAPTER_CONTEXT) !=
		DOS_EXEC_BACKEND_SESSION_OK ||
	    dos_exec_backend_session_retire(&table, handle) !=
		DOS_EXEC_BACKEND_SESSION_OK)
		return 7;
	return arguments_valid ? 0 : 8;
}

static int test_shared_non_dos_interrupt_reflection(void)
{
	struct dos_exec_backend_session_table table =
		DOS_EXEC_BACKEND_SESSION_TABLE_INITIALIZER;
	struct dos_exec_backend_session_handle handle;
	struct dos_memory_arena arena =
		DOS_MEMORY_ARENA_INITIALIZER(ARENA_IDENTITY);
	struct dos_personality personality = {0};
	struct dos_execution_step_result step;
	struct dos_machine machine;
	enum dos_exec_backend_session_status session_status;
	dos_linear_address_t stack_linear;
	uint32_t detail = DETAIL_SENTINEL;
	size_t vector_linear = 0x10u * 4u;

	reset_adapter();
	configured_event.vector = 0x10u;
	guest_memory[vector_linear] = 0x34u;
	guest_memory[vector_linear + 1u] = 0x12u;
	guest_memory[vector_linear + 2u] = 0x78u;
	guest_memory[vector_linear + 3u] = 0x56u;
	if (!initialize_fixture(&table, &machine))
		return 1;
	if (dos_memory_arena_initialize(&arena, &machine, TEST_ARENA_HEAD,
					TEST_ARENA_END) != DOS_SUCCESS)
		return 2;
	if (dos_personality_initialize(
		&personality, PERSONALITY_IDENTITY, MACHINE_IDENTITY, &machine,
		&arena, RUNTIME_IDENTITY, expected_handoff.child_psp,
		&test_drive_config) !=
	    DOS_PERSONALITY_READY)
		return 3;
	session_status = dos_exec_backend_session_prepare(
		&table, &backend_ops, ADAPTER_CONTEXT, MACHINE_IDENTITY,
		&machine, &expected_handoff, &handle, &detail);
	if (session_status != DOS_EXEC_BACKEND_SESSION_OK)
		return 10 + (int)session_status;
	if (dos_exec_backend_session_publish(
		&table, handle, &backend_ops, ADAPTER_CONTEXT,
		MACHINE_IDENTITY, &machine, &expected_handoff) !=
	    DOS_EXEC_BACKEND_SESSION_OK)
		return 5;

	step = dos_execution_step(&table, handle, &backend_ops,
				  ADAPTER_CONTEXT, MACHINE_IDENTITY, &machine,
				  &personality);
	stack_linear = dos_far_to_linear(
		expected_handoff.entry_state.ss,
		(uint16_t)(expected_handoff.entry_state.esp - 6u), false);
	if (step.status != (uint32_t)DOS_EXECUTION_STEP_CHAIN_RESUMED ||
	    step.session_status != (uint32_t)DOS_EXEC_BACKEND_SESSION_OK ||
	    step.interrupt.disposition != DOS_INTERRUPT_CHAIN ||
	    step.state.cs != 0x5678u || step.state.eip != 0x1234u ||
	    step.state.esp != 0x0ffau ||
	    (step.state.eflags & (DOS_EFLAGS_IF | DOS_EFLAGS_TF)) != 0u ||
	    guest_memory[stack_linear] != 0x9bu ||
	    guest_memory[stack_linear + 1u] != 0x78u ||
	    guest_memory[stack_linear + 2u] != 0x56u ||
	    guest_memory[stack_linear + 3u] != 0x34u ||
	    guest_memory[stack_linear + 4u] != 0x02u ||
	    guest_memory[stack_linear + 5u] != 0x02u || run_calls != 1u)
		return 6;
	if (dos_exec_backend_session_stop(&table, handle, &backend_ops,
					  ADAPTER_CONTEXT) !=
		    DOS_EXEC_BACKEND_SESSION_OK ||
	    dos_exec_backend_session_retire(&table, handle) !=
		    DOS_EXEC_BACKEND_SESSION_OK)
		return 7;
	return arguments_valid ? 0 : 8;
}

static int test_shared_port_policy_service(void)
{
	struct dos_exec_backend_session_table table =
		DOS_EXEC_BACKEND_SESSION_TABLE_INITIALIZER;
	struct dos_exec_backend_session_handle handle;
	struct dos_memory_arena arena =
		DOS_MEMORY_ARENA_INITIALIZER(ARENA_IDENTITY);
	struct dos_personality personality = {0};
	struct dos_execution_step_result step;
	struct dos_machine machine;
	enum dos_exec_backend_session_status session_status;
	uint32_t detail = DETAIL_SENTINEL;

	reset_adapter();
	configured_event = (struct dos_execution_event){
		.kind = (uint32_t)DOS_EXEC_EVENT_PORT_IO,
		.port = 0x1234u,
		.io_width = (uint8_t)DOS_IO_WIDTH_16,
		.io_write = 0u,
	};
	if (!initialize_fixture(&table, &machine))
		return 1;
	if (dos_memory_arena_initialize(&arena, &machine, TEST_ARENA_HEAD,
					TEST_ARENA_END) != DOS_SUCCESS)
		return 2;
	if (dos_personality_initialize(
		&personality, PERSONALITY_IDENTITY, MACHINE_IDENTITY, &machine,
		&arena, RUNTIME_IDENTITY, expected_handoff.child_psp,
		&test_drive_config) !=
	    DOS_PERSONALITY_READY)
		return 3;
	session_status = dos_exec_backend_session_prepare(
		&table, &backend_ops, ADAPTER_CONTEXT, MACHINE_IDENTITY,
		&machine, &expected_handoff, &handle, &detail);
	if (session_status != DOS_EXEC_BACKEND_SESSION_OK)
		return 10 + (int)session_status;
	if (dos_exec_backend_session_publish(
		&table, handle, &backend_ops, ADAPTER_CONTEXT,
		MACHINE_IDENTITY, &machine, &expected_handoff) !=
	    DOS_EXEC_BACKEND_SESSION_OK)
		return 5;

	step = dos_execution_step(&table, handle, &backend_ops,
				  ADAPTER_CONTEXT, MACHINE_IDENTITY, &machine,
				  &personality);
	if (step.status != (uint32_t)DOS_EXECUTION_STEP_PORT_RESUMED ||
	    step.interrupt.disposition != DOS_INTERRUPT_HANDLED ||
	    step.interrupt.machine_status != DOS_MACHINE_OK ||
	    dos_register_low16(step.state.eax) != 0xabcdu ||
	    step.event.value != 0xabcdu || port_read_calls != 1u)
		return 6;

	configured_event = (struct dos_execution_event){
		.kind = (uint32_t)DOS_EXEC_EVENT_PORT_IO,
		.value = 0x5au,
		.port = 0x1234u,
		.io_width = (uint8_t)DOS_IO_WIDTH_8,
		.io_write = 1u,
	};
	step = dos_execution_step(&table, handle, &backend_ops,
				  ADAPTER_CONTEXT, MACHINE_IDENTITY, &machine,
				  &personality);
	if (step.status != (uint32_t)DOS_EXECUTION_STEP_PORT_RESUMED ||
	    port_write_calls != 1u || observed_port_write_value != 0x5au)
		return 7;

	configured_event = (struct dos_execution_event){
		.kind = (uint32_t)DOS_EXEC_EVENT_PORT_IO,
		.port = 0x1234u,
		.io_width = (uint8_t)DOS_IO_WIDTH_16,
	};
	configured_port_read_status = DOS_MACHINE_IO_DENIED;
	step = dos_execution_step(&table, handle, &backend_ops,
				  ADAPTER_CONTEXT, MACHINE_IDENTITY, &machine,
				  &personality);
	if (step.status != (uint32_t)DOS_EXECUTION_STEP_BLOCKED ||
	    step.interrupt.disposition != DOS_INTERRUPT_BLOCKED ||
	    step.interrupt.machine_status != DOS_MACHINE_IO_DENIED ||
	    port_read_calls != 2u)
		return 8;
	if (dos_exec_backend_session_stop(&table, handle, &backend_ops,
					  ADAPTER_CONTEXT) !=
		    DOS_EXEC_BACKEND_SESSION_OK ||
	    dos_exec_backend_session_retire(&table, handle) !=
		    DOS_EXEC_BACKEND_SESSION_OK)
		return 9;
	return arguments_valid ? 0 : 10;
}

static int test_shared_ems_transfer_boundary(void)
{
	struct dos_exec_backend_session_table table =
		DOS_EXEC_BACKEND_SESSION_TABLE_INITIALIZER;
	struct dos_exec_backend_session_handle handle;
	struct dos_memory_arena arena =
		DOS_MEMORY_ARENA_INITIALIZER(ARENA_IDENTITY);
	struct dos_personality personality = {0};
	struct dos_execution_step_result step;
	struct dos_cpu_state vm86_state;
	struct dos_machine machine;
	enum dos_exec_backend_session_status session_status;
	uint32_t slot_index;
	uint32_t detail = DETAIL_SENTINEL;

	reset_adapter();
	configured_event.vector = 0x67u;
	vm86_state = expected_handoff.entry_state;
	vm86_state.eax = 0xde0cu;
	vm86_state.esi = 0x1000u;
	vm86_state.mode = (uint32_t)DOS_CPU_VM86;
	if (!initialize_fixture(&table, &machine))
		return 1;
	if (dos_memory_arena_initialize(&arena, &machine, TEST_ARENA_HEAD,
					TEST_ARENA_END) != DOS_SUCCESS)
		return 2;
	if (dos_personality_initialize(
		    &personality, PERSONALITY_IDENTITY, MACHINE_IDENTITY, &machine,
		    &arena, RUNTIME_IDENTITY, expected_handoff.child_psp,
		    &test_drive_config) != DOS_PERSONALITY_READY ||
	    dos_personality_set_ems(
		    &personality, &ems_page_ops, MACHINE_IDENTITY,
		    &ems_frame_binding, &ems_vcpi_ops, BACKEND_CONTEXT,
		    &ems_runtime_config) != DOS_PERSONALITY_READY)
		return 3;
	session_status = dos_exec_backend_session_prepare(
		&table, &backend_ops, ADAPTER_CONTEXT, MACHINE_IDENTITY,
		&machine, &expected_handoff, &handle, &detail);
	if (session_status != DOS_EXEC_BACKEND_SESSION_OK)
		return 10 + (int)session_status;
	if (dos_exec_backend_session_publish(
		    &table, handle, &backend_ops, ADAPTER_CONTEXT,
		    MACHINE_IDENTITY, &machine, &expected_handoff) !=
		    DOS_EXEC_BACKEND_SESSION_OK ||
	    dos_exec_backend_session_replace_state(
		    &table, handle, &backend_ops, ADAPTER_CONTEXT,
		    MACHINE_IDENTITY, &machine, &expected_handoff.entry_state,
		    &vm86_state) != DOS_EXEC_BACKEND_SESSION_OK)
		return 5;

	step = dos_execution_step(&table, handle, &backend_ops,
				  ADAPTER_CONTEXT, MACHINE_IDENTITY, &machine,
				  &personality);
	slot_index = (uint32_t)(handle.value &
				DOS_EXEC_BACKEND_SESSION_SLOT_MASK) - 1u;
	if (step.status !=
			(uint32_t)DOS_EXECUTION_STEP_EXECUTION_TRANSFERRED ||
	    step.interrupt.disposition != DOS_INTERRUPT_EXECUTION_TRANSFERRED ||
	    step.interrupt.machine_status != DOS_MACHINE_OK ||
	    step.state.eax != 0xfeedbeefu ||
	    step.state.mode != (uint32_t)DOS_CPU_PROTECTED32 ||
	    table.slots[slot_index].current_state.eax != vm86_state.eax ||
	    table.slots[slot_index].current_state.mode !=
		    vm86_state.mode ||
	    run_calls != 1u)
		return 6;
	if (dos_exec_backend_session_stop(&table, handle, &backend_ops,
					  ADAPTER_CONTEXT) !=
		    DOS_EXEC_BACKEND_SESSION_OK ||
	    dos_exec_backend_session_retire(&table, handle) !=
		    DOS_EXEC_BACKEND_SESSION_OK)
		return 7;
	return arguments_valid ? 0 : 8;
}

static int run_tests(void)
{
	int status = test_dormant_publish_stop_and_aba();

	if (status != 0)
		return 10 + status;
	status = test_rejection_and_uncertainty();
	if (status != 0)
		return 20 + status;
	status = test_precise_run_and_imprecise_poison();
	if (status != 0)
		return 30 + status;
	status = test_shared_int21_service_resume();
	if (status != 0)
		return 40 + status;
	status = test_shared_non_dos_interrupt_reflection();
	if (status != 0)
		return 50 + status;
	status = test_shared_port_policy_service();
	if (status != 0)
		return 60 + status;
	status = test_shared_ems_transfer_boundary();
	if (status != 0)
		return 70 + status;
	return 0;
}

DOSC32_TEST_ENTRY(run_tests)

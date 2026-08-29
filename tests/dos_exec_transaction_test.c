// SPDX-License-Identifier: GPL-2.0-only
/* Freestanding tests for the common DOS EXEC transaction prefix. */
#include "dos_exec_int21.h"
#include "test_entry.h"
#include "dos_exec_native.h"

#define TEST_COORDINATOR_ID ((kernel_object_handle_t)0x434f4f5244494e41ull)
#define TEST_MACHINE_ID ((kernel_object_handle_t)0x4d414348494e4541ull)
#define TEST_MACHINE_CONTEXT ((kernel_object_handle_t)0x4d41434843545831ull)
#define TEST_FILE_ID ((kernel_object_handle_t)0x46494c4541444150ull)
#define TEST_FILE_TABLE_ID ((kernel_object_handle_t)0x46494c455441424cull)
#define TEST_OTHER_FILE_TABLE_ID ((kernel_object_handle_t)0x46494c4554414232ull)
#define TEST_FILE_CONTEXT ((kernel_object_handle_t)0x46494c4543545831ull)
#define TEST_RUNTIME_ID ((kernel_object_handle_t)0x52554e54494d4531ull)
#define TEST_OTHER_RUNTIME_ID ((kernel_object_handle_t)0x52554e54494d4532ull)
#define TEST_SFT_ID ((kernel_object_handle_t)0x5346544144415054ull)
#define TEST_SFT_CONTEXT ((kernel_object_handle_t)0x5346544354583031ull)
#define TEST_DRIVE_ID ((kernel_object_handle_t)0x4452495645414450ull)
#define TEST_DRIVE_CONTEXT ((kernel_object_handle_t)0x4452495645435458ull)
#define TEST_OBSERVER_ID ((kernel_object_handle_t)0x4f42534552564552ull)
#define TEST_OBSERVER_CONTEXT ((kernel_object_handle_t)0x4f42534354583031ull)
#define TEST_READER_CONTEXT ((kernel_object_handle_t)0x5245414445523031ull)
#define TEST_MEMORY_ARENA_ID ((kernel_object_handle_t)0x4d454d4152454e41ull)
#define TEST_MEMORY_TABLE_ID ((dos_memory_lease_table_identity_t)0x4d454d31u)
#define TEST_BACKEND_TABLE_ID ((kernel_object_handle_t)0x42454e4454414231ull)
#define TEST_BACKEND_ID ((kernel_object_handle_t)0x42454e4441445031ull)
#define TEST_BACKEND_CONTEXT ((kernel_object_handle_t)0x42454e4443545831ull)
#define TEST_BACKEND_SESSION ((kernel_object_handle_t)0x42454e4453455331ull)
#define TEST_MEMORY_HEAD 0x5000u
#define TEST_MEMORY_END 0x7000u
#define TEST_INITIAL_FREE_PARAGRAPHS \
	(TEST_MEMORY_END - TEST_MEMORY_HEAD - 1u)
#define TEST_PSP_PARAGRAPHS 16u

#define HANDLE_SENTINEL 0xa5a55a5af00ff00full
#define DETAIL_SENTINEL 0xa5a55a5au
#define BYTE_SENTINEL ((uint8_t)0xa5u)

#define EVENT_OBSERVER_ACQUIRE 1u
#define EVENT_FILE_OPEN 2u
#define EVENT_FILE_PROBE 3u
#define EVENT_FILE_CLOSE 4u
#define EVENT_OBSERVER_RELEASE 5u
#define EVENT_OBSERVER_QUARANTINE 6u
#define EVENT_MACHINE_READ 7u
#define EVENT_MACHINE_WRITE 8u
#define EVENT_FILE_READ 9u
#define EVENT_CAPACITY 64u

#define REENTRY_NONE 0u
#define REENTRY_OBSERVER_ACQUIRE 1u
#define REENTRY_FILE_OPEN 2u
#define REENTRY_FILE_PROBE 3u
#define REENTRY_FILE_CLOSE 4u
#define REENTRY_ABORT_FILE_CLOSE 5u
#define REENTRY_OBSERVER_RELEASE 6u
#define REENTRY_OBSERVER_QUARANTINE 7u
#define REENTRY_ENVIRONMENT_READ 8u
#define REENTRY_NAME_READ 9u
#define REENTRY_ENVIRONMENT_PREPARE 10u
#define REENTRY_IMAGE_READ 11u
#define REENTRY_TARGET_PREPARE 12u
#define REENTRY_RESIDENT_LOAD 13u
#define REENTRY_RELOCATE 14u
#define REENTRY_PARENT_CAPTURE 15u
#define REENTRY_INHERITANCE 16u
#define REENTRY_INITIAL_STATE 17u

#define TEST_GUEST_MEMORY_SIZE DOS_REAL_MODE_ADDRESS_LIMIT
#define TEST_EXECUTABLE_NAME_LINEAR 0x00020100u
#define TEST_EXEC_COMMAND_TAIL_POINTER_OFFSET 2u
#define TEST_EXEC_FIRST_FCB_POINTER_OFFSET 6u
#define TEST_EXEC_SECOND_FCB_POINTER_OFFSET 10u

static const uint8_t canonical_executable_name[] = {
    'C', ':', 0x5cu, 'B', 'I', 'N', 0x5cu, 'A',
    'P', 'P', '.',   'E', 'X', 'E', 0u,
};
static uint8_t executable_name[DOS_EXEC_NAME_SCAN_LIMIT];

struct test_fixture {
	struct dos_exec_transaction_table transactions;
	struct dos_exec_file_lease_table files;
	struct dos_memory_lease_table memory_leases;
	struct dos_memory_arena memory_arena;
	struct dos_process_runtime runtime;
	struct dos_exec_backend_session_table backend_sessions;
	struct dos_machine machine;
	struct dos_exec_transaction_services services;
};

static enum dos_exec_file_adapter_status configured_open_status;
static enum dos_exec_file_adapter_status configured_probe_status;
static enum dos_exec_file_close_result configured_close_result;
static enum dos_exec_observer_adapter_status configured_observer_acquire;
static enum dos_exec_observer_adapter_status configured_observer_release;
static enum dos_exec_observer_adapter_status configured_observer_quarantine;
static enum dos_image_read_status configured_read_status;
static enum dos_exec_backend_prepare_status configured_backend_prepare;
static enum dos_exec_backend_release_status configured_backend_release;
static uint32_t configured_open_detail;
static uint32_t configured_probe_detail;
static file_offset_t configured_open_size;
static uint8_t configured_is_device;
static uint8_t configured_short_read;
static const uint8_t *expected_name_pointer;
static size_t expected_name_length;
static uint8_t callback_arguments_valid;
static uint8_t events[EVENT_CAPACITY];
static uint32_t event_count;
static uint32_t observer_acquire_calls;
static uint32_t observer_release_calls;
static uint32_t observer_quarantine_calls;
static uint32_t open_calls;
static uint32_t probe_calls;
static uint32_t close_calls;
static uint32_t read_calls;
static uint32_t backend_prepare_calls;
static uint32_t backend_release_calls;
static uint32_t backend_run_calls;
static uint32_t machine_read_calls;
static uint32_t machine_write_calls;
static uint32_t fail_machine_read_call;
static uint32_t name_read_calls;
static uint32_t fail_name_read_call;
static uint32_t callback_sequence;
static uint32_t last_name_read_sequence;
static uint32_t last_open_sequence;
static uint64_t next_observer_generation;
static uint8_t publish_observer_generation_on_failure;
static struct dos_exec_transaction_table *reentry_table;
static const struct dos_exec_transaction_services *reentry_services;
static struct dos_exec_transaction_request reentry_request;
static struct dos_exec_transaction_handle reentry_handle;
static enum dos_exec_transaction_status reentry_status;
static uint8_t reentry_point;
static uint8_t reentry_active;
static struct dos_exec_transaction_environment nested_environment_result;
static struct dos_load_plan nested_load_plan;
static struct dos_exec_transaction_target nested_target_result;
static struct dos_exec_transaction_resident nested_resident_result;
static struct dos_exec_transaction_relocation nested_relocation_result;
static struct dos_exec_transaction_parent nested_parent_result;
static struct dos_exec_transaction_inheritance nested_inheritance_result;
static uint8_t guest_memory[TEST_GUEST_MEMORY_SIZE];
static uint8_t file_image[512];
static size_t file_image_readable_size;
static struct dos_exec_transaction_table *current_transactions;
static uint16_t released_mcb_segment;
static uint8_t verify_memory_released_before_close;
static uint32_t sft_lookup_calls;
static uint32_t sft_device_open_calls;
static uint32_t sft_acquire_calls;
static uint32_t sft_release_calls;
static uint32_t sft_device_close_calls;
static uint32_t drive_resolve_calls;
static uint8_t resolved_drives[2];
static uint8_t configured_invalid_drive;
static uint8_t configured_fault_drive;

static void set_environment_result_sentinel(
    struct dos_exec_transaction_environment *environment);
static bool environment_result_is_sentinel(
    const struct dos_exec_transaction_environment *environment);
static void set_load_plan_sentinel(struct dos_load_plan *plan);
static bool load_plan_is_sentinel(const struct dos_load_plan *plan);
static void set_target_result_sentinel(
    struct dos_exec_transaction_target *target);
static bool target_result_is_sentinel(
    const struct dos_exec_transaction_target *target);
static void set_resident_result_sentinel(
    struct dos_exec_transaction_resident *resident);
static bool resident_result_is_sentinel(
    const struct dos_exec_transaction_resident *resident);
static void set_relocation_result_sentinel(
    struct dos_exec_transaction_relocation *relocation);
static bool relocation_result_is_sentinel(
    const struct dos_exec_transaction_relocation *relocation);
static void set_parent_result_sentinel(
    struct dos_exec_transaction_parent *parent);
static bool parent_result_is_sentinel(
    const struct dos_exec_transaction_parent *parent);
static void set_inheritance_result_sentinel(
    struct dos_exec_transaction_inheritance *inheritance);
static bool inheritance_result_is_sentinel(
    const struct dos_exec_transaction_inheritance *inheritance);

static void attempt_reentry(uint8_t point)
{
	struct dos_exec_transaction_handle nested = {.value = HANDLE_SENTINEL};
	uint32_t detail = DETAIL_SENTINEL;
	uint8_t is_device = BYTE_SENTINEL;
	struct dos_exec_environment_source_plan environment_source = {
	    .source = {.offset = 0x1111u, .segment = 0x2222u},
	    .parent_psp = 0x3333u,
	    .subfunction = 0x44u,
	    .kind = 0x55u,
	};

	if (reentry_point != point || reentry_active != 0u ||
	    reentry_table == NULL || reentry_services == NULL)
		return;
	reentry_active = 1u;
	switch (point) {
	case REENTRY_OBSERVER_ACQUIRE:
	case REENTRY_OBSERVER_QUARANTINE:
		reentry_status = dos_exec_transaction_begin(
		    reentry_table, reentry_services, &reentry_request, &nested);
		break;
	case REENTRY_FILE_OPEN:
	case REENTRY_NAME_READ:
		reentry_status = dos_exec_transaction_open(
		    reentry_table, reentry_handle, reentry_services,
		    executable_name, ARRAY_SIZE(executable_name), &detail);
		break;
	case REENTRY_FILE_PROBE:
		reentry_status = dos_exec_transaction_probe(
		    reentry_table, reentry_handle, reentry_services, &is_device,
		    &detail);
		break;
	case REENTRY_FILE_CLOSE:
		reentry_status = dos_exec_transaction_close(
		    reentry_table, reentry_handle, reentry_services);
		break;
	case REENTRY_ABORT_FILE_CLOSE:
	case REENTRY_OBSERVER_RELEASE:
		reentry_status = dos_exec_transaction_abort(
		    reentry_table, reentry_handle, reentry_services);
		break;
	case REENTRY_ENVIRONMENT_READ:
		reentry_status = dos_exec_transaction_select_environment(
		    reentry_table, reentry_handle, reentry_services,
		    &environment_source);
		if (environment_source.source.offset != 0x1111u ||
		    environment_source.source.segment != 0x2222u ||
		    environment_source.parent_psp != 0x3333u ||
		    environment_source.subfunction != 0x44u ||
		    environment_source.kind != 0x55u)
			callback_arguments_valid = 0u;
		break;
	case REENTRY_ENVIRONMENT_PREPARE:
		reentry_status = dos_exec_transaction_prepare_environment(
		    reentry_table, reentry_handle, reentry_services,
		    &nested_environment_result);
		if (!environment_result_is_sentinel(
			&nested_environment_result))
			callback_arguments_valid = 0u;
		break;
	case REENTRY_IMAGE_READ:
		reentry_status = dos_exec_transaction_inspect_image(
		    reentry_table, reentry_handle, reentry_services,
		    &nested_load_plan);
		if (!load_plan_is_sentinel(&nested_load_plan))
			callback_arguments_valid = 0u;
		break;
	case REENTRY_TARGET_PREPARE:
		reentry_status = dos_exec_transaction_prepare_target(
		    reentry_table, reentry_handle, reentry_services,
		    &nested_target_result);
		if (!target_result_is_sentinel(&nested_target_result))
			callback_arguments_valid = 0u;
		break;
	case REENTRY_RESIDENT_LOAD:
		reentry_status = dos_exec_transaction_load_resident(
		    reentry_table, reentry_handle, reentry_services,
		    &nested_resident_result);
		if (!resident_result_is_sentinel(&nested_resident_result))
			callback_arguments_valid = 0u;
		break;
	case REENTRY_RELOCATE:
		reentry_status = dos_exec_transaction_relocate_resident(
		    reentry_table, reentry_handle, reentry_services,
		    &nested_relocation_result);
		if (!relocation_result_is_sentinel(&nested_relocation_result))
			callback_arguments_valid = 0u;
		break;
	case REENTRY_PARENT_CAPTURE:
		reentry_status = dos_exec_transaction_capture_parent(
		    reentry_table, reentry_handle, reentry_services,
		    &nested_parent_result);
		if (!parent_result_is_sentinel(&nested_parent_result))
			callback_arguments_valid = 0u;
		break;
	case REENTRY_INHERITANCE:
		reentry_status = dos_exec_transaction_prepare_inheritance(
		    reentry_table, reentry_handle, reentry_services,
		    &nested_inheritance_result);
		if (!inheritance_result_is_sentinel(&nested_inheritance_result))
			callback_arguments_valid = 0u;
		break;
	case REENTRY_INITIAL_STATE:
		reentry_status = dos_exec_transaction_finalize_initial_state(
		    reentry_table, reentry_handle, reentry_services,
		    &nested_resident_result);
		if (!resident_result_is_sentinel(&nested_resident_result))
			callback_arguments_valid = 0u;
		break;
	default:
		break;
	}
	reentry_active = 0u;
}

static void record_event(uint8_t event)
{
	if (event_count < EVENT_CAPACITY)
		events[event_count] = event;
	++event_count;
}

static void reset_adapters(void)
{
	size_t index;

	configured_open_status = DOS_EXEC_FILE_ADAPTER_OK;
	configured_probe_status = DOS_EXEC_FILE_ADAPTER_OK;
	configured_close_result = DOS_EXEC_FILE_CLOSE_CLOSED;
	configured_observer_acquire = DOS_EXEC_OBSERVER_ADAPTER_OK;
	configured_observer_release = DOS_EXEC_OBSERVER_ADAPTER_OK;
	configured_observer_quarantine = DOS_EXEC_OBSERVER_ADAPTER_OK;
	configured_read_status = DOS_IMAGE_READ_IO_ERROR;
	configured_backend_prepare = DOS_EXEC_BACKEND_PREPARED;
	configured_backend_release = DOS_EXEC_BACKEND_RELEASED;
	configured_open_detail = 0u;
	configured_probe_detail = 0u;
	configured_open_size = 0x12345u;
	configured_is_device = 0u;
	configured_short_read = 0u;
	expected_name_pointer = executable_name;
	expected_name_length = ARRAY_SIZE(canonical_executable_name);
	callback_arguments_valid = 1u;
	event_count = 0u;
	observer_acquire_calls = 0u;
	observer_release_calls = 0u;
	observer_quarantine_calls = 0u;
	open_calls = 0u;
	probe_calls = 0u;
	close_calls = 0u;
	read_calls = 0u;
	backend_prepare_calls = 0u;
	backend_release_calls = 0u;
	backend_run_calls = 0u;
	machine_read_calls = 0u;
	machine_write_calls = 0u;
	fail_machine_read_call = 0u;
	name_read_calls = 0u;
	fail_name_read_call = 0u;
	callback_sequence = 0u;
	last_name_read_sequence = 0u;
	last_open_sequence = 0u;
	next_observer_generation = 1u;
	publish_observer_generation_on_failure = 0u;
	reentry_table = NULL;
	reentry_services = NULL;
	reentry_request = (struct dos_exec_transaction_request){0};
	reentry_handle.value = 0u;
	reentry_status = DOS_EXEC_TRANSACTION_OK;
	reentry_point = REENTRY_NONE;
	reentry_active = 0u;
	set_environment_result_sentinel(&nested_environment_result);
	set_load_plan_sentinel(&nested_load_plan);
	set_target_result_sentinel(&nested_target_result);
	set_resident_result_sentinel(&nested_resident_result);
	set_relocation_result_sentinel(&nested_relocation_result);
	set_parent_result_sentinel(&nested_parent_result);
	set_inheritance_result_sentinel(&nested_inheritance_result);
	current_transactions = NULL;
	file_image_readable_size = 0u;
	released_mcb_segment = 0u;
	verify_memory_released_before_close = 0u;
	sft_lookup_calls = 0u;
	sft_device_open_calls = 0u;
	sft_acquire_calls = 0u;
	sft_release_calls = 0u;
	sft_device_close_calls = 0u;
	drive_resolve_calls = 0u;
	resolved_drives[0] = 0u;
	resolved_drives[1] = 0u;
	configured_invalid_drive = 0xffu;
	configured_fault_drive = 0xfeu;
	for (index = 0u; index < ARRAY_SIZE(events); ++index)
		events[index] = 0u;
	for (index = 0u; index < ARRAY_SIZE(file_image); ++index)
		file_image[index] = 0u;
	for (index = 0u; index < ARRAY_SIZE(canonical_executable_name); ++index)
		guest_memory[TEST_EXECUTABLE_NAME_LINEAR + index] =
		    canonical_executable_name[index];
}

static enum dos_exec_file_adapter_status
test_open(kernel_object_handle_t context, const uint8_t *path,
	  size_t path_length, struct dos_exec_file_open_result *result)
{
	size_t index;

	record_event(EVENT_FILE_OPEN);
	++open_calls;
	++callback_sequence;
	if (last_name_read_sequence <= last_open_sequence ||
	    last_name_read_sequence >= callback_sequence)
		callback_arguments_valid = 0u;
	last_open_sequence = callback_sequence;
	attempt_reentry(REENTRY_FILE_OPEN);
	if (context != TEST_FILE_CONTEXT || path == NULL ||
	    (expected_name_pointer != NULL && path != expected_name_pointer) ||
	    path_length != expected_name_length || result == NULL)
		callback_arguments_valid = 0u;
	if (expected_name_pointer == NULL && path != NULL &&
	    path_length == ARRAY_SIZE(canonical_executable_name)) {
		for (index = 0u; index < path_length; ++index) {
			if (path[index] != canonical_executable_name[index]) {
				callback_arguments_valid = 0u;
				break;
			}
		}
	}
	if (result != NULL)
		result->failure_detail = configured_open_detail;
	if (configured_open_status != DOS_EXEC_FILE_ADAPTER_OK)
		return configured_open_status;
	result->reader_context = TEST_READER_CONTEXT;
	result->size = configured_open_size;
	result->reserved = 0u;
	return DOS_EXEC_FILE_ADAPTER_OK;
}

static enum dos_exec_file_adapter_status
test_probe_device(kernel_object_handle_t context,
		  kernel_object_handle_t reader_context,
		  struct dos_exec_file_probe_result *result)
{
	record_event(EVENT_FILE_PROBE);
	++probe_calls;
	attempt_reentry(REENTRY_FILE_PROBE);
	if (context != TEST_FILE_CONTEXT ||
	    reader_context != TEST_READER_CONTEXT || result == NULL)
		callback_arguments_valid = 0u;
	if (result != NULL) {
		result->failure_detail = configured_probe_detail;
		result->is_device = configured_is_device;
		result->reserved[0] = 0u;
		result->reserved[1] = 0u;
		result->reserved[2] = 0u;
	}
	return configured_probe_status;
}

static enum dos_image_read_status
test_read(kernel_object_handle_t reader_context, file_offset_t offset,
	  void *destination, size_t destination_capacity, size_t count,
	  size_t *bytes_read)
{
	uint8_t *output = (uint8_t *)destination;
	size_t actual = count;
	size_t index;

	record_event(EVENT_FILE_READ);
	++read_calls;
	attempt_reentry(REENTRY_IMAGE_READ);
	attempt_reentry(REENTRY_RESIDENT_LOAD);
	attempt_reentry(REENTRY_RELOCATE);
	if (reader_context != TEST_READER_CONTEXT || destination == NULL ||
	    bytes_read == NULL || count > destination_capacity ||
	    offset > configured_open_size ||
	    (file_offset_t)count > configured_open_size - offset) {
		callback_arguments_valid = 0u;
		return DOS_IMAGE_READ_IO_ERROR;
	}
	*bytes_read = 0u;
	if (configured_read_status != DOS_IMAGE_READ_OK)
		return configured_read_status;
	if (offset > file_image_readable_size ||
	    count > file_image_readable_size - (size_t)offset)
		return DOS_IMAGE_READ_IO_ERROR;
	if (configured_short_read != 0u && actual != 0u)
		--actual;
	for (index = 0u; index < actual; ++index)
		output[index] = file_image[(size_t)offset + index];
	*bytes_read = actual;
	return DOS_IMAGE_READ_OK;
}

static enum dos_exec_file_close_result
test_close(kernel_object_handle_t context,
	   kernel_object_handle_t reader_context)
{
	dos_linear_address_t released_mcb_linear;

	record_event(EVENT_FILE_CLOSE);
	++close_calls;
	attempt_reentry(REENTRY_FILE_CLOSE);
	attempt_reentry(REENTRY_ABORT_FILE_CLOSE);
	if (context != TEST_FILE_CONTEXT ||
	    reader_context != TEST_READER_CONTEXT)
		callback_arguments_valid = 0u;
	if (verify_memory_released_before_close != 0u) {
		released_mcb_linear = dos_far_to_linear(released_mcb_segment, 0u,
						 false);
		if (guest_memory[(size_t)released_mcb_linear + 1u] != 0u ||
		    guest_memory[(size_t)released_mcb_linear + 2u] != 0u)
			callback_arguments_valid = 0u;
	}
	return configured_close_result;
}

static enum dos_exec_observer_adapter_status
test_observer_acquire(kernel_object_handle_t context, uint64_t *generation)
{
	record_event(EVENT_OBSERVER_ACQUIRE);
	++observer_acquire_calls;
	attempt_reentry(REENTRY_OBSERVER_ACQUIRE);
	if (context != TEST_OBSERVER_CONTEXT || generation == NULL)
		callback_arguments_valid = 0u;
	if (configured_observer_acquire != DOS_EXEC_OBSERVER_ADAPTER_OK) {
		if (publish_observer_generation_on_failure != 0u)
			*generation = next_observer_generation++;
		return configured_observer_acquire;
	}
	*generation = next_observer_generation++;
	return DOS_EXEC_OBSERVER_ADAPTER_OK;
}

static enum dos_exec_observer_adapter_status
test_observer_release(kernel_object_handle_t context, uint64_t generation)
{
	record_event(EVENT_OBSERVER_RELEASE);
	++observer_release_calls;
	attempt_reentry(REENTRY_OBSERVER_RELEASE);
	if (context != TEST_OBSERVER_CONTEXT || generation == 0u)
		callback_arguments_valid = 0u;
	return configured_observer_release;
}

static enum dos_exec_observer_adapter_status
test_observer_quarantine(kernel_object_handle_t context, uint64_t generation)
{
	record_event(EVENT_OBSERVER_QUARANTINE);
	++observer_quarantine_calls;
	attempt_reentry(REENTRY_OBSERVER_QUARANTINE);
	if (context != TEST_OBSERVER_CONTEXT)
		callback_arguments_valid = 0u;
	(void)generation;
	return configured_observer_quarantine;
}

static enum dos_machine_status
test_machine_read(kernel_object_handle_t context,
		  dos_linear_address_t linear_address, void *destination,
		  size_t destination_capacity, size_t count)
{
	uint8_t *bytes = (uint8_t *)destination;
	bool reading_name = false;
	size_t index;
	uint32_t slot_index;

	if (current_transactions != NULL) {
		for (slot_index = 0u;
		     slot_index < DOS_EXEC_TRANSACTION_SLOT_COUNT;
		     ++slot_index) {
			if (current_transactions->slots[slot_index].state ==
			    (uint8_t)DOS_EXEC_TRANSACTION_STATE_NAME_READING) {
				reading_name = true;
				break;
			}
		}
	}
	if (reading_name) {
		++name_read_calls;
		last_name_read_sequence = ++callback_sequence;
		attempt_reentry(REENTRY_NAME_READ);
	} else {
		record_event(EVENT_MACHINE_READ);
		++machine_read_calls;
		attempt_reentry(REENTRY_ENVIRONMENT_READ);
		attempt_reentry(REENTRY_ENVIRONMENT_PREPARE);
		attempt_reentry(REENTRY_TARGET_PREPARE);
		attempt_reentry(REENTRY_RESIDENT_LOAD);
		attempt_reentry(REENTRY_RELOCATE);
		attempt_reentry(REENTRY_PARENT_CAPTURE);
	}
	if (context != TEST_MACHINE_CONTEXT || destination == NULL ||
	    count > destination_capacity ||
	    (uint64_t)linear_address > TEST_GUEST_MEMORY_SIZE ||
	    (uint64_t)count >
		TEST_GUEST_MEMORY_SIZE - (uint64_t)linear_address) {
		callback_arguments_valid = 0u;
		return DOS_MACHINE_ADDRESS_FAULT;
	}
	if ((reading_name && fail_name_read_call != 0u &&
	     name_read_calls == fail_name_read_call) ||
	    (!reading_name && fail_machine_read_call != 0u &&
	     machine_read_calls == fail_machine_read_call))
		return DOS_MACHINE_IO_FAULT;
	for (index = 0u; index < count; ++index)
		bytes[index] = guest_memory[(size_t)linear_address + index];
	return DOS_MACHINE_OK;
}

static enum dos_machine_status
test_machine_write(kernel_object_handle_t context,
		   dos_linear_address_t linear_address, const void *source,
		   size_t source_capacity, size_t count)
{
	const uint8_t *bytes = (const uint8_t *)source;
	size_t index;

	record_event(EVENT_MACHINE_WRITE);
	++machine_write_calls;
	attempt_reentry(REENTRY_ENVIRONMENT_PREPARE);
	attempt_reentry(REENTRY_TARGET_PREPARE);
	attempt_reentry(REENTRY_RESIDENT_LOAD);
	attempt_reentry(REENTRY_RELOCATE);

	if (context != TEST_MACHINE_CONTEXT || source == NULL ||
	    count > source_capacity ||
	    (uint64_t)linear_address > TEST_GUEST_MEMORY_SIZE ||
	    (uint64_t)count >
		TEST_GUEST_MEMORY_SIZE - (uint64_t)linear_address) {
		callback_arguments_valid = 0u;
		return DOS_MACHINE_ADDRESS_FAULT;
	}
	for (index = 0u; index < count; ++index)
		guest_memory[(size_t)linear_address + index] = bytes[index];
	return DOS_MACHINE_OK;
}

static enum dos_sft_adapter_status
test_sft_lookup(kernel_object_handle_t context, uint8_t sfn,
		struct dos_sft_view *view)
{
	++sft_lookup_calls;
	attempt_reentry(REENTRY_INHERITANCE);
	if (context != TEST_SFT_CONTEXT || view == NULL)
		return DOS_SFT_ADAPTER_FAULT;
	if (sfn != 1u && sfn != 2u)
		return DOS_SFT_ADAPTER_INVALID_SFT;
	view->reference_handle = 0x1000u + sfn;
	view->flags = sfn == 2u ? DOS_SFT_FLAG_IS_NETWORK : 0u;
	view->mode = 0u;
	return DOS_SFT_ADAPTER_OK;
}

static enum dos_sft_adapter_status
test_sft_device_open(kernel_object_handle_t context,
		     dos_sft_reference_handle_t reference)
{
	++sft_device_open_calls;
	attempt_reentry(REENTRY_INHERITANCE);
	return context == TEST_SFT_CONTEXT && reference == 0x1001u
		   ? DOS_SFT_ADAPTER_OK
		   : DOS_SFT_ADAPTER_FAULT;
}

static enum dos_sft_adapter_status
test_sft_acquire(kernel_object_handle_t context,
		 dos_sft_reference_handle_t reference)
{
	++sft_acquire_calls;
	attempt_reentry(REENTRY_INHERITANCE);
	return context == TEST_SFT_CONTEXT &&
		       (reference == 0x1001u || reference == 0x1002u)
		   ? DOS_SFT_ADAPTER_OK
		   : DOS_SFT_ADAPTER_FAULT;
}

static enum dos_sft_adapter_status
test_sft_release(kernel_object_handle_t context,
		 dos_sft_reference_handle_t reference)
{
	++sft_release_calls;
	return context == TEST_SFT_CONTEXT &&
		       (reference == 0x1001u || reference == 0x1002u)
		   ? DOS_SFT_ADAPTER_OK
		   : DOS_SFT_ADAPTER_FAULT;
}

static enum dos_sft_adapter_status
test_sft_device_close(kernel_object_handle_t context,
		      dos_sft_reference_handle_t reference)
{
	++sft_device_close_calls;
	return context == TEST_SFT_CONTEXT && reference == 0x1001u
		   ? DOS_SFT_ADAPTER_OK
		   : DOS_SFT_ADAPTER_FAULT;
}

static const struct dos_sft_batch_ops transaction_sft_ops = {
    .identity = TEST_SFT_ID,
    .lookup = test_sft_lookup,
    .device_open = test_sft_device_open,
    .reference_acquire = test_sft_acquire,
    .reference_release = test_sft_release,
    .device_close = test_sft_device_close,
};

static enum dos_exec_drive_visibility_status
test_drive_resolve(kernel_object_handle_t context, uint8_t drive_designator)
{
	if (drive_resolve_calls < ARRAY_SIZE(resolved_drives))
		resolved_drives[drive_resolve_calls] = drive_designator;
	++drive_resolve_calls;
	attempt_reentry(REENTRY_INITIAL_STATE);
	if (context != TEST_DRIVE_CONTEXT) {
		callback_arguments_valid = 0u;
		return DOS_EXEC_DRIVE_FAULT;
	}
	if (drive_designator == configured_fault_drive)
		return DOS_EXEC_DRIVE_FAULT;
	return drive_designator == configured_invalid_drive
		   ? DOS_EXEC_DRIVE_INVALID
		   : DOS_EXEC_DRIVE_VISIBLE;
}

static const struct dos_exec_drive_visibility_ops transaction_drive_ops = {
    .identity = TEST_DRIVE_ID,
    .resolve = test_drive_resolve,
};

static enum dos_exec_backend_prepare_status test_backend_prepare(
    kernel_object_handle_t context, const struct dos_machine *machine,
    kernel_object_handle_t machine_identity,
    const struct dos_exec_handoff_plan *handoff,
    struct dos_exec_backend_prepare_result *result)
{
	++backend_prepare_calls;
	if (context != TEST_BACKEND_CONTEXT ||
	    machine_identity != TEST_MACHINE_ID || machine == NULL ||
	    machine->context != TEST_MACHINE_CONTEXT ||
	    !dos_exec_handoff_plan_has_valid_encoding(handoff) || result == NULL)
		callback_arguments_valid = 0u;
	*result = (struct dos_exec_backend_prepare_result){
	    .backend_context = configured_backend_prepare ==
				       DOS_EXEC_BACKEND_REJECTED
				   ? KERNEL_OBJECT_HANDLE_INVALID
				   : TEST_BACKEND_SESSION,
	    .failure_detail = configured_backend_prepare ==
				      DOS_EXEC_BACKEND_REJECTED
				  ? 0x12345678u
				  : 0u,
	    .reserved = {0u},
	};
	return configured_backend_prepare;
}

static enum dos_exec_backend_release_status test_backend_release(
    kernel_object_handle_t context, kernel_object_handle_t backend_context)
{
	++backend_release_calls;
	if (context != TEST_BACKEND_CONTEXT ||
	    backend_context != TEST_BACKEND_SESSION)
		callback_arguments_valid = 0u;
	return configured_backend_release;
}

static enum dos_exec_backend_run_status test_backend_run(
    kernel_object_handle_t context, kernel_object_handle_t backend_context,
    kernel_object_handle_t machine_identity,
    const struct dos_machine *machine, struct dos_cpu_state *state,
    struct dos_execution_event *event)
{
	++backend_run_calls;
	(void)context;
	(void)backend_context;
	(void)machine_identity;
	(void)machine;
	(void)state;
	(void)event;
	return DOS_EXEC_BACKEND_RUN_UNCERTAIN;
}

static const struct dos_exec_backend_ops backend_ops = {
	.identity = TEST_BACKEND_ID,
	.capabilities = DOS_EXEC_CAP_VM86,
	.prepare = test_backend_prepare,
	.release = test_backend_release,
	.run_until_event = test_backend_run,
};

static const struct dos_exec_file_lease_ops file_ops = {
    .identity = TEST_FILE_ID,
    .open = test_open,
    .probe_device = test_probe_device,
    .read = test_read,
    .close = test_close,
};

static const struct dos_exec_observer_ops observer_ops = {
    .identity = TEST_OBSERVER_ID,
    .acquire = test_observer_acquire,
    .release = test_observer_release,
    .quarantine = test_observer_quarantine,
};

static const struct dos_machine_ops machine_ops = {
    .read_memory = test_machine_read,
    .write_memory = test_machine_write,
    .read_port = NULL,
    .write_port = NULL,
    .set_a20 = NULL,
};

static bool initialize_fixture(struct test_fixture *fixture)
{
	struct dos_far_pointer16 dta = {
	    .offset = 0x0080u,
	    .segment = 0x1234u,
	};

	if (fixture == NULL)
		return false;
	current_transactions = &fixture->transactions;
	fixture->machine = (struct dos_machine){
	    .ops = &machine_ops,
	    .context = TEST_MACHINE_CONTEXT,
	    .address_limit = DOS_REAL_MODE_ADDRESS_LIMIT,
	    .a20_enabled = false,
	};
	if (dos_exec_transaction_table_construct(&fixture->transactions) !=
		DOS_EXEC_TRANSACTION_OK ||
	    dos_exec_transaction_table_initialize(&fixture->transactions,
						  TEST_COORDINATOR_ID) !=
		DOS_EXEC_TRANSACTION_OK ||
	    dos_exec_file_lease_table_construct(&fixture->files) !=
		DOS_EXEC_FILE_LEASE_OK ||
	    dos_exec_file_lease_table_initialize(&fixture->files,
						 TEST_FILE_TABLE_ID) !=
		DOS_EXEC_FILE_LEASE_OK ||
	    dos_memory_arena_construct(&fixture->memory_arena,
				       TEST_MEMORY_ARENA_ID) != DOS_MEMORY_OK ||
	    dos_memory_arena_initialize_checked(
		&fixture->memory_arena, &fixture->machine, TEST_MEMORY_HEAD,
		TEST_MEMORY_END) != DOS_MEMORY_OK ||
	    dos_memory_lease_table_construct(&fixture->memory_leases,
					     TEST_MEMORY_TABLE_ID) !=
		DOS_MEMORY_LEASE_OK ||
	    dos_memory_lease_table_initialize(&fixture->memory_leases) !=
		DOS_MEMORY_LEASE_OK ||
	    dos_exec_backend_session_table_construct(
		&fixture->backend_sessions) != DOS_EXEC_BACKEND_SESSION_OK ||
	    dos_exec_backend_session_table_initialize(
		&fixture->backend_sessions, TEST_BACKEND_TABLE_ID) !=
		DOS_EXEC_BACKEND_SESSION_OK ||
	    dos_process_runtime_construct(&fixture->runtime) !=
		DOS_PROCESS_RUNTIME_OK ||
	    dos_process_runtime_initialize(&fixture->runtime, TEST_RUNTIME_ID,
					   0x1234u,
					   dta) != DOS_PROCESS_RUNTIME_OK)
		return false;
	/* Arena construction performs one rollback-protected setup read.  It is
	 * fixture setup, not an EXEC event observed by the assertions below. */
	event_count = 0u;
	machine_read_calls = 0u;
	machine_write_calls = 0u;
	for (size_t index = 0u; index < ARRAY_SIZE(events); ++index)
		events[index] = 0u;
	fixture->services = (struct dos_exec_transaction_services){
	    .file_leases = &fixture->files,
	    .file_ops = &file_ops,
	    .observer_ops = &observer_ops,
	    .sft_ops = &transaction_sft_ops,
	    .drive_ops = &transaction_drive_ops,
	    .runtime = &fixture->runtime,
	    .machine = &fixture->machine,
	    .memory_leases = &fixture->memory_leases,
	    .memory_arena = &fixture->memory_arena,
	    .backend_sessions = &fixture->backend_sessions,
	    .backend_ops = &backend_ops,
	    .coordinator_identity = TEST_COORDINATOR_ID,
	    .machine_identity = TEST_MACHINE_ID,
	    .file_lease_table_identity = TEST_FILE_TABLE_ID,
	    .file_adapter_context = TEST_FILE_CONTEXT,
	    .sft_adapter_identity = TEST_SFT_ID,
	    .sft_adapter_context = TEST_SFT_CONTEXT,
	    .drive_adapter_identity = TEST_DRIVE_ID,
	    .drive_adapter_context = TEST_DRIVE_CONTEXT,
	    .observer_adapter_context = TEST_OBSERVER_CONTEXT,
	    .backend_session_table_identity = TEST_BACKEND_TABLE_ID,
	    .backend_adapter_context = TEST_BACKEND_CONTEXT,
	    .memory_lease_table_identity = TEST_MEMORY_TABLE_ID,
	};
	return true;
}

static void set_parent_result_sentinel(
    struct dos_exec_transaction_parent *parent)
{
	parent->snapshot.machine_identity = HANDLE_SENTINEL;
	parent->snapshot.machine_context = 0xa1a1a1a1u;
	parent->snapshot.machine_address_limit = 0xa2a2a2a2u;
	parent->snapshot.parent_psp_segment = 0xa3a3u;
	parent->snapshot.a20_enabled = 0xa4u;
	parent->snapshot.captured = 0xa5u;
	parent->snapshot.parent_psp[0] = 0xa6u;
	parent->snapshot.parent_jft.entries[0] = 0xa7u;
	parent->has_snapshot = 0xa8u;
	parent->reserved[0] = 0xa9u;
}

static bool parent_result_is_sentinel(
    const struct dos_exec_transaction_parent *parent)
{
	return parent->snapshot.machine_identity == HANDLE_SENTINEL &&
	       parent->snapshot.machine_context == 0xa1a1a1a1u &&
	       parent->snapshot.machine_address_limit == 0xa2a2a2a2u &&
	       parent->snapshot.parent_psp_segment == 0xa3a3u &&
	       parent->snapshot.a20_enabled == 0xa4u &&
	       parent->snapshot.captured == 0xa5u &&
	       parent->snapshot.parent_psp[0] == 0xa6u &&
	       parent->snapshot.parent_jft.entries[0] == 0xa7u &&
	       parent->has_snapshot == 0xa8u && parent->reserved[0] == 0xa9u;
}

static void set_inheritance_result_sentinel(
    struct dos_exec_transaction_inheritance *inheritance)
{
	inheritance->batch = HANDLE_SENTINEL;
	inheritance->child_jft.entries[0] = 0xa1u;
	inheritance->has_batch = 0xa2u;
	inheritance->has_child_jft = 0xa3u;
	inheritance->reserved[0] = 0xa4u;
}

static bool inheritance_result_is_sentinel(
    const struct dos_exec_transaction_inheritance *inheritance)
{
	return inheritance->batch == HANDLE_SENTINEL &&
	       inheritance->child_jft.entries[0] == 0xa1u &&
	       inheritance->has_batch == 0xa2u &&
	       inheritance->has_child_jft == 0xa3u &&
	       inheritance->reserved[0] == 0xa4u;
}

static void set_relocation_result_sentinel(
    struct dos_exec_transaction_relocation *relocation)
{
	size_t index;

	relocation->request.relocation_table_offset = 0xa1a1a1a1u;
	relocation->request.resident_size = 0xa2a2a2a2u;
	relocation->request.resident_linear_address = 0xa3a3a3a3u;
	relocation->request.relocation_count = 0xa4a4u;
	relocation->request.relocation_factor = 0xa5a5u;
	relocation->result.validated_entries = 0xb1b1u;
	relocation->result.applied_entries = 0xb2b2u;
	relocation->applicable = 0xc1u;
	relocation->has_request = 0xc2u;
	relocation->applied = 0xc3u;
	for (index = 0u; index < ARRAY_SIZE(relocation->reserved); ++index)
		relocation->reserved[index] = (uint8_t)(index + 1u);
}

static bool relocation_result_is_sentinel(
    const struct dos_exec_transaction_relocation *relocation)
{
	size_t index;

	if (relocation->request.relocation_table_offset != 0xa1a1a1a1u ||
	    relocation->request.resident_size != 0xa2a2a2a2u ||
	    relocation->request.resident_linear_address != 0xa3a3a3a3u ||
	    relocation->request.relocation_count != 0xa4a4u ||
	    relocation->request.relocation_factor != 0xa5a5u ||
	    relocation->result.validated_entries != 0xb1b1u ||
	    relocation->result.applied_entries != 0xb2b2u ||
	    relocation->applicable != 0xc1u ||
	    relocation->has_request != 0xc2u ||
	    relocation->applied != 0xc3u)
		return false;
	for (index = 0u; index < ARRAY_SIZE(relocation->reserved); ++index) {
		if (relocation->reserved[index] != (uint8_t)(index + 1u))
			return false;
	}
	return true;
}

static struct dos_exec_transaction_request make_request(uint8_t subfunction)
{
	struct dos_exec_transaction_request request = {
	    .executable_name = {.offset = 0x0100u, .segment = 0x2000u},
	    .parameter_block = {.offset = 0x0200u, .segment = 0x3000u},
	    .subfunction = subfunction,
	    .reserved = {0u},
	};

	return request;
}

static uint32_t transaction_slot(struct dos_exec_transaction_handle handle)
{
	return (uint32_t)(handle.value & DOS_EXEC_TRANSACTION_SLOT_MASK) - 1u;
}

static void
configure_reentry(struct test_fixture *fixture,
		  const struct dos_exec_transaction_request *request,
		  struct dos_exec_transaction_handle handle, uint8_t point);

static void put_guest_word(uint16_t segment, uint16_t offset, uint16_t value)
{
	uint32_t linear = dos_far_to_linear(segment, offset, false);

	guest_memory[linear] = (uint8_t)value;
	linear = dos_far_to_linear(segment, (uint16_t)(offset + 1u), false);
	guest_memory[linear] = (uint8_t)(value >> 8);
}

static void put_guest_far(uint16_t segment, uint16_t offset,
		  uint16_t value_segment, uint16_t value_offset)
{
	put_guest_word(segment, offset, value_offset);
	put_guest_word(segment, (uint16_t)(offset + 2u), value_segment);
}

static void put_guest_name(struct dos_far_pointer16 source,
			   const uint8_t *name, size_t length,
			   bool a20_enabled)
{
	size_t index;

	for (index = 0u; index < length; ++index) {
		uint16_t offset =
		    (uint16_t)((uint32_t)source.offset + (uint32_t)index);
		dos_linear_address_t linear =
		    dos_far_to_linear(source.segment, offset, a20_enabled);

		guest_memory[(size_t)linear] = name[index];
	}
}

static uint16_t get_guest_word(uint16_t segment, uint16_t offset)
{
	dos_linear_address_t linear =
	    dos_far_to_linear(segment, offset, false);

	return (uint16_t)guest_memory[(size_t)linear] |
	       ((uint16_t)guest_memory[(size_t)linear + 1u] << 8);
}

static uint16_t get_psp_word(const struct dos_exec_transaction_psp *psp,
			     size_t offset)
{
	return (uint16_t)psp->image.bytes[offset] |
	       ((uint16_t)psp->image.bytes[offset + 1u] << 8);
}

static void fill_guest_bytes(struct dos_far_pointer16 destination,
			     uint8_t value, size_t count)
{
	size_t index;

	for (index = 0u; index < count; ++index) {
		uint16_t offset = (uint16_t)((uint32_t)destination.offset +
					     (uint32_t)index);
		dos_linear_address_t linear =
		    dos_far_to_linear(destination.segment, offset, false);

		guest_memory[(size_t)linear] = value;
	}
}

static bool guest_bytes_equal(struct dos_far_pointer16 source,
			      const uint8_t *expected, size_t count)
{
	size_t index;

	for (index = 0u; index < count; ++index) {
		uint16_t offset =
		    (uint16_t)((uint32_t)source.offset + (uint32_t)index);
		dos_linear_address_t linear =
		    dos_far_to_linear(source.segment, offset, false);

		if (guest_memory[(size_t)linear] != expected[index])
			return false;
	}
	return true;
}

static bool guest_bytes_have_value(struct dos_far_pointer16 source,
				   uint8_t value, size_t count)
{
	size_t index;

	for (index = 0u; index < count; ++index) {
		uint16_t offset =
		    (uint16_t)((uint32_t)source.offset + (uint32_t)index);
		dos_linear_address_t linear =
		    dos_far_to_linear(source.segment, offset, false);

		if (guest_memory[(size_t)linear] != value)
			return false;
	}
	return true;
}

static void set_environment_result_sentinel(
    struct dos_exec_transaction_environment *environment)
{
	*environment = (struct dos_exec_transaction_environment){
	    .plan =
		{
		    .source = {.offset = 0x1111u, .segment = 0x2222u},
		    .environment_bytes = 0x33333333u,
		    .executable_name =
			{
			    .source =
				{.offset = 0x4444u, .segment = 0x5555u},
			    .bytes_including_nul = 0x6666u,
			    .reserved = 0x7777u,
			},
		    .payload_bytes = 0x88888888u,
		    .allocation_bytes = 0x99999999u,
		    .paragraphs = 0xaaaau,
		    .reserved = 0xbbbbu,
		},
	    .lease =
		{
		    .handle = {.value = HANDLE_SENTINEL},
		    .guest_segment = 0xccccu,
		    .paragraphs = 0xddddu,
		    .maximum_available = 0xeeeeu,
		    .reserved = 0xffffu,
		},
	    .has_block = 0x5au,
	    .reserved = {1u, 2u, 3u, 4u, 5u, 6u, 7u},
	};
}

static bool environment_result_is_sentinel(
    const struct dos_exec_transaction_environment *environment)
{
	return environment->plan.source.offset == 0x1111u &&
	       environment->plan.source.segment == 0x2222u &&
	       environment->plan.environment_bytes == 0x33333333u &&
	       environment->plan.executable_name.source.offset == 0x4444u &&
	       environment->plan.executable_name.source.segment == 0x5555u &&
	       environment->plan.executable_name.bytes_including_nul == 0x6666u &&
	       environment->plan.executable_name.reserved == 0x7777u &&
	       environment->plan.payload_bytes == 0x88888888u &&
	       environment->plan.allocation_bytes == 0x99999999u &&
	       environment->plan.paragraphs == 0xaaaau &&
	       environment->plan.reserved == 0xbbbbu &&
	       environment->lease.handle.value == HANDLE_SENTINEL &&
	       environment->lease.guest_segment == 0xccccu &&
	       environment->lease.paragraphs == 0xddddu &&
	       environment->lease.maximum_available == 0xeeeeu &&
	       environment->lease.reserved == 0xffffu &&
	       environment->has_block == 0x5au &&
	       environment->reserved[0] == 1u &&
	       environment->reserved[1] == 2u &&
	       environment->reserved[2] == 3u &&
	       environment->reserved[3] == 4u &&
	       environment->reserved[4] == 5u &&
	       environment->reserved[5] == 6u &&
	       environment->reserved[6] == 7u;
}

static bool environment_result_is_zero(
    const struct dos_exec_transaction_environment *environment)
{
	size_t index;

	if (environment->plan.source.offset != 0u ||
	    environment->plan.source.segment != 0u ||
	    environment->plan.environment_bytes != 0u ||
	    environment->plan.executable_name.source.offset != 0u ||
	    environment->plan.executable_name.source.segment != 0u ||
	    environment->plan.executable_name.bytes_including_nul != 0u ||
	    environment->plan.executable_name.reserved != 0u ||
	    environment->plan.payload_bytes != 0u ||
	    environment->plan.allocation_bytes != 0u ||
	    environment->plan.paragraphs != 0u ||
	    environment->plan.reserved != 0u ||
	    environment->lease.handle.value != 0u ||
	    environment->lease.guest_segment != 0u ||
	    environment->lease.paragraphs != 0u ||
	    environment->lease.maximum_available != 0u ||
	    environment->lease.reserved != 0u || environment->has_block != 0u)
		return false;
	for (index = 0u; index < ARRAY_SIZE(environment->reserved); ++index) {
		if (environment->reserved[index] != 0u)
			return false;
	}
	return true;
}

static void set_load_plan_sentinel(struct dos_load_plan *plan)
{
	*plan = (struct dos_load_plan){
	    .format = 0xa1u,
	    .old_mz_signature = 0xa2u,
	    .load_high = 0xa3u,
	    .target_kind = 0xa4u,
	    .reserved32 = 0xa5a5a5a5u,
	    .file_size = 0x1111222233334444ull,
	    .image_file_offset = 0x5555666677778888ull,
	    .image_size = 0x9999aaaabbbbccccull,
	    .minimum_image_paragraphs = 0xddddeeeeffff0001ull,
	    .minimum_extra_paragraphs = 0x1234u,
	    .maximum_extra_paragraphs = 0x2345u,
	    .initial_cs = 0x3456u,
	    .initial_ip = 0x4567u,
	    .initial_ss = 0x5678u,
	    .initial_sp = 0x6789u,
	    .relocation_count = 0x789au,
	    .relocation_table_offset = 0x89abu,
	};
}

static bool load_plan_is_sentinel(const struct dos_load_plan *plan)
{
	return plan->format == 0xa1u && plan->old_mz_signature == 0xa2u &&
	       plan->load_high == 0xa3u && plan->target_kind == 0xa4u &&
	       plan->reserved32 == 0xa5a5a5a5u &&
	       plan->file_size == 0x1111222233334444ull &&
	       plan->image_file_offset == 0x5555666677778888ull &&
	       plan->image_size == 0x9999aaaabbbbccccull &&
	       plan->minimum_image_paragraphs == 0xddddeeeeffff0001ull &&
	       plan->minimum_extra_paragraphs == 0x1234u &&
	       plan->maximum_extra_paragraphs == 0x2345u &&
	       plan->initial_cs == 0x3456u && plan->initial_ip == 0x4567u &&
	       plan->initial_ss == 0x5678u && plan->initial_sp == 0x6789u &&
	       plan->relocation_count == 0x789au &&
	       plan->relocation_table_offset == 0x89abu;
}

/* Deliberately impossible poison values prove all-or-unchanged output. */
static void set_target_result_sentinel(
    struct dos_exec_transaction_target *target)
{
	*target = (struct dos_exec_transaction_target){
	    .allocation =
		{
		    .format = 0xa1u,
		    .load_high = 0xa2u,
		    .reserved = 0xa3a3u,
		    .available_paragraphs = 0xa4a4u,
		    .block_paragraphs = 0xa5a5u,
		},
	    .lease =
		{
		    .handle = {.value = HANDLE_SENTINEL},
		    .guest_segment = 0xb1b1u,
		    .paragraphs = 0xb2b2u,
		    .maximum_available = 0xb3b3u,
		    .reserved = 0xb4b4u,
		},
	    .has_load_block = 0xc1u,
	    .reserved = {1u, 2u, 3u, 4u, 5u, 6u, 7u},
	};
}

static bool target_result_is_sentinel(
    const struct dos_exec_transaction_target *target)
{
	return target->allocation.format == 0xa1u &&
	       target->allocation.load_high == 0xa2u &&
	       target->allocation.reserved == 0xa3a3u &&
	       target->allocation.available_paragraphs == 0xa4a4u &&
	       target->allocation.block_paragraphs == 0xa5a5u &&
	       target->lease.handle.value == HANDLE_SENTINEL &&
	       target->lease.guest_segment == 0xb1b1u &&
	       target->lease.paragraphs == 0xb2b2u &&
	       target->lease.maximum_available == 0xb3b3u &&
	       target->lease.reserved == 0xb4b4u &&
	       target->has_load_block == 0xc1u &&
	       target->reserved[0] == 1u && target->reserved[1] == 2u &&
	       target->reserved[2] == 3u && target->reserved[3] == 4u &&
	       target->reserved[4] == 5u && target->reserved[5] == 6u &&
	       target->reserved[6] == 7u;
}

static void set_resident_result_sentinel(
    struct dos_exec_transaction_resident *resident)
{
	size_t index;

	for (index = 0u; index < ARRAY_SIZE(resident->process.bytes); ++index)
		resident->process.bytes[index] = 0xa6u;
	resident->load.lease_handle = HANDLE_SENTINEL;
	resident->load.file_bytes_written = 0xb1b1b1b1u;
	resident->load.resident_bytes = 0xb2b2b2b2u;
	resident->load.untouched_bytes = 0xb3b3b3b3u;
	resident->load.reserved = 0xb4b4b4b4u;
	resident->format = 0xc1u;
	resident->has_process_plan = 0xc2u;
	resident->has_resident = 0xc3u;
	for (index = 0u; index < ARRAY_SIZE(resident->reserved); ++index)
		resident->reserved[index] = (uint8_t)(index + 1u);
}

static bool resident_result_is_sentinel(
    const struct dos_exec_transaction_resident *resident)
{
	size_t index;

	for (index = 0u; index < ARRAY_SIZE(resident->process.bytes); ++index) {
		if (resident->process.bytes[index] != 0xa6u)
			return false;
	}
	if (resident->load.lease_handle != HANDLE_SENTINEL ||
	    resident->load.file_bytes_written != 0xb1b1b1b1u ||
	    resident->load.resident_bytes != 0xb2b2b2b2u ||
	    resident->load.untouched_bytes != 0xb3b3b3b3u ||
	    resident->load.reserved != 0xb4b4b4b4u ||
	    resident->format != 0xc1u || resident->has_process_plan != 0xc2u ||
	    resident->has_resident != 0xc3u)
		return false;
	for (index = 0u; index < ARRAY_SIZE(resident->reserved); ++index) {
		if (resident->reserved[index] != (uint8_t)(index + 1u))
			return false;
	}
	return true;
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

static void put_file_word(size_t offset, uint16_t value)
{
	file_image[offset] = (uint8_t)value;
	file_image[offset + 1u] = (uint8_t)(value >> 8);
}

static void configure_mz_file(void)
{
	configured_open_size = 512u;
	configured_read_status = DOS_IMAGE_READ_OK;
	file_image_readable_size = ARRAY_SIZE(file_image);
	put_file_word(0u, 0x5a4du);
	put_file_word(2u, 0u);
	put_file_word(4u, 1u);
	put_file_word(6u, 2u);
	put_file_word(8u, 2u);
	put_file_word(10u, 1u);
	put_file_word(12u, 0xffffu);
	put_file_word(14u, 0x20u);
	put_file_word(16u, 0xfffeu);
	put_file_word(20u, 0x100u);
	put_file_word(22u, 0x10u);
	put_file_word(24u, 0x1cu);
	/* Two offset:segment relocation entries.  The second one
	 * begins at the resident file boundary, which EXEC permits. */
	put_file_word(28u, 0u);
	put_file_word(30u, 0u);
	put_file_word(32u, 2u);
	put_file_word(34u, 0u);
	file_image[32u] = 0x31u;
	file_image[287u] = 0x72u;
	file_image[511u] = 0x93u;
}

static bool event_was_recorded_after(uint32_t first, uint8_t expected)
{
	uint32_t index;
	uint32_t end = event_count < EVENT_CAPACITY ? event_count : EVENT_CAPACITY;

	for (index = first; index < end; ++index) {
		if (events[index] == expected)
			return true;
	}
	return false;
}

static bool name_plan_is_zero(const struct dos_exec_name_plan *plan)
{
	return plan->source.offset == 0u && plan->source.segment == 0u &&
	       plan->bytes_including_nul == 0u && plan->reserved == 0u;
}

static void set_environment_source_sentinel(
    struct dos_exec_environment_source_plan *source)
{
	source->source.offset = 0x1111u;
	source->source.segment = 0x2222u;
	source->parent_psp = 0x3333u;
	source->subfunction = 0x44u;
	source->kind = 0x55u;
}

static bool environment_source_is_sentinel(
    const struct dos_exec_environment_source_plan *source)
{
	return source->source.offset == 0x1111u &&
	       source->source.segment == 0x2222u &&
	       source->parent_psp == 0x3333u && source->subfunction == 0x44u &&
	       source->kind == 0x55u;
}

static bool prepare_file_probed(
    struct test_fixture *fixture,
    const struct dos_exec_transaction_request *request,
    struct dos_exec_transaction_handle *handle)
{
	uint32_t detail = DETAIL_SENTINEL;
	uint8_t is_device = BYTE_SENTINEL;

	return dos_exec_transaction_begin(&fixture->transactions,
					  &fixture->services, request,
					  handle) == DOS_EXEC_TRANSACTION_OK &&
	       dos_exec_transaction_open(
		   &fixture->transactions, *handle, &fixture->services,
		   executable_name, ARRAY_SIZE(executable_name), &detail) ==
		   DOS_EXEC_TRANSACTION_OK &&
	       detail == 0u &&
	       dos_exec_transaction_probe(
		   &fixture->transactions, *handle, &fixture->services,
		   &is_device, &detail) == DOS_EXEC_TRANSACTION_OK &&
	       is_device == 0u && detail == 0u;
}

static bool prepare_process_without_environment(
    struct test_fixture *fixture,
    const struct dos_exec_transaction_request *request,
    struct dos_exec_transaction_handle *handle)
{
	struct dos_exec_environment_source_plan source;
	struct dos_exec_transaction_environment environment;

	put_guest_word(request->parameter_block.segment,
		       request->parameter_block.offset, 0u);
	put_guest_word(0x1234u, 0x002cu, 0u);
	return prepare_file_probed(fixture, request, handle) &&
	       dos_exec_transaction_select_environment(
		   &fixture->transactions, *handle, &fixture->services,
		   &source) == DOS_EXEC_TRANSACTION_OK &&
	       source.kind == DOS_EXEC_ENVIRONMENT_SOURCE_NONE &&
	       source.parent_psp == 0x1234u &&
	       dos_exec_transaction_prepare_environment(
		   &fixture->transactions, *handle, &fixture->services,
		   &environment) == DOS_EXEC_TRANSACTION_OK &&
	       environment_result_is_zero(&environment);
}

static bool prepare_process_image(
    struct test_fixture *fixture,
    const struct dos_exec_transaction_request *request,
    struct dos_exec_transaction_handle *handle)
{
	struct dos_load_plan image;

	configure_mz_file();
	return prepare_process_without_environment(fixture, request, handle) &&
	       dos_exec_transaction_inspect_image(
		   &fixture->transactions, *handle, &fixture->services,
		   &image) == DOS_EXEC_TRANSACTION_OK &&
	       dos_load_plan_has_inspected_encoding(&image) &&
	       image.format == (uint8_t)DOS_IMAGE_MZ;
}

static bool prepare_mz_process_psp(
    struct test_fixture *fixture,
    const struct dos_exec_transaction_request *request,
    struct dos_exec_transaction_handle *handle)
{
	struct dos_exec_transaction_target target;
	struct dos_exec_transaction_resident resident;
	struct dos_exec_transaction_relocation relocation;
	struct dos_exec_transaction_parent parent;
	struct dos_exec_transaction_inheritance inheritance;
	struct dos_exec_transaction_psp psp;
	struct dos_process_far_address terminate_vector = {
	    .segment = 0x7777u,
	    .offset = 0x8888u,
	};
	size_t index;

	if (!prepare_process_image(fixture, request, handle) ||
	    dos_exec_transaction_prepare_target(
		&fixture->transactions, *handle, &fixture->services,
		&target) != DOS_EXEC_TRANSACTION_OK ||
	    dos_exec_transaction_load_resident(
		&fixture->transactions, *handle, &fixture->services,
		&resident) != DOS_EXEC_TRANSACTION_OK ||
	    dos_exec_transaction_relocate_resident(
		&fixture->transactions, *handle, &fixture->services,
		&relocation) != DOS_EXEC_TRANSACTION_OK ||
	    dos_exec_transaction_close(&fixture->transactions, *handle,
				       &fixture->services) !=
		DOS_EXEC_TRANSACTION_OK)
		return false;

	guest_memory[(size_t)dos_far_to_linear(0x1234u, 0u, false)] = 0xcdu;
	put_guest_word(
	    0x1234u,
	    (uint16_t)__builtin_offsetof(struct dos_psp_prefix40, jft_length),
	    DOS_PSP_DEFAULT_HANDLES);
	put_guest_far(
	    0x1234u,
	    (uint16_t)__builtin_offsetof(struct dos_psp_prefix40, jft_pointer),
	    0x1234u,
	    (uint16_t)__builtin_offsetof(struct dos_psp_prefix40, jft));
	put_guest_far(
	    0x1234u,
	    (uint16_t)__builtin_offsetof(struct dos_psp_prefix40,
					 control_c_vector),
	    0x1111u, 0x2222u);
	put_guest_far(
	    0x1234u,
	    (uint16_t)__builtin_offsetof(struct dos_psp_prefix40,
					 fatal_abort_vector),
	    0x3333u, 0x4444u);
	for (index = 0u; index < DOS_PSP_DEFAULT_HANDLES; ++index) {
		dos_linear_address_t linear = dos_far_to_linear(
		    0x1234u,
		    (uint16_t)(__builtin_offsetof(struct dos_psp_prefix40, jft) +
			       index),
		    false);

		guest_memory[(size_t)linear] = (uint8_t)(index + 1u);
	}
	if (dos_exec_transaction_capture_parent(
		&fixture->transactions, *handle, &fixture->services,
		&parent) != DOS_EXEC_TRANSACTION_OK ||
	    dos_exec_transaction_prepare_inheritance(
		&fixture->transactions, *handle, &fixture->services,
		&inheritance) != DOS_EXEC_TRANSACTION_OK)
		return false;

	put_guest_far(request->parameter_block.segment,
		      (uint16_t)(request->parameter_block.offset +
				 TEST_EXEC_FIRST_FCB_POINTER_OFFSET),
		      0x2200u, 0x0100u);
	put_guest_far(request->parameter_block.segment,
		      (uint16_t)(request->parameter_block.offset +
				 TEST_EXEC_SECOND_FCB_POINTER_OFFSET),
		      0x2200u, 0x0200u);
	put_guest_far(request->parameter_block.segment,
		      (uint16_t)(request->parameter_block.offset +
				 TEST_EXEC_COMMAND_TAIL_POINTER_OFFSET),
		      0x2200u, 0x0300u);
	for (index = 0u; index < DOS_PROCESS_FCB_PREFIX_BYTES; ++index) {
		guest_memory[(size_t)dos_far_to_linear(
		    0x2200u, (uint16_t)(0x0100u + index), false)] =
		    (uint8_t)(0x10u + index);
		guest_memory[(size_t)dos_far_to_linear(
		    0x2200u, (uint16_t)(0x0200u + index), false)] =
		    (uint8_t)(0x40u + index);
	}
	for (index = 0u; index < sizeof(struct dos_command_tail40); ++index)
		guest_memory[(size_t)dos_far_to_linear(
		    0x2200u, (uint16_t)(0x0300u + index), false)] =
		    (uint8_t)(0x80u + index);
	return dos_exec_transaction_prepare_psp(
		   &fixture->transactions, *handle, &fixture->services,
		   terminate_vector, &psp) == DOS_EXEC_TRANSACTION_OK &&
	       target.has_load_block == 1u && resident.has_resident == 1u &&
	       relocation.applied == 1u && parent.has_snapshot == 1u &&
	       inheritance.has_child_jft == 1u && psp.has_image == 1u;
}

static int test_normal_source_order_and_aba(void)
{
	struct test_fixture fixture;
	struct dos_exec_transaction_request request =
	    make_request(DOS_EXEC_LOAD_AND_EXECUTE);
	struct dos_exec_transaction_handle first;
	struct dos_exec_transaction_handle second;
	struct dos_exec_transaction_slot *slot;
	uint32_t detail = DETAIL_SENTINEL;
	uint8_t is_device = BYTE_SENTINEL;
	uint32_t calls_before;

	reset_adapters();
	if (!initialize_fixture(&fixture) ||
	    !dos_exec_transaction_table_is_drained(&fixture.transactions))
		return 1;
	first.value = HANDLE_SENTINEL;
	if (dos_exec_transaction_begin(&fixture.transactions, &fixture.services,
				       &request,
				       &first) != DOS_EXEC_TRANSACTION_OK)
		return 2;
	if (first.value == HANDLE_SENTINEL)
		return 20;
	if (observer_acquire_calls != 1u)
		return 21;
	if (event_count != 1u || events[0] != EVENT_OBSERVER_ACQUIRE)
		return 22;
	slot = &fixture.transactions.slots[transaction_slot(first)];
	if (slot->state != DOS_EXEC_TRANSACTION_STATE_OBSERVED ||
	    slot->coordinator_identity != TEST_COORDINATOR_ID ||
	    slot->machine_identity != TEST_MACHINE_ID ||
	    slot->machine_context != TEST_MACHINE_CONTEXT ||
	    slot->file_adapter_identity != TEST_FILE_ID ||
	    slot->file_lease_table_identity != TEST_FILE_TABLE_ID ||
	    slot->runtime_identity != TEST_RUNTIME_ID ||
	    slot->sft_adapter_identity != TEST_SFT_ID ||
	    slot->observer_adapter_identity != TEST_OBSERVER_ID ||
	    slot->parent_runtime.generation != fixture.runtime.generation ||
	    slot->parent_runtime.runtime_identity != TEST_RUNTIME_ID ||
	    slot->parent_runtime.current_psp != 0x1234u ||
	    slot->request.subfunction != DOS_EXEC_LOAD_AND_EXECUTE)
		return 3;
	if (dos_exec_transaction_open(&fixture.transactions, first,
				      &fixture.services, executable_name,
				      ARRAY_SIZE(executable_name),
				      &detail) != DOS_EXEC_TRANSACTION_OK ||
	    detail != 0u || open_calls != 1u || event_count != 2u ||
	    events[1] != EVENT_FILE_OPEN || !callback_arguments_valid ||
	    slot->state != DOS_EXEC_TRANSACTION_STATE_FILE_OPEN ||
	    slot->has_file_lease != 1u)
		return 4;
	if (dos_exec_transaction_probe(&fixture.transactions, first,
				       &fixture.services, &is_device,
				       &detail) != DOS_EXEC_TRANSACTION_OK ||
	    is_device != 0u || detail != 0u || probe_calls != 1u ||
	    event_count != 3u || events[2] != EVENT_FILE_PROBE ||
	    slot->state != DOS_EXEC_TRANSACTION_STATE_FILE_PROBED)
		return 5;
	if (dos_exec_transaction_close(&fixture.transactions, first,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_OK ||
	    close_calls != 1u || event_count != 4u ||
	    events[3] != EVENT_FILE_CLOSE ||
	    slot->state != DOS_EXEC_TRANSACTION_STATE_FILE_CLOSED)
		return 6;
	if (dos_exec_transaction_abort(&fixture.transactions, first,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_OK ||
	    close_calls != 1u || observer_release_calls != 1u ||
	    event_count != 5u || events[4] != EVENT_OBSERVER_RELEASE ||
	    slot->state != DOS_EXEC_TRANSACTION_STATE_ABORTED ||
	    slot->has_file_lease != 0u ||
	    !dos_exec_file_lease_table_is_drained(&fixture.files))
		return 7;
	calls_before = observer_release_calls;
	if (dos_exec_transaction_abort(&fixture.transactions, first,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_OK ||
	    observer_release_calls != calls_before ||
	    dos_exec_transaction_retire(&fixture.transactions,
					TEST_COORDINATOR_ID,
					first) != DOS_EXEC_TRANSACTION_OK ||
	    !dos_exec_transaction_table_is_drained(&fixture.transactions))
		return 8;

	request = make_request(DOS_EXEC_OVERLAY);
	if (dos_exec_transaction_begin(&fixture.transactions, &fixture.services,
				       &request,
				       &second) != DOS_EXEC_TRANSACTION_OK ||
	    transaction_slot(second) != transaction_slot(first) ||
	    second.value == first.value ||
	    dos_exec_transaction_abort(&fixture.transactions, first,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_STALE_HANDLE ||
	    dos_exec_transaction_abort(&fixture.transactions, second,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_OK ||
	    dos_exec_transaction_retire(&fixture.transactions,
					TEST_COORDINATOR_ID,
					second) != DOS_EXEC_TRANSACTION_OK)
		return 9;
	return 0;
}

static int test_subfunctions_capacity_and_nonwrap(void)
{
	struct test_fixture fixture;
	struct dos_exec_transaction_request request;
	struct dos_exec_transaction_handle handle;
	struct dos_exec_transaction_handle sentinel = {.value =
							   HANDLE_SENTINEL};
	uint8_t accepted[] = {DOS_EXEC_LOAD_ONLY, DOS_EXEC_OVERLAY};
	uint32_t index;
	uint32_t calls_before;

	reset_adapters();
	if (!initialize_fixture(&fixture) ||
	    dos_exec_transaction_table_initialize(&fixture.transactions,
						  TEST_COORDINATOR_ID) !=
		DOS_EXEC_TRANSACTION_INVALID_STATE)
		return 1;
	request = make_request(2u);
	if (dos_exec_transaction_begin(&fixture.transactions, &fixture.services,
				       &request, &sentinel) !=
		DOS_EXEC_TRANSACTION_INVALID_ARGUMENT ||
	    sentinel.value != HANDLE_SENTINEL || observer_acquire_calls != 0u)
		return 2;
	request = make_request(4u);
	if (dos_exec_transaction_begin(&fixture.transactions, &fixture.services,
				       &request, &sentinel) !=
		DOS_EXEC_TRANSACTION_INVALID_ARGUMENT ||
	    sentinel.value != HANDLE_SENTINEL || observer_acquire_calls != 0u)
		return 3;
	for (index = 0u; index < ARRAY_SIZE(accepted); ++index) {
		request = make_request(accepted[index]);
		if (dos_exec_transaction_begin(
			&fixture.transactions, &fixture.services, &request,
			&handle) != DOS_EXEC_TRANSACTION_OK)
			return 4;
		if (dos_exec_transaction_abort(&fixture.transactions, handle,
					       &fixture.services) !=
			DOS_EXEC_TRANSACTION_OK ||
		    dos_exec_transaction_retire(&fixture.transactions,
						TEST_COORDINATOR_ID, handle) !=
			DOS_EXEC_TRANSACTION_OK)
			return 5;
	}

	request = make_request(DOS_EXEC_LOAD_AND_EXECUTE);
	if (dos_exec_transaction_begin(&fixture.transactions, &fixture.services,
				       &request,
				       &handle) != DOS_EXEC_TRANSACTION_OK)
		return 6;
	calls_before = observer_acquire_calls;
	if (dos_exec_transaction_begin(&fixture.transactions, &fixture.services,
				       &request, &sentinel) !=
		DOS_EXEC_TRANSACTION_BUSY ||
	    sentinel.value != HANDLE_SENTINEL ||
	    observer_acquire_calls != calls_before)
		return 7;
	if (dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_OK ||
	    dos_exec_transaction_retire(&fixture.transactions,
					TEST_COORDINATOR_ID,
					handle) != DOS_EXEC_TRANSACTION_OK)
		return 8;
	fixture.transactions.slots[0].generation =
	    DOS_EXEC_TRANSACTION_GENERATION_MAX;
	if (dos_exec_transaction_begin(&fixture.transactions, &fixture.services,
				       &request,
				       &handle) != DOS_EXEC_TRANSACTION_OK ||
	    transaction_slot(handle) != 1u ||
	    dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_OK ||
	    dos_exec_transaction_retire(&fixture.transactions,
					TEST_COORDINATOR_ID,
					handle) != DOS_EXEC_TRANSACTION_OK)
		return 9;
	for (index = 0u; index < DOS_EXEC_TRANSACTION_SLOT_COUNT; ++index)
		fixture.transactions.slots[index].generation =
		    DOS_EXEC_TRANSACTION_GENERATION_MAX;
	calls_before = observer_acquire_calls;
	if (dos_exec_transaction_begin(&fixture.transactions, &fixture.services,
				       &request, &sentinel) !=
		DOS_EXEC_TRANSACTION_GENERATION_EXHAUSTED ||
	    sentinel.value != HANDLE_SENTINEL ||
	    observer_acquire_calls != calls_before)
		return 10;
	return 0;
}

static int test_overlay_omits_process_only_bindings(void)
{
	struct test_fixture fixture;
	struct dos_exec_transaction_services overlay_services;
	struct dos_exec_transaction_services wrong_services;
	struct dos_exec_transaction_request request =
	    make_request(DOS_EXEC_OVERLAY);
	struct dos_exec_transaction_handle handle = {.value = HANDLE_SENTINEL};
	struct dos_exec_transaction_slot *slot;
	uint32_t detail = DETAIL_SENTINEL;
	uint8_t is_device = BYTE_SENTINEL;
	uint32_t acquire_before;

	reset_adapters();
	if (!initialize_fixture(&fixture))
		return 1;
	overlay_services = fixture.services;
	overlay_services.runtime = NULL;
	overlay_services.sft_ops = NULL;
	overlay_services.sft_adapter_identity = KERNEL_OBJECT_HANDLE_INVALID;
	overlay_services.sft_adapter_context = KERNEL_OBJECT_HANDLE_INVALID;
	overlay_services.drive_ops = NULL;
	overlay_services.drive_adapter_identity = KERNEL_OBJECT_HANDLE_INVALID;
	overlay_services.drive_adapter_context = KERNEL_OBJECT_HANDLE_INVALID;
	request.parameter_block.offset = 0u;
	request.parameter_block.segment = 0u;
	if (dos_exec_transaction_begin(&fixture.transactions, &overlay_services,
				       &request,
				       &handle) != DOS_EXEC_TRANSACTION_OK)
		return 2;
	slot = &fixture.transactions.slots[transaction_slot(handle)];
	if (slot->runtime_identity != KERNEL_OBJECT_HANDLE_INVALID ||
	    slot->sft_adapter_identity != KERNEL_OBJECT_HANDLE_INVALID ||
	    slot->sft_adapter_context != KERNEL_OBJECT_HANDLE_INVALID ||
	    slot->drive_adapter_identity != KERNEL_OBJECT_HANDLE_INVALID ||
	    slot->drive_adapter_context != KERNEL_OBJECT_HANDLE_INVALID ||
	    slot->parent_runtime.generation != 0u ||
	    slot->parent_runtime.runtime_identity != 0u ||
	    slot->parent_runtime.dta.offset != 0u ||
	    slot->parent_runtime.dta.segment != 0u ||
	    slot->parent_runtime.current_psp != 0u ||
	    slot->parent_runtime.reserved != 0u)
		return 3;
	wrong_services = overlay_services;
	wrong_services.file_adapter_context++;
	if (dos_exec_transaction_open(&fixture.transactions, handle,
				      &wrong_services, executable_name,
				      ARRAY_SIZE(executable_name), &detail) !=
		DOS_EXEC_TRANSACTION_BINDING_MISMATCH ||
	    detail != DETAIL_SENTINEL || open_calls != 0u)
		return 4;
	if (dos_exec_transaction_open(&fixture.transactions, handle,
				      &overlay_services, executable_name,
				      ARRAY_SIZE(executable_name),
				      &detail) != DOS_EXEC_TRANSACTION_OK ||
	    dos_exec_transaction_probe(&fixture.transactions, handle,
				       &overlay_services, &is_device,
				       &detail) != DOS_EXEC_TRANSACTION_OK ||
	    dos_exec_transaction_abort(&fixture.transactions, handle,
				       &overlay_services) !=
		DOS_EXEC_TRANSACTION_OK ||
	    dos_exec_transaction_retire(&fixture.transactions,
					TEST_COORDINATOR_ID,
					handle) != DOS_EXEC_TRANSACTION_OK)
		return 5;

	reset_adapters();
	if (!initialize_fixture(&fixture))
		return 6;
	overlay_services = fixture.services;
	overlay_services.sft_ops = NULL;
	overlay_services.sft_adapter_identity = KERNEL_OBJECT_HANDLE_INVALID;
	overlay_services.sft_adapter_context = KERNEL_OBJECT_HANDLE_INVALID;
	overlay_services.drive_ops = NULL;
	overlay_services.drive_adapter_identity = KERNEL_OBJECT_HANDLE_INVALID;
	overlay_services.drive_adapter_context = KERNEL_OBJECT_HANDLE_INVALID;
	handle.value = HANDLE_SENTINEL;
	if (dos_exec_transaction_begin(&fixture.transactions, &overlay_services,
				       &request,
				       &handle) != DOS_EXEC_TRANSACTION_OK ||
	    dos_process_runtime_set_current_psp(&fixture.runtime, 0x4321u) !=
		DOS_PROCESS_RUNTIME_OK ||
	    dos_exec_transaction_abort(&fixture.transactions, handle,
				       &overlay_services) !=
		DOS_EXEC_TRANSACTION_OK ||
	    dos_exec_transaction_retire(&fixture.transactions,
					TEST_COORDINATOR_ID,
					handle) != DOS_EXEC_TRANSACTION_OK)
		return 7;

	reset_adapters();
	if (!initialize_fixture(&fixture) ||
	    dos_process_runtime_poison(&fixture.runtime) !=
		DOS_PROCESS_RUNTIME_OK)
		return 8;
	overlay_services = fixture.services;
	overlay_services.sft_ops = NULL;
	overlay_services.sft_adapter_identity = KERNEL_OBJECT_HANDLE_INVALID;
	overlay_services.sft_adapter_context = KERNEL_OBJECT_HANDLE_INVALID;
	overlay_services.drive_ops = NULL;
	overlay_services.drive_adapter_identity = KERNEL_OBJECT_HANDLE_INVALID;
	overlay_services.drive_adapter_context = KERNEL_OBJECT_HANDLE_INVALID;
	handle.value = HANDLE_SENTINEL;
	if (dos_exec_transaction_begin(&fixture.transactions, &overlay_services,
				       &request,
				       &handle) != DOS_EXEC_TRANSACTION_OK ||
	    dos_exec_transaction_abort(&fixture.transactions, handle,
				       &overlay_services) !=
		DOS_EXEC_TRANSACTION_OK ||
	    dos_exec_transaction_retire(&fixture.transactions,
					TEST_COORDINATOR_ID,
					handle) != DOS_EXEC_TRANSACTION_OK)
		return 9;
	acquire_before = observer_acquire_calls;
	request = make_request(DOS_EXEC_LOAD_AND_EXECUTE);
	handle.value = HANDLE_SENTINEL;
	if (dos_exec_transaction_begin(&fixture.transactions, &overlay_services,
				       &request, &handle) !=
		DOS_EXEC_TRANSACTION_INVALID_ARGUMENT ||
	    handle.value != HANDLE_SENTINEL ||
	    observer_acquire_calls != acquire_before ||
	    !dos_exec_transaction_table_is_drained(&fixture.transactions))
		return 10;
	return 0;
}

static int test_environment_selection_paths(void)
{
	struct test_fixture fixture;
	struct dos_exec_transaction_request request;
	struct dos_exec_transaction_handle handle;
	struct dos_exec_transaction_slot *slot;
	struct dos_exec_environment_source_plan source;
	uint32_t detail = DETAIL_SENTINEL;
	uint8_t is_device = BYTE_SENTINEL;
	uint32_t reads_before;

	/* AL=0 may select only after OPEN/IOCTL, and an explicit word prevents
	 * the parent PSP read. */
	reset_adapters();
	request = make_request(DOS_EXEC_LOAD_AND_EXECUTE);
	put_guest_word(request.parameter_block.segment,
		       request.parameter_block.offset, 0x4567u);
	put_guest_word(0x1234u, 0x002cu, 0x7777u);
	if (!initialize_fixture(&fixture) ||
	    dos_exec_transaction_begin(&fixture.transactions, &fixture.services,
				       &request, &handle) !=
		DOS_EXEC_TRANSACTION_OK)
		return 1;
	set_environment_source_sentinel(&source);
	if (dos_exec_transaction_select_environment(
		&fixture.transactions, handle, &fixture.services, &source) !=
		DOS_EXEC_TRANSACTION_INVALID_STATE ||
	    !environment_source_is_sentinel(&source) || machine_read_calls != 0u)
		return 2;
	if (dos_exec_transaction_open(
		&fixture.transactions, handle, &fixture.services,
		executable_name, ARRAY_SIZE(executable_name), &detail) !=
		DOS_EXEC_TRANSACTION_OK)
		return 3;
	set_environment_source_sentinel(&source);
	if (dos_exec_transaction_select_environment(
		&fixture.transactions, handle, &fixture.services, &source) !=
		DOS_EXEC_TRANSACTION_INVALID_STATE ||
	    !environment_source_is_sentinel(&source) || machine_read_calls != 0u ||
	    dos_exec_transaction_probe(
		&fixture.transactions, handle, &fixture.services, &is_device,
		&detail) != DOS_EXEC_TRANSACTION_OK)
		return 4;
	reads_before = machine_read_calls;
	if (dos_exec_transaction_select_environment(
		&fixture.transactions, handle, &fixture.services, NULL) !=
		DOS_EXEC_TRANSACTION_INVALID_ARGUMENT ||
	    machine_read_calls != reads_before)
		return 5;
	set_environment_source_sentinel(&source);
	if (dos_exec_transaction_select_environment(
		&fixture.transactions, handle, &fixture.services, &source) !=
		DOS_EXEC_TRANSACTION_OK ||
	    machine_read_calls != reads_before + 1u ||
	    event_count != 4u || events[3] != EVENT_MACHINE_READ ||
	    source.source.offset != 0u || source.source.segment != 0x4567u ||
	    source.parent_psp != 0u ||
	    source.subfunction != DOS_EXEC_LOAD_AND_EXECUTE ||
	    source.kind != DOS_EXEC_ENVIRONMENT_SOURCE_PARAMETER)
		return 6;
	slot = &fixture.transactions.slots[transaction_slot(handle)];
	if (slot->state != DOS_EXEC_TRANSACTION_STATE_ENV_READY ||
	    slot->environment_source.source.segment != 0x4567u ||
	    slot->environment_source.kind !=
		DOS_EXEC_ENVIRONMENT_SOURCE_PARAMETER ||
	    !dos_exec_environment_source_plan_has_valid_encoding(
		&slot->environment_source))
		return 7;
	set_environment_source_sentinel(&source);
	reads_before = machine_read_calls;
	if (dos_exec_transaction_select_environment(
		&fixture.transactions, handle, &fixture.services, &source) !=
		DOS_EXEC_TRANSACTION_INVALID_STATE ||
	    !environment_source_is_sentinel(&source) ||
	    machine_read_calls != reads_before ||
	    dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_OK ||
	    slot->state != DOS_EXEC_TRANSACTION_STATE_ABORTED ||
	    slot->has_file_lease != 0u || close_calls != 1u ||
	    observer_release_calls != 1u || event_count != 6u ||
	    events[4] != EVENT_FILE_CLOSE ||
	    events[5] != EVENT_OBSERVER_RELEASE ||
	    slot->environment_source.source.segment != 0x4567u)
		return 8;
	if (dos_exec_transaction_retire(&fixture.transactions,
					TEST_COORDINATOR_ID,
					handle) != DOS_EXEC_TRANSACTION_OK ||
	    slot->environment_source.source.offset != 0u ||
	    slot->environment_source.source.segment != 0u ||
	    slot->environment_source.parent_psp != 0u ||
	    slot->environment_source.subfunction != 0u ||
	    slot->environment_source.kind != 0u)
		return 9;

	/* AL=1 reads CurrentPDB:002ch only after the parameter word is zero. */
	reset_adapters();
	request = make_request(DOS_EXEC_LOAD_ONLY);
	put_guest_word(request.parameter_block.segment,
		       request.parameter_block.offset, 0u);
	put_guest_word(0x1234u, 0x002cu, 0x6789u);
	if (!initialize_fixture(&fixture) ||
	    !prepare_file_probed(&fixture, &request, &handle) ||
	    dos_exec_transaction_select_environment(
		&fixture.transactions, handle, &fixture.services, &source) !=
		DOS_EXEC_TRANSACTION_OK ||
	    machine_read_calls != 2u || source.source.offset != 0u ||
	    source.source.segment != 0x6789u || source.parent_psp != 0x1234u ||
	    source.subfunction != DOS_EXEC_LOAD_ONLY ||
	    source.kind != DOS_EXEC_ENVIRONMENT_SOURCE_PARENT ||
	    dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_OK ||
	    dos_exec_transaction_retire(&fixture.transactions,
					TEST_COORDINATOR_ID,
					handle) != DOS_EXEC_TRANSACTION_OK)
		return 10;

	/* The legal AL=0, CurrentPDB=0, no-environment plan is all zero; state
	 * still distinguishes it from an unpublished plan. */
	reset_adapters();
	request = make_request(DOS_EXEC_LOAD_AND_EXECUTE);
	put_guest_word(request.parameter_block.segment,
		       request.parameter_block.offset, 0u);
	put_guest_word(0u, 0x002cu, 0u);
	if (!initialize_fixture(&fixture) ||
	    dos_process_runtime_set_current_psp(&fixture.runtime, 0u) !=
		DOS_PROCESS_RUNTIME_OK ||
	    !prepare_file_probed(&fixture, &request, &handle) ||
	    dos_exec_transaction_select_environment(
		&fixture.transactions, handle, &fixture.services, &source) !=
		DOS_EXEC_TRANSACTION_OK ||
	    machine_read_calls != 2u || source.source.offset != 0u ||
	    source.source.segment != 0u || source.parent_psp != 0u ||
	    source.subfunction != DOS_EXEC_LOAD_AND_EXECUTE ||
	    source.kind != DOS_EXEC_ENVIRONMENT_SOURCE_NONE ||
	    fixture.transactions.slots[transaction_slot(handle)].state !=
		DOS_EXEC_TRANSACTION_STATE_ENV_READY ||
	    dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_OK ||
	    dos_exec_transaction_retire(&fixture.transactions,
					TEST_COORDINATOR_ID,
					handle) != DOS_EXEC_TRANSACTION_OK)
		return 11;

	/* EXEC3 owns no process environment continuation and performs no read. */
	reset_adapters();
	request = make_request(DOS_EXEC_OVERLAY);
	if (!initialize_fixture(&fixture) ||
	    !prepare_file_probed(&fixture, &request, &handle))
		return 12;
	set_environment_source_sentinel(&source);
	if (dos_exec_transaction_select_environment(
		&fixture.transactions, handle, &fixture.services, &source) !=
		DOS_EXEC_TRANSACTION_INVALID_STATE ||
	    !environment_source_is_sentinel(&source) || machine_read_calls != 0u ||
	    fixture.transactions.slots[transaction_slot(handle)].state !=
		DOS_EXEC_TRANSACTION_STATE_FILE_PROBED ||
	    dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_OK)
		return 13;
	return callback_arguments_valid ? 0 : 14;
}

static int test_environment_failures_and_corruption(void)
{
	static const uint32_t failing_reads[] = {1u, 2u};
	struct test_fixture fixture;
	struct dos_exec_transaction_request request;
	struct dos_exec_transaction_handle handle;
	struct dos_exec_transaction_slot *slot;
	struct dos_exec_environment_source_plan source;
	uint32_t index;

	/* Fault either the parameter word or the conditional parent word. */
	for (index = 0u; index < ARRAY_SIZE(failing_reads); ++index) {
		reset_adapters();
		request = make_request(DOS_EXEC_LOAD_ONLY);
		put_guest_word(request.parameter_block.segment,
			       request.parameter_block.offset, 0u);
		put_guest_word(0x1234u, 0x002cu, 0x6789u);
		if (!initialize_fixture(&fixture) ||
		    !prepare_file_probed(&fixture, &request, &handle))
			return 1;
		fail_machine_read_call = failing_reads[index];
		set_environment_source_sentinel(&source);
		if (dos_exec_transaction_select_environment(
			&fixture.transactions, handle, &fixture.services,
			&source) != DOS_EXEC_TRANSACTION_ENVIRONMENT_FAULT ||
		    machine_read_calls != failing_reads[index] ||
		    !environment_source_is_sentinel(&source))
			return 2;
		slot = &fixture.transactions.slots[transaction_slot(handle)];
		if (slot->state != DOS_EXEC_TRANSACTION_STATE_FAILED ||
		    slot->environment_source.source.offset != 0u ||
		    slot->environment_source.source.segment != 0u ||
		    slot->environment_source.parent_psp != 0u ||
		    slot->environment_source.subfunction != 0u ||
		    slot->environment_source.kind != 0u ||
		    dos_exec_transaction_abort(&fixture.transactions, handle,
					       &fixture.services) !=
			DOS_EXEC_TRANSACTION_OK ||
		    close_calls != 1u || observer_release_calls != 1u ||
		    dos_exec_transaction_retire(&fixture.transactions,
						TEST_COORDINATOR_ID,
						handle) !=
			DOS_EXEC_TRANSACTION_OK)
			return 3;
	}

	/* A changed runtime is rejected by pure preflight before guest memory. */
	reset_adapters();
	request = make_request(DOS_EXEC_LOAD_AND_EXECUTE);
	if (!initialize_fixture(&fixture) ||
	    !prepare_file_probed(&fixture, &request, &handle) ||
	    dos_process_runtime_set_current_psp(&fixture.runtime, 0x4321u) !=
		DOS_PROCESS_RUNTIME_OK)
		return 4;
	set_environment_source_sentinel(&source);
	if (dos_exec_transaction_select_environment(
		&fixture.transactions, handle, &fixture.services, &source) !=
		DOS_EXEC_TRANSACTION_RUNTIME_NOT_READY ||
	    !environment_source_is_sentinel(&source) || machine_read_calls != 0u ||
	    fixture.transactions.slots[transaction_slot(handle)].state !=
		DOS_EXEC_TRANSACTION_STATE_FAILED ||
	    dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_OK ||
	    close_calls != 1u || observer_release_calls != 1u ||
	    dos_exec_transaction_retire(&fixture.transactions,
					TEST_COORDINATOR_ID,
					handle) != DOS_EXEC_TRANSACTION_OK)
		return 5;

	/* Runtime poison is coordinator-wide and visible before quarantine
	 * reentry; it never starts the guest decoder. */
	reset_adapters();
	if (!initialize_fixture(&fixture) ||
	    !prepare_file_probed(&fixture, &request, &handle) ||
	    dos_process_runtime_poison(&fixture.runtime) !=
		DOS_PROCESS_RUNTIME_OK)
		return 6;
	configure_reentry(&fixture, &request, handle,
			  REENTRY_OBSERVER_QUARANTINE);
	set_environment_source_sentinel(&source);
	if (dos_exec_transaction_select_environment(
		&fixture.transactions, handle, &fixture.services, &source) !=
		DOS_EXEC_TRANSACTION_POISONED ||
	    !environment_source_is_sentinel(&source) || machine_read_calls != 0u ||
	    fixture.transactions.poisoned != 1u ||
	    fixture.transactions.slots[transaction_slot(handle)].state !=
		DOS_EXEC_TRANSACTION_STATE_POISONED ||
	    observer_quarantine_calls != 1u ||
	    reentry_status != DOS_EXEC_TRANSACTION_POISONED)
		return 7;

	/* A retained plan is validated on every public entry. */
	reset_adapters();
	put_guest_word(request.parameter_block.segment,
		       request.parameter_block.offset, 0x4567u);
	if (!initialize_fixture(&fixture) ||
	    !prepare_file_probed(&fixture, &request, &handle) ||
	    dos_exec_transaction_select_environment(
		&fixture.transactions, handle, &fixture.services, &source) !=
		DOS_EXEC_TRANSACTION_OK)
		return 8;
	slot = &fixture.transactions.slots[transaction_slot(handle)];
	slot->environment_source.kind = 0xffu;
	if (dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_INVALID_STATE ||
	    close_calls != 0u || observer_release_calls != 0u)
		return 9;

	/* A syntactically valid inherited plan must still name the captured
	 * parent PSP, not an arbitrary segment. */
	reset_adapters();
	request = make_request(DOS_EXEC_LOAD_ONLY);
	put_guest_word(request.parameter_block.segment,
		       request.parameter_block.offset, 0u);
	put_guest_word(0x1234u, 0x002cu, 0x6789u);
	if (!initialize_fixture(&fixture) ||
	    !prepare_file_probed(&fixture, &request, &handle) ||
	    dos_exec_transaction_select_environment(
		&fixture.transactions, handle, &fixture.services, &source) !=
		DOS_EXEC_TRANSACTION_OK)
		return 10;
	slot = &fixture.transactions.slots[transaction_slot(handle)];
	slot->environment_source.parent_psp++;
	if (!dos_exec_environment_source_plan_has_valid_encoding(
		&slot->environment_source) ||
	    dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_INVALID_STATE ||
	    close_calls != 0u || observer_release_calls != 0u)
		return 11;
	return callback_arguments_valid ? 0 : 12;
}

static int test_guest_name_open_boundary(void)
{
	static const uint8_t wrapped_name[] = {'W', 'R', 0u};
	static const uint8_t a20_name[] = {'X', 0u};
	struct test_fixture fixture;
	struct dos_exec_transaction_request request;
	struct dos_exec_transaction_handle handle;
	struct dos_exec_transaction_slot *slot;
	struct dos_exec_environment_source_plan environment_source;
	uint8_t short_scratch[2];
	uint32_t detail;
	uint32_t reads_before;
	uint8_t is_device;

	/* DStrLen fault precedes OPEN and publishes neither plan nor lease. */
	reset_adapters();
	request = make_request(DOS_EXEC_LOAD_AND_EXECUTE);
	if (!initialize_fixture(&fixture) ||
	    dos_exec_transaction_begin(&fixture.transactions, &fixture.services,
				       &request, &handle) !=
		DOS_EXEC_TRANSACTION_OK)
		return 1;
	fail_name_read_call = 2u;
	detail = DETAIL_SENTINEL;
	if (dos_exec_transaction_open(&fixture.transactions, handle,
				      &fixture.services, executable_name,
				      ARRAY_SIZE(executable_name), &detail) !=
		DOS_EXEC_TRANSACTION_NAME_FAULT)
		return 2;
	slot = &fixture.transactions.slots[transaction_slot(handle)];
	if (detail != DETAIL_SENTINEL || name_read_calls != 2u ||
	    open_calls != 0u || slot->state != DOS_EXEC_TRANSACTION_STATE_FAILED ||
	    slot->has_file_lease != 0u || !name_plan_is_zero(&slot->executable_name) ||
	    dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) != DOS_EXEC_TRANSACTION_OK ||
	    dos_exec_transaction_retire(&fixture.transactions,
					TEST_COORDINATOR_ID, handle) !=
		DOS_EXEC_TRANSACTION_OK)
		return 3;

	/* Bounded scratch is a deliberate safe deviation from unbounded DOS
	 * memory: stop at capacity, do not continue scanning, and do not OPEN. */
	reset_adapters();
	request = make_request(DOS_EXEC_LOAD_ONLY);
	if (!initialize_fixture(&fixture) ||
	    dos_exec_transaction_begin(&fixture.transactions, &fixture.services,
				       &request, &handle) !=
		DOS_EXEC_TRANSACTION_OK)
		return 4;
	detail = DETAIL_SENTINEL;
	if (dos_exec_transaction_open(&fixture.transactions, handle,
				      &fixture.services, short_scratch,
				      ARRAY_SIZE(short_scratch), &detail) !=
		DOS_EXEC_TRANSACTION_NAME_FAULT)
		return 5;
	slot = &fixture.transactions.slots[transaction_slot(handle)];
	if (detail != DETAIL_SENTINEL || name_read_calls != 2u ||
	    open_calls != 0u || !name_plan_is_zero(&slot->executable_name) ||
	    short_scratch[0] != canonical_executable_name[0] ||
	    short_scratch[1] != canonical_executable_name[1] ||
	    dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) != DOS_EXEC_TRANSACTION_OK ||
	    dos_exec_transaction_retire(&fixture.transactions,
					TEST_COORDINATOR_ID, handle) !=
		DOS_EXEC_TRANSACTION_OK)
		return 6;

	/* Empty ASCIZ has a DStrLen byte count of one and still reaches OPEN.
	 * The read callback reenters while NAME_READING and must be rejected. */
	reset_adapters();
	request = make_request(DOS_EXEC_LOAD_AND_EXECUTE);
	guest_memory[TEST_EXECUTABLE_NAME_LINEAR] = 0u;
	expected_name_length = 1u;
	if (!initialize_fixture(&fixture) ||
	    dos_exec_transaction_begin(&fixture.transactions, &fixture.services,
				       &request, &handle) !=
		DOS_EXEC_TRANSACTION_OK)
		return 7;
	configure_reentry(&fixture, &request, handle, REENTRY_NAME_READ);
	detail = DETAIL_SENTINEL;
	if (dos_exec_transaction_open(&fixture.transactions, handle,
				      &fixture.services, executable_name,
				      ARRAY_SIZE(executable_name), &detail) !=
		DOS_EXEC_TRANSACTION_OK)
		return 8;
	slot = &fixture.transactions.slots[transaction_slot(handle)];
	if (detail != 0u || name_read_calls != 1u || open_calls != 1u ||
	    reentry_status != DOS_EXEC_TRANSACTION_INVALID_STATE ||
	    slot->executable_name.bytes_including_nul != 1u ||
	    slot->executable_name.source.offset !=
		request.executable_name.offset ||
	    slot->executable_name.source.segment !=
		request.executable_name.segment ||
	    !dos_exec_name_plan_has_valid_encoding(&slot->executable_name) ||
	    dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) != DOS_EXEC_TRANSACTION_OK ||
	    dos_exec_transaction_retire(&fixture.transactions,
					TEST_COORDINATOR_ID, handle) !=
		DOS_EXEC_TRANSACTION_OK)
		return 9;

	/* An ordinary OPEN failure occurs after a successful scan but cannot
	 * publish that local plan into the persistent slot. */
	reset_adapters();
	request = make_request(DOS_EXEC_LOAD_ONLY);
	configured_open_status = DOS_EXEC_FILE_ADAPTER_FAULT;
	configured_open_detail = 0x12345678u;
	if (!initialize_fixture(&fixture) ||
	    dos_exec_transaction_begin(&fixture.transactions, &fixture.services,
				       &request, &handle) !=
		DOS_EXEC_TRANSACTION_OK)
		return 10;
	detail = DETAIL_SENTINEL;
	if (dos_exec_transaction_open(&fixture.transactions, handle,
				      &fixture.services, executable_name,
				      ARRAY_SIZE(executable_name), &detail) !=
		DOS_EXEC_TRANSACTION_OPEN_FAILED)
		return 11;
	slot = &fixture.transactions.slots[transaction_slot(handle)];
	if (detail != 0x12345678u || open_calls != 1u ||
	    slot->has_file_lease != 0u || !name_plan_is_zero(&slot->executable_name) ||
	    dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) != DOS_EXEC_TRANSACTION_OK ||
	    dos_exec_transaction_retire(&fixture.transactions,
					TEST_COORDINATOR_ID, handle) !=
		DOS_EXEC_TRANSACTION_OK)
		return 12;

	/* SCASB increments the 16-bit offset; it does not advance the segment. */
	reset_adapters();
	request = make_request(DOS_EXEC_LOAD_AND_EXECUTE);
	request.executable_name.offset = 0xfffeu;
	request.executable_name.segment = 0x2000u;
	put_guest_name(request.executable_name, wrapped_name,
		       ARRAY_SIZE(wrapped_name), false);
	expected_name_length = ARRAY_SIZE(wrapped_name);
	if (!initialize_fixture(&fixture) ||
	    dos_exec_transaction_begin(&fixture.transactions, &fixture.services,
				       &request, &handle) !=
		DOS_EXEC_TRANSACTION_OK ||
	    dos_exec_transaction_open(&fixture.transactions, handle,
				      &fixture.services, executable_name,
				      ARRAY_SIZE(executable_name), &detail) !=
		DOS_EXEC_TRANSACTION_OK)
		return 13;
	slot = &fixture.transactions.slots[transaction_slot(handle)];
	if (name_read_calls != ARRAY_SIZE(wrapped_name) ||
	    executable_name[0] != (uint8_t)'W' ||
	    executable_name[1] != (uint8_t)'R' || executable_name[2] != 0u ||
	    slot->executable_name.bytes_including_nul !=
		ARRAY_SIZE(wrapped_name) ||
	    dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) != DOS_EXEC_TRANSACTION_OK ||
	    dos_exec_transaction_retire(&fixture.transactions,
					TEST_COORDINATOR_ID, handle) !=
		DOS_EXEC_TRANSACTION_OK)
		return 14;

	/* AL=3 uses the same A20-aware name acquisition, retains the plan, and
	 * still rejects process-only environment selection without a guest read. */
	reset_adapters();
	request = make_request(DOS_EXEC_OVERLAY);
	request.executable_name.offset = 0x0010u;
	request.executable_name.segment = 0xffffu;
	put_guest_name(request.executable_name, a20_name, ARRAY_SIZE(a20_name),
		       true);
	expected_name_length = ARRAY_SIZE(a20_name);
	if (!initialize_fixture(&fixture))
		return 15;
	fixture.machine.a20_enabled = true;
	if (dos_exec_transaction_begin(&fixture.transactions, &fixture.services,
				       &request, &handle) !=
		DOS_EXEC_TRANSACTION_OK ||
	    dos_exec_transaction_open(&fixture.transactions, handle,
				      &fixture.services, executable_name,
				      ARRAY_SIZE(executable_name), &detail) !=
		DOS_EXEC_TRANSACTION_OK)
		return 16;
	slot = &fixture.transactions.slots[transaction_slot(handle)];
	if (slot->executable_name.bytes_including_nul != ARRAY_SIZE(a20_name) ||
	    executable_name[0] != (uint8_t)'X' || executable_name[1] != 0u)
		return 17;
	is_device = BYTE_SENTINEL;
	detail = DETAIL_SENTINEL;
	if (dos_exec_transaction_probe(&fixture.transactions, handle,
				       &fixture.services, &is_device, &detail) !=
		DOS_EXEC_TRANSACTION_OK)
		return 18;
	reads_before = machine_read_calls;
	set_environment_source_sentinel(&environment_source);
	if (dos_exec_transaction_select_environment(
		&fixture.transactions, handle, &fixture.services,
		&environment_source) != DOS_EXEC_TRANSACTION_INVALID_STATE ||
	    machine_read_calls != reads_before ||
	    !environment_source_is_sentinel(&environment_source) ||
	    dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) != DOS_EXEC_TRANSACTION_OK ||
	    dos_exec_transaction_retire(&fixture.transactions,
					TEST_COORDINATOR_ID, handle) !=
		DOS_EXEC_TRANSACTION_OK)
		return 19;
	return callback_arguments_valid ? 0 : 20;
}

static int test_environment_block_success_and_reverse_abort(void)
{
	static const uint8_t environment_bytes[] = {
	    'P', 'A', 'T', 'H', '=', 'C', ':', 0u, 0u,
	};
	static const uint8_t trailer[] = {1u, 0u};
	struct test_fixture fixture;
	struct dos_exec_transaction_request request =
	    make_request(DOS_EXEC_LOAD_AND_EXECUTE);
	struct dos_exec_transaction_handle handle;
	struct dos_exec_transaction_environment result;
	struct dos_exec_transaction_environment unchanged;
	struct dos_exec_transaction_slot *slot;
	struct dos_exec_environment_source_plan source;
	struct dos_memory_lease_slot *memory_slot;
	struct dos_far_pointer16 environment_source = {
	    .offset = 0u,
	    .segment = 0x4000u,
	};
	struct dos_far_pointer16 destination;
	uint32_t abort_event;
	uint32_t memory_slot_index;
	uint32_t reads_before;
	uint32_t writes_before;

	reset_adapters();
	put_guest_word(request.parameter_block.segment,
		       request.parameter_block.offset,
		       environment_source.segment);
	put_guest_name(environment_source, environment_bytes,
		       ARRAY_SIZE(environment_bytes), false);
	if (!initialize_fixture(&fixture))
		return 1;
	/* First-fit will return head+1.  Make the unwritten paragraph tail
	 * observable instead of relying on the test image's zero fill. */
	fill_guest_bytes((struct dos_far_pointer16){
			     .offset = 0u,
			     .segment = TEST_MEMORY_HEAD + 1u,
			 },
			 0xccu, 32u);
	if (!prepare_file_probed(&fixture, &request, &handle))
		return 2;
	if (dos_exec_transaction_select_environment(
		&fixture.transactions, handle, &fixture.services,
		&source) != DOS_EXEC_TRANSACTION_OK)
		return 13;
	if (source.source.offset != 0u)
		return 14;
	if (source.source.segment != environment_source.segment)
		return 15;
	/* Explicit environment segments do not consult CurrentPDB:002ch, so the
	 * environment plan records parent_psp=0 even though the lease owner is the
	 * pinned CurrentPDB snapshot below. */
	if (source.parent_psp != 0u)
		return 16;
	if (source.kind != DOS_EXEC_ENVIRONMENT_SOURCE_PARAMETER)
		return 17;

	configure_reentry(&fixture, &request, handle,
			  REENTRY_ENVIRONMENT_PREPARE);
	set_environment_result_sentinel(&result);
	if (dos_exec_transaction_prepare_environment(
		&fixture.transactions, handle, &fixture.services,
		&result) != DOS_EXEC_TRANSACTION_OK ||
	    reentry_status != DOS_EXEC_TRANSACTION_INVALID_STATE ||
	    !callback_arguments_valid)
		return 3;
	slot = &fixture.transactions.slots[transaction_slot(handle)];
	if (slot->state !=
		DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_BLOCK_READY ||
	    result.has_block != 1u || result.plan.source.offset != 0u ||
	    result.plan.source.segment != environment_source.segment ||
	    result.plan.environment_bytes != ARRAY_SIZE(environment_bytes) ||
	    result.plan.executable_name.source.offset !=
		request.executable_name.offset ||
	    result.plan.executable_name.source.segment !=
		request.executable_name.segment ||
	    result.plan.executable_name.bytes_including_nul !=
		ARRAY_SIZE(canonical_executable_name) ||
	    result.plan.payload_bytes != 26u ||
	    result.plan.allocation_bytes != 32u ||
	    result.plan.paragraphs != 2u || result.lease.paragraphs != 2u ||
	    result.lease.guest_segment != TEST_MEMORY_HEAD + 1u ||
	    result.lease.handle.value == 0u ||
	    result.lease.handle.value == KERNEL_OBJECT_HANDLE_INVALID ||
	    result.lease.reserved != 0u ||
	    dos_memory_lease_table_is_drained(&fixture.memory_leases) ||
	    get_guest_word(TEST_MEMORY_HEAD, 1u) != 0x1234u)
		return 4;
	memory_slot_index =
	    (uint32_t)(result.lease.handle.value & DOS_MEMORY_LEASE_SLOT_MASK);
	memory_slot = &fixture.memory_leases.slots[memory_slot_index];
	if (memory_slot->state != DOS_MEMORY_LEASE_SLOT_ACTIVE ||
	    memory_slot->guest_segment != result.lease.guest_segment ||
	    memory_slot->paragraphs != result.plan.paragraphs ||
	    memory_slot->owner != 0x1234u)
		return 5;

	destination.offset = 0u;
	destination.segment = result.lease.guest_segment;
	if (!guest_bytes_equal(destination, environment_bytes,
			       ARRAY_SIZE(environment_bytes)))
		return 6;
	destination.offset = ARRAY_SIZE(environment_bytes);
	if (!guest_bytes_equal(destination, trailer, ARRAY_SIZE(trailer)))
		return 7;
	destination.offset = ARRAY_SIZE(environment_bytes) +
			     ARRAY_SIZE(trailer);
	if (!guest_bytes_equal(destination, canonical_executable_name,
			       ARRAY_SIZE(canonical_executable_name)))
		return 8;
	destination.offset = result.plan.payload_bytes;
	if (!guest_bytes_have_value(
		destination, 0xccu,
		(size_t)(result.plan.allocation_bytes -
			 result.plan.payload_bytes)))
		return 9;

	set_environment_result_sentinel(&unchanged);
	reads_before = machine_read_calls;
	writes_before = machine_write_calls;
	if (dos_exec_transaction_prepare_environment(
		&fixture.transactions, handle, &fixture.services,
		&unchanged) != DOS_EXEC_TRANSACTION_INVALID_STATE ||
	    !environment_result_is_sentinel(&unchanged) ||
	    machine_read_calls != reads_before ||
	    machine_write_calls != writes_before)
		return 10;

	verify_memory_released_before_close = 1u;
	released_mcb_segment = (uint16_t)(result.lease.guest_segment - 1u);
	abort_event = event_count;
	if (dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_OK ||
	    get_guest_word(released_mcb_segment, 1u) != 0u ||
	    slot->environment.has_block != 0u ||
	    memory_slot->state != DOS_MEMORY_LEASE_SLOT_ABORTED ||
	    !dos_memory_lease_table_is_drained(&fixture.memory_leases) ||
	    close_calls != 1u || observer_release_calls != 1u ||
	    !event_was_recorded_after(abort_event, EVENT_MACHINE_WRITE) ||
	    event_count < 2u || event_count > EVENT_CAPACITY ||
	    events[event_count - 2u] != EVENT_FILE_CLOSE ||
	    events[event_count - 1u] != EVENT_OBSERVER_RELEASE ||
	    dos_exec_transaction_retire(&fixture.transactions,
					TEST_COORDINATOR_ID,
					handle) != DOS_EXEC_TRANSACTION_OK)
		return 11;
	return callback_arguments_valid ? 0 : 12;
}

static int test_environment_failure_retains_and_retries_lease(void)
{
	static const uint8_t environment_bytes[] = {
	    'T', 'M', 'P', '=', 'C', ':', 0u, 0u,
	};
	struct test_fixture fixture;
	struct dos_exec_transaction_request request =
	    make_request(DOS_EXEC_LOAD_ONLY);
	struct dos_exec_transaction_handle handle;
	struct dos_exec_transaction_environment result;
	struct dos_exec_transaction_slot *slot;
	struct dos_exec_environment_source_plan source;
	struct dos_memory_lease_slot *memory_slot;
	struct dos_far_pointer16 environment_source = {
	    .offset = 0u,
	    .segment = 0x4100u,
	};
	uint32_t memory_slot_index;

	reset_adapters();
	put_guest_word(request.parameter_block.segment,
		       request.parameter_block.offset,
		       environment_source.segment);
	put_guest_name(environment_source, environment_bytes,
		       ARRAY_SIZE(environment_bytes), false);
	if (!initialize_fixture(&fixture) ||
	    !prepare_file_probed(&fixture, &request, &handle) ||
	    dos_exec_transaction_select_environment(
		&fixture.transactions, handle, &fixture.services,
		&source) != DOS_EXEC_TRANSACTION_OK)
		return 1;

	/* Name validation already succeeded at OPEN. An early NUL at the later
	 * argv[0] copy point must fail after allocation without
	 * publishing the result or losing the rollback capability. */
	guest_memory[TEST_EXECUTABLE_NAME_LINEAR] = 0u;
	set_environment_result_sentinel(&result);
	if (dos_exec_transaction_prepare_environment(
		&fixture.transactions, handle, &fixture.services,
		&result) != DOS_EXEC_TRANSACTION_BAD_ENVIRONMENT ||
	    !environment_result_is_sentinel(&result))
		return 2;
	slot = &fixture.transactions.slots[transaction_slot(handle)];
	if (slot->state != DOS_EXEC_TRANSACTION_STATE_FAILED ||
	    slot->environment.has_block != 1u ||
	    slot->environment.plan.environment_bytes !=
		ARRAY_SIZE(environment_bytes) ||
	    slot->environment.lease.handle.value == 0u ||
	    dos_memory_lease_table_is_drained(&fixture.memory_leases) ||
	    get_guest_word(
		(uint16_t)(slot->environment.lease.guest_segment - 1u),
		1u) != 0x1234u)
		return 3;
	memory_slot_index = (uint32_t)(slot->environment.lease.handle.value &
				       DOS_MEMORY_LEASE_SLOT_MASK);
	memory_slot = &fixture.memory_leases.slots[memory_slot_index];
	if (memory_slot->state != DOS_MEMORY_LEASE_SLOT_ACTIVE)
		return 4;

	verify_memory_released_before_close = 1u;
	released_mcb_segment =
	    (uint16_t)(slot->environment.lease.guest_segment - 1u);
	fail_machine_read_call = machine_read_calls + 1u;
	if (dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_MEMORY_LEASE_RETAINED ||
	    slot->state != DOS_EXEC_TRANSACTION_STATE_ABORTING ||
	    slot->environment.has_block != 1u ||
	    memory_slot->state != DOS_MEMORY_LEASE_SLOT_ACTIVE ||
	    get_guest_word(released_mcb_segment, 1u) != 0x1234u ||
	    close_calls != 0u || observer_release_calls != 0u)
		return 5;

	fail_machine_read_call = 0u;
	if (dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_OK ||
	    slot->state != DOS_EXEC_TRANSACTION_STATE_ABORTED ||
	    slot->environment.has_block != 0u ||
	    memory_slot->state != DOS_MEMORY_LEASE_SLOT_ABORTED ||
	    get_guest_word(released_mcb_segment, 1u) != 0u ||
	    !dos_memory_lease_table_is_drained(&fixture.memory_leases) ||
	    close_calls != 1u || observer_release_calls != 1u ||
	    event_count < 2u || event_count > EVENT_CAPACITY ||
	    events[event_count - 2u] != EVENT_FILE_CLOSE ||
	    events[event_count - 1u] != EVENT_OBSERVER_RELEASE ||
	    dos_exec_transaction_retire(&fixture.transactions,
					TEST_COORDINATOR_ID,
					handle) != DOS_EXEC_TRANSACTION_OK)
		return 6;
	return callback_arguments_valid ? 0 : 7;
}

static int test_environment_none_preflight_and_allocation_guards(void)
{
	static const uint8_t environment_bytes[] = {
	    'A', '=', 'B', 0u, 0u,
	};
	struct test_fixture fixture;
	struct dos_exec_transaction_request request;
	struct dos_exec_transaction_handle handle;
	struct dos_exec_transaction_environment result;
	struct dos_exec_transaction_slot *slot;
	struct dos_exec_environment_source_plan source;
	struct dos_exec_transaction_services wrong_services;
	struct dos_memory_lease_table other_memory_leases;
	struct dos_far_pointer16 environment_source = {
	    .offset = 0u,
	    .segment = 0x4200u,
	};
	uint32_t reads_before;
	uint32_t writes_before;

	/* A genuine NONE environment selection publishes all-zero state and performs
	 * no environment scan or MCB operation. */
	reset_adapters();
	request = make_request(DOS_EXEC_LOAD_AND_EXECUTE);
	put_guest_word(request.parameter_block.segment,
		       request.parameter_block.offset, 0u);
	put_guest_word(0u, 0x002cu, 0u);
	if (!initialize_fixture(&fixture) ||
	    dos_process_runtime_set_current_psp(&fixture.runtime, 0u) !=
		DOS_PROCESS_RUNTIME_OK ||
	    !prepare_file_probed(&fixture, &request, &handle) ||
	    dos_exec_transaction_select_environment(
		&fixture.transactions, handle, &fixture.services,
		&source) != DOS_EXEC_TRANSACTION_OK ||
	    source.kind != DOS_EXEC_ENVIRONMENT_SOURCE_NONE)
		return 1;
	set_environment_result_sentinel(&result);
	reads_before = machine_read_calls;
	writes_before = machine_write_calls;
	if (dos_exec_transaction_prepare_environment(
		&fixture.transactions, handle, &fixture.services,
		&result) != DOS_EXEC_TRANSACTION_OK ||
	    !environment_result_is_zero(&result) ||
	    machine_read_calls != reads_before ||
	    machine_write_calls != writes_before ||
	    !dos_memory_lease_table_is_drained(&fixture.memory_leases) ||
	    fixture.transactions.slots[transaction_slot(handle)].state !=
		DOS_EXEC_TRANSACTION_STATE_ENVIRONMENT_BLOCK_READY ||
	    dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_OK ||
	    dos_exec_transaction_retire(&fixture.transactions,
					TEST_COORDINATOR_ID,
					handle) != DOS_EXEC_TRANSACTION_OK)
		return 2;

	/* An explicit nonzero environment with CurrentPDB zero would create an
	 * MCB whose DOS owner value means free.  Reject it before scanning or
	 * allocating, leaving the caller's result untouched. */
	reset_adapters();
	request = make_request(DOS_EXEC_LOAD_ONLY);
	put_guest_word(request.parameter_block.segment,
		       request.parameter_block.offset,
		       environment_source.segment);
	put_guest_name(environment_source, environment_bytes,
		       ARRAY_SIZE(environment_bytes), false);
	if (!initialize_fixture(&fixture) ||
	    dos_process_runtime_set_current_psp(&fixture.runtime, 0u) !=
		DOS_PROCESS_RUNTIME_OK ||
	    !prepare_file_probed(&fixture, &request, &handle) ||
	    dos_exec_transaction_select_environment(
		&fixture.transactions, handle, &fixture.services,
		&source) != DOS_EXEC_TRANSACTION_OK)
		return 3;
	set_environment_result_sentinel(&result);
	reads_before = machine_read_calls;
	writes_before = machine_write_calls;
	if (dos_exec_transaction_prepare_environment(
		&fixture.transactions, handle, &fixture.services,
		&result) != DOS_EXEC_TRANSACTION_RUNTIME_NOT_READY ||
	    !environment_result_is_sentinel(&result) ||
	    machine_read_calls != reads_before ||
	    machine_write_calls != writes_before ||
	    !dos_memory_lease_table_is_drained(&fixture.memory_leases))
		return 4;
	slot = &fixture.transactions.slots[transaction_slot(handle)];
	if (slot->state != DOS_EXEC_TRANSACTION_STATE_FAILED ||
	    slot->environment.has_block != 0u ||
	    dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_OK ||
	    dos_exec_transaction_retire(&fixture.transactions,
					TEST_COORDINATOR_ID,
					handle) != DOS_EXEC_TRANSACTION_OK)
		return 5;

	/* A generation-pinned memory lease table is part of the process EXEC
	 * binding.  A different, otherwise valid table fails before guest I/O. */
	reset_adapters();
	request = make_request(DOS_EXEC_LOAD_AND_EXECUTE);
	put_guest_word(request.parameter_block.segment,
		       request.parameter_block.offset,
		       environment_source.segment);
	put_guest_name(environment_source, environment_bytes,
		       ARRAY_SIZE(environment_bytes), false);
	if (!initialize_fixture(&fixture) ||
	    !prepare_file_probed(&fixture, &request, &handle) ||
	    dos_exec_transaction_select_environment(
		&fixture.transactions, handle, &fixture.services,
		&source) != DOS_EXEC_TRANSACTION_OK ||
	    dos_memory_lease_table_construct(
		&other_memory_leases, TEST_MEMORY_TABLE_ID + 1u) !=
		DOS_MEMORY_LEASE_OK ||
	    dos_memory_lease_table_initialize(&other_memory_leases) !=
		DOS_MEMORY_LEASE_OK)
		return 6;
	wrong_services = fixture.services;
	wrong_services.memory_leases = &other_memory_leases;
	wrong_services.memory_lease_table_identity = TEST_MEMORY_TABLE_ID + 1u;
	set_environment_result_sentinel(&result);
	reads_before = machine_read_calls;
	writes_before = machine_write_calls;
	if (dos_exec_transaction_prepare_environment(
		&fixture.transactions, handle, &wrong_services,
		&result) != DOS_EXEC_TRANSACTION_BINDING_MISMATCH ||
	    !environment_result_is_sentinel(&result) ||
	    machine_read_calls != reads_before ||
	    machine_write_calls != writes_before ||
	    fixture.transactions.slots[transaction_slot(handle)].state !=
		DOS_EXEC_TRANSACTION_STATE_ENV_READY ||
	    dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_OK ||
	    dos_exec_transaction_retire(&fixture.transactions,
					TEST_COORDINATOR_ID,
					handle) != DOS_EXEC_TRANSACTION_OK)
		return 7;

	/* The scan may succeed while the parent arena cannot satisfy the exact
	 * paragraph plan.  No lease or result is published in that case. */
	reset_adapters();
	request = make_request(DOS_EXEC_LOAD_ONLY);
	put_guest_word(request.parameter_block.segment,
		       request.parameter_block.offset,
		       environment_source.segment);
	put_guest_name(environment_source, environment_bytes,
		       ARRAY_SIZE(environment_bytes), false);
	if (!initialize_fixture(&fixture) ||
	    !prepare_file_probed(&fixture, &request, &handle) ||
	    dos_exec_transaction_select_environment(
		&fixture.transactions, handle, &fixture.services,
		&source) != DOS_EXEC_TRANSACTION_OK)
		return 8;
	/* One paragraph cannot hold the two-paragraph environment plan. */
	put_guest_word(TEST_MEMORY_HEAD, 3u, 1u);
	set_environment_result_sentinel(&result);
	if (dos_exec_transaction_prepare_environment(
		&fixture.transactions, handle, &fixture.services,
		&result) != DOS_EXEC_TRANSACTION_NOT_ENOUGH_MEMORY ||
	    !environment_result_is_sentinel(&result) ||
	    !dos_memory_lease_table_is_drained(&fixture.memory_leases))
		return 9;
	slot = &fixture.transactions.slots[transaction_slot(handle)];
	if (slot->state != DOS_EXEC_TRANSACTION_STATE_FAILED ||
	    slot->environment.has_block != 0u ||
	    slot->environment.plan.paragraphs != 2u ||
	    dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_OK ||
	    dos_exec_transaction_retire(&fixture.transactions,
					TEST_COORDINATOR_ID,
					handle) != DOS_EXEC_TRANSACTION_OK)
		return 10;
	return callback_arguments_valid ? 0 : 11;
}

static int test_image_inspection_process_and_overlay(void)
{
	struct test_fixture fixture;
	struct dos_exec_transaction_request request;
	struct dos_exec_transaction_handle handle;
	struct dos_exec_transaction_slot *slot;
	struct dos_exec_environment_source_plan source;
	struct dos_exec_transaction_environment environment;
	struct dos_load_plan plan;
	uint32_t reads_before;

	/* EXEC0/1 cannot reach exec_read_header until the ordered environment
	 * transition has completed, even when the selected result is NONE. */
	reset_adapters();
	configure_mz_file();
	request = make_request(DOS_EXEC_LOAD_AND_EXECUTE);
	put_guest_word(request.parameter_block.segment,
		       request.parameter_block.offset, 0u);
	put_guest_word(0x1234u, 0x002cu, 0u);
	if (!initialize_fixture(&fixture) ||
	    !prepare_file_probed(&fixture, &request, &handle))
		return 1;
	set_load_plan_sentinel(&plan);
	if (dos_exec_transaction_inspect_image(
		&fixture.transactions, handle, &fixture.services,
		&plan) != DOS_EXEC_TRANSACTION_INVALID_STATE ||
	    !load_plan_is_sentinel(&plan) || read_calls != 0u)
		return 2;
	if (dos_exec_transaction_select_environment(
		&fixture.transactions, handle, &fixture.services,
		&source) != DOS_EXEC_TRANSACTION_OK ||
	    source.kind != DOS_EXEC_ENVIRONMENT_SOURCE_NONE ||
	    dos_exec_transaction_prepare_environment(
		&fixture.transactions, handle, &fixture.services,
		&environment) != DOS_EXEC_TRANSACTION_OK ||
	    !environment_result_is_zero(&environment))
		return 3;

	configure_reentry(&fixture, &request, handle, REENTRY_IMAGE_READ);
	set_load_plan_sentinel(&plan);
	if (dos_exec_transaction_inspect_image(
		&fixture.transactions, handle, &fixture.services,
		&plan) != DOS_EXEC_TRANSACTION_OK ||
	    reentry_status != DOS_EXEC_TRANSACTION_INVALID_STATE ||
	    read_calls != 1u || !callback_arguments_valid ||
	    !dos_load_plan_has_inspected_encoding(&plan) ||
	    plan.format != (uint8_t)DOS_IMAGE_MZ ||
	    plan.target_kind != (uint8_t)DOS_LOAD_TARGET_PROCESS ||
	    plan.file_size != 512u || plan.image_file_offset != 32u ||
	    plan.image_size != 30u * 16u ||
	    plan.minimum_image_paragraphs != 30u ||
	    plan.minimum_extra_paragraphs != 1u ||
	    plan.maximum_extra_paragraphs != 0xffffu ||
	    plan.initial_cs != 0x10u || plan.initial_ip != 0x100u ||
	    plan.initial_ss != 0x20u || plan.initial_sp != 0xfffeu ||
	    plan.relocation_count != 2u ||
	    plan.relocation_table_offset != 0x1cu)
		return 4;
	slot = &fixture.transactions.slots[transaction_slot(handle)];
	if (slot->state != DOS_EXEC_TRANSACTION_STATE_IMAGE_READY ||
	    slot->image.has_plan != 1u ||
	    !dos_load_plan_has_inspected_encoding(&slot->image.plan) ||
	    slot->image.plan.file_size != plan.file_size)
		return 5;
	reads_before = read_calls;
	set_load_plan_sentinel(&plan);
	if (dos_exec_transaction_inspect_image(
		&fixture.transactions, handle, &fixture.services,
		&plan) != DOS_EXEC_TRANSACTION_INVALID_STATE ||
	    !load_plan_is_sentinel(&plan) || read_calls != reads_before ||
	    dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_OK)
		return 6;
	if (slot->state != DOS_EXEC_TRANSACTION_STATE_ABORTED ||
	    slot->image.has_plan != 1u ||
	    dos_exec_transaction_retire(&fixture.transactions,
					TEST_COORDINATOR_ID,
					handle) != DOS_EXEC_TRANSACTION_OK ||
	    slot->image.has_plan != 0u || !load_plan_is_zero(&slot->image.plan))
		return 7;

	/* EXEC3 skips every process-only binding and environment operation, then
	 * classifies the same immutable file reader for its overlay target. */
	reset_adapters();
	configured_open_size = 3u;
	configured_read_status = DOS_IMAGE_READ_OK;
	file_image_readable_size = 3u;
	file_image[0] = 0x90u;
	request = make_request(DOS_EXEC_OVERLAY);
	if (!initialize_fixture(&fixture) ||
	    !prepare_file_probed(&fixture, &request, &handle))
		return 8;
	set_load_plan_sentinel(&plan);
	if (dos_exec_transaction_inspect_image(
		&fixture.transactions, handle, &fixture.services,
		&plan) != DOS_EXEC_TRANSACTION_OK ||
	    !dos_load_plan_has_inspected_encoding(&plan) ||
	    plan.format != (uint8_t)DOS_IMAGE_COM ||
	    plan.target_kind != (uint8_t)DOS_LOAD_TARGET_OVERLAY ||
	    plan.file_size != 3u || plan.image_size != 3u ||
	    plan.initial_ip != 0x100u || read_calls != 1u ||
	    machine_read_calls != 0u)
		return 9;
	slot = &fixture.transactions.slots[transaction_slot(handle)];
	if (slot->state != DOS_EXEC_TRANSACTION_STATE_IMAGE_READY ||
	    slot->image.has_plan != 1u ||
	    slot->environment_source.source.segment != 0u ||
	    slot->environment.has_block != 0u ||
	    dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_OK ||
	    dos_exec_transaction_retire(&fixture.transactions,
					TEST_COORDINATOR_ID,
					handle) != DOS_EXEC_TRANSACTION_OK)
		return 10;
	return callback_arguments_valid ? 0 : 11;
}

static int test_image_inspection_failures_are_reversible(void)
{
	struct test_fixture fixture;
	struct dos_exec_transaction_request request;
	struct dos_exec_transaction_handle handle;
	struct dos_exec_transaction_slot *slot;
	struct dos_load_plan plan;

	/* OR AX,AX after ExecRead makes a zero-length image bad format without a
	 * file read callback. */
	reset_adapters();
	configured_open_size = 0u;
	configured_read_status = DOS_IMAGE_READ_OK;
	request = make_request(DOS_EXEC_OVERLAY);
	if (!initialize_fixture(&fixture) ||
	    !prepare_file_probed(&fixture, &request, &handle))
		return 1;
	set_load_plan_sentinel(&plan);
	if (dos_exec_transaction_inspect_image(
		&fixture.transactions, handle, &fixture.services,
		&plan) != DOS_EXEC_TRANSACTION_BAD_IMAGE ||
	    !load_plan_is_sentinel(&plan) || read_calls != 0u)
		return 2;
	slot = &fixture.transactions.slots[transaction_slot(handle)];
	if (slot->state != DOS_EXEC_TRANSACTION_STATE_FAILED ||
	    slot->image.has_plan != 0u ||
	    dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_OK ||
	    dos_exec_transaction_retire(&fixture.transactions,
					TEST_COORDINATOR_ID,
					handle) != DOS_EXEC_TRANSACTION_OK)
		return 3;

	/* Header I/O failure is mapped to EXEC's bad-format path and retains the
	 * open file lease for ordinary abort. */
	reset_adapters();
	configured_open_size = DOS_EXEC_PRIVATE_MZ_HEADER_BYTES;
	configured_read_status = DOS_IMAGE_READ_IO_ERROR;
	file_image_readable_size = ARRAY_SIZE(file_image);
	request = make_request(DOS_EXEC_OVERLAY);
	if (!initialize_fixture(&fixture) ||
	    !prepare_file_probed(&fixture, &request, &handle))
		return 4;
	set_load_plan_sentinel(&plan);
	if (dos_exec_transaction_inspect_image(
		&fixture.transactions, handle, &fixture.services,
		&plan) != DOS_EXEC_TRANSACTION_BAD_IMAGE ||
	    !load_plan_is_sentinel(&plan) || read_calls != 1u ||
	    dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_OK ||
	    dos_exec_transaction_retire(&fixture.transactions,
					TEST_COORDINATOR_ID,
					handle) != DOS_EXEC_TRANSACTION_OK)
		return 5;

	/* A successful adapter that returns fewer bytes than its immutable size
	 * is not treated as a mutable-EOF COM file.  This documented safety
	 * deviation fails closed and leaves the public plan untouched. */
	reset_adapters();
	configured_open_size = DOS_EXEC_PRIVATE_MZ_HEADER_BYTES;
	configured_read_status = DOS_IMAGE_READ_OK;
	configured_short_read = 1u;
	file_image_readable_size = ARRAY_SIZE(file_image);
	request = make_request(DOS_EXEC_OVERLAY);
	if (!initialize_fixture(&fixture) ||
	    !prepare_file_probed(&fixture, &request, &handle))
		return 6;
	set_load_plan_sentinel(&plan);
	if (dos_exec_transaction_inspect_image(
		&fixture.transactions, handle, &fixture.services,
		&plan) != DOS_EXEC_TRANSACTION_BAD_IMAGE ||
	    !load_plan_is_sentinel(&plan) || read_calls != 1u ||
	    dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_OK ||
	    dos_exec_transaction_retire(&fixture.transactions,
					TEST_COORDINATOR_ID,
					handle) != DOS_EXEC_TRANSACTION_OK)
		return 7;

	/* EXEC3's FFFFh request returning exactly FFFFh reaches exec_no_mem. */
	reset_adapters();
	configured_open_size = 0xffffu;
	configured_read_status = DOS_IMAGE_READ_OK;
	file_image_readable_size = ARRAY_SIZE(file_image);
	file_image[0] = 0x90u;
	request = make_request(DOS_EXEC_OVERLAY);
	if (!initialize_fixture(&fixture) ||
	    !prepare_file_probed(&fixture, &request, &handle))
		return 8;
	set_load_plan_sentinel(&plan);
	if (dos_exec_transaction_inspect_image(
		&fixture.transactions, handle, &fixture.services,
		&plan) != DOS_EXEC_TRANSACTION_IMAGE_TOO_LARGE ||
	    !load_plan_is_sentinel(&plan) || read_calls != 1u ||
	    dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_OK ||
	    dos_exec_transaction_retire(&fixture.transactions,
					TEST_COORDINATOR_ID,
					handle) != DOS_EXEC_TRANSACTION_OK)
		return 9;

	/* Revalidate the pinned CurrentPDB generation before the file callback.
	 * A changed runtime leaves the prepared NONE environment abortable. */
	reset_adapters();
	configured_open_size = 3u;
	configured_read_status = DOS_IMAGE_READ_OK;
	file_image_readable_size = 3u;
	file_image[0] = 0x90u;
	request = make_request(DOS_EXEC_LOAD_ONLY);
	if (!initialize_fixture(&fixture) ||
	    !prepare_process_without_environment(&fixture, &request, &handle) ||
	    dos_process_runtime_set_current_psp(&fixture.runtime, 0x4321u) !=
		DOS_PROCESS_RUNTIME_OK)
		return 10;
	set_load_plan_sentinel(&plan);
	if (dos_exec_transaction_inspect_image(
		&fixture.transactions, handle, &fixture.services,
		&plan) != DOS_EXEC_TRANSACTION_RUNTIME_NOT_READY ||
	    !load_plan_is_sentinel(&plan) || read_calls != 0u ||
	    fixture.transactions.slots[transaction_slot(handle)].state !=
		DOS_EXEC_TRANSACTION_STATE_FAILED ||
	    dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_OK ||
	    dos_exec_transaction_retire(&fixture.transactions,
					TEST_COORDINATOR_ID,
					handle) != DOS_EXEC_TRANSACTION_OK)
		return 11;
	return callback_arguments_valid ? 0 : 12;
}

static int test_process_target_allocation_and_reverse_abort(void)
{
	struct test_fixture fixture;
	struct dos_exec_transaction_request request =
	    make_request(DOS_EXEC_LOAD_AND_EXECUTE);
	struct dos_exec_transaction_handle handle;
	struct dos_exec_transaction_slot *slot;
	struct dos_exec_transaction_target target;
	uint32_t reads_before_abort;

	reset_adapters();
	if (!initialize_fixture(&fixture) ||
	    !prepare_process_image(&fixture, &request, &handle))
		return 1;
	configure_reentry(&fixture, &request, handle,
			  REENTRY_TARGET_PREPARE);
	set_target_result_sentinel(&target);
	if (dos_exec_transaction_prepare_target(
		&fixture.transactions, handle, &fixture.services,
		&target) != DOS_EXEC_TRANSACTION_OK ||
	    reentry_status != DOS_EXEC_TRANSACTION_INVALID_STATE ||
	    !callback_arguments_valid ||
	    target.allocation.format != (uint8_t)DOS_IMAGE_MZ ||
	    target.allocation.load_high != 0u ||
	    target.allocation.available_paragraphs !=
		TEST_INITIAL_FREE_PARAGRAPHS ||
	    target.allocation.block_paragraphs !=
		TEST_INITIAL_FREE_PARAGRAPHS ||
	    target.lease.guest_segment != TEST_MEMORY_HEAD + 1u ||
	    target.lease.paragraphs != TEST_INITIAL_FREE_PARAGRAPHS ||
	    target.lease.maximum_available != TEST_INITIAL_FREE_PARAGRAPHS ||
	    target.lease.handle.value == 0u ||
	    target.lease.handle.value == KERNEL_OBJECT_HANDLE_INVALID ||
	    target.has_load_block != 1u ||
	    get_guest_word(TEST_MEMORY_HEAD, 1u) != 0x1234u)
		return 2;
	slot = &fixture.transactions.slots[transaction_slot(handle)];
	if (slot->state != DOS_EXEC_TRANSACTION_STATE_TARGET_READY ||
	    slot->target.has_load_block != 1u ||
	    slot->target.lease.handle.value != target.lease.handle.value ||
	    slot->target.allocation.block_paragraphs !=
		TEST_INITIAL_FREE_PARAGRAPHS)
		return 3;

	set_target_result_sentinel(&target);
	if (dos_exec_transaction_prepare_target(
		&fixture.transactions, handle, &fixture.services,
		&target) != DOS_EXEC_TRANSACTION_INVALID_STATE ||
	    !target_result_is_sentinel(&target))
		return 4;

	/* A fault while releasing the newest lease retains it for retry and
	 * cannot close the executable before both MCB leases are gone. */
	reads_before_abort = machine_read_calls;
	fail_machine_read_call = reads_before_abort + 1u;
	if (dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_MEMORY_LEASE_RETAINED ||
	    slot->state != DOS_EXEC_TRANSACTION_STATE_ABORTING ||
	    slot->target.has_load_block != 1u || close_calls != 0u)
		return 5;
	fail_machine_read_call = 0u;
	released_mcb_segment = TEST_MEMORY_HEAD;
	verify_memory_released_before_close = 1u;
	if (dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_OK ||
	    slot->state != DOS_EXEC_TRANSACTION_STATE_ABORTED ||
	    slot->target.has_load_block != 0u ||
	    get_guest_word(TEST_MEMORY_HEAD, 1u) != 0u ||
	    !dos_memory_lease_table_is_drained(&fixture.memory_leases) ||
	    close_calls != 1u || observer_release_calls != 1u)
		return 6;
	if (dos_exec_transaction_retire(&fixture.transactions,
					TEST_COORDINATOR_ID,
					handle) != DOS_EXEC_TRANSACTION_OK ||
	    slot->target.allocation.available_paragraphs != 0u ||
	    slot->target.lease.handle.value != 0u)
		return 7;
	return 0;
}

static int test_process_target_failures_are_reversible(void)
{
	struct test_fixture fixture;
	struct dos_exec_transaction_request request;
	struct dos_exec_transaction_handle handle;
	struct dos_exec_transaction_slot *slot;
	struct dos_exec_transaction_target target;
	struct dos_load_plan image;
	uint32_t reads_before;

	/* EXEC3 owns no process arena and must not query it. */
	reset_adapters();
	configure_mz_file();
	request = make_request(DOS_EXEC_OVERLAY);
	if (!initialize_fixture(&fixture) ||
	    !prepare_file_probed(&fixture, &request, &handle) ||
	    dos_exec_transaction_inspect_image(
		&fixture.transactions, handle, &fixture.services,
		&image) != DOS_EXEC_TRANSACTION_OK)
		return 1;
	reads_before = machine_read_calls;
	set_target_result_sentinel(&target);
	if (dos_exec_transaction_prepare_target(
		&fixture.transactions, handle, &fixture.services,
		&target) != DOS_EXEC_TRANSACTION_INVALID_STATE ||
	    !target_result_is_sentinel(&target) ||
	    machine_read_calls != reads_before ||
	    dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_OK ||
	    dos_exec_transaction_retire(&fixture.transactions,
					TEST_COORDINATOR_ID,
					handle) != DOS_EXEC_TRANSACTION_OK)
		return 2;

	/* The BX=ffffh largest-block probe can succeed with a block too small for
	 * PSP plus resident image; no allocation is attempted. */
	reset_adapters();
	request = make_request(DOS_EXEC_LOAD_ONLY);
	if (!initialize_fixture(&fixture) ||
	    !prepare_process_image(&fixture, &request, &handle))
		return 3;
	put_guest_word(TEST_MEMORY_HEAD, 3u, TEST_PSP_PARAGRAPHS);
	set_target_result_sentinel(&target);
	if (dos_exec_transaction_prepare_target(
		&fixture.transactions, handle, &fixture.services,
		&target) != DOS_EXEC_TRANSACTION_NOT_ENOUGH_MEMORY ||
	    !target_result_is_sentinel(&target))
		return 4;
	slot = &fixture.transactions.slots[transaction_slot(handle)];
	if (slot->state != DOS_EXEC_TRANSACTION_STATE_FAILED ||
	    slot->target.has_load_block != 0u ||
	    slot->target.allocation.available_paragraphs != 0u ||
	    dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_OK ||
	    dos_exec_transaction_retire(&fixture.transactions,
					TEST_COORDINATOR_ID,
					handle) != DOS_EXEC_TRANSACTION_OK)
		return 5;

	/* A stale CurrentPDB snapshot is rejected before the first arena read. */
	reset_adapters();
	if (!initialize_fixture(&fixture) ||
	    !prepare_process_image(&fixture, &request, &handle) ||
	    dos_process_runtime_set_current_psp(&fixture.runtime, 0x4321u) !=
		DOS_PROCESS_RUNTIME_OK)
		return 6;
	reads_before = machine_read_calls;
	set_target_result_sentinel(&target);
	if (dos_exec_transaction_prepare_target(
		&fixture.transactions, handle, &fixture.services,
		&target) != DOS_EXEC_TRANSACTION_RUNTIME_NOT_READY ||
	    !target_result_is_sentinel(&target) ||
	    machine_read_calls != reads_before ||
	    dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_OK ||
	    dos_exec_transaction_retire(&fixture.transactions,
					TEST_COORDINATOR_ID,
					handle) != DOS_EXEC_TRANSACTION_OK)
		return 7;

	/* A query fault publishes neither a plan nor a lease. */
	reset_adapters();
	if (!initialize_fixture(&fixture) ||
	    !prepare_process_image(&fixture, &request, &handle))
		return 8;
	fail_machine_read_call = machine_read_calls + 1u;
	set_target_result_sentinel(&target);
	if (dos_exec_transaction_prepare_target(
		&fixture.transactions, handle, &fixture.services,
		&target) != DOS_EXEC_TRANSACTION_MEMORY_FAULT ||
	    !target_result_is_sentinel(&target))
		return 9;
	fail_machine_read_call = 0u;
	slot = &fixture.transactions.slots[transaction_slot(handle)];
	if (slot->target.has_load_block != 0u ||
	    slot->target.allocation.available_paragraphs != 0u ||
	    dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_OK ||
	    dos_exec_transaction_retire(&fixture.transactions,
					TEST_COORDINATOR_ID,
					handle) != DOS_EXEC_TRANSACTION_OK)
		return 10;

	/* A later allocation read fault retains the already selected exact
	 * paragraph plan but never claims a lease. */
	reset_adapters();
	if (!initialize_fixture(&fixture) ||
	    !prepare_process_image(&fixture, &request, &handle))
		return 11;
	fail_machine_read_call = machine_read_calls + 2u;
	set_target_result_sentinel(&target);
	if (dos_exec_transaction_prepare_target(
		&fixture.transactions, handle, &fixture.services,
		&target) != DOS_EXEC_TRANSACTION_MEMORY_FAULT ||
	    !target_result_is_sentinel(&target))
		return 12;
	fail_machine_read_call = 0u;
	slot = &fixture.transactions.slots[transaction_slot(handle)];
	if (slot->state != DOS_EXEC_TRANSACTION_STATE_FAILED ||
	    slot->target.has_load_block != 0u ||
	    slot->target.allocation.available_paragraphs !=
		TEST_INITIAL_FREE_PARAGRAPHS ||
	    slot->target.allocation.block_paragraphs !=
		TEST_INITIAL_FREE_PARAGRAPHS ||
	    dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_OK ||
	    dos_exec_transaction_retire(&fixture.transactions,
					TEST_COORDINATOR_ID,
					handle) != DOS_EXEC_TRANSACTION_OK)
		return 13;
	return callback_arguments_valid ? 0 : 14;
}

static int test_process_resident_loads_and_reverse_abort(void)
{
	struct test_fixture fixture;
	struct dos_exec_transaction_request request;
	struct dos_exec_transaction_handle handle;
	struct dos_exec_transaction_slot *slot;
	struct dos_exec_transaction_target target;
	struct dos_exec_transaction_resident resident;
	struct dos_exec_transaction_relocation relocation;
	struct dos_exec_transaction_parent parent;
	struct dos_exec_transaction_inheritance inheritance;
	struct dos_exec_transaction_psp psp;
	struct dos_process_far_address terminate_vector = {
	    .segment = 0x7777u,
	    .offset = 0x8888u,
	};
	dos_linear_address_t load_linear;
	dos_linear_address_t psp_linear;
	uint32_t reads_before;
	uint32_t writes_before;
	size_t index;

	/* MZ follows exec_big_read's private load block and retains relocation
	 * metadata without applying relocation words early. */
	reset_adapters();
	request = make_request(DOS_EXEC_LOAD_AND_EXECUTE);
	if (!initialize_fixture(&fixture) ||
	    !prepare_process_image(&fixture, &request, &handle) ||
	    dos_exec_transaction_prepare_target(
		&fixture.transactions, handle, &fixture.services,
		&target) != DOS_EXEC_TRANSACTION_OK)
		return 1;
	configure_reentry(&fixture, &request, handle, REENTRY_RESIDENT_LOAD);
	set_resident_result_sentinel(&resident);
	if (dos_exec_transaction_load_resident(
		&fixture.transactions, handle, &fixture.services,
		&resident) != DOS_EXEC_TRANSACTION_OK ||
	    reentry_status != DOS_EXEC_TRANSACTION_INVALID_STATE ||
	    !callback_arguments_valid ||
	    resident.format != (uint8_t)DOS_IMAGE_MZ ||
	    resident.has_process_plan != 1u || resident.has_resident != 1u ||
	    resident.process.mz.psp_segment != target.lease.guest_segment ||
	    resident.process.mz.launch_mode !=
		(uint8_t)DOS_PROCESS_LAUNCH_EXECUTE ||
	    resident.process.mz.initial_state.eax != 0u ||
	    resident.process.mz.initial_state.ebx != 0u ||
	    resident.process.mz.relocation_count != 2u ||
	    resident.load.lease_handle != target.lease.handle.value ||
	    resident.load.file_bytes_written != 480u ||
	    resident.load.resident_bytes != 480u ||
	    resident.load.untouched_bytes != 0u || read_calls != 3u)
		return 2;
	load_linear = resident.process.mz.load_linear_address;
	if (guest_memory[(size_t)load_linear] != file_image[32u] ||
	    guest_memory[(size_t)load_linear + 255u] != file_image[287u] ||
	    guest_memory[(size_t)load_linear + 479u] != file_image[511u])
		return 3;
	slot = &fixture.transactions.slots[transaction_slot(handle)];
	if (slot->state != DOS_EXEC_TRANSACTION_STATE_RESIDENT_READY ||
	    slot->resident.has_resident != 1u ||
	    slot->resident.load.lease_handle != target.lease.handle.value)
		return 4;
	configure_reentry(&fixture, &request, handle, REENTRY_RELOCATE);
	set_relocation_result_sentinel(&relocation);
	if (dos_exec_transaction_relocate_resident(
		&fixture.transactions, handle, &fixture.services,
		&relocation) != DOS_EXEC_TRANSACTION_OK ||
	    reentry_status != DOS_EXEC_TRANSACTION_INVALID_STATE ||
	    relocation.applicable != 1u || relocation.has_request != 1u ||
	    relocation.applied != 1u ||
	    relocation.request.relocation_table_offset != 0x1cu ||
	    relocation.request.resident_size != 480u ||
	    relocation.request.resident_linear_address != load_linear ||
	    relocation.request.relocation_count != 2u ||
	    relocation.request.relocation_factor !=
		resident.process.mz.load_segment ||
	    relocation.result.validated_entries != 2u ||
	    relocation.result.applied_entries != 2u || read_calls != 5u ||
	    get_guest_word(resident.process.mz.load_segment, 0u) != 0x5042u ||
	    get_guest_word(resident.process.mz.load_segment, 0x31u) != 0x5011u ||
	    slot->state != DOS_EXEC_TRANSACTION_STATE_IMAGE_PRIVATE)
		return 5;
	set_relocation_result_sentinel(&relocation);
	if (dos_exec_transaction_relocate_resident(
		&fixture.transactions, handle, &fixture.services,
		&relocation) != DOS_EXEC_TRANSACTION_INVALID_STATE ||
	    !relocation_result_is_sentinel(&relocation))
		return 6;
	set_resident_result_sentinel(&resident);
	if (dos_exec_transaction_load_resident(
		&fixture.transactions, handle, &fixture.services,
		&resident) != DOS_EXEC_TRANSACTION_INVALID_STATE ||
	    !resident_result_is_sentinel(&resident))
		return 7;
	configure_reentry(&fixture, &request, handle, REENTRY_FILE_CLOSE);
	if (dos_exec_transaction_close(&fixture.transactions, handle,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_OK ||
	    reentry_status != DOS_EXEC_TRANSACTION_INVALID_STATE ||
	    close_calls != 1u ||
	    slot->state != DOS_EXEC_TRANSACTION_STATE_PROCESS_FILE_CLOSED)
		return 8;
	guest_memory[(size_t)dos_far_to_linear(0x1234u, 0u, false)] = 0xcdu;
	put_guest_word(
	    0x1234u,
	    (uint16_t)__builtin_offsetof(struct dos_psp_prefix40, jft_length),
	    DOS_PSP_DEFAULT_HANDLES);
	put_guest_word(
	    0x1234u,
	    (uint16_t)__builtin_offsetof(struct dos_psp_prefix40, jft_pointer),
	    (uint16_t)__builtin_offsetof(struct dos_psp_prefix40, jft));
	put_guest_word(
	    0x1234u,
	    (uint16_t)(__builtin_offsetof(struct dos_psp_prefix40, jft_pointer) +
		       2u),
	    0x1234u);
	put_guest_far(
	    0x1234u,
	    (uint16_t)__builtin_offsetof(struct dos_psp_prefix40,
					 control_c_vector),
	    0x1111u, 0x2222u);
	put_guest_far(
	    0x1234u,
	    (uint16_t)__builtin_offsetof(struct dos_psp_prefix40,
					 fatal_abort_vector),
	    0x3333u, 0x4444u);
	for (index = 0u; index < DOS_PSP_DEFAULT_HANDLES; ++index) {
		dos_linear_address_t jft_linear = dos_far_to_linear(
		    0x1234u,
		    (uint16_t)(__builtin_offsetof(struct dos_psp_prefix40, jft) +
			       index),
		    false);

		guest_memory[(size_t)jft_linear] = (uint8_t)(index + 1u);
	}
	configure_reentry(&fixture, &request, handle, REENTRY_PARENT_CAPTURE);
	set_parent_result_sentinel(&parent);
	if (dos_exec_transaction_capture_parent(
		&fixture.transactions, handle, &fixture.services,
		&parent) != DOS_EXEC_TRANSACTION_OK ||
	    reentry_status != DOS_EXEC_TRANSACTION_INVALID_STATE ||
	    parent.has_snapshot != 1u || parent.snapshot.captured != 1u ||
	    parent.snapshot.machine_identity != TEST_MACHINE_ID ||
	    parent.snapshot.machine_context != TEST_MACHINE_CONTEXT ||
	    parent.snapshot.parent_psp_segment != 0x1234u ||
	    parent.snapshot.parent_psp[0] != 0xcdu ||
	    parent.snapshot.parent_jft.entries[0] != 1u ||
	    parent.snapshot.parent_jft.entries[19] != 20u ||
	    slot->state != DOS_EXEC_TRANSACTION_STATE_PARENT_SNAPSHOT_READY)
		return 9;
	set_parent_result_sentinel(&parent);
	if (dos_exec_transaction_capture_parent(
		&fixture.transactions, handle, &fixture.services,
		&parent) != DOS_EXEC_TRANSACTION_INVALID_STATE ||
	    !parent_result_is_sentinel(&parent))
		return 10;
	configure_reentry(&fixture, &request, handle, REENTRY_INHERITANCE);
	set_inheritance_result_sentinel(&inheritance);
	if (dos_exec_transaction_prepare_inheritance(
		&fixture.transactions, handle, &fixture.services,
		&inheritance) != DOS_EXEC_TRANSACTION_OK ||
	    reentry_status != DOS_EXEC_TRANSACTION_INVALID_STATE ||
	    inheritance.has_batch != 1u || inheritance.has_child_jft != 1u ||
	    inheritance.batch == DOS_SFT_BATCH_HANDLE_INVALID ||
	    inheritance.child_jft.entries[0] != 1u ||
	    inheritance.child_jft.entries[1] != 2u ||
	    inheritance.child_jft.entries[2] != DOS_JFT_ENTRY_UNUSED ||
	    sft_lookup_calls != DOS_PSP_DEFAULT_HANDLES ||
	    sft_device_open_calls != 1u || sft_acquire_calls != 2u ||
	    slot->state != DOS_EXEC_TRANSACTION_STATE_SFT_RESERVED)
		return 11;
	set_inheritance_result_sentinel(&inheritance);
	if (dos_exec_transaction_prepare_inheritance(
		&fixture.transactions, handle, &fixture.services,
		&inheritance) != DOS_EXEC_TRANSACTION_INVALID_STATE ||
	    !inheritance_result_is_sentinel(&inheritance))
		return 12;
	put_guest_far(request.parameter_block.segment,
		      (uint16_t)(request.parameter_block.offset +
				 TEST_EXEC_FIRST_FCB_POINTER_OFFSET),
		      0x2200u, 0x0100u);
	put_guest_far(request.parameter_block.segment,
		      (uint16_t)(request.parameter_block.offset +
				 TEST_EXEC_SECOND_FCB_POINTER_OFFSET),
		      0x2200u, 0x0200u);
	put_guest_far(request.parameter_block.segment,
		      (uint16_t)(request.parameter_block.offset +
				 TEST_EXEC_COMMAND_TAIL_POINTER_OFFSET),
		      0x2200u, 0x0300u);
	for (index = 0u; index < DOS_PROCESS_FCB_PREFIX_BYTES; ++index) {
		guest_memory[(size_t)dos_far_to_linear(
		    0x2200u, (uint16_t)(0x0100u + index), false)] =
		    (uint8_t)(0x10u + index);
		guest_memory[(size_t)dos_far_to_linear(
		    0x2200u, (uint16_t)(0x0200u + index), false)] =
		    (uint8_t)(0x40u + index);
	}
	for (index = 0u; index < sizeof(struct dos_command_tail40); ++index)
		guest_memory[(size_t)dos_far_to_linear(
		    0x2200u, (uint16_t)(0x0300u + index), false)] =
		    (uint8_t)(0x80u + index);
	configure_reentry(&fixture, &request, handle, REENTRY_PARENT_CAPTURE);
	if (dos_exec_transaction_prepare_psp(
		&fixture.transactions, handle, &fixture.services,
		terminate_vector, &psp) != DOS_EXEC_TRANSACTION_OK ||
	    reentry_status != DOS_EXEC_TRANSACTION_INVALID_STATE ||
	    psp.has_image != 1u || psp.image.segment != target.lease.guest_segment ||
	    psp.image.bytes[DOS_PSP_FIRST_FCB_OFFSET] != 0x10u ||
	    psp.image.bytes[DOS_PSP_FIRST_FCB_OFFSET + 11u] != 0x1bu ||
	    psp.image.bytes[DOS_PSP_FIRST_FCB_OFFSET + 12u] != 0u ||
	    psp.image.bytes[DOS_PSP_SECOND_FCB_OFFSET] != 0x40u ||
	    psp.image.bytes[DOS_PSP_COMMAND_TAIL_OFFSET] != 0x80u ||
	    psp.image.bytes[DOS_PSP_COMMAND_TAIL_OFFSET + 127u] != 0xffu ||
	    psp.image.bytes[__builtin_offsetof(struct dos_psp_prefix40, jft)] !=
		1u ||
	    psp.image.bytes[__builtin_offsetof(struct dos_psp_prefix40, jft) +
			    1u] != 2u ||
	    get_psp_word(&psp, __builtin_offsetof(struct dos_psp_prefix40,
					      block_length)) !=
		(uint16_t)(target.lease.guest_segment +
			   target.allocation.block_paragraphs) ||
	    get_psp_word(&psp, __builtin_offsetof(struct dos_psp_prefix40,
					      parent_psp)) != 0x1234u ||
	    get_psp_word(&psp, __builtin_offsetof(struct dos_psp_prefix40,
					      environment_segment)) != 0u ||
	    get_psp_word(&psp, __builtin_offsetof(struct dos_psp_prefix40,
					      exit_vector)) != 0x8888u ||
	    get_psp_word(&psp, __builtin_offsetof(struct dos_psp_prefix40,
					      exit_vector) + 2u) != 0x7777u ||
	    get_psp_word(&psp, __builtin_offsetof(struct dos_psp_prefix40,
					      control_c_vector)) != 0x2222u ||
	    get_psp_word(&psp, __builtin_offsetof(struct dos_psp_prefix40,
					      control_c_vector) + 2u) != 0x1111u ||
	    get_psp_word(&psp, __builtin_offsetof(struct dos_psp_prefix40,
					      fatal_abort_vector)) != 0x4444u ||
	    get_psp_word(&psp, __builtin_offsetof(struct dos_psp_prefix40,
					      fatal_abort_vector) + 2u) != 0x3333u ||
	    get_psp_word(&psp, __builtin_offsetof(struct dos_psp_prefix40,
					      jft_length)) !=
		DOS_PSP_DEFAULT_HANDLES ||
	    get_psp_word(&psp, __builtin_offsetof(struct dos_psp_prefix40,
					      jft_pointer)) !=
		__builtin_offsetof(struct dos_psp_prefix40, jft) ||
	    get_psp_word(&psp, __builtin_offsetof(struct dos_psp_prefix40,
					      jft_pointer) + 2u) !=
		psp.image.segment ||
	    slot->state != DOS_EXEC_TRANSACTION_STATE_PSP_PREPARED)
		return 13;
	psp_linear = dos_far_to_linear(psp.image.segment, 0u, false);
	for (index = 0u; index < DOS_PSP_SIZE; ++index)
		guest_memory[(size_t)psp_linear + index] = 0x6du;
	configured_invalid_drive = 0x40u;
	configure_reentry(&fixture, &request, handle, REENTRY_INITIAL_STATE);
	set_resident_result_sentinel(&resident);
	if (dos_exec_transaction_finalize_initial_state(
		&fixture.transactions, handle, &fixture.services,
		&resident) != DOS_EXEC_TRANSACTION_OK ||
	    reentry_status != DOS_EXEC_TRANSACTION_INVALID_STATE ||
	    drive_resolve_calls != 2u || resolved_drives[0] != 0x40u ||
	    resolved_drives[1] != 0x10u ||
	    resident.format != (uint8_t)DOS_IMAGE_MZ ||
	    resident.process.mz.initial_state.eax != 0xff00u ||
	    resident.process.mz.initial_state.ebx != 0xff00u ||
	    resident.process.mz.load_only_stack_value != 0xff00u ||
	    slot->state != DOS_EXEC_TRANSACTION_STATE_INITIAL_STATE_READY)
		return 14;
	set_resident_result_sentinel(&resident);
	if (dos_exec_transaction_finalize_initial_state(
		&fixture.transactions, handle, &fixture.services,
		&resident) != DOS_EXEC_TRANSACTION_INVALID_STATE ||
	    !resident_result_is_sentinel(&resident) || drive_resolve_calls != 2u)
		return 15;
	configure_reentry(&fixture, &request, handle, REENTRY_RESIDENT_LOAD);
	writes_before = machine_write_calls;
	if (dos_exec_transaction_stage_process_memory(
		&fixture.transactions, handle, &fixture.services) !=
		DOS_EXEC_TRANSACTION_OK ||
	    reentry_status != DOS_EXEC_TRANSACTION_INVALID_STATE ||
	    machine_write_calls != writes_before + 4u ||
	    slot->journal.record_count != 4u ||
	    slot->state != DOS_EXEC_TRANSACTION_STATE_GLOBAL_STAGED ||
	    guest_memory[(size_t)psp_linear] != psp.image.bytes[0] ||
	    guest_memory[(size_t)psp_linear + DOS_PSP_SIZE - 1u] !=
		psp.image.bytes[DOS_PSP_SIZE - 1u])
		return 16;
	writes_before = machine_write_calls;
	if (dos_exec_transaction_stage_process_memory(
		&fixture.transactions, handle, &fixture.services) !=
		DOS_EXEC_TRANSACTION_INVALID_STATE ||
	    machine_write_calls != writes_before)
		return 17;
	if (dos_exec_transaction_abort(&fixture.transactions, handle,
				      &fixture.services) !=
	    DOS_EXEC_TRANSACTION_OK)
		return 18;
	if (
	    slot->resident.has_process_plan != 0u ||
	    slot->resident.has_resident != 0u ||
	    slot->resident.load.lease_handle != 0u || close_calls != 1u ||
	    slot->parent.has_snapshot != 0u || slot->inheritance.has_batch != 0u ||
	    slot->psp.has_image != 0u ||
	    slot->journal.state != DOS_EXEC_JOURNAL_STATE_ABORTED ||
	    guest_memory[(size_t)psp_linear] != 0x6du ||
	    guest_memory[(size_t)psp_linear + DOS_PSP_SIZE - 1u] != 0x6du ||
	    sft_release_calls != 2u || sft_device_close_calls != 1u)
		return 19;
	if (dos_exec_transaction_retire(&fixture.transactions,
				       TEST_COORDINATOR_ID,
				       handle) != DOS_EXEC_TRANSACTION_OK)
		return 20;

	/* COM preserves the requested-capacity semantics: bytes beyond
	 * EOF stay untouched and are accounted for, never zero-filled. */
	reset_adapters();
	configured_open_size = 3u;
	configured_read_status = DOS_IMAGE_READ_OK;
	file_image_readable_size = 3u;
	file_image[0] = 0x90u;
	file_image[1] = 0x41u;
	file_image[2] = 0x42u;
	request = make_request(DOS_EXEC_LOAD_ONLY);
	if (!initialize_fixture(&fixture) ||
	    !prepare_process_without_environment(&fixture, &request, &handle))
		return 12;
	{
		struct dos_load_plan image;

		if (dos_exec_transaction_inspect_image(
			&fixture.transactions, handle, &fixture.services,
			&image) != DOS_EXEC_TRANSACTION_OK ||
		    image.format != (uint8_t)DOS_IMAGE_COM ||
		    dos_exec_transaction_prepare_target(
			&fixture.transactions, handle, &fixture.services,
			&target) != DOS_EXEC_TRANSACTION_OK)
			return 13;
	}
	load_linear = (dos_linear_address_t)
	    ((uint32_t)(target.lease.guest_segment + TEST_PSP_PARAGRAPHS) << 4);
	guest_memory[(size_t)load_linear + 3u] = 0x7bu;
	set_resident_result_sentinel(&resident);
	if (dos_exec_transaction_load_resident(
		&fixture.transactions, handle, &fixture.services,
		&resident) != DOS_EXEC_TRANSACTION_OK ||
	    resident.format != (uint8_t)DOS_IMAGE_COM ||
	    resident.process.com.launch_mode !=
		(uint8_t)DOS_PROCESS_LAUNCH_LOAD_ONLY ||
	    resident.process.com.image_size != 3u ||
	    resident.process.com.read_capacity != 0xff00u ||
	    resident.process.com.load_only_stack_value != 0u ||
	    resident.load.file_bytes_written != 3u ||
	    resident.load.resident_bytes != 0xff00u ||
	    resident.load.untouched_bytes != 0xfefdu ||
	    guest_memory[(size_t)load_linear] != 0x90u ||
	    guest_memory[(size_t)load_linear + 1u] != 0x41u ||
	    guest_memory[(size_t)load_linear + 2u] != 0x42u ||
	    guest_memory[(size_t)load_linear + 3u] != 0x7bu)
		return 14;
	reads_before = read_calls;
	writes_before = machine_write_calls;
	set_relocation_result_sentinel(&relocation);
	if (dos_exec_transaction_relocate_resident(
		&fixture.transactions, handle, &fixture.services,
		&relocation) != DOS_EXEC_TRANSACTION_OK ||
	    relocation.applicable != 0u || relocation.has_request != 0u ||
	    relocation.applied != 0u ||
	    relocation.request.relocation_count != 0u ||
	    relocation.result.applied_entries != 0u ||
	    read_calls != reads_before || machine_write_calls != writes_before ||
	    fixture.transactions.slots[transaction_slot(handle)].state !=
		DOS_EXEC_TRANSACTION_STATE_IMAGE_PRIVATE)
		return 15;
	if (dos_exec_transaction_close(&fixture.transactions, handle,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_OK ||
	    fixture.transactions.slots[transaction_slot(handle)].state !=
		DOS_EXEC_TRANSACTION_STATE_PROCESS_FILE_CLOSED ||
	    close_calls != 1u ||
	    dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_OK ||
	    dos_exec_transaction_retire(&fixture.transactions,
				TEST_COORDINATOR_ID,
				handle) != DOS_EXEC_TRANSACTION_OK)
		return 16;
	return callback_arguments_valid ? 0 : 17;
}

static int test_process_resident_failure_is_reversible(void)
{
	struct test_fixture fixture;
	struct dos_exec_transaction_request request =
	    make_request(DOS_EXEC_LOAD_ONLY);
	struct dos_exec_transaction_handle handle;
	struct dos_exec_transaction_target target;
	struct dos_exec_transaction_resident resident;
	struct dos_exec_transaction_relocation relocation;
	struct dos_exec_transaction_slot *slot;
	struct dos_load_plan image;

	reset_adapters();
	if (!initialize_fixture(&fixture) ||
	    !prepare_process_image(&fixture, &request, &handle) ||
	    dos_exec_transaction_prepare_target(
		&fixture.transactions, handle, &fixture.services,
		&target) != DOS_EXEC_TRANSACTION_OK)
		return 1;
	configured_short_read = 1u;
	set_resident_result_sentinel(&resident);
	if (dos_exec_transaction_load_resident(
		&fixture.transactions, handle, &fixture.services,
		&resident) != DOS_EXEC_TRANSACTION_BAD_IMAGE ||
	    !resident_result_is_sentinel(&resident))
		return 2;
	slot = &fixture.transactions.slots[transaction_slot(handle)];
	if (slot->state != DOS_EXEC_TRANSACTION_STATE_FAILED ||
	    slot->resident.has_process_plan != 1u ||
	    slot->resident.has_resident != 0u ||
	    slot->resident.load.lease_handle != 0u ||
	    slot->target.has_load_block != 1u)
		return 3;
	configured_short_read = 0u;
	if (dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_OK ||
	    slot->resident.has_process_plan != 0u ||
	    slot->target.has_load_block != 0u ||
	    !dos_memory_lease_table_is_drained(&fixture.memory_leases) ||
	    dos_exec_transaction_retire(&fixture.transactions,
				TEST_COORDINATOR_ID,
				handle) != DOS_EXEC_TRANSACTION_OK)
		return 4;

	/* A relocation entry outside the resident span is a bad executable,
	 * not a host pointer fault.  The private block remains abortable. */
	reset_adapters();
	configure_mz_file();
	put_file_word(30u, 0xffffu);
	if (!initialize_fixture(&fixture) ||
	    !prepare_process_without_environment(&fixture, &request, &handle) ||
	    dos_exec_transaction_inspect_image(
		&fixture.transactions, handle, &fixture.services,
		&image) != DOS_EXEC_TRANSACTION_OK ||
	    dos_exec_transaction_prepare_target(
		&fixture.transactions, handle, &fixture.services,
		&target) != DOS_EXEC_TRANSACTION_OK ||
	    dos_exec_transaction_load_resident(
		&fixture.transactions, handle, &fixture.services,
		&resident) != DOS_EXEC_TRANSACTION_OK)
		return 5;
	set_relocation_result_sentinel(&relocation);
	if (dos_exec_transaction_relocate_resident(
		&fixture.transactions, handle, &fixture.services,
		&relocation) != DOS_EXEC_TRANSACTION_BAD_IMAGE ||
	    !relocation_result_is_sentinel(&relocation))
		return 6;
	slot = &fixture.transactions.slots[transaction_slot(handle)];
	if (slot->state != DOS_EXEC_TRANSACTION_STATE_FAILED ||
	    slot->relocation.applicable != 1u ||
	    slot->relocation.has_request != 1u ||
	    slot->relocation.applied != 0u ||
	    slot->relocation.result.applied_entries != 0u ||
	    slot->target.has_load_block != 1u ||
	    dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_OK ||
	    slot->relocation.has_request != 0u ||
	    slot->resident.has_resident != 0u ||
	    dos_exec_transaction_retire(&fixture.transactions,
				TEST_COORDINATOR_ID,
				handle) != DOS_EXEC_TRANSACTION_OK)
		return 7;
	return callback_arguments_valid ? 0 : 8;
}

static int test_initial_state_drive_fault_is_reversible(void)
{
	struct test_fixture fixture;
	struct dos_exec_transaction_request request =
	    make_request(DOS_EXEC_LOAD_AND_EXECUTE);
	struct dos_exec_transaction_handle handle;
	struct dos_exec_transaction_resident resident;
	struct dos_exec_transaction_slot *slot;

	reset_adapters();
	if (!initialize_fixture(&fixture) ||
	    !prepare_mz_process_psp(&fixture, &request, &handle))
		return 1;
	slot = &fixture.transactions.slots[transaction_slot(handle)];
	configured_fault_drive = 0x40u;
	set_resident_result_sentinel(&resident);
	if (dos_exec_transaction_finalize_initial_state(
		&fixture.transactions, handle, &fixture.services,
		&resident) != DOS_EXEC_TRANSACTION_DRIVE_FAULT ||
	    !resident_result_is_sentinel(&resident) || drive_resolve_calls != 1u ||
	    resolved_drives[0] != 0x40u ||
	    slot->state != DOS_EXEC_TRANSACTION_STATE_FAILED ||
	    slot->resident.process.mz.initial_state.eax != 0u ||
	    slot->resident.process.mz.initial_state.ebx != 0u ||
	    slot->resident.process.mz.load_only_stack_value != 0u)
		return 2;
	if (dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_OK ||
	    slot->psp.has_image != 0u || slot->inheritance.has_batch != 0u ||
	    slot->target.has_load_block != 0u || sft_release_calls != 2u ||
	    sft_device_close_calls != 1u ||
	    !dos_memory_lease_table_is_drained(&fixture.memory_leases) ||
	    dos_exec_transaction_retire(&fixture.transactions,
					TEST_COORDINATOR_ID,
					handle) != DOS_EXEC_TRANSACTION_OK)
		return 3;
	return callback_arguments_valid ? 0 : 4;
}

static int test_load_only_process_memory_is_journaled(void)
{
	struct test_fixture fixture;
	struct dos_exec_transaction_request request =
	    make_request(DOS_EXEC_LOAD_ONLY);
	struct dos_exec_transaction_handle handle;
	struct dos_exec_transaction_resident resident;
	struct dos_exec_transaction_slot *slot;
	struct dos_memory_lease_view lease_view;
	dos_linear_address_t psp_linear;
	dos_linear_address_t mcb_linear;
	uint16_t mcb_segment;
	uint16_t stack_segment;
	uint16_t stack_offset;
	uint32_t writes_before;
	size_t index;

	reset_adapters();
	if (!initialize_fixture(&fixture) ||
	    !prepare_mz_process_psp(&fixture, &request, &handle))
		return 1;
	slot = &fixture.transactions.slots[transaction_slot(handle)];
	psp_linear = dos_far_to_linear(slot->psp.image.segment, 0u, false);
	for (index = 0u; index < DOS_PSP_SIZE; ++index)
		guest_memory[(size_t)psp_linear + index] = 0x6du;
	stack_segment = slot->resident.process.mz.initial_state.ss;
	stack_offset = slot->resident.process.mz.load_only_stack_pointer;
	put_guest_word(stack_segment, stack_offset, 0xa55au);
	mcb_segment = (uint16_t)(slot->target.lease.guest_segment - 1u);
	mcb_linear = dos_far_to_linear(mcb_segment, 0u, false);
	put_guest_word(0u, 0x0088u, 0x1357u);
	put_guest_word(0u, 0x008au, 0x2468u);
	put_guest_word(request.parameter_block.segment,
		       (uint16_t)(request.parameter_block.offset + 14u),
		       0x1111u);
	put_guest_word(request.parameter_block.segment,
		       (uint16_t)(request.parameter_block.offset + 16u),
		       0x2222u);
	put_guest_word(request.parameter_block.segment,
		       (uint16_t)(request.parameter_block.offset + 18u),
		       0x3333u);
	put_guest_word(request.parameter_block.segment,
		       (uint16_t)(request.parameter_block.offset + 20u),
		       0x4444u);
	configured_invalid_drive = 0x40u;
	if (dos_exec_transaction_finalize_initial_state(
		&fixture.transactions, handle, &fixture.services,
		&resident) != DOS_EXEC_TRANSACTION_OK ||
	    resident.process.mz.load_only_stack_value != 0xff00u)
		return 2;
	writes_before = machine_write_calls;
	if (dos_exec_transaction_stage_process_memory(
		&fixture.transactions, handle, &fixture.services) !=
		DOS_EXEC_TRANSACTION_OK ||
	    machine_write_calls != writes_before + 5u ||
	    slot->journal.record_count != 5u ||
	    slot->state != DOS_EXEC_TRANSACTION_STATE_GLOBAL_STAGED ||
	    get_guest_word(stack_segment, stack_offset) != 0xff00u ||
	    guest_memory[(size_t)psp_linear] != slot->psp.image.bytes[0])
		return 3;
	writes_before = machine_write_calls;
	if (dos_exec_transaction_stage_global_memory(
		&fixture.transactions, handle, &fixture.services) !=
		DOS_EXEC_TRANSACTION_OK ||
	    machine_write_calls != writes_before + 3u ||
	    slot->journal.record_count != 8u ||
	    slot->state != DOS_EXEC_TRANSACTION_STATE_GLOBAL_MEMORY_READY ||
	    slot->publication.has_environment_rebind != 0u ||
	    slot->publication.has_load_rebind != 1u ||
	    get_guest_word((uint16_t)(slot->target.lease.guest_segment - 1u),
			   1u) != slot->target.lease.guest_segment ||
	    guest_memory[(size_t)mcb_linear + 8u] != (uint8_t)'A' ||
	    guest_memory[(size_t)mcb_linear + 9u] != (uint8_t)'P' ||
	    guest_memory[(size_t)mcb_linear + 10u] != (uint8_t)'P' ||
	    guest_memory[(size_t)mcb_linear + 11u] != 0u ||
	    get_guest_word(0u, 0x0088u) != 0x8888u ||
	    get_guest_word(0u, 0x008au) != 0x7777u ||
	    get_guest_word(request.parameter_block.segment,
			   (uint16_t)(request.parameter_block.offset + 14u)) !=
		slot->resident.process.mz.load_only_stack_pointer ||
	    get_guest_word(request.parameter_block.segment,
			   (uint16_t)(request.parameter_block.offset + 16u)) !=
		slot->resident.process.mz.initial_state.ss ||
	    get_guest_word(request.parameter_block.segment,
			   (uint16_t)(request.parameter_block.offset + 18u)) !=
		dos_register_low16(slot->resident.process.mz.initial_state.eip) ||
	    get_guest_word(request.parameter_block.segment,
			   (uint16_t)(request.parameter_block.offset + 20u)) !=
		slot->resident.process.mz.initial_state.cs ||
	    dos_memory_lease_resolve_active(
		&fixture.memory_leases, &fixture.memory_arena, &fixture.machine,
		slot->target.lease.handle, 0x1234u, &lease_view) !=
		DOS_MEMORY_LEASE_OK ||
	    lease_view.owner != 0x1234u)
		return 4;
	if (dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) !=
	    DOS_EXEC_TRANSACTION_OK ||
	    get_guest_word(stack_segment, stack_offset) != 0xa55au ||
	    guest_memory[(size_t)psp_linear] != 0x6du ||
	    get_guest_word(0u, 0x0088u) != 0x1357u ||
	    get_guest_word(0u, 0x008au) != 0x2468u ||
	    get_guest_word(request.parameter_block.segment,
			   (uint16_t)(request.parameter_block.offset + 14u)) !=
		0x1111u ||
	    get_guest_word(request.parameter_block.segment,
			   (uint16_t)(request.parameter_block.offset + 20u)) !=
		0x4444u ||
	    get_guest_word(mcb_segment, 1u) != 0u ||
	    slot->journal.state != DOS_EXEC_JOURNAL_STATE_ABORTED ||
	    !dos_memory_lease_table_is_drained(&fixture.memory_leases) ||
	    dos_exec_transaction_retire(&fixture.transactions,
					TEST_COORDINATOR_ID,
					handle) != DOS_EXEC_TRANSACTION_OK)
		return 5;
	return callback_arguments_valid ? 0 : 6;
}

static int test_stack_fault_restores_batched_psp(void)
{
	struct test_fixture fixture;
	struct dos_exec_transaction_request request =
	    make_request(DOS_EXEC_LOAD_ONLY);
	struct dos_exec_transaction_handle handle;
	struct dos_exec_transaction_resident resident;
	struct dos_exec_transaction_slot *slot;
	dos_linear_address_t psp_linear;
	uint16_t stack_segment;
	uint16_t stack_offset;
	size_t index;

	reset_adapters();
	if (!initialize_fixture(&fixture) ||
	    !prepare_mz_process_psp(&fixture, &request, &handle))
		return 1;
	slot = &fixture.transactions.slots[transaction_slot(handle)];
	psp_linear = dos_far_to_linear(slot->psp.image.segment, 0u, false);
	for (index = 0u; index < DOS_PSP_SIZE; ++index)
		guest_memory[(size_t)psp_linear + index] = 0x6du;
	stack_segment = slot->resident.process.mz.initial_state.ss;
	stack_offset = slot->resident.process.mz.load_only_stack_pointer;
	put_guest_word(stack_segment, stack_offset, 0xa55au);
	if (dos_exec_transaction_finalize_initial_state(
		&fixture.transactions, handle, &fixture.services,
		&resident) != DOS_EXEC_TRANSACTION_OK)
		return 2;
	/* Four 64-byte PSP records succeed; the following stack read fails. */
	fail_machine_read_call = machine_read_calls + 5u;
	if (dos_exec_transaction_stage_process_memory(
		&fixture.transactions, handle, &fixture.services) !=
		DOS_EXEC_TRANSACTION_MEMORY_FAULT ||
	    slot->state != DOS_EXEC_TRANSACTION_STATE_FAILED ||
	    slot->journal.state != DOS_EXEC_JOURNAL_STATE_ABORTED ||
	    slot->journal.record_count != 0u ||
	    get_guest_word(stack_segment, stack_offset) != 0xa55au)
		return 3;
	for (index = 0u; index < DOS_PSP_SIZE; ++index) {
		if (guest_memory[(size_t)psp_linear + index] != 0x6du)
			return 4;
	}
	fail_machine_read_call = 0u;
	if (dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_OK ||
	    !dos_memory_lease_table_is_drained(&fixture.memory_leases) ||
	    dos_exec_transaction_retire(&fixture.transactions,
					TEST_COORDINATOR_ID,
					handle) != DOS_EXEC_TRANSACTION_OK)
		return 5;
	return callback_arguments_valid ? 0 : 6;
}

static int test_load_only_transaction_seal(void)
{
	struct test_fixture fixture;
	struct dos_exec_transaction_request request =
	    make_request(DOS_EXEC_LOAD_ONLY);
	struct dos_exec_transaction_handle handle;
	struct dos_exec_transaction_resident resident;
	struct dos_exec_transaction_slot *slot;
	struct dos_memory_lease_view lease_view;
	enum dos_sft_batch_state sft_state = DOS_SFT_BATCH_STATE_ABORTED;
	dos_sft_batch_handle_t batch;
	uint16_t child_psp;
	uint32_t reads_before;
	uint32_t writes_before;

	reset_adapters();
	if (!initialize_fixture(&fixture) ||
	    !prepare_mz_process_psp(&fixture, &request, &handle))
		return 1;
	slot = &fixture.transactions.slots[transaction_slot(handle)];
	configured_invalid_drive = 0x40u;
	if (dos_exec_transaction_finalize_initial_state(
		&fixture.transactions, handle, &fixture.services,
		&resident) != DOS_EXEC_TRANSACTION_OK ||
	    dos_exec_transaction_stage_process_memory(
		&fixture.transactions, handle, &fixture.services) !=
		DOS_EXEC_TRANSACTION_OK ||
	    dos_exec_transaction_stage_global_memory(
		&fixture.transactions, handle, &fixture.services) !=
		DOS_EXEC_TRANSACTION_OK)
		return 2;
	child_psp = slot->target.lease.guest_segment;
	batch = slot->inheritance.batch;
	reads_before = machine_read_calls;
	writes_before = machine_write_calls;
	if (dos_exec_transaction_seal_load_only(
		&fixture.transactions, handle, &fixture.services) !=
		DOS_EXEC_TRANSACTION_OK ||
	    slot->state != DOS_EXEC_TRANSACTION_STATE_PUBLISHED ||
	    slot->journal.state != DOS_EXEC_JOURNAL_STATE_SEALED ||
	    slot->journal.record_count != 0u ||
	    slot->observer.state != DOS_EXEC_OBSERVER_STATE_RELEASED ||
	    fixture.runtime.current_psp != child_psp ||
	    fixture.runtime.dta.segment != child_psp ||
	    fixture.runtime.dta.offset != DOS_PSP_COMMAND_TAIL_OFFSET ||
	    get_guest_word((uint16_t)(child_psp - 1u), 1u) != child_psp ||
	    machine_read_calls != reads_before ||
	    machine_write_calls != writes_before ||
	    observer_release_calls != 1u ||
	    !dos_memory_lease_table_is_drained(&fixture.memory_leases) ||
	    dos_sft_batch_get_state(slot->inheritance.batch, &sft_state) !=
		DOS_SFT_BATCH_OK ||
	    sft_state != DOS_SFT_BATCH_STATE_COMMITTED ||
	    dos_memory_lease_resolve_active(
		&fixture.memory_leases, &fixture.memory_arena, &fixture.machine,
		slot->target.lease.handle, child_psp, &lease_view) !=
		DOS_MEMORY_LEASE_INVALID_STATE)
		return 3;
	if (dos_exec_transaction_seal_load_only(
		&fixture.transactions, handle, &fixture.services) !=
		DOS_EXEC_TRANSACTION_INVALID_STATE ||
	    dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_INVALID_STATE)
		return 4;
	if (dos_exec_transaction_retire_published(
		&fixture.transactions, handle, &fixture.services) !=
		DOS_EXEC_TRANSACTION_OK ||
	    !dos_exec_transaction_table_is_drained(&fixture.transactions) ||
	    !dos_exec_file_lease_table_is_drained(&fixture.files) ||
	    dos_sft_batch_get_state(batch, &sft_state) !=
		DOS_SFT_BATCH_STALE_HANDLE ||
	    dos_exec_transaction_seal_load_only(
		&fixture.transactions, handle, &fixture.services) !=
		DOS_EXEC_TRANSACTION_STALE_HANDLE)
		return 5;
	return callback_arguments_valid ? 0 : 6;
}

static int test_execute_handoff_is_journaled(void)
{
	struct test_fixture fixture;
	struct dos_exec_transaction_request request =
	    make_request(DOS_EXEC_LOAD_AND_EXECUTE);
	struct dos_exec_transaction_handle handle;
	struct dos_exec_transaction_resident resident;
	struct dos_exec_handoff_plan handoff;
	struct dos_exec_handoff_plan unchanged = {0};
	struct dos_exec_transaction_slot *slot;
	uint16_t initial_sp;
	uint16_t stack_offset;
	uint16_t stack_segment;
	uint32_t writes_before;

	reset_adapters();
	if (!initialize_fixture(&fixture) ||
	    !prepare_mz_process_psp(&fixture, &request, &handle))
		return 1;
	slot = &fixture.transactions.slots[transaction_slot(handle)];
	initial_sp =
	    dos_register_low16(slot->resident.process.mz.initial_state.esp);
	stack_segment = slot->resident.process.mz.initial_state.ss;
	stack_offset = (uint16_t)(initial_sp - DOS_EXEC_HANDOFF_STACK_BYTES);
	put_guest_word(stack_segment, stack_offset, 0xa55au);
	put_guest_word(stack_segment, (uint16_t)(stack_offset + 2u), 0x5aa5u);
	if (dos_exec_transaction_finalize_initial_state(
		&fixture.transactions, handle, &fixture.services,
		&resident) != DOS_EXEC_TRANSACTION_OK ||
	    dos_exec_transaction_stage_process_memory(
		&fixture.transactions, handle, &fixture.services) !=
		DOS_EXEC_TRANSACTION_OK ||
	    slot->journal.record_count != 4u ||
	    dos_exec_transaction_stage_global_memory(
		&fixture.transactions, handle, &fixture.services) !=
		DOS_EXEC_TRANSACTION_OK ||
	    slot->journal.record_count != 6u)
		return 2;
	writes_before = machine_write_calls;
	if (dos_exec_transaction_prepare_handoff(
		&fixture.transactions, handle, &fixture.services,
		&handoff) != DOS_EXEC_TRANSACTION_OK ||
	    slot->state != DOS_EXEC_TRANSACTION_STATE_HANDOFF_READY ||
	    slot->publication.has_handoff != 1u ||
	    slot->journal.record_count != 7u ||
	    machine_write_calls != writes_before + 1u ||
	    !dos_exec_handoff_plan_has_valid_encoding(&handoff) ||
	    handoff.child_psp != slot->target.lease.guest_segment ||
	    handoff.entry_state.esp !=
		slot->resident.process.mz.initial_state.esp ||
	    get_guest_word(stack_segment, stack_offset) !=
		dos_register_low16(handoff.entry_state.eip) ||
	    get_guest_word(stack_segment, (uint16_t)(stack_offset + 2u)) !=
		handoff.entry_state.cs)
		return 3;
	unchanged.child_psp = 0xa5a5u;
	unchanged.format = 0x5au;
	if (dos_exec_transaction_prepare_handoff(
		&fixture.transactions, handle, &fixture.services,
		&unchanged) != DOS_EXEC_TRANSACTION_INVALID_STATE ||
	    unchanged.child_psp != 0xa5a5u || unchanged.format != 0x5au)
		return 4;
	if (dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_OK ||
	    get_guest_word(stack_segment, stack_offset) != 0xa55au ||
	    get_guest_word(stack_segment, (uint16_t)(stack_offset + 2u)) !=
		0x5aa5u ||
	    slot->publication.has_handoff != 0u ||
	    slot->journal.state != DOS_EXEC_JOURNAL_STATE_ABORTED ||
	    dos_exec_transaction_retire(&fixture.transactions,
					TEST_COORDINATOR_ID,
					handle) != DOS_EXEC_TRANSACTION_OK)
		return 5;
	return callback_arguments_valid ? 0 : 6;
}

static bool prepare_execute_backend_boundary(
    struct test_fixture *fixture,
    const struct dos_exec_transaction_request *request,
    struct dos_exec_transaction_handle *handle)
{
	struct dos_exec_transaction_resident resident;
	struct dos_exec_handoff_plan handoff;

	return prepare_mz_process_psp(fixture, request, handle) &&
	       dos_exec_transaction_finalize_initial_state(
		   &fixture->transactions, *handle, &fixture->services,
		   &resident) == DOS_EXEC_TRANSACTION_OK &&
	       dos_exec_transaction_stage_process_memory(
		   &fixture->transactions, *handle, &fixture->services) ==
		   DOS_EXEC_TRANSACTION_OK &&
	       dos_exec_transaction_stage_global_memory(
		   &fixture->transactions, *handle, &fixture->services) ==
		   DOS_EXEC_TRANSACTION_OK &&
	       dos_exec_transaction_prepare_handoff(
		   &fixture->transactions, *handle, &fixture->services,
		   &handoff) == DOS_EXEC_TRANSACTION_OK;
}

static int test_execute_backend_seal_and_transfer(void)
{
	struct test_fixture fixture;
	struct dos_exec_transaction_request request =
	    make_request(DOS_EXEC_LOAD_AND_EXECUTE);
	struct dos_exec_transaction_handle handle;
	struct dos_exec_backend_session_handle session = {
	    .value = HANDLE_SENTINEL,
	};
	struct dos_exec_transaction_slot *slot;
	enum dos_exec_backend_session_state backend_state =
	    DOS_EXEC_BACKEND_SESSION_VACANT;
	uint16_t child_psp;
	uint32_t detail = DETAIL_SENTINEL;

	reset_adapters();
	if (!initialize_fixture(&fixture) ||
	    !prepare_execute_backend_boundary(&fixture, &request, &handle))
		return 1;
	slot = &fixture.transactions.slots[transaction_slot(handle)];
	child_psp = slot->target.lease.guest_segment;
	if (dos_exec_transaction_prepare_backend(
		&fixture.transactions, handle, &fixture.services, &detail) !=
		DOS_EXEC_TRANSACTION_OK ||
	    detail != 0u || backend_prepare_calls != 1u ||
	    backend_run_calls != 0u ||
	    slot->state != DOS_EXEC_TRANSACTION_STATE_BACKEND_DORMANT ||
	    slot->publication.has_backend_session != 1u ||
	    dos_exec_backend_session_get_state(
		&fixture.backend_sessions,
		slot->publication.backend_session, &backend_state) !=
		DOS_EXEC_BACKEND_SESSION_OK ||
	    backend_state != DOS_EXEC_BACKEND_SESSION_DORMANT)
		return 2;
	if (dos_exec_transaction_seal_execute(
		&fixture.transactions, handle, &fixture.services,
		&session) != DOS_EXEC_TRANSACTION_OK ||
	    session.value == HANDLE_SENTINEL ||
	    session.value != slot->publication.backend_session.value ||
	    slot->state != DOS_EXEC_TRANSACTION_STATE_PUBLISHED ||
	    slot->journal.state != DOS_EXEC_JOURNAL_STATE_SEALED ||
	    slot->observer.state != DOS_EXEC_OBSERVER_STATE_RELEASED ||
	    fixture.runtime.current_psp != child_psp ||
	    get_guest_word((uint16_t)(child_psp - 1u), 1u) != child_psp ||
	    dos_exec_backend_session_get_state(
		&fixture.backend_sessions, session, &backend_state) !=
		DOS_EXEC_BACKEND_SESSION_OK ||
	    backend_state != DOS_EXEC_BACKEND_SESSION_RUNNABLE ||
	    backend_run_calls != 0u || observer_release_calls != 1u)
		return 3;
	if (dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_INVALID_STATE ||
	    dos_exec_transaction_retire_published(
		&fixture.transactions, handle, &fixture.services) !=
		DOS_EXEC_TRANSACTION_OK ||
	    !dos_exec_transaction_table_is_drained(&fixture.transactions) ||
	    dos_exec_backend_session_get_state(
		&fixture.backend_sessions, session, &backend_state) !=
		DOS_EXEC_BACKEND_SESSION_OK ||
	    backend_state != DOS_EXEC_BACKEND_SESSION_RUNNABLE)
		return 4;
	if (dos_exec_backend_session_stop(
		&fixture.backend_sessions, session, &backend_ops,
		TEST_BACKEND_CONTEXT) != DOS_EXEC_BACKEND_SESSION_OK ||
	    backend_release_calls != 1u ||
	    dos_exec_backend_session_retire(&fixture.backend_sessions,
					    session) !=
		DOS_EXEC_BACKEND_SESSION_OK ||
	    !dos_exec_backend_session_table_is_drained(
		&fixture.backend_sessions))
		return 5;
	return callback_arguments_valid ? 0 : 6;
}

static int test_execute_backend_abort_retries_release(void)
{
	struct test_fixture fixture;
	struct dos_exec_transaction_request request =
	    make_request(DOS_EXEC_LOAD_AND_EXECUTE);
	struct dos_exec_transaction_handle handle;
	struct dos_exec_transaction_slot *slot;
	uint32_t detail = DETAIL_SENTINEL;

	reset_adapters();
	if (!initialize_fixture(&fixture) ||
	    !prepare_execute_backend_boundary(&fixture, &request, &handle) ||
	    dos_exec_transaction_prepare_backend(
		&fixture.transactions, handle, &fixture.services, &detail) !=
		DOS_EXEC_TRANSACTION_OK)
		return 1;
	slot = &fixture.transactions.slots[transaction_slot(handle)];
	configured_backend_release = DOS_EXEC_BACKEND_RETAINED;
	if (dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_BACKEND_RETAINED ||
	    slot->state != DOS_EXEC_TRANSACTION_STATE_BACKEND_DORMANT ||
	    slot->journal.state != DOS_EXEC_JOURNAL_STATE_STAGING ||
	    slot->publication.has_backend_session != 1u ||
	    backend_release_calls != 1u)
		return 2;
	configured_backend_release = DOS_EXEC_BACKEND_RELEASED;
	if (dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_OK ||
	    slot->state != DOS_EXEC_TRANSACTION_STATE_ABORTED ||
	    backend_release_calls != 2u ||
	    slot->publication.has_backend_session != 0u ||
	    !dos_exec_backend_session_table_is_drained(
		&fixture.backend_sessions) ||
	    dos_exec_transaction_retire(&fixture.transactions,
					TEST_COORDINATOR_ID,
					handle) != DOS_EXEC_TRANSACTION_OK)
		return 3;
	return callback_arguments_valid ? 0 : 4;
}

static void prepare_executor_guest_request(
	const struct dos_exec_transaction_request *request)
{
	size_t index;

	put_guest_word(request->parameter_block.segment,
		       request->parameter_block.offset, 0u);
	put_guest_word(0x1234u, 0x002cu, 0u);
	guest_memory[(size_t)dos_far_to_linear(0x1234u, 0u, false)] = 0xcdu;
	put_guest_word(
		0x1234u,
		(uint16_t)__builtin_offsetof(struct dos_psp_prefix40, jft_length),
		DOS_PSP_DEFAULT_HANDLES);
	put_guest_far(
		0x1234u,
		(uint16_t)__builtin_offsetof(struct dos_psp_prefix40, jft_pointer),
		0x1234u,
		(uint16_t)__builtin_offsetof(struct dos_psp_prefix40, jft));
	put_guest_far(
		0x1234u,
		(uint16_t)__builtin_offsetof(struct dos_psp_prefix40,
					     control_c_vector),
		0x1111u, 0x2222u);
	put_guest_far(
		0x1234u,
		(uint16_t)__builtin_offsetof(struct dos_psp_prefix40,
					     fatal_abort_vector),
		0x3333u, 0x4444u);
	for (index = 0u; index < DOS_PSP_DEFAULT_HANDLES; ++index) {
		dos_linear_address_t linear = dos_far_to_linear(
			0x1234u,
			(uint16_t)(__builtin_offsetof(struct dos_psp_prefix40, jft) +
				   index),
			false);

		guest_memory[(size_t)linear] = index < 2u
						 ? (uint8_t)(index + 1u)
						 : DOS_JFT_ENTRY_UNUSED;
	}
	put_guest_far(request->parameter_block.segment,
		      (uint16_t)(request->parameter_block.offset +
				 TEST_EXEC_FIRST_FCB_POINTER_OFFSET),
		      0x2200u, 0x0100u);
	put_guest_far(request->parameter_block.segment,
		      (uint16_t)(request->parameter_block.offset +
				 TEST_EXEC_SECOND_FCB_POINTER_OFFSET),
		      0x2200u, 0x0200u);
	put_guest_far(request->parameter_block.segment,
		      (uint16_t)(request->parameter_block.offset +
				 TEST_EXEC_COMMAND_TAIL_POINTER_OFFSET),
		      0x2200u, 0x0300u);
	for (index = 0u; index < DOS_PROCESS_FCB_PREFIX_BYTES; ++index) {
		guest_memory[(size_t)dos_far_to_linear(
			0x2200u, (uint16_t)(0x0100u + index), false)] =
			(uint8_t)(0x10u + index);
		guest_memory[(size_t)dos_far_to_linear(
			0x2200u, (uint16_t)(0x0200u + index), false)] =
			(uint8_t)(0x40u + index);
	}
	for (index = 0u; index < sizeof(struct dos_command_tail40); ++index)
		guest_memory[(size_t)dos_far_to_linear(
			0x2200u, (uint16_t)(0x0300u + index), false)] =
			(uint8_t)(0x80u + index);
}

static int test_unified_executor_success_and_cleanup(void)
{
	struct test_fixture fixture;
	struct dos_exec_transaction_request request =
		make_request(DOS_EXEC_LOAD_AND_EXECUTE);
	struct dos_process_far_address terminate_vector = {
		.segment = 0x7777u,
		.offset = 0x8888u,
	};
	struct dos_exec_executor_result result;
	enum dos_exec_backend_session_state session_state;

	reset_adapters();
	if (!initialize_fixture(&fixture))
		return 1;
	configure_mz_file();
	prepare_executor_guest_request(&request);
	expected_name_pointer = NULL;
	if (dos_exec_executor_execute(
		&fixture.transactions, &fixture.services, &request,
		terminate_vector, &result) != DOS_EXEC_EXECUTOR_OK ||
	    result.primary_status != DOS_EXEC_TRANSACTION_OK ||
	    result.cleanup_status != DOS_EXEC_TRANSACTION_OK ||
	    result.failure_detail != 0u || result.has_session != 1u ||
	    result.session.value == 0u ||
	    !dos_exec_transaction_table_is_drained(&fixture.transactions) ||
	    !dos_exec_file_lease_table_is_drained(&fixture.files) ||
	    fixture.runtime.current_psp == 0x1234u ||
	    dos_exec_backend_session_get_state(
		&fixture.backend_sessions, result.session, &session_state) !=
		DOS_EXEC_BACKEND_SESSION_OK ||
	    session_state != DOS_EXEC_BACKEND_SESSION_RUNNABLE ||
	    backend_run_calls != 0u)
		return 2;
	if (dos_exec_backend_session_stop(
		&fixture.backend_sessions, result.session, &backend_ops,
		TEST_BACKEND_CONTEXT) != DOS_EXEC_BACKEND_SESSION_OK ||
	    dos_exec_backend_session_retire(&fixture.backend_sessions,
					    result.session) !=
		DOS_EXEC_BACKEND_SESSION_OK)
		return 3;

	reset_adapters();
	if (!initialize_fixture(&fixture))
		return 4;
	configured_open_status = DOS_EXEC_FILE_ADAPTER_FAULT;
	configured_open_detail = 0x4321u;
	expected_name_pointer = NULL;
	if (dos_exec_executor_execute(
		&fixture.transactions, &fixture.services, &request,
		terminate_vector, &result) !=
		DOS_EXEC_EXECUTOR_TRANSACTION_FAILED ||
	    result.primary_status != DOS_EXEC_TRANSACTION_OPEN_FAILED ||
	    result.cleanup_status != DOS_EXEC_TRANSACTION_OK ||
	    result.failure_detail != 0x4321u || result.has_session != 0u ||
	    !dos_exec_transaction_table_is_drained(&fixture.transactions) ||
	    !dos_exec_file_lease_table_is_drained(&fixture.files) ||
	    observer_release_calls != 1u)
		return 5;
	return callback_arguments_valid ? 0 : 6;
}

static int test_native_exec_uses_standard_guest_request(void)
{
	static const uint8_t native_tail[] = {' ', '/', 'B'};
	struct test_fixture fixture;
	struct dos_exec_transaction_request parent_request =
		make_request(DOS_EXEC_LOAD_AND_EXECUTE);
	struct dos_exec_native_request request = {
		.executable_name = canonical_executable_name,
		.executable_name_length =
			ARRAY_SIZE(canonical_executable_name) - 1u,
		.command_tail = native_tail,
		.command_tail_capacity = ARRAY_SIZE(native_tail),
		.command_tail_length = ARRAY_SIZE(native_tail),
	};
	struct dos_process_far_address terminate_vector = {
		.segment = 0x7777u,
		.offset = 0x8888u,
	};
	struct dos_exec_native_result result;
	enum dos_exec_backend_session_state session_state;

	reset_adapters();
	if (!initialize_fixture(&fixture))
		return 1;
	configure_mz_file();
	prepare_executor_guest_request(&parent_request);
	expected_name_pointer = NULL;
	if (dos_exec_native_execute(
		&fixture.transactions, &fixture.services, &request,
		terminate_vector, &result) != DOS_EXEC_NATIVE_OK ||
	    result.executor.primary_status != DOS_EXEC_TRANSACTION_OK ||
	    result.executor.cleanup_status != DOS_EXEC_TRANSACTION_OK ||
	    result.executor.has_session != 1u ||
	    result.executor.session.value == 0u ||
	    result.allocation_status != DOS_MEMORY_OK ||
	    result.staging_status != DOS_MACHINE_OK ||
	    result.cleanup_status != DOS_MEMORY_OK ||
	    result.scratch_segment == 0u ||
	    !dos_exec_transaction_table_is_drained(&fixture.transactions) ||
	    !dos_exec_file_lease_table_is_drained(&fixture.files) ||
	    dos_memory_arena_validate_checked(
		&fixture.memory_arena, &fixture.machine) != DOS_MEMORY_OK ||
	    dos_exec_backend_session_get_state(
		&fixture.backend_sessions, result.executor.session,
		&session_state) != DOS_EXEC_BACKEND_SESSION_OK ||
	    session_state != DOS_EXEC_BACKEND_SESSION_RUNNABLE ||
	    backend_run_calls != 0u)
		return 2;
	if (dos_exec_backend_session_stop(
		&fixture.backend_sessions, result.executor.session, &backend_ops,
		TEST_BACKEND_CONTEXT) != DOS_EXEC_BACKEND_SESSION_OK ||
	    dos_exec_backend_session_retire(
		&fixture.backend_sessions, result.executor.session) !=
		DOS_EXEC_BACKEND_SESSION_OK)
		return 3;
	return callback_arguments_valid ? 0 : 4;
}

static int test_int21_exec_decodes_and_publishes_child(void)
{
	struct test_fixture fixture;
	struct dos_exec_transaction_request request =
		make_request(DOS_EXEC_LOAD_AND_EXECUTE);
	struct dos_cpu_state state = {
		.eax = 0x4b00u,
		.ebx = request.parameter_block.offset,
		.edx = request.executable_name.offset,
		.eip = 0x8888u,
		.eflags = DOS_EFLAGS_CF,
		.cs = 0x7777u,
		.ds = request.executable_name.segment,
		.es = request.parameter_block.segment,
		.mode = DOS_CPU_VM86,
	};
	struct dos_exec_int21_result result;
	enum dos_exec_backend_session_state session_state;

	reset_adapters();
	if (!initialize_fixture(&fixture))
		return 1;
	configure_mz_file();
	prepare_executor_guest_request(&request);
	expected_name_pointer = NULL;
	if (dos_exec_int21_execute(
		&fixture.transactions, &fixture.services, &state, &result) !=
		DOS_EXEC_INT21_CHILD_READY ||
	    result.status != DOS_EXEC_INT21_CHILD_READY ||
	    result.dos_error != DOS_SUCCESS ||
	    (result.resume_state.eflags & DOS_EFLAGS_CF) != 0u ||
	    result.executor.has_session != 1u ||
	    result.executor.session.value == 0u ||
	    dos_exec_backend_session_get_state(
		&fixture.backend_sessions, result.executor.session,
		&session_state) != DOS_EXEC_BACKEND_SESSION_OK ||
	    session_state != DOS_EXEC_BACKEND_SESSION_RUNNABLE)
		return 2;
	if (dos_exec_backend_session_stop(
		&fixture.backend_sessions, result.executor.session, &backend_ops,
		TEST_BACKEND_CONTEXT) != DOS_EXEC_BACKEND_SESSION_OK ||
	    dos_exec_backend_session_retire(
		&fixture.backend_sessions, result.executor.session) !=
		DOS_EXEC_BACKEND_SESSION_OK)
		return 3;

	reset_adapters();
	if (!initialize_fixture(&fixture))
		return 4;
	prepare_executor_guest_request(&request);
	configured_open_status = DOS_EXEC_FILE_ADAPTER_FAULT;
	configured_open_detail = 0x4321u;
	expected_name_pointer = NULL;
	if (dos_exec_int21_execute(
		&fixture.transactions, &fixture.services, &state, &result) !=
		DOS_EXEC_INT21_DOS_ERROR ||
	    result.status != DOS_EXEC_INT21_DOS_ERROR ||
	    result.dos_error != DOS_ERROR_FILE_NOT_FOUND ||
	    dos_register_low16(result.resume_state.eax) !=
		DOS_ERROR_FILE_NOT_FOUND ||
	    (result.resume_state.eflags & DOS_EFLAGS_CF) == 0u ||
	    result.executor.has_session != 0u ||
	    !dos_exec_transaction_table_is_drained(&fixture.transactions) ||
	    !dos_exec_file_lease_table_is_drained(&fixture.files))
		return 5;

	state.eax = 0x4b02u;
	if (dos_exec_int21_execute(
		&fixture.transactions, &fixture.services, &state, &result) !=
		DOS_EXEC_INT21_DOS_ERROR ||
	    result.dos_error != DOS_ERROR_INVALID_FUNCTION ||
	    dos_register_low16(result.resume_state.eax) !=
		DOS_ERROR_INVALID_FUNCTION ||
	    (result.resume_state.eflags & DOS_EFLAGS_CF) == 0u)
		return 6;
	return callback_arguments_valid ? 0 : 7;
}

static int test_open_probe_device_and_output_rules(void)
{
	struct test_fixture fixture;
	struct dos_exec_transaction_request request =
	    make_request(DOS_EXEC_LOAD_ONLY);
	struct dos_exec_transaction_handle handle;
	uint32_t detail;
	uint8_t is_device;

	reset_adapters();
	if (!initialize_fixture(&fixture) ||
	    dos_exec_transaction_begin(&fixture.transactions, &fixture.services,
				       &request,
				       &handle) != DOS_EXEC_TRANSACTION_OK)
		return 1;
	configured_open_status = DOS_EXEC_FILE_ADAPTER_FAULT;
	configured_open_detail = 0x12345678u;
	detail = DETAIL_SENTINEL;
	if (dos_exec_transaction_open(&fixture.transactions, handle,
				      &fixture.services, executable_name,
				      ARRAY_SIZE(executable_name), &detail) !=
		DOS_EXEC_TRANSACTION_OPEN_FAILED ||
	    detail != 0x12345678u ||
	    fixture.transactions.slots[transaction_slot(handle)].state !=
		DOS_EXEC_TRANSACTION_STATE_FAILED ||
	    close_calls != 0u ||
	    dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_OK ||
	    observer_release_calls != 1u || close_calls != 0u ||
	    dos_exec_transaction_retire(&fixture.transactions,
					TEST_COORDINATOR_ID,
					handle) != DOS_EXEC_TRANSACTION_OK)
		return 2;

	configured_open_status = DOS_EXEC_FILE_ADAPTER_OK;
	configured_open_detail = 0u;
	configured_probe_status = DOS_EXEC_FILE_ADAPTER_FAULT;
	configured_probe_detail = 0x87654321u;
	if (dos_exec_transaction_begin(&fixture.transactions, &fixture.services,
				       &request,
				       &handle) != DOS_EXEC_TRANSACTION_OK)
		return 3;
	detail = DETAIL_SENTINEL;
	if (dos_exec_transaction_open(&fixture.transactions, handle,
				      &fixture.services, executable_name,
				      ARRAY_SIZE(executable_name),
				      &detail) != DOS_EXEC_TRANSACTION_OK)
		return 4;
	detail = DETAIL_SENTINEL;
	is_device = BYTE_SENTINEL;
	if (dos_exec_transaction_probe(
		&fixture.transactions, handle, &fixture.services, &is_device,
		&detail) != DOS_EXEC_TRANSACTION_PROBE_FAILED ||
	    detail != 0x87654321u || is_device != BYTE_SENTINEL ||
	    dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_OK ||
	    close_calls != 1u || observer_release_calls != 2u ||
	    dos_exec_transaction_retire(&fixture.transactions,
					TEST_COORDINATOR_ID,
					handle) != DOS_EXEC_TRANSACTION_OK)
		return 5;

	configured_probe_status = DOS_EXEC_FILE_ADAPTER_OK;
	configured_probe_detail = 0u;
	configured_is_device = 1u;
	if (dos_exec_transaction_begin(&fixture.transactions, &fixture.services,
				       &request,
				       &handle) != DOS_EXEC_TRANSACTION_OK)
		return 6;
	detail = DETAIL_SENTINEL;
	if (dos_exec_transaction_open(&fixture.transactions, handle,
				      &fixture.services, executable_name,
				      ARRAY_SIZE(executable_name),
				      &detail) != DOS_EXEC_TRANSACTION_OK)
		return 7;
	is_device = BYTE_SENTINEL;
	detail = DETAIL_SENTINEL;
	if (dos_exec_transaction_probe(
		&fixture.transactions, handle, &fixture.services, &is_device,
		&detail) != DOS_EXEC_TRANSACTION_IS_DEVICE ||
	    is_device != 1u || detail != 0u ||
	    dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_OK ||
	    close_calls != 2u || observer_release_calls != 3u ||
	    dos_exec_transaction_retire(&fixture.transactions,
					TEST_COORDINATOR_ID,
					handle) != DOS_EXEC_TRANSACTION_OK)
		return 8;
	return callback_arguments_valid ? 0 : 9;
}

static int test_retained_abort_retry(void)
{
	struct test_fixture fixture;
	struct dos_exec_transaction_request request =
	    make_request(DOS_EXEC_LOAD_AND_EXECUTE);
	struct dos_exec_transaction_handle handle;
	struct dos_exec_transaction_slot *slot;
	uint32_t detail = DETAIL_SENTINEL;
	uint8_t is_device = BYTE_SENTINEL;

	reset_adapters();
	if (!initialize_fixture(&fixture) ||
	    dos_exec_transaction_begin(&fixture.transactions, &fixture.services,
				       &request,
				       &handle) != DOS_EXEC_TRANSACTION_OK ||
	    dos_exec_transaction_open(&fixture.transactions, handle,
				      &fixture.services, executable_name,
				      ARRAY_SIZE(executable_name),
				      &detail) != DOS_EXEC_TRANSACTION_OK ||
	    dos_exec_transaction_probe(&fixture.transactions, handle,
				       &fixture.services, &is_device,
				       &detail) != DOS_EXEC_TRANSACTION_OK)
		return 1;
	slot = &fixture.transactions.slots[transaction_slot(handle)];
	configured_close_result = DOS_EXEC_FILE_CLOSE_RETAINED;
	if (dos_exec_transaction_close(&fixture.transactions, handle,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_CLOSE_RETAINED ||
	    slot->state != DOS_EXEC_TRANSACTION_STATE_FAILED ||
	    close_calls != 1u || observer_release_calls != 0u)
		return 2;
	if (dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_CLOSE_RETAINED ||
	    slot->state != DOS_EXEC_TRANSACTION_STATE_ABORTING ||
	    close_calls != 2u || observer_release_calls != 0u ||
	    slot->has_file_lease != 1u)
		return 3;
	configured_close_result = DOS_EXEC_FILE_CLOSE_CLOSED;
	if (dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_OK ||
	    slot->state != DOS_EXEC_TRANSACTION_STATE_ABORTED ||
	    close_calls != 3u || observer_release_calls != 1u ||
	    slot->has_file_lease != 0u ||
	    dos_exec_transaction_retire(&fixture.transactions,
					TEST_COORDINATOR_ID,
					handle) != DOS_EXEC_TRANSACTION_OK)
		return 4;
	return 0;
}

static int test_binding_and_unchanged_outputs(void)
{
	struct test_fixture fixture;
	struct dos_exec_file_lease_table other_files;
	struct dos_process_runtime other_runtime =
	    DOS_PROCESS_RUNTIME_INITIALIZER;
	struct dos_exec_transaction_services wrong_services;
	struct dos_exec_transaction_request request =
	    make_request(DOS_EXEC_LOAD_AND_EXECUTE);
	struct dos_exec_transaction_handle handle;
	uint32_t detail = DETAIL_SENTINEL;
	uint8_t is_device = BYTE_SENTINEL;

	reset_adapters();
	if (!initialize_fixture(&fixture) ||
	    dos_exec_transaction_begin(&fixture.transactions, &fixture.services,
				       &request,
				       &handle) != DOS_EXEC_TRANSACTION_OK)
		return 1;
	wrong_services = fixture.services;
	wrong_services.machine_identity++;
	if (dos_exec_transaction_open(&fixture.transactions, handle,
				      &wrong_services, executable_name,
				      ARRAY_SIZE(executable_name), &detail) !=
		DOS_EXEC_TRANSACTION_BINDING_MISMATCH ||
	    detail != DETAIL_SENTINEL || open_calls != 0u)
		return 2;
	if (dos_exec_file_lease_table_construct(&other_files) !=
		DOS_EXEC_FILE_LEASE_OK ||
	    dos_exec_file_lease_table_initialize(&other_files,
						 TEST_OTHER_FILE_TABLE_ID) !=
		DOS_EXEC_FILE_LEASE_OK)
		return 3;
	wrong_services = fixture.services;
	wrong_services.file_leases = &other_files;
	wrong_services.file_lease_table_identity = TEST_OTHER_FILE_TABLE_ID;
	if (dos_exec_transaction_open(&fixture.transactions, handle,
				      &wrong_services, executable_name,
				      ARRAY_SIZE(executable_name), &detail) !=
		DOS_EXEC_TRANSACTION_BINDING_MISMATCH ||
	    detail != DETAIL_SENTINEL || open_calls != 0u)
		return 4;
	if (dos_process_runtime_initialize(
		&other_runtime, TEST_OTHER_RUNTIME_ID, 0x1234u,
		(struct dos_far_pointer16){.offset = 0x0080u,
					   .segment = 0x1234u}) !=
	    DOS_PROCESS_RUNTIME_OK)
		return 5;
	wrong_services = fixture.services;
	wrong_services.runtime = &other_runtime;
	if (dos_exec_transaction_open(&fixture.transactions, handle,
				      &wrong_services, executable_name,
				      ARRAY_SIZE(executable_name), &detail) !=
		DOS_EXEC_TRANSACTION_BINDING_MISMATCH ||
	    detail != DETAIL_SENTINEL || open_calls != 0u)
		return 6;
	wrong_services = fixture.services;
	wrong_services.drive_adapter_context++;
	if (dos_exec_transaction_open(&fixture.transactions, handle,
				      &wrong_services, executable_name,
				      ARRAY_SIZE(executable_name), &detail) !=
		DOS_EXEC_TRANSACTION_BINDING_MISMATCH ||
	    detail != DETAIL_SENTINEL || open_calls != 0u)
		return 7;
	if (dos_exec_transaction_probe(
		&fixture.transactions, handle, &fixture.services, &is_device,
		&detail) != DOS_EXEC_TRANSACTION_INVALID_STATE ||
	    is_device != BYTE_SENTINEL || detail != DETAIL_SENTINEL ||
	    probe_calls != 0u ||
	    dos_exec_transaction_retire(&fixture.transactions,
					TEST_COORDINATOR_ID, handle) !=
		DOS_EXEC_TRANSACTION_INVALID_STATE)
		return 8;
	if (dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_OK ||
	    dos_exec_transaction_retire(&fixture.transactions,
					TEST_COORDINATOR_ID,
					handle) != DOS_EXEC_TRANSACTION_OK)
		return 9;

	configured_observer_acquire = DOS_EXEC_OBSERVER_ADAPTER_BUSY;
	handle.value = HANDLE_SENTINEL;
	if (dos_exec_transaction_begin(&fixture.transactions, &fixture.services,
				       &request, &handle) !=
		DOS_EXEC_TRANSACTION_OBSERVER_BUSY ||
	    handle.value != HANDLE_SENTINEL ||
	    !dos_exec_transaction_table_is_drained(&fixture.transactions))
		return 10;
	return 0;
}

static int test_malformed_slots_fail_closed(void)
{
	struct test_fixture fixture;
	struct dos_exec_transaction_request request =
	    make_request(DOS_EXEC_LOAD_AND_EXECUTE);
	struct dos_exec_transaction_handle handle;
	struct dos_exec_transaction_handle sentinel = {.value =
							   HANDLE_SENTINEL};
	struct dos_exec_transaction_slot *slot;
	uint32_t calls_before;
	uint32_t detail = DETAIL_SENTINEL;
	uint32_t other_slot;

	reset_adapters();
	if (!initialize_fixture(&fixture))
		return 1;
	fixture.transactions.slots[0].observer.state =
	    (uint8_t)DOS_EXEC_OBSERVER_STATE_HELD;
	fixture.transactions.slots[0].file_lease.value = 0x1234u;
	if (dos_exec_transaction_table_is_drained(&fixture.transactions) ||
	    dos_exec_transaction_begin(&fixture.transactions, &fixture.services,
				       &request, &sentinel) !=
		DOS_EXEC_TRANSACTION_INVALID_STATE ||
	    sentinel.value != HANDLE_SENTINEL || observer_acquire_calls != 0u)
		return 2;

	reset_adapters();
	if (!initialize_fixture(&fixture) ||
	    dos_exec_transaction_begin(&fixture.transactions, &fixture.services,
				       &request,
				       &handle) != DOS_EXEC_TRANSACTION_OK ||
	    dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_OK)
		return 3;
	slot = &fixture.transactions.slots[transaction_slot(handle)];
	slot->has_file_lease = 1u;
	slot->file_lease.value = 0x1234u;
	calls_before = observer_acquire_calls;
	if (dos_exec_transaction_begin(&fixture.transactions, &fixture.services,
				       &request, &sentinel) !=
		DOS_EXEC_TRANSACTION_INVALID_STATE ||
	    sentinel.value != HANDLE_SENTINEL ||
	    observer_acquire_calls != calls_before)
		return 4;

	reset_adapters();
	if (!initialize_fixture(&fixture) ||
	    dos_exec_transaction_begin(&fixture.transactions, &fixture.services,
				       &request,
				       &handle) != DOS_EXEC_TRANSACTION_OK)
		return 5;
	slot = &fixture.transactions.slots[transaction_slot(handle)];
	slot->coordinator_identity++;
	if (dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_INVALID_STATE ||
	    observer_release_calls != 0u)
		return 6;

	reset_adapters();
	if (!initialize_fixture(&fixture) ||
	    dos_exec_transaction_begin(&fixture.transactions, &fixture.services,
				       &request,
				       &handle) != DOS_EXEC_TRANSACTION_OK)
		return 7;
	slot = &fixture.transactions.slots[transaction_slot(handle)];
	slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_ENV_READY;
	if (dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_INVALID_STATE ||
	    observer_release_calls != 0u)
		return 8;

	reset_adapters();
	if (!initialize_fixture(&fixture) ||
	    dos_exec_transaction_begin(&fixture.transactions, &fixture.services,
				       &request,
				       &handle) != DOS_EXEC_TRANSACTION_OK)
		return 9;
	slot = &fixture.transactions.slots[transaction_slot(handle)];
	slot->state = (uint8_t)DOS_EXEC_TRANSACTION_STATE_POISONED;
	slot->observer.state = (uint8_t)DOS_EXEC_OBSERVER_STATE_POISONED;
	calls_before = observer_acquire_calls;
	sentinel.value = HANDLE_SENTINEL;
	if (dos_exec_transaction_begin(&fixture.transactions, &fixture.services,
				       &request, &sentinel) !=
		DOS_EXEC_TRANSACTION_POISONED ||
	    fixture.transactions.poisoned != 0u ||
	    sentinel.value != HANDLE_SENTINEL ||
	    observer_acquire_calls != calls_before)
		return 10;

	reset_adapters();
	if (!initialize_fixture(&fixture) ||
	    dos_exec_transaction_begin(&fixture.transactions, &fixture.services,
				       &request,
				       &handle) != DOS_EXEC_TRANSACTION_OK)
		return 11;
	other_slot =
	    (transaction_slot(handle) + 1u) % DOS_EXEC_TRANSACTION_SLOT_COUNT;
	fixture.transactions.slots[other_slot] =
	    fixture.transactions.slots[transaction_slot(handle)];
	calls_before = observer_acquire_calls;
	if (dos_exec_transaction_begin(&fixture.transactions, &fixture.services,
				       &request, &sentinel) !=
		DOS_EXEC_TRANSACTION_INVALID_STATE ||
	    sentinel.value != HANDLE_SENTINEL ||
	    dos_exec_transaction_open(&fixture.transactions, handle,
				      &fixture.services, executable_name,
				      ARRAY_SIZE(executable_name), &detail) !=
		DOS_EXEC_TRANSACTION_INVALID_STATE ||
	    detail != DETAIL_SENTINEL || open_calls != 0u ||
	    dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_INVALID_STATE ||
	    observer_acquire_calls != calls_before ||
	    observer_release_calls != 0u)
		return 12;

	reset_adapters();
	if (!initialize_fixture(&fixture))
		return 13;
	fixture.transactions.slots[0].environment_source.source.segment =
	    0x1234u;
	fixture.transactions.slots[0].environment_source.kind =
	    DOS_EXEC_ENVIRONMENT_SOURCE_PARAMETER;
	sentinel.value = HANDLE_SENTINEL;
	if (dos_exec_transaction_table_is_drained(&fixture.transactions) ||
	    dos_exec_transaction_begin(&fixture.transactions, &fixture.services,
				       &request, &sentinel) !=
		DOS_EXEC_TRANSACTION_INVALID_STATE ||
	    sentinel.value != HANDLE_SENTINEL || observer_acquire_calls != 0u)
		return 14;
	return 0;
}

static void
configure_reentry(struct test_fixture *fixture,
		  const struct dos_exec_transaction_request *request,
		  struct dos_exec_transaction_handle handle, uint8_t point)
{
	reentry_table = &fixture->transactions;
	reentry_services = &fixture->services;
	reentry_request = *request;
	reentry_handle = handle;
	reentry_status = DOS_EXEC_TRANSACTION_OK;
	reentry_point = point;
}

static int test_callback_reentry_is_rejected(void)
{
	struct test_fixture fixture;
	struct dos_exec_transaction_request request =
	    make_request(DOS_EXEC_LOAD_AND_EXECUTE);
	struct dos_exec_transaction_handle handle = {.value = HANDLE_SENTINEL};
	struct dos_exec_environment_source_plan source;
	uint32_t detail = DETAIL_SENTINEL;
	uint8_t is_device = BYTE_SENTINEL;

	reset_adapters();
	if (!initialize_fixture(&fixture))
		return 1;
	configure_reentry(&fixture, &request, handle, REENTRY_OBSERVER_ACQUIRE);
	if (dos_exec_transaction_begin(&fixture.transactions, &fixture.services,
				       &request,
				       &handle) != DOS_EXEC_TRANSACTION_OK ||
	    reentry_status != DOS_EXEC_TRANSACTION_BUSY ||
	    observer_acquire_calls != 1u)
		return 2;
	configure_reentry(&fixture, &request, handle, REENTRY_FILE_OPEN);
	if (dos_exec_transaction_open(&fixture.transactions, handle,
				      &fixture.services, executable_name,
				      ARRAY_SIZE(executable_name),
				      &detail) != DOS_EXEC_TRANSACTION_OK ||
	    reentry_status != DOS_EXEC_TRANSACTION_INVALID_STATE ||
	    open_calls != 1u)
		return 3;
	configure_reentry(&fixture, &request, handle, REENTRY_FILE_PROBE);
	if (dos_exec_transaction_probe(&fixture.transactions, handle,
				       &fixture.services, &is_device,
				       &detail) != DOS_EXEC_TRANSACTION_OK ||
	    reentry_status != DOS_EXEC_TRANSACTION_INVALID_STATE ||
	    probe_calls != 1u)
		return 4;
	configure_reentry(&fixture, &request, handle, REENTRY_FILE_CLOSE);
	if (dos_exec_transaction_close(&fixture.transactions, handle,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_OK ||
	    reentry_status != DOS_EXEC_TRANSACTION_INVALID_STATE ||
	    close_calls != 1u)
		return 5;
	configure_reentry(&fixture, &request, handle, REENTRY_OBSERVER_RELEASE);
	if (dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_OK ||
	    reentry_status != DOS_EXEC_TRANSACTION_INVALID_STATE ||
	    observer_release_calls != 1u ||
	    dos_exec_transaction_retire(&fixture.transactions,
					TEST_COORDINATOR_ID,
					handle) != DOS_EXEC_TRANSACTION_OK)
		return 6;

	reset_adapters();
	if (!initialize_fixture(&fixture) ||
	    dos_exec_transaction_begin(&fixture.transactions, &fixture.services,
				       &request,
				       &handle) != DOS_EXEC_TRANSACTION_OK ||
	    dos_exec_transaction_open(&fixture.transactions, handle,
				      &fixture.services, executable_name,
				      ARRAY_SIZE(executable_name),
				      &detail) != DOS_EXEC_TRANSACTION_OK ||
	    dos_exec_transaction_probe(&fixture.transactions, handle,
				       &fixture.services, &is_device,
				       &detail) != DOS_EXEC_TRANSACTION_OK)
		return 7;
	configure_reentry(&fixture, &request, handle, REENTRY_ABORT_FILE_CLOSE);
	if (dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_OK ||
	    reentry_status != DOS_EXEC_TRANSACTION_INVALID_STATE ||
	    close_calls != 1u || observer_release_calls != 1u ||
	    dos_exec_transaction_retire(&fixture.transactions,
					TEST_COORDINATOR_ID,
					handle) != DOS_EXEC_TRANSACTION_OK)
		return 8;

	reset_adapters();
	put_guest_word(request.parameter_block.segment,
		       request.parameter_block.offset, 0x4567u);
	if (!initialize_fixture(&fixture) ||
	    !prepare_file_probed(&fixture, &request, &handle))
		return 9;
	configure_reentry(&fixture, &request, handle,
			  REENTRY_ENVIRONMENT_READ);
	set_environment_source_sentinel(&source);
	if (dos_exec_transaction_select_environment(
		&fixture.transactions, handle, &fixture.services, &source) !=
		DOS_EXEC_TRANSACTION_OK ||
	    reentry_status != DOS_EXEC_TRANSACTION_INVALID_STATE ||
	    machine_read_calls != 1u ||
	    source.kind != DOS_EXEC_ENVIRONMENT_SOURCE_PARAMETER ||
	    fixture.transactions.slots[transaction_slot(handle)].state !=
		DOS_EXEC_TRANSACTION_STATE_ENV_READY ||
	    dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_OK ||
	    dos_exec_transaction_retire(&fixture.transactions,
					TEST_COORDINATOR_ID,
					handle) != DOS_EXEC_TRANSACTION_OK)
		return 10;
	return callback_arguments_valid ? 0 : 11;
}

static int test_uncertain_and_invalid_adapter_poison(void)
{
	struct test_fixture fixture;
	struct dos_exec_transaction_request request =
	    make_request(DOS_EXEC_OVERLAY);
	struct dos_exec_transaction_handle handle;
	struct dos_exec_transaction_handle sentinel = {.value =
							   HANDLE_SENTINEL};
	uint32_t detail = DETAIL_SENTINEL;
	uint8_t is_device = BYTE_SENTINEL;
	uint32_t acquire_before;
	uint32_t open_before;
	uint32_t close_before;
	uint32_t index;
	const enum dos_exec_observer_adapter_status uncertain_results[] = {
	    DOS_EXEC_OBSERVER_ADAPTER_BUSY,
	    DOS_EXEC_OBSERVER_ADAPTER_FAULT,
	};

	reset_adapters();
	if (!initialize_fixture(&fixture) ||
	    dos_exec_transaction_begin(&fixture.transactions, &fixture.services,
				       &request,
				       &handle) != DOS_EXEC_TRANSACTION_OK ||
	    dos_exec_transaction_open(&fixture.transactions, handle,
				      &fixture.services, executable_name,
				      ARRAY_SIZE(executable_name),
				      &detail) != DOS_EXEC_TRANSACTION_OK ||
	    dos_exec_transaction_probe(&fixture.transactions, handle,
				       &fixture.services, &is_device,
				       &detail) != DOS_EXEC_TRANSACTION_OK)
		return 1;
	configure_reentry(&fixture, &request, handle,
			  REENTRY_OBSERVER_QUARANTINE);
	configured_close_result = DOS_EXEC_FILE_CLOSE_UNCERTAIN;
	if (dos_exec_transaction_close(&fixture.transactions, handle,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_POISONED ||
	    fixture.transactions.poisoned != 1u ||
	    fixture.transactions.slots[transaction_slot(handle)].state !=
		DOS_EXEC_TRANSACTION_STATE_POISONED ||
	    reentry_status != DOS_EXEC_TRANSACTION_POISONED ||
	    observer_quarantine_calls != 1u || observer_release_calls != 0u)
		return 2;
	acquire_before = observer_acquire_calls;
	open_before = open_calls;
	close_before = close_calls;
	if (dos_exec_transaction_begin(&fixture.transactions, &fixture.services,
				       &request, &sentinel) !=
		DOS_EXEC_TRANSACTION_POISONED ||
	    sentinel.value != HANDLE_SENTINEL ||
	    dos_exec_transaction_open(&fixture.transactions, handle,
				      &fixture.services, executable_name,
				      ARRAY_SIZE(executable_name), &detail) !=
		DOS_EXEC_TRANSACTION_POISONED ||
	    dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_POISONED ||
	    observer_acquire_calls != acquire_before ||
	    open_calls != open_before || close_calls != close_before ||
	    observer_quarantine_calls != 1u)
		return 3;

	reset_adapters();
	if (!initialize_fixture(&fixture) ||
	    dos_exec_transaction_begin(&fixture.transactions, &fixture.services,
				       &request,
				       &handle) != DOS_EXEC_TRANSACTION_OK)
		return 4;
	configured_open_status = (enum dos_exec_file_adapter_status)99;
	detail = DETAIL_SENTINEL;
	if (dos_exec_transaction_open(&fixture.transactions, handle,
				      &fixture.services, executable_name,
				      ARRAY_SIZE(executable_name), &detail) !=
		DOS_EXEC_TRANSACTION_POISONED ||
	    detail != DETAIL_SENTINEL || fixture.transactions.poisoned != 1u ||
	    observer_quarantine_calls != 1u || observer_release_calls != 0u)
		return 5;

	reset_adapters();
	if (!initialize_fixture(&fixture))
		return 6;
	configure_reentry(&fixture, &request, handle,
			  REENTRY_OBSERVER_QUARANTINE);
	configured_observer_acquire = (enum dos_exec_observer_adapter_status)99;
	handle.value = HANDLE_SENTINEL;
	if (dos_exec_transaction_begin(&fixture.transactions, &fixture.services,
				       &request, &handle) !=
		DOS_EXEC_TRANSACTION_POISONED ||
	    handle.value != HANDLE_SENTINEL ||
	    fixture.transactions.poisoned != 1u ||
	    observer_acquire_calls != 1u || observer_quarantine_calls != 1u ||
	    reentry_status != DOS_EXEC_TRANSACTION_POISONED)
		return 7;
	if (dos_exec_transaction_begin(&fixture.transactions, &fixture.services,
				       &request, &handle) !=
		DOS_EXEC_TRANSACTION_POISONED ||
	    observer_acquire_calls != 1u || observer_quarantine_calls != 1u)
		return 8;

	for (index = 0u; index < ARRAY_SIZE(uncertain_results); ++index) {
		reset_adapters();
		if (!initialize_fixture(&fixture))
			return 9;
		configure_reentry(&fixture, &request, handle,
				  REENTRY_OBSERVER_QUARANTINE);
		configured_observer_acquire = uncertain_results[index];
		publish_observer_generation_on_failure = 1u;
		handle.value = HANDLE_SENTINEL;
		if (dos_exec_transaction_begin(
			&fixture.transactions, &fixture.services, &request,
			&handle) != DOS_EXEC_TRANSACTION_POISONED ||
		    handle.value != HANDLE_SENTINEL ||
		    fixture.transactions.poisoned != 1u ||
		    observer_acquire_calls != 1u ||
		    observer_quarantine_calls != 1u ||
		    reentry_status != DOS_EXEC_TRANSACTION_POISONED)
			return 10;
	}

	reset_adapters();
	if (!initialize_fixture(&fixture))
		return 11;
	configure_reentry(&fixture, &request, handle,
			  REENTRY_OBSERVER_QUARANTINE);
	next_observer_generation = 0u;
	configured_observer_quarantine = DOS_EXEC_OBSERVER_ADAPTER_FAULT;
	handle.value = HANDLE_SENTINEL;
	if (dos_exec_transaction_begin(&fixture.transactions, &fixture.services,
				       &request, &handle) !=
		DOS_EXEC_TRANSACTION_POISONED ||
	    handle.value != HANDLE_SENTINEL ||
	    fixture.transactions.poisoned != 1u ||
	    observer_acquire_calls != 1u || observer_quarantine_calls != 1u ||
	    reentry_status != DOS_EXEC_TRANSACTION_POISONED ||
	    dos_exec_transaction_begin(&fixture.transactions, &fixture.services,
				       &request, &handle) !=
		DOS_EXEC_TRANSACTION_POISONED ||
	    observer_acquire_calls != 1u || observer_quarantine_calls != 1u)
		return 12;

	reset_adapters();
	if (!initialize_fixture(&fixture) ||
	    dos_exec_transaction_begin(&fixture.transactions, &fixture.services,
				       &request,
				       &handle) != DOS_EXEC_TRANSACTION_OK)
		return 13;
	configure_reentry(&fixture, &request, handle,
			  REENTRY_OBSERVER_QUARANTINE);
	configured_observer_release = (enum dos_exec_observer_adapter_status)99;
	if (dos_exec_transaction_abort(&fixture.transactions, handle,
				       &fixture.services) !=
		DOS_EXEC_TRANSACTION_POISONED ||
	    fixture.transactions.poisoned != 1u ||
	    observer_release_calls != 1u || observer_quarantine_calls != 1u ||
	    reentry_status != DOS_EXEC_TRANSACTION_POISONED)
		return 14;
	return 0;
}

static int run_tests(void)
{
	int status;

	status = test_normal_source_order_and_aba();
	if (status != 0)
		return 10 + status;
	status = test_subfunctions_capacity_and_nonwrap();
	if (status != 0)
		return 30 + status;
	status = test_overlay_omits_process_only_bindings();
	if (status != 0)
		return 45 + status;
	status = test_environment_selection_paths();
	if (status != 0)
		return 55 + status;
	status = test_environment_failures_and_corruption();
	if (status != 0)
		return 70 + status;
	status = test_open_probe_device_and_output_rules();
	if (status != 0)
		return 90 + status;
	status = test_retained_abort_retry();
	if (status != 0)
		return 110 + status;
	status = test_binding_and_unchanged_outputs();
	if (status != 0)
		return 120 + status;
	status = test_malformed_slots_fail_closed();
	if (status != 0)
		return 140 + status;
	status = test_callback_reentry_is_rejected();
	if (status != 0)
		return 160 + status;
	status = test_uncertain_and_invalid_adapter_poison();
	if (status != 0)
		return 180 + status;
	status = test_guest_name_open_boundary();
	if (status != 0)
		return 200 + status;
	status = test_environment_block_success_and_reverse_abort();
	if (status != 0)
		return 220 + status;
	status = test_environment_failure_retains_and_retries_lease();
	if (status != 0)
		return 235 + status;
	status = test_environment_none_preflight_and_allocation_guards();
	if (status != 0)
		return 244 + status;
	status = test_image_inspection_process_and_overlay();
	if (status != 0)
		return 20 + status;
	status = test_image_inspection_failures_are_reversible();
	if (status != 0)
		return 40 + status;
	status = test_process_target_allocation_and_reverse_abort();
	if (status != 0)
		return 60 + status;
	status = test_process_target_failures_are_reversible();
	if (status != 0)
		return 80 + status;
	status = test_process_resident_loads_and_reverse_abort();
	if (status != 0)
		return 100 + status;
	status = test_process_resident_failure_is_reversible();
	if (status != 0)
		return 120 + status;
	status = test_initial_state_drive_fault_is_reversible();
	if (status != 0)
		return 140 + status;
	status = test_load_only_process_memory_is_journaled();
	if (status != 0)
		return 150 + status;
	status = test_load_only_transaction_seal();
	if (status != 0)
		return 155 + status;
	status = test_execute_handoff_is_journaled();
	if (status != 0)
		return 158 + status;
	status = test_execute_backend_seal_and_transfer();
	if (status != 0)
		return 162 + status;
	status = test_execute_backend_abort_retries_release();
	if (status != 0)
		return 168 + status;
	status = test_unified_executor_success_and_cleanup();
	if (status != 0)
		return 175 + status;
	status = test_native_exec_uses_standard_guest_request();
	if (status != 0)
		return 182 + status;
	status = test_int21_exec_decodes_and_publishes_child();
	if (status != 0)
		return 190 + status;
	status = test_stack_fault_restores_batched_psp();
	if (status != 0)
		return 160 + status;
	return 0;
}

DOSC32_TEST_ENTRY(run_tests)

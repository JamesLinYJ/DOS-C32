// SPDX-License-Identifier: GPL-2.0-only
/* Freestanding lifecycle tests for the DOS EXEC executable-file lease. */
#include "dos_exec_file_lease.h"
#include "test_entry.h"

#define TEST_ADAPTER_IDENTITY ((kernel_object_handle_t)0x1111222233334444ull)
#define TEST_TABLE_IDENTITY ((kernel_object_handle_t)0x46494c455441424cull)
#define TEST_OTHER_IDENTITY ((kernel_object_handle_t)0x5555666677778888ull)
#define TEST_CONTEXT ((kernel_object_handle_t)0x0123456789abcdefull)
#define TEST_OTHER_CONTEXT ((kernel_object_handle_t)0x1020304050607080ull)
#define TEST_READER_CONTEXT ((kernel_object_handle_t)0x8877665544332211ull)
#define TEST_NEXT_READER_CONTEXT ((kernel_object_handle_t)0x8877665544332212ull)
#define TEST_FILE_SIZE ((file_offset_t)0x123456789abcdef0ull)
#define HANDLE_SENTINEL 0xaaaaaaaa55555555ull
#define DETAIL_SENTINEL 0xa5a55a5au
#define BYTE_SENTINEL ((uint8_t)0xa5u)

#define EVENT_OPEN 1u
#define EVENT_PROBE 2u
#define EVENT_READ 3u
#define EVENT_CLOSE 4u
#define EVENT_CAPACITY 128u

static const uint8_t test_path[] = {'C', ':', 0x5cu, 'B', 'I', 'N', 0x5cu,
				    'A', 'P', 'P',   '.', 'E', 'X', 'E'};

static enum dos_exec_file_adapter_status configured_open_status;
static enum dos_exec_file_adapter_status configured_probe_status;
static enum dos_exec_file_close_result configured_close_result;
static kernel_object_handle_t configured_reader_context;
static file_offset_t configured_file_size;
static uint32_t configured_failure_detail;
static uint32_t configured_open_reserved;
static uint32_t configured_probe_failure_detail;
static uint8_t configured_probe_reserved;
static uint8_t configured_is_device;
static uint8_t callback_arguments_valid;
static uint8_t events[EVENT_CAPACITY];
static uint32_t event_count;
static uint32_t open_calls;
static uint32_t probe_calls;
static uint32_t read_calls;
static uint32_t close_calls;
static uint32_t open_failure_detail;
static uint32_t probe_failure_detail;
static kernel_object_handle_t certainly_closed_context;

static void record_event(uint8_t event)
{
	if (event_count < EVENT_CAPACITY)
		events[event_count] = event;
	++event_count;
}

static void reset_adapter(void)
{
	size_t index;

	configured_open_status = DOS_EXEC_FILE_ADAPTER_OK;
	configured_probe_status = DOS_EXEC_FILE_ADAPTER_OK;
	configured_close_result = DOS_EXEC_FILE_CLOSE_CLOSED;
	configured_reader_context = TEST_READER_CONTEXT;
	configured_file_size = TEST_FILE_SIZE;
	configured_failure_detail = 0u;
	configured_open_reserved = 0u;
	configured_probe_failure_detail = 0u;
	configured_probe_reserved = 0u;
	configured_is_device = 0u;
	callback_arguments_valid = 1u;
	event_count = 0u;
	open_calls = 0u;
	probe_calls = 0u;
	read_calls = 0u;
	close_calls = 0u;
	open_failure_detail = DETAIL_SENTINEL;
	probe_failure_detail = DETAIL_SENTINEL;
	certainly_closed_context = KERNEL_OBJECT_HANDLE_INVALID;
	for (index = 0u; index < ARRAY_SIZE(events); ++index)
		events[index] = 0u;
}

static bool path_matches(const uint8_t *path, size_t length)
{
	size_t index;

	if (path == NULL || length != ARRAY_SIZE(test_path))
		return false;
	for (index = 0u; index < ARRAY_SIZE(test_path); ++index) {
		if (path[index] != test_path[index])
			return false;
	}
	return true;
}

static enum dos_exec_file_adapter_status
test_open(kernel_object_handle_t context, const uint8_t *path,
	  size_t path_length, struct dos_exec_file_open_result *result)
{
	record_event(EVENT_OPEN);
	++open_calls;
	if (context != TEST_CONTEXT || !path_matches(path, path_length) ||
	    result == NULL)
		callback_arguments_valid = 0u;
	result->failure_detail = configured_failure_detail;
	result->reserved = configured_open_reserved;
	if (configured_open_status != DOS_EXEC_FILE_ADAPTER_OK)
		return configured_open_status;
	result->reader_context = configured_reader_context;
	result->size = configured_file_size;
	return DOS_EXEC_FILE_ADAPTER_OK;
}

static enum dos_exec_file_adapter_status
test_probe_device(kernel_object_handle_t context,
		  kernel_object_handle_t reader_context,
		  struct dos_exec_file_probe_result *result)
{
	record_event(EVENT_PROBE);
	++probe_calls;
	if (context != TEST_CONTEXT ||
	    reader_context != configured_reader_context || result == NULL)
		callback_arguments_valid = 0u;
	result->failure_detail = configured_probe_failure_detail;
	result->reserved[0] = configured_probe_reserved;
	if (configured_probe_status != DOS_EXEC_FILE_ADAPTER_OK)
		return configured_probe_status;
	result->is_device = configured_is_device;
	return DOS_EXEC_FILE_ADAPTER_OK;
}

static enum dos_image_read_status
test_read(kernel_object_handle_t reader_context, file_offset_t offset,
	  void *destination, size_t destination_capacity, size_t count,
	  size_t *bytes_read)
{
	uint8_t *bytes = (uint8_t *)destination;

	record_event(EVENT_READ);
	++read_calls;
	if (reader_context == certainly_closed_context)
		return DOS_IMAGE_READ_IO_ERROR;
	if (reader_context != configured_reader_context || offset != 7u ||
	    destination == NULL || destination_capacity < count ||
	    bytes_read == NULL)
		callback_arguments_valid = 0u;
	if (count != 0u)
		bytes[0] = 0x5au;
	*bytes_read = count;
	return DOS_IMAGE_READ_OK;
}

static enum dos_image_read_status
sentinel_read(kernel_object_handle_t reader_context, file_offset_t offset,
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

static enum dos_exec_file_close_result
test_close(kernel_object_handle_t context,
	   kernel_object_handle_t reader_context)
{
	record_event(EVENT_CLOSE);
	++close_calls;
	if (context != TEST_CONTEXT ||
	    reader_context != configured_reader_context)
		callback_arguments_valid = 0u;
	if (configured_close_result != DOS_EXEC_FILE_CLOSE_RETAINED)
		certainly_closed_context = reader_context;
	return configured_close_result;
}

static const struct dos_exec_file_lease_ops test_ops = {
    .identity = TEST_ADAPTER_IDENTITY,
    .open = test_open,
    .probe_device = test_probe_device,
    .read = test_read,
    .close = test_close,
};

static enum dos_exec_file_lease_status
test_acquire(struct dos_exec_file_lease_table *table,
	     const struct dos_exec_file_lease_ops *ops,
	     kernel_object_handle_t context, const uint8_t *path,
	     size_t path_length, struct dos_exec_file_lease_handle *handle)
{
	return dos_exec_file_lease_acquire(table, ops, context, path,
					   path_length, handle,
					   &open_failure_detail);
}

static enum dos_exec_file_lease_status
test_probe(struct dos_exec_file_lease_table *table,
	   struct dos_exec_file_lease_handle handle,
	   const struct dos_exec_file_lease_ops *ops,
	   kernel_object_handle_t context, uint8_t *is_device)
{
	return dos_exec_file_lease_probe_device(
	    table, handle, ops, context, is_device, &probe_failure_detail);
}

static bool initialize_table(struct dos_exec_file_lease_table *table)
{
	return dos_exec_file_lease_table_construct(table) ==
		   DOS_EXEC_FILE_LEASE_OK &&
	       dos_exec_file_lease_table_initialize(
		   table, TEST_TABLE_IDENTITY) == DOS_EXEC_FILE_LEASE_OK;
}

static void set_handle_sentinel(struct dos_exec_file_lease_handle *handle)
{
	handle->value = HANDLE_SENTINEL;
}

static bool handle_is_sentinel(struct dos_exec_file_lease_handle handle)
{
	return handle.value == HANDLE_SENTINEL;
}

static void set_reader_sentinel(struct dos_image_reader *reader)
{
	reader->context = (kernel_object_handle_t)0xabcdefabcdefabcdull;
	reader->size = (file_offset_t)0xfedcbafedcbafedcull;
	reader->read = sentinel_read;
}

static bool reader_is_sentinel(const struct dos_image_reader *reader)
{
	return reader->context ==
		   (kernel_object_handle_t)0xabcdefabcdefabcdull &&
	       reader->size == (file_offset_t)0xfedcbafedcbafedcull &&
	       reader->read == sentinel_read;
}

static uint32_t handle_slot(struct dos_exec_file_lease_handle handle)
{
	return (uint32_t)(handle.value & DOS_EXEC_FILE_LEASE_SLOT_MASK) - 1u;
}

static int test_success_and_aba(void)
{
	static struct dos_exec_file_lease_table table =
	    DOS_EXEC_FILE_LEASE_TABLE_INITIALIZER;
	struct dos_exec_file_lease_handle first;
	struct dos_exec_file_lease_handle second;
	struct dos_image_reader reader;
	size_t bytes_read = 0u;
	uint8_t byte = 0u;
	uint8_t is_device = BYTE_SENTINEL;
	uint32_t calls_before;

	reset_adapter();
	if (dos_exec_file_lease_table_initialize(&table, TEST_TABLE_IDENTITY) !=
		DOS_EXEC_FILE_LEASE_OK ||
	    dos_exec_file_lease_table_initialize(&table, TEST_TABLE_IDENTITY) !=
		DOS_EXEC_FILE_LEASE_INVALID_STATE ||
	    table.identity != TEST_TABLE_IDENTITY ||
	    !dos_exec_file_lease_table_is_drained(&table))
		return 1;
	set_handle_sentinel(&first);
	if (test_acquire(&table, &test_ops, TEST_CONTEXT, test_path,
			 ARRAY_SIZE(test_path),
			 &first) != DOS_EXEC_FILE_LEASE_OK ||
	    handle_is_sentinel(first) || open_failure_detail != 0u ||
	    open_calls != 1u || probe_calls != 0u ||
	    !callback_arguments_valid || event_count != 1u ||
	    events[0] != EVENT_OPEN ||
	    dos_exec_file_lease_table_is_drained(&table))
		return 2;
	if (table.slots[handle_slot(first)].state !=
		(uint8_t)DOS_EXEC_FILE_LEASE_STATE_OPEN ||
	    table.slots[handle_slot(first)].adapter_identity !=
		TEST_ADAPTER_IDENTITY ||
	    table.slots[handle_slot(first)].adapter_context != TEST_CONTEXT ||
	    table.slots[handle_slot(first)].reader_context !=
		TEST_READER_CONTEXT ||
	    table.slots[handle_slot(first)].size != TEST_FILE_SIZE)
		return 3;
	set_reader_sentinel(&reader);
	if (dos_exec_file_lease_resolve_reader(&table, first, &test_ops,
					       TEST_CONTEXT, &reader) !=
		DOS_EXEC_FILE_LEASE_INVALID_STATE ||
	    !reader_is_sentinel(&reader))
		return 4;
	if (test_probe(&table, first, &test_ops, TEST_CONTEXT, &is_device) !=
		DOS_EXEC_FILE_LEASE_OK ||
	    is_device != 0u || probe_failure_detail != 0u ||
	    probe_calls != 1u || event_count != 2u || events[1] != EVENT_PROBE)
		return 5;
	is_device = BYTE_SENTINEL;
	if (dos_exec_file_lease_query_device(&table, first, &test_ops,
					     TEST_CONTEXT, &is_device) !=
		DOS_EXEC_FILE_LEASE_OK ||
	    is_device != 0u)
		return 6;
	calls_before = probe_calls;
	is_device = BYTE_SENTINEL;
	probe_failure_detail = DETAIL_SENTINEL;
	if (test_probe(&table, first, &test_ops, TEST_CONTEXT, &is_device) !=
		DOS_EXEC_FILE_LEASE_OK ||
	    is_device != 0u || probe_failure_detail != 0u ||
	    probe_calls != calls_before)
		return 7;
	set_reader_sentinel(&reader);
	if (dos_exec_file_lease_resolve_reader(&table, first, &test_ops,
					       TEST_CONTEXT, &reader) !=
		DOS_EXEC_FILE_LEASE_OK ||
	    reader.context != TEST_READER_CONTEXT ||
	    reader.size != TEST_FILE_SIZE || reader.read != test_read)
		return 8;
	if (reader.read(reader.context, 7u, &byte, sizeof(byte), sizeof(byte),
			&bytes_read) != DOS_IMAGE_READ_OK ||
	    byte != 0x5au || bytes_read != 1u || read_calls != 1u ||
	    event_count != 3u || events[2] != EVENT_READ)
		return 9;
	if (dos_exec_file_lease_close(&table, first, &test_ops, TEST_CONTEXT) !=
		DOS_EXEC_FILE_LEASE_OK ||
	    close_calls != 1u || event_count != 4u ||
	    events[3] != EVENT_CLOSE ||
	    !dos_exec_file_lease_table_is_drained(&table))
		return 10;
	byte = 0u;
	bytes_read = BYTE_SENTINEL;
	if (reader.read(reader.context, 7u, &byte, sizeof(byte), sizeof(byte),
			&bytes_read) != DOS_IMAGE_READ_IO_ERROR ||
	    byte != 0u || bytes_read != BYTE_SENTINEL || read_calls != 2u)
		return 11;
	calls_before = close_calls;
	if (dos_exec_file_lease_close(&table, first, &test_ops, TEST_CONTEXT) !=
		DOS_EXEC_FILE_LEASE_OK ||
	    dos_exec_file_lease_abort(&table, first, &test_ops, TEST_CONTEXT) !=
		DOS_EXEC_FILE_LEASE_OK ||
	    close_calls != calls_before)
		return 12;
	if (dos_exec_file_lease_retire(&table, first) != DOS_EXEC_FILE_LEASE_OK)
		return 13;
	is_device = BYTE_SENTINEL;
	if (dos_exec_file_lease_query_device(&table, first, &test_ops,
					     TEST_CONTEXT, &is_device) !=
		DOS_EXEC_FILE_LEASE_STALE_HANDLE ||
	    is_device != BYTE_SENTINEL)
		return 14;
	configured_reader_context = TEST_NEXT_READER_CONTEXT;
	if (test_acquire(&table, &test_ops, TEST_CONTEXT, test_path,
			 ARRAY_SIZE(test_path),
			 &second) != DOS_EXEC_FILE_LEASE_OK ||
	    handle_slot(second) != handle_slot(first) ||
	    second.value == first.value ||
	    dos_exec_file_lease_close(&table, first, &test_ops, TEST_CONTEXT) !=
		DOS_EXEC_FILE_LEASE_STALE_HANDLE)
		return 15;
	if (dos_exec_file_lease_abort(&table, second, &test_ops,
				      TEST_CONTEXT) != DOS_EXEC_FILE_LEASE_OK ||
	    dos_exec_file_lease_retire(&table, second) !=
		DOS_EXEC_FILE_LEASE_OK)
		return 16;
	return 0;
}

static int test_device_rejected_as_reader(void)
{
	struct dos_exec_file_lease_table table;
	struct dos_exec_file_lease_handle handle;
	struct dos_image_reader reader;
	uint8_t is_device = BYTE_SENTINEL;

	reset_adapter();
	configured_is_device = 1u;
	if (!initialize_table(&table) ||
	    test_acquire(&table, &test_ops, TEST_CONTEXT, test_path,
			 ARRAY_SIZE(test_path),
			 &handle) != DOS_EXEC_FILE_LEASE_OK ||
	    test_probe(&table, handle, &test_ops, TEST_CONTEXT, &is_device) !=
		DOS_EXEC_FILE_LEASE_OK ||
	    is_device != 1u)
		return 1;
	set_reader_sentinel(&reader);
	if (dos_exec_file_lease_resolve_reader(&table, handle, &test_ops,
					       TEST_CONTEXT, &reader) !=
		DOS_EXEC_FILE_LEASE_IS_DEVICE ||
	    !reader_is_sentinel(&reader) || read_calls != 0u)
		return 2;
	if (dos_exec_file_lease_close(&table, handle, &test_ops,
				      TEST_CONTEXT) != DOS_EXEC_FILE_LEASE_OK ||
	    dos_exec_file_lease_retire(&table, handle) !=
		DOS_EXEC_FILE_LEASE_OK)
		return 3;
	return 0;
}

static int test_open_and_probe_failures(void)
{
	struct dos_exec_file_lease_table table;
	struct dos_exec_file_lease_handle handle;
	struct dos_image_reader reader;
	uint8_t is_device;

	reset_adapter();
	if (!initialize_table(&table))
		return 1;
	set_handle_sentinel(&handle);
	if (test_acquire(&table, &test_ops, TEST_CONTEXT, NULL, 0u, &handle) !=
		DOS_EXEC_FILE_LEASE_INVALID_ARGUMENT ||
	    !handle_is_sentinel(handle) ||
	    open_failure_detail != DETAIL_SENTINEL || open_calls != 0u)
		return 2;
	configured_open_status = DOS_EXEC_FILE_ADAPTER_FAULT;
	configured_failure_detail = 0x00000002u;
	set_handle_sentinel(&handle);
	open_failure_detail = DETAIL_SENTINEL;
	if (test_acquire(&table, &test_ops, TEST_CONTEXT, test_path,
			 ARRAY_SIZE(test_path),
			 &handle) != DOS_EXEC_FILE_LEASE_OPEN_FAILED ||
	    !handle_is_sentinel(handle) || open_failure_detail != 0x00000002u ||
	    open_calls != 1u || close_calls != 0u ||
	    !dos_exec_file_lease_table_is_drained(&table))
		return 3;
	configured_failure_detail = 0x00000003u;
	set_handle_sentinel(&handle);
	open_failure_detail = DETAIL_SENTINEL;
	if (test_acquire(&table, &test_ops, TEST_CONTEXT, test_path,
			 ARRAY_SIZE(test_path),
			 &handle) != DOS_EXEC_FILE_LEASE_OPEN_FAILED ||
	    !handle_is_sentinel(handle) || open_failure_detail != 0x00000003u ||
	    open_calls != 2u || close_calls != 0u ||
	    !dos_exec_file_lease_table_is_drained(&table))
		return 4;

	configured_open_status = DOS_EXEC_FILE_ADAPTER_OK;
	configured_failure_detail = 0u;
	configured_probe_status = DOS_EXEC_FILE_ADAPTER_FAULT;
	configured_probe_failure_detail = 0x00000004u;
	if (test_acquire(&table, &test_ops, TEST_CONTEXT, test_path,
			 ARRAY_SIZE(test_path),
			 &handle) != DOS_EXEC_FILE_LEASE_OK)
		return 5;
	is_device = BYTE_SENTINEL;
	probe_failure_detail = DETAIL_SENTINEL;
	if (test_probe(&table, handle, &test_ops, TEST_CONTEXT, &is_device) !=
		DOS_EXEC_FILE_LEASE_PROBE_FAILED ||
	    is_device != BYTE_SENTINEL || probe_failure_detail != 0x00000004u ||
	    probe_calls != 1u ||
	    table.slots[handle_slot(handle)].state !=
		(uint8_t)DOS_EXEC_FILE_LEASE_STATE_OPEN)
		return 6;
	configured_probe_failure_detail = 0x00000005u;
	probe_failure_detail = DETAIL_SENTINEL;
	if (test_probe(&table, handle, &test_ops, TEST_CONTEXT, &is_device) !=
		DOS_EXEC_FILE_LEASE_PROBE_FAILED ||
	    is_device != BYTE_SENTINEL || probe_failure_detail != 0x00000005u ||
	    probe_calls != 2u)
		return 7;
	set_reader_sentinel(&reader);
	if (dos_exec_file_lease_resolve_reader(&table, handle, &test_ops,
					       TEST_CONTEXT, &reader) !=
		DOS_EXEC_FILE_LEASE_INVALID_STATE ||
	    !reader_is_sentinel(&reader))
		return 8;
	configured_close_result = DOS_EXEC_FILE_CLOSE_RETAINED;
	if (dos_exec_file_lease_abort(&table, handle, &test_ops,
				      TEST_CONTEXT) !=
		DOS_EXEC_FILE_LEASE_CLOSE_RETAINED ||
	    table.slots[handle_slot(handle)].state !=
		(uint8_t)DOS_EXEC_FILE_LEASE_STATE_OPEN)
		return 9;
	configured_close_result = DOS_EXEC_FILE_CLOSE_CLOSED;
	if (dos_exec_file_lease_abort(&table, handle, &test_ops,
				      TEST_CONTEXT) != DOS_EXEC_FILE_LEASE_OK ||
	    dos_exec_file_lease_retire(&table, handle) !=
		DOS_EXEC_FILE_LEASE_OK)
		return 10;

	configured_probe_status = DOS_EXEC_FILE_ADAPTER_OK;
	configured_probe_failure_detail = 0u;
	configured_is_device = 2u;
	if (test_acquire(&table, &test_ops, TEST_CONTEXT, test_path,
			 ARRAY_SIZE(test_path),
			 &handle) != DOS_EXEC_FILE_LEASE_OK)
		return 11;
	is_device = BYTE_SENTINEL;
	probe_failure_detail = DETAIL_SENTINEL;
	if (test_probe(&table, handle, &test_ops, TEST_CONTEXT, &is_device) !=
		DOS_EXEC_FILE_LEASE_POISONED ||
	    is_device != BYTE_SENTINEL ||
	    probe_failure_detail != DETAIL_SENTINEL ||
	    dos_exec_file_lease_table_is_drained(&table))
		return 12;

	configured_is_device = 0u;
	configured_reader_context = 0u;
	set_handle_sentinel(&handle);
	if (test_acquire(&table, &test_ops, TEST_CONTEXT, test_path,
			 ARRAY_SIZE(test_path),
			 &handle) != DOS_EXEC_FILE_LEASE_POISONED ||
	    !handle_is_sentinel(handle) || open_failure_detail != 0u ||
	    dos_exec_file_lease_table_is_drained(&table))
		return 13;
	return 0;
}

static int test_adapter_contract_violations(void)
{
	struct dos_exec_file_lease_table open_detail_table;
	struct dos_exec_file_lease_table open_reserved_table;
	struct dos_exec_file_lease_table open_status_table;
	struct dos_exec_file_lease_table probe_reserved_table;
	struct dos_exec_file_lease_table probe_status_table;
	struct dos_exec_file_lease_handle handle;
	uint8_t is_device;

	reset_adapter();
	if (!initialize_table(&open_detail_table))
		return 1;
	configured_failure_detail = 1u;
	set_handle_sentinel(&handle);
	if (test_acquire(&open_detail_table, &test_ops, TEST_CONTEXT, test_path,
			 ARRAY_SIZE(test_path),
			 &handle) != DOS_EXEC_FILE_LEASE_POISONED ||
	    !handle_is_sentinel(handle) || open_failure_detail != 0u ||
	    dos_exec_file_lease_table_is_drained(&open_detail_table))
		return 2;

	reset_adapter();
	if (!initialize_table(&open_reserved_table))
		return 3;
	configured_open_reserved = 1u;
	set_handle_sentinel(&handle);
	if (test_acquire(&open_reserved_table, &test_ops, TEST_CONTEXT,
			 test_path, ARRAY_SIZE(test_path),
			 &handle) != DOS_EXEC_FILE_LEASE_POISONED ||
	    !handle_is_sentinel(handle) || open_failure_detail != 0u ||
	    dos_exec_file_lease_table_is_drained(&open_reserved_table))
		return 4;

	reset_adapter();
	if (!initialize_table(&open_status_table))
		return 5;
	configured_open_status = (enum dos_exec_file_adapter_status)2u;
	set_handle_sentinel(&handle);
	if (test_acquire(&open_status_table, &test_ops, TEST_CONTEXT, test_path,
			 ARRAY_SIZE(test_path),
			 &handle) != DOS_EXEC_FILE_LEASE_POISONED ||
	    !handle_is_sentinel(handle) ||
	    open_failure_detail != DETAIL_SENTINEL ||
	    dos_exec_file_lease_table_is_drained(&open_status_table))
		return 6;

	reset_adapter();
	if (!initialize_table(&probe_reserved_table) ||
	    test_acquire(&probe_reserved_table, &test_ops, TEST_CONTEXT,
			 test_path, ARRAY_SIZE(test_path),
			 &handle) != DOS_EXEC_FILE_LEASE_OK)
		return 7;
	configured_probe_reserved = 1u;
	is_device = BYTE_SENTINEL;
	probe_failure_detail = DETAIL_SENTINEL;
	if (test_probe(&probe_reserved_table, handle, &test_ops, TEST_CONTEXT,
		       &is_device) != DOS_EXEC_FILE_LEASE_POISONED ||
	    is_device != BYTE_SENTINEL ||
	    probe_failure_detail != DETAIL_SENTINEL ||
	    dos_exec_file_lease_table_is_drained(&probe_reserved_table))
		return 8;

	reset_adapter();
	if (!initialize_table(&probe_status_table) ||
	    test_acquire(&probe_status_table, &test_ops, TEST_CONTEXT,
			 test_path, ARRAY_SIZE(test_path),
			 &handle) != DOS_EXEC_FILE_LEASE_OK)
		return 9;
	configured_probe_status = (enum dos_exec_file_adapter_status)2u;
	configured_probe_failure_detail = 0x00000006u;
	is_device = BYTE_SENTINEL;
	probe_failure_detail = DETAIL_SENTINEL;
	if (test_probe(&probe_status_table, handle, &test_ops, TEST_CONTEXT,
		       &is_device) != DOS_EXEC_FILE_LEASE_POISONED ||
	    is_device != BYTE_SENTINEL ||
	    probe_failure_detail != DETAIL_SENTINEL ||
	    dos_exec_file_lease_table_is_drained(&probe_status_table))
		return 10;
	return 0;
}

static int test_binding_and_unchanged_outputs(void)
{
	struct dos_exec_file_lease_table table;
	struct dos_exec_file_lease_handle handle;
	struct dos_exec_file_lease_handle stale;
	struct dos_exec_file_lease_ops wrong_ops = test_ops;
	struct dos_image_reader reader;
	uint8_t is_device = BYTE_SENTINEL;
	uint32_t close_before;

	reset_adapter();
	if (!initialize_table(&table) ||
	    test_acquire(&table, &test_ops, TEST_CONTEXT, test_path,
			 ARRAY_SIZE(test_path),
			 &handle) != DOS_EXEC_FILE_LEASE_OK ||
	    test_probe(&table, handle, &test_ops, TEST_CONTEXT, &is_device) !=
		DOS_EXEC_FILE_LEASE_OK)
		return 1;
	wrong_ops.identity = TEST_OTHER_IDENTITY;
	set_reader_sentinel(&reader);
	is_device = BYTE_SENTINEL;
	probe_failure_detail = DETAIL_SENTINEL;
	close_before = close_calls;
	if (test_probe(&table, handle, &wrong_ops, TEST_CONTEXT, &is_device) !=
		DOS_EXEC_FILE_LEASE_IDENTITY_MISMATCH ||
	    is_device != BYTE_SENTINEL ||
	    probe_failure_detail != DETAIL_SENTINEL ||
	    dos_exec_file_lease_resolve_reader(&table, handle, &wrong_ops,
					       TEST_CONTEXT, &reader) !=
		DOS_EXEC_FILE_LEASE_IDENTITY_MISMATCH ||
	    !reader_is_sentinel(&reader) ||
	    dos_exec_file_lease_close(&table, handle, &wrong_ops,
				      TEST_CONTEXT) !=
		DOS_EXEC_FILE_LEASE_IDENTITY_MISMATCH ||
	    close_calls != close_before)
		return 2;
	set_reader_sentinel(&reader);
	if (dos_exec_file_lease_resolve_reader(&table, handle, &test_ops,
					       TEST_OTHER_CONTEXT, &reader) !=
		DOS_EXEC_FILE_LEASE_CONTEXT_MISMATCH ||
	    !reader_is_sentinel(&reader) ||
	    dos_exec_file_lease_abort(&table, handle, &test_ops,
				      TEST_OTHER_CONTEXT) !=
		DOS_EXEC_FILE_LEASE_CONTEXT_MISMATCH ||
	    close_calls != close_before)
		return 3;
	table.identity = KERNEL_OBJECT_HANDLE_INVALID;
	is_device = BYTE_SENTINEL;
	if (dos_exec_file_lease_query_device(&table, handle, &test_ops,
					     TEST_CONTEXT, &is_device) !=
		DOS_EXEC_FILE_LEASE_POISONED ||
	    is_device != BYTE_SENTINEL || close_calls != close_before)
		return 4;
	table.identity = TEST_TABLE_IDENTITY;
	stale = handle;
	stale.value += 1ull << DOS_EXEC_FILE_LEASE_SLOT_BITS;
	is_device = BYTE_SENTINEL;
	if (dos_exec_file_lease_query_device(&table, stale, &test_ops,
					     TEST_CONTEXT, &is_device) !=
		DOS_EXEC_FILE_LEASE_STALE_HANDLE ||
	    is_device != BYTE_SENTINEL)
		return 5;
	if (dos_exec_file_lease_close(&table, handle, &test_ops,
				      TEST_CONTEXT) != DOS_EXEC_FILE_LEASE_OK ||
	    dos_exec_file_lease_retire(&table, handle) !=
		DOS_EXEC_FILE_LEASE_OK)
		return 6;
	return 0;
}

static int test_retained_retry(void)
{
	struct dos_exec_file_lease_table table;
	struct dos_exec_file_lease_handle handle;
	struct dos_image_reader reader;
	uint8_t is_device;

	reset_adapter();
	if (!initialize_table(&table) ||
	    test_acquire(&table, &test_ops, TEST_CONTEXT, test_path,
			 ARRAY_SIZE(test_path),
			 &handle) != DOS_EXEC_FILE_LEASE_OK ||
	    test_probe(&table, handle, &test_ops, TEST_CONTEXT, &is_device) !=
		DOS_EXEC_FILE_LEASE_OK)
		return 1;
	configured_close_result = DOS_EXEC_FILE_CLOSE_RETAINED;
	if (dos_exec_file_lease_close(&table, handle, &test_ops,
				      TEST_CONTEXT) !=
		DOS_EXEC_FILE_LEASE_CLOSE_RETAINED ||
	    close_calls != 1u ||
	    table.slots[handle_slot(handle)].state !=
		(uint8_t)DOS_EXEC_FILE_LEASE_STATE_PROBED ||
	    dos_exec_file_lease_table_is_drained(&table))
		return 2;
	set_reader_sentinel(&reader);
	if (dos_exec_file_lease_resolve_reader(&table, handle, &test_ops,
					       TEST_CONTEXT, &reader) !=
		DOS_EXEC_FILE_LEASE_OK ||
	    reader.read != test_read)
		return 3;
	configured_close_result = DOS_EXEC_FILE_CLOSE_CLOSED;
	if (dos_exec_file_lease_abort(&table, handle, &test_ops,
				      TEST_CONTEXT) != DOS_EXEC_FILE_LEASE_OK ||
	    close_calls != 2u ||
	    dos_exec_file_lease_retire(&table, handle) !=
		DOS_EXEC_FILE_LEASE_OK)
		return 4;
	return 0;
}

static int test_uncertain_poison(void)
{
	struct dos_exec_file_lease_table table;
	struct dos_exec_file_lease_handle handle;
	struct dos_image_reader reader;
	uint8_t is_device;
	uint32_t close_before;

	reset_adapter();
	if (!initialize_table(&table) ||
	    test_acquire(&table, &test_ops, TEST_CONTEXT, test_path,
			 ARRAY_SIZE(test_path),
			 &handle) != DOS_EXEC_FILE_LEASE_OK ||
	    test_probe(&table, handle, &test_ops, TEST_CONTEXT, &is_device) !=
		DOS_EXEC_FILE_LEASE_OK)
		return 1;
	configured_close_result = DOS_EXEC_FILE_CLOSE_UNCERTAIN;
	if (dos_exec_file_lease_close(&table, handle, &test_ops,
				      TEST_CONTEXT) !=
		DOS_EXEC_FILE_LEASE_POISONED ||
	    close_calls != 1u ||
	    table.slots[handle_slot(handle)].state !=
		(uint8_t)DOS_EXEC_FILE_LEASE_STATE_POISONED ||
	    dos_exec_file_lease_table_is_drained(&table))
		return 2;
	close_before = close_calls;
	set_reader_sentinel(&reader);
	is_device = BYTE_SENTINEL;
	if (dos_exec_file_lease_resolve_reader(&table, handle, &test_ops,
					       TEST_CONTEXT, &reader) !=
		DOS_EXEC_FILE_LEASE_POISONED ||
	    !reader_is_sentinel(&reader) ||
	    dos_exec_file_lease_query_device(&table, handle, &test_ops,
					     TEST_CONTEXT, &is_device) !=
		DOS_EXEC_FILE_LEASE_POISONED ||
	    is_device != BYTE_SENTINEL ||
	    dos_exec_file_lease_close(&table, handle, &test_ops,
				      TEST_CONTEXT) !=
		DOS_EXEC_FILE_LEASE_POISONED ||
	    dos_exec_file_lease_abort(&table, handle, &test_ops,
				      TEST_CONTEXT) !=
		DOS_EXEC_FILE_LEASE_POISONED ||
	    dos_exec_file_lease_retire(&table, handle) !=
		DOS_EXEC_FILE_LEASE_POISONED ||
	    close_calls != close_before)
		return 3;
	return 0;
}

static int test_slots_and_terminal_generation(void)
{
	struct dos_exec_file_lease_handle
	    handles[DOS_EXEC_FILE_LEASE_SLOT_COUNT];
	struct dos_exec_file_lease_table table;
	struct dos_exec_file_lease_handle output;
	uint32_t index;

	reset_adapter();
	if (!initialize_table(&table))
		return 1;
	for (index = 0u; index < DOS_EXEC_FILE_LEASE_SLOT_COUNT; ++index) {
		if (test_acquire(&table, &test_ops, TEST_CONTEXT, test_path,
				 ARRAY_SIZE(test_path),
				 &handles[index]) != DOS_EXEC_FILE_LEASE_OK)
			return 2;
	}
	set_handle_sentinel(&output);
	open_failure_detail = DETAIL_SENTINEL;
	if (test_acquire(&table, &test_ops, TEST_CONTEXT, test_path,
			 ARRAY_SIZE(test_path),
			 &output) != DOS_EXEC_FILE_LEASE_NO_SLOT ||
	    !handle_is_sentinel(output) ||
	    open_failure_detail != DETAIL_SENTINEL ||
	    open_calls != DOS_EXEC_FILE_LEASE_SLOT_COUNT)
		return 3;
	for (index = DOS_EXEC_FILE_LEASE_SLOT_COUNT; index != 0u; --index) {
		if (dos_exec_file_lease_abort(&table, handles[index - 1u],
					      &test_ops, TEST_CONTEXT) !=
			DOS_EXEC_FILE_LEASE_OK ||
		    dos_exec_file_lease_retire(&table, handles[index - 1u]) !=
			DOS_EXEC_FILE_LEASE_OK)
			return 4;
	}

	if (!initialize_table(&table))
		return 5;
	reset_adapter();
	for (index = 0u; index < DOS_EXEC_FILE_LEASE_SLOT_COUNT; ++index)
		table.slots[index].generation =
		    DOS_EXEC_FILE_LEASE_GENERATION_MAX;
	set_handle_sentinel(&output);
	open_failure_detail = DETAIL_SENTINEL;
	if (test_acquire(&table, &test_ops, TEST_CONTEXT, test_path,
			 ARRAY_SIZE(test_path),
			 &output) != DOS_EXEC_FILE_LEASE_GENERATION_EXHAUSTED ||
	    !handle_is_sentinel(output) ||
	    open_failure_detail != DETAIL_SENTINEL || open_calls != 0u)
		return 6;
	table.slots[0].generation = DOS_EXEC_FILE_LEASE_GENERATION_MAX - 1u;
	if (test_acquire(&table, &test_ops, TEST_CONTEXT, test_path,
			 ARRAY_SIZE(test_path),
			 &output) != DOS_EXEC_FILE_LEASE_OK ||
	    (output.value >> DOS_EXEC_FILE_LEASE_SLOT_BITS) !=
		DOS_EXEC_FILE_LEASE_GENERATION_MAX ||
	    output.value == KERNEL_OBJECT_HANDLE_INVALID)
		return 7;
	if (dos_exec_file_lease_close(&table, output, &test_ops,
				      TEST_CONTEXT) != DOS_EXEC_FILE_LEASE_OK ||
	    dos_exec_file_lease_retire(&table, output) !=
		DOS_EXEC_FILE_LEASE_OK ||
	    dos_exec_file_lease_close(&table, output, &test_ops,
				      TEST_CONTEXT) !=
		DOS_EXEC_FILE_LEASE_STALE_HANDLE)
		return 8;
	set_handle_sentinel(&output);
	open_failure_detail = DETAIL_SENTINEL;
	if (test_acquire(&table, &test_ops, TEST_CONTEXT, test_path,
			 ARRAY_SIZE(test_path),
			 &output) != DOS_EXEC_FILE_LEASE_GENERATION_EXHAUSTED ||
	    !handle_is_sentinel(output) ||
	    open_failure_detail != DETAIL_SENTINEL || open_calls != 1u)
		return 9;
	return 0;
}

static int run_tests(void)
{
	int result = test_success_and_aba();

	if (result != 0)
		return 10 + result;
	result = test_device_rejected_as_reader();
	if (result != 0)
		return 30 + result;
	result = test_open_and_probe_failures();
	if (result != 0)
		return 50 + result;
	result = test_adapter_contract_violations();
	if (result != 0)
		return 80 + result;
	result = test_binding_and_unchanged_outputs();
	if (result != 0)
		return 100 + result;
	result = test_retained_retry();
	if (result != 0)
		return 120 + result;
	result = test_uncertain_poison();
	if (result != 0)
		return 140 + result;
	result = test_slots_and_terminal_generation();
	if (result != 0)
		return 170 + result;
	return 0;
}

DOSC32_TEST_ENTRY(run_tests)

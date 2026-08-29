// SPDX-License-Identifier: GPL-2.0-only
/* Freestanding I/O Manager named character-device lifecycle tests. */
#include "iomgr_device.h"
#include "test_entry.h"

#define TEST_REGISTRATION_SLOTS 16u
#define TEST_INSTANCE_SLOTS 32u
#define TEST_DEVICE_ID 0x4445564943453031ull
#define TEST_DRIVER_CONTEXT ((kernel_object_handle_t)0x101u)
#define TEST_INSTANCE_CONTEXT_BASE ((kernel_object_handle_t)0x1000u)
#define TEST_HANDLE_SENTINEL ((kernel_object_handle_t)0xa5a55a5af00ff00full)

static const uint8_t stream_name_bytes[] = {
	'S', 'T', 'R', 'E', 'A', 'M'
};
#ifndef IOMGR_DEVICE_TEST_GENERATION_MAX
static const uint8_t short_name_bytes[] = { 'S', 'T', 'R' };
static const uint8_t missing_name_bytes[] = { 'M', 'I', 'S', 'S' };
#endif

static enum iomgr_device_callback_status open_status;
static enum iomgr_device_callback_status close_status;
static enum iomgr_device_callback_status read_status;
static enum iomgr_device_callback_status write_status;
static enum iomgr_device_callback_status control_status;
static enum iomgr_device_callback_status query_status;
static struct iomgr_device_query_result query_result;
static kernel_object_handle_t next_instance_context;
static kernel_object_handle_t forced_instance_context;
static size_t read_completion;
static size_t write_completion;
static size_t control_completion;
static uint32_t open_calls;
static uint32_t close_calls;
static uint32_t read_calls;
static uint32_t write_calls;
static uint32_t control_calls;
static uint32_t query_calls;
static uint64_t observed_control_operation;
static size_t observed_write_count;
static uint8_t observed_write[16];
static bool reenter_on_read;
static iomgr_device_handle_t reentry_device;
static enum iomgr_status reentry_status;

static bool instance_context_is_valid(kernel_object_handle_t context)
{
	return context >= TEST_INSTANCE_CONTEXT_BASE &&
	       context != KERNEL_OBJECT_HANDLE_INVALID;
}

static void reset_callbacks(void)
{
	open_status = IOMGR_DEVICE_CALLBACK_OK;
	close_status = IOMGR_DEVICE_CALLBACK_OK;
	read_status = IOMGR_DEVICE_CALLBACK_OK;
	write_status = IOMGR_DEVICE_CALLBACK_OK;
	control_status = IOMGR_DEVICE_CALLBACK_OK;
	query_status = IOMGR_DEVICE_CALLBACK_OK;
	query_result = (struct iomgr_device_query_result){
		.pending_read_bytes = 0u,
		.state = IOMGR_DEVICE_STATE_WRITE_READY,
		.reserved = 0u,
	};
	next_instance_context = TEST_INSTANCE_CONTEXT_BASE;
	forced_instance_context = KERNEL_OBJECT_HANDLE_INVALID;
	read_completion = 0u;
	write_completion = 0u;
	control_completion = 0u;
	open_calls = 0u;
	close_calls = 0u;
	read_calls = 0u;
	write_calls = 0u;
	control_calls = 0u;
	query_calls = 0u;
	observed_control_operation = 0u;
	observed_write_count = 0u;
	reenter_on_read = false;
	reentry_device = IOMGR_DEVICE_HANDLE_INVALID;
	reentry_status = IOMGR_OK;
}

static enum iomgr_device_callback_status
test_open(kernel_object_handle_t context,
	  kernel_object_handle_t *instance_context)
{
	kernel_object_handle_t prepared;

	++open_calls;
	if (context != TEST_DRIVER_CONTEXT || instance_context == NULL)
		return IOMGR_DEVICE_CALLBACK_UNCERTAIN;
	if (open_status != IOMGR_DEVICE_CALLBACK_OK)
		return open_status;
	prepared = forced_instance_context != KERNEL_OBJECT_HANDLE_INVALID
			   ? forced_instance_context
			   : next_instance_context++;
	*instance_context = prepared;
	return IOMGR_DEVICE_CALLBACK_OK;
}

static enum iomgr_device_callback_status
test_close(kernel_object_handle_t context,
	   kernel_object_handle_t instance_context)
{
	++close_calls;
	if (context != TEST_DRIVER_CONTEXT ||
	    !instance_context_is_valid(instance_context))
		return IOMGR_DEVICE_CALLBACK_UNCERTAIN;
	return close_status;
}

static enum iomgr_device_callback_status
test_read(kernel_object_handle_t context,
	  kernel_object_handle_t instance_context, uint8_t *destination,
	  size_t capacity, size_t count, size_t *bytes_read)
{
	size_t index;
	size_t writable;

	++read_calls;
	if (context != TEST_DRIVER_CONTEXT ||
	    !instance_context_is_valid(instance_context) ||
	    bytes_read == NULL || count > capacity ||
	    (capacity != 0u && destination == NULL))
		return IOMGR_DEVICE_CALLBACK_UNCERTAIN;
	if (reenter_on_read) {
		reenter_on_read = false;
		reentry_status = iomgr_device_close(reentry_device);
	}
	if (read_status != IOMGR_DEVICE_CALLBACK_OK)
		return read_status;
	writable = read_completion < capacity ? read_completion : capacity;
	for (index = 0u; index < writable; ++index)
		destination[index] = (uint8_t)(0xa0u + index);
	*bytes_read = read_completion;
	return IOMGR_DEVICE_CALLBACK_OK;
}

static enum iomgr_device_callback_status
test_write(kernel_object_handle_t context,
	   kernel_object_handle_t instance_context, const uint8_t *source,
	   size_t source_capacity, size_t count, size_t *bytes_written)
{
	size_t index;
	size_t observable;

	++write_calls;
	if (context != TEST_DRIVER_CONTEXT ||
	    !instance_context_is_valid(instance_context) ||
	    bytes_written == NULL || count > source_capacity ||
	    (source_capacity != 0u && source == NULL))
		return IOMGR_DEVICE_CALLBACK_UNCERTAIN;
	if (write_status != IOMGR_DEVICE_CALLBACK_OK)
		return write_status;
	observable = count < ARRAY_SIZE(observed_write)
			     ? count
			     : ARRAY_SIZE(observed_write);
	for (index = 0u; index < observable; ++index)
		observed_write[index] = source[index];
	observed_write_count = count;
	*bytes_written = write_completion;
	return IOMGR_DEVICE_CALLBACK_OK;
}

static enum iomgr_device_callback_status test_control(
	kernel_object_handle_t context, kernel_object_handle_t instance_context,
	uint64_t operation, const uint8_t *input, size_t input_capacity,
	size_t input_count, uint8_t *output, size_t output_capacity,
	size_t *bytes_returned)
{
	size_t writable;

	++control_calls;
	if (context != TEST_DRIVER_CONTEXT ||
	    !instance_context_is_valid(instance_context) ||
	    bytes_returned == NULL || input_count > input_capacity ||
	    (input_capacity != 0u && input == NULL) ||
	    (output_capacity != 0u && output == NULL))
		return IOMGR_DEVICE_CALLBACK_UNCERTAIN;
	if (control_status != IOMGR_DEVICE_CALLBACK_OK)
		return control_status;
	observed_control_operation = operation;
	writable = control_completion < output_capacity
			   ? control_completion
			   : output_capacity;
	if (writable != 0u)
		output[0] = (uint8_t)operation;
	if (writable > 1u)
		output[1] = input_count != 0u ? input[0] : 0u;
	*bytes_returned = control_completion;
	return IOMGR_DEVICE_CALLBACK_OK;
}

static enum iomgr_device_callback_status
test_query(kernel_object_handle_t context,
	   kernel_object_handle_t instance_context,
	   struct iomgr_device_query_result *result)
{
	++query_calls;
	if (context != TEST_DRIVER_CONTEXT ||
	    !instance_context_is_valid(instance_context) || result == NULL)
		return IOMGR_DEVICE_CALLBACK_UNCERTAIN;
	if (query_status != IOMGR_DEVICE_CALLBACK_OK)
		return query_status;
	*result = query_result;
	return IOMGR_DEVICE_CALLBACK_OK;
}

static struct iomgr_device_ops make_ops(uint64_t identity,
					uint32_t capabilities)
{
	struct iomgr_device_ops ops = {
		.abi_version = IOMGR_DEVICE_ABI_VERSION,
		.reserved = 0u,
		.identity = identity,
		.context = TEST_DRIVER_CONTEXT,
		.capabilities = capabilities,
		.reserved2 = 0u,
		.open = test_open,
		.close = test_close,
		.read = NULL,
		.write = NULL,
		.control = NULL,
		.query_info = test_query,
	};

	if ((capabilities & IOMGR_DEVICE_CAP_READ) != 0u)
		ops.read = test_read;
	if ((capabilities & IOMGR_DEVICE_CAP_WRITE) != 0u)
		ops.write = test_write;
	if ((capabilities & IOMGR_DEVICE_CAP_CONTROL) != 0u)
		ops.control = test_control;
	return ops;
}

static struct iomgr_device_name make_name(const uint8_t *bytes, size_t length)
{
	return (struct iomgr_device_name){
		.bytes = bytes,
		.length = length,
	};
}

#ifndef IOMGR_DEVICE_TEST_GENERATION_MAX
static bool bytes_match(const uint8_t *left, const uint8_t *right,
			size_t count)
{
	size_t index;

	for (index = 0u; index < count; ++index) {
		if (left[index] != right[index])
			return false;
	}
	return true;
}

static int test_initialization_and_validation(void)
{
	struct iomgr_device_ops ops = make_ops(
		TEST_DEVICE_ID, IOMGR_DEVICE_CAP_READ | IOMGR_DEVICE_CAP_WRITE |
				IOMGR_DEVICE_CAP_CONTROL);
	struct iomgr_device_name stream_name = make_name(
		stream_name_bytes, ARRAY_SIZE(stream_name_bytes));
	struct iomgr_device_name short_name = make_name(
		short_name_bytes, ARRAY_SIZE(short_name_bytes));
	struct iomgr_device_name invalid_name = make_name(stream_name_bytes, 0u);
	iomgr_device_registration_handle_t registration = TEST_HANDLE_SENTINEL;
	iomgr_device_registration_handle_t prefix_registration;
	iomgr_device_registration_handle_t replacement;
	uint8_t maximum_name_bytes[IOMGR_DEVICE_NAME_MAX_BYTES];
	struct iomgr_device_name maximum_name = make_name(
		maximum_name_bytes, ARRAY_SIZE(maximum_name_bytes));
	iomgr_device_handle_t device = TEST_HANDLE_SENTINEL;
	struct iomgr_device_info info;
	size_t index;

	if (iomgr_device_register(&stream_name, &ops, &registration) !=
		IOMGR_NOT_INITIALIZED ||
	    iomgr_device_unregister(registration) != IOMGR_NOT_INITIALIZED ||
	    iomgr_device_open(&stream_name, &device) != IOMGR_NOT_INITIALIZED ||
	    iomgr_device_close(device) != IOMGR_NOT_INITIALIZED ||
	    iomgr_device_query_info(device, &info) != IOMGR_NOT_INITIALIZED ||
	    registration != TEST_HANDLE_SENTINEL ||
	    device != TEST_HANDLE_SENTINEL ||
	    iomgr_device_initialize() != IOMGR_OK ||
	    iomgr_device_initialize() != IOMGR_ALREADY_INITIALIZED)
		return 1;
	if (iomgr_device_register(NULL, &ops, &registration) !=
		IOMGR_INVALID_ARGUMENT ||
	    iomgr_device_register(&invalid_name, &ops, &registration) !=
		IOMGR_INVALID_ARGUMENT)
		return 2;
	invalid_name.length = IOMGR_DEVICE_NAME_MAX_BYTES + 1u;
	if (iomgr_device_register(&invalid_name, &ops, &registration) !=
		IOMGR_INVALID_ARGUMENT ||
	    iomgr_device_register(&stream_name, NULL, &registration) !=
		IOMGR_INVALID_ARGUMENT ||
	    iomgr_device_register(&stream_name, &ops, NULL) !=
		IOMGR_INVALID_ARGUMENT)
		return 3;

	ops.abi_version = 0u;
	if (iomgr_device_register(&stream_name, &ops, &registration) !=
		IOMGR_INVALID_ARGUMENT)
		return 4;
	ops = make_ops(TEST_DEVICE_ID, IOMGR_DEVICE_CAP_READ);
	ops.read = NULL;
	if (iomgr_device_register(&stream_name, &ops, &registration) !=
		IOMGR_INVALID_ARGUMENT)
		return 5;
	ops = make_ops(TEST_DEVICE_ID, 0u);
	ops.read = test_read;
	if (iomgr_device_register(&stream_name, &ops, &registration) !=
		IOMGR_INVALID_ARGUMENT)
		return 6;
	ops = make_ops(TEST_DEVICE_ID, IOMGR_DEVICE_CAP_READ |
					   IOMGR_DEVICE_CAP_WRITE |
					   IOMGR_DEVICE_CAP_CONTROL);
	if (iomgr_device_register(&stream_name, &ops, &registration) !=
		IOMGR_OK ||
	    registration == TEST_HANDLE_SENTINEL ||
	    iomgr_device_register(&stream_name, &ops, &replacement) !=
		IOMGR_ALREADY_EXISTS ||
	    iomgr_device_register(&short_name, &ops, &prefix_registration) !=
		IOMGR_OK ||
	    iomgr_device_unregister(prefix_registration) != IOMGR_OK)
		return 7;
	for (index = 0u; index < ARRAY_SIZE(maximum_name_bytes); ++index)
		maximum_name_bytes[index] = (uint8_t)(index + 1u);
	if (iomgr_device_register(&maximum_name, &ops, &replacement) != IOMGR_OK ||
	    iomgr_device_unregister(replacement) != IOMGR_OK ||
	    iomgr_device_unregister(registration) != IOMGR_OK ||
	    iomgr_device_unregister(registration) != IOMGR_STALE_HANDLE)
		return 8;
	if (iomgr_device_register(&stream_name, &ops, &replacement) != IOMGR_OK ||
	    replacement == registration ||
	    iomgr_device_unregister(registration) != IOMGR_STALE_HANDLE ||
	    iomgr_device_unregister(replacement) != IOMGR_OK)
		return 9;
	return 0;
}

static int test_fixed_capacity(void)
{
	struct iomgr_device_ops ops = make_ops(
		TEST_DEVICE_ID, IOMGR_DEVICE_CAP_READ | IOMGR_DEVICE_CAP_WRITE |
				IOMGR_DEVICE_CAP_CONTROL);
	iomgr_device_registration_handle_t
		registrations[TEST_REGISTRATION_SLOTS];
	iomgr_device_handle_t devices[TEST_INSTANCE_SLOTS];
	uint8_t name_bytes[2] = { 'R', 0u };
	struct iomgr_device_name name = make_name(name_bytes,
						ARRAY_SIZE(name_bytes));
	iomgr_device_registration_handle_t extra_registration =
		TEST_HANDLE_SENTINEL;
	iomgr_device_handle_t extra_device = TEST_HANDLE_SENTINEL;
	size_t index;

	reset_callbacks();
	for (index = 0u; index < ARRAY_SIZE(registrations); ++index) {
		name_bytes[1] = (uint8_t)(index + 1u);
		ops.identity = TEST_DEVICE_ID + index;
		if (iomgr_device_register(&name, &ops, &registrations[index]) !=
			IOMGR_OK)
			return 1;
	}
	name_bytes[1] = 0xffu;
	if (iomgr_device_register(&name, &ops, &extra_registration) !=
		IOMGR_NO_SLOT ||
	    extra_registration != TEST_HANDLE_SENTINEL)
		return 2;
	for (index = ARRAY_SIZE(registrations); index != 0u; --index) {
		if (iomgr_device_unregister(registrations[index - 1u]) != IOMGR_OK)
			return 3;
	}

	name_bytes[1] = 1u;
	ops.identity = TEST_DEVICE_ID;
	if (iomgr_device_register(&name, &ops, &registrations[0]) != IOMGR_OK)
		return 4;
	for (index = 0u; index < ARRAY_SIZE(devices); ++index) {
		if (iomgr_device_open(&name, &devices[index]) != IOMGR_OK)
			return 5;
	}
	if (iomgr_device_open(&name, &extra_device) != IOMGR_NO_SLOT ||
	    extra_device != TEST_HANDLE_SENTINEL ||
	    iomgr_device_unregister(registrations[0]) != IOMGR_BUSY)
		return 6;
	for (index = ARRAY_SIZE(devices); index != 0u; --index) {
		if (iomgr_device_close(devices[index - 1u]) != IOMGR_OK)
			return 7;
	}
	if (iomgr_device_unregister(registrations[0]) != IOMGR_OK)
		return 8;
	return 0;
}

static int test_operations_and_precise_failures(void)
{
	struct iomgr_device_ops ops = make_ops(
		TEST_DEVICE_ID, IOMGR_DEVICE_CAP_READ | IOMGR_DEVICE_CAP_WRITE |
				IOMGR_DEVICE_CAP_CONTROL);
	struct iomgr_device_name stream_name = make_name(
		stream_name_bytes, ARRAY_SIZE(stream_name_bytes));
	struct iomgr_device_name missing_name = make_name(
		missing_name_bytes, ARRAY_SIZE(missing_name_bytes));
	iomgr_device_registration_handle_t registration;
	iomgr_device_handle_t device = TEST_HANDLE_SENTINEL;
	iomgr_device_handle_t replacement;
	struct iomgr_device_info info;
	uint8_t read_buffer[8] = { 0 };
	const uint8_t write_buffer[] = { 1u, 2u, 3u, 4u };
	const uint8_t input[] = { 0x5au };
	uint8_t output[4] = { 0 };
	size_t completed;
	uint32_t calls;

	reset_callbacks();
	if (iomgr_device_register(&stream_name, &ops, &registration) != IOMGR_OK ||
	    iomgr_device_open(&missing_name, &device) != IOMGR_NOT_FOUND ||
	    device != TEST_HANDLE_SENTINEL)
		return 1;
	open_status = IOMGR_DEVICE_CALLBACK_NO_RESOURCES;
	if (iomgr_device_open(&stream_name, &device) != IOMGR_NO_SLOT ||
	    device != TEST_HANDLE_SENTINEL)
		return 1;
	open_status = IOMGR_DEVICE_CALLBACK_OK;
	if (iomgr_device_open(&stream_name, &device) != IOMGR_OK ||
	    device == TEST_HANDLE_SENTINEL || device == registration ||
	    iomgr_device_unregister(device) != IOMGR_STALE_HANDLE ||
	    iomgr_device_close(registration) != IOMGR_STALE_HANDLE)
		return 1;
	query_result.pending_read_bytes = 7u;
	query_result.state = IOMGR_DEVICE_STATE_READ_READY |
			     IOMGR_DEVICE_STATE_WRITE_READY;
	if (iomgr_device_query_info(device, &info) != IOMGR_OK ||
	    info.identity != TEST_DEVICE_ID ||
	    info.pending_read_bytes != 7u ||
	    info.capabilities != ops.capabilities ||
	    info.state != query_result.state ||
	    info.name_length != ARRAY_SIZE(stream_name_bytes) ||
	    info.reserved16 != 0u || info.reserved32 != 0u ||
	    !bytes_match(info.name, stream_name_bytes,
			 ARRAY_SIZE(stream_name_bytes)))
		return 2;
	info.identity = TEST_HANDLE_SENTINEL;
	query_status = IOMGR_DEVICE_CALLBACK_IO_ERROR;
	if (iomgr_device_query_info(device, &info) != IOMGR_IO_ERROR ||
	    info.identity != TEST_HANDLE_SENTINEL)
		return 2;
	query_status = IOMGR_DEVICE_CALLBACK_OK;

	calls = read_calls;
	if (iomgr_device_read(device, NULL, 1u, 1u, &completed) !=
		IOMGR_INVALID_ARGUMENT ||
	    iomgr_device_read(device, read_buffer, 1u, 2u, &completed) !=
		IOMGR_INVALID_ARGUMENT ||
	    read_calls != calls)
		return 3;
	read_completion = 3u;
	reentry_device = device;
	reenter_on_read = true;
	calls = close_calls;
	completed = 99u;
	if (iomgr_device_read(device, read_buffer, sizeof(read_buffer), 5u,
			      &completed) != IOMGR_OK ||
	    completed != 3u || read_buffer[0] != 0xa0u ||
	    read_buffer[2] != 0xa2u || reentry_status != IOMGR_BUSY ||
	    close_calls != calls)
		return 4;
	write_completion = 2u;
	completed = 99u;
	if (iomgr_device_write(device, write_buffer, sizeof(write_buffer),
			       sizeof(write_buffer), &completed) != IOMGR_OK ||
	    completed != 2u || observed_write_count != sizeof(write_buffer) ||
	    !bytes_match(observed_write, write_buffer, sizeof(write_buffer)))
		return 5;
	control_completion = 2u;
	completed = 99u;
	if (iomgr_device_control(device, 0x1234u, input, sizeof(input),
				  sizeof(input), output, sizeof(output),
				  &completed) != IOMGR_OK ||
	    completed != 2u || observed_control_operation != 0x1234u ||
	    output[0] != 0x34u || output[1] != input[0])
		return 6;

	read_status = IOMGR_DEVICE_CALLBACK_IO_ERROR;
	completed = 99u;
	if (iomgr_device_read(device, read_buffer, sizeof(read_buffer), 1u,
			      &completed) != IOMGR_IO_ERROR ||
	    completed != 99u)
		return 7;
	read_status = IOMGR_DEVICE_CALLBACK_OK;
	write_status = IOMGR_DEVICE_CALLBACK_NO_SPACE;
	if (iomgr_device_write(device, write_buffer, sizeof(write_buffer), 1u,
			       &completed) != IOMGR_NO_SPACE)
		return 8;
	write_status = IOMGR_DEVICE_CALLBACK_OK;
	control_status = IOMGR_DEVICE_CALLBACK_UNSUPPORTED;
	if (iomgr_device_control(device, 1u, NULL, 0u, 0u, NULL, 0u,
				  &completed) != IOMGR_UNSUPPORTED)
		return 9;
	control_status = IOMGR_DEVICE_CALLBACK_OK;
	close_status = IOMGR_DEVICE_CALLBACK_BUSY;
	if (iomgr_device_close(device) != IOMGR_BUSY ||
	    iomgr_device_query_info(device, &info) != IOMGR_OK)
		return 10;
	close_status = IOMGR_DEVICE_CALLBACK_OK;
	if (iomgr_device_close(device) != IOMGR_OK ||
	    iomgr_device_query_info(device, &info) != IOMGR_STALE_HANDLE ||
	    iomgr_device_open(&stream_name, &replacement) != IOMGR_OK ||
	    replacement == device || iomgr_device_close(replacement) != IOMGR_OK ||
	    iomgr_device_unregister(registration) != IOMGR_OK)
		return 11;
	return 0;
}

static int test_capability_boundaries(void)
{
	struct iomgr_device_ops ops = make_ops(TEST_DEVICE_ID,
					       IOMGR_DEVICE_CAP_READ);
	struct iomgr_device_name name = make_name(short_name_bytes,
						 ARRAY_SIZE(short_name_bytes));
	iomgr_device_registration_handle_t registration;
	iomgr_device_handle_t device;
	struct iomgr_device_info info;
	const uint8_t source[] = { 1u };
	uint8_t destination[1];
	size_t completed;

	reset_callbacks();
	query_result.state = IOMGR_DEVICE_STATE_READ_READY;
	if (iomgr_device_register(&name, &ops, &registration) != IOMGR_OK ||
	    iomgr_device_open(&name, &device) != IOMGR_OK ||
	    iomgr_device_write(device, source, sizeof(source), sizeof(source),
			       &completed) != IOMGR_UNSUPPORTED ||
	    iomgr_device_control(device, 0u, NULL, 0u, 0u, NULL, 0u,
				  &completed) != IOMGR_UNSUPPORTED)
		return 1;
	read_completion = 1u;
	if (iomgr_device_read(device, destination, sizeof(destination), 1u,
			      &completed) != IOMGR_OK ||
	    completed != 1u || destination[0] != 0xa0u ||
	    iomgr_device_query_info(device, &info) != IOMGR_OK ||
	    info.capabilities != IOMGR_DEVICE_CAP_READ ||
	    iomgr_device_close(device) != IOMGR_OK ||
	    iomgr_device_open(&name, &device) != IOMGR_OK)
		return 2;
	query_result.state = IOMGR_DEVICE_STATE_WRITE_READY;
	info.identity = TEST_HANDLE_SENTINEL;
	if (iomgr_device_query_info(device, &info) != IOMGR_UNCERTAIN ||
	    info.identity != TEST_HANDLE_SENTINEL ||
	    iomgr_device_query_info(device, &info) != IOMGR_POISONED ||
	    iomgr_device_unregister(registration) != IOMGR_POISONED)
		return 3;
	return 0;
}

static int test_uncertain_isolation(void)
{
	struct iomgr_device_ops ops = make_ops(
		TEST_DEVICE_ID, IOMGR_DEVICE_CAP_READ | IOMGR_DEVICE_CAP_WRITE |
				IOMGR_DEVICE_CAP_CONTROL);
	uint8_t name_bytes[] = { 'Q', 'U', 'A', 'R' };
	struct iomgr_device_name name = make_name(name_bytes,
						 ARRAY_SIZE(name_bytes));
	iomgr_device_registration_handle_t registration;
	iomgr_device_handle_t poisoned;
	iomgr_device_handle_t survivor;
	iomgr_device_handle_t replacement;
	struct iomgr_device_info info;
	uint8_t buffer[2];
	const uint8_t source[] = { 0x5au };
	size_t completed = 77u;
	uint32_t calls;

	reset_callbacks();
	if (iomgr_device_register(&name, &ops, &registration) != IOMGR_OK ||
	    iomgr_device_open(&name, &survivor) != IOMGR_OK ||
	    iomgr_device_open(&name, &poisoned) != IOMGR_OK)
		return 1;
	read_status = IOMGR_DEVICE_CALLBACK_UNCERTAIN;
	if (iomgr_device_read(poisoned, buffer, sizeof(buffer), 1u,
			      &completed) != IOMGR_UNCERTAIN ||
	    completed != 77u ||
	    iomgr_device_query_info(poisoned, &info) != IOMGR_POISONED ||
	    iomgr_device_close(poisoned) != IOMGR_POISONED ||
	    iomgr_device_unregister(registration) != IOMGR_POISONED)
		return 2;
	read_status = IOMGR_DEVICE_CALLBACK_OK;
	if (iomgr_device_query_info(survivor, &info) != IOMGR_OK ||
	    iomgr_device_close(survivor) != IOMGR_OK ||
	    iomgr_device_open(&name, &replacement) != IOMGR_OK ||
	    iomgr_device_close(replacement) != IOMGR_OK ||
	    iomgr_device_unregister(registration) != IOMGR_POISONED)
		return 3;

	name_bytes[0] = 'O';
	if (iomgr_device_register(&name, &ops, &registration) != IOMGR_OK ||
	    iomgr_device_open(&name, &survivor) != IOMGR_OK)
		return 4;
	open_status = IOMGR_DEVICE_CALLBACK_UNCERTAIN;
	replacement = TEST_HANDLE_SENTINEL;
	if (iomgr_device_open(&name, &replacement) != IOMGR_UNCERTAIN ||
	    replacement != TEST_HANDLE_SENTINEL)
		return 4;
	open_status = IOMGR_DEVICE_CALLBACK_OK;
	calls = open_calls;
	if (iomgr_device_open(&name, &replacement) != IOMGR_POISONED ||
	    replacement != TEST_HANDLE_SENTINEL || open_calls != calls ||
	    iomgr_device_query_info(survivor, &info) != IOMGR_POISONED ||
	    iomgr_device_close(survivor) != IOMGR_OK ||
	    iomgr_device_unregister(registration) != IOMGR_POISONED)
		return 5;

	name_bytes[0] = 'Z';
	forced_instance_context = 0u;
	replacement = TEST_HANDLE_SENTINEL;
	if (iomgr_device_register(&name, &ops, &registration) != IOMGR_OK ||
	    iomgr_device_open(&name, &replacement) != IOMGR_UNCERTAIN ||
	    replacement != TEST_HANDLE_SENTINEL)
		return 6;
	forced_instance_context = KERNEL_OBJECT_HANDLE_INVALID;
	if (iomgr_device_open(&name, &replacement) != IOMGR_POISONED ||
	    iomgr_device_unregister(registration) != IOMGR_POISONED)
		return 7;

	name_bytes[0] = 'C';
	if (iomgr_device_register(&name, &ops, &registration) != IOMGR_OK ||
	    iomgr_device_open(&name, &replacement) != IOMGR_OK)
		return 8;
	control_status = (enum iomgr_device_callback_status)99u;
	if (iomgr_device_control(replacement, 0u, NULL, 0u, 0u, NULL, 0u,
				  &completed) != IOMGR_UNCERTAIN ||
	    iomgr_device_close(replacement) != IOMGR_POISONED)
		return 9;
	control_status = IOMGR_DEVICE_CALLBACK_OK;

	name_bytes[0] = 'B';
	if (iomgr_device_register(&name, &ops, &registration) != IOMGR_OK ||
	    iomgr_device_open(&name, &replacement) != IOMGR_OK)
		return 10;
	read_completion = 2u;
	if (iomgr_device_read(replacement, buffer, sizeof(buffer), 1u,
			      &completed) != IOMGR_UNCERTAIN ||
	    iomgr_device_query_info(replacement, &info) != IOMGR_POISONED)
		return 11;

	name_bytes[0] = 'I';
	if (iomgr_device_register(&name, &ops, &registration) != IOMGR_OK ||
	    iomgr_device_open(&name, &replacement) != IOMGR_OK)
		return 12;
	query_result.reserved = 1u;
	if (iomgr_device_query_info(replacement, &info) != IOMGR_UNCERTAIN ||
	    iomgr_device_close(replacement) != IOMGR_POISONED)
		return 13;

	query_result.reserved = 0u;
	name_bytes[0] = 'W';
	if (iomgr_device_register(&name, &ops, &registration) != IOMGR_OK ||
	    iomgr_device_open(&name, &replacement) != IOMGR_OK)
		return 14;
	write_completion = 2u;
	if (iomgr_device_write(replacement, source, sizeof(source), 1u,
			       &completed) != IOMGR_UNCERTAIN ||
	    iomgr_device_close(replacement) != IOMGR_POISONED)
		return 15;

	write_completion = 0u;
	name_bytes[0] = 'T';
	if (iomgr_device_register(&name, &ops, &registration) != IOMGR_OK ||
	    iomgr_device_open(&name, &replacement) != IOMGR_OK)
		return 16;
	control_completion = 1u;
	if (iomgr_device_control(replacement, 0u, NULL, 0u, 0u, NULL, 0u,
				  &completed) != IOMGR_UNCERTAIN ||
	    iomgr_device_close(replacement) != IOMGR_POISONED)
		return 17;

	control_completion = 0u;
	name_bytes[0] = 'D';
	if (iomgr_device_register(&name, &ops, &registration) != IOMGR_OK ||
	    iomgr_device_open(&name, &replacement) != IOMGR_OK)
		return 18;
	close_status = IOMGR_DEVICE_CALLBACK_UNCERTAIN;
	if (iomgr_device_close(replacement) != IOMGR_UNCERTAIN ||
	    iomgr_device_close(replacement) != IOMGR_POISONED ||
	    iomgr_device_unregister(registration) != IOMGR_POISONED)
		return 19;
	return 0;
}

static int run_tests(void)
{
	int status;

	reset_callbacks();
	status = test_initialization_and_validation();
	if (status != 0)
		return 10 + status;
	status = test_fixed_capacity();
	if (status != 0)
		return 30 + status;
	status = test_operations_and_precise_failures();
	if (status != 0)
		return 50 + status;
	status = test_capability_boundaries();
	if (status != 0)
		return 80 + status;
	status = test_uncertain_isolation();
	return status == 0 ? 0 : 100 + status;
}
#else
static int run_tests(void)
{
	struct iomgr_device_ops ops = make_ops(
		TEST_DEVICE_ID, IOMGR_DEVICE_CAP_READ | IOMGR_DEVICE_CAP_WRITE |
				IOMGR_DEVICE_CAP_CONTROL);
	struct iomgr_device_name name = make_name(
		stream_name_bytes, ARRAY_SIZE(stream_name_bytes));
	iomgr_device_registration_handle_t registration;
	iomgr_device_registration_handle_t first_registration;
	iomgr_device_handle_t device;
	iomgr_device_handle_t first_device = IOMGR_DEVICE_HANDLE_INVALID;
	struct iomgr_device_info info;
	size_t index;
	uint32_t calls;

	if ((uint32_t)IOMGR_DEVICE_TEST_GENERATION_MAX != 2u)
		return 1;
	reset_callbacks();
	if (iomgr_device_initialize() != IOMGR_OK ||
	    iomgr_device_register(&name, &ops, &registration) != IOMGR_OK)
		return 2;
	first_registration = registration;
	for (index = 0u; index < TEST_INSTANCE_SLOTS; ++index) {
		if (iomgr_device_open(&name, &device) != IOMGR_OK)
			return 3;
		if (index == 0u)
			first_device = device;
		if (iomgr_device_close(device) != IOMGR_OK)
			return 4;
		open_status = IOMGR_DEVICE_CALLBACK_NO_RESOURCES;
		device = TEST_HANDLE_SENTINEL;
		calls = open_calls;
		if (iomgr_device_open(&name, &device) != IOMGR_NO_SLOT ||
		    device != TEST_HANDLE_SENTINEL || open_calls != calls + 1u)
			return 5;
		open_status = IOMGR_DEVICE_CALLBACK_OK;
	}
	device = TEST_HANDLE_SENTINEL;
	calls = open_calls;
	if (iomgr_device_open(&name, &device) != IOMGR_NO_SLOT ||
	    device != TEST_HANDLE_SENTINEL || open_calls != calls ||
	    iomgr_device_query_info(first_device, &info) != IOMGR_STALE_HANDLE ||
	    iomgr_device_unregister(registration) != IOMGR_OK)
		return 6;

	for (index = 0u;; ++index) {
		registration = TEST_HANDLE_SENTINEL;
		if (iomgr_device_register(&name, &ops, &registration) ==
			IOMGR_NO_SLOT)
			break;
		if (registration == TEST_HANDLE_SENTINEL ||
		    iomgr_device_unregister(first_registration) !=
			IOMGR_STALE_HANDLE ||
		    iomgr_device_unregister(registration) != IOMGR_OK ||
		    index >= TEST_REGISTRATION_SLOTS * 2u)
			return 7;
	}
	if (index != TEST_REGISTRATION_SLOTS * 2u - 1u ||
	    registration != TEST_HANDLE_SENTINEL)
		return 8;
	return 0;
}
#endif

DOSC32_TEST_ENTRY(run_tests)

// SPDX-License-Identifier: GPL-2.0-only
/* Freestanding tests for the simulated DOS address and I/O boundary. */
#include "dos_machine.h"
#include "test_entry.h"

static uint8_t guest_memory[DOS_A20_WRAP_ADDRESS];
static bool backend_a20;
static uint32_t last_port_value;
static uint32_t memory_write_calls;
static uint32_t fail_memory_write_call;
static bool fail_all_memory_writes;
static uint32_t port_read_value = 0xabcdu;
static enum dos_machine_status port_read_result = DOS_MACHINE_OK;
static enum dos_machine_status port_write_result = DOS_MACHINE_OK;
static enum dos_machine_status a20_result = DOS_MACHINE_OK;
static enum dos_machine_status a20_query_result = DOS_MACHINE_OK;
static bool a20_apply_requested = true;
static uint32_t set_a20_calls;
static uint32_t query_a20_calls;

static enum dos_machine_status
test_read_memory(kernel_object_handle_t context, dos_linear_address_t address,
		 void *destination, size_t destination_capacity, size_t count)
{
	uint8_t *output = (uint8_t *)destination;
	size_t index;

	if (context != 1u || count > destination_capacity ||
	    (uint64_t)count > DOS_A20_WRAP_ADDRESS - (uint64_t)address)
		return DOS_MACHINE_ADDRESS_FAULT;
	for (index = 0u; index < count; ++index)
		output[index] = guest_memory[address + index];
	return DOS_MACHINE_OK;
}

static enum dos_machine_status
test_write_memory(kernel_object_handle_t context, dos_linear_address_t address,
		  const void *source, size_t source_capacity, size_t count)
{
	const uint8_t *input = (const uint8_t *)source;
	size_t index;

	++memory_write_calls;
	if (context != 1u || count > source_capacity ||
	    (uint64_t)count > DOS_A20_WRAP_ADDRESS - (uint64_t)address)
		return DOS_MACHINE_ADDRESS_FAULT;
	if (fail_all_memory_writes ||
	    memory_write_calls == fail_memory_write_call) {
		if (count != 0u)
			guest_memory[address] = 0xeeu;
		return DOS_MACHINE_IO_FAULT;
	}
	for (index = 0u; index < count; ++index)
		guest_memory[address + index] = input[index];
	return DOS_MACHINE_OK;
}

static enum dos_machine_status test_read_port(kernel_object_handle_t context,
					      uint16_t port,
					      enum dos_io_width width,
					      uint32_t *value)
{
	if (context != 1u || port != 0x1234u ||
	    (width != DOS_IO_WIDTH_8 && width != DOS_IO_WIDTH_16 &&
	     width != DOS_IO_WIDTH_32))
		return DOS_MACHINE_IO_DENIED;
	*value = port_read_value;
	return port_read_result;
}

static enum dos_machine_status test_write_port(kernel_object_handle_t context,
					       uint16_t port,
					       enum dos_io_width width,
					       uint32_t value)
{
	if (context != 1u || port != 0x1234u || width != DOS_IO_WIDTH_16)
		return DOS_MACHINE_IO_DENIED;
	last_port_value = value;
	return port_write_result;
}

static enum dos_machine_status test_set_a20(kernel_object_handle_t context,
					    bool enabled)
{
	++set_a20_calls;
	if (context != 1u)
		return DOS_MACHINE_INVALID_ARGUMENT;
	if (a20_apply_requested)
		backend_a20 = enabled;
	return a20_result;
}

static enum dos_machine_status test_query_a20(kernel_object_handle_t context,
					       bool *enabled)
{
	++query_a20_calls;
	if (context != 1u || enabled == NULL)
		return DOS_MACHINE_INVALID_ARGUMENT;
	if (a20_query_result != DOS_MACHINE_OK)
		return a20_query_result;
	*enabled = backend_a20;
	return DOS_MACHINE_OK;
}

static int run_tests(void)
{
	static const struct dos_machine_ops ops = {
	    .read_memory = test_read_memory,
	    .write_memory = test_write_memory,
	    .read_port = test_read_port,
	    .write_port = test_write_port,
	    .set_a20 = test_set_a20,
	    .query_a20 = test_query_a20,
	};
	struct dos_machine machine;
	uint8_t output[4];
	uint8_t input[4] = {5u, 6u, 7u, 8u};
	uint8_t replacement[4] = {9u, 10u, 11u, 12u};
	uint8_t rollback[4];
	uint32_t port_value;
	bool a20_enabled;

	if (dos_machine_configure(&machine, &ops, 1u, DOS_A20_WRAP_ADDRESS,
				  false) != DOS_MACHINE_OK)
		return 1;
	guest_memory[0xffffeu] = 1u;
	guest_memory[0xfffffu] = 2u;
	guest_memory[0] = 3u;
	guest_memory[1] = 4u;
	if (dos_machine_read_far(&machine, 0xffffu, 0x000eu, output,
				 sizeof(output),
				 sizeof(output)) != DOS_MACHINE_OK)
		return 2;
	if (output[0] != 1u || output[1] != 2u || output[2] != 3u ||
	    output[3] != 4u)
		return 3;
	if (dos_machine_write_far(&machine, 0xffffu, 0x000eu, input,
				  sizeof(input),
				  sizeof(input)) != DOS_MACHINE_OK)
		return 4;
	if (guest_memory[0xffffeu] != 5u || guest_memory[0xfffffu] != 6u ||
	    guest_memory[0] != 7u || guest_memory[1] != 8u)
		return 5;
	guest_memory[0xffffeu] = 1u;
	guest_memory[0xfffffu] = 2u;
	guest_memory[0] = 3u;
	guest_memory[1] = 4u;
	if (dos_machine_replace_far(&machine, 0xffffu, 0x000eu, replacement,
				    sizeof(replacement), rollback,
				    sizeof(rollback),
				    sizeof(replacement)) != DOS_MACHINE_OK ||
	    guest_memory[0xffffeu] != 9u || guest_memory[0xfffffu] != 10u ||
	    guest_memory[0] != 11u || guest_memory[1] != 12u ||
	    rollback[0] != 1u || rollback[3] != 4u)
		return 26;
	if (dos_machine_read(&machine, 0xfffffu, output, sizeof(output), 2u) !=
	    DOS_MACHINE_ADDRESS_FAULT)
		return 6;
	if (dos_machine_read(&machine, 0u, output, 1u, 2u) !=
	    DOS_MACHINE_INVALID_ARGUMENT)
		return 7;
	if (dos_machine_set_a20(&machine, true) != DOS_MACHINE_OK ||
	    !machine.a20_enabled || !backend_a20)
		return 8;
	if (dos_machine_query_a20(&machine, &a20_enabled) != DOS_MACHINE_OK ||
	    !a20_enabled)
		return 33;
	if (dos_machine_read_port(&machine, 0x1234u, DOS_IO_WIDTH_16,
				  &port_value) != DOS_MACHINE_OK ||
	    port_value != 0xabcdu)
		return 9;
	if (dos_machine_write_port(&machine, 0x1234u, DOS_IO_WIDTH_16,
				   0x4321u) != DOS_MACHINE_OK ||
	    last_port_value != 0x4321u)
		return 10;
	if (dos_machine_write_port(&machine, 0x1234u, DOS_IO_WIDTH_8, 0x100u) !=
	    DOS_MACHINE_INVALID_ARGUMENT)
		return 11;
	if (dos_machine_configure(&machine, &ops, 1u,
				  DOS_GUEST_32_ADDRESS_LIMIT + 1u,
				  false) != DOS_MACHINE_INVALID_ARGUMENT)
		return 12;

	guest_memory[100u] = 1u;
	guest_memory[101u] = 2u;
	guest_memory[102u] = 3u;
	guest_memory[103u] = 4u;
	memory_write_calls = 0u;
	if (dos_machine_replace(&machine, 100u, replacement,
				sizeof(replacement), rollback, sizeof(rollback),
				sizeof(replacement)) != DOS_MACHINE_OK ||
	    guest_memory[100u] != 9u || guest_memory[103u] != 12u ||
	    rollback[0] != 1u || rollback[3] != 4u)
		return 13;

	guest_memory[100u] = 1u;
	guest_memory[101u] = 2u;
	guest_memory[102u] = 3u;
	guest_memory[103u] = 4u;
	memory_write_calls = 0u;
	fail_memory_write_call = 1u;
	if (dos_machine_replace(&machine, 100u, replacement,
				sizeof(replacement), rollback, sizeof(rollback),
				sizeof(replacement)) != DOS_MACHINE_IO_FAULT ||
	    guest_memory[100u] != 1u || guest_memory[101u] != 2u ||
	    guest_memory[102u] != 3u || guest_memory[103u] != 4u)
		return 14;
	fail_memory_write_call = 0u;

	memory_write_calls = 0u;
	fail_all_memory_writes = true;
	if (dos_machine_replace(&machine, 100u, replacement,
				sizeof(replacement), rollback, sizeof(rollback),
				sizeof(replacement)) !=
	    DOS_MACHINE_ROLLBACK_FAILED)
		return 15;
	fail_all_memory_writes = false;
	if (dos_machine_replace(&machine, 100u, rollback, sizeof(rollback),
				rollback, sizeof(rollback), sizeof(rollback)) !=
	    DOS_MACHINE_INVALID_ARGUMENT)
		return 16;

	if (dos_machine_set_a20(&machine, false) != DOS_MACHINE_OK ||
	    machine.a20_enabled || backend_a20)
		return 17;
	guest_memory[0x1fffeu] = 0x11u;
	guest_memory[0x1ffffu] = 0x22u;
	guest_memory[0x10000u] = 0x33u;
	guest_memory[0x10001u] = 0x44u;
	guest_memory[0x20000u] = 0xa5u;
	if (dos_machine_validate_far(&machine, 0x1000u, 0xfffeu, 4u) !=
		DOS_MACHINE_OK ||
	    dos_machine_read_far(&machine, 0x1000u, 0xfffeu, output,
				 sizeof(output),
				 sizeof(output)) != DOS_MACHINE_OK ||
	    output[0] != 0x11u || output[1] != 0x22u || output[2] != 0x33u ||
	    output[3] != 0x44u)
		return 18;
	if (dos_machine_write_far(&machine, 0x1000u, 0xfffeu, input,
				  sizeof(input),
				  sizeof(input)) != DOS_MACHINE_OK ||
	    guest_memory[0x1fffeu] != 5u || guest_memory[0x1ffffu] != 6u ||
	    guest_memory[0x10000u] != 7u || guest_memory[0x10001u] != 8u ||
	    guest_memory[0x20000u] != 0xa5u)
		return 19;

	guest_memory[0x1fffeu] = 0x11u;
	guest_memory[0x1ffffu] = 0x22u;
	guest_memory[0x10000u] = 0x33u;
	guest_memory[0x10001u] = 0x44u;
	memory_write_calls = 0u;
	if (dos_machine_replace_far(&machine, 0x1000u, 0xfffeu, replacement,
				    sizeof(replacement), rollback,
				    sizeof(rollback),
				    sizeof(replacement)) != DOS_MACHINE_OK ||
	    guest_memory[0x1fffeu] != 9u || guest_memory[0x1ffffu] != 10u ||
	    guest_memory[0x10000u] != 11u || guest_memory[0x10001u] != 12u ||
	    rollback[0] != 0x11u || rollback[3] != 0x44u)
		return 23;

	guest_memory[0x1fffeu] = 0x11u;
	guest_memory[0x1ffffu] = 0x22u;
	guest_memory[0x10000u] = 0x33u;
	guest_memory[0x10001u] = 0x44u;
	memory_write_calls = 0u;
	fail_memory_write_call = 2u;
	if (dos_machine_replace_far(&machine, 0x1000u, 0xfffeu, replacement,
				    sizeof(replacement), rollback,
				    sizeof(rollback), sizeof(replacement)) !=
		DOS_MACHINE_IO_FAULT ||
	    guest_memory[0x1fffeu] != 0x11u ||
	    guest_memory[0x1ffffu] != 0x22u ||
	    guest_memory[0x10000u] != 0x33u ||
	    guest_memory[0x10001u] != 0x44u || memory_write_calls != 4u)
		return 24;
	fail_memory_write_call = 0u;

	memory_write_calls = 0u;
	fail_all_memory_writes = true;
	if (dos_machine_replace_far(&machine, 0x1000u, 0xfffeu, replacement,
				    sizeof(replacement), rollback,
				    sizeof(rollback), sizeof(replacement)) !=
	    DOS_MACHINE_ROLLBACK_FAILED)
		return 25;
	fail_all_memory_writes = false;

	/* Offset wrapping and A20 wrapping are independent address transforms.
	 */
	guest_memory[0x0ffeeu] = 0x51u;
	guest_memory[0x0ffefu] = 0x52u;
	guest_memory[0xffff0u] = 0x53u;
	guest_memory[0xffff1u] = 0x54u;
	if (dos_machine_read_far(&machine, 0xffffu, 0xfffeu, output,
				 sizeof(output),
				 sizeof(output)) != DOS_MACHINE_OK ||
	    output[0] != 0x51u || output[1] != 0x52u || output[2] != 0x53u ||
	    output[3] != 0x54u)
		return 20;

	guest_memory[0x1fffeu] = 5u;
	guest_memory[0x1ffffu] = 6u;
	guest_memory[0x10000u] = 7u;
	guest_memory[0x10001u] = 8u;
	if (dos_machine_set_a20(&machine, true) != DOS_MACHINE_OK ||
	    dos_machine_validate_far(&machine, 0xffffu, 0xffffu, 1u) !=
		DOS_MACHINE_ADDRESS_FAULT)
		return 21;
	/* The 64 KiB offset rule is unchanged when A20 is enabled. */
	if (dos_machine_read_far(&machine, 0x1000u, 0xfffeu, output,
				 sizeof(output),
				 sizeof(output)) != DOS_MACHINE_OK ||
	    output[0] != 5u || output[1] != 6u || output[2] != 7u ||
	    output[3] != 8u)
		return 22;

	/* A failed or malformed port callback must not publish its scratch
	 * value to the caller. */
	port_value = 0xfeedbeefu;
	port_read_value = 0x1234u;
	port_read_result = DOS_MACHINE_IO_FAULT;
	if (dos_machine_read_port(&machine, 0x1234u, DOS_IO_WIDTH_16,
				  &port_value) != DOS_MACHINE_IO_FAULT ||
	    port_value != 0xfeedbeefu)
		return 27;
	port_read_result = (enum dos_machine_status)99;
	if (dos_machine_read_port(&machine, 0x1234u, DOS_IO_WIDTH_16,
				  &port_value) != DOS_MACHINE_IO_FAULT ||
	    port_value != 0xfeedbeefu)
		return 28;
	port_read_result = DOS_MACHINE_OK;
	port_read_value = 0x100u;
	if (dos_machine_read_port(&machine, 0x1234u, DOS_IO_WIDTH_8,
				  &port_value) != DOS_MACHINE_IO_FAULT ||
	    port_value != 0xfeedbeefu)
		return 29;
	port_read_value = 0xabcdu;
	port_write_result = (enum dos_machine_status)99;
	if (dos_machine_write_port(&machine, 0x1234u, DOS_IO_WIDTH_16,
				   0x4321u) != DOS_MACHINE_IO_FAULT)
		return 30;
	port_write_result = DOS_MACHINE_OK;

	/* A callback success is not authoritative: an independent query must
	 * prove that the requested transform actually took effect. */
	backend_a20 = false;
	a20_result = DOS_MACHINE_OK;
	a20_query_result = DOS_MACHINE_OK;
	a20_apply_requested = false;
	set_a20_calls = 0u;
	query_a20_calls = 0u;
	if (dos_machine_configure(&machine, &ops, 1u,
				  DOS_A20_WRAP_ADDRESS, false) !=
		DOS_MACHINE_OK ||
	    dos_machine_set_a20(&machine, true) != DOS_MACHINE_IO_FAULT ||
	    machine.poisoned != 0u || machine.a20_enabled || backend_a20 ||
	    set_a20_calls != 1u || query_a20_calls != 1u)
		return 31;

	/* A pessimistic set result is recoverable when the independent query
	 * proves that the requested state was reached. */
	a20_apply_requested = true;
	a20_result = DOS_MACHINE_IO_FAULT;
	if (dos_machine_set_a20(&machine, true) != DOS_MACHINE_OK ||
	    machine.poisoned != 0u || !machine.a20_enabled || !backend_a20 ||
	    set_a20_calls != 2u || query_a20_calls != 2u)
		return 34;

	/* If the final hardware state cannot be queried, no cached transform is
	 * safe to publish and the machine lifetime is quarantined. */
	backend_a20 = false;
	a20_result = DOS_MACHINE_OK;
	a20_query_result = DOS_MACHINE_IO_FAULT;
	set_a20_calls = 0u;
	query_a20_calls = 0u;
	if (dos_machine_configure(&machine, &ops, 1u,
				  DOS_A20_WRAP_ADDRESS, false) !=
		DOS_MACHINE_OK ||
	    dos_machine_set_a20(&machine, true) != DOS_MACHINE_IO_FAULT ||
	    machine.poisoned != 1u || machine.a20_enabled || !backend_a20 ||
	    set_a20_calls != 1u || query_a20_calls != 1u)
		return 35;
	port_value = 0xfeedbeefu;
	if (dos_machine_read_port(&machine, 0x1234u, DOS_IO_WIDTH_16,
				  &port_value) != DOS_MACHINE_STOPPED ||
	    port_value != 0xfeedbeefu ||
	    dos_machine_validate_far(&machine, 0u, 0u, 1u) !=
		DOS_MACHINE_STOPPED ||
	    dos_machine_set_a20(&machine, false) != DOS_MACHINE_STOPPED ||
	    set_a20_calls != 1u)
		return 32;
	a20_result = DOS_MACHINE_OK;
	a20_query_result = DOS_MACHINE_OK;
	a20_apply_requested = true;
	return 0;
}

DOSC32_TEST_ENTRY(run_tests)

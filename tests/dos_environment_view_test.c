// SPDX-License-Identifier: GPL-2.0-only
/* Bounded PSP environment lookup and failure-publication tests. */
#include "dos_environment_view.h"
#include "test_entry.h"

#define TEST_MEMORY_BYTES 0x40000u
#define TEST_CONTEXT ((kernel_object_handle_t)0x45564e5649455701ull)
#define TEST_PSP 0x1000u
#define TEST_ENVIRONMENT 0x1200u

static uint8_t guest_memory[TEST_MEMORY_BYTES];
static uint32_t read_calls;
static uint32_t fail_read_call;

static enum dos_machine_status
read_memory(kernel_object_handle_t context, dos_linear_address_t address,
	    void *destination, size_t destination_capacity, size_t count)
{
	uint8_t *output = (uint8_t *)destination;
	size_t index;

	++read_calls;
	if (context != TEST_CONTEXT || destination == NULL ||
	    count > destination_capacity || address > TEST_MEMORY_BYTES ||
	    count > TEST_MEMORY_BYTES - (size_t)address)
		return DOS_MACHINE_ADDRESS_FAULT;
	if (fail_read_call != 0u && read_calls == fail_read_call)
		return DOS_MACHINE_IO_FAULT;
	for (index = 0u; index < count; ++index)
		output[index] = guest_memory[(size_t)address + index];
	return DOS_MACHINE_OK;
}

static enum dos_machine_status
write_memory(kernel_object_handle_t context, dos_linear_address_t address,
	     const void *source, size_t source_capacity, size_t count)
{
	(void)context;
	(void)address;
	(void)source;
	(void)source_capacity;
	(void)count;
	return DOS_MACHINE_UNSUPPORTED;
}

static const struct dos_machine_ops machine_ops = {
	.read_memory = read_memory,
	.write_memory = write_memory,
};

static void clear_memory(void)
{
	size_t index;

	for (index = 0u; index < sizeof(guest_memory); ++index)
		guest_memory[index] = 0u;
	read_calls = 0u;
	fail_read_call = 0u;
}

static void write_le16(size_t address, uint16_t value)
{
	guest_memory[address] = (uint8_t)value;
	guest_memory[address + 1u] = (uint8_t)(value >> 8u);
}

static void prepare_environment(const uint8_t *bytes, size_t count,
				uint16_t owner, uint16_t paragraphs)
{
	size_t psp_linear = (size_t)TEST_PSP << 4u;
	size_t environment_linear = (size_t)TEST_ENVIRONMENT << 4u;
	size_t mcb_linear = (size_t)(TEST_ENVIRONMENT - 1u) << 4u;
	size_t index;

	write_le16(psp_linear + 0x2cu, TEST_ENVIRONMENT);
	guest_memory[mcb_linear] = (uint8_t)'Z';
	write_le16(mcb_linear + 1u, owner);
	write_le16(mcb_linear + 3u, paragraphs);
	for (index = 0u; index < count; ++index)
		guest_memory[environment_linear + index] = bytes[index];
}

static bool bytes_equal(const uint8_t *left, const uint8_t *right,
			size_t count)
{
	size_t index;

	for (index = 0u; index < count; ++index) {
		if (left[index] != right[index])
			return false;
	}
	return true;
}

static int test_lookup_and_stream(const struct dos_machine *machine)
{
	static const uint8_t environment[] = {
		'P', 'a', 't', 'h', '=', 'C', ':', '\\', 'B', 'I', 'N', ';',
		'D', ':', '\\', 'T', 'O', 'O', 'L', 'S', 0u,
		'E', 'M', 'P', 'T', 'Y', '=', 0u,
		'C', 'O', 'M', 'S', 'P', 'E', 'C', '=', 'C', ':', '\\',
		'C', 'O', 'M', 'M', 'A', 'N', 'D', '.', 'C', 'O', 'M', 0u,
		0u, 1u, 0u, 'C', ':', '\\', 'C', 'O', 'M', 'M', 'A', 'N',
		'D', '.', 'C', 'O', 'M', 0u,
	};
	static const uint8_t expected[] = "C:\\BIN;D:\\TOOLS";
	static const uint8_t suffix[] = "D:\\TOOLS";
	uint8_t output[32];
	size_t value_length = 0xaaaau;
	size_t bytes_read = 0xbbbbu;
	size_t index;

	clear_memory();
	prepare_environment(environment, sizeof(environment), TEST_PSP, 16u);
	for (index = 0u; index < sizeof(output); ++index)
		output[index] = 0xccu;
	if (dos_environment_view_read_value(
		    machine, TEST_PSP, (const uint8_t *)"PATH", 4u, 0u,
		    output, sizeof(expected) - 1u, &value_length, &bytes_read) !=
	    DOS_ENVIRONMENT_VIEW_OK)
		return 1;
	if (value_length != sizeof(expected) - 1u ||
	    bytes_read != sizeof(expected) - 1u ||
	    !bytes_equal(output, expected, sizeof(expected) - 1u) ||
	    output[sizeof(expected) - 1u] != 0xccu)
		return 2;
	if (dos_environment_view_read_value(
		    machine, TEST_PSP, (const uint8_t *)"path", 4u, 7u,
		    output, sizeof(suffix) - 1u, &value_length, &bytes_read) !=
	    DOS_ENVIRONMENT_VIEW_OK || value_length != sizeof(expected) - 1u ||
	    bytes_read != sizeof(suffix) - 1u ||
	    !bytes_equal(output, suffix, sizeof(suffix) - 1u))
		return 3;
	value_length = 0xaaaau;
	bytes_read = 0xbbbbu;
	if (dos_environment_view_read_value(
		    machine, TEST_PSP, (const uint8_t *)"EMPTY", 5u, 0u,
		    NULL, 0u, &value_length, &bytes_read) !=
	    DOS_ENVIRONMENT_VIEW_OK || value_length != 0u || bytes_read != 0u)
		return 4;
	return 0;
}

static int test_failures_do_not_publish(const struct dos_machine *machine)
{
	static const uint8_t environment[] = {
		'P', 'A', 'T', 'H', '=', 'C', ':', '\\', 'B', 'I', 'N', 0u, 0u,
	};
	uint8_t output[16];
	uint8_t expected[16];
	size_t value_length;
	size_t bytes_read;
	size_t index;

	clear_memory();
	prepare_environment(environment, sizeof(environment), TEST_PSP, 4u);
	for (index = 0u; index < sizeof(output); ++index) {
		output[index] = 0x5au;
		expected[index] = 0x5au;
	}
	value_length = 0x1111u;
	bytes_read = 0x2222u;
	if (dos_environment_view_read_value(
		    machine, TEST_PSP, (const uint8_t *)"MISSING", 7u, 0u,
		    output, sizeof(output), &value_length, &bytes_read) !=
	    DOS_ENVIRONMENT_VIEW_NOT_FOUND ||
	    value_length != 0x1111u || bytes_read != 0x2222u ||
	    !bytes_equal(output, expected, sizeof(output)))
		return 1;
	prepare_environment(environment, sizeof(environment), 0x9999u, 4u);
	if (dos_environment_view_read_value(
		    machine, TEST_PSP, (const uint8_t *)"PATH", 4u, 0u,
		    output, sizeof(output), &value_length, &bytes_read) !=
	    DOS_ENVIRONMENT_VIEW_BAD_BLOCK ||
	    !bytes_equal(output, expected, sizeof(output)))
		return 2;
	prepare_environment(environment, sizeof(environment), TEST_PSP, 4u);
	read_calls = 0u;
	fail_read_call = 3u;
	if (dos_environment_view_read_value(
		    machine, TEST_PSP, (const uint8_t *)"PATH", 4u, 0u,
		    output, sizeof(output), &value_length, &bytes_read) !=
	    DOS_ENVIRONMENT_VIEW_MACHINE_FAULT ||
	    !bytes_equal(output, expected, sizeof(output)))
		return 3;
	return 0;
}

static int run_tests(void)
{
	struct dos_machine machine;
	int result;

	if (dos_machine_configure(&machine, &machine_ops, TEST_CONTEXT,
				  TEST_MEMORY_BYTES, true) != DOS_MACHINE_OK)
		return 1;
	result = test_lookup_and_stream(&machine);
	if (result != 0)
		return 10 + result;
	result = test_failures_do_not_publish(&machine);
	if (result != 0)
		return 20 + result;
	return 0;
}

DOSC32_TEST_ENTRY(run_tests)

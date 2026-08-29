// SPDX-License-Identifier: GPL-2.0-only
/* Freestanding IVT byte-layout, bounds and rollback tests. */
#include "dos_vectors.h"
#include "test_entry.h"

#define MEMORY_BYTES 1024u
#define TEST_CONTEXT ((kernel_object_handle_t)0x564543544f52534full)

static uint8_t guest_memory[MEMORY_BYTES];
static uint32_t write_calls;
static uint32_t fail_write_call;
static bool fail_all_writes;

static enum dos_machine_status
read_memory(kernel_object_handle_t context, dos_linear_address_t linear_address,
	    void *destination, size_t destination_capacity, size_t count)
{
	uint8_t *output = (uint8_t *)destination;
	size_t index;

	if (context != TEST_CONTEXT || destination == NULL ||
	    count > destination_capacity || linear_address > MEMORY_BYTES ||
	    count > MEMORY_BYTES - (size_t)linear_address)
		return DOS_MACHINE_ADDRESS_FAULT;
	for (index = 0u; index < count; ++index)
		output[index] = guest_memory[(size_t)linear_address + index];
	return DOS_MACHINE_OK;
}

static enum dos_machine_status write_memory(kernel_object_handle_t context,
					    dos_linear_address_t linear_address,
					    const void *source,
					    size_t source_capacity,
					    size_t count)
{
	const uint8_t *input = (const uint8_t *)source;
	size_t index;

	++write_calls;
	if (context != TEST_CONTEXT || source == NULL ||
	    count > source_capacity || linear_address > MEMORY_BYTES ||
	    count > MEMORY_BYTES - (size_t)linear_address)
		return DOS_MACHINE_ADDRESS_FAULT;
	/* Deliberately dirty one byte before failing to exercise rollback. */
	if (fail_all_writes || write_calls == fail_write_call) {
		guest_memory[linear_address] = 0xeeu;
		return DOS_MACHINE_IO_FAULT;
	}
	for (index = 0u; index < count; ++index)
		guest_memory[(size_t)linear_address + index] = input[index];
	return DOS_MACHINE_OK;
}

static const struct dos_machine_ops machine_ops = {
    .read_memory = read_memory,
    .write_memory = write_memory,
    .read_port = NULL,
    .write_port = NULL,
    .set_a20 = NULL,
};

static int run_tests(void)
{
	struct dos_far_pointer16 address;
	struct dos_machine machine;
	size_t vector_offset = 0x21u * 4u;

	if (dos_machine_configure(&machine, &machine_ops, TEST_CONTEXT,
				  MEMORY_BYTES, false) != DOS_MACHINE_OK)
		return 1;
	guest_memory[vector_offset] = 0x34u;
	guest_memory[vector_offset + 1u] = 0x12u;
	guest_memory[vector_offset + 2u] = 0xcdu;
	guest_memory[vector_offset + 3u] = 0xabu;
	if (dos_vector_get(&machine, 0x21u, &address) != DOS_VECTOR_OK ||
	    address.offset != 0x1234u || address.segment != 0xabcdu)
		return 2;

	address.offset = 0xbeefu;
	address.segment = 0x4567u;
	if (dos_vector_set(&machine, 0xffu, address) != DOS_VECTOR_OK ||
	    guest_memory[0x3fcu] != 0xefu || guest_memory[0x3fdu] != 0xbeu ||
	    guest_memory[0x3feu] != 0x67u || guest_memory[0x3ffu] != 0x45u)
		return 3;

	write_calls = 0u;
	fail_write_call = 1u;
	address.offset = 0xaaaau;
	address.segment = 0xbbbbu;
	if (dos_vector_set(&machine, 0x21u, address) !=
		DOS_VECTOR_MACHINE_FAULT ||
	    guest_memory[vector_offset] != 0x34u ||
	    guest_memory[vector_offset + 1u] != 0x12u ||
	    guest_memory[vector_offset + 2u] != 0xcdu ||
	    guest_memory[vector_offset + 3u] != 0xabu)
		return 4;

	write_calls = 0u;
	fail_write_call = 0u;
	fail_all_writes = true;
	/* Force both the attempted write and rollback to fail. */
	if (dos_vector_set(&machine, 0x20u, address) !=
	    DOS_VECTOR_ROLLBACK_FAILED)
		return 5;
	fail_all_writes = false;
	fail_write_call = 0u;
	if (dos_vector_get(NULL, 0u, &address) != DOS_VECTOR_INVALID_ARGUMENT ||
	    dos_vector_get(&machine, 0u, NULL) != DOS_VECTOR_INVALID_ARGUMENT)
		return 6;

	machine.address_limit = 3u;
	if (dos_vector_get(&machine, 0u, &address) != DOS_VECTOR_MACHINE_FAULT)
		return 7;
	return 0;
}

DOSC32_TEST_ENTRY(run_tests)

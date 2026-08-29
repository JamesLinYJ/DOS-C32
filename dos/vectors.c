// SPDX-License-Identifier: GPL-2.0-only
/*
 * Simulated real-mode interrupt vector table.
 *
 * Compatibility contract: each vector is a little-endian offset:segment pair at n * 4.
 * Safety changes: byte decoding, checked machine bounds and rollback after a
 * backend write failure; no guest address becomes a native pointer.
 */
#include "dos_vectors.h"

#define DOS_VECTOR_BYTES 4u

static dos_linear_address_t vector_linear_address(uint8_t vector)
{
	return (dos_linear_address_t)vector * DOS_VECTOR_BYTES;
}

static uint16_t read_le16(const uint8_t *bytes)
{
	return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static void write_le16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
}

enum dos_vector_status dos_vector_get(const struct dos_machine *machine,
				      uint8_t vector,
				      struct dos_far_pointer16 *address)
{
	uint8_t encoded[DOS_VECTOR_BYTES];

	if (address == NULL)
		return DOS_VECTOR_INVALID_ARGUMENT;
	address->offset = 0u;
	address->segment = 0u;
	if (machine == NULL)
		return DOS_VECTOR_INVALID_ARGUMENT;
	if (dos_machine_read(machine, vector_linear_address(vector), encoded,
			     sizeof(encoded),
			     sizeof(encoded)) != DOS_MACHINE_OK)
		return DOS_VECTOR_MACHINE_FAULT;
	address->offset = read_le16(encoded);
	address->segment = read_le16(encoded + 2u);
	return DOS_VECTOR_OK;
}

enum dos_vector_status dos_vector_set(const struct dos_machine *machine,
				      uint8_t vector,
				      struct dos_far_pointer16 address)
{
	uint8_t previous[DOS_VECTOR_BYTES];
	uint8_t encoded[DOS_VECTOR_BYTES];
	dos_linear_address_t linear = vector_linear_address(vector);
	enum dos_machine_status status;

	if (machine == NULL)
		return DOS_VECTOR_INVALID_ARGUMENT;
	write_le16(encoded, address.offset);
	write_le16(encoded + 2u, address.segment);
	status =
	    dos_machine_replace(machine, linear, encoded, sizeof(encoded),
				previous, sizeof(previous), sizeof(encoded));
	if (status == DOS_MACHINE_OK)
		return DOS_VECTOR_OK;
	if (status == DOS_MACHINE_ROLLBACK_FAILED)
		return DOS_VECTOR_ROLLBACK_FAILED;
	return DOS_VECTOR_MACHINE_FAULT;
}

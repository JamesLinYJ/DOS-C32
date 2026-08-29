// SPDX-License-Identifier: GPL-2.0-only
/*
 * DOS guest-machine boundary
 *
 * Compatibility contract: translate simulated segment:offset memory and mediated I/O
 * Safety changes: no guest integer is cast to a native pointer; every native
 *                 buffer carries a capacity; A20 wrap is explicit and bounded
 */
#include "dos_machine.h"

static bool io_width_is_valid(enum dos_io_width width)
{
	return width == DOS_IO_WIDTH_8 || width == DOS_IO_WIDTH_16 ||
	       width == DOS_IO_WIDTH_32;
}

static bool machine_status_is_valid(enum dos_machine_status status)
{
	return status == DOS_MACHINE_OK ||
	       status == DOS_MACHINE_INVALID_ARGUMENT ||
	       status == DOS_MACHINE_ADDRESS_FAULT ||
	       status == DOS_MACHINE_IO_DENIED ||
	       status == DOS_MACHINE_IO_FAULT ||
	       status == DOS_MACHINE_ROLLBACK_FAILED ||
	       status == DOS_MACHINE_UNSUPPORTED ||
	       status == DOS_MACHINE_STOPPED;
}

static enum dos_machine_status
normalize_callback_status(enum dos_machine_status status)
{
	return machine_status_is_valid(status) ? status : DOS_MACHINE_IO_FAULT;
}

static enum dos_machine_status query_a20_backend(
	struct dos_machine *machine, bool *enabled)
{
	bool candidate = false;
	enum dos_machine_status status;

	if (machine->ops->query_a20 == NULL || enabled == NULL)
		return DOS_MACHINE_UNSUPPORTED;
	status = normalize_callback_status(
		machine->ops->query_a20(machine->context, &candidate));
	if (status != DOS_MACHINE_OK)
		return status;
	*enabled = candidate;
	return DOS_MACHINE_OK;
}

static bool port_value_fits(enum dos_io_width width, uint32_t value)
{
	if (width == DOS_IO_WIDTH_8)
		return value <= 0xffu;
	if (width == DOS_IO_WIDTH_16)
		return value <= 0xffffu;
	return width == DOS_IO_WIDTH_32;
}

static enum dos_machine_status machine_ready(const struct dos_machine *machine)
{
	if (machine == NULL || machine->poisoned > 1u)
		return DOS_MACHINE_INVALID_ARGUMENT;
	return machine->poisoned == 0u ? DOS_MACHINE_OK : DOS_MACHINE_STOPPED;
}

static bool guest_range_is_valid(const struct dos_machine *machine,
				 dos_linear_address_t address, size_t count)
{
	uint64_t start = (uint64_t)address;
	uint64_t length = (uint64_t)count;

	return start < machine->address_limit &&
	       length <= machine->address_limit - start;
}

static bool native_ranges_overlap(const void *first, const void *second,
				  size_t count)
{
	uintptr_t first_start = (uintptr_t)first;
	uintptr_t second_start = (uintptr_t)second;
	uintptr_t first_end;
	uintptr_t second_end;

	if (check_add_overflow(first_start, count, &first_end) ||
	    check_add_overflow(second_start, count, &second_end))
		return true;
	return first_start < second_end && second_start < first_end;
}

enum dos_machine_status dos_machine_configure(struct dos_machine *machine,
					      const struct dos_machine_ops *ops,
					      kernel_object_handle_t context,
					      uint64_t address_limit,
					      bool a20_enabled)
{
	if (machine == NULL || ops == NULL || ops->read_memory == NULL ||
	    ops->write_memory == NULL || address_limit == 0u ||
	    address_limit > DOS_GUEST_32_ADDRESS_LIMIT)
		return DOS_MACHINE_INVALID_ARGUMENT;

	machine->ops = ops;
	machine->context = context;
	machine->address_limit = address_limit;
	machine->a20_enabled = a20_enabled;
	machine->poisoned = 0u;
	return DOS_MACHINE_OK;
}

enum dos_machine_status dos_machine_read(const struct dos_machine *machine,
					 dos_linear_address_t linear_address,
					 void *destination,
					 size_t destination_capacity,
					 size_t count)
{
	enum dos_machine_status status;

	if (machine == NULL || machine->ops == NULL ||
	    machine->ops->read_memory == NULL)
		return DOS_MACHINE_INVALID_ARGUMENT;
	status = machine_ready(machine);
	if (status != DOS_MACHINE_OK)
		return status;
	if (count == 0u)
		return DOS_MACHINE_OK;
	if (destination == NULL || count > destination_capacity)
		return DOS_MACHINE_INVALID_ARGUMENT;
	if (!guest_range_is_valid(machine, linear_address, count))
		return DOS_MACHINE_ADDRESS_FAULT;

	status = machine->ops->read_memory(machine->context, linear_address,
					  destination, destination_capacity,
					  count);
	return normalize_callback_status(status);
}

enum dos_machine_status dos_machine_write(const struct dos_machine *machine,
					  dos_linear_address_t linear_address,
					  const void *source,
					  size_t source_capacity, size_t count)
{
	enum dos_machine_status status;

	if (machine == NULL || machine->ops == NULL ||
	    machine->ops->write_memory == NULL)
		return DOS_MACHINE_INVALID_ARGUMENT;
	status = machine_ready(machine);
	if (status != DOS_MACHINE_OK)
		return status;
	if (count == 0u)
		return DOS_MACHINE_OK;
	if (source == NULL || count > source_capacity)
		return DOS_MACHINE_INVALID_ARGUMENT;
	if (!guest_range_is_valid(machine, linear_address, count))
		return DOS_MACHINE_ADDRESS_FAULT;

	status = machine->ops->write_memory(machine->context, linear_address,
					   source, source_capacity, count);
	return normalize_callback_status(status);
}

enum dos_machine_status
dos_machine_replace(const struct dos_machine *machine,
		    dos_linear_address_t linear_address, const void *source,
		    size_t source_capacity, void *rollback,
		    size_t rollback_capacity, size_t count)
{
	enum dos_machine_status status;

	if (count == 0u)
		return DOS_MACHINE_OK;
	if (source == NULL || rollback == NULL || count > source_capacity ||
	    count > rollback_capacity ||
	    native_ranges_overlap(source, rollback, count))
		return DOS_MACHINE_INVALID_ARGUMENT;
	status = dos_machine_read(machine, linear_address, rollback,
				  rollback_capacity, count);
	if (status != DOS_MACHINE_OK)
		return status;
	status = dos_machine_write(machine, linear_address, source,
				   source_capacity, count);
	if (status == DOS_MACHINE_OK)
		return DOS_MACHINE_OK;
	if (dos_machine_write(machine, linear_address, rollback,
			      rollback_capacity, count) != DOS_MACHINE_OK)
		return DOS_MACHINE_ROLLBACK_FAILED;
	return status;
}

static size_t far_transfer_chunk(const struct dos_machine *machine,
				 uint16_t segment, uint16_t offset,
				 size_t remaining,
				 dos_linear_address_t *linear_address)
{
	size_t until_offset_wrap = (size_t)0x10000u - (size_t)offset;
	size_t chunk =
	    remaining < until_offset_wrap ? remaining : until_offset_wrap;

	*linear_address =
	    dos_far_to_linear(segment, offset, machine->a20_enabled);
	if (!machine->a20_enabled) {
		size_t until_a20_wrap =
		    (size_t)(DOS_A20_WRAP_ADDRESS - *linear_address);

		if (until_a20_wrap < chunk)
			chunk = until_a20_wrap;
	}
	return chunk;
}

enum dos_machine_status
dos_machine_validate_far(const struct dos_machine *machine, uint16_t segment,
			 uint16_t offset, size_t count)
{
	size_t completed = 0u;
	uint16_t current_offset = offset;
	enum dos_machine_status status;

	if (machine == NULL || machine->address_limit == 0u ||
	    machine->address_limit > DOS_GUEST_32_ADDRESS_LIMIT)
		return DOS_MACHINE_INVALID_ARGUMENT;
	status = machine_ready(machine);
	if (status != DOS_MACHINE_OK)
		return status;
	while (completed < count) {
		dos_linear_address_t linear;
		size_t chunk =
		    far_transfer_chunk(machine, segment, current_offset,
				       count - completed, &linear);

		if (chunk == 0u ||
		    !guest_range_is_valid(machine, linear, chunk))
			return DOS_MACHINE_ADDRESS_FAULT;
		completed += chunk;
		current_offset = (uint16_t)((size_t)current_offset + chunk);
	}
	return DOS_MACHINE_OK;
}

enum dos_machine_status dos_machine_read_far(const struct dos_machine *machine,
					     uint16_t segment, uint16_t offset,
					     void *destination,
					     size_t destination_capacity,
					     size_t count)
{
	uint8_t *output = (uint8_t *)destination;
	size_t completed = 0u;
	uint16_t current_offset = offset;
	enum dos_machine_status status;

	if (machine == NULL || machine->ops == NULL ||
	    machine->ops->read_memory == NULL)
		return DOS_MACHINE_INVALID_ARGUMENT;
	if (count != 0u &&
	    (destination == NULL || count > destination_capacity))
		return DOS_MACHINE_INVALID_ARGUMENT;
	status = dos_machine_validate_far(machine, segment, offset, count);
	if (status != DOS_MACHINE_OK)
		return status;

	while (completed < count) {
		dos_linear_address_t linear;
		size_t chunk =
		    far_transfer_chunk(machine, segment, current_offset,
				       count - completed, &linear);

		status =
		    dos_machine_read(machine, linear, output + completed,
				     destination_capacity - completed, chunk);
		if (status != DOS_MACHINE_OK)
			return status;
		completed += chunk;
		current_offset = (uint16_t)((size_t)current_offset + chunk);
	}
	return DOS_MACHINE_OK;
}

enum dos_machine_status dos_machine_write_far(const struct dos_machine *machine,
					      uint16_t segment, uint16_t offset,
					      const void *source,
					      size_t source_capacity,
					      size_t count)
{
	const uint8_t *input = (const uint8_t *)source;
	size_t completed = 0u;
	uint16_t current_offset = offset;
	enum dos_machine_status status;

	if (machine == NULL || machine->ops == NULL ||
	    machine->ops->write_memory == NULL)
		return DOS_MACHINE_INVALID_ARGUMENT;
	if (count != 0u && (source == NULL || count > source_capacity))
		return DOS_MACHINE_INVALID_ARGUMENT;
	status = dos_machine_validate_far(machine, segment, offset, count);
	if (status != DOS_MACHINE_OK)
		return status;

	while (completed < count) {
		dos_linear_address_t linear;
		size_t chunk =
		    far_transfer_chunk(machine, segment, current_offset,
				       count - completed, &linear);

		status = dos_machine_write(machine, linear, input + completed,
					   source_capacity - completed, chunk);
		if (status != DOS_MACHINE_OK)
			return status;
		completed += chunk;
		current_offset = (uint16_t)((size_t)current_offset + chunk);
	}
	return DOS_MACHINE_OK;
}

enum dos_machine_status
dos_machine_replace_far(const struct dos_machine *machine, uint16_t segment,
			uint16_t offset, const void *source,
			size_t source_capacity, void *rollback,
			size_t rollback_capacity, size_t count)
{
	enum dos_machine_status status;

	if (count == 0u)
		return DOS_MACHINE_OK;
	if (source == NULL || rollback == NULL || count > source_capacity ||
	    count > rollback_capacity ||
	    native_ranges_overlap(source, rollback, count))
		return DOS_MACHINE_INVALID_ARGUMENT;
	status = dos_machine_read_far(machine, segment, offset, rollback,
				      rollback_capacity, count);
	if (status != DOS_MACHINE_OK)
		return status;
	status = dos_machine_write_far(machine, segment, offset, source,
				       source_capacity, count);
	if (status == DOS_MACHINE_OK)
		return DOS_MACHINE_OK;
	if (dos_machine_write_far(machine, segment, offset, rollback,
				  rollback_capacity, count) != DOS_MACHINE_OK)
		return DOS_MACHINE_ROLLBACK_FAILED;
	return status;
}

enum dos_machine_status dos_machine_read_port(const struct dos_machine *machine,
					      uint16_t port,
					      enum dos_io_width width,
					      uint32_t *value)
{
	enum dos_machine_status status;
	uint32_t candidate = 0u;

	if (machine == NULL || machine->ops == NULL ||
	    machine->ops->read_port == NULL || value == NULL ||
	    !io_width_is_valid(width))
		return DOS_MACHINE_INVALID_ARGUMENT;
	status = machine_ready(machine);
	if (status != DOS_MACHINE_OK)
		return status;
	status = normalize_callback_status(machine->ops->read_port(
	    machine->context, port, width, &candidate));
	if (status != DOS_MACHINE_OK)
		return status;
	if (!port_value_fits(width, candidate))
		return DOS_MACHINE_IO_FAULT;
	*value = candidate;
	return DOS_MACHINE_OK;
}

enum dos_machine_status
dos_machine_write_port(const struct dos_machine *machine, uint16_t port,
		       enum dos_io_width width, uint32_t value)
{
	enum dos_machine_status status;

	if (machine == NULL || machine->ops == NULL ||
	    machine->ops->write_port == NULL || !io_width_is_valid(width))
		return DOS_MACHINE_INVALID_ARGUMENT;
	status = machine_ready(machine);
	if (status != DOS_MACHINE_OK)
		return status;
	if (width == DOS_IO_WIDTH_8 && value > 0xffu)
		return DOS_MACHINE_INVALID_ARGUMENT;
	if (width == DOS_IO_WIDTH_16 && value > 0xffffu)
		return DOS_MACHINE_INVALID_ARGUMENT;
	status = machine->ops->write_port(machine->context, port, width, value);
	return normalize_callback_status(status);
}

enum dos_machine_status dos_machine_set_a20(struct dos_machine *machine,
					    bool enabled)
{
	bool observed = false;
	enum dos_machine_status query_status;
	enum dos_machine_status set_status;
	enum dos_machine_status status;

	if (machine == NULL || machine->ops == NULL ||
	    machine->ops->set_a20 == NULL || machine->ops->query_a20 == NULL)
		return DOS_MACHINE_INVALID_ARGUMENT;
	status = machine_ready(machine);
	if (status != DOS_MACHINE_OK)
		return status;
	/* Fail closed before entering the transition.  The set callback alone
	 * cannot prove the physical result, including when it reports success. */
	machine->poisoned = 1u;
	set_status = normalize_callback_status(
		machine->ops->set_a20(machine->context, enabled));
	query_status = query_a20_backend(machine, &observed);
	if (query_status != DOS_MACHINE_OK)
		return set_status != DOS_MACHINE_OK ? set_status : query_status;
	machine->a20_enabled = observed;
	machine->poisoned = 0u;
	/* The final state is authoritative.  A backend-reported failure is
	 * recoverable when the independent query proves the requested result. */
	return observed == enabled ? DOS_MACHINE_OK : DOS_MACHINE_IO_FAULT;
}

enum dos_machine_status dos_machine_query_a20(struct dos_machine *machine,
					      bool *enabled)
{
	bool observed;
	enum dos_machine_status status;

	if (machine == NULL || machine->ops == NULL || enabled == NULL)
		return DOS_MACHINE_INVALID_ARGUMENT;
	status = machine_ready(machine);
	if (status != DOS_MACHINE_OK)
		return status;
	status = query_a20_backend(machine, &observed);
	if (status != DOS_MACHINE_OK) {
		/* A failed state query makes the segmented address transform
		 * unprovable until a new machine lifetime is configured. */
		machine->poisoned = 1u;
		return status;
	}
	machine->a20_enabled = observed;
	*enabled = observed;
	return DOS_MACHINE_OK;
}

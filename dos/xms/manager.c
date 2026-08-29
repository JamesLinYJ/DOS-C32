// SPDX-License-Identifier: GPL-2.0-only
/*
 * Bounded XMS 3.0 service.
 *
 * Compatibility contract: Lotus/Intel/Microsoft/AST XMS 3.0, including the
 * INT 2Fh 4300h/4310h discovery calls and the 2.x handle ABI used by Windows
 * 3.x Setup.  The three-byte VM86 call gate is architectural glue only; all
 * allocation, validation and copying remains native protected-mode C.
 */
#include "dos_xms.h"

#include "hma.h"

#define XMS_VERSION 0x0300u
#define XMS_DRIVER_REVISION 0x0300u
#define XMS_INSTALLATION_CHECK 0x4300u
#define XMS_GET_CONTROL_ADDRESS 0x4310u
#define XMS_GATE_BYTES 3u
#define XMS_MOVE_DESCRIPTOR_BYTES 16u
#define XMS_TRANSFER_BYTES 256u

#define XMS_FUNCTION_VERSION 0x00u
#define XMS_FUNCTION_REQUEST_HMA 0x01u
#define XMS_FUNCTION_RELEASE_HMA 0x02u
#define XMS_FUNCTION_GLOBAL_ENABLE_A20 0x03u
#define XMS_FUNCTION_GLOBAL_DISABLE_A20 0x04u
#define XMS_FUNCTION_LOCAL_ENABLE_A20 0x05u
#define XMS_FUNCTION_LOCAL_DISABLE_A20 0x06u
#define XMS_FUNCTION_QUERY_A20 0x07u
#define XMS_FUNCTION_QUERY_FREE 0x08u
#define XMS_FUNCTION_ALLOCATE 0x09u
#define XMS_FUNCTION_FREE 0x0au
#define XMS_FUNCTION_MOVE 0x0bu
#define XMS_FUNCTION_LOCK 0x0cu
#define XMS_FUNCTION_UNLOCK 0x0du
#define XMS_FUNCTION_HANDLE_INFO 0x0eu
#define XMS_FUNCTION_REALLOCATE 0x0fu
#define XMS_FUNCTION_QUERY_FREE_EXTENDED 0x88u
#define XMS_FUNCTION_ALLOCATE_EXTENDED 0x89u
#define XMS_FUNCTION_HANDLE_INFO_EXTENDED 0x8eu
#define XMS_FUNCTION_REALLOCATE_EXTENDED 0x8fu

#define XMS_ERROR_NOT_IMPLEMENTED 0x80u
#define XMS_ERROR_A20_FAILURE 0x82u
#define XMS_ERROR_A20_STILL_ENABLED 0x94u
#define XMS_ERROR_OUT_OF_MEMORY 0xa0u
#define XMS_ERROR_OUT_OF_HANDLES 0xa1u
#define XMS_ERROR_INVALID_HANDLE 0xa2u
#define XMS_ERROR_INVALID_SOURCE_HANDLE 0xa3u
#define XMS_ERROR_INVALID_SOURCE_OFFSET 0xa4u
#define XMS_ERROR_INVALID_DESTINATION_HANDLE 0xa5u
#define XMS_ERROR_INVALID_DESTINATION_OFFSET 0xa6u
#define XMS_ERROR_INVALID_LENGTH 0xa7u
#define XMS_ERROR_BLOCK_NOT_LOCKED 0xaau
#define XMS_ERROR_BLOCK_LOCKED 0xabu
#define XMS_ERROR_LOCK_OVERFLOW 0xacu

struct xms_move_descriptor {
	uint32_t length;
	uint16_t source_handle;
	uint32_t source_offset;
	uint16_t destination_handle;
	uint32_t destination_offset;
};

static bool valid_context(kernel_object_handle_t context)
{
	return context != 0u && context != KERNEL_OBJECT_HANDLE_INVALID;
}

static bool config_is_valid(const struct dos_xms_config *config)
{
	size_t index;

	if (config == NULL || config->hma_minimum_bytes > DOS_XMS_HMA_BYTES)
		return false;
	for (index = 0u; index < ARRAY_SIZE(config->reserved); ++index) {
		if (config->reserved[index] != 0u)
			return false;
	}
	return true;
}

static bool manager_lifetime_is_valid(const struct dos_xms_manager *manager)
{
	return manager != NULL && manager->constructed == 1u &&
	       manager->initialized == 1u && valid_context(manager->identity) &&
	       manager->generation != 0u;
}

static bool handle_is_allocated(const struct dos_xms_manager *manager,
				 size_t index)
{
	return index < DOS_XMS_HANDLE_COUNT &&
	       (manager->allocated_bitmap & (1u << (uint32_t)index)) != 0u;
}

static uint16_t read_le16(const uint8_t *bytes)
{
	return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8u);
}

static uint32_t read_le32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8u) |
	       ((uint32_t)bytes[2] << 16u) | ((uint32_t)bytes[3] << 24u);
}

static void return_failure(struct dos_cpu_state *state, uint8_t error)
{
	dos_register_set_low16(&state->eax, 0u);
	dos_register_set_low8(&state->ebx, error);
}

static void return_success(struct dos_cpu_state *state)
{
	dos_register_set_low16(&state->eax, 1u);
	dos_register_set_low8(&state->ebx, 0u);
}

static struct dos_xms_handle *resolve_handle(struct dos_xms_manager *manager,
					     uint16_t handle)
{
	if (handle == 0u || handle > DOS_XMS_HANDLE_COUNT ||
	    !handle_is_allocated(manager, handle - 1u))
		return NULL;
	return &manager->handles[handle - 1u];
}

static const struct dos_xms_handle *resolve_const_handle(
	const struct dos_xms_manager *manager, uint16_t handle)
{
	if (handle == 0u || handle > DOS_XMS_HANDLE_COUNT ||
	    !handle_is_allocated(manager, handle - 1u))
		return NULL;
	return &manager->handles[handle - 1u];
}

struct xms_endpoint {
	const struct dos_xms_handle *handle;
	uint64_t offset;
	uint32_t conventional_linear;
	bool conventional;
};

static enum dos_xms_status memory_fault(struct dos_xms_manager *manager,
				       enum dos_xms_memory_status status)
{
	if (status == DOS_XMS_MEMORY_UNCERTAIN)
		manager->poisoned = 1u;
	return DOS_XMS_MACHINE_FAULT;
}

static enum dos_xms_status a20_fault(struct dos_xms_manager *manager,
				     struct dos_machine *machine,
				     struct dos_cpu_state *state)
{
	if (machine->poisoned != 0u) {
		manager->poisoned = 1u;
		return DOS_XMS_MACHINE_FAULT;
	}
	return_failure(state, XMS_ERROR_A20_FAILURE);
	return DOS_XMS_READY;
}

static enum dos_xms_status query_a20(struct dos_xms_manager *manager,
				     struct dos_machine *machine,
				     struct dos_cpu_state *state,
				     bool *enabled)
{
	if (dos_machine_query_a20(machine, enabled) != DOS_MACHINE_OK)
		return a20_fault(manager, machine, state);
	return DOS_XMS_READY;
}

static enum dos_xms_status ensure_a20(struct dos_xms_manager *manager,
				      struct dos_machine *machine,
				      struct dos_cpu_state *state, bool requested,
				      bool *achieved)
{
	bool observed;
	enum dos_xms_status status;

	*achieved = false;
	status = query_a20(manager, machine, state, &observed);
	if (status != DOS_XMS_READY || manager->poisoned != 0u)
		return status;
	if (observed == requested) {
		*achieved = true;
		return DOS_XMS_READY;
	}
	if (dos_machine_set_a20(machine, requested) != DOS_MACHINE_OK)
		return a20_fault(manager, machine, state);
	*achieved = true;
	return DOS_XMS_READY;
}

static bool decode_endpoint(const struct dos_xms_manager *manager,
			    uint16_t handle_number, uint32_t encoded_offset,
			    uint32_t length, bool source,
			    struct xms_endpoint *endpoint, uint8_t *error)
{
	const struct dos_xms_handle *handle;

	if (handle_number == 0u) {
		uint32_t linear = (encoded_offset >> 16u) * 16u +
				  (encoded_offset & 0xffffu);

		if (linear >= 0x100000u || length > 0x100000u - linear) {
			*error = source ? XMS_ERROR_INVALID_SOURCE_OFFSET
					: XMS_ERROR_INVALID_DESTINATION_OFFSET;
			return false;
		}
		*endpoint = (struct xms_endpoint){
			.handle = NULL,
			.offset = linear,
			.conventional_linear = linear,
			.conventional = true,
		};
		return true;
	}
	handle = resolve_const_handle(manager, handle_number);
	if (handle == NULL) {
		*error = source ? XMS_ERROR_INVALID_SOURCE_HANDLE
				: XMS_ERROR_INVALID_DESTINATION_HANDLE;
		return false;
	}
	if ((uint64_t)encoded_offset > handle->size_bytes ||
	    (uint64_t)length > handle->size_bytes - encoded_offset) {
		*error = source ? XMS_ERROR_INVALID_SOURCE_OFFSET
				: XMS_ERROR_INVALID_DESTINATION_OFFSET;
		return false;
	}
	*endpoint = (struct xms_endpoint){
		.handle = handle,
		.offset = encoded_offset,
		.conventional_linear = 0u,
		.conventional = false,
	};
	return true;
}

static enum dos_xms_status move_memory(
	struct dos_xms_manager *manager, const struct dos_machine *machine,
	struct dos_cpu_state *state)
{
	uint8_t raw[XMS_MOVE_DESCRIPTOR_BYTES];
	uint8_t buffer[XMS_TRANSFER_BYTES];
	struct xms_move_descriptor move;
	struct xms_endpoint source;
	struct xms_endpoint destination;
	uint32_t completed = 0u;
	bool backward;
	uint8_t error;

	if (dos_machine_read_far(machine, state->ds,
				 dos_register_low16(state->esi), raw, sizeof(raw),
				 sizeof(raw)) != DOS_MACHINE_OK)
		return DOS_XMS_MACHINE_FAULT;
	move = (struct xms_move_descriptor){
		.length = read_le32(raw),
		.source_handle = read_le16(raw + 4u),
		.source_offset = read_le32(raw + 6u),
		.destination_handle = read_le16(raw + 10u),
		.destination_offset = read_le32(raw + 12u),
	};
	if (move.length == 0u || (move.length & 1u) != 0u) {
		return_failure(state, XMS_ERROR_INVALID_LENGTH);
		return DOS_XMS_READY;
	}
	if (!decode_endpoint(manager, move.source_handle, move.source_offset,
			     move.length, true, &source, &error) ||
	    !decode_endpoint(manager, move.destination_handle,
			     move.destination_offset, move.length, false, &destination,
			     &error)) {
		return_failure(state, error);
		return DOS_XMS_READY;
	}
	backward = source.conventional == destination.conventional &&
		   (source.conventional ||
		    source.handle->block == destination.handle->block) &&
		   destination.offset > source.offset &&
		   destination.offset < source.offset + move.length;
	while (completed < move.length) {
		uint32_t remaining = move.length - completed;
		size_t count = remaining < sizeof(buffer)
				       ? (size_t)remaining
				       : sizeof(buffer);
		uint32_t position = backward
				    ? move.length - completed - (uint32_t)count
				    : completed;
		enum dos_machine_status machine_status;

		if (source.conventional) {
			machine_status = dos_machine_read(
				machine, source.conventional_linear + position, buffer,
				sizeof(buffer), count);
			if (machine_status != DOS_MACHINE_OK)
				return DOS_XMS_MACHINE_FAULT;
		} else {
			enum dos_xms_memory_status status = manager->ops->read(
				manager->memory_context, source.handle->block,
				source.offset + position, buffer, sizeof(buffer),
				count);

			if (status != DOS_XMS_MEMORY_OK)
				return memory_fault(manager, status);
		}
		if (destination.conventional) {
			machine_status = dos_machine_write(
				machine,
				destination.conventional_linear + position, buffer,
				count, count);
			if (machine_status != DOS_MACHINE_OK)
				return DOS_XMS_MACHINE_FAULT;
		} else {
			enum dos_xms_memory_status status = manager->ops->write(
				manager->memory_context, destination.handle->block,
				destination.offset + position, buffer, count, count);

			if (status != DOS_XMS_MEMORY_OK)
				return memory_fault(manager, status);
		}
		completed += (uint32_t)count;
	}
	return_success(state);
	return DOS_XMS_READY;
}

static enum dos_xms_memory_status copy_extended_block(
	const struct dos_xms_manager *manager, dos_xms_block_t source,
	dos_xms_block_t destination, uint64_t byte_count)
{
	uint8_t buffer[XMS_TRANSFER_BYTES];
	uint64_t completed = 0u;

	while (completed < byte_count) {
		uint64_t remaining = byte_count - completed;
		size_t count = remaining < sizeof(buffer)
				       ? (size_t)remaining
				       : sizeof(buffer);
		enum dos_xms_memory_status status = manager->ops->read(
			manager->memory_context, source, completed, buffer,
			sizeof(buffer), count);

		if (status != DOS_XMS_MEMORY_OK)
			return status;
		status = manager->ops->write(manager->memory_context, destination,
					     completed, buffer, count, count);
		if (status != DOS_XMS_MEMORY_OK)
			return status;
		completed += count;
	}
	return DOS_XMS_MEMORY_OK;
}

enum dos_xms_status dos_xms_construct(
	struct dos_xms_manager *manager, kernel_object_handle_t manager_identity)
{
	if (manager == NULL || !valid_context(manager_identity))
		return DOS_XMS_INVALID_ARGUMENT;
	*manager = (struct dos_xms_manager){
		.identity = manager_identity,
		.constructed = 1u,
	};
	return DOS_XMS_READY;
}

enum dos_xms_status dos_xms_initialize(
	struct dos_xms_manager *manager, const struct dos_machine *machine,
	const struct dos_xms_memory_ops *ops,
	kernel_object_handle_t memory_context,
	const struct dos_xms_config *config)
{
	static const uint8_t gate[XMS_GATE_BYTES] = {
		0xcdu, DOS_XMS_CONTROL_VECTOR, 0xcbu,
	};
	struct dos_xms_manager prepared = {0};
	uint64_t largest_bytes;
	uint64_t total_bytes;
	uint64_t highest_address;
	enum dos_xms_memory_status memory_status;

	if (manager == NULL || manager->constructed != 1u ||
	    manager->initialized != 0u || !valid_context(manager->identity) ||
	    machine == NULL || machine->ops == NULL ||
	    machine->ops->set_a20 == NULL || machine->ops->query_a20 == NULL ||
	    ops == NULL || ops->query == NULL || ops->allocate == NULL ||
	    ops->release == NULL || ops->read == NULL || ops->write == NULL ||
	    !valid_context(memory_context) || !config_is_valid(config))
		return DOS_XMS_INVALID_ARGUMENT;
	memory_status = ops->query(memory_context, &largest_bytes, &total_bytes,
				   &highest_address);
	if (memory_status != DOS_XMS_MEMORY_OK)
		return memory_status == DOS_XMS_MEMORY_NO_MEMORY
			       ? DOS_XMS_INVALID_ARGUMENT
			       : DOS_XMS_MACHINE_FAULT;
	if (largest_bytes == 0u || largest_bytes > total_bytes ||
	    highest_address == 0u)
		return DOS_XMS_INVALID_ARGUMENT;
	if (dos_machine_write_far(machine, DOS_XMS_CONTROL_SEGMENT,
				  DOS_XMS_CONTROL_OFFSET, gate, sizeof(gate),
				  sizeof(gate)) != DOS_MACHINE_OK)
		return DOS_XMS_MACHINE_FAULT;
	prepared.ops = ops;
	prepared.memory_context = memory_context;
	prepared.identity = manager->identity;
	prepared.generation = 1u;
	prepared.hma_minimum_bytes = config->hma_minimum_bytes;
	prepared.initialized = 1u;
	prepared.constructed = 1u;
	*manager = prepared;
	return DOS_XMS_READY;
}

enum dos_xms_status dos_xms_multiplex(
	const struct dos_xms_manager *manager, struct dos_cpu_state *state)
{
	uint16_t function;

	if (state == NULL || !manager_lifetime_is_valid(manager))
		return DOS_XMS_CHAIN;
	function = dos_register_low16(state->eax);
	if (function == XMS_INSTALLATION_CHECK) {
		dos_register_set_low8(&state->eax, 0x80u);
		return DOS_XMS_READY;
	}
	if (function == XMS_GET_CONTROL_ADDRESS) {
		state->es = DOS_XMS_CONTROL_SEGMENT;
		dos_register_set_low16(&state->ebx, DOS_XMS_CONTROL_OFFSET);
		return DOS_XMS_READY;
	}
	return DOS_XMS_CHAIN;
}

enum dos_xms_status dos_xms_control(
	struct dos_xms_manager *manager, struct dos_machine *machine,
	struct dos_cpu_state *state)
{
	struct dos_xms_handle *handle;
	dos_xms_block_t new_block;
	uint64_t largest_bytes;
	uint64_t total_bytes;
	uint64_t highest_address;
	uint64_t requested_bytes;
	uint64_t new_physical_address;
	uint64_t new_capacity_bytes;
	enum dos_xms_memory_status memory_status;
	uint16_t handle_number;
	uint32_t requested_kib;
	uint8_t function;
	size_t index;

	if (machine == NULL || state == NULL ||
	    !manager_lifetime_is_valid(manager))
		return DOS_XMS_INVALID_ARGUMENT;
	if (manager->poisoned != 0u)
		return DOS_XMS_MACHINE_FAULT;
	function = dos_register_high8(state->eax);
	switch (function) {
	case XMS_FUNCTION_VERSION:
		dos_register_set_low16(&state->eax, XMS_VERSION);
		dos_register_set_low16(&state->ebx, XMS_DRIVER_REVISION);
		return dos_xms_hma_report_version(manager, machine, state);
	case XMS_FUNCTION_REQUEST_HMA:
		return dos_xms_hma_request(manager, machine, state);
	case XMS_FUNCTION_RELEASE_HMA:
		return dos_xms_hma_release(manager, machine, state);
	case XMS_FUNCTION_GLOBAL_ENABLE_A20:
		{
			bool achieved;
			enum dos_xms_status status =
				ensure_a20(manager, machine, state, true,
					   &achieved);

			if (status != DOS_XMS_READY || !achieved)
				return status;
		}
		return_success(state);
		return DOS_XMS_READY;
	case XMS_FUNCTION_GLOBAL_DISABLE_A20:
		if (manager->local_a20_locks != 0u) {
			return_failure(state, XMS_ERROR_A20_STILL_ENABLED);
			return DOS_XMS_READY;
		}
		{
			bool achieved;
			enum dos_xms_status status =
				ensure_a20(manager, machine, state, false,
					   &achieved);

			if (status != DOS_XMS_READY || !achieved)
				return status;
		}
		return_success(state);
		return DOS_XMS_READY;
	case XMS_FUNCTION_LOCAL_ENABLE_A20:
		if (manager->local_a20_locks == 0xffffu) {
			return_failure(state, XMS_ERROR_A20_FAILURE);
			return DOS_XMS_READY;
		}
		{
			bool achieved;
			enum dos_xms_status status =
				ensure_a20(manager, machine, state, true,
					   &achieved);

			if (status != DOS_XMS_READY || !achieved)
				return status;
		}
		++manager->local_a20_locks;
		return_success(state);
		return DOS_XMS_READY;
	case XMS_FUNCTION_LOCAL_DISABLE_A20:
		if (manager->local_a20_locks == 0u) {
			return_failure(state, XMS_ERROR_A20_FAILURE);
			return DOS_XMS_READY;
		}
		if (manager->local_a20_locks > 1u) {
			bool achieved;
			enum dos_xms_status status =
				ensure_a20(manager, machine, state, true,
					   &achieved);

			if (status != DOS_XMS_READY || !achieved)
				return status;
			--manager->local_a20_locks;
			return_failure(state, XMS_ERROR_A20_STILL_ENABLED);
			return DOS_XMS_READY;
		}
		{
			bool achieved;
			enum dos_xms_status status =
				ensure_a20(manager, machine, state, false,
					   &achieved);

			if (status != DOS_XMS_READY || !achieved)
				return status;
		}
		manager->local_a20_locks = 0u;
		return_success(state);
		return DOS_XMS_READY;
	case XMS_FUNCTION_QUERY_A20:
		{
			bool enabled;
			enum dos_xms_status status =
				query_a20(manager, machine, state, &enabled);

			if (status != DOS_XMS_READY)
				return status;
			dos_register_set_low16(&state->eax, enabled ? 1u : 0u);
		}
		dos_register_set_low8(&state->ebx, 0u);
		return DOS_XMS_READY;
	case XMS_FUNCTION_QUERY_FREE:
	case XMS_FUNCTION_QUERY_FREE_EXTENDED:
		memory_status = manager->ops->query(
			manager->memory_context, &largest_bytes, &total_bytes,
			&highest_address);
		if (memory_status != DOS_XMS_MEMORY_OK)
			return memory_fault(manager, memory_status);
		if (largest_bytes > total_bytes) {
			manager->poisoned = 1u;
			return DOS_XMS_MACHINE_FAULT;
		}
		largest_bytes >>= 10u;
		total_bytes >>= 10u;
		if (function == XMS_FUNCTION_QUERY_FREE_EXTENDED) {
			state->eax = (uint32_t)(largest_bytes > 0xffffffffu
						? 0xffffffffu
						: largest_bytes);
			state->edx = (uint32_t)(total_bytes > 0xffffffffu
						? 0xffffffffu
						: total_bytes);
			state->ecx = (uint32_t)(highest_address > 0xffffffffu
						? 0xffffffffu
						: highest_address);
		} else {
			dos_register_set_low16(
				&state->eax,
				(uint16_t)(largest_bytes > 0xffffu
						   ? 0xffffu
						   : largest_bytes));
			dos_register_set_low16(
				&state->edx,
				(uint16_t)(total_bytes > 0xffffu
						   ? 0xffffu
						   : total_bytes));
		}
		dos_register_set_low8(&state->ebx,
				      total_bytes == 0u ? XMS_ERROR_OUT_OF_MEMORY
							: 0u);
		return DOS_XMS_READY;
	case XMS_FUNCTION_ALLOCATE:
	case XMS_FUNCTION_ALLOCATE_EXTENDED:
		requested_kib = function == XMS_FUNCTION_ALLOCATE_EXTENDED
				? state->edx
				: dos_register_low16(state->edx);
		if (requested_kib == 0u) {
			return_failure(state, XMS_ERROR_INVALID_LENGTH);
			return DOS_XMS_READY;
		}
		for (index = 0u; index < DOS_XMS_HANDLE_COUNT; ++index) {
			if (!handle_is_allocated(manager, index))
				break;
		}
		if (index == DOS_XMS_HANDLE_COUNT) {
			return_failure(state, XMS_ERROR_OUT_OF_HANDLES);
			return DOS_XMS_READY;
		}
		requested_bytes = (uint64_t)requested_kib << 10u;
		memory_status = manager->ops->allocate(
			manager->memory_context, requested_bytes, &new_block,
			&new_physical_address, &new_capacity_bytes);
		if (memory_status == DOS_XMS_MEMORY_NO_MEMORY) {
			return_failure(state, XMS_ERROR_OUT_OF_MEMORY);
			return DOS_XMS_READY;
		}
		if (memory_status != DOS_XMS_MEMORY_OK)
			return memory_fault(manager, memory_status);
		if (new_block == DOS_XMS_BLOCK_INVALID ||
		    new_capacity_bytes < requested_bytes ||
		    new_physical_address > 0xffffffffu) {
			memory_status = manager->ops->release(manager->memory_context,
							      new_block);
			manager->poisoned = 1u;
			(void)memory_status;
			return DOS_XMS_MACHINE_FAULT;
		}
		manager->handles[index] = (struct dos_xms_handle){
			.block = new_block,
			.size_bytes = requested_bytes,
			.capacity_bytes = new_capacity_bytes,
			.physical_address = new_physical_address,
			.lock_count = 0u,
		};
		manager->allocated_bitmap |= 1u << (uint32_t)index;
		return_success(state);
		dos_register_set_low16(&state->edx, (uint16_t)(index + 1u));
		return DOS_XMS_READY;
	case XMS_FUNCTION_FREE:
		handle = resolve_handle(manager, dos_register_low16(state->edx));
		if (handle == NULL) {
			return_failure(state, XMS_ERROR_INVALID_HANDLE);
			return DOS_XMS_READY;
		}
		if (handle->lock_count != 0u) {
			return_failure(state, XMS_ERROR_BLOCK_LOCKED);
			return DOS_XMS_READY;
		}
		memory_status = manager->ops->release(manager->memory_context,
						      handle->block);
		if (memory_status != DOS_XMS_MEMORY_OK)
			return memory_fault(manager, memory_status);
		index = (size_t)(handle - &manager->handles[0]);
		*handle = (struct dos_xms_handle){0};
		manager->allocated_bitmap &= ~(1u << (uint32_t)index);
		return_success(state);
		return DOS_XMS_READY;
	case XMS_FUNCTION_MOVE:
		return move_memory(manager, machine, state);
	case XMS_FUNCTION_LOCK:
		handle = resolve_handle(manager, dos_register_low16(state->edx));
		if (handle == NULL) {
			return_failure(state, XMS_ERROR_INVALID_HANDLE);
			return DOS_XMS_READY;
		}
		if (handle->lock_count == 0xffu) {
			return_failure(state, XMS_ERROR_LOCK_OVERFLOW);
			return DOS_XMS_READY;
		}
		++handle->lock_count;
		return_success(state);
		dos_register_set_low16(&state->ebx,
				       (uint16_t)handle->physical_address);
		dos_register_set_low16(
			&state->edx, (uint16_t)(handle->physical_address >> 16u));
		return DOS_XMS_READY;
	case XMS_FUNCTION_UNLOCK:
		handle = resolve_handle(manager, dos_register_low16(state->edx));
		if (handle == NULL) {
			return_failure(state, XMS_ERROR_INVALID_HANDLE);
			return DOS_XMS_READY;
		}
		if (handle->lock_count == 0u) {
			return_failure(state, XMS_ERROR_BLOCK_NOT_LOCKED);
			return DOS_XMS_READY;
		}
		--handle->lock_count;
		return_success(state);
		return DOS_XMS_READY;
	case XMS_FUNCTION_HANDLE_INFO:
	case XMS_FUNCTION_HANDLE_INFO_EXTENDED:
		handle = resolve_handle(manager, dos_register_low16(state->edx));
		if (handle == NULL) {
			return_failure(state, XMS_ERROR_INVALID_HANDLE);
			return DOS_XMS_READY;
		}
		handle_number = 0u;
		for (index = 0u; index < DOS_XMS_HANDLE_COUNT; ++index) {
			if (!handle_is_allocated(manager, index))
				++handle_number;
		}
		return_success(state);
		dos_register_set_high8(&state->ebx, handle->lock_count);
		if (function == XMS_FUNCTION_HANDLE_INFO_EXTENDED) {
			state->ecx = handle_number;
			state->edx =
				(uint32_t)((handle->size_bytes >> 10u) >
						   0xffffffffu
					   ? 0xffffffffu
					   : (handle->size_bytes >> 10u));
		} else {
			dos_register_set_low8(&state->ebx,
					      (uint8_t)handle_number);
			dos_register_set_low16(
				&state->edx,
				(uint16_t)((handle->size_bytes >> 10u) >
						   0xffffu
					   ? 0xffffu
					   : (handle->size_bytes >> 10u)));
		}
		return DOS_XMS_READY;
	case XMS_FUNCTION_REALLOCATE:
	case XMS_FUNCTION_REALLOCATE_EXTENDED:
		handle = resolve_handle(manager, dos_register_low16(state->edx));
		requested_kib = function == XMS_FUNCTION_REALLOCATE_EXTENDED
				? state->ebx
				: dos_register_low16(state->ebx);
		if (handle == NULL) {
			return_failure(state, XMS_ERROR_INVALID_HANDLE);
			return DOS_XMS_READY;
		}
		if (handle->lock_count != 0u) {
			return_failure(state, XMS_ERROR_BLOCK_LOCKED);
			return DOS_XMS_READY;
		}
		if (requested_kib == 0u) {
			return_failure(state, XMS_ERROR_INVALID_LENGTH);
			return DOS_XMS_READY;
		}
		requested_bytes = (uint64_t)requested_kib << 10u;
		if (requested_bytes <= handle->capacity_bytes) {
			handle->size_bytes = requested_bytes;
			return_success(state);
			return DOS_XMS_READY;
		}
		memory_status = manager->ops->allocate(
			manager->memory_context, requested_bytes, &new_block,
			&new_physical_address, &new_capacity_bytes);
		if (memory_status == DOS_XMS_MEMORY_NO_MEMORY) {
			return_failure(state, XMS_ERROR_OUT_OF_MEMORY);
			return DOS_XMS_READY;
		}
		if (memory_status != DOS_XMS_MEMORY_OK)
			return memory_fault(manager, memory_status);
		if (new_block == DOS_XMS_BLOCK_INVALID ||
		    new_capacity_bytes < requested_bytes ||
		    new_physical_address > 0xffffffffu) {
			(void)manager->ops->release(manager->memory_context,
						    new_block);
			manager->poisoned = 1u;
			return DOS_XMS_MACHINE_FAULT;
		}
		memory_status = copy_extended_block(
			manager, handle->block, new_block, handle->size_bytes);
		if (memory_status != DOS_XMS_MEMORY_OK) {
			enum dos_xms_memory_status cleanup_status =
				manager->ops->release(manager->memory_context,
						      new_block);

			if (cleanup_status != DOS_XMS_MEMORY_OK)
				manager->poisoned = 1u;
			return memory_fault(manager, memory_status);
		}
		memory_status = manager->ops->release(manager->memory_context,
						      handle->block);
		if (memory_status != DOS_XMS_MEMORY_OK) {
			enum dos_xms_memory_status cleanup_status =
				manager->ops->release(manager->memory_context,
						      new_block);

			if (cleanup_status != DOS_XMS_MEMORY_OK)
				manager->poisoned = 1u;
			return memory_fault(manager, memory_status);
		}
		*handle = (struct dos_xms_handle){
			.block = new_block,
			.size_bytes = requested_bytes,
			.capacity_bytes = new_capacity_bytes,
			.physical_address = new_physical_address,
			.lock_count = 0u,
		};
		return_success(state);
		return DOS_XMS_READY;
	default:
		return_failure(state, XMS_ERROR_NOT_IMPLEMENTED);
		return DOS_XMS_READY;
	}
}

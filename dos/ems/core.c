// SPDX-License-Identifier: GPL-2.0-only
/*
 * DOS-C32 EMS 4.0 manager core
 *
 * DOS contract:   dispatch the standard INT 67h register services and carry
 *                 VCPI requests through the separately owned platform gate
 * Safety changes: fixed tables, generation-bounded reuse, validated page
 *                 leases, transactional register outputs and reverse unwind
 */
#include "private.h"

#define EMS_FUNCTION_STATUS 0x40u
#define EMS_FUNCTION_PAGE_FRAME 0x41u
#define EMS_FUNCTION_PAGE_COUNTS 0x42u
#define EMS_FUNCTION_ALLOCATE 0x43u
#define EMS_FUNCTION_MAP 0x44u
#define EMS_FUNCTION_RELEASE 0x45u
#define EMS_FUNCTION_VERSION 0x46u
#define EMS_FUNCTION_VCPI 0xdeu

#define EMS_UNMAP_LOGICAL_PAGE 0xffffu
#define EMS_VISIBLE_PAGE_LIMIT 0xffffu

static const uint8_t ems_device_name[DOS_EMS_DEVICE_NAME_BYTES] = {
	'E', 'M', 'M', 'X', 'X', 'X', 'X', '0'
};

bool dos_ems_identity_is_valid(kernel_object_handle_t identity)
{
	return identity != 0u && identity != KERNEL_OBJECT_HANDLE_INVALID;
}

static bool page_operations_are_complete(
	const struct dos_ems_page_ops *operations)
{
	return operations != NULL && operations->query != NULL &&
	       operations->allocate != NULL && operations->release != NULL;
}

static bool frame_operations_are_complete(
	const struct dos_ems_page_frame_ops *operations)
{
	return operations != NULL && operations->acquire != NULL &&
	       operations->release != NULL && operations->map != NULL &&
	       operations->unmap != NULL;
}

static bool vcpi_operations_are_complete(
	const struct dos_vcpi_platform_ops *operations)
{
	return operations != NULL && operations->translate_low_page != NULL &&
	       operations->read_virtual_cr0 != NULL &&
	       operations->query_pic_mappings != NULL &&
	       operations->set_pic_mappings != NULL &&
	       operations->handoff != NULL;
}

static bool service_config_is_valid(const struct dos_ems_config *config)
{
	uint64_t frame_address;
	uint64_t frame_bytes = (uint64_t)DOS_EMS_PAGE_BYTES *
			       DOS_EMS_PAGE_FRAME_SLOTS;

	if (config == NULL || config->reserved != 0u ||
	    config->reserved2 != 0u ||
	    (config->page_frame_segment &
	     (DOS_EMS_PAGE_PARAGRAPHS - 1u)) != 0u)
		return false;
	frame_address = (uint64_t)config->page_frame_segment << 4u;
	return frame_bytes <= DOS_A20_WRAP_ADDRESS &&
	       frame_address <= DOS_A20_WRAP_ADDRESS - frame_bytes;
}

bool dos_ems_runtime_config_is_valid(
	const struct dos_ems_runtime_config *config)
{
	uint32_t index;

	if (config == NULL || !service_config_is_valid(&config->service))
		return false;
	for (index = 0u; index < DOS_EMS_DEVICE_NAME_BYTES; ++index) {
		if (config->device_name[index] != ems_device_name[index])
			return false;
	}
	for (index = 0u; index < ARRAY_SIZE(config->reserved); ++index) {
		if (config->reserved[index] != 0u)
			return false;
	}
	return true;
}

void dos_ems_construct(struct dos_ems_manager *manager)
{
	if (manager == NULL)
		return;
	*manager = (struct dos_ems_manager){
		.page_context = KERNEL_OBJECT_HANDLE_INVALID,
		.page_frame = {
			.context = KERNEL_OBJECT_HANDLE_INVALID,
			.lease = DOS_EMS_PAGE_FRAME_LEASE_INVALID,
		},
		.vcpi_context = KERNEL_OBJECT_HANDLE_INVALID,
		.constructed = 1u,
	};
}

bool dos_ems_page_snapshot_is_valid(
	const struct dos_ems_page_snapshot *snapshot)
{
	return snapshot != NULL && snapshot->managed_pages != 0u &&
	       snapshot->largest_free_pages <= snapshot->total_free_pages &&
	       snapshot->total_free_pages <= snapshot->managed_pages &&
	       snapshot->highest_address >= DOS_EMS_NATIVE_PAGE_BYTES - 1u &&
	       (snapshot->highest_address &
		(DOS_EMS_NATIVE_PAGE_BYTES - 1u)) ==
		       DOS_EMS_NATIVE_PAGE_BYTES - 1u &&
	       snapshot->managed_pages <=
		       (snapshot->highest_address >>
			DOS_EMS_NATIVE_PAGE_SHIFT) + 1u;
}

enum dos_ems_status dos_ems_page_fault(
	struct dos_ems_manager *manager, enum dos_ems_page_status status)
{
	if (manager == NULL)
		return DOS_EMS_INVALID_ARGUMENT;
	switch (status) {
	case DOS_EMS_PAGE_OK:
		return DOS_EMS_READY;
	case DOS_EMS_PAGE_NO_MEMORY:
	case DOS_EMS_PAGE_FAULT:
		return DOS_EMS_MEMORY_FAULT;
	case DOS_EMS_PAGE_INVALID_BLOCK:
	case DOS_EMS_PAGE_UNCERTAIN:
	default:
		manager->poisoned = 1u;
		return DOS_EMS_POISONED;
	}
}

enum dos_ems_status dos_ems_query_pages(
	struct dos_ems_manager *manager,
	struct dos_ems_page_snapshot *snapshot)
{
	struct dos_ems_page_snapshot prepared = {0};
	enum dos_ems_page_status page_status;

	if (manager == NULL || snapshot == NULL ||
	    !page_operations_are_complete(manager->page_ops) ||
	    !dos_ems_identity_is_valid(manager->page_context))
		return DOS_EMS_INVALID_ARGUMENT;
	if (manager->poisoned != 0u)
		return DOS_EMS_POISONED;
	page_status = manager->page_ops->query(manager->page_context, &prepared);
	if (page_status != DOS_EMS_PAGE_OK)
		return dos_ems_page_fault(manager, page_status);
	if (!dos_ems_page_snapshot_is_valid(&prepared)) {
		manager->poisoned = 1u;
		return DOS_EMS_POISONED;
	}
	*snapshot = prepared;
	return DOS_EMS_READY;
}

bool dos_ems_allocation_result_is_valid(
	dos_ems_page_block_t block, uint64_t physical_address,
	uint64_t requested_pages, uint64_t capacity_pages)
{
	uint64_t capacity_bytes;
	uint64_t guest_page_limit =
		DOS_GUEST_32_ADDRESS_LIMIT >> DOS_EMS_NATIVE_PAGE_SHIFT;

	if (block == DOS_EMS_PAGE_BLOCK_INVALID || requested_pages == 0u ||
	    capacity_pages < requested_pages ||
	    capacity_pages > guest_page_limit ||
	    (physical_address & (DOS_EMS_NATIVE_PAGE_BYTES - 1u)) != 0u)
		return false;
	capacity_bytes = capacity_pages << DOS_EMS_NATIVE_PAGE_SHIFT;
	return physical_address <=
	       DOS_GUEST_32_ADDRESS_LIMIT - capacity_bytes;
}

enum dos_ems_status dos_ems_reject_invalid_allocation(
	struct dos_ems_manager *manager, dos_ems_page_block_t block)
{
	enum dos_ems_page_status page_status;

	if (manager == NULL ||
	    !page_operations_are_complete(manager->page_ops) ||
	    !dos_ems_identity_is_valid(manager->page_context))
		return DOS_EMS_INVALID_ARGUMENT;
	page_status = manager->page_ops->release(manager->page_context, block);
	(void)page_status;
	manager->poisoned = 1u;
	return DOS_EMS_POISONED;
}

void dos_ems_return_status(struct dos_cpu_state *state, uint8_t status)
{
	if (state != NULL)
		dos_register_set_high8(&state->eax, status);
}

static bool frame_binding_is_valid(
	const struct dos_ems_page_frame_binding *binding,
	const struct dos_ems_config *config)
{
	uint64_t expected_address;
	uint64_t expected_bytes = (uint64_t)DOS_EMS_PAGE_BYTES *
				 DOS_EMS_PAGE_FRAME_SLOTS;

	if (binding == NULL || !service_config_is_valid(config) ||
	    !frame_operations_are_complete(binding->ops) ||
	    !dos_ems_identity_is_valid(binding->context) ||
	    binding->lease == DOS_EMS_PAGE_FRAME_LEASE_INVALID)
		return false;
	expected_address = (uint64_t)config->page_frame_segment << 4u;
	return binding->linear_address == expected_address &&
	       binding->byte_count == expected_bytes;
}

enum dos_ems_status dos_ems_initialize(
	struct dos_ems_manager *manager,
	const struct dos_ems_page_ops *page_ops,
	kernel_object_handle_t page_context,
	const struct dos_ems_page_frame_binding *page_frame,
	const struct dos_vcpi_platform_ops *vcpi_ops,
	kernel_object_handle_t vcpi_context,
	const struct dos_ems_config *config)
{
	struct dos_ems_manager prepared;
	struct dos_ems_page_snapshot snapshot;
	enum dos_ems_status status;
	bool vcpi_present = vcpi_ops != NULL;

	if (manager == NULL || manager->constructed != 1u ||
	    manager->initialized != 0u || manager->poisoned != 0u ||
	    !page_operations_are_complete(page_ops) ||
	    !dos_ems_identity_is_valid(page_context) ||
	    !frame_binding_is_valid(page_frame, config))
		return DOS_EMS_INVALID_ARGUMENT;
	if (vcpi_present) {
		if (!vcpi_operations_are_complete(vcpi_ops) ||
		    !dos_ems_identity_is_valid(vcpi_context))
			return DOS_EMS_INVALID_ARGUMENT;
	} else if (dos_ems_identity_is_valid(vcpi_context)) {
		return DOS_EMS_INVALID_ARGUMENT;
	}

	dos_ems_construct(&prepared);
	prepared.page_ops = page_ops;
	prepared.page_context = page_context;
	prepared.page_frame = *page_frame;
	prepared.vcpi_ops = vcpi_present ? vcpi_ops : NULL;
	prepared.vcpi_context = vcpi_present
					? vcpi_context
					: KERNEL_OBJECT_HANDLE_INVALID;
	prepared.page_frame_segment = config->page_frame_segment;
	prepared.vcpi_available = vcpi_present ? 1u : 0u;
	status = dos_ems_query_pages(&prepared, &snapshot);
	if (status != DOS_EMS_READY) {
		if (status == DOS_EMS_POISONED)
			manager->poisoned = 1u;
		return status;
	}
	prepared.initialized = 1u;
	*manager = prepared;
	return DOS_EMS_READY;
}

static bool handle_bit_is_set(const struct dos_ems_manager *manager,
			      uint32_t index)
{
	return (manager->allocated_handles & (1u << index)) != 0u;
}

static void handle_bit_set(struct dos_ems_manager *manager, uint32_t index)
{
	manager->allocated_handles |= 1u << index;
}

static void handle_bit_clear(struct dos_ems_manager *manager, uint32_t index)
{
	manager->allocated_handles &= ~(1u << index);
}

static uint32_t find_free_handle(const struct dos_ems_manager *manager)
{
	uint32_t index;

	for (index = 0u; index < DOS_EMS_HANDLE_COUNT; ++index) {
		if (!handle_bit_is_set(manager, index) &&
		    manager->handles[index].generation < DOS_EMS_GENERATION_MAX)
			return index;
	}
	return DOS_EMS_HANDLE_COUNT;
}

static struct dos_ems_handle *resolve_handle(
	struct dos_ems_manager *manager, uint16_t visible_handle)
{
	uint32_t index;

	if (visible_handle == 0u || visible_handle > DOS_EMS_HANDLE_COUNT)
		return NULL;
	index = (uint32_t)visible_handle - 1u;
	return handle_bit_is_set(manager, index) ? &manager->handles[index]
					       : NULL;
}

static bool vcpi_bit_is_set(const struct dos_ems_manager *manager,
			    uint32_t index)
{
	return (manager->vcpi_allocated[index >> 5u] &
		(1u << (index & 31u))) != 0u;
}

static bool handle_entry_is_valid(const struct dos_ems_manager *manager,
				  uint32_t index)
{
	const struct dos_ems_handle *handle = &manager->handles[index];
	uint64_t requested_pages;

	if (handle->generation > DOS_EMS_GENERATION_MAX)
		return false;
	if (!handle_bit_is_set(manager, index))
		return handle->block == DOS_EMS_PAGE_BLOCK_INVALID &&
		       handle->physical_address == 0u &&
		       handle->page_count == 0u && handle->capacity_pages == 0u;
	if (handle->generation == 0u || handle->page_count == 0u ||
	    handle->page_count > EMS_VISIBLE_PAGE_LIMIT)
		return false;
	requested_pages = handle->page_count <<
			  (DOS_EMS_PAGE_SHIFT - DOS_EMS_NATIVE_PAGE_SHIFT);
	return dos_ems_allocation_result_is_valid(
		handle->block, handle->physical_address, requested_pages,
		handle->capacity_pages);
}

static bool vcpi_entry_is_valid(const struct dos_ems_manager *manager,
				uint32_t index)
{
	const struct dos_vcpi_allocation *allocation =
		&manager->vcpi_allocations[index];

	if (allocation->generation > DOS_EMS_GENERATION_MAX)
		return false;
	if (!vcpi_bit_is_set(manager, index))
		return allocation->block == DOS_EMS_PAGE_BLOCK_INVALID &&
		       allocation->physical_address == 0u &&
		       allocation->capacity_pages == 0u;
	return allocation->generation != 0u &&
	       dos_ems_allocation_result_is_valid(
		       allocation->block, allocation->physical_address, 1u,
		       allocation->capacity_pages);
}

static bool frame_slot_is_valid(struct dos_ems_manager *manager,
				uint32_t index)
{
	const struct dos_ems_page_frame_slot *slot =
		&manager->frame_slots[index];
	struct dos_ems_handle *handle;
	uint64_t expected_address;
	uint32_t reserved_index;

	for (reserved_index = 0u;
	     reserved_index < ARRAY_SIZE(slot->reserved); ++reserved_index) {
		if (slot->reserved[reserved_index] != 0u)
			return false;
	}
	if (slot->mapped == 0u)
		return slot->physical_address == 0u && slot->handle == 0u &&
		       slot->logical_page == 0u;
	if (slot->mapped != 1u)
		return false;
	handle = resolve_handle(manager, slot->handle);
	if (handle == NULL || slot->logical_page >= handle->page_count)
		return false;
	expected_address = handle->physical_address +
			   ((uint64_t)slot->logical_page <<
			    DOS_EMS_PAGE_SHIFT);
	return slot->physical_address == expected_address;
}

static bool manager_state_is_valid(struct dos_ems_manager *manager)
{
	struct dos_ems_config config = {
		.page_frame_segment = manager->page_frame_segment,
	};
	uint32_t index;

	if (!page_operations_are_complete(manager->page_ops) ||
	    !dos_ems_identity_is_valid(manager->page_context) ||
	    !frame_binding_is_valid(&manager->page_frame, &config) ||
	    manager->vcpi_available > 1u)
		return false;
	if (manager->vcpi_available != 0u) {
		if (!vcpi_operations_are_complete(manager->vcpi_ops) ||
		    !dos_ems_identity_is_valid(manager->vcpi_context))
			return false;
	} else if (manager->vcpi_ops != NULL ||
		   dos_ems_identity_is_valid(manager->vcpi_context)) {
		return false;
	}
	for (index = 0u; index < ARRAY_SIZE(manager->reserved); ++index) {
		if (manager->reserved[index] != 0u)
			return false;
	}
	for (index = 0u; index < DOS_EMS_HANDLE_COUNT; ++index) {
		if (!handle_entry_is_valid(manager, index))
			return false;
	}
	for (index = 0u; index < DOS_VCPI_ALLOCATION_COUNT; ++index) {
		if (!vcpi_entry_is_valid(manager, index))
			return false;
	}
	for (index = 0u; index < DOS_EMS_PAGE_FRAME_SLOTS; ++index) {
		if (!frame_slot_is_valid(manager, index))
			return false;
	}
	return true;
}

static uint16_t visible_ems_pages(uint64_t native_pages)
{
	uint64_t pages = native_pages >>
			 (DOS_EMS_PAGE_SHIFT - DOS_EMS_NATIVE_PAGE_SHIFT);

	return pages > EMS_VISIBLE_PAGE_LIMIT ? EMS_VISIBLE_PAGE_LIMIT
					      : (uint16_t)pages;
}

static enum dos_ems_status query_page_counts(
	struct dos_ems_manager *manager, struct dos_cpu_state *state)
{
	struct dos_ems_page_snapshot snapshot;
	enum dos_ems_status status;

	status = dos_ems_query_pages(manager, &snapshot);
	if (status != DOS_EMS_READY)
		return status;
	dos_register_set_low16(&state->ebx,
			       visible_ems_pages(snapshot.total_free_pages));
	dos_register_set_low16(&state->edx,
			       visible_ems_pages(snapshot.managed_pages));
	dos_ems_return_status(state, DOS_EMS_STATUS_OK);
	return DOS_EMS_READY;
}

static enum dos_ems_status allocate_handle(
	struct dos_ems_manager *manager, struct dos_cpu_state *state)
{
	struct dos_ems_page_snapshot snapshot;
	dos_ems_page_block_t block = DOS_EMS_PAGE_BLOCK_INVALID;
	uint64_t physical_address = 0u;
	uint64_t capacity_pages = 0u;
	uint64_t requested_native_pages;
	uint64_t generation;
	enum dos_ems_page_status page_status;
	enum dos_ems_status status;
	uint16_t requested_pages = dos_register_low16(state->ebx);
	uint32_t index;

	if (requested_pages == 0u) {
		dos_ems_return_status(state, DOS_EMS_STATUS_ZERO_PAGES);
		return DOS_EMS_READY;
	}
	index = find_free_handle(manager);
	if (index == DOS_EMS_HANDLE_COUNT) {
		dos_ems_return_status(state, DOS_EMS_STATUS_NO_MORE_HANDLES);
		return DOS_EMS_READY;
	}
	requested_native_pages = (uint64_t)requested_pages <<
				 (DOS_EMS_PAGE_SHIFT -
				  DOS_EMS_NATIVE_PAGE_SHIFT);
	status = dos_ems_query_pages(manager, &snapshot);
	if (status != DOS_EMS_READY)
		return status;
	if (requested_native_pages > snapshot.managed_pages) {
		dos_ems_return_status(state, DOS_EMS_STATUS_OUT_OF_PAGES);
		return DOS_EMS_READY;
	}
	if (requested_native_pages > snapshot.total_free_pages ||
	    requested_native_pages > snapshot.largest_free_pages) {
		dos_ems_return_status(state, DOS_EMS_STATUS_OUT_OF_FREE_PAGES);
		return DOS_EMS_READY;
	}
	page_status = manager->page_ops->allocate(
		manager->page_context, requested_native_pages, &block,
		&physical_address, &capacity_pages);
	if (page_status == DOS_EMS_PAGE_NO_MEMORY) {
		dos_ems_return_status(state, DOS_EMS_STATUS_OUT_OF_FREE_PAGES);
		return DOS_EMS_READY;
	}
	if (page_status != DOS_EMS_PAGE_OK)
		return dos_ems_page_fault(manager, page_status);
	if (!dos_ems_allocation_result_is_valid(
			block, physical_address, requested_native_pages,
			capacity_pages))
		return dos_ems_reject_invalid_allocation(manager, block);

	generation = manager->handles[index].generation + 1u;
	manager->handles[index] = (struct dos_ems_handle){
		.block = block,
		.generation = generation,
		.physical_address = physical_address,
		.page_count = requested_pages,
		.capacity_pages = capacity_pages,
	};
	handle_bit_set(manager, index);
	dos_register_set_low16(&state->edx, (uint16_t)(index + 1u));
	dos_ems_return_status(state, DOS_EMS_STATUS_OK);
	return DOS_EMS_READY;
}

static enum dos_ems_status frame_operation_status(
	struct dos_ems_manager *manager,
	enum dos_ems_page_frame_status frame_status)
{
	switch (frame_status) {
	case DOS_EMS_PAGE_FRAME_OK:
		return DOS_EMS_READY;
	case DOS_EMS_PAGE_FRAME_UNAVAILABLE:
	case DOS_EMS_PAGE_FRAME_CONFLICT:
	case DOS_EMS_PAGE_FRAME_FAULT:
		return DOS_EMS_MEMORY_FAULT;
	case DOS_EMS_PAGE_FRAME_UNCERTAIN:
	default:
		manager->poisoned = 1u;
		return DOS_EMS_POISONED;
	}
}

static enum dos_ems_status unmap_frame_slot(
	struct dos_ems_manager *manager, uint8_t physical_page)
{
	struct dos_ems_page_frame_slot *slot =
		&manager->frame_slots[physical_page];
	enum dos_ems_page_frame_status frame_status;

	if (slot->mapped == 0u)
		return DOS_EMS_READY;
	frame_status = manager->page_frame.ops->unmap(
		manager->page_frame.context, manager->page_frame.lease,
		physical_page);
	if (frame_status != DOS_EMS_PAGE_FRAME_OK)
		return frame_operation_status(manager, frame_status);
	*slot = (struct dos_ems_page_frame_slot){0};
	return DOS_EMS_READY;
}

static enum dos_ems_status map_handle_page(
	struct dos_ems_manager *manager, struct dos_cpu_state *state)
{
	struct dos_ems_handle *handle;
	struct dos_ems_page_frame_slot prepared_slot;
	uint16_t logical_page = dos_register_low16(state->ebx);
	uint16_t visible_handle = dos_register_low16(state->edx);
	uint8_t physical_page = dos_register_low8(state->eax);
	uint64_t native_page_offset;
	uint64_t source_physical_address;
	enum dos_ems_page_frame_status frame_status;
	enum dos_ems_status status;

	if (physical_page >= DOS_EMS_PAGE_FRAME_SLOTS) {
		dos_ems_return_status(state,
				      DOS_EMS_STATUS_PHYSICAL_PAGE_INVALID);
		return DOS_EMS_READY;
	}
	if (logical_page == EMS_UNMAP_LOGICAL_PAGE) {
		status = unmap_frame_slot(manager, physical_page);
		if (status == DOS_EMS_READY)
			dos_ems_return_status(state, DOS_EMS_STATUS_OK);
		return status;
	}
	handle = resolve_handle(manager, visible_handle);
	if (handle == NULL) {
		dos_ems_return_status(state, DOS_EMS_STATUS_INVALID_HANDLE);
		return DOS_EMS_READY;
	}
	if ((uint64_t)logical_page >= handle->page_count) {
		dos_ems_return_status(state,
				      DOS_EMS_STATUS_LOGICAL_PAGE_INVALID);
		return DOS_EMS_READY;
	}
	native_page_offset = (uint64_t)logical_page <<
			     (DOS_EMS_PAGE_SHIFT -
			      DOS_EMS_NATIVE_PAGE_SHIFT);
	if (handle->capacity_pages < DOS_EMS_NATIVE_PAGES_PER_PAGE ||
	    native_page_offset >
		    handle->capacity_pages - DOS_EMS_NATIVE_PAGES_PER_PAGE) {
		manager->poisoned = 1u;
		return DOS_EMS_POISONED;
	}
	source_physical_address = handle->physical_address +
				  (native_page_offset <<
				   DOS_EMS_NATIVE_PAGE_SHIFT);
	frame_status = manager->page_frame.ops->map(
		manager->page_frame.context, manager->page_frame.lease,
		physical_page, source_physical_address);
	if (frame_status != DOS_EMS_PAGE_FRAME_OK)
		return frame_operation_status(manager, frame_status);
	prepared_slot = (struct dos_ems_page_frame_slot){
		.physical_address = source_physical_address,
		.handle = visible_handle,
		.logical_page = logical_page,
		.mapped = 1u,
	};
	manager->frame_slots[physical_page] = prepared_slot;
	dos_ems_return_status(state, DOS_EMS_STATUS_OK);
	return DOS_EMS_READY;
}

static bool restore_frame_slots(struct dos_ems_manager *manager,
				uint32_t slot_mask)
{
	uint32_t remaining;

	for (remaining = DOS_EMS_PAGE_FRAME_SLOTS; remaining != 0u;
	     --remaining) {
		const struct dos_ems_page_frame_slot *slot;
		enum dos_ems_page_frame_status frame_status;
		uint32_t index = remaining - 1u;

		if ((slot_mask & (1u << index)) == 0u)
			continue;
		slot = &manager->frame_slots[index];
		frame_status = manager->page_frame.ops->map(
			manager->page_frame.context, manager->page_frame.lease,
			(uint8_t)index, slot->physical_address);
		if (frame_status != DOS_EMS_PAGE_FRAME_OK)
			return false;
	}
	return true;
}

static enum dos_ems_status release_handle(
	struct dos_ems_manager *manager, struct dos_cpu_state *state)
{
	struct dos_ems_handle *handle;
	uint16_t visible_handle = dos_register_low16(state->edx);
	uint64_t generation;
	uint32_t index;
	uint32_t unmapped_slots = 0u;
	enum dos_ems_page_frame_status frame_status;
	enum dos_ems_page_status page_status;

	handle = resolve_handle(manager, visible_handle);
	if (handle == NULL) {
		dos_ems_return_status(state, DOS_EMS_STATUS_INVALID_HANDLE);
		return DOS_EMS_READY;
	}
	for (index = 0u; index < DOS_EMS_PAGE_FRAME_SLOTS; ++index) {
		if (manager->frame_slots[index].mapped == 0u ||
		    manager->frame_slots[index].handle != visible_handle)
			continue;
		frame_status = manager->page_frame.ops->unmap(
			manager->page_frame.context, manager->page_frame.lease,
			(uint8_t)index);
		if (frame_status != DOS_EMS_PAGE_FRAME_OK) {
			if (frame_status != DOS_EMS_PAGE_FRAME_UNAVAILABLE &&
			    frame_status != DOS_EMS_PAGE_FRAME_CONFLICT &&
			    frame_status != DOS_EMS_PAGE_FRAME_FAULT) {
				manager->poisoned = 1u;
				return DOS_EMS_POISONED;
			}
			if (!restore_frame_slots(manager, unmapped_slots)) {
				manager->poisoned = 1u;
				return DOS_EMS_POISONED;
			}
			return DOS_EMS_MEMORY_FAULT;
		}
		unmapped_slots |= 1u << index;
	}

	page_status = manager->page_ops->release(manager->page_context,
						 handle->block);
	if (page_status != DOS_EMS_PAGE_OK) {
		if (page_status != DOS_EMS_PAGE_FAULT ||
		    !restore_frame_slots(manager, unmapped_slots)) {
			manager->poisoned = 1u;
			return DOS_EMS_POISONED;
		}
		return DOS_EMS_MEMORY_FAULT;
	}
	for (index = 0u; index < DOS_EMS_PAGE_FRAME_SLOTS; ++index) {
		if ((unmapped_slots & (1u << index)) != 0u)
			manager->frame_slots[index] =
				(struct dos_ems_page_frame_slot){0};
	}
	index = (uint32_t)visible_handle - 1u;
	generation = handle->generation;
	*handle = (struct dos_ems_handle){
		.generation = generation,
	};
	handle_bit_clear(manager, index);
	dos_ems_return_status(state, DOS_EMS_STATUS_OK);
	return DOS_EMS_READY;
}

static enum dos_ems_status dispatch_standard_ems(
	struct dos_ems_manager *manager, struct dos_cpu_state *state,
	uint8_t function)
{
	switch (function) {
	case EMS_FUNCTION_STATUS:
		dos_ems_return_status(state, DOS_EMS_STATUS_OK);
		return DOS_EMS_READY;
	case EMS_FUNCTION_PAGE_FRAME:
		dos_register_set_low16(&state->ebx,
				       manager->page_frame_segment);
		dos_ems_return_status(state, DOS_EMS_STATUS_OK);
		return DOS_EMS_READY;
	case EMS_FUNCTION_PAGE_COUNTS:
		return query_page_counts(manager, state);
	case EMS_FUNCTION_ALLOCATE:
		return allocate_handle(manager, state);
	case EMS_FUNCTION_MAP:
		return map_handle_page(manager, state);
	case EMS_FUNCTION_RELEASE:
		return release_handle(manager, state);
	case EMS_FUNCTION_VERSION:
		dos_register_set_low8(&state->eax, DOS_EMS_VERSION);
		dos_ems_return_status(state, DOS_EMS_STATUS_OK);
		return DOS_EMS_READY;
	default:
		dos_ems_return_status(state, DOS_EMS_STATUS_INVALID_FUNCTION);
		return DOS_EMS_READY;
	}
}

enum dos_ems_status dos_ems_interrupt(
	struct dos_ems_manager *manager, struct dos_cpu_state *state)
{
	struct dos_cpu_state original;
	enum dos_ems_status status;
	uint8_t function;

	if (manager == NULL || state == NULL || manager->constructed != 1u ||
	    manager->initialized != 1u ||
	    !dos_cpu_mode_value_is_valid(state->mode))
		return DOS_EMS_INVALID_ARGUMENT;
	original = *state;
	if (manager->poisoned != 0u)
		return DOS_EMS_POISONED;
	if (!manager_state_is_valid(manager)) {
		manager->poisoned = 1u;
		return DOS_EMS_POISONED;
	}
	function = dos_register_high8(state->eax);
	status = function == EMS_FUNCTION_VCPI
			 ? dos_vcpi_dispatch(manager, state)
			 : dispatch_standard_ems(manager, state, function);
	if (status != DOS_EMS_READY &&
	    status != DOS_EMS_EXECUTION_TRANSFERRED)
		*state = original;
	if (status == DOS_EMS_READY ||
	    status == DOS_EMS_EXECUTION_TRANSFERRED ||
	    status == DOS_EMS_INVALID_ARGUMENT ||
	    status == DOS_EMS_MEMORY_FAULT || status == DOS_EMS_POISONED)
		return status;
	manager->poisoned = 1u;
	*state = original;
	return DOS_EMS_POISONED;
}

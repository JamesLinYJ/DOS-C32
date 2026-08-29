// SPDX-License-Identifier: GPL-2.0-only
/*
 * VCPI 1.0 register services behind the EMS INT 67h owner.
 *
 * Safety: host CR0 is never exposed directly; low-memory translation and
 *         CPU-mode transfers cross explicit, failure-typed platform gates.
 */
#include "private.h"

#define VCPI_FUNCTION_PRESENCE 0x00u
#define VCPI_FUNCTION_GET_INTERFACE 0x01u
#define VCPI_FUNCTION_GET_MAXIMUM_ADDRESS 0x02u
#define VCPI_FUNCTION_GET_FREE_PAGES 0x03u
#define VCPI_FUNCTION_ALLOCATE_PAGE 0x04u
#define VCPI_FUNCTION_FREE_PAGE 0x05u
#define VCPI_FUNCTION_TRANSLATE_LOW_PAGE 0x06u
#define VCPI_FUNCTION_READ_CR0 0x07u
#define VCPI_FUNCTION_GET_PIC_MAPPINGS 0x0au
#define VCPI_FUNCTION_SET_PIC_MAPPINGS 0x0bu
#define VCPI_FUNCTION_SWITCH_MODE 0x0cu
#define VCPI_SWITCH_STRUCTURE_BYTES 22u

static bool vcpi_bit_is_set(const struct dos_ems_manager *manager,
			    uint32_t index)
{
	return (manager->vcpi_allocated[index >> 5u] &
		(1u << (index & 31u))) != 0u;
}

static void vcpi_bit_set(struct dos_ems_manager *manager, uint32_t index)
{
	manager->vcpi_allocated[index >> 5u] |= 1u << (index & 31u);
}

static void vcpi_bit_clear(struct dos_ems_manager *manager, uint32_t index)
{
	manager->vcpi_allocated[index >> 5u] &= ~(1u << (index & 31u));
}

static uint32_t find_vcpi_slot(const struct dos_ems_manager *manager)
{
	uint32_t index;

	for (index = 0u; index < DOS_VCPI_ALLOCATION_COUNT; ++index) {
		if (!vcpi_bit_is_set(manager, index) &&
		    manager->vcpi_allocations[index].generation <
			    DOS_EMS_GENERATION_MAX)
			return index;
	}
	return DOS_VCPI_ALLOCATION_COUNT;
}

static uint32_t available_vcpi_slots(const struct dos_ems_manager *manager)
{
	uint32_t available = 0u;
	uint32_t index;

	for (index = 0u; index < DOS_VCPI_ALLOCATION_COUNT; ++index) {
		if (!vcpi_bit_is_set(manager, index) &&
		    manager->vcpi_allocations[index].generation <
			    DOS_EMS_GENERATION_MAX)
			++available;
	}
	return available;
}

static uint32_t find_vcpi_physical_page(
	const struct dos_ems_manager *manager, uint32_t physical_address)
{
	uint32_t index;

	for (index = 0u; index < DOS_VCPI_ALLOCATION_COUNT; ++index) {
		if (vcpi_bit_is_set(manager, index) &&
		    manager->vcpi_allocations[index].physical_address ==
			    physical_address)
			return index;
	}
	return DOS_VCPI_ALLOCATION_COUNT;
}

static bool protected_caller_function_is_valid(uint8_t function)
{
	return function == VCPI_FUNCTION_GET_FREE_PAGES ||
	       function == VCPI_FUNCTION_ALLOCATE_PAGE ||
	       function == VCPI_FUNCTION_FREE_PAGE ||
	       function == VCPI_FUNCTION_SWITCH_MODE;
}

static enum dos_ems_status platform_failure(
	struct dos_ems_manager *manager, struct dos_cpu_state *state,
	enum dos_vcpi_platform_status status)
{
	if (status == DOS_VCPI_PLATFORM_UNSUPPORTED) {
		dos_ems_return_status(state, DOS_EMS_STATUS_INVALID_FUNCTION);
		return DOS_EMS_READY;
	}
	if (status == DOS_VCPI_PLATFORM_UNCERTAIN ||
	    (status != DOS_VCPI_PLATFORM_OK &&
	     status != DOS_VCPI_PLATFORM_FAULT)) {
		manager->poisoned = 1u;
		return DOS_EMS_POISONED;
	}
	return DOS_EMS_MEMORY_FAULT;
}

static enum dos_ems_status vcpi_get_maximum_address(
	struct dos_ems_manager *manager, struct dos_cpu_state *state)
{
	struct dos_ems_page_snapshot snapshot;
	uint64_t address;
	enum dos_ems_status status;

	status = dos_ems_query_pages(manager, &snapshot);
	if (status != DOS_EMS_READY)
		return status;
	address = snapshot.highest_address &
		  ~((uint64_t)DOS_EMS_NATIVE_PAGE_BYTES - 1u);
	if (address >
	    DOS_GUEST_32_ADDRESS_LIMIT - DOS_EMS_NATIVE_PAGE_BYTES)
		address = DOS_GUEST_32_ADDRESS_LIMIT -
			  DOS_EMS_NATIVE_PAGE_BYTES;
	state->edx = (uint32_t)address;
	dos_ems_return_status(state, DOS_EMS_STATUS_OK);
	return DOS_EMS_READY;
}

static enum dos_ems_status vcpi_get_free_pages(
	struct dos_ems_manager *manager, struct dos_cpu_state *state)
{
	struct dos_ems_page_snapshot snapshot;
	uint64_t allocatable_pages;
	enum dos_ems_status status;

	status = dos_ems_query_pages(manager, &snapshot);
	if (status != DOS_EMS_READY)
		return status;
	allocatable_pages = snapshot.total_free_pages;
	if (allocatable_pages > available_vcpi_slots(manager))
		allocatable_pages = available_vcpi_slots(manager);
	state->edx = allocatable_pages > 0xffffffffu
			     ? 0xffffffffu
			     : (uint32_t)allocatable_pages;
	dos_ems_return_status(state, DOS_EMS_STATUS_OK);
	return DOS_EMS_READY;
}

static enum dos_ems_status vcpi_allocate_page(
	struct dos_ems_manager *manager, struct dos_cpu_state *state)
{
	dos_ems_page_block_t block = DOS_EMS_PAGE_BLOCK_INVALID;
	uint64_t physical_address = 0u;
	uint64_t capacity_pages = 0u;
	enum dos_ems_page_status page_status;
	uint32_t index = find_vcpi_slot(manager);

	if (index == DOS_VCPI_ALLOCATION_COUNT) {
		dos_ems_return_status(state,
				      DOS_EMS_STATUS_OUT_OF_FREE_PAGES);
		return DOS_EMS_READY;
	}
	page_status = manager->page_ops->allocate(
		manager->page_context, 1u, &block, &physical_address,
		&capacity_pages);
	if (page_status == DOS_EMS_PAGE_NO_MEMORY) {
		dos_ems_return_status(state,
				      DOS_EMS_STATUS_OUT_OF_FREE_PAGES);
		return DOS_EMS_READY;
	}
	if (page_status != DOS_EMS_PAGE_OK)
		return dos_ems_page_fault(manager, page_status);
	if (!dos_ems_allocation_result_is_valid(
			block, physical_address, 1u, capacity_pages))
		return dos_ems_reject_invalid_allocation(manager, block);

	manager->vcpi_allocations[index] = (struct dos_vcpi_allocation){
		.block = block,
		.generation =
			manager->vcpi_allocations[index].generation + 1u,
		.physical_address = physical_address,
		.capacity_pages = capacity_pages,
	};
	vcpi_bit_set(manager, index);
	state->edx = (uint32_t)physical_address;
	dos_ems_return_status(state, DOS_EMS_STATUS_OK);
	return DOS_EMS_READY;
}

static enum dos_ems_status vcpi_free_page(
	struct dos_ems_manager *manager, struct dos_cpu_state *state)
{
	struct dos_vcpi_allocation *allocation;
	enum dos_ems_page_status page_status;
	uint64_t generation;
	uint32_t index;

	if ((state->edx & (DOS_EMS_NATIVE_PAGE_BYTES - 1u)) != 0u) {
		dos_ems_return_status(state,
				      DOS_EMS_STATUS_LOGICAL_PAGE_INVALID);
		return DOS_EMS_READY;
	}
	index = find_vcpi_physical_page(manager, state->edx);
	if (index == DOS_VCPI_ALLOCATION_COUNT) {
		dos_ems_return_status(state,
				      DOS_EMS_STATUS_LOGICAL_PAGE_INVALID);
		return DOS_EMS_READY;
	}
	allocation = &manager->vcpi_allocations[index];
	page_status = manager->page_ops->release(manager->page_context,
						 allocation->block);
	if (page_status != DOS_EMS_PAGE_OK)
		return dos_ems_page_fault(manager, page_status);
	generation = allocation->generation;
	*allocation = (struct dos_vcpi_allocation){
		.generation = generation,
	};
	vcpi_bit_clear(manager, index);
	dos_ems_return_status(state, DOS_EMS_STATUS_OK);
	return DOS_EMS_READY;
}

static enum dos_ems_status vcpi_translate_low_page(
	struct dos_ems_manager *manager, struct dos_cpu_state *state)
{
	uint16_t page = dos_register_low16(state->ecx);
	uint64_t physical_address = 0u;
	enum dos_vcpi_platform_status platform_status;

	if (page >= DOS_VCPI_FIRST_MEGABYTE_PAGES) {
		dos_ems_return_status(state,
				      DOS_EMS_STATUS_PHYSICAL_PAGE_INVALID);
		return DOS_EMS_READY;
	}
	platform_status = manager->vcpi_ops->translate_low_page(
		manager->vcpi_context, page, &physical_address);
	if (platform_status != DOS_VCPI_PLATFORM_OK)
		return platform_failure(manager, state, platform_status);
	if (physical_address >
		    DOS_GUEST_32_ADDRESS_LIMIT - DOS_EMS_NATIVE_PAGE_BYTES ||
	    (physical_address & (DOS_EMS_NATIVE_PAGE_BYTES - 1u)) != 0u) {
		manager->poisoned = 1u;
		return DOS_EMS_POISONED;
	}
	state->edx = (uint32_t)physical_address;
	dos_ems_return_status(state, DOS_EMS_STATUS_OK);
	return DOS_EMS_READY;
}

static enum dos_ems_status vcpi_read_cr0(
	struct dos_ems_manager *manager, struct dos_cpu_state *state)
{
	uint32_t virtual_cr0 = 0u;
	enum dos_vcpi_platform_status platform_status;

	platform_status = manager->vcpi_ops->read_virtual_cr0(
		manager->vcpi_context, &virtual_cr0);
	if (platform_status != DOS_VCPI_PLATFORM_OK)
		return platform_failure(manager, state, platform_status);
	state->ebx = virtual_cr0;
	dos_ems_return_status(state, DOS_EMS_STATUS_OK);
	return DOS_EMS_READY;
}

static enum dos_ems_status vcpi_get_pic_mappings(
	struct dos_ems_manager *manager, struct dos_cpu_state *state)
{
	uint8_t master_base = 0u;
	uint8_t slave_base = 0u;
	enum dos_vcpi_platform_status platform_status;

	platform_status = manager->vcpi_ops->query_pic_mappings(
		manager->vcpi_context, &master_base, &slave_base);
	if (platform_status != DOS_VCPI_PLATFORM_OK)
		return platform_failure(manager, state, platform_status);
	dos_register_set_low16(&state->ebx, master_base);
	dos_register_set_low16(&state->ecx, slave_base);
	dos_ems_return_status(state, DOS_EMS_STATUS_OK);
	return DOS_EMS_READY;
}

static enum dos_ems_status vcpi_set_pic_mappings(
	struct dos_ems_manager *manager, struct dos_cpu_state *state)
{
	uint16_t master_base = dos_register_low16(state->ebx);
	uint16_t slave_base = dos_register_low16(state->ecx);
	enum dos_vcpi_platform_status platform_status;

	if (master_base > 0xffu || slave_base > 0xffu) {
		dos_ems_return_status(state, DOS_EMS_STATUS_INVALID_FUNCTION);
		return DOS_EMS_READY;
	}
	platform_status = manager->vcpi_ops->set_pic_mappings(
		manager->vcpi_context, (uint8_t)master_base,
		(uint8_t)slave_base);
	if (platform_status != DOS_VCPI_PLATFORM_OK)
		return platform_failure(manager, state, platform_status);
	dos_ems_return_status(state, DOS_EMS_STATUS_OK);
	return DOS_EMS_READY;
}

static enum dos_ems_status vcpi_handoff(
	struct dos_ems_manager *manager, struct dos_cpu_state *state,
	uint8_t function)
{
	struct dos_vcpi_handoff_request request = {0};
	struct dos_cpu_state original = *state;
	enum dos_vcpi_handoff_status handoff_status;
	bool interface_request = function == VCPI_FUNCTION_GET_INTERFACE;

	if (interface_request) {
		request.kind = DOS_VCPI_HANDOFF_GET_INTERFACE;
		request.page_table_segment = state->es;
		request.page_table_offset = dos_register_low16(state->edi);
		request.descriptor_segment = state->ds;
		request.descriptor_offset = dos_register_low16(state->esi);
	} else if (state->mode == (uint32_t)DOS_CPU_REAL16 ||
		   state->mode == (uint32_t)DOS_CPU_VM86) {
		if (state->esi >
		    DOS_A20_WRAP_ADDRESS - VCPI_SWITCH_STRUCTURE_BYTES) {
			dos_ems_return_status(
				state, DOS_EMS_STATUS_PHYSICAL_PAGE_INVALID);
			return DOS_EMS_READY;
		}
		request.kind = DOS_VCPI_HANDOFF_ENTER_PROTECTED;
		request.switch_data_linear = state->esi;
	} else {
		request.kind = DOS_VCPI_HANDOFF_RETURN_TO_V86;
	}
	request.caller_mode = state->mode;
	handoff_status = manager->vcpi_ops->handoff(
		manager->vcpi_context, &request, state);
	if (handoff_status == DOS_VCPI_HANDOFF_UNSUPPORTED) {
		*state = original;
		dos_ems_return_status(state, DOS_EMS_STATUS_INVALID_FUNCTION);
		return DOS_EMS_READY;
	}
	if (handoff_status == DOS_VCPI_HANDOFF_UNCERTAIN ||
	    handoff_status > DOS_VCPI_HANDOFF_UNCERTAIN) {
		*state = original;
		manager->poisoned = 1u;
		return DOS_EMS_POISONED;
	}
	if (handoff_status == DOS_VCPI_HANDOFF_FAULT) {
		*state = original;
		return DOS_EMS_MEMORY_FAULT;
	}
	if (interface_request &&
	    handoff_status == DOS_VCPI_HANDOFF_COMPLETED) {
		if (state->mode != original.mode) {
			*state = original;
			manager->poisoned = 1u;
			return DOS_EMS_POISONED;
		}
		dos_ems_return_status(state, DOS_EMS_STATUS_OK);
		return DOS_EMS_READY;
	}
	if (!interface_request &&
	    handoff_status == DOS_VCPI_HANDOFF_TRANSFERRED)
		return DOS_EMS_EXECUTION_TRANSFERRED;

	*state = original;
	manager->poisoned = 1u;
	return DOS_EMS_POISONED;
}

enum dos_ems_status dos_vcpi_dispatch(
	struct dos_ems_manager *manager, struct dos_cpu_state *state)
{
	uint8_t function = dos_register_low8(state->eax);
	bool protected_caller =
		state->mode == (uint32_t)DOS_CPU_PROTECTED16 ||
		state->mode == (uint32_t)DOS_CPU_PROTECTED32;

	if (manager->vcpi_available == 0u) {
		dos_ems_return_status(state, DOS_EMS_STATUS_INVALID_FUNCTION);
		return DOS_EMS_READY;
	}
	if (protected_caller &&
	    !protected_caller_function_is_valid(function)) {
		dos_ems_return_status(state, DOS_EMS_STATUS_INVALID_FUNCTION);
		return DOS_EMS_READY;
	}
	switch (function) {
	case VCPI_FUNCTION_PRESENCE:
		dos_register_set_low16(
			&state->ebx,
			(uint16_t)(DOS_VCPI_VERSION_MAJOR << 8u) |
				DOS_VCPI_VERSION_MINOR);
		dos_ems_return_status(state, DOS_EMS_STATUS_OK);
		return DOS_EMS_READY;
	case VCPI_FUNCTION_GET_INTERFACE:
		return vcpi_handoff(manager, state, function);
	case VCPI_FUNCTION_GET_MAXIMUM_ADDRESS:
		return vcpi_get_maximum_address(manager, state);
	case VCPI_FUNCTION_GET_FREE_PAGES:
		return vcpi_get_free_pages(manager, state);
	case VCPI_FUNCTION_ALLOCATE_PAGE:
		return vcpi_allocate_page(manager, state);
	case VCPI_FUNCTION_FREE_PAGE:
		return vcpi_free_page(manager, state);
	case VCPI_FUNCTION_TRANSLATE_LOW_PAGE:
		return vcpi_translate_low_page(manager, state);
	case VCPI_FUNCTION_READ_CR0:
		return vcpi_read_cr0(manager, state);
	case VCPI_FUNCTION_GET_PIC_MAPPINGS:
		return vcpi_get_pic_mappings(manager, state);
	case VCPI_FUNCTION_SET_PIC_MAPPINGS:
		return vcpi_set_pic_mappings(manager, state);
	case VCPI_FUNCTION_SWITCH_MODE:
		return vcpi_handoff(manager, state, function);
	default:
		dos_ems_return_status(state, DOS_EMS_STATUS_INVALID_FUNCTION);
		return DOS_EMS_READY;
	}
}

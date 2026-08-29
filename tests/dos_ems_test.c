// SPDX-License-Identifier: GPL-2.0-only
/* Host-safe register and lifecycle tests for the EMS/VCPI service core. */
#include "dos_ems.h"
#if !defined(DOSC32_QEMU_SYSTEM_TEST)
#include "test_entry.h"
#endif

#define TEST_PAGE_CONTEXT ((kernel_object_handle_t)0x1001u)
#define TEST_VCPI_CONTEXT ((kernel_object_handle_t)0x1002u)
#define TEST_FRAME_CONTEXT ((kernel_object_handle_t)0x1003u)
#define TEST_FRAME_LEASE ((dos_ems_page_frame_lease_t)0x1004u)
#define TEST_PAGE_COUNT 512u
#define TEST_PAGE_BASE 0x00200000u
#define TEST_BLOCK_COUNT 64u

struct test_block {
	uint64_t generation;
	uint16_t first_page;
	uint16_t page_count;
	uint8_t live;
	uint8_t reserved[3];
};

static uint32_t allocated_pages[TEST_PAGE_COUNT / 32u];
static struct test_block blocks[TEST_BLOCK_COUNT];
static enum dos_ems_page_status forced_release_status;
static enum dos_vcpi_handoff_status forced_handoff_status;
static struct dos_vcpi_handoff_request last_handoff;
static uint32_t handoff_calls;
static uint8_t master_pic_base;
static uint8_t slave_pic_base;
static enum dos_ems_page_frame_status forced_frame_map_status;
static enum dos_ems_page_frame_status forced_frame_unmap_status;
static uint64_t mapped_frame_address[DOS_EMS_PAGE_FRAME_SLOTS];
static uint8_t mapped_frame_slots[DOS_EMS_PAGE_FRAME_SLOTS];
static uint32_t frame_map_calls;
static uint32_t frame_unmap_calls;
static struct dos_ems_manager manager;
static struct dos_ems_manager no_vcpi_manager;
static struct dos_ems_manager failure_manager;

static bool page_is_allocated(uint32_t page)
{
	return (allocated_pages[page >> 5u] &
		(1u << (page & 31u))) != 0u;
}

static void set_page_allocated(uint32_t page, bool allocated)
{
	if (allocated)
		allocated_pages[page >> 5u] |= 1u << (page & 31u);
	else
		allocated_pages[page >> 5u] &= ~(1u << (page & 31u));
}

static dos_ems_page_block_t block_token(uint32_t slot, uint64_t generation)
{
	return (generation << 16u) | (slot + 1u);
}

static struct test_block *resolve_block(dos_ems_page_block_t token)
{
	uint32_t encoded_slot = (uint32_t)(token & 0xffffu);
	uint32_t slot;

	if (encoded_slot == 0u || encoded_slot > TEST_BLOCK_COUNT)
		return NULL;
	slot = encoded_slot - 1u;
	if (blocks[slot].live == 0u ||
	    block_token(slot, blocks[slot].generation) != token)
		return NULL;
	return &blocks[slot];
}

static enum dos_ems_page_status query_pages(
	kernel_object_handle_t context,
	struct dos_ems_page_snapshot *snapshot)
{
	uint64_t largest = 0u;
	uint64_t current = 0u;
	uint64_t total = 0u;
	uint32_t page;

	if (context != TEST_PAGE_CONTEXT || snapshot == NULL)
		return DOS_EMS_PAGE_FAULT;
	for (page = 0u; page < TEST_PAGE_COUNT; ++page) {
		if (page_is_allocated(page)) {
			current = 0u;
			continue;
		}
		++current;
		++total;
		if (current > largest)
			largest = current;
	}
	*snapshot = (struct dos_ems_page_snapshot){
		.largest_free_pages = largest,
		.total_free_pages = total,
		.managed_pages = TEST_PAGE_COUNT,
		.highest_address = TEST_PAGE_BASE +
				   (uint64_t)TEST_PAGE_COUNT *
					   DOS_EMS_NATIVE_PAGE_BYTES -
				   1u,
	};
	return DOS_EMS_PAGE_OK;
}

static enum dos_ems_page_status allocate_pages(
	kernel_object_handle_t context, uint64_t requested_pages,
	dos_ems_page_block_t *block, uint64_t *physical_address,
	uint64_t *capacity_pages)
{
	uint32_t count;
	uint32_t first;
	uint32_t page;
	uint32_t slot;

	if (context != TEST_PAGE_CONTEXT || requested_pages == 0u ||
	    requested_pages > TEST_PAGE_COUNT || block == NULL ||
	    physical_address == NULL || capacity_pages == NULL)
		return DOS_EMS_PAGE_FAULT;
	count = (uint32_t)requested_pages;
	for (slot = 0u; slot < TEST_BLOCK_COUNT; ++slot) {
		if (blocks[slot].live == 0u)
			break;
	}
	if (slot == TEST_BLOCK_COUNT)
		return DOS_EMS_PAGE_NO_MEMORY;
	for (first = 0u; first <= TEST_PAGE_COUNT - count; ++first) {
		for (page = 0u; page < count; ++page) {
			if (page_is_allocated(first + page))
				break;
		}
		if (page == count)
			break;
	}
	if (first > TEST_PAGE_COUNT - count)
		return DOS_EMS_PAGE_NO_MEMORY;
	for (page = 0u; page < count; ++page)
		set_page_allocated(first + page, true);
	++blocks[slot].generation;
	blocks[slot].first_page = (uint16_t)first;
	blocks[slot].page_count = (uint16_t)count;
	blocks[slot].live = 1u;
	*block = block_token(slot, blocks[slot].generation);
	*physical_address = TEST_PAGE_BASE +
			    (uint64_t)first * DOS_EMS_NATIVE_PAGE_BYTES;
	*capacity_pages = count;
	return DOS_EMS_PAGE_OK;
}

static enum dos_ems_page_status release_pages(
	kernel_object_handle_t context, dos_ems_page_block_t token)
{
	struct test_block *block = resolve_block(token);
	uint32_t page;

	if (context != TEST_PAGE_CONTEXT || block == NULL)
		return DOS_EMS_PAGE_INVALID_BLOCK;
	if (forced_release_status != DOS_EMS_PAGE_OK)
		return forced_release_status;
	for (page = 0u; page < block->page_count; ++page)
		set_page_allocated(block->first_page + page, false);
	block->live = 0u;
	block->first_page = 0u;
	block->page_count = 0u;
	return DOS_EMS_PAGE_OK;
}

static enum dos_vcpi_platform_status translate_low_page(
	kernel_object_handle_t context, uint16_t page,
	uint64_t *physical_address)
{
	if (context != TEST_VCPI_CONTEXT ||
	    page >= DOS_VCPI_FIRST_MEGABYTE_PAGES ||
	    physical_address == NULL)
		return DOS_VCPI_PLATFORM_FAULT;
	*physical_address = (uint64_t)page * DOS_EMS_NATIVE_PAGE_BYTES;
	return DOS_VCPI_PLATFORM_OK;
}

static enum dos_vcpi_platform_status read_virtual_cr0(
	kernel_object_handle_t context, uint32_t *virtual_cr0)
{
	if (context != TEST_VCPI_CONTEXT || virtual_cr0 == NULL)
		return DOS_VCPI_PLATFORM_FAULT;
	*virtual_cr0 = 0x80000011u;
	return DOS_VCPI_PLATFORM_OK;
}

static enum dos_vcpi_platform_status query_pic_mappings(
	kernel_object_handle_t context, uint8_t *master_base,
	uint8_t *slave_base)
{
	if (context != TEST_VCPI_CONTEXT || master_base == NULL ||
	    slave_base == NULL)
		return DOS_VCPI_PLATFORM_FAULT;
	*master_base = master_pic_base;
	*slave_base = slave_pic_base;
	return DOS_VCPI_PLATFORM_OK;
}

static enum dos_vcpi_platform_status set_pic_mappings(
	kernel_object_handle_t context, uint8_t master_base,
	uint8_t slave_base)
{
	if (context != TEST_VCPI_CONTEXT)
		return DOS_VCPI_PLATFORM_FAULT;
	master_pic_base = master_base;
	slave_pic_base = slave_base;
	return DOS_VCPI_PLATFORM_OK;
}

static enum dos_ems_page_frame_status frame_acquire(
	kernel_object_handle_t context, uint64_t linear_address,
	uint64_t byte_count, dos_ems_page_frame_lease_t *lease)
{
	if (context != TEST_FRAME_CONTEXT || linear_address != 0xe0000u ||
	    byte_count != 0x10000u || lease == NULL)
		return DOS_EMS_PAGE_FRAME_FAULT;
	*lease = TEST_FRAME_LEASE;
	return DOS_EMS_PAGE_FRAME_OK;
}

static enum dos_ems_page_frame_status frame_release(
	kernel_object_handle_t context, dos_ems_page_frame_lease_t lease)
{
	return context == TEST_FRAME_CONTEXT && lease == TEST_FRAME_LEASE
		       ? DOS_EMS_PAGE_FRAME_OK
		       : DOS_EMS_PAGE_FRAME_FAULT;
}

static enum dos_ems_page_frame_status frame_map(
	kernel_object_handle_t context, dos_ems_page_frame_lease_t lease,
	uint8_t physical_page, uint64_t source_physical_address)
{
	if (context != TEST_FRAME_CONTEXT || lease != TEST_FRAME_LEASE ||
	    physical_page >= DOS_EMS_PAGE_FRAME_SLOTS ||
	    (source_physical_address & (DOS_EMS_NATIVE_PAGE_BYTES - 1u)) != 0u)
		return DOS_EMS_PAGE_FRAME_FAULT;
	++frame_map_calls;
	if (forced_frame_map_status != DOS_EMS_PAGE_FRAME_OK)
		return forced_frame_map_status;
	mapped_frame_slots[physical_page] = 1u;
	mapped_frame_address[physical_page] = source_physical_address;
	return DOS_EMS_PAGE_FRAME_OK;
}

static enum dos_ems_page_frame_status frame_unmap(
	kernel_object_handle_t context, dos_ems_page_frame_lease_t lease,
	uint8_t physical_page)
{
	if (context != TEST_FRAME_CONTEXT || lease != TEST_FRAME_LEASE ||
	    physical_page >= DOS_EMS_PAGE_FRAME_SLOTS)
		return DOS_EMS_PAGE_FRAME_FAULT;
	++frame_unmap_calls;
	if (forced_frame_unmap_status != DOS_EMS_PAGE_FRAME_OK)
		return forced_frame_unmap_status;
	mapped_frame_slots[physical_page] = 0u;
	mapped_frame_address[physical_page] = 0u;
	return DOS_EMS_PAGE_FRAME_OK;
}

static enum dos_vcpi_handoff_status handoff(
	kernel_object_handle_t context,
	const struct dos_vcpi_handoff_request *request,
	struct dos_cpu_state *state)
{
	if (context != TEST_VCPI_CONTEXT || request == NULL || state == NULL)
		return DOS_VCPI_HANDOFF_FAULT;
	++handoff_calls;
	last_handoff = *request;
	if (forced_handoff_status != DOS_VCPI_HANDOFF_COMPLETED)
		return forced_handoff_status;
	if (request->kind == DOS_VCPI_HANDOFF_GET_INTERFACE) {
		state->ebx = 0x12345678u;
		dos_register_set_low16(
			&state->edi,
			(uint16_t)(dos_register_low16(state->edi) + 0x400u));
		return DOS_VCPI_HANDOFF_COMPLETED;
	}
	if (request->kind == DOS_VCPI_HANDOFF_ENTER_PROTECTED) {
		state->mode = (uint32_t)DOS_CPU_PROTECTED32;
		return DOS_VCPI_HANDOFF_TRANSFERRED;
	}
	if (request->kind == DOS_VCPI_HANDOFF_RETURN_TO_V86) {
		state->mode = (uint32_t)DOS_CPU_VM86;
		return DOS_VCPI_HANDOFF_TRANSFERRED;
	}
	return DOS_VCPI_HANDOFF_FAULT;
}

static const struct dos_ems_page_ops page_ops = {
	.query = query_pages,
	.allocate = allocate_pages,
	.release = release_pages,
};

static const struct dos_vcpi_platform_ops vcpi_ops = {
	.translate_low_page = translate_low_page,
	.read_virtual_cr0 = read_virtual_cr0,
	.query_pic_mappings = query_pic_mappings,
	.set_pic_mappings = set_pic_mappings,
	.handoff = handoff,
};

static const struct dos_ems_page_frame_ops frame_ops = {
	.acquire = frame_acquire,
	.release = frame_release,
	.map = frame_map,
	.unmap = frame_unmap,
};

static const struct dos_ems_page_frame_binding frame_binding = {
	.ops = &frame_ops,
	.context = TEST_FRAME_CONTEXT,
	.lease = TEST_FRAME_LEASE,
	.linear_address = 0xe0000u,
	.byte_count = 0x10000u,
};

static const struct dos_ems_config config = {
	.page_frame_segment = 0xe000u,
	.reserved = 0u,
	.reserved2 = 0u,
};

static enum dos_ems_status ems_call(struct dos_ems_manager *owner,
				    struct dos_cpu_state *state,
				    uint8_t function)
{
	state->eax = (state->eax & 0xffff00ffu) |
		     ((uint32_t)function << 8u);
	return dos_ems_interrupt(owner, state);
}

static enum dos_ems_status vcpi_call(struct dos_ems_manager *owner,
				     struct dos_cpu_state *state,
				     uint8_t function)
{
	state->eax = (state->eax & 0xffff0000u) | 0xde00u | function;
	return dos_ems_interrupt(owner, state);
}

static int test_standard_ems(void)
{
	struct dos_cpu_state state = {.mode = DOS_CPU_VM86};
	uint64_t first_generation;
	uint16_t handle;
	uint32_t index;

	if (ems_call(&manager, &state, 0x40u) != DOS_EMS_READY ||
	    dos_register_high8(state.eax) != 0u)
		return 1;
	state = (struct dos_cpu_state){.mode = DOS_CPU_VM86};
	if (ems_call(&manager, &state, 0x41u) != DOS_EMS_READY ||
	    dos_register_high8(state.eax) != 0u ||
	    dos_register_low16(state.ebx) != config.page_frame_segment)
		return 2;
	state = (struct dos_cpu_state){.mode = DOS_CPU_VM86};
	if (ems_call(&manager, &state, 0x42u) != DOS_EMS_READY ||
	    dos_register_low16(state.ebx) != 128u ||
	    dos_register_low16(state.edx) != 128u)
		return 3;
	state = (struct dos_cpu_state){.mode = DOS_CPU_VM86};
	if (ems_call(&manager, &state, 0x43u) != DOS_EMS_READY ||
	    dos_register_high8(state.eax) != 0x89u)
		return 4;
	state = (struct dos_cpu_state){
		.ebx = 4u,
		.mode = DOS_CPU_VM86,
	};
	if (ems_call(&manager, &state, 0x43u) != DOS_EMS_READY ||
	    dos_register_high8(state.eax) != 0u ||
	    dos_register_low16(state.edx) != 1u ||
	    manager.handles[0].page_count != 4u ||
	    manager.handles[0].capacity_pages != 16u)
		return 5;
	first_generation = manager.handles[0].generation;
	state = (struct dos_cpu_state){
		.eax = 3u,
		.ebx = 2u,
		.edx = 1u,
		.mode = DOS_CPU_VM86,
	};
	if (ems_call(&manager, &state, 0x44u) != DOS_EMS_READY ||
	    dos_register_high8(state.eax) != 0u || frame_map_calls != 1u ||
	    mapped_frame_slots[3] != 1u ||
	    mapped_frame_address[3] !=
		    manager.handles[0].physical_address +
			    2u * DOS_EMS_PAGE_BYTES ||
	    manager.frame_slots[3].mapped != 1u ||
	    manager.frame_slots[3].handle != 1u ||
	    manager.frame_slots[3].logical_page != 2u)
		return 16;
	state = (struct dos_cpu_state){
		.eax = 2u,
		.ebx = 4u,
		.edx = 1u,
		.mode = DOS_CPU_VM86,
	};
	if (ems_call(&manager, &state, 0x44u) != DOS_EMS_READY ||
	    dos_register_high8(state.eax) != 0x8au || frame_map_calls != 1u)
		return 17;
	state = (struct dos_cpu_state){
		.eax = 3u,
		.ebx = 0xffffu,
		.mode = DOS_CPU_VM86,
	};
	if (ems_call(&manager, &state, 0x44u) != DOS_EMS_READY ||
	    dos_register_high8(state.eax) != 0u || frame_unmap_calls != 1u ||
	    mapped_frame_slots[3] != 0u ||
	    manager.frame_slots[3].mapped != 0u)
		return 18;
	state = (struct dos_cpu_state){
		.eax = 0u,
		.ebx = 0u,
		.edx = 1u,
		.mode = DOS_CPU_VM86,
	};
	if (ems_call(&manager, &state, 0x44u) != DOS_EMS_READY ||
	    dos_register_high8(state.eax) != 0u || frame_map_calls != 2u ||
	    mapped_frame_slots[0] != 1u)
		return 19;
	state = (struct dos_cpu_state){.mode = DOS_CPU_VM86};
	if (ems_call(&manager, &state, 0x42u) != DOS_EMS_READY ||
	    dos_register_low16(state.ebx) != 124u ||
	    dos_register_low16(state.edx) != 128u)
		return 6;
	state = (struct dos_cpu_state){
		.edx = 33u,
		.mode = DOS_CPU_VM86,
	};
	if (ems_call(&manager, &state, 0x45u) != DOS_EMS_READY ||
	    dos_register_high8(state.eax) != 0x83u)
		return 7;
	state = (struct dos_cpu_state){
		.edx = 1u,
		.mode = DOS_CPU_VM86,
	};
	if (ems_call(&manager, &state, 0x45u) != DOS_EMS_READY ||
	    dos_register_high8(state.eax) != 0u || frame_unmap_calls != 2u ||
	    mapped_frame_slots[0] != 0u ||
	    manager.frame_slots[0].mapped != 0u)
		return 8;
	state = (struct dos_cpu_state){
		.ebx = 1u,
		.mode = DOS_CPU_VM86,
	};
	if (ems_call(&manager, &state, 0x43u) != DOS_EMS_READY ||
	    manager.handles[0].generation != first_generation + 1u)
		return 9;
	state = (struct dos_cpu_state){
		.edx = 1u,
		.mode = DOS_CPU_VM86,
	};
	if (ems_call(&manager, &state, 0x45u) != DOS_EMS_READY)
		return 10;
	for (index = 0u; index < DOS_EMS_HANDLE_COUNT; ++index) {
		state = (struct dos_cpu_state){
			.ebx = 1u,
			.mode = DOS_CPU_VM86,
		};
		if (ems_call(&manager, &state, 0x43u) != DOS_EMS_READY ||
		    dos_register_high8(state.eax) != 0u)
			return 11;
	}
	state = (struct dos_cpu_state){
		.ebx = 1u,
		.mode = DOS_CPU_VM86,
	};
	if (ems_call(&manager, &state, 0x43u) != DOS_EMS_READY ||
	    dos_register_high8(state.eax) != 0x85u)
		return 12;
	for (handle = 1u; handle <= DOS_EMS_HANDLE_COUNT; ++handle) {
		state = (struct dos_cpu_state){
			.edx = handle,
			.mode = DOS_CPU_VM86,
		};
		if (ems_call(&manager, &state, 0x45u) != DOS_EMS_READY)
			return 13;
	}
	state = (struct dos_cpu_state){.mode = DOS_CPU_VM86};
	if (ems_call(&manager, &state, 0x46u) != DOS_EMS_READY ||
	    dos_register_low16(state.eax) != DOS_EMS_VERSION)
		return 14;
	state = (struct dos_cpu_state){.mode = DOS_CPU_VM86};
	if (ems_call(&manager, &state, 0x44u) != DOS_EMS_READY ||
	    dos_register_high8(state.eax) != 0x83u)
		return 15;
	return 0;
}

static int test_vcpi_register_services(void)
{
	struct dos_cpu_state state = {.mode = DOS_CPU_VM86};
	uint32_t physical_page;

	if (vcpi_call(&manager, &state, 0x00u) != DOS_EMS_READY ||
	    dos_register_high8(state.eax) != 0u ||
	    dos_register_low16(state.ebx) != 0x0100u)
		return 1;
	state = (struct dos_cpu_state){.mode = DOS_CPU_VM86};
	if (vcpi_call(&manager, &state, 0x02u) != DOS_EMS_READY ||
	    state.edx != 0x003ff000u)
		return 2;
	state = (struct dos_cpu_state){.mode = DOS_CPU_VM86};
	if (vcpi_call(&manager, &state, 0x03u) != DOS_EMS_READY ||
	    state.edx != DOS_VCPI_ALLOCATION_COUNT)
		return 3;
	state = (struct dos_cpu_state){.mode = DOS_CPU_VM86};
	if (vcpi_call(&manager, &state, 0x04u) != DOS_EMS_READY ||
	    dos_register_high8(state.eax) != 0u ||
	    state.edx != TEST_PAGE_BASE)
		return 4;
	physical_page = state.edx;
	state = (struct dos_cpu_state){
		.edx = physical_page + 1u,
		.mode = DOS_CPU_VM86,
	};
	if (vcpi_call(&manager, &state, 0x05u) != DOS_EMS_READY ||
	    dos_register_high8(state.eax) != 0x8au)
		return 5;
	state = (struct dos_cpu_state){
		.edx = physical_page,
		.mode = DOS_CPU_VM86,
	};
	if (vcpi_call(&manager, &state, 0x05u) != DOS_EMS_READY ||
	    dos_register_high8(state.eax) != 0u)
		return 6;
	state = (struct dos_cpu_state){
		.ecx = 42u,
		.mode = DOS_CPU_VM86,
	};
	if (vcpi_call(&manager, &state, 0x06u) != DOS_EMS_READY ||
	    state.edx != 42u * DOS_EMS_NATIVE_PAGE_BYTES)
		return 7;
	state = (struct dos_cpu_state){
		.ecx = DOS_VCPI_FIRST_MEGABYTE_PAGES,
		.mode = DOS_CPU_VM86,
	};
	if (vcpi_call(&manager, &state, 0x06u) != DOS_EMS_READY ||
	    dos_register_high8(state.eax) != 0x8bu)
		return 8;
	state = (struct dos_cpu_state){.mode = DOS_CPU_VM86};
	if (vcpi_call(&manager, &state, 0x07u) != DOS_EMS_READY ||
	    state.ebx != 0x80000011u)
		return 9;
	state = (struct dos_cpu_state){.mode = DOS_CPU_VM86};
	if (vcpi_call(&manager, &state, 0x0au) != DOS_EMS_READY ||
	    dos_register_low16(state.ebx) != 0x08u ||
	    dos_register_low16(state.ecx) != 0x70u)
		return 10;
	state = (struct dos_cpu_state){
		.ebx = 0x20u,
		.ecx = 0x28u,
		.mode = DOS_CPU_VM86,
	};
	if (vcpi_call(&manager, &state, 0x0bu) != DOS_EMS_READY)
		return 11;
	state = (struct dos_cpu_state){.mode = DOS_CPU_VM86};
	if (vcpi_call(&manager, &state, 0x0au) != DOS_EMS_READY ||
	    dos_register_low16(state.ebx) != 0x20u ||
	    dos_register_low16(state.ecx) != 0x28u)
		return 12;
	state = (struct dos_cpu_state){.mode = DOS_CPU_VM86};
	if (vcpi_call(&manager, &state, 0x08u) != DOS_EMS_READY ||
	    dos_register_high8(state.eax) != 0x84u)
		return 13;
	state = (struct dos_cpu_state){.mode = DOS_CPU_PROTECTED32};
	if (vcpi_call(&manager, &state, 0x02u) != DOS_EMS_READY ||
	    dos_register_high8(state.eax) != 0x84u)
		return 14;
	state = (struct dos_cpu_state){.mode = DOS_CPU_PROTECTED32};
	if (vcpi_call(&manager, &state, 0x03u) != DOS_EMS_READY ||
	    state.edx != DOS_VCPI_ALLOCATION_COUNT)
		return 15;
	return 0;
}

static int test_vcpi_handoffs(void)
{
	struct dos_cpu_state state;

	forced_handoff_status = DOS_VCPI_HANDOFF_COMPLETED;
	handoff_calls = 0u;
	state = (struct dos_cpu_state){
		.esi = 0x0200u,
		.edi = 0x0100u,
		.ds = 0x1111u,
		.es = 0x2222u,
		.mode = DOS_CPU_VM86,
	};
	if (vcpi_call(&manager, &state, 0x01u) != DOS_EMS_READY ||
	    handoff_calls != 1u ||
	    last_handoff.kind != DOS_VCPI_HANDOFF_GET_INTERFACE ||
	    last_handoff.page_table_segment != 0x2222u ||
	    last_handoff.page_table_offset != 0x0100u ||
	    last_handoff.descriptor_segment != 0x1111u ||
	    last_handoff.descriptor_offset != 0x0200u ||
	    state.ebx != 0x12345678u ||
	    dos_register_low16(state.edi) != 0x0500u ||
	    dos_register_high8(state.eax) != 0u)
		return 1;
	state = (struct dos_cpu_state){
		.esi = 0x00080000u,
		.mode = DOS_CPU_VM86,
	};
	if (vcpi_call(&manager, &state, 0x0cu) !=
			DOS_EMS_EXECUTION_TRANSFERRED ||
	    last_handoff.kind != DOS_VCPI_HANDOFF_ENTER_PROTECTED ||
	    last_handoff.switch_data_linear != 0x00080000u ||
	    state.mode != (uint32_t)DOS_CPU_PROTECTED32)
		return 2;
	state = (struct dos_cpu_state){.mode = DOS_CPU_PROTECTED32};
	if (vcpi_call(&manager, &state, 0x0cu) !=
			DOS_EMS_EXECUTION_TRANSFERRED ||
	    last_handoff.kind != DOS_VCPI_HANDOFF_RETURN_TO_V86 ||
	    state.mode != (uint32_t)DOS_CPU_VM86)
		return 3;
	forced_handoff_status = DOS_VCPI_HANDOFF_UNSUPPORTED;
	state = (struct dos_cpu_state){
		.ebx = 0xaabbccddu,
		.mode = DOS_CPU_VM86,
	};
	if (vcpi_call(&manager, &state, 0x01u) != DOS_EMS_READY ||
	    state.ebx != 0xaabbccddu ||
	    dos_register_high8(state.eax) != 0x84u)
		return 4;
	forced_handoff_status = DOS_VCPI_HANDOFF_COMPLETED;
	return 0;
}

static int test_absent_vcpi_and_poison(void)
{
	struct dos_cpu_state state = {.mode = DOS_CPU_VM86};

	if (vcpi_call(&no_vcpi_manager, &state, 0x00u) != DOS_EMS_READY ||
	    dos_register_high8(state.eax) != 0x84u)
		return 1;
	state = (struct dos_cpu_state){
		.ebx = 1u,
		.mode = DOS_CPU_VM86,
	};
	if (ems_call(&failure_manager, &state, 0x43u) != DOS_EMS_READY ||
	    dos_register_low16(state.edx) != 1u)
		return 2;
	forced_release_status = DOS_EMS_PAGE_UNCERTAIN;
	state = (struct dos_cpu_state){
		.edx = 1u,
		.mode = DOS_CPU_VM86,
	};
	if (ems_call(&failure_manager, &state, 0x45u) != DOS_EMS_POISONED ||
	    failure_manager.poisoned == 0u)
		return 3;
	state = (struct dos_cpu_state){.mode = DOS_CPU_VM86};
	if (ems_call(&failure_manager, &state, 0x40u) != DOS_EMS_POISONED)
		return 4;
	forced_release_status = DOS_EMS_PAGE_OK;
	return 0;
}

int dos_ems_test_run(void);

int dos_ems_test_run(void)
{
	int status;
	uint32_t slot;

	forced_release_status = DOS_EMS_PAGE_OK;
	forced_handoff_status = DOS_VCPI_HANDOFF_COMPLETED;
	forced_frame_map_status = DOS_EMS_PAGE_FRAME_OK;
	forced_frame_unmap_status = DOS_EMS_PAGE_FRAME_OK;
	frame_map_calls = 0u;
	frame_unmap_calls = 0u;
	for (slot = 0u; slot < DOS_EMS_PAGE_FRAME_SLOTS; ++slot) {
		mapped_frame_address[slot] = 0u;
		mapped_frame_slots[slot] = 0u;
	}
	master_pic_base = 0x08u;
	slave_pic_base = 0x70u;
	dos_ems_construct(&manager);
	if (dos_ems_initialize(&manager, &page_ops, TEST_PAGE_CONTEXT,
			       &frame_binding, &vcpi_ops, TEST_VCPI_CONTEXT,
			       &config) !=
	    DOS_EMS_READY)
		return 1;
	dos_ems_construct(&no_vcpi_manager);
	if (dos_ems_initialize(&no_vcpi_manager, &page_ops,
			       TEST_PAGE_CONTEXT, &frame_binding, NULL,
			       KERNEL_OBJECT_HANDLE_INVALID, &config) !=
	    DOS_EMS_READY)
		return 2;
	status = test_standard_ems();
	if (status != 0)
		return 10 + status;
	status = test_vcpi_register_services();
	if (status != 0)
		return 30 + status;
	status = test_vcpi_handoffs();
	if (status != 0)
		return 50 + status;
	dos_ems_construct(&failure_manager);
	if (dos_ems_initialize(&failure_manager, &page_ops,
			       TEST_PAGE_CONTEXT, &frame_binding, NULL,
			       KERNEL_OBJECT_HANDLE_INVALID, &config) !=
	    DOS_EMS_READY)
		return 60;
	status = test_absent_vcpi_and_poison();
	if (status != 0)
		return 60 + status;
	return 0;
}

#if !defined(DOSC32_QEMU_SYSTEM_TEST)
DOSC32_TEST_ENTRY(dos_ems_test_run)
#endif

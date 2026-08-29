// SPDX-License-Identifier: GPL-2.0-only
/* Freestanding register-contract tests for the protected-mode XMS manager. */
#include "dos_machine.h"
#include "dos_xms.h"
#include "test_entry.h"

#define TEST_MACHINE_CONTEXT ((kernel_object_handle_t)1u)
#define TEST_XMS_CONTEXT ((kernel_object_handle_t)2u)
#define TEST_MANAGER_IDENTITY ((kernel_object_handle_t)3u)
#define TEST_ADDRESS_SPACE_IDENTITY ((kernel_object_handle_t)4u)
#define TEST_XMS_BYTES (256u * 1024u)
#define TEST_XMS_PHYSICAL_BASE 0x00100000u
#define TEST_XMS_PAGE_BYTES 4096u
#define TEST_XMS_PAGES (TEST_XMS_BYTES / TEST_XMS_PAGE_BYTES)

static uint8_t guest_memory[DOS_REAL_MODE_ADDRESS_LIMIT];
static uint8_t xms_memory[TEST_XMS_BYTES];
static uint64_t xms_allocated_pages;
struct test_xms_block {
	uint16_t first_page;
	uint16_t page_count;
	uint8_t allocated;
	uint8_t reserved[3];
};
static struct test_xms_block xms_blocks[DOS_XMS_HANDLE_COUNT];
static enum dos_machine_status a20_result = DOS_MACHINE_OK;
static enum dos_machine_status a20_query_result = DOS_MACHINE_OK;
static bool a20_apply_requested = true;
static bool backend_a20;
static uint32_t a20_calls;
static uint32_t a20_query_calls;
static enum dos_xms_memory_status hma_result = DOS_XMS_MEMORY_OK;
static struct dos_xms_hma_snapshot hma_snapshot = {
	.address_space_identity = TEST_ADDRESS_SPACE_IDENTITY,
	.address_space_generation = 1u,
	.machine_context = TEST_MACHINE_CONTEXT,
	.base_address = DOS_XMS_HMA_BASE,
	.byte_count = DOS_XMS_HMA_BYTES,
};

static enum dos_machine_status
guest_read(kernel_object_handle_t context, dos_linear_address_t address,
	   void *destination, size_t capacity, size_t count)
{
	uint8_t *output = (uint8_t *)destination;
	size_t index;

	if (context != TEST_MACHINE_CONTEXT || destination == NULL ||
	    count > capacity || address > sizeof(guest_memory) ||
	    count > sizeof(guest_memory) - address)
		return DOS_MACHINE_ADDRESS_FAULT;
	for (index = 0u; index < count; ++index)
		output[index] = guest_memory[address + index];
	return DOS_MACHINE_OK;
}

static enum dos_machine_status
guest_write(kernel_object_handle_t context, dos_linear_address_t address,
	    const void *source, size_t capacity, size_t count)
{
	const uint8_t *input = (const uint8_t *)source;
	size_t index;

	if (context != TEST_MACHINE_CONTEXT || source == NULL ||
	    count > capacity || address > sizeof(guest_memory) ||
	    count > sizeof(guest_memory) - address)
		return DOS_MACHINE_ADDRESS_FAULT;
	for (index = 0u; index < count; ++index)
		guest_memory[address + index] = input[index];
	return DOS_MACHINE_OK;
}

static bool test_page_is_allocated(uint32_t page)
{
	return (xms_allocated_pages & ((uint64_t)1u << page)) != 0u;
}

static struct test_xms_block *test_block(dos_xms_block_t block)
{
	if (block == DOS_XMS_BLOCK_INVALID || block > ARRAY_SIZE(xms_blocks) ||
	    xms_blocks[block - 1u].allocated == 0u)
		return NULL;
	return &xms_blocks[block - 1u];
}

static enum dos_xms_memory_status xms_query(
	kernel_object_handle_t context, uint64_t *largest_bytes,
	uint64_t *total_bytes, uint64_t *highest_address)
{
	uint32_t largest = 0u;
	uint32_t current = 0u;
	uint32_t total = 0u;
	uint32_t page;

	if (context != TEST_XMS_CONTEXT || largest_bytes == NULL ||
	    total_bytes == NULL || highest_address == NULL)
		return DOS_XMS_MEMORY_FAULT;
	for (page = 0u; page < TEST_XMS_PAGES; ++page) {
		if (test_page_is_allocated(page)) {
			current = 0u;
			continue;
		}
		++current;
		++total;
		if (current > largest)
			largest = current;
	}
	*largest_bytes = (uint64_t)largest * TEST_XMS_PAGE_BYTES;
	*total_bytes = (uint64_t)total * TEST_XMS_PAGE_BYTES;
	*highest_address = TEST_XMS_PHYSICAL_BASE + TEST_XMS_BYTES - 1u;
	return DOS_XMS_MEMORY_OK;
}

static enum dos_xms_memory_status xms_allocate(
	kernel_object_handle_t context, uint64_t requested_bytes,
	dos_xms_block_t *block, uint64_t *physical_address,
	uint64_t *capacity_bytes)
{
	uint32_t requested_pages;
	uint32_t first;
	uint32_t page;
	uint32_t slot;

	if (context != TEST_XMS_CONTEXT || requested_bytes == 0u ||
	    requested_bytes > TEST_XMS_BYTES || block == NULL ||
	    physical_address == NULL || capacity_bytes == NULL)
		return DOS_XMS_MEMORY_FAULT;
	requested_pages = (uint32_t)((requested_bytes +
					 TEST_XMS_PAGE_BYTES - 1u) /
					TEST_XMS_PAGE_BYTES);
	for (slot = 0u; slot < ARRAY_SIZE(xms_blocks); ++slot) {
		if (xms_blocks[slot].allocated == 0u)
			break;
	}
	if (slot == ARRAY_SIZE(xms_blocks))
		return DOS_XMS_MEMORY_NO_MEMORY;
	for (first = 0u; first <= TEST_XMS_PAGES - requested_pages; ++first) {
		for (page = 0u; page < requested_pages; ++page) {
			if (test_page_is_allocated(first + page))
				break;
		}
		if (page == requested_pages)
			break;
	}
	if (first > TEST_XMS_PAGES - requested_pages)
		return DOS_XMS_MEMORY_NO_MEMORY;
	for (page = 0u; page < requested_pages; ++page)
		xms_allocated_pages |= (uint64_t)1u << (first + page);
	xms_blocks[slot] = (struct test_xms_block){
		.first_page = (uint16_t)first,
		.page_count = (uint16_t)requested_pages,
		.allocated = 1u,
	};
	*block = slot + 1u;
	*physical_address = TEST_XMS_PHYSICAL_BASE +
				    (uint64_t)first * TEST_XMS_PAGE_BYTES;
	*capacity_bytes = (uint64_t)requested_pages * TEST_XMS_PAGE_BYTES;
	return DOS_XMS_MEMORY_OK;
}

static enum dos_xms_memory_status xms_release(
	kernel_object_handle_t context, dos_xms_block_t block)
{
	struct test_xms_block *allocation = test_block(block);
	uint32_t first_byte;
	uint32_t byte_count;
	uint32_t byte;
	uint32_t page;

	if (context != TEST_XMS_CONTEXT || allocation == NULL)
		return DOS_XMS_MEMORY_FAULT;
	first_byte = (uint32_t)allocation->first_page * TEST_XMS_PAGE_BYTES;
	byte_count = (uint32_t)allocation->page_count * TEST_XMS_PAGE_BYTES;
	for (byte = 0u; byte < byte_count; ++byte)
		xms_memory[first_byte + byte] = 0u;
	for (page = 0u; page < allocation->page_count; ++page)
		xms_allocated_pages &=
			~((uint64_t)1u << (allocation->first_page + page));
	*allocation = (struct test_xms_block){0};
	return DOS_XMS_MEMORY_OK;
}

static enum dos_xms_memory_status
xms_read(kernel_object_handle_t context, dos_xms_block_t block,
	 uint64_t offset, void *destination, size_t capacity, size_t count)
{
	struct test_xms_block *allocation = test_block(block);
	uint8_t *output = (uint8_t *)destination;
	uint64_t allocation_bytes;
	uint64_t base;
	size_t index;

	if (context != TEST_XMS_CONTEXT || destination == NULL ||
	    allocation == NULL || count > capacity)
		return DOS_XMS_MEMORY_FAULT;
	allocation_bytes =
		(uint64_t)allocation->page_count * TEST_XMS_PAGE_BYTES;
	base = (uint64_t)allocation->first_page * TEST_XMS_PAGE_BYTES;
	if (offset > allocation_bytes || count > allocation_bytes - offset)
		return DOS_XMS_MEMORY_FAULT;
	for (index = 0u; index < count; ++index)
		output[index] = xms_memory[base + offset + index];
	return DOS_XMS_MEMORY_OK;
}

static enum dos_xms_memory_status
xms_write(kernel_object_handle_t context, dos_xms_block_t block,
	  uint64_t offset, const void *source, size_t capacity, size_t count)
{
	struct test_xms_block *allocation = test_block(block);
	const uint8_t *input = (const uint8_t *)source;
	uint64_t allocation_bytes;
	uint64_t base;
	size_t index;

	if (context != TEST_XMS_CONTEXT || source == NULL || allocation == NULL ||
	    count > capacity)
		return DOS_XMS_MEMORY_FAULT;
	allocation_bytes =
		(uint64_t)allocation->page_count * TEST_XMS_PAGE_BYTES;
	base = (uint64_t)allocation->first_page * TEST_XMS_PAGE_BYTES;
	if (offset > allocation_bytes || count > allocation_bytes - offset)
		return DOS_XMS_MEMORY_FAULT;
	for (index = 0u; index < count; ++index)
		xms_memory[base + offset + index] = input[index];
	return DOS_XMS_MEMORY_OK;
}

static enum dos_xms_memory_status
xms_query_hma(kernel_object_handle_t context,
	      const struct dos_machine *machine,
	      struct dos_xms_hma_snapshot *snapshot)
{
	if (context != TEST_XMS_CONTEXT || machine == NULL || snapshot == NULL)
		return DOS_XMS_MEMORY_FAULT;
	if (machine->address_limit < DOS_XMS_HMA_LIMIT)
		return DOS_XMS_MEMORY_NO_MEMORY;
	if (hma_result != DOS_XMS_MEMORY_OK)
		return hma_result;
	*snapshot = hma_snapshot;
	return DOS_XMS_MEMORY_OK;
}

static enum dos_machine_status set_a20(kernel_object_handle_t context,
				       bool enabled)
{
	if (context != TEST_MACHINE_CONTEXT)
		return DOS_MACHINE_IO_FAULT;
	++a20_calls;
	if (a20_apply_requested)
		backend_a20 = enabled;
	return a20_result;
}

static enum dos_machine_status query_a20(kernel_object_handle_t context,
					  bool *enabled)
{
	if (context != TEST_MACHINE_CONTEXT || enabled == NULL)
		return DOS_MACHINE_IO_FAULT;
	++a20_query_calls;
	if (a20_query_result != DOS_MACHINE_OK)
		return a20_query_result;
	*enabled = backend_a20;
	return DOS_MACHINE_OK;
}

static enum dos_xms_status xms_call(struct dos_xms_manager *manager,
				    struct dos_machine *machine,
				    struct dos_cpu_state *state,
				    uint8_t function)
{
	state->eax = (state->eax & 0xffff00ffu) | ((uint32_t)function << 8u);
	return dos_xms_control(manager, machine, state);
}

static enum dos_xms_status initialize_manager(
	struct dos_xms_manager *manager, const struct dos_machine *machine,
	const struct dos_xms_memory_ops *memory_ops,
	kernel_object_handle_t identity, uint16_t hma_minimum_bytes)
{
	const struct dos_xms_config config = {
		.hma_minimum_bytes = hma_minimum_bytes,
		.reserved = {0u},
	};

	if (dos_xms_construct(manager, identity) != DOS_XMS_READY)
		return DOS_XMS_INVALID_ARGUMENT;
	return dos_xms_initialize(manager, machine, memory_ops, TEST_XMS_CONTEXT,
				  &config);
}

static int test_hma_contract(struct dos_xms_manager *manager,
			     struct dos_machine *machine)
{
	struct dos_cpu_state state = {0};

	if (xms_call(manager, machine, &state, 0x00u) != DOS_XMS_READY ||
	    dos_register_low16(state.eax) != 0x0300u ||
	    dos_register_low16(state.ebx) != 0x0300u ||
	    dos_register_low16(state.edx) != 1u)
		return 1;
	state = (struct dos_cpu_state){.edx = 4095u};
	if (xms_call(manager, machine, &state, 0x01u) != DOS_XMS_READY ||
	    dos_register_low16(state.eax) != 0u ||
	    dos_register_low8(state.ebx) != 0x92u || manager->hma.active != 0u)
		return 2;
	state = (struct dos_cpu_state){.edx = 4096u};
	if (xms_call(manager, machine, &state, 0x01u) != DOS_XMS_READY ||
	    dos_register_low16(state.eax) != 1u ||
	    dos_register_low8(state.ebx) != 0u || manager->hma.active != 1u ||
	    manager->hma.manager_identity != manager->identity ||
	    manager->hma.manager_generation != manager->generation ||
	    manager->hma.lease_generation != manager->hma_generation ||
	    manager->hma.mapping.base_address != DOS_XMS_HMA_BASE ||
	    manager->hma.mapping.byte_count != DOS_XMS_HMA_BYTES)
		return 3;
	/* HMA ownership is global to this DOS guest: an existing lease wins
	 * before the request-size policy, with no caller identity extension. */
	state = (struct dos_cpu_state){.edx = 0u, .ds = 0xbeefu};
	if (xms_call(manager, machine, &state, 0x01u) != DOS_XMS_READY ||
	    dos_register_low16(state.eax) != 0u ||
	    dos_register_low8(state.ebx) != 0x91u)
		return 4;
	state = (struct dos_cpu_state){.ds = 0xcafeu};
	if (xms_call(manager, machine, &state, 0x02u) != DOS_XMS_READY ||
	    dos_register_low16(state.eax) != 1u || manager->hma.active != 0u)
		return 5;
	state = (struct dos_cpu_state){0};
	if (xms_call(manager, machine, &state, 0x02u) != DOS_XMS_READY ||
	    dos_register_low16(state.eax) != 0u ||
	    dos_register_low8(state.ebx) != 0x93u)
		return 6;
	/* FFFFh is the conventional maximum-request sentinel.  A single HMA
	 * lease is granted whole rather than carved into a normal XMS block. */
	state = (struct dos_cpu_state){.edx = 0xffffu};
	if (xms_call(manager, machine, &state, 0x01u) != DOS_XMS_READY ||
	    dos_register_low16(state.eax) != 1u ||
	    manager->allocated_bitmap != 0u)
		return 7;
	hma_result = DOS_XMS_MEMORY_NO_MEMORY;
	state = (struct dos_cpu_state){0};
	if (xms_call(manager, machine, &state, 0x02u) != DOS_XMS_READY ||
	    dos_register_low16(state.eax) != 0u ||
	    dos_register_low8(state.ebx) != 0x90u || manager->hma.active != 1u)
		return 8;
	/* A known temporary loss does not silently discard ownership. */
	hma_result = DOS_XMS_MEMORY_OK;
	state = (struct dos_cpu_state){0};
	if (xms_call(manager, machine, &state, 0x02u) != DOS_XMS_READY ||
	    dos_register_low16(state.eax) != 1u || manager->hma.active != 0u)
		return 9;
	hma_result = DOS_XMS_MEMORY_NO_MEMORY;
	state = (struct dos_cpu_state){0};
	if (xms_call(manager, machine, &state, 0x00u) != DOS_XMS_READY ||
	    dos_register_low16(state.edx) != 0u)
		return 10;
	state = (struct dos_cpu_state){.edx = 4096u};
	if (xms_call(manager, machine, &state, 0x01u) != DOS_XMS_READY ||
	    dos_register_low16(state.eax) != 0u ||
	    dos_register_low8(state.ebx) != 0x90u || manager->hma.active != 0u)
		return 11;
	hma_result = DOS_XMS_MEMORY_OK;
	return 0;
}

static int test_hma_failure_boundaries(
	struct dos_machine *machine, const struct dos_xms_memory_ops *memory_ops)
{
	struct dos_xms_manager manager;
	struct dos_cpu_state state;
	uint64_t original_generation = hma_snapshot.address_space_generation;
	uint64_t original_bytes = hma_snapshot.byte_count;

	if (initialize_manager(&manager, machine, memory_ops, 0x31u, 0u) !=
	    DOS_XMS_READY)
		return 1;
	state = (struct dos_cpu_state){.edx = 1u};
	if (xms_call(&manager, machine, &state, 0x01u) != DOS_XMS_READY ||
	    manager.hma.active != 1u)
		return 2;
	++hma_snapshot.address_space_generation;
	state = (struct dos_cpu_state){0};
	if (xms_call(&manager, machine, &state, 0x02u) !=
		DOS_XMS_MACHINE_FAULT ||
	    manager.poisoned != 1u || manager.hma.active != 1u)
		return 3;
	hma_snapshot.address_space_generation = original_generation;

	if (initialize_manager(&manager, machine, memory_ops, 0x32u, 0u) !=
	    DOS_XMS_READY)
		return 4;
	state = (struct dos_cpu_state){.edx = 1u};
	if (xms_call(&manager, machine, &state, 0x01u) != DOS_XMS_READY)
		return 5;
	manager.hma.manager_identity = 0xdeadbeefu;
	state = (struct dos_cpu_state){0};
	if (xms_call(&manager, machine, &state, 0x02u) !=
		DOS_XMS_MACHINE_FAULT ||
	    manager.poisoned != 1u || manager.hma.active != 1u)
		return 6;

	if (initialize_manager(&manager, machine, memory_ops, 0x33u, 0u) !=
	    DOS_XMS_READY)
		return 7;
	hma_snapshot.byte_count = DOS_XMS_HMA_BYTES - 1u;
	state = (struct dos_cpu_state){.edx = 1u};
	if (xms_call(&manager, machine, &state, 0x01u) !=
		DOS_XMS_MACHINE_FAULT ||
	    manager.poisoned != 1u || manager.hma.active != 0u)
		return 8;
	hma_snapshot.byte_count = original_bytes;
	return 0;
}

static int test_a20_contract(struct dos_xms_manager *manager,
			     struct dos_machine *machine)
{
	struct dos_cpu_state state = {0};

	if (xms_call(manager, machine, &state, 0x07u) != DOS_XMS_READY ||
	    dos_register_low16(state.eax) != 0u ||
	    dos_register_low8(state.ebx) != 0u)
		return 1;
	state = (struct dos_cpu_state){0};
	if (xms_call(manager, machine, &state, 0x03u) != DOS_XMS_READY ||
	    dos_register_low16(state.eax) != 1u || !machine->a20_enabled ||
	    !backend_a20 || a20_calls != 1u)
		return 2;
	state = (struct dos_cpu_state){0};
	if (xms_call(manager, machine, &state, 0x04u) != DOS_XMS_READY ||
	    dos_register_low16(state.eax) != 1u || machine->a20_enabled ||
	    backend_a20 || a20_calls != 2u)
		return 3;
	state = (struct dos_cpu_state){0};
	if (xms_call(manager, machine, &state, 0x05u) != DOS_XMS_READY ||
	    dos_register_low16(state.eax) != 1u || !machine->a20_enabled ||
	    manager->local_a20_locks != 1u || a20_calls != 3u)
		return 4;
	state = (struct dos_cpu_state){0};
	if (xms_call(manager, machine, &state, 0x05u) != DOS_XMS_READY ||
	    dos_register_low16(state.eax) != 1u ||
	    manager->local_a20_locks != 2u || a20_calls != 3u)
		return 5;
	state = (struct dos_cpu_state){0};
	if (xms_call(manager, machine, &state, 0x06u) != DOS_XMS_READY ||
	    dos_register_low16(state.eax) != 0u ||
	    dos_register_low8(state.ebx) != 0x94u ||
	    manager->local_a20_locks != 1u || !machine->a20_enabled)
		return 6;
	state = (struct dos_cpu_state){0};
	if (xms_call(manager, machine, &state, 0x04u) != DOS_XMS_READY ||
	    dos_register_low16(state.eax) != 0u ||
	    dos_register_low8(state.ebx) != 0x94u || !machine->a20_enabled)
		return 7;
	state = (struct dos_cpu_state){0};
	if (xms_call(manager, machine, &state, 0x06u) != DOS_XMS_READY ||
	    dos_register_low16(state.eax) != 1u || machine->a20_enabled ||
	    backend_a20 || manager->local_a20_locks != 0u || a20_calls != 4u)
		return 8;
	state = (struct dos_cpu_state){0};
	if (xms_call(manager, machine, &state, 0x06u) != DOS_XMS_READY ||
	    dos_register_low16(state.eax) != 0u ||
	    dos_register_low8(state.ebx) != 0x82u)
		return 9;
	manager->local_a20_locks = 0xffffu;
	state = (struct dos_cpu_state){0};
	if (xms_call(manager, machine, &state, 0x05u) != DOS_XMS_READY ||
	    dos_register_low16(state.eax) != 0u ||
	    dos_register_low8(state.ebx) != 0x82u ||
	    manager->local_a20_locks != 0xffffu || a20_calls != 4u)
		return 10;
	manager->local_a20_locks = 0u;
	a20_result = DOS_MACHINE_OK;
	a20_apply_requested = false;
	state = (struct dos_cpu_state){0};
	if (xms_call(manager, machine, &state, 0x05u) != DOS_XMS_READY ||
	    dos_register_low16(state.eax) != 0u ||
	    dos_register_low8(state.ebx) != 0x82u ||
	    machine->poisoned != 0u || manager->poisoned != 0u ||
	    machine->a20_enabled || backend_a20 || a20_calls != 5u ||
	    manager->local_a20_locks != 0u)
		return 11;
	a20_apply_requested = true;
	a20_query_result = DOS_MACHINE_IO_FAULT;
	state = (struct dos_cpu_state){0};
	if (xms_call(manager, machine, &state, 0x07u) !=
		DOS_XMS_MACHINE_FAULT ||
	    machine->poisoned != 1u || manager->poisoned != 1u)
		return 12;
	a20_query_result = DOS_MACHINE_OK;
	return 0;
}

static int test_reallocation_contract(struct dos_xms_manager *manager,
				      struct dos_machine *machine)
{
	uint8_t source[512];
	uint8_t destination[512];
	dos_xms_block_t original_block = manager->handles[0].block;
	struct dos_cpu_state state;
	size_t index;

	for (index = 0u; index < sizeof(source); ++index)
		source[index] = (uint8_t)(index ^ 0x5au);
	if (xms_write(TEST_XMS_CONTEXT, original_block, 1024u, source,
		      sizeof(source), sizeof(source)) != DOS_XMS_MEMORY_OK)
		return 1;
	state = (struct dos_cpu_state){.ebx = 80u, .edx = 1u};
	if (xms_call(manager, machine, &state, 0x0fu) != DOS_XMS_READY ||
	    dos_register_low16(state.eax) != 1u ||
	    manager->handles[0].block == original_block ||
	    manager->handles[0].size_bytes != 80u * 1024u)
		return 2;
	if (xms_read(TEST_XMS_CONTEXT, manager->handles[0].block, 1024u,
		     destination, sizeof(destination), sizeof(destination)) !=
	    DOS_XMS_MEMORY_OK)
		return 3;
	for (index = 0u; index < sizeof(source); ++index) {
		if (destination[index] != source[index])
			return 4;
	}
	state = (struct dos_cpu_state){0};
	if (xms_call(manager, machine, &state, 0x08u) != DOS_XMS_READY ||
	    dos_register_low16(state.eax) != 112u ||
	    dos_register_low16(state.edx) != 176u)
		return 5;
	state = (struct dos_cpu_state){.edx = 1u};
	if (xms_call(manager, machine, &state, 0x0au) != DOS_XMS_READY ||
	    dos_register_low16(state.eax) != 1u)
		return 6;
	state = (struct dos_cpu_state){0};
	if (xms_call(manager, machine, &state, 0x08u) != DOS_XMS_READY ||
	    dos_register_low16(state.eax) != 256u ||
	    dos_register_low16(state.edx) != 256u)
		return 7;
	return 0;
}

static int test_xms_30_extended_contract(struct dos_xms_manager *manager,
					 struct dos_machine *machine)
{
	struct dos_cpu_state state = {0};
	uint16_t handle;

	if (xms_call(manager, machine, &state, 0x88u) != DOS_XMS_READY ||
	    state.eax != 192u || state.edx != 192u ||
	    state.ecx != TEST_XMS_PHYSICAL_BASE + TEST_XMS_BYTES - 1u ||
	    dos_register_low8(state.ebx) != 0u)
		return 1;
	state = (struct dos_cpu_state){.edx = 32u};
	if (xms_call(manager, machine, &state, 0x89u) != DOS_XMS_READY ||
	    dos_register_low16(state.eax) != 1u ||
	    dos_register_low16(state.edx) != 2u)
		return 2;
	handle = dos_register_low16(state.edx);
	state = (struct dos_cpu_state){.edx = handle};
	if (xms_call(manager, machine, &state, 0x8eu) != DOS_XMS_READY ||
	    dos_register_low16(state.eax) != 1u ||
	    dos_register_high8(state.ebx) != 0u ||
	    state.ecx != DOS_XMS_HANDLE_COUNT - 2u || state.edx != 32u)
		return 3;
	state = (struct dos_cpu_state){.ebx = 48u, .edx = handle};
	if (xms_call(manager, machine, &state, 0x8fu) != DOS_XMS_READY ||
	    dos_register_low16(state.eax) != 1u ||
	    manager->handles[handle - 1u].size_bytes != 48u * 1024u)
		return 4;
	state = (struct dos_cpu_state){.edx = handle};
	if (xms_call(manager, machine, &state, 0x0au) != DOS_XMS_READY ||
	    dos_register_low16(state.eax) != 1u)
		return 5;
	return 0;
}

static int run_tests(void)
{
	static const struct dos_machine_ops machine_ops = {
		.read_memory = guest_read,
		.write_memory = guest_write,
		.set_a20 = set_a20,
		.query_a20 = query_a20,
	};
	static const struct dos_xms_memory_ops memory_ops = {
		.query = xms_query,
		.allocate = xms_allocate,
		.release = xms_release,
		.read = xms_read,
		.write = xms_write,
		.query_hma = xms_query_hma,
	};
	struct dos_xms_manager manager;
	struct dos_machine machine;
	struct dos_cpu_state state = {0};
	int a20_status;
	int extended_status;
	int hma_status;
	int reallocation_status;

	if (dos_machine_configure(&machine, &machine_ops,
				  TEST_MACHINE_CONTEXT,
				  DOS_REAL_MODE_ADDRESS_LIMIT, false) !=
	    DOS_MACHINE_OK)
		return 1;
	if (initialize_manager(&manager, &machine, &memory_ops,
			       TEST_MANAGER_IDENTITY, 4096u) != DOS_XMS_READY)
		return 2;
	if (guest_memory[0xf100u] != 0xcdu ||
	    guest_memory[0xf101u] != DOS_XMS_CONTROL_VECTOR ||
	    guest_memory[0xf102u] != 0xcbu)
		return 3;

	state.eax = 0x4300u;
	if (dos_xms_multiplex(&manager, &state) != DOS_XMS_READY ||
	    dos_register_low8(state.eax) != 0x80u)
		return 4;
	state = (struct dos_cpu_state){.eax = 0x4310u};
	if (dos_xms_multiplex(&manager, &state) != DOS_XMS_READY ||
	    state.es != DOS_XMS_CONTROL_SEGMENT ||
	    dos_register_low16(state.ebx) != DOS_XMS_CONTROL_OFFSET)
		return 5;

	hma_status = test_hma_contract(&manager, &machine);
	if (hma_status != 0)
		return 50 + hma_status;
	hma_status = test_hma_failure_boundaries(&machine, &memory_ops);
	if (hma_status != 0)
		return 70 + hma_status;

	state = (struct dos_cpu_state){.edx = 64u};
	if (xms_call(&manager, &machine, &state, 0x09u) != DOS_XMS_READY ||
	    dos_register_low16(state.eax) != 1u ||
	    dos_register_low16(state.edx) != 1u ||
	    dos_register_low8(state.ebx) != 0u)
		return 6;

	state = (struct dos_cpu_state){.ebx = 0xffffffffu, .edx = 1u};
	if (xms_call(&manager, &machine, &state, 0x0eu) != DOS_XMS_READY ||
	    dos_register_low16(state.eax) != 1u ||
	    dos_register_high8(state.ebx) != 0u ||
	    dos_register_low8(state.ebx) != DOS_XMS_HANDLE_COUNT - 1u ||
	    dos_register_low16(state.edx) != 64u)
		return 7;

	state = (struct dos_cpu_state){.edx = 1u};
	if (xms_call(&manager, &machine, &state, 0x0cu) != DOS_XMS_READY ||
	    dos_register_low16(state.eax) != 1u ||
	    dos_register_low16(state.edx) !=
		(uint16_t)(TEST_XMS_PHYSICAL_BASE >> 16u) ||
	    dos_register_low16(state.ebx) !=
		(uint16_t)TEST_XMS_PHYSICAL_BASE)
		return 8;

	state = (struct dos_cpu_state){.edx = 1u};
	if (xms_call(&manager, &machine, &state, 0x0eu) != DOS_XMS_READY ||
	    dos_register_high8(state.ebx) != 1u)
		return 9;
	state = (struct dos_cpu_state){.edx = 1u};
	if (xms_call(&manager, &machine, &state, 0x0du) != DOS_XMS_READY ||
	    dos_register_low16(state.eax) != 1u)
		return 10;

	extended_status = test_xms_30_extended_contract(&manager, &machine);
	if (extended_status != 0)
		return 10 + extended_status;

	reallocation_status = test_reallocation_contract(&manager, &machine);
	if (reallocation_status != 0)
		return 20 + reallocation_status;

	a20_status = test_a20_contract(&manager, &machine);
	if (a20_status != 0)
		return 30 + a20_status;

	return 0;
}

DOSC32_TEST_ENTRY(run_tests)

// SPDX-License-Identifier: GPL-2.0-only
/* Freestanding tests for generation-checked EXEC memory leases. */
#include "dos_memory_lease.h"
#include "test_entry.h"

#define TEST_MEMORY_BYTES 0x00110000u
#define TEST_CONTEXT 0x4c45415345ull
#define TEST_ARENA_IDENTITY ((kernel_object_handle_t)0x4c45415345415245ull)
#define TEST_REBOUND_ARENA_IDENTITY                                            \
	((kernel_object_handle_t)0x4c45415345415246ull)
#define TEST_LEASE_TABLE_IDENTITY                                            \
	((dos_memory_lease_table_identity_t)0x4c534131u)
#define TEST_REBOUND_LEASE_TABLE_IDENTITY                                      \
	((dos_memory_lease_table_identity_t)0x4c534132u)
#define TEST_TAMPERED_LEASE_TABLE_IDENTITY                                     \
	((dos_memory_lease_table_identity_t)0x4c534133u)
#define MCB_BYTES 16u
#define MCB_OWNER_OFFSET 1u
#define MCB_SIZE_OFFSET 3u
#define MCB_NAME_OFFSET 8u

static uint8_t guest_memory[TEST_MEMORY_BYTES];
static uint32_t read_calls;
static uint32_t write_calls;
static bool fail_write_enabled;
static dos_linear_address_t fail_write_address;
static uint32_t fail_write_count;
static uint32_t fail_write_partial_bytes;

static uint16_t get_le16(const uint8_t *bytes)
{
	return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static void put_le16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
}

static uint8_t *mcb_bytes(uint16_t block_segment)
{
	uint16_t header_segment = (uint16_t)(block_segment - 1u);

	return guest_memory + ((uint32_t)header_segment << 4);
}

static bool name_matches(uint16_t block_segment,
			 const uint8_t name[DOS_MEMORY_OWNER_NAME_BYTES])
{
	const uint8_t *mcb = mcb_bytes(block_segment);
	size_t index;

	for (index = 0u; index < DOS_MEMORY_OWNER_NAME_BYTES; ++index) {
		if (mcb[MCB_NAME_OFFSET + index] != name[index])
			return false;
	}
	return true;
}

static void set_name(uint16_t block_segment,
		     const uint8_t name[DOS_MEMORY_OWNER_NAME_BYTES])
{
	uint8_t *mcb = mcb_bytes(block_segment);
	size_t index;

	for (index = 0u; index < DOS_MEMORY_OWNER_NAME_BYTES; ++index)
		mcb[MCB_NAME_OFFSET + index] = name[index];
}

static bool receipt_matches(const struct dos_memory_lease_receipt *left,
			    const struct dos_memory_lease_receipt *right)
{
	return left->handle.value == right->handle.value &&
	       left->guest_segment == right->guest_segment &&
	       left->paragraphs == right->paragraphs &&
	       left->maximum_available == right->maximum_available &&
	       left->reserved == right->reserved;
}

static void copy_lease_view(struct dos_memory_lease_view *destination,
			    const struct dos_memory_lease_view *source)
{
	destination->handle = source->handle;
	destination->machine_context = source->machine_context;
	destination->arena_identity = source->arena_identity;
	destination->arena_generation = source->arena_generation;
	destination->guest_segment = source->guest_segment;
	destination->paragraphs = source->paragraphs;
	destination->owner = source->owner;
	destination->reserved = source->reserved;
}

static bool lease_view_matches(const struct dos_memory_lease_view *left,
			       const struct dos_memory_lease_view *right)
{
	return left->handle.value == right->handle.value &&
	       left->machine_context == right->machine_context &&
	       left->arena_identity == right->arena_identity &&
	       left->arena_generation == right->arena_generation &&
	       left->guest_segment == right->guest_segment &&
	       left->paragraphs == right->paragraphs &&
	       left->owner == right->owner && left->reserved == right->reserved;
}

static void copy_mcb(uint16_t block_segment, uint8_t copy[MCB_BYTES])
{
	const uint8_t *mcb = mcb_bytes(block_segment);
	size_t index;

	for (index = 0u; index < MCB_BYTES; ++index)
		copy[index] = mcb[index];
}

static bool mcb_matches(uint16_t block_segment, const uint8_t copy[MCB_BYTES])
{
	const uint8_t *mcb = mcb_bytes(block_segment);
	size_t index;

	for (index = 0u; index < MCB_BYTES; ++index) {
		if (mcb[index] != copy[index])
			return false;
	}
	return true;
}

static enum dos_machine_status
test_read_memory(kernel_object_handle_t context, dos_linear_address_t address,
		 void *destination, size_t destination_capacity, size_t count)
{
	uint8_t *output = (uint8_t *)destination;
	size_t index;

	if (context != TEST_CONTEXT || count > destination_capacity ||
	    (uint64_t)address > TEST_MEMORY_BYTES ||
	    (uint64_t)count > TEST_MEMORY_BYTES - (uint64_t)address)
		return DOS_MACHINE_ADDRESS_FAULT;
	++read_calls;
	for (index = 0u; index < count; ++index)
		output[index] = guest_memory[address + index];
	return DOS_MACHINE_OK;
}

static enum dos_machine_status
test_write_memory(kernel_object_handle_t context, dos_linear_address_t address,
		  const void *source, size_t source_capacity, size_t count)
{
	const uint8_t *input = (const uint8_t *)source;
	size_t partial_count;
	size_t index;

	if (context != TEST_CONTEXT || count > source_capacity ||
	    (uint64_t)address > TEST_MEMORY_BYTES ||
	    (uint64_t)count > TEST_MEMORY_BYTES - (uint64_t)address)
		return DOS_MACHINE_ADDRESS_FAULT;
	++write_calls;
	if (fail_write_enabled && address == fail_write_address &&
	    fail_write_count != 0u) {
		--fail_write_count;
		partial_count = fail_write_partial_bytes;
		if (partial_count > count)
			partial_count = count;
		for (index = 0u; index < partial_count; ++index)
			guest_memory[address + index] = input[index];
		return DOS_MACHINE_ADDRESS_FAULT;
	}
	for (index = 0u; index < count; ++index)
		guest_memory[address + index] = input[index];
	return DOS_MACHINE_OK;
}

static void clear_write_failure(void)
{
	fail_write_enabled = false;
	fail_write_address = 0u;
	fail_write_count = 0u;
	fail_write_partial_bytes = 0u;
}

static bool reset_machine(struct dos_machine *machine)
{
	static const struct dos_machine_ops ops = {
	    .read_memory = test_read_memory,
	    .write_memory = test_write_memory,
	    .read_port = NULL,
	    .write_port = NULL,
	    .set_a20 = NULL,
	};
	size_t index;

	for (index = 0u; index < TEST_MEMORY_BYTES; ++index)
		guest_memory[index] = 0u;
	read_calls = 0u;
	write_calls = 0u;
	clear_write_failure();
	return dos_machine_configure(machine, &ops, TEST_CONTEXT,
				     TEST_MEMORY_BYTES, true) == DOS_MACHINE_OK;
}

static bool initialize_runtime(struct dos_machine *machine,
			       struct dos_memory_arena *arena,
			       struct dos_memory_lease_table *table,
			       uint16_t head_segment, uint16_t end_segment)
{
	if (!reset_machine(machine))
		return false;
	if (dos_memory_arena_construct(arena, TEST_ARENA_IDENTITY) !=
	    DOS_MEMORY_OK)
		return false;
	if (dos_memory_arena_initialize_checked(arena, machine, head_segment,
						end_segment) != DOS_MEMORY_OK)
		return false;
	if (dos_memory_lease_table_construct(table,
					     TEST_LEASE_TABLE_IDENTITY) !=
	    DOS_MEMORY_LEASE_OK)
		return false;
	return dos_memory_lease_table_initialize(table) == DOS_MEMORY_LEASE_OK;
}

static int test_owner_name_abort_and_aba(void)
{
	static const struct dos_memory_owner_identity owner = {
	    .psp_segment = 0x1234u,
	    .name = {'P', 'R', 'O', 'G', 'R', 'A', 'M', ' '},
	};
	struct dos_memory_arena arena;
	struct dos_memory_lease_table table;
	struct dos_memory_lease_receipt first;
	struct dos_memory_lease_receipt second;
	struct dos_machine machine;
	struct dos_machine wrong_machine;
	uint32_t writes_before;

	if (!initialize_runtime(&machine, &arena, &table, 0x2000u, 0x2100u))
		return 1;
	if (arena.generation != 1u ||
	    dos_memory_lease_acquire(&table, &arena, &machine, &owner, 10u,
				     &first) != DOS_MEMORY_LEASE_OK ||
	    first.handle.value == 0u || first.guest_segment != 0x2001u ||
	    first.paragraphs != 10u || first.maximum_available != 255u ||
	    first.reserved != 0u ||
	    get_le16(mcb_bytes(first.guest_segment) + MCB_OWNER_OFFSET) !=
		owner.psp_segment ||
	    get_le16(mcb_bytes(first.guest_segment) + MCB_SIZE_OFFSET) != 10u ||
	    !name_matches(first.guest_segment, owner.name))
		return 2;

	writes_before = write_calls;
	if (dos_memory_lease_release(&table, &arena, &machine, first.handle,
				     0x9999u) !=
		DOS_MEMORY_LEASE_OWNER_MISMATCH ||
	    write_calls != writes_before)
		return 3;
	put_le16(mcb_bytes(first.guest_segment) + MCB_OWNER_OFFSET, 0x7777u);
	if (dos_memory_lease_release(&table, &arena, &machine, first.handle,
				     owner.psp_segment) !=
	    DOS_MEMORY_LEASE_OWNER_MISMATCH)
		return 4;
	put_le16(mcb_bytes(first.guest_segment) + MCB_OWNER_OFFSET,
		 owner.psp_segment);
	if (dos_memory_lease_abort(&table, &arena, &machine, first.handle,
				   owner.psp_segment) != DOS_MEMORY_LEASE_OK ||
	    get_le16(mcb_bytes(first.guest_segment) + MCB_OWNER_OFFSET) != 0u)
		return 5;
	writes_before = write_calls;
	if (dos_memory_lease_abort(&table, &arena, &machine, first.handle,
				   owner.psp_segment) != DOS_MEMORY_LEASE_OK ||
	    write_calls != writes_before)
		return 6;

	if (dos_memory_lease_acquire(&table, &arena, &machine, &owner, 8u,
				     &second) != DOS_MEMORY_LEASE_OK ||
	    second.handle.value == first.handle.value ||
	    (second.handle.value & DOS_MEMORY_LEASE_SLOT_MASK) !=
		(first.handle.value & DOS_MEMORY_LEASE_SLOT_MASK) ||
	    dos_memory_lease_abort(&table, &arena, &machine, first.handle,
				   owner.psp_segment) !=
		DOS_MEMORY_LEASE_STALE_HANDLE)
		return 7;
	wrong_machine = machine;
	wrong_machine.context = TEST_CONTEXT + 1u;
	if (dos_memory_lease_release(&table, &arena, &wrong_machine,
				     second.handle, owner.psp_segment) !=
	    DOS_MEMORY_LEASE_CONTEXT_MISMATCH)
		return 8;
	if (dos_memory_lease_release(&table, &arena, &machine, second.handle,
				     owner.psp_segment) !=
		DOS_MEMORY_LEASE_OK ||
	    dos_memory_lease_release(&table, &arena, &machine, second.handle,
				     owner.psp_segment) !=
		DOS_MEMORY_LEASE_INVALID_STATE ||
	    dos_memory_lease_abort(&table, &arena, &machine, second.handle,
				   owner.psp_segment) != DOS_MEMORY_LEASE_OK)
		return 9;
	return 0;
}

static int test_arena_generation_rejects_old_lease(void)
{
	static const struct dos_memory_owner_identity owner = {
	    .psp_segment = 0x4321u,
	    .name = {'R', 'E', 'L', 'O', 'A', 'D', ' ', ' '},
	};
	struct dos_memory_arena arena;
	struct dos_memory_lease_table table;
	struct dos_memory_lease_receipt receipt;
	struct dos_machine machine;
	uint32_t writes_before;

	if (!initialize_runtime(&machine, &arena, &table, 0x2200u, 0x2300u) ||
	    dos_memory_lease_acquire(&table, &arena, &machine, &owner, 12u,
				     &receipt) != DOS_MEMORY_LEASE_OK)
		return 1;
	if (dos_memory_arena_initialize_checked(&arena, &machine, 0x2200u,
						0x2300u) != DOS_MEMORY_OK ||
	    arena.generation != 2u)
		return 2;
	writes_before = write_calls;
	if (dos_memory_lease_abort(&table, &arena, &machine, receipt.handle,
				   owner.psp_segment) !=
		DOS_MEMORY_LEASE_STALE_HANDLE ||
	    write_calls != writes_before)
		return 3;
	return 0;
}

static int test_fixed_slots_and_failed_publication(void)
{
	static const struct dos_memory_owner_identity owner = {
	    .psp_segment = 0x5555u,
	    .name = {'S', 'L', 'O', 'T', 'T', 'E', 'S', 'T'},
	};
	struct dos_memory_arena arena;
	struct dos_memory_lease_table table;
	struct dos_memory_lease_receipt receipts[DOS_MEMORY_LEASE_SLOT_COUNT];
	struct dos_memory_lease_receipt sentinel = {
	    .handle = {.value = 0xfeedfacecafebeefull},
	    .guest_segment = 0xaaaau,
	    .paragraphs = 0xbbbbu,
	    .maximum_available = 0xccccu,
	    .reserved = 0xddddu,
	};
	struct dos_memory_lease_receipt output;
	struct dos_machine machine;
	uint32_t writes_before;
	size_t index;

	if (!initialize_runtime(&machine, &arena, &table, 0x3000u, 0x3100u))
		return 1;
	output = sentinel;
	if (dos_memory_lease_acquire(&table, &arena, &machine, &owner, 0xffffu,
				     &output) !=
		DOS_MEMORY_LEASE_NOT_ENOUGH_MEMORY ||
	    !receipt_matches(&output, &sentinel))
		return 2;
	for (index = 0u; index < DOS_MEMORY_LEASE_SLOT_COUNT; ++index) {
		if (dos_memory_lease_acquire(&table, &arena, &machine, &owner,
					     1u, &receipts[index]) !=
		    DOS_MEMORY_LEASE_OK)
			return 3;
	}
	output = sentinel;
	writes_before = write_calls;
	if (dos_memory_lease_acquire(&table, &arena, &machine, &owner, 1u,
				     &output) != DOS_MEMORY_LEASE_NO_SLOT ||
	    !receipt_matches(&output, &sentinel) ||
	    write_calls != writes_before)
		return 4;
	for (index = DOS_MEMORY_LEASE_SLOT_COUNT; index != 0u; --index) {
		if (dos_memory_lease_abort(
			&table, &arena, &machine, receipts[index - 1u].handle,
			owner.psp_segment) != DOS_MEMORY_LEASE_OK)
			return 5;
	}
	return 0;
}

static int test_table_initializes_once(void)
{
	static const struct dos_memory_owner_identity owner = {
	    .psp_segment = 0x6666u,
	    .name = {'O', 'N', 'C', 'E', ' ', ' ', ' ', ' '},
	};
	struct dos_memory_arena arena;
	struct dos_memory_lease_table table;
	static struct dos_memory_lease_table initializer_table =
	    DOS_MEMORY_LEASE_TABLE_INITIALIZER(
		TEST_TAMPERED_LEASE_TABLE_IDENTITY);
	struct dos_memory_lease_receipt receipt;
	struct dos_machine machine;
	uint32_t writes_before;

	if (dos_memory_lease_table_initialize(&initializer_table) !=
		DOS_MEMORY_LEASE_OK ||
	    initializer_table.lifetime_identity !=
		TEST_TAMPERED_LEASE_TABLE_IDENTITY ||
	    initializer_table.slots[0].lifetime_identity !=
		TEST_TAMPERED_LEASE_TABLE_IDENTITY ||
	    !dos_memory_lease_table_is_drained(&initializer_table))
		return 1;
	if (!initialize_runtime(&machine, &arena, &table, 0x4000u, 0x4100u))
		return 2;
	if (dos_memory_lease_acquire(&table, &arena, &machine, &owner, 2u,
				     &receipt) != DOS_MEMORY_LEASE_OK)
		return 3;
	writes_before = write_calls;
	if (dos_memory_lease_table_initialize(&table) !=
		DOS_MEMORY_LEASE_INVALID_STATE ||
	    write_calls != writes_before || !table.initialized ||
	    table.slots[0].state != DOS_MEMORY_LEASE_SLOT_ACTIVE ||
	    table.slots[0].generation != 1u)
		return 4;
	if (dos_memory_lease_abort(&table, &arena, &machine, receipt.handle,
				   owner.psp_segment) != DOS_MEMORY_LEASE_OK)
		return 5;
	return 0;
}

static int test_fixed_encoding_fails_closed(void)
{
	static const struct dos_memory_owner_identity owner = {
	    .psp_segment = 0x6767u,
	    .name = {'E', 'N', 'C', 'O', 'D', 'I', 'N', 'G'},
	};
	static const struct dos_memory_lease_receipt sentinel = {
	    .handle = {.value = 0xfeedfacecafebeefull},
	    .guest_segment = 0xaaaau,
	    .paragraphs = 0xbbbbu,
	    .maximum_available = 0xccccu,
	    .reserved = 0xddddu,
	};
	struct dos_memory_lease_receipt output;
	struct dos_memory_lease_table table;
	struct dos_memory_arena arena;
	struct dos_machine machine;
	uint32_t writes_before;

	if (!initialize_runtime(&machine, &arena, &table, 0x4100u, 0x4200u))
		return 1;
	output = sentinel;
	writes_before = write_calls;
	table.slots[0].reserved[0] = 1u;
	if (dos_memory_lease_table_is_drained(&table) ||
	    dos_memory_lease_acquire(&table, &arena, &machine, &owner, 1u,
				     &output) !=
		DOS_MEMORY_LEASE_INVALID_ARGUMENT ||
	    !receipt_matches(&output, &sentinel) ||
	    write_calls != writes_before)
		return 2;
	table.slots[0].reserved[0] = 0u;
	table.slots[0].state = 0xffu;
	if (dos_memory_lease_table_is_drained(&table) ||
	    dos_memory_lease_acquire(&table, &arena, &machine, &owner, 1u,
				     &output) !=
		DOS_MEMORY_LEASE_INVALID_ARGUMENT ||
	    !receipt_matches(&output, &sentinel) ||
	    write_calls != writes_before)
		return 3;
	table.slots[0].state = DOS_MEMORY_LEASE_SLOT_VACANT;
	table.reserved[1] = 1u;
	if (dos_memory_lease_table_is_drained(&table) ||
	    dos_memory_lease_acquire(&table, &arena, &machine, &owner, 1u,
				     &output) !=
		DOS_MEMORY_LEASE_INVALID_ARGUMENT ||
	    !receipt_matches(&output, &sentinel) ||
	    write_calls != writes_before)
		return 4;
	table.reserved[1] = 0u;
	table.initialized = 2u;
	if (dos_memory_lease_table_is_drained(&table) ||
	    dos_memory_lease_acquire(&table, &arena, &machine, &owner, 1u,
				     &output) !=
		DOS_MEMORY_LEASE_INVALID_ARGUMENT ||
	    !receipt_matches(&output, &sentinel) ||
	    write_calls != writes_before)
		return 5;
	table.initialized = 1u;
	table.constructed = 2u;
	if (dos_memory_lease_table_is_drained(&table) ||
	    dos_memory_lease_acquire(&table, &arena, &machine, &owner, 1u,
				     &output) !=
		DOS_MEMORY_LEASE_INVALID_ARGUMENT ||
	    !receipt_matches(&output, &sentinel) ||
	    write_calls != writes_before)
		return 6;
	table.constructed = 1u;
	arena.initialized = 2u;
	if (dos_memory_lease_acquire(&table, &arena, &machine, &owner, 1u,
				     &output) !=
		DOS_MEMORY_LEASE_INVALID_ARGUMENT ||
	    !receipt_matches(&output, &sentinel) ||
	    write_calls != writes_before)
		return 7;
	arena.initialized = 1u;
	arena.machine_poisoned = 2u;
	if (dos_memory_lease_acquire(&table, &arena, &machine, &owner, 1u,
				     &output) !=
		DOS_MEMORY_LEASE_INVALID_ARGUMENT ||
	    !receipt_matches(&output, &sentinel) ||
	    write_calls != writes_before)
		return 8;
	arena.machine_poisoned = 0u;
	arena.constructed = 2u;
	if (dos_memory_lease_acquire(&table, &arena, &machine, &owner, 1u,
				     &output) !=
		DOS_MEMORY_LEASE_INVALID_ARGUMENT ||
	    !receipt_matches(&output, &sentinel) ||
	    write_calls != writes_before)
		return 9;
	arena.constructed = 1u;
	arena.reserved[0] = 1u;
	if (dos_memory_lease_acquire(&table, &arena, &machine, &owner, 1u,
				     &output) !=
		DOS_MEMORY_LEASE_INVALID_ARGUMENT ||
	    !receipt_matches(&output, &sentinel) ||
	    write_calls != writes_before)
		return 10;
	return 0;
}

static int test_table_lifetime_identity_rejects_aba(void)
{
	static const struct dos_memory_owner_identity owner = {
	    .psp_segment = 0x6868u,
	    .name = {'L', 'I', 'F', 'E', 'T', 'I', 'M', 'E'},
	};
	static const struct dos_memory_lease_view sentinel = {
	    .handle = {.value = 0xfeedfacecafebeefull},
	    .machine_context = (kernel_object_handle_t)0x1111222233334444ull,
	    .arena_identity = (kernel_object_handle_t)0x5555666677778888ull,
	    .arena_generation = 0x9999aaaabbbbccccull,
	    .guest_segment = 0xaaaau,
	    .paragraphs = 0xbbbbu,
	    .owner = 0xccccu,
	    .reserved = 0xddddu,
	};
	struct dos_memory_arena arena;
	struct dos_memory_lease_table table;
	struct dos_memory_lease_receipt old_receipt;
	struct dos_memory_lease_receipt new_receipt;
	struct dos_memory_lease_handle tampered_handle;
	struct dos_memory_lease_view output;
	struct dos_machine machine;
	uint8_t mcb_before[MCB_BYTES];
	uint32_t reads_before;
	uint32_t writes_before;

	if (!initialize_runtime(&machine, &arena, &table, 0x4180u, 0x4280u) ||
	    dos_memory_lease_acquire(&table, &arena, &machine, &owner, 3u,
				     &old_receipt) != DOS_MEMORY_LEASE_OK ||
	    dos_memory_lease_abort(&table, &arena, &machine,
				   old_receipt.handle, owner.psp_segment) !=
		DOS_MEMORY_LEASE_OK ||
	    !dos_memory_lease_table_is_drained(&table))
		return 1;

	reads_before = read_calls;
	writes_before = write_calls;
	if (dos_memory_lease_table_construct(&table, 0u) !=
		DOS_MEMORY_LEASE_INVALID_ARGUMENT ||
	    dos_memory_lease_table_construct(
		&table, DOS_MEMORY_LEASE_TABLE_IDENTITY_INVALID) !=
		DOS_MEMORY_LEASE_INVALID_ARGUMENT ||
	    table.lifetime_identity != TEST_LEASE_TABLE_IDENTITY ||
	    read_calls != reads_before || write_calls != writes_before)
		return 2;

	if (dos_memory_lease_table_construct(
		&table, TEST_REBOUND_LEASE_TABLE_IDENTITY) !=
		DOS_MEMORY_LEASE_OK ||
	    dos_memory_lease_table_initialize(&table) != DOS_MEMORY_LEASE_OK ||
	    dos_memory_lease_acquire(&table, &arena, &machine, &owner, 3u,
				     &new_receipt) != DOS_MEMORY_LEASE_OK ||
	    table.slots[0].lifetime_identity !=
		TEST_REBOUND_LEASE_TABLE_IDENTITY ||
	    new_receipt.guest_segment != old_receipt.guest_segment ||
	    (old_receipt.handle.value & DOS_MEMORY_LEASE_HANDLE_LOCAL_MASK) !=
		(new_receipt.handle.value & DOS_MEMORY_LEASE_HANDLE_LOCAL_MASK) ||
	    old_receipt.handle.value == new_receipt.handle.value ||
	    (dos_memory_lease_table_identity_t)(
		old_receipt.handle.value >>
		DOS_MEMORY_LEASE_TABLE_IDENTITY_SHIFT) !=
		TEST_LEASE_TABLE_IDENTITY ||
	    (dos_memory_lease_table_identity_t)(
		new_receipt.handle.value >>
		DOS_MEMORY_LEASE_TABLE_IDENTITY_SHIFT) !=
		TEST_REBOUND_LEASE_TABLE_IDENTITY)
		return 3;

	copy_mcb(new_receipt.guest_segment, mcb_before);
	reads_before = read_calls;
	writes_before = write_calls;
	if (dos_memory_lease_abort(&table, &arena, &machine,
				   old_receipt.handle, owner.psp_segment) !=
		DOS_MEMORY_LEASE_IDENTITY_MISMATCH ||
	    read_calls != reads_before || write_calls != writes_before ||
	    !mcb_matches(new_receipt.guest_segment, mcb_before))
		return 4;

	tampered_handle = new_receipt.handle;
	tampered_handle.value =
	    (tampered_handle.value & DOS_MEMORY_LEASE_HANDLE_LOCAL_MASK) |
	    ((uint64_t)TEST_TAMPERED_LEASE_TABLE_IDENTITY <<
	     DOS_MEMORY_LEASE_TABLE_IDENTITY_SHIFT);
	copy_lease_view(&output, &sentinel);
	if (dos_memory_lease_resolve_active(
		&table, &arena, &machine, tampered_handle, owner.psp_segment,
		&output) != DOS_MEMORY_LEASE_IDENTITY_MISMATCH ||
	    !lease_view_matches(&output, &sentinel) ||
	    read_calls != reads_before || write_calls != writes_before)
		return 5;

	if (dos_memory_lease_table_construct(&table, 0u) !=
		DOS_MEMORY_LEASE_INVALID_ARGUMENT ||
	    dos_memory_lease_table_construct(
		&table, DOS_MEMORY_LEASE_TABLE_IDENTITY_INVALID) !=
		DOS_MEMORY_LEASE_INVALID_ARGUMENT ||
	    table.lifetime_identity != TEST_REBOUND_LEASE_TABLE_IDENTITY ||
	    table.slots[0].state != DOS_MEMORY_LEASE_SLOT_ACTIVE ||
	    table.slots[0].generation != 1u || read_calls != reads_before ||
	    write_calls != writes_before)
		return 6;

	table.lifetime_identity = 0u;
	copy_lease_view(&output, &sentinel);
	if (dos_memory_lease_resolve_active(
		&table, &arena, &machine, new_receipt.handle,
		owner.psp_segment, &output) != DOS_MEMORY_LEASE_INVALID_ARGUMENT ||
	    !lease_view_matches(&output, &sentinel) ||
	    read_calls != reads_before || write_calls != writes_before)
		return 7;
	table.lifetime_identity = DOS_MEMORY_LEASE_TABLE_IDENTITY_INVALID;
	copy_lease_view(&output, &sentinel);
	if (dos_memory_lease_resolve_active(
		&table, &arena, &machine, new_receipt.handle,
		owner.psp_segment, &output) != DOS_MEMORY_LEASE_INVALID_ARGUMENT ||
	    !lease_view_matches(&output, &sentinel) ||
	    read_calls != reads_before || write_calls != writes_before)
		return 8;
	table.lifetime_identity = TEST_TAMPERED_LEASE_TABLE_IDENTITY;
	copy_lease_view(&output, &sentinel);
	if (dos_memory_lease_resolve_active(
		&table, &arena, &machine, new_receipt.handle,
		owner.psp_segment, &output) != DOS_MEMORY_LEASE_INVALID_ARGUMENT ||
	    !lease_view_matches(&output, &sentinel) ||
	    read_calls != reads_before || write_calls != writes_before)
		return 9;
	table.lifetime_identity = TEST_REBOUND_LEASE_TABLE_IDENTITY;
	table.slots[0].lifetime_identity = TEST_TAMPERED_LEASE_TABLE_IDENTITY;
	copy_lease_view(&output, &sentinel);
	if (dos_memory_lease_resolve_active(
		&table, &arena, &machine, new_receipt.handle,
		owner.psp_segment, &output) != DOS_MEMORY_LEASE_INVALID_ARGUMENT ||
	    !lease_view_matches(&output, &sentinel) ||
	    read_calls != reads_before || write_calls != writes_before)
		return 10;
	table.slots[0].lifetime_identity = TEST_REBOUND_LEASE_TABLE_IDENTITY;
	table.reserved[0] = 1u;
	copy_lease_view(&output, &sentinel);
	if (dos_memory_lease_resolve_active(
		&table, &arena, &machine, new_receipt.handle,
		owner.psp_segment, &output) != DOS_MEMORY_LEASE_INVALID_ARGUMENT ||
	    !lease_view_matches(&output, &sentinel) ||
	    read_calls != reads_before || write_calls != writes_before ||
	    !mcb_matches(new_receipt.guest_segment, mcb_before))
		return 11;
	table.reserved[0] = 0u;

	if (dos_memory_lease_abort(&table, &arena, &machine,
				   new_receipt.handle, owner.psp_segment) !=
		DOS_MEMORY_LEASE_OK ||
	    !dos_memory_lease_table_is_drained(&table))
		return 12;
	return 0;
}

static int test_arena_identity_rejects_same_location_rebind(void)
{
	static const struct dos_memory_owner_identity owner = {
	    .psp_segment = 0x7777u,
	    .name = {'R', 'E', 'B', 'I', 'N', 'D', ' ', ' '},
	};
	struct dos_memory_arena arena;
	struct dos_memory_lease_table table;
	struct dos_memory_lease_receipt old_receipt;
	struct dos_memory_lease_receipt new_receipt;
	struct dos_machine machine;
	uint32_t writes_before;

	if (!initialize_runtime(&machine, &arena, &table, 0x4200u, 0x4300u) ||
	    dos_memory_lease_acquire(&table, &arena, &machine, &owner, 3u,
				     &old_receipt) != DOS_MEMORY_LEASE_OK)
		return 1;
	if (dos_memory_arena_construct(&arena, TEST_REBOUND_ARENA_IDENTITY) !=
		DOS_MEMORY_OK ||
	    dos_memory_arena_initialize_checked(&arena, &machine, 0x4200u,
						0x4300u) != DOS_MEMORY_OK ||
	    arena.identity != TEST_REBOUND_ARENA_IDENTITY ||
	    arena.generation != 1u)
		return 2;
	if (dos_memory_lease_acquire(&table, &arena, &machine, &owner, 3u,
				     &new_receipt) != DOS_MEMORY_LEASE_OK ||
	    new_receipt.guest_segment != old_receipt.guest_segment ||
	    new_receipt.handle.value == old_receipt.handle.value)
		return 3;
	writes_before = write_calls;
	if (dos_memory_lease_abort(&table, &arena, &machine, old_receipt.handle,
				   owner.psp_segment) !=
		DOS_MEMORY_LEASE_IDENTITY_MISMATCH ||
	    write_calls != writes_before ||
	    get_le16(mcb_bytes(new_receipt.guest_segment) + MCB_OWNER_OFFSET) !=
		owner.psp_segment)
		return 4;
	if (dos_memory_lease_abort(&table, &arena, &machine, new_receipt.handle,
				   owner.psp_segment) != DOS_MEMORY_LEASE_OK)
		return 5;
	return 0;
}

static int test_generation_exhaustion_never_wraps(void)
{
	static const struct dos_memory_owner_identity owner = {
	    .psp_segment = 0x8888u,
	    .name = {'E', 'X', 'H', 'A', 'U', 'S', 'T', ' '},
	};
	static const struct dos_memory_lease_receipt sentinel = {
	    .handle = {.value = 0xfeedfacecafebeefull},
	    .guest_segment = 0xaaaau,
	    .paragraphs = 0xbbbbu,
	    .maximum_available = 0xccccu,
	    .reserved = 0xddddu,
	};
	struct dos_memory_arena arena;
	struct dos_memory_lease_table table;
	struct dos_memory_lease_receipt receipt;
	struct dos_memory_lease_receipt output;
	struct dos_machine machine;
	uint8_t mcb_before[MCB_BYTES];
	uint32_t writes_before;
	size_t index;

	if (!initialize_runtime(&machine, &arena, &table, 0x4400u, 0x4500u))
		return 1;
	/* The last generation is issued once, then that terminal slot retires.
	 */
	table.slots[0].generation = DOS_MEMORY_LEASE_GENERATION_MAX - 1u;
	table.slots[0].state = DOS_MEMORY_LEASE_SLOT_ABORTED;
	if (dos_memory_lease_acquire(&table, &arena, &machine, &owner, 2u,
				     &receipt) != DOS_MEMORY_LEASE_OK ||
	    (receipt.handle.value & DOS_MEMORY_LEASE_SLOT_MASK) != 0u ||
	    ((receipt.handle.value >> DOS_MEMORY_LEASE_GENERATION_SHIFT) &
	     DOS_MEMORY_LEASE_GENERATION_MAX) !=
		DOS_MEMORY_LEASE_GENERATION_MAX ||
	    dos_memory_lease_abort(&table, &arena, &machine, receipt.handle,
				   owner.psp_segment) != DOS_MEMORY_LEASE_OK ||
	    table.slots[0].generation != DOS_MEMORY_LEASE_GENERATION_MAX ||
	    table.slots[0].state != DOS_MEMORY_LEASE_SLOT_ABORTED)
		return 2;
	if (dos_memory_lease_acquire(&table, &arena, &machine, &owner, 2u,
				     &receipt) != DOS_MEMORY_LEASE_OK ||
	    (receipt.handle.value & DOS_MEMORY_LEASE_SLOT_MASK) != 1u ||
	    dos_memory_lease_abort(&table, &arena, &machine, receipt.handle,
				   owner.psp_segment) != DOS_MEMORY_LEASE_OK)
		return 3;

	for (index = 0u; index < DOS_MEMORY_LEASE_SLOT_COUNT; ++index) {
		table.slots[index].generation = DOS_MEMORY_LEASE_GENERATION_MAX;
		table.slots[index].state = DOS_MEMORY_LEASE_SLOT_ABORTED;
	}
	output = sentinel;
	copy_mcb(0x4401u, mcb_before);
	writes_before = write_calls;
	if (dos_memory_lease_acquire(&table, &arena, &machine, &owner, 2u,
				     &output) !=
		DOS_MEMORY_LEASE_GENERATION_EXHAUSTED ||
	    !receipt_matches(&output, &sentinel) ||
	    write_calls != writes_before || !mcb_matches(0x4401u, mcb_before))
		return 4;
	return 0;
}

static int test_active_view_resolution(void)
{
	static const struct dos_memory_owner_identity owner = {
	    .psp_segment = 0x9999u,
	    .name = {'V', 'I', 'E', 'W', ' ', ' ', ' ', ' '},
	};
	static const struct dos_memory_lease_view sentinel = {
	    .handle = {.value = 0xfeedfacecafebeefull},
	    .machine_context = (kernel_object_handle_t)0x1111222233334444ull,
	    .arena_identity = (kernel_object_handle_t)0x5555666677778888ull,
	    .arena_generation = 0x9999aaaabbbbccccull,
	    .guest_segment = 0xaaaau,
	    .paragraphs = 0xbbbbu,
	    .owner = 0xccccu,
	    .reserved = 0xddddu,
	};
	struct dos_memory_arena arena;
	struct dos_memory_arena changed_arena;
	struct dos_memory_lease_table table;
	struct dos_memory_lease_receipt receipt;
	struct dos_memory_lease_handle stale_handle;
	struct dos_memory_lease_view output;
	struct dos_machine machine;
	struct dos_machine wrong_machine;

	if (!initialize_runtime(&machine, &arena, &table, 0x4600u, 0x4700u) ||
	    dos_memory_lease_acquire(&table, &arena, &machine, &owner, 6u,
				     &receipt) != DOS_MEMORY_LEASE_OK)
		return 1;
	if (dos_memory_lease_resolve_active(&table, &arena, &machine,
					    receipt.handle, owner.psp_segment,
					    &output) != DOS_MEMORY_LEASE_OK ||
	    output.handle.value != receipt.handle.value ||
	    output.machine_context != machine.context ||
	    output.arena_identity != arena.identity ||
	    output.arena_generation != arena.generation ||
	    output.guest_segment != receipt.guest_segment ||
	    output.paragraphs != receipt.paragraphs ||
	    output.owner != owner.psp_segment || output.reserved != 0u)
		return 2;

	stale_handle = receipt.handle;
	stale_handle.value += (uint64_t)1u << DOS_MEMORY_LEASE_SLOT_BITS;
	copy_lease_view(&output, &sentinel);
	if (dos_memory_lease_resolve_active(
		&table, &arena, &machine, stale_handle, owner.psp_segment,
		&output) != DOS_MEMORY_LEASE_STALE_HANDLE ||
	    !lease_view_matches(&output, &sentinel))
		return 3;

	wrong_machine = machine;
	wrong_machine.context = TEST_CONTEXT + 1u;
	copy_lease_view(&output, &sentinel);
	if (dos_memory_lease_resolve_active(&table, &arena, &wrong_machine,
					    receipt.handle, owner.psp_segment,
					    &output) !=
		DOS_MEMORY_LEASE_CONTEXT_MISMATCH ||
	    !lease_view_matches(&output, &sentinel))
		return 4;

	changed_arena = arena;
	changed_arena.identity = TEST_REBOUND_ARENA_IDENTITY;
	copy_lease_view(&output, &sentinel);
	if (dos_memory_lease_resolve_active(&table, &changed_arena, &machine,
					    receipt.handle, owner.psp_segment,
					    &output) !=
		DOS_MEMORY_LEASE_IDENTITY_MISMATCH ||
	    !lease_view_matches(&output, &sentinel))
		return 5;

	changed_arena = arena;
	++changed_arena.generation;
	copy_lease_view(&output, &sentinel);
	if (dos_memory_lease_resolve_active(
		&table, &changed_arena, &machine, receipt.handle,
		owner.psp_segment, &output) != DOS_MEMORY_LEASE_STALE_HANDLE ||
	    !lease_view_matches(&output, &sentinel))
		return 6;

	copy_lease_view(&output, &sentinel);
	if (dos_memory_lease_resolve_active(&table, &arena, &machine,
					    receipt.handle, 0xaaaau, &output) !=
		DOS_MEMORY_LEASE_OWNER_MISMATCH ||
	    !lease_view_matches(&output, &sentinel))
		return 7;

	changed_arena = arena;
	changed_arena.machine_poisoned = true;
	copy_lease_view(&output, &sentinel);
	if (dos_memory_lease_resolve_active(&table, &changed_arena, &machine,
					    receipt.handle, owner.psp_segment,
					    &output) !=
		DOS_MEMORY_LEASE_MACHINE_POISONED ||
	    !lease_view_matches(&output, &sentinel))
		return 8;

	if (dos_memory_lease_abort(&table, &arena, &machine, receipt.handle,
				   owner.psp_segment) != DOS_MEMORY_LEASE_OK)
		return 9;
	copy_lease_view(&output, &sentinel);
	if (dos_memory_lease_resolve_active(
		&table, &arena, &machine, receipt.handle, owner.psp_segment,
		&output) != DOS_MEMORY_LEASE_INVALID_STATE ||
	    !lease_view_matches(&output, &sentinel))
		return 10;
	return 0;
}

static int test_ownership_transfer_and_publish(void)
{
	static const uint8_t environment_name[DOS_MEMORY_OWNER_NAME_BYTES] = {
	    'E', 'N', 'V', 'I', 'R', 'O', 'N', ' '};
	static const struct dos_memory_owner_identity named_child = {
	    .psp_segment = 0x3333u,
	    .name = {'C', 'H', 'I', 'L', 'D', ' ', ' ', ' '},
	};
	static const struct dos_memory_owner_name_patch short_patch = {
	    .bytes = {'X', 0u, 0u, 0u, 0u, 0u, 0u, 0u},
	    .count = 1u,
	    .reserved = {0u},
	};
	static const uint8_t short_name[DOS_MEMORY_OWNER_NAME_BYTES] = {
	    'X', 'N', 'V', 'I', 'R', 'O', 'N', ' '};
	static const struct dos_memory_owner_name_patch invalid_patch = {
	    .bytes = {'Y', 0u, 0u, 0u, 0u, 0u, 0u, 0u},
	    .count = 1u,
	    .reserved = {0u, 0u, 0u, 1u, 0u, 0u, 0u},
	};
	static const struct dos_memory_lease_receipt sentinel = {
	    .handle = {.value = 0xfeedfacecafebeefull},
	    .guest_segment = 0xaaaau,
	    .paragraphs = 0xbbbbu,
	    .maximum_available = 0xccccu,
	    .reserved = 0xddddu,
	};
	struct dos_memory_arena arena;
	struct dos_memory_arena poisoned_arena;
	struct dos_memory_lease_table table;
	struct dos_memory_lease_receipt receipt;
	struct dos_memory_lease_receipt next_receipt;
	struct dos_memory_lease_view view;
	struct dos_machine machine;
	uint8_t published_mcb[MCB_BYTES];
	uint32_t reads_before;
	uint32_t writes_before;

	if (!initialize_runtime(&machine, &arena, &table, 0x4800u, 0x4900u) ||
	    !dos_memory_lease_table_is_drained(&table) ||
	    dos_memory_lease_table_is_drained(NULL))
		return 1;
	table.slots[0].state = DOS_MEMORY_LEASE_SLOT_ACQUIRING;
	if (dos_memory_lease_table_is_drained(&table))
		return 2;
	table.slots[0].state = DOS_MEMORY_LEASE_SLOT_RELEASING;
	if (dos_memory_lease_table_is_drained(&table))
		return 3;
	table.slots[0].state = DOS_MEMORY_LEASE_SLOT_VACANT;

	receipt = sentinel;
	reads_before = read_calls;
	writes_before = write_calls;
	if (dos_memory_lease_acquire_unnamed(&table, &arena, &machine, 0u, 5u,
					     &receipt) !=
		DOS_MEMORY_LEASE_INVALID_ARGUMENT ||
	    !receipt_matches(&receipt, &sentinel) ||
	    read_calls != reads_before || write_calls != writes_before)
		return 4;

	/* An unnamed allocation changes only its owner; an EXEC environment
	 * keeps the name. */
	set_name(0x4801u, environment_name);
	if (dos_memory_lease_acquire_unnamed(&table, &arena, &machine, 0x1111u,
					     5u,
					     &receipt) != DOS_MEMORY_LEASE_OK ||
	    receipt.guest_segment != 0x4801u ||
	    !name_matches(receipt.guest_segment, environment_name) ||
	    dos_memory_lease_table_is_drained(&table))
		return 5;

	copy_mcb(receipt.guest_segment, published_mcb);
	reads_before = read_calls;
	writes_before = write_calls;
	if (dos_memory_lease_transfer_owner(&table, &arena, &machine,
					    receipt.handle, 0x2222u, 0x3333u) !=
		DOS_MEMORY_LEASE_OWNER_MISMATCH ||
	    read_calls != reads_before || write_calls != writes_before ||
	    !mcb_matches(receipt.guest_segment, published_mcb))
		return 6;

	if (dos_memory_lease_transfer_owner(&table, &arena, &machine,
					    receipt.handle, 0x1111u,
					    0x2222u) != DOS_MEMORY_LEASE_OK ||
	    get_le16(mcb_bytes(receipt.guest_segment) + MCB_OWNER_OFFSET) !=
		0x2222u ||
	    !name_matches(receipt.guest_segment, environment_name) ||
	    dos_memory_lease_resolve_active(&table, &arena, &machine,
					    receipt.handle, 0x1111u, &view) !=
		DOS_MEMORY_LEASE_OWNER_MISMATCH ||
	    dos_memory_lease_resolve_active(&table, &arena, &machine,
					    receipt.handle, 0x2222u,
					    &view) != DOS_MEMORY_LEASE_OK ||
	    view.owner != 0x2222u)
		return 7;

	copy_mcb(receipt.guest_segment, published_mcb);
	reads_before = read_calls;
	writes_before = write_calls;
	if (dos_memory_lease_transfer_owner_name_patch(
		&table, &arena, &machine, receipt.handle, 0x2222u, 0x2a2au,
		&invalid_patch) != DOS_MEMORY_LEASE_INVALID_ARGUMENT ||
	    dos_memory_lease_transfer_owner_name_patch(
		&table, &arena, &machine, receipt.handle, 0x2222u, 0x2a2au,
		NULL) != DOS_MEMORY_LEASE_INVALID_ARGUMENT ||
	    read_calls != reads_before || write_calls != writes_before ||
	    !mcb_matches(receipt.guest_segment, published_mcb))
		return 8;

	/* Cached capability ownership never overrides the canonical guest MCB.
	 */
	put_le16(mcb_bytes(receipt.guest_segment) + MCB_OWNER_OFFSET, 0x7777u);
	writes_before = write_calls;
	if (dos_memory_lease_transfer_owner_name_patch(
		&table, &arena, &machine, receipt.handle, 0x2222u, 0x2a2au,
		&short_patch) != DOS_MEMORY_LEASE_OWNER_MISMATCH ||
	    write_calls != writes_before ||
	    get_le16(mcb_bytes(receipt.guest_segment) + MCB_OWNER_OFFSET) !=
		0x7777u ||
	    dos_memory_lease_resolve_active(&table, &arena, &machine,
					    receipt.handle, 0x2222u,
					    &view) != DOS_MEMORY_LEASE_OK ||
	    view.owner != 0x2222u)
		return 9;
	put_le16(mcb_bytes(receipt.guest_segment) + MCB_OWNER_OFFSET, 0x2222u);

	if (dos_memory_lease_transfer_owner_name_patch(
		&table, &arena, &machine, receipt.handle, 0x2222u, 0x2a2au,
		&short_patch) != DOS_MEMORY_LEASE_OK ||
	    get_le16(mcb_bytes(receipt.guest_segment) + MCB_OWNER_OFFSET) !=
		0x2a2au ||
	    !name_matches(receipt.guest_segment, short_name) ||
	    dos_memory_lease_resolve_active(&table, &arena, &machine,
					    receipt.handle, 0x2a2au,
					    &view) != DOS_MEMORY_LEASE_OK ||
	    view.owner != 0x2a2au)
		return 10;

	if (dos_memory_lease_transfer_named_owner(
		&table, &arena, &machine, receipt.handle, 0x2a2au,
		&named_child) != DOS_MEMORY_LEASE_OK ||
	    get_le16(mcb_bytes(receipt.guest_segment) + MCB_OWNER_OFFSET) !=
		named_child.psp_segment ||
	    !name_matches(receipt.guest_segment, named_child.name))
		return 11;

	reads_before = read_calls;
	writes_before = write_calls;
	if (dos_memory_lease_preflight_publish(&table, &arena, &machine,
					       receipt.handle, 0x2222u) !=
		DOS_MEMORY_LEASE_OWNER_MISMATCH ||
	    dos_memory_lease_preflight_publish(
		&table, &arena, &machine, receipt.handle,
		named_child.psp_segment) != DOS_MEMORY_LEASE_OK ||
	    dos_memory_lease_publish(&table, &arena, &machine, receipt.handle,
				     0x2222u) !=
		DOS_MEMORY_LEASE_OWNER_MISMATCH ||
	    read_calls != reads_before || write_calls != writes_before)
		return 12;
	if (dos_memory_lease_publish(&table, &arena, &machine, receipt.handle,
				     named_child.psp_segment) !=
		DOS_MEMORY_LEASE_OK ||
	    dos_memory_lease_publish(&table, &arena, &machine, receipt.handle,
				     named_child.psp_segment) !=
		DOS_MEMORY_LEASE_OK ||
	    read_calls != reads_before || write_calls != writes_before ||
	    !dos_memory_lease_table_is_drained(&table))
		return 13;
	poisoned_arena = arena;
	poisoned_arena.machine_poisoned = true;
	if (dos_memory_lease_publish(&table, &poisoned_arena, &machine,
				     receipt.handle, named_child.psp_segment) !=
		DOS_MEMORY_LEASE_MACHINE_POISONED ||
	    read_calls != reads_before || write_calls != writes_before)
		return 14;

	copy_mcb(receipt.guest_segment, published_mcb);
	if (dos_memory_lease_resolve_active(&table, &arena, &machine,
					    receipt.handle,
					    named_child.psp_segment, &view) !=
		DOS_MEMORY_LEASE_INVALID_STATE ||
	    dos_memory_lease_preflight_publish(
		&table, &arena, &machine, receipt.handle,
		named_child.psp_segment) != DOS_MEMORY_LEASE_INVALID_STATE ||
	    dos_memory_lease_abort(&table, &arena, &machine, receipt.handle,
				   named_child.psp_segment) !=
		DOS_MEMORY_LEASE_INVALID_STATE ||
	    dos_memory_lease_release(&table, &arena, &machine, receipt.handle,
				     named_child.psp_segment) !=
		DOS_MEMORY_LEASE_INVALID_STATE ||
	    read_calls != reads_before || write_calls != writes_before ||
	    !mcb_matches(receipt.guest_segment, published_mcb))
		return 15;

	if (dos_memory_lease_acquire_unnamed(&table, &arena, &machine, 0x4444u,
					     1u, &next_receipt) !=
		DOS_MEMORY_LEASE_OK ||
	    next_receipt.handle.value == receipt.handle.value ||
	    (next_receipt.handle.value & DOS_MEMORY_LEASE_SLOT_MASK) !=
		(receipt.handle.value & DOS_MEMORY_LEASE_SLOT_MASK))
		return 16;
	reads_before = read_calls;
	writes_before = write_calls;
	if (dos_memory_lease_publish(&table, &arena, &machine, receipt.handle,
				     named_child.psp_segment) !=
		DOS_MEMORY_LEASE_STALE_HANDLE ||
	    read_calls != reads_before || write_calls != writes_before ||
	    dos_memory_lease_abort(&table, &arena, &machine,
				   next_receipt.handle,
				   0x4444u) != DOS_MEMORY_LEASE_OK ||
	    !dos_memory_lease_table_is_drained(&table))
		return 17;
	return 0;
}

static int test_transfer_failure_and_poison(void)
{
	static const struct dos_memory_owner_identity owner = {
	    .psp_segment = 0x5555u,
	    .name = {'F', 'A', 'I', 'L', 'U', 'R', 'E', ' '},
	};
	static const struct dos_memory_owner_name_patch name_patch = {
	    .bytes = {'Z', 0u, 0u, 0u, 0u, 0u, 0u, 0u},
	    .count = 1u,
	    .reserved = {0u},
	};
	struct dos_memory_arena arena;
	struct dos_memory_lease_table table;
	struct dos_memory_lease_receipt receipt;
	struct dos_machine machine;
	uint8_t mcb_before[MCB_BYTES];
	uint32_t slot_index;
	uint32_t reads_before;
	uint32_t writes_before;

	if (!initialize_runtime(&machine, &arena, &table, 0x4a00u, 0x4b00u) ||
	    dos_memory_lease_acquire(&table, &arena, &machine, &owner, 4u,
				     &receipt) != DOS_MEMORY_LEASE_OK)
		return 1;
	slot_index =
	    (uint32_t)(receipt.handle.value & DOS_MEMORY_LEASE_SLOT_MASK);
	copy_mcb(receipt.guest_segment, mcb_before);
	clear_write_failure();
	fail_write_enabled = true;
	fail_write_address =
	    (dos_linear_address_t)((uint32_t)(receipt.guest_segment - 1u) << 4);
	fail_write_count = 1u;
	fail_write_partial_bytes = 7u;
	if (dos_memory_lease_transfer_owner_name_patch(
		&table, &arena, &machine, receipt.handle, owner.psp_segment,
		0x6666u, &name_patch) != DOS_MEMORY_LEASE_MACHINE_FAULT ||
	    arena.machine_poisoned ||
	    table.slots[slot_index].owner != owner.psp_segment ||
	    !mcb_matches(receipt.guest_segment, mcb_before))
		return 2;

	clear_write_failure();
	fail_write_enabled = true;
	fail_write_address =
	    (dos_linear_address_t)((uint32_t)(receipt.guest_segment - 1u) << 4);
	fail_write_count = 2u;
	fail_write_partial_bytes = 4u;
	if (dos_memory_lease_transfer_owner_name_patch(
		&table, &arena, &machine, receipt.handle, owner.psp_segment,
		0x6666u, &name_patch) != DOS_MEMORY_LEASE_MACHINE_POISONED ||
	    !arena.machine_poisoned ||
	    table.slots[slot_index].owner != owner.psp_segment ||
	    table.slots[slot_index].state != DOS_MEMORY_LEASE_SLOT_ACTIVE ||
	    dos_memory_lease_table_is_drained(&table))
		return 3;

	clear_write_failure();
	reads_before = read_calls;
	writes_before = write_calls;
	if (dos_memory_lease_publish(&table, &arena, &machine, receipt.handle,
				     owner.psp_segment) !=
		DOS_MEMORY_LEASE_MACHINE_POISONED ||
	    table.slots[slot_index].state != DOS_MEMORY_LEASE_SLOT_ACTIVE ||
	    read_calls != reads_before || write_calls != writes_before)
		return 4;
	return 0;
}

static int test_rebind_plan_and_callback_free_publish(void)
{
	static const struct dos_memory_owner_identity parent = {
	    .psp_segment = 0x5151u,
	    .name = {'P', 'A', 'R', 'E', 'N', 'T', ' ', ' '},
	};
	static const struct dos_memory_owner_name_patch name_patch = {
	    .bytes = {'W', 'I', 'N', 0u, 0u, 0u, 0u, 0u},
	    .count = 4u,
	    .reserved = {0u},
	};
	struct dos_memory_lease_rebind_plan plan;
	struct dos_memory_lease_rebind_plan tampered;
	struct dos_memory_arena arena;
	struct dos_memory_lease_table table;
	struct dos_memory_lease_receipt receipt;
	struct dos_machine machine;
	uint8_t before[MCB_BYTES];
	uint32_t reads_before;
	uint32_t writes_before;
	uint32_t slot_index;
	size_t index;

	if (!initialize_runtime(&machine, &arena, &table, 0x4c00u, 0x4d00u) ||
	    dos_memory_lease_acquire(&table, &arena, &machine, &parent, 4u,
				     &receipt) != DOS_MEMORY_LEASE_OK)
		return 1;
	slot_index =
	    (uint32_t)(receipt.handle.value & DOS_MEMORY_LEASE_SLOT_MASK);
	copy_mcb(receipt.guest_segment, before);
	writes_before = write_calls;
	if (dos_memory_lease_prepare_owner_name_patch_rebind(
		&table, &arena, &machine, receipt.handle, parent.psp_segment,
		0x6161u, &name_patch, &plan) != DOS_MEMORY_LEASE_OK ||
	    write_calls != writes_before ||
	    !mcb_matches(receipt.guest_segment, before) ||
	    table.slots[slot_index].owner != parent.psp_segment ||
	    table.slots[slot_index].state != DOS_MEMORY_LEASE_SLOT_ACTIVE ||
	    plan.handle.value != receipt.handle.value ||
	    plan.machine_context != machine.context ||
	    plan.arena_identity != arena.identity ||
	    plan.arena_generation != arena.generation ||
	    plan.guest_segment != receipt.guest_segment ||
	    plan.paragraphs != receipt.paragraphs ||
	    plan.arena_head_segment != arena.head_segment ||
	    plan.value.expected_owner != parent.psp_segment ||
	    plan.value.new_owner != 0x6161u ||
	    get_le16(plan.value.replacement_bytes + MCB_OWNER_OFFSET) !=
		0x6161u ||
	    plan.value.replacement_bytes[MCB_NAME_OFFSET] != (uint8_t)'W' ||
	    plan.value.replacement_bytes[MCB_NAME_OFFSET + 1u] !=
		(uint8_t)'I' ||
	    plan.value.replacement_bytes[MCB_NAME_OFFSET + 2u] !=
		(uint8_t)'N' ||
	    plan.value.replacement_bytes[MCB_NAME_OFFSET + 3u] != 0u ||
	    plan.value.replacement_bytes[MCB_NAME_OFFSET + 4u] !=
		(uint8_t)'N')
		return 2;

	reads_before = read_calls;
	writes_before = write_calls;
	if (dos_memory_lease_preflight_rebind_publish(
		&table, &arena, &machine, &plan) != DOS_MEMORY_LEASE_OK ||
	    read_calls != reads_before || write_calls != writes_before)
		return 3;
	tampered = plan;
	++tampered.paragraphs;
	if (dos_memory_lease_preflight_rebind_publish(
		&table, &arena, &machine, &tampered) !=
		DOS_MEMORY_LEASE_STALE_HANDLE ||
	    table.slots[slot_index].owner != parent.psp_segment ||
	    table.slots[slot_index].state != DOS_MEMORY_LEASE_SLOT_ACTIVE ||
	    read_calls != reads_before || write_calls != writes_before)
		return 4;

	/* Simulate the enclosing EXEC journal's already-staged MCB bytes. */
	for (index = 0u; index < DOS_MEMORY_MCB_BYTES; ++index)
		mcb_bytes(receipt.guest_segment)[index] =
		    plan.value.replacement_bytes[index];
	if (dos_memory_lease_rebind_publish(&table, &arena, &machine, &plan) !=
		DOS_MEMORY_LEASE_OK ||
	    table.slots[slot_index].owner != 0x6161u ||
	    table.slots[slot_index].state != DOS_MEMORY_LEASE_SLOT_PUBLISHED ||
	    get_le16(mcb_bytes(receipt.guest_segment) + MCB_OWNER_OFFSET) !=
		0x6161u ||
	    read_calls != reads_before || write_calls != writes_before ||
	    !dos_memory_lease_table_is_drained(&table))
		return 5;
	return 0;
}

static int run_tests(void)
{
	int status = test_owner_name_abort_and_aba();

	if (status != 0)
		return 10 + status;
	status = test_arena_generation_rejects_old_lease();
	if (status != 0)
		return 30 + status;
	status = test_fixed_slots_and_failed_publication();
	if (status != 0)
		return 50 + status;
	status = test_table_initializes_once();
	if (status != 0)
		return 70 + status;
	status = test_fixed_encoding_fails_closed();
	if (status != 0)
		return 80 + status;
	status = test_table_lifetime_identity_rejects_aba();
	if (status != 0)
		return 90 + status;
	status = test_arena_identity_rejects_same_location_rebind();
	if (status != 0)
		return 110 + status;
	status = test_generation_exhaustion_never_wraps();
	if (status != 0)
		return 130 + status;
	status = test_active_view_resolution();
	if (status != 0)
		return 150 + status;
	status = test_ownership_transfer_and_publish();
	if (status != 0)
		return 170 + status;
	status = test_transfer_failure_and_poison();
	if (status != 0)
		return 190 + status;
	status = test_rebind_plan_and_callback_free_publish();
	if (status != 0)
		return 210 + status;
	return 0;
}

DOSC32_TEST_ENTRY(run_tests)

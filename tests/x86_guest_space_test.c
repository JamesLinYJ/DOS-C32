// SPDX-License-Identifier: GPL-2.0-only
/* Host-safe transaction tests for display memory plus I/O foreground leases. */
#include "test_entry.h"
#include "vm86_firmware.h"
#include "x86_guest_space.h"

#include "x86_legacy_bios.h"
#include "x86_guest_memory_runtime.h"

#define ADDRESS_SPACE_IDENTITY                                             \
	((kernel_object_handle_t)0x4144445253504143ull)
#define MACHINE_IDENTITY ((kernel_object_handle_t)0x4d414348494e4531ull)
#define REQUESTER_IDENTITY ((kernel_object_handle_t)0x5245515545535431ull)
#define OTHER_REQUESTER_IDENTITY                                           \
	((kernel_object_handle_t)0x5245515545535432ull)
#define INTERRUPT_SOURCE_IDENTITY                                         \
	((kernel_object_handle_t)0x495251534f555243ull)
#define SECOND_INTERRUPT_SOURCE_IDENTITY                                  \
	((kernel_object_handle_t)0x495251534f555232ull)
#define IRQ_ROUTER_IDENTITY ((kernel_object_handle_t)0x495251524f555445ull)
#define I8042_IRQ_PRODUCER_IDENTITY                                       \
	((kernel_object_handle_t)0x4938303432495251ull)
#define I8042_INPUT_SOURCE_IDENTITY                                       \
	((kernel_object_handle_t)0x4938303432494e50ull)
#define TEST_PAGE_DIRECTORY 0x00123000u
#define DISPLAY_TEST_PORT 0x03c0u
#define SYSTEM_CONTROL_A_TEST_PORT 0x0092u
#define MCGA_GRAPHICS_BASE 0x000a0000u
#define MCGA_GRAPHICS_BYTES 0x00010000u
#define MCGA_COLOR_TEXT_BASE 0x000b8000u
#define MCGA_COLOR_TEXT_BYTES 0x00008000u
#define TEST_PRIVATE_PAGE_COUNT 2u
#define TEST_PAGE_PRESENT (1u << 0)
#define TEST_PAGE_WRITABLE (1u << 1)
#define TEST_PAGE_USER (1u << 2)
#define TEST_FIRMWARE_PAGE X86_DOS_VIDEO_LIMIT

static uint8_t video_granted_ranges;
static bool fail_video_grant;
static uint8_t fail_video_grant_bit;
static bool fail_video_revoke;
static bool fail_io_release;
static bool fail_io_revoke;
static bool guest_range_accessible = true;
static uint32_t guest_range_address;
static size_t guest_range_count;
static bool guest_range_writable;
static uint8_t guest_memory[X86_REAL_MODE_LINEAR_LIMIT];
static uint8_t private_memory[TEST_PRIVATE_PAGE_COUNT][X86_PAGE_BYTES];
static bool private_leased[TEST_PRIVATE_PAGE_COUNT];
static uint64_t private_generation[TEST_PRIVATE_PAGE_COUNT];
static bool shadow_active[TEST_PRIVATE_PAGE_COUNT];
static uint32_t shadow_linear[TEST_PRIVATE_PAGE_COUNT];
static bool corrupt_shadow_mapping;
static bool fail_private_release_once;
static bool fail_private_release_always;
static uint32_t private_allocation_count;
static uint32_t private_release_count;

bool x86_firmware_shadow_test_physical_map(
	uint32_t physical_address, size_t count, bool writable,
	struct native_mapping *mapping);
bool x86_guest_space_test_physical_map(
	uint32_t physical_address, size_t count, bool writable,
	struct native_mapping *mapping);

enum x86_io_resource_status x86_guest_space_test_foreground_acquire(
	x86_io_resource_handle_t resource,
	kernel_object_handle_t owner_identity,
	kernel_object_handle_t requester_identity,
	x86_io_foreground_token_t *token);
enum x86_io_resource_status x86_guest_space_test_foreground_release(
	x86_io_foreground_token_t token,
	kernel_object_handle_t requester_identity);
enum x86_io_resource_status x86_guest_space_test_foreground_revoke(
	x86_io_resource_handle_t resource,
	kernel_object_handle_t owner_identity);

enum x86_io_resource_status x86_guest_space_test_foreground_acquire(
	x86_io_resource_handle_t resource,
	kernel_object_handle_t owner_identity,
	kernel_object_handle_t requester_identity,
	x86_io_foreground_token_t *token)
{
	return x86_io_resource_foreground_acquire(
		resource, owner_identity, requester_identity, token);
}

enum x86_io_resource_status x86_guest_space_test_foreground_release(
	x86_io_foreground_token_t token,
	kernel_object_handle_t requester_identity)
{
	if (fail_io_release)
		return X86_IO_RESOURCE_INVALID_STATE;
	return x86_io_resource_foreground_release(token, requester_identity);
}

enum x86_io_resource_status x86_guest_space_test_foreground_revoke(
	x86_io_resource_handle_t resource,
	kernel_object_handle_t owner_identity)
{
	if (fail_io_revoke)
		return X86_IO_RESOURCE_INVALID_STATE;
	return x86_io_resource_foreground_revoke(resource, owner_identity);
}

enum dos_machine_status dos_machine_configure(
	struct dos_machine *machine, const struct dos_machine_ops *ops,
	kernel_object_handle_t context, uint64_t address_limit, bool a20_enabled)
{
	if (machine == NULL || ops == NULL || ops->read_memory == NULL ||
	    ops->write_memory == NULL || context == 0u ||
	    context == KERNEL_OBJECT_HANDLE_INVALID || address_limit == 0u)
		return DOS_MACHINE_INVALID_ARGUMENT;
	machine->ops = ops;
	machine->context = context;
	machine->address_limit = address_limit;
	machine->a20_enabled = a20_enabled;
	machine->poisoned = 0u;
	return DOS_MACHINE_OK;
}

bool x86_paging_snapshot(struct x86_paging_binding *binding)
{
	if (binding == NULL)
		return false;
	*binding = (struct x86_paging_binding){
		.generation = 1u,
		.page_directory = TEST_PAGE_DIRECTORY,
		.guest_linear_limit = X86_REAL_MODE_LINEAR_LIMIT,
	};
	return true;
}

bool x86_paging_binding_is_active(const struct x86_paging_binding *binding)
{
	return binding != NULL && binding->generation == 1u &&
	       binding->page_directory == TEST_PAGE_DIRECTORY &&
	       binding->guest_linear_limit == X86_REAL_MODE_LINEAR_LIMIT;
}

static bool fake_private_address(uint32_t physical_address, size_t count,
				 uint32_t *index, uint32_t *offset)
{
	uint32_t relative;

	if (index == NULL || offset == NULL ||
	    physical_address < X86_BOOT_IDENTITY_FLOOR ||
	    physical_address >= X86_BOOT_IDENTITY_FLOOR +
				TEST_PRIVATE_PAGE_COUNT * X86_PAGE_BYTES)
		return false;
	relative = physical_address - X86_BOOT_IDENTITY_FLOOR;
	*index = relative / X86_PAGE_BYTES;
	*offset = relative & (X86_PAGE_BYTES - 1u);
	return count <= (size_t)(X86_PAGE_BYTES - *offset);
}

static bool fake_physical_map(uint32_t physical_address, size_t count,
			      bool writable, struct native_mapping *mapping)
{
	uint32_t index;
	uint32_t offset;

	(void)writable;
	if (mapping == NULL)
		return false;
	if (physical_address < X86_REAL_MODE_LINEAR_LIMIT &&
	    count <= (size_t)(X86_REAL_MODE_LINEAR_LIMIT - physical_address)) {
		mapping->pointer = &guest_memory[physical_address];
		mapping->length = count;
		return true;
	}
	if (!fake_private_address(physical_address, count, &index, &offset) ||
	    !private_leased[index])
		return false;
	mapping->pointer = &private_memory[index][offset];
	mapping->length = count;
	return true;
}

bool x86_firmware_shadow_test_physical_map(
	uint32_t physical_address, size_t count, bool writable,
	struct native_mapping *mapping)
{
	return fake_physical_map(physical_address, count, writable, mapping);
}

bool x86_guest_space_test_physical_map(
	uint32_t physical_address, size_t count, bool writable,
	struct native_mapping *mapping)
{
	return fake_physical_map(physical_address, count, writable, mapping);
}

static bool shadow_index_for_physical(uint32_t physical_page,
				      uint32_t *index)
{
	uint32_t offset;

	return fake_private_address(physical_page, X86_PAGE_BYTES, index,
				    &offset) &&
	       offset == 0u && private_leased[*index];
}

bool x86_paging_guest_identity_translate(
	const struct x86_paging_binding *binding, uint32_t address,
	bool writable, struct x86_paging_guest_translation *translation)
{
	uint32_t page = address & ~(X86_PAGE_BYTES - 1u);
	size_t index;

	guest_range_address = address;
	guest_range_count = 1u;
	guest_range_writable = writable;
	if (!x86_paging_binding_is_active(binding) || translation == NULL ||
	    address >= X86_REAL_MODE_LINEAR_LIMIT || !guest_range_accessible)
		return false;
	for (index = 0u; index < TEST_PRIVATE_PAGE_COUNT; ++index) {
		if (shadow_active[index] && shadow_linear[index] == page)
			return false;
	}
	if (writable && address >= X86_DOS_VIDEO_LIMIT &&
	    address < X86_LEGACY_ROM_LIMIT)
		return false;
	*translation = (struct x86_paging_guest_translation){
		.physical_address = address,
		.contiguous_bytes =
			X86_PAGE_BYTES - (address & (X86_PAGE_BYTES - 1u)),
	};
	return true;
}

enum x86_paging_guest_shadow_status x86_paging_guest_shadow_snapshot(
	const struct x86_paging_binding *binding, uint32_t linear_page,
	struct x86_paging_guest_shadow_snapshot *snapshot)
{
	size_t index;

	if (binding == NULL || snapshot == NULL ||
	    (linear_page & (X86_PAGE_BYTES - 1u)) != 0u ||
	    linear_page < X86_DOS_VIDEO_LIMIT ||
	    linear_page >= X86_LEGACY_ROM_LIMIT)
		return X86_PAGING_GUEST_SHADOW_INVALID_ARGUMENT;
	if (!x86_paging_binding_is_active(binding))
		return X86_PAGING_GUEST_SHADOW_STALE_BINDING;
	for (index = 0u; index < TEST_PRIVATE_PAGE_COUNT; ++index) {
		if (shadow_active[index] && shadow_linear[index] == linear_page)
			return X86_PAGING_GUEST_SHADOW_SOURCE_MISMATCH;
	}
	*snapshot = (struct x86_paging_guest_shadow_snapshot){
		.paging = *binding,
		.linear_page = linear_page,
		.original_entry = linear_page | TEST_PAGE_PRESENT | TEST_PAGE_USER,
		.reserved = {0u},
	};
	return X86_PAGING_GUEST_SHADOW_OK;
}

enum x86_paging_guest_shadow_status x86_paging_guest_shadow_publish(
	const struct x86_paging_guest_shadow_snapshot *snapshot,
	uint32_t private_physical_page)
{
	uint32_t index;

	if (snapshot == NULL ||
	    !x86_paging_binding_is_active(&snapshot->paging) ||
	    !shadow_index_for_physical(private_physical_page, &index))
		return X86_PAGING_GUEST_SHADOW_TARGET_DENIED;
	if (shadow_active[index])
		return X86_PAGING_GUEST_SHADOW_SOURCE_MISMATCH;
	shadow_active[index] = true;
	shadow_linear[index] = snapshot->linear_page;
	return X86_PAGING_GUEST_SHADOW_OK;
}

enum x86_paging_guest_shadow_status x86_paging_guest_shadow_restore(
	const struct x86_paging_guest_shadow_snapshot *snapshot,
	uint32_t private_physical_page)
{
	uint32_t index;
	bool corrupted;

	if (snapshot == NULL ||
	    !x86_paging_binding_is_active(&snapshot->paging) ||
	    !shadow_index_for_physical(private_physical_page, &index))
		return X86_PAGING_GUEST_SHADOW_STALE_BINDING;
	if (!shadow_active[index])
		return X86_PAGING_GUEST_SHADOW_OK;
	corrupted = corrupt_shadow_mapping;
	shadow_active[index] = false;
	shadow_linear[index] = 0u;
	corrupt_shadow_mapping = false;
	return corrupted ? X86_PAGING_GUEST_SHADOW_MAPPING_MISMATCH
			 : X86_PAGING_GUEST_SHADOW_OK;
}

bool x86_paging_guest_shadow_translate(
	const struct x86_paging_guest_shadow_snapshot *snapshot,
	uint32_t private_physical_page, uint32_t address, bool writable,
	struct x86_paging_guest_translation *translation)
{
	uint32_t index;
	uint32_t offset;

	(void)writable;
	if (snapshot == NULL || translation == NULL || corrupt_shadow_mapping ||
	    !shadow_index_for_physical(private_physical_page, &index) ||
	    !shadow_active[index] ||
	    shadow_linear[index] != snapshot->linear_page ||
	    address < snapshot->linear_page ||
	    address >= snapshot->linear_page + X86_PAGE_BYTES)
		return false;
	offset = address - snapshot->linear_page;
	*translation = (struct x86_paging_guest_translation){
		.physical_address = private_physical_page + offset,
		.contiguous_bytes = X86_PAGE_BYTES - offset,
	};
	return true;
}

enum x86_guest_memory_status x86_guest_memory_runtime_allocate(
	kernel_object_handle_t allocation_owner, uint32_t page_count,
	x86_guest_memory_lease_t *lease, uint32_t *physical_address)
{
	uint32_t index;
	uint32_t byte_index;

	if (allocation_owner != ADDRESS_SPACE_IDENTITY || page_count != 1u ||
	    lease == NULL || physical_address == NULL)
		return X86_GUEST_MEMORY_INVALID_ARGUMENT;
	for (index = 0u; index < TEST_PRIVATE_PAGE_COUNT; ++index) {
		if (private_leased[index])
			continue;
		private_leased[index] = true;
		++private_generation[index];
		for (byte_index = 0u; byte_index < X86_PAGE_BYTES; ++byte_index)
			private_memory[index][byte_index] = 0u;
		*lease = (private_generation[index] << 8u) | (index + 1u);
		*physical_address = X86_BOOT_IDENTITY_FLOOR +
				    index * X86_PAGE_BYTES;
		++private_allocation_count;
		return X86_GUEST_MEMORY_OK;
	}
	return X86_GUEST_MEMORY_NO_MEMORY;
}

static bool resolve_private_lease(x86_guest_memory_lease_t lease,
				  uint32_t *index)
{
	uint32_t encoded;
	uint64_t generation;

	if (index == NULL)
		return false;
	encoded = (uint32_t)(lease & 0xffu);
	generation = lease >> 8u;
	if (encoded == 0u || encoded > TEST_PRIVATE_PAGE_COUNT)
		return false;
	*index = encoded - 1u;
	return private_leased[*index] &&
	       private_generation[*index] == generation;
}

enum x86_guest_memory_status x86_guest_memory_runtime_release(
	kernel_object_handle_t allocation_owner, x86_guest_memory_lease_t lease)
{
	uint32_t index;
	uint32_t byte_index;

	if (allocation_owner != ADDRESS_SPACE_IDENTITY)
		return X86_GUEST_MEMORY_OWNER_MISMATCH;
	if (!resolve_private_lease(lease, &index))
		return X86_GUEST_MEMORY_STALE_LEASE;
	if (fail_private_release_once || fail_private_release_always) {
		fail_private_release_once = false;
		return X86_GUEST_MEMORY_ZERO_FAILED;
	}
	for (byte_index = 0u; byte_index < X86_PAGE_BYTES; ++byte_index)
		private_memory[index][byte_index] = 0u;
	private_leased[index] = false;
	++private_release_count;
	return X86_GUEST_MEMORY_OK;
}

enum x86_guest_memory_status x86_guest_memory_runtime_inspect(
	x86_guest_memory_lease_t lease,
	struct x86_guest_memory_lease_info *info)
{
	uint32_t index;

	if (info == NULL)
		return X86_GUEST_MEMORY_INVALID_ARGUMENT;
	if (!resolve_private_lease(lease, &index))
		return X86_GUEST_MEMORY_STALE_LEASE;
	*info = (struct x86_guest_memory_lease_info){
		.owner = ADDRESS_SPACE_IDENTITY,
		.physical_address = X86_BOOT_IDENTITY_FLOOR +
				    index * X86_PAGE_BYTES,
		.page_count = 1u,
	};
	return X86_GUEST_MEMORY_OK;
}

bool x86_paging_guest_range_is_accessible(uint32_t address, size_t count,
					  bool writable)
{
	guest_range_address = address;
	guest_range_count = count;
	guest_range_writable = writable;
	return guest_range_accessible;
}

static uint8_t video_range_bit(uint32_t address, size_t count)
{
	if (address == MCGA_GRAPHICS_BASE && count == MCGA_GRAPHICS_BYTES)
		return 1u;
	if (address == MCGA_COLOR_TEXT_BASE && count == MCGA_COLOR_TEXT_BYTES)
		return 2u;
	return 0u;
}

bool x86_paging_grant_legacy_video_range(uint32_t address, size_t count)
{
	uint8_t bit = video_range_bit(address, count);

	if (fail_video_grant || bit == 0u || bit == fail_video_grant_bit ||
	    (video_granted_ranges & bit) != 0u)
		return false;
	video_granted_ranges |= bit;
	return true;
}

bool x86_paging_revoke_legacy_video_range(uint32_t address, size_t count)
{
	uint8_t bit = video_range_bit(address, count);

	if (bit == 0u || fail_video_revoke)
		return false;
	video_granted_ranges &= (uint8_t)~bit;
	return true;
}

bool x86_paging_legacy_video_range_is_user_accessible(uint32_t address,
						       size_t count)
{
	uint8_t bit = video_range_bit(address, count);

	return bit != 0u && (video_granted_ranges & bit) != 0u;
}

#if !defined(X86_GUEST_SPACE_ABSENT_DISPLAY_TEST)
static bool display_memory_is_user_accessible(void)
{
	return video_granted_ranges == 3u;
}
#endif

bool x86_legacy_bios_snapshot(struct x86_legacy_bios_snapshot *snapshot)
{
	if (snapshot == NULL)
		return false;
#if defined(X86_GUEST_SPACE_ABSENT_DISPLAY_TEST)
	*snapshot = (struct x86_legacy_bios_snapshot){
		.generation = 1u,
		.conventional_kib = 640u,
	};
#else
	*snapshot = (struct x86_legacy_bios_snapshot){
		.generation = 1u,
		.conventional_kib = 640u,
		.display_capabilities =
			X86_LEGACY_DISPLAY_COLOR_TEXT_MEMORY |
			X86_LEGACY_DISPLAY_GRAPHICS_MEMORY |
			X86_LEGACY_DISPLAY_COLOR_CRTC_IO |
			X86_LEGACY_DISPLAY_CONTROL_IO |
			X86_LEGACY_DISPLAY_MCGA_COMPATIBLE,
		.display_active_dcc = 0x0cu,
		.display_dcc_valid = 1u,
	};
#endif
	return true;
}

#if !defined(X86_GUEST_SPACE_ABSENT_DISPLAY_TEST)
static bool resource_vga_io_is_closed(void)
{
	uint32_t value = 0x12345678u;

	return x86_io_resource_read(REQUESTER_IDENTITY, DISPLAY_TEST_PORT,
				    DOS_IO_WIDTH_8, &value) ==
		       X86_IO_RESOURCE_ACCESS_DENIED &&
	       value == 0x12345678u;
}

static bool resource_vga_io_is_open(void)
{
	uint32_t value = 0x12345678u;

	return x86_io_resource_read(REQUESTER_IDENTITY, DISPLAY_TEST_PORT,
				    DOS_IO_WIDTH_8, &value) ==
		       X86_IO_RESOURCE_CALLBACK_FAULT &&
	       value == 0x12345678u;
}

static bool guest_vga_io_is_closed(const struct dos_machine *machine)
{
	uint32_t value = 0x12345678u;

	return machine != NULL && machine->ops != NULL &&
		       machine->ops->read_port(machine->context, DISPLAY_TEST_PORT,
				       DOS_IO_WIDTH_8, &value) ==
		       DOS_MACHINE_IO_DENIED &&
		       value == 0x12345678u;
}
#endif

static int run_tests(void)
{
	const struct x86_guest_space_config guest_config = {
		.address_space_identity = ADDRESS_SPACE_IDENTITY,
		.machine_identity = MACHINE_IDENTITY,
		.irq_router_identity = IRQ_ROUTER_IDENTITY,
		.i8042_irq_producer_identity = I8042_IRQ_PRODUCER_IDENTITY,
		.i8042 = {
			.command_byte = X86_I8042_COMMAND_BYTE_SYSTEM |
					X86_I8042_COMMAND_BYTE_IRQ1 |
					X86_I8042_COMMAND_BYTE_IRQ12,
			.input_port = 0x5au,
			.output_port = X86_I8042_OUTPUT_PORT_RESET_HIGH,
			.keyboard_present = 1u,
			.auxiliary_present = 1u,
			.keyboard_scanning_enabled = 1u,
			.keyboard_scan_set = 2u,
			.keyboard_unlocked = 1u,
			.keyboard_id_length = 2u,
			.keyboard_id_first = 0xabu,
			.keyboard_id_second = 0x83u,
			.auxiliary_id = 0u,
			.reserved = {0u},
		},
		.reserved = {0u},
	};
	const struct dos_machine *machine;
	size_t memory_index;
#if !defined(X86_GUEST_SPACE_ABSENT_DISPLAY_TEST)
	const struct x86_i8042_input_config input_config = {
		.capabilities = X86_I8042_INPUT_KEYBOARD,
		.reserved = {0u},
	};
	const uint8_t input_sequence[] = {0x1eu, 0x9eu};
	struct x86_guest_space_pit_binding pit_binding = {
		.source_identity = OTHER_REQUESTER_IDENTITY,
	};
	struct x86_guest_space_pit_binding stale_pit_binding;
	struct x86_i8042_input_binding input_binding;
	struct x86_i8042_input_binding stale_input_binding;
	struct x86_i8042_keyboard_mode input_mode;
	struct x86_i8042_keyboard_mode translated_input_mode;
	struct x86_guest_space_firmware_binding firmware_first;
	struct x86_guest_space_firmware_binding firmware_second;
	struct x86_guest_space_firmware_binding firmware_third;
	struct x86_legacy_interrupt_claim interrupt_claim = {0};
	size_t processed = 0u;
	uint32_t input_value = 0u;
	x86_io_foreground_token_t next_token;
	uint16_t conventional_end = 0u;
	bool a20_enabled = true;
	uint8_t unchanged = 0xa5u;
	uint8_t original_cross_page[4];
	uint8_t shadow_readback[4] = {0u};
	const uint8_t shadow_write[4] = {0x11u, 0x22u, 0x33u, 0x44u};
#endif
	x86_io_foreground_token_t token = 0x1111u;

	for (memory_index = 0u; memory_index < 2u * X86_PAGE_BYTES;
	     ++memory_index)
		guest_memory[TEST_FIRMWARE_PAGE + memory_index] =
			(uint8_t)(memory_index ^ 0x5au);
	if (x86_guest_space_initialize(&guest_config) != X86_GUEST_SPACE_OK)
		return 1;
	machine = x86_guest_space_machine();
#if defined(X86_GUEST_SPACE_ABSENT_DISPLAY_TEST)
	{
		uint32_t absent_value = 0u;

		if (machine == NULL ||
		    machine->ops->read_port(machine->context, DISPLAY_TEST_PORT,
					    DOS_IO_WIDTH_8, &absent_value) !=
			    DOS_MACHINE_OK ||
		    absent_value != 0xffu ||
		    x86_guest_space_display_foreground_acquire(
			    MACHINE_IDENTITY, REQUESTER_IDENTITY, &token) !=
			    X86_GUEST_SPACE_IO_DENIED ||
		    token != 0x1111u || video_granted_ranges != 0u)
			return 24;
		return 0;
	}
#else
	if (machine == NULL || machine->context != ADDRESS_SPACE_IDENTITY ||
	    x86_guest_space_machine_identity() != MACHINE_IDENTITY ||
	    display_memory_is_user_accessible() ||
	    !resource_vga_io_is_closed() || !guest_vga_io_is_closed(machine))
		return 2;
#if defined(X86_GUEST_SPACE_QUARANTINE_TEST)
	{
		struct dos_machine wrong_machine = *machine;

		if (x86_guest_space_display_foreground_acquire(
			    MACHINE_IDENTITY, REQUESTER_IDENTITY, &token) !=
				    X86_GUEST_SPACE_OK ||
		    !display_memory_is_user_accessible() ||
		    !resource_vga_io_is_open() ||
		    x86_guest_space_quarantine(MACHINE_IDENTITY,
					       &wrong_machine) !=
			    X86_GUEST_SPACE_MACHINE_MISMATCH ||
		    x86_guest_space_quarantine(MACHINE_IDENTITY, machine) !=
			    X86_GUEST_SPACE_OK ||
		    x86_guest_space_machine() != NULL ||
		    display_memory_is_user_accessible() ||
		    !resource_vga_io_is_closed() ||
		    x86_guest_space_display_foreground_revoke(MACHINE_IDENTITY) !=
			    X86_GUEST_SPACE_OK ||
		    x86_guest_space_quarantine(MACHINE_IDENTITY, machine) !=
			    X86_GUEST_SPACE_OK)
			return 45;
		return 0;
	}
#endif
#if defined(X86_GUEST_SPACE_FIRMWARE_CORRUPTION_TEST)
	if (x86_guest_space_firmware_execution_acquire(
		    MACHINE_IDENTITY, &firmware_first) != X86_GUEST_SPACE_OK ||
	    x86_guest_space_firmware_write_fault(
		    MACHINE_IDENTITY, &firmware_first, 0x07u,
		    TEST_FIRMWARE_PAGE + 0x123u) != X86_GUEST_SPACE_OK)
		return 35;
	corrupt_shadow_mapping = true;
	if (x86_vm86_firmware_execution_release(
		    MACHINE_IDENTITY, &firmware_first) !=
		    DOS_EXEC_BACKEND_RELEASE_UNCERTAIN ||
	    shadow_active[0] || private_leased[0] ||
	    private_release_count != 1u || x86_guest_space_machine() != NULL)
		return 36;
	return 0;
#endif
#if defined(X86_GUEST_SPACE_FIRMWARE_EXHAUSTION_TEST)
	if (x86_guest_space_firmware_execution_acquire(
		    MACHINE_IDENTITY, &firmware_first) != X86_GUEST_SPACE_OK ||
	    x86_guest_space_firmware_write_fault(
		    MACHINE_IDENTITY, &firmware_first, 0x07u,
		    TEST_FIRMWARE_PAGE + 0x123u) != X86_GUEST_SPACE_OK ||
	    x86_guest_space_firmware_write_fault(
		    MACHINE_IDENTITY, &firmware_first, 0x07u,
		    TEST_FIRMWARE_PAGE + X86_PAGE_BYTES + 0x123u) !=
		    X86_GUEST_SPACE_OK)
		return 43;
	fail_private_release_always = true;
	if (x86_vm86_firmware_execution_release(
		    MACHINE_IDENTITY, &firmware_first) !=
		    DOS_EXEC_BACKEND_RELEASE_UNCERTAIN ||
	    shadow_active[0] || shadow_active[1] || !private_leased[0] ||
	    !private_leased[1] || x86_guest_space_machine() != NULL)
		return 44;
	return 0;
#endif
	/* Exact P|W|U faults create one private page, repeat idempotently, and
	 * share it across nested execution clients until the final release. */
	if (x86_guest_space_firmware_execution_acquire(
		    MACHINE_IDENTITY, &firmware_first) != X86_GUEST_SPACE_OK ||
	    x86_guest_space_firmware_execution_acquire(
		    MACHINE_IDENTITY, &firmware_second) != X86_GUEST_SPACE_OK ||
	    x86_guest_space_firmware_write_fault(
		    MACHINE_IDENTITY, &firmware_first, 0x03u,
		    TEST_FIRMWARE_PAGE + 0x123u) != X86_GUEST_SPACE_IO_DENIED ||
	    x86_guest_space_firmware_write_fault(
		    MACHINE_IDENTITY, &firmware_first, 0x0fu,
		    TEST_FIRMWARE_PAGE + 0x123u) != X86_GUEST_SPACE_IO_DENIED ||
	    x86_guest_space_firmware_write_fault(
		    MACHINE_IDENTITY, &firmware_first, 0x07u,
		    X86_DOS_VIDEO_LIMIT - 1u) != X86_GUEST_SPACE_IO_DENIED ||
	    private_allocation_count != 0u ||
	    x86_guest_space_firmware_write_fault(
		    MACHINE_IDENTITY, &firmware_first, 0x07u,
		    TEST_FIRMWARE_PAGE + 0x123u) != X86_GUEST_SPACE_OK ||
	    private_allocation_count != 1u || !shadow_active[0] ||
	    private_memory[0][0x123u] !=
		    guest_memory[TEST_FIRMWARE_PAGE + 0x123u] ||
	    x86_guest_space_firmware_write_fault(
		    MACHINE_IDENTITY, &firmware_first, 0x07u,
		    TEST_FIRMWARE_PAGE + 0x123u) != X86_GUEST_SPACE_OK ||
	    private_allocation_count != 1u ||
	    x86_guest_space_firmware_write_fault(
		    MACHINE_IDENTITY, &firmware_second, 0x07u,
		    TEST_FIRMWARE_PAGE + X86_PAGE_BYTES + 1u) !=
		    X86_GUEST_SPACE_OK ||
	    private_allocation_count != 2u || !shadow_active[1])
		return 37;
	for (memory_index = 0u; memory_index < ARRAY_SIZE(original_cross_page);
	     ++memory_index)
		original_cross_page[memory_index] =
			guest_memory[TEST_FIRMWARE_PAGE + X86_PAGE_BYTES - 2u +
				     memory_index];
	if (machine->ops->write_memory(
		    machine->context,
		    TEST_FIRMWARE_PAGE + X86_PAGE_BYTES - 2u, shadow_write,
		    ARRAY_SIZE(shadow_write), ARRAY_SIZE(shadow_write)) !=
		    DOS_MACHINE_OK ||
	    machine->ops->read_memory(
		    machine->context,
		    TEST_FIRMWARE_PAGE + X86_PAGE_BYTES - 2u, shadow_readback,
		    ARRAY_SIZE(shadow_readback), ARRAY_SIZE(shadow_readback)) !=
		    DOS_MACHINE_OK)
		return 38;
	for (memory_index = 0u; memory_index < ARRAY_SIZE(shadow_write);
	     ++memory_index) {
		if (shadow_readback[memory_index] != shadow_write[memory_index] ||
		    guest_memory[TEST_FIRMWARE_PAGE + X86_PAGE_BYTES - 2u +
				 memory_index] != original_cross_page[memory_index])
			return 39;
	}
	if (x86_guest_space_firmware_write_fault(
		    MACHINE_IDENTITY, &firmware_first, 0x07u,
		    TEST_FIRMWARE_PAGE + 2u * X86_PAGE_BYTES) !=
		    X86_GUEST_SPACE_CAPACITY_EXHAUSTED ||
	    x86_guest_space_firmware_execution_release(
		    MACHINE_IDENTITY, &firmware_second) != X86_GUEST_SPACE_OK ||
	    private_release_count != 0u || !shadow_active[0] ||
	    !shadow_active[1] ||
	    x86_guest_space_firmware_execution_release(
		    MACHINE_IDENTITY, &firmware_first) != X86_GUEST_SPACE_OK ||
	    private_release_count != 2u || shadow_active[0] ||
	    shadow_active[1] || private_leased[0] || private_leased[1] ||
	    x86_guest_space_firmware_execution_release(
		    MACHINE_IDENTITY, &firmware_first) != X86_GUEST_SPACE_OK)
		return 40;
	if (x86_guest_space_firmware_execution_acquire(
		    MACHINE_IDENTITY, &firmware_third) != X86_GUEST_SPACE_OK ||
	    x86_guest_space_firmware_execution_release(
		    MACHINE_IDENTITY, &firmware_second) !=
		    X86_GUEST_SPACE_STALE_BINDING ||
	    x86_guest_space_firmware_write_fault(
		    MACHINE_IDENTITY, &firmware_third, 0x07u,
		    TEST_FIRMWARE_PAGE + 7u) != X86_GUEST_SPACE_OK)
		return 41;
	fail_private_release_once = true;
	if (x86_guest_space_firmware_execution_release(
		    MACHINE_IDENTITY, &firmware_third) !=
		    X86_GUEST_SPACE_DEVICE_EVENT_RETRY ||
	    shadow_active[0] || !private_leased[0] ||
	    x86_guest_space_firmware_execution_release(
		    MACHINE_IDENTITY, &firmware_third) != X86_GUEST_SPACE_OK ||
	    private_leased[0] || private_release_count != 3u)
		return 42;
	if (machine->ops->read_memory(machine->context, 0x1234u, NULL, 0u,
				      0u) != DOS_MACHINE_OK ||
	    machine->ops->write_memory(machine->context, 0x5678u, NULL, 0u,
				       0u) != DOS_MACHINE_OK)
		return 21;
	guest_range_accessible = false;
	if (machine->ops->read_memory(machine->context, 0x100000u, &unchanged,
				      sizeof(unchanged), 1u) !=
		    DOS_MACHINE_ADDRESS_FAULT ||
	    unchanged != 0xa5u || guest_range_address != 0x100000u ||
	    guest_range_count != 1u || guest_range_writable ||
	    machine->ops->write_memory(machine->context, 0x100000u, &unchanged,
				       sizeof(unchanged), 1u) !=
		    DOS_MACHINE_ADDRESS_FAULT ||
	    !guest_range_writable)
		return 22;
	guest_range_accessible = true;
	if (machine->ops->query_a20(machine->context, &a20_enabled) !=
			DOS_MACHINE_OK ||
	    a20_enabled ||
	    machine->ops->set_a20(machine->context, true) != DOS_MACHINE_OK ||
	    machine->ops->query_a20(machine->context, &a20_enabled) !=
			DOS_MACHINE_OK ||
	    !a20_enabled || !machine->a20_enabled ||
	    machine->ops->set_a20(machine->context, false) != DOS_MACHINE_OK ||
	    machine->a20_enabled)
		return 14;
	if (x86_guest_space_conventional_end_segment(&conventional_end) !=
		    X86_GUEST_SPACE_OK ||
	    conventional_end != 0xa000u)
		return 3;
	/* A duplicate producer fails before publish and unwinds to a retryable
	 * empty router; the caller's output binding remains untouched. */
	if (x86_guest_space_native_pit_bind(
		    OTHER_REQUESTER_IDENTITY, INTERRUPT_SOURCE_IDENTITY, 1193u,
		    false, &pit_binding) != X86_GUEST_SPACE_MACHINE_MISMATCH ||
	    x86_guest_space_native_pit_bind(
		    MACHINE_IDENTITY, I8042_IRQ_PRODUCER_IDENTITY, 1193u, false,
		    &pit_binding) != X86_GUEST_SPACE_INTERRUPT_FAULT ||
	    pit_binding.source_identity != OTHER_REQUESTER_IDENTITY ||
	    x86_guest_space_native_pit_bind(
		    MACHINE_IDENTITY, INTERRUPT_SOURCE_IDENTITY, 1193u, false,
		    &pit_binding) != X86_GUEST_SPACE_OK ||
	    machine->ops->write_port(machine->context, X86_PIT_CONTROL_PORT,
				     DOS_IO_WIDTH_8, 0x36u) != DOS_MACHINE_OK ||
	    machine->ops->write_port(machine->context,
				     X86_PIT_CHANNEL0_PORT,
				     DOS_IO_WIDTH_8, 0xa9u) != DOS_MACHINE_OK ||
	    machine->ops->write_port(machine->context,
				     X86_PIT_CHANNEL0_PORT,
				     DOS_IO_WIDTH_8, 0x04u) != DOS_MACHINE_OK ||
	    x86_guest_space_native_pit_submit(&pit_binding, 1193u) !=
		    X86_GUEST_SPACE_OK ||
	    x86_guest_space_interrupt_claim(
		    OTHER_REQUESTER_IDENTITY, &interrupt_claim) !=
		    X86_GUEST_SPACE_MACHINE_MISMATCH ||
	    x86_guest_space_interrupt_claim(MACHINE_IDENTITY,
					    &interrupt_claim) !=
		    X86_GUEST_SPACE_OK ||
	    interrupt_claim.irq != 0u || interrupt_claim.vector != 0x08u ||
	    machine->ops->write_port(machine->context,
				     X86_PIC_PRIMARY_COMMAND_PORT,
				     DOS_IO_WIDTH_8, 0x20u) != DOS_MACHINE_OK)
		return 21;
	stale_pit_binding = pit_binding;
	stale_pit_binding.producer_generation++;
	if (x86_guest_space_native_pit_submit(&stale_pit_binding, 1193u) !=
		    X86_GUEST_SPACE_STALE_BINDING ||
	    x86_guest_space_i8042_input_bind(
		    MACHINE_IDENTITY, I8042_INPUT_SOURCE_IDENTITY, &input_config,
		    &input_binding) != X86_GUEST_SPACE_OK ||
	    x86_guest_space_i8042_input_keyboard_mode(
		    &input_binding, &input_mode) != X86_GUEST_SPACE_OK ||
	    input_mode.scan_set != 2u || input_mode.translation_enabled != 0u ||
	    x86_guest_space_i8042_input_inject(
		    &input_binding, X86_I8042_INPUT_KIND_KEYBOARD_SCAN,
		    input_sequence[0]) != X86_GUEST_SPACE_INVALID_ARGUMENT ||
	    x86_guest_space_i8042_input_inject_keyboard(
		    &input_binding, &input_mode, input_sequence[0]) !=
		    X86_GUEST_SPACE_OK ||
	    x86_guest_space_interrupt_claim(MACHINE_IDENTITY,
					    &interrupt_claim) !=
		    X86_GUEST_SPACE_OK ||
	    interrupt_claim.irq != 1u || interrupt_claim.vector != 0x09u ||
	    machine->ops->write_port(machine->context,
				     X86_PIC_PRIMARY_COMMAND_PORT,
				     DOS_IO_WIDTH_8, 0x20u) != DOS_MACHINE_OK ||
	    machine->ops->read_port(machine->context, X86_I8042_DATA_PORT,
				    DOS_IO_WIDTH_8, &input_value) !=
		    DOS_MACHINE_OK ||
	    input_value != input_sequence[0])
		return 22;
	/* A mode epoch closes query-to-inject races without partial enqueue. */
	if (machine->ops->write_port(machine->context, X86_I8042_COMMAND_PORT,
				     DOS_IO_WIDTH_8, 0x60u) != DOS_MACHINE_OK ||
	    machine->ops->write_port(
		    machine->context, X86_I8042_DATA_PORT, DOS_IO_WIDTH_8,
		    X86_I8042_COMMAND_BYTE_SYSTEM |
			    X86_I8042_COMMAND_BYTE_IRQ1 |
			    X86_I8042_COMMAND_BYTE_IRQ12 |
			    X86_I8042_COMMAND_BYTE_TRANSLATE) != DOS_MACHINE_OK ||
	    x86_guest_space_i8042_input_inject_keyboard_sequence(
		    &input_binding, &input_mode, input_sequence,
		    ARRAY_SIZE(input_sequence), ARRAY_SIZE(input_sequence)) !=
		    X86_GUEST_SPACE_INPUT_MODE_CHANGED ||
	    machine->ops->read_port(machine->context, X86_I8042_STATUS_PORT,
				    DOS_IO_WIDTH_8, &input_value) != DOS_MACHINE_OK ||
	    (input_value & X86_I8042_STATUS_OUTPUT_FULL) != 0u ||
	    x86_guest_space_i8042_input_keyboard_mode(
		    &input_binding, &translated_input_mode) != X86_GUEST_SPACE_OK ||
	    translated_input_mode.mode_generation <= input_mode.mode_generation ||
	    translated_input_mode.scan_set != 2u ||
	    translated_input_mode.translation_enabled != 1u)
		return 33;
	/* Test-only direct controller access creates one older pending event. */
	if (x86_io_resource_write(KERNEL_OBJECT_HANDLE_INVALID,
				  X86_I8042_COMMAND_PORT, DOS_IO_WIDTH_8,
				  0xdfu) != X86_IO_RESOURCE_OK ||
	    x86_guest_space_i8042_input_inject_keyboard_sequence(
		    &input_binding, &translated_input_mode, input_sequence,
		    ARRAY_SIZE(input_sequence), ARRAY_SIZE(input_sequence)) !=
		    X86_GUEST_SPACE_INPUT_COMMITTED_DELIVERY_PENDING ||
	    !machine->a20_enabled ||
	    x86_guest_space_device_events_pump(MACHINE_IDENTITY, 1u,
					       &processed) != X86_GUEST_SPACE_OK ||
	    processed != 1u ||
	    x86_guest_space_interrupt_claim(MACHINE_IDENTITY,
					    &interrupt_claim) !=
		    X86_GUEST_SPACE_OK ||
	    interrupt_claim.irq != 1u || interrupt_claim.vector != 0x09u ||
	    machine->ops->write_port(machine->context,
				     X86_PIC_PRIMARY_COMMAND_PORT,
				     DOS_IO_WIDTH_8, 0x20u) != DOS_MACHINE_OK ||
	    machine->ops->read_port(machine->context, X86_I8042_DATA_PORT,
				    DOS_IO_WIDTH_8, &input_value) != DOS_MACHINE_OK ||
	    input_value != input_sequence[0] ||
	    x86_guest_space_interrupt_claim(MACHINE_IDENTITY,
					    &interrupt_claim) !=
		    X86_GUEST_SPACE_OK ||
	    interrupt_claim.irq != 1u || interrupt_claim.vector != 0x09u ||
	    machine->ops->write_port(machine->context,
				     X86_PIC_PRIMARY_COMMAND_PORT,
				     DOS_IO_WIDTH_8, 0x20u) != DOS_MACHINE_OK ||
	    machine->ops->read_port(machine->context, X86_I8042_DATA_PORT,
				    DOS_IO_WIDTH_8, &input_value) != DOS_MACHINE_OK ||
	    input_value != input_sequence[1] ||
	    x86_guest_space_interrupt_claim(MACHINE_IDENTITY,
					    &interrupt_claim) !=
		    X86_GUEST_SPACE_NO_INTERRUPT)
		return 34;
	/* Both port 92h and i8042 events update one machine-owned A20 fact. */
	if (machine->ops->write_port(machine->context, X86_I8042_COMMAND_PORT,
				     DOS_IO_WIDTH_8, 0xdfu) != DOS_MACHINE_OK ||
	    machine->ops->query_a20(machine->context, &a20_enabled) !=
		    DOS_MACHINE_OK ||
	    !a20_enabled || !machine->a20_enabled ||
	    machine->ops->write_port(machine->context,
				     SYSTEM_CONTROL_A_TEST_PORT,
				     DOS_IO_WIDTH_8, 0u) != DOS_MACHINE_OK ||
	    machine->ops->query_a20(machine->context, &a20_enabled) !=
		    DOS_MACHINE_OK ||
	    a20_enabled || machine->a20_enabled)
		return 28;

	fail_video_grant = true;
	if (x86_guest_space_display_foreground_acquire(
		    MACHINE_IDENTITY, REQUESTER_IDENTITY, &token) !=
		    X86_GUEST_SPACE_PAGING_MISMATCH ||
	    token != 0x1111u || display_memory_is_user_accessible() ||
	    !resource_vga_io_is_closed() || !guest_vga_io_is_closed(machine))
		return 4;
	fail_video_grant = false;
	fail_video_grant_bit = 2u;
	token = 0x1212u;
	if (x86_guest_space_display_foreground_acquire(
		    MACHINE_IDENTITY, REQUESTER_IDENTITY, &token) !=
		    X86_GUEST_SPACE_PAGING_MISMATCH ||
	    token != 0x1212u || video_granted_ranges != 0u ||
	    !resource_vga_io_is_closed() || !guest_vga_io_is_closed(machine))
		return 23;
	fail_video_grant_bit = 0u;
	fail_video_grant_bit = 2u;
	fail_video_revoke = true;
	token = 0x1313u;
	if (x86_guest_space_display_foreground_acquire(
		    MACHINE_IDENTITY, REQUESTER_IDENTITY, &token) !=
		    X86_GUEST_SPACE_PAGING_MISMATCH ||
	    token != 0x1313u || video_granted_ranges != 1u ||
	    !resource_vga_io_is_closed() || !guest_vga_io_is_closed(machine) ||
	    x86_guest_space_display_foreground_acquire(
		    MACHINE_IDENTITY, REQUESTER_IDENTITY, &token) !=
		    X86_GUEST_SPACE_IO_DENIED)
		return 25;
	fail_video_grant_bit = 0u;
	fail_video_revoke = false;
	if (x86_guest_space_display_foreground_revoke(MACHINE_IDENTITY) !=
		    X86_GUEST_SPACE_OK ||
	    video_granted_ranges != 0u)
		return 26;

	token = 0x2222u;
	fail_video_grant = true;
	fail_io_revoke = true;
	if (x86_guest_space_display_foreground_acquire(
		    MACHINE_IDENTITY, REQUESTER_IDENTITY, &token) !=
		    X86_GUEST_SPACE_IO_FAULT ||
	    token != 0x2222u || display_memory_is_user_accessible() ||
	    !resource_vga_io_is_open() || !guest_vga_io_is_closed(machine) ||
	    x86_guest_space_display_foreground_acquire(
		    MACHINE_IDENTITY, REQUESTER_IDENTITY, &token) !=
		    X86_GUEST_SPACE_IO_DENIED)
		return 5;
	fail_io_revoke = false;
	if (x86_guest_space_display_foreground_revoke(MACHINE_IDENTITY) !=
		    X86_GUEST_SPACE_OK ||
	    !resource_vga_io_is_closed() || !guest_vga_io_is_closed(machine))
		return 6;
	fail_video_grant = false;
	if (x86_guest_space_display_foreground_acquire(
		    MACHINE_IDENTITY, REQUESTER_IDENTITY, &token) !=
		    X86_GUEST_SPACE_OK ||
	    token == X86_IO_FOREGROUND_TOKEN_INVALID ||
	    !display_memory_is_user_accessible())
		return 7;
	if (x86_guest_space_display_foreground_release(
		    OTHER_REQUESTER_IDENTITY, token) != X86_GUEST_SPACE_IO_DENIED ||
	    !display_memory_is_user_accessible())
		return 8;
	if (x86_guest_space_display_foreground_release(REQUESTER_IDENTITY,
						       token) !=
		    X86_GUEST_SPACE_OK ||
	    display_memory_is_user_accessible() ||
	    !resource_vga_io_is_closed() || !guest_vga_io_is_closed(machine))
		return 9;

	if (x86_guest_space_display_foreground_acquire(
		    MACHINE_IDENTITY, REQUESTER_IDENTITY, &next_token) !=
		    X86_GUEST_SPACE_OK ||
	    next_token == token)
		return 10;
	fail_io_release = true;
	if (x86_guest_space_display_foreground_release(
		    REQUESTER_IDENTITY, next_token) != X86_GUEST_SPACE_IO_FAULT ||
	    display_memory_is_user_accessible() ||
	    !resource_vga_io_is_open() || !guest_vga_io_is_closed(machine) ||
	    x86_guest_space_display_foreground_acquire(
		    MACHINE_IDENTITY, REQUESTER_IDENTITY, &token) !=
		    X86_GUEST_SPACE_IO_DENIED)
		return 11;
	fail_io_release = false;
	if (x86_guest_space_display_foreground_release(
		    REQUESTER_IDENTITY, next_token) != X86_GUEST_SPACE_OK ||
	    !resource_vga_io_is_closed() || !guest_vga_io_is_closed(machine))
		return 12;

	if (x86_guest_space_display_foreground_acquire(
		    MACHINE_IDENTITY, REQUESTER_IDENTITY, &next_token) !=
		    X86_GUEST_SPACE_OK)
		return 13;
	fail_video_revoke = true;
	if (x86_guest_space_display_foreground_release(
		    REQUESTER_IDENTITY, next_token) !=
		    X86_GUEST_SPACE_PAGING_MISMATCH ||
	    !display_memory_is_user_accessible() ||
	    !resource_vga_io_is_closed() || !guest_vga_io_is_closed(machine) ||
	    x86_guest_space_display_foreground_acquire(
		    MACHINE_IDENTITY, REQUESTER_IDENTITY, &token) !=
		    X86_GUEST_SPACE_IO_DENIED)
		return 14;
	fail_video_revoke = false;
	if (x86_guest_space_display_foreground_revoke(MACHINE_IDENTITY) !=
		    X86_GUEST_SPACE_OK ||
	    display_memory_is_user_accessible() ||
	    !resource_vga_io_is_closed() || !guest_vga_io_is_closed(machine))
		return 15;

	if (x86_guest_space_display_foreground_acquire(
		    MACHINE_IDENTITY, REQUESTER_IDENTITY, &next_token) !=
		    X86_GUEST_SPACE_OK)
		return 16;
	fail_io_revoke = true;
	if (x86_guest_space_display_foreground_revoke(MACHINE_IDENTITY) !=
		    X86_GUEST_SPACE_IO_FAULT ||
	    display_memory_is_user_accessible() ||
	    !resource_vga_io_is_open() || !guest_vga_io_is_closed(machine) ||
	    x86_guest_space_display_foreground_acquire(
		    MACHINE_IDENTITY, REQUESTER_IDENTITY, &token) !=
		    X86_GUEST_SPACE_IO_DENIED)
		return 17;
	fail_io_revoke = false;
	if (x86_guest_space_display_foreground_revoke(MACHINE_IDENTITY) !=
		    X86_GUEST_SPACE_OK ||
	    !resource_vga_io_is_closed() || !guest_vga_io_is_closed(machine))
		return 18;

	if (x86_guest_space_display_foreground_acquire(
		    MACHINE_IDENTITY, REQUESTER_IDENTITY, &next_token) !=
		    X86_GUEST_SPACE_OK)
		return 19;
	fail_video_revoke = true;
	if (x86_guest_space_display_foreground_revoke(MACHINE_IDENTITY) !=
		    X86_GUEST_SPACE_PAGING_MISMATCH ||
	    !display_memory_is_user_accessible() ||
	    !resource_vga_io_is_closed() || !guest_vga_io_is_closed(machine) ||
	    x86_guest_space_display_foreground_acquire(
		    MACHINE_IDENTITY, REQUESTER_IDENTITY, &token) !=
		    X86_GUEST_SPACE_IO_DENIED)
		return 20;
	fail_video_revoke = false;
	if (x86_guest_space_display_foreground_revoke(MACHINE_IDENTITY) !=
		    X86_GUEST_SPACE_OK ||
	    display_memory_is_user_accessible())
		return 27;
	stale_input_binding = input_binding;
	if (x86_guest_space_i8042_input_quiesce(MACHINE_IDENTITY,
					       &input_binding) !=
		    X86_GUEST_SPACE_OK ||
	    x86_guest_space_i8042_input_resume(MACHINE_IDENTITY,
					      &input_binding) !=
		    X86_GUEST_SPACE_OK ||
	    x86_guest_space_i8042_input_quiesce(MACHINE_IDENTITY,
					       &input_binding) !=
		    X86_GUEST_SPACE_OK ||
	    x86_guest_space_native_pit_quiesce(&pit_binding) !=
		    X86_GUEST_SPACE_OK ||
	    x86_guest_space_i8042_input_resume(MACHINE_IDENTITY,
					      &input_binding) !=
		    X86_GUEST_SPACE_INVALID_STATE ||
	    x86_guest_space_native_pit_unbind(&pit_binding) !=
		    X86_GUEST_SPACE_INVALID_STATE ||
	    x86_guest_space_i8042_input_unbind(MACHINE_IDENTITY,
					      &input_binding) !=
		    X86_GUEST_SPACE_OK ||
	    x86_guest_space_i8042_input_inject_keyboard(
		    &stale_input_binding, &translated_input_mode, 0x30u) !=
		    X86_GUEST_SPACE_INVALID_STATE ||
	    x86_guest_space_native_pit_resume(&pit_binding) !=
		    X86_GUEST_SPACE_OK ||
	    x86_guest_space_i8042_input_inject_keyboard(
		    &stale_input_binding, &translated_input_mode, 0x30u) !=
		    X86_GUEST_SPACE_STALE_BINDING)
		return 29;
	if (x86_guest_space_native_pit_quiesce(&pit_binding) !=
		    X86_GUEST_SPACE_OK ||
	    x86_guest_space_native_pit_submit(&pit_binding, 1193u) !=
		    X86_GUEST_SPACE_INVALID_STATE ||
	    x86_guest_space_native_pit_resume(&pit_binding) !=
		    X86_GUEST_SPACE_OK ||
	    x86_guest_space_native_pit_quiesce(&pit_binding) !=
		    X86_GUEST_SPACE_OK ||
	    x86_guest_space_native_pit_unbind(&pit_binding) !=
		    X86_GUEST_SPACE_OK ||
	    x86_guest_space_native_pit_submit(&pit_binding, 1193u) !=
		    X86_GUEST_SPACE_STALE_BINDING)
		return 30;
	stale_pit_binding = pit_binding;
	if (x86_guest_space_native_pit_bind(
		    MACHINE_IDENTITY, SECOND_INTERRUPT_SOURCE_IDENTITY, 1193u,
		    false, &pit_binding) != X86_GUEST_SPACE_OK ||
	    pit_binding.router_generation <=
		    stale_pit_binding.router_generation ||
	    x86_guest_space_native_pit_submit(&stale_pit_binding, 1193u) !=
		    X86_GUEST_SPACE_STALE_BINDING ||
	    x86_guest_space_native_pit_quiesce(&pit_binding) !=
		    X86_GUEST_SPACE_OK ||
	    x86_guest_space_native_pit_unbind(&pit_binding) !=
		    X86_GUEST_SPACE_OK)
		return 31;
	/* A poisoned published sink propagates to the whole guest-space owner. */
	if (x86_guest_space_native_pit_bind(
		    MACHINE_IDENTITY, INTERRUPT_SOURCE_IDENTITY, 1193u, false,
		    &pit_binding) != X86_GUEST_SPACE_OK ||
	    x86_legacy_chipset_poison(ADDRESS_SPACE_IDENTITY) !=
		    X86_LEGACY_CHIPSET_OK ||
	    x86_guest_space_native_pit_submit(&pit_binding, 1193u) !=
		    X86_GUEST_SPACE_DEVICE_FAULT ||
	    x86_guest_space_machine() != NULL)
		return 32;
	return 0;
#endif
}

DOSC32_TEST_ENTRY(run_tests)

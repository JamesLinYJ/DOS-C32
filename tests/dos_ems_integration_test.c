// SPDX-License-Identifier: GPL-2.0-only
/* Production composition tests for personality -> EMS -> x86 owner/device. */
#include "dos_ems_device.h"
#include "dos_personality.h"
#include "test_entry.h"
#include "x86_ems_config.h"
#include "x86_ems_memory.h"
#include "x86_guest_memory_runtime.h"

#define TEST_PERSONALITY_IDENTITY ((kernel_object_handle_t)0x701u)
#define TEST_MACHINE_IDENTITY ((kernel_object_handle_t)0x702u)
#define TEST_MACHINE_CONTEXT ((kernel_object_handle_t)0x703u)
#define TEST_RUNTIME_IDENTITY ((kernel_object_handle_t)0x704u)
#define TEST_MEMORY_OWNER ((kernel_object_handle_t)0x705u)
#define TEST_VCPI_CONTEXT ((kernel_object_handle_t)0x706u)
#define TEST_DEVICE_IDENTITY ((kernel_object_handle_t)0x707u)
#define TEST_DEVICE_CONTEXT ((kernel_object_handle_t)0x708u)
#define TEST_FRAME_CONTEXT ((kernel_object_handle_t)0x709u)
#define TEST_FRAME_LEASE ((dos_ems_page_frame_lease_t)0x70au)
#define TEST_PHYSICAL_ADDRESS 0x00800000u
#define TEST_MANAGED_PAGES 4096u

static const struct dos_int21_drive_config drive_config = {
	.available_drive_mask = (uint32_t)1u << 2u,
	.current_drive = 2u,
	.boot_drive = 3u,
	.last_drive = 3u,
	.reserved = 0u,
};

static const struct dos_machine_ops machine_ops = {0};
static struct dos_ems_runtime_config ems_config;
static struct x86_guest_memory_snapshot runtime_snapshot;
static enum x86_guest_memory_status query_status;
static x86_guest_memory_lease_t live_lease;
static kernel_object_handle_t live_owner;
static uint32_t live_pages;
static uint32_t allocate_calls;
static uint32_t release_calls;
static enum dos_vcpi_handoff_status configured_handoff_status;
static uint32_t handoff_calls;
static uint8_t master_pic_base;
static uint8_t slave_pic_base;
static enum dos_ems_page_frame_status configured_frame_acquire_status;
static enum dos_ems_page_frame_status configured_frame_release_status;
static uint32_t frame_acquire_calls;
static uint32_t frame_release_calls;
static uint8_t frame_lease_live;

/* Personality collaborators not involved in the EMS-specific test boundary. */
enum dos_xms_status dos_xms_construct(
	struct dos_xms_manager *manager, kernel_object_handle_t manager_identity)
{
	if (manager == NULL || manager_identity == 0u ||
	    manager_identity == KERNEL_OBJECT_HANDLE_INVALID)
		return DOS_XMS_INVALID_ARGUMENT;
	*manager = (struct dos_xms_manager){
		.identity = manager_identity,
		.constructed = 1u,
	};
	return DOS_XMS_READY;
}

enum dos_int21_status dos_int21_context_initialize(
	struct dos_int21_context *context, const struct dos_machine *machine,
	const struct dos_memory_arena *memory_arena,
	kernel_object_handle_t runtime_identity, uint16_t current_psp,
	const struct dos_int21_drive_config *config)
{
	if (context == NULL || machine == NULL || memory_arena == NULL ||
	    runtime_identity != TEST_RUNTIME_IDENTITY || current_psp == 0u ||
	    config == NULL)
		return DOS_INT21_INVALID_ARGUMENT;
	*context = (struct dos_int21_context){0};
	context->machine = *machine;
	context->memory_arena = *memory_arena;
	context->initialized = true;
	return DOS_INT21_HANDLED;
}

enum dos_int21_status dos_int21_dispatch(
	struct dos_int21_context *context, struct dos_cpu_state *registers)
{
	(void)context;
	(void)registers;
	return DOS_INT21_UNIMPLEMENTED;
}

enum dos_xms_status dos_xms_initialize(
	struct dos_xms_manager *manager, const struct dos_machine *machine,
	const struct dos_xms_memory_ops *ops,
	kernel_object_handle_t memory_context,
	const struct dos_xms_config *config)
{
	(void)manager;
	(void)machine;
	(void)ops;
	(void)memory_context;
	(void)config;
	return DOS_XMS_INVALID_ARGUMENT;
}

enum dos_xms_status dos_xms_multiplex(
	const struct dos_xms_manager *manager, struct dos_cpu_state *state)
{
	(void)manager;
	(void)state;
	return DOS_XMS_CHAIN;
}

enum dos_xms_status dos_xms_control(
	struct dos_xms_manager *manager, struct dos_machine *machine,
	struct dos_cpu_state *state)
{
	(void)manager;
	(void)machine;
	(void)state;
	return DOS_XMS_CHAIN;
}

enum x86_guest_memory_status x86_guest_memory_runtime_query_snapshot(
	struct x86_guest_memory_snapshot *snapshot)
{
	if (snapshot == NULL)
		return X86_GUEST_MEMORY_INVALID_ARGUMENT;
	if (query_status != X86_GUEST_MEMORY_OK)
		return query_status;
	*snapshot = runtime_snapshot;
	return X86_GUEST_MEMORY_OK;
}

enum x86_guest_memory_status x86_guest_memory_runtime_allocate(
	kernel_object_handle_t owner, uint32_t page_count,
	x86_guest_memory_lease_t *lease, uint32_t *physical_address)
{
	if (owner == 0u || owner == KERNEL_OBJECT_HANDLE_INVALID ||
	    page_count == 0u || lease == NULL || physical_address == NULL)
		return X86_GUEST_MEMORY_INVALID_ARGUMENT;
	if (live_lease != X86_GUEST_MEMORY_LEASE_INVALID ||
	    page_count > runtime_snapshot.total_free_pages)
		return X86_GUEST_MEMORY_NO_MEMORY;
	live_lease = 0x1001u;
	live_owner = owner;
	live_pages = page_count;
	runtime_snapshot.total_free_pages -= page_count;
	runtime_snapshot.largest_free_pages =
		runtime_snapshot.total_free_pages;
	++allocate_calls;
	*lease = live_lease;
	*physical_address = TEST_PHYSICAL_ADDRESS;
	return X86_GUEST_MEMORY_OK;
}

enum x86_guest_memory_status x86_guest_memory_runtime_inspect(
	x86_guest_memory_lease_t lease,
	struct x86_guest_memory_lease_info *info)
{
	if (lease != live_lease || info == NULL)
		return X86_GUEST_MEMORY_STALE_LEASE;
	*info = (struct x86_guest_memory_lease_info){
		.owner = live_owner,
		.physical_address = TEST_PHYSICAL_ADDRESS,
		.page_count = live_pages,
	};
	return X86_GUEST_MEMORY_OK;
}

enum x86_guest_memory_status x86_guest_memory_runtime_release(
	kernel_object_handle_t owner, x86_guest_memory_lease_t lease)
{
	if (lease != live_lease)
		return X86_GUEST_MEMORY_STALE_LEASE;
	if (owner != live_owner)
		return X86_GUEST_MEMORY_OWNER_MISMATCH;
	runtime_snapshot.total_free_pages += live_pages;
	runtime_snapshot.largest_free_pages =
		runtime_snapshot.total_free_pages;
	live_lease = X86_GUEST_MEMORY_LEASE_INVALID;
	live_owner = KERNEL_OBJECT_HANDLE_INVALID;
	live_pages = 0u;
	++release_calls;
	return X86_GUEST_MEMORY_OK;
}

static enum dos_vcpi_platform_status translate_low_page(
	kernel_object_handle_t context, uint16_t page,
	uint64_t *physical_address)
{
	if (context != TEST_VCPI_CONTEXT || physical_address == NULL)
		return DOS_VCPI_PLATFORM_FAULT;
	*physical_address = (uint64_t)page * DOS_EMS_NATIVE_PAGE_BYTES;
	return DOS_VCPI_PLATFORM_OK;
}

static enum dos_vcpi_platform_status read_virtual_cr0(
	kernel_object_handle_t context, uint32_t *virtual_cr0)
{
	if (context != TEST_VCPI_CONTEXT || virtual_cr0 == NULL)
		return DOS_VCPI_PLATFORM_FAULT;
	*virtual_cr0 = 0x11u;
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
	++frame_acquire_calls;
	if (context != TEST_FRAME_CONTEXT || linear_address != 0xe0000u ||
	    byte_count != 0x10000u || lease == NULL)
		return DOS_EMS_PAGE_FRAME_FAULT;
	if (configured_frame_acquire_status != DOS_EMS_PAGE_FRAME_OK)
		return configured_frame_acquire_status;
	if (frame_lease_live != 0u)
		return DOS_EMS_PAGE_FRAME_CONFLICT;
	frame_lease_live = 1u;
	*lease = TEST_FRAME_LEASE;
	return DOS_EMS_PAGE_FRAME_OK;
}

static enum dos_ems_page_frame_status frame_release(
	kernel_object_handle_t context, dos_ems_page_frame_lease_t lease)
{
	++frame_release_calls;
	if (context != TEST_FRAME_CONTEXT || lease != TEST_FRAME_LEASE ||
	    frame_lease_live == 0u)
		return DOS_EMS_PAGE_FRAME_FAULT;
	if (configured_frame_release_status != DOS_EMS_PAGE_FRAME_OK)
		return configured_frame_release_status;
	frame_lease_live = 0u;
	return DOS_EMS_PAGE_FRAME_OK;
}

static enum dos_ems_page_frame_status frame_map(
	kernel_object_handle_t context, dos_ems_page_frame_lease_t lease,
	uint8_t physical_page, uint64_t source_physical_address)
{
	return context == TEST_FRAME_CONTEXT && lease == TEST_FRAME_LEASE &&
		       physical_page < DOS_EMS_PAGE_FRAME_SLOTS &&
		       (source_physical_address &
			(DOS_EMS_NATIVE_PAGE_BYTES - 1u)) == 0u
		       ? DOS_EMS_PAGE_FRAME_OK
		       : DOS_EMS_PAGE_FRAME_FAULT;
}

static enum dos_ems_page_frame_status frame_unmap(
	kernel_object_handle_t context, dos_ems_page_frame_lease_t lease,
	uint8_t physical_page)
{
	return context == TEST_FRAME_CONTEXT && lease == TEST_FRAME_LEASE &&
		       physical_page < DOS_EMS_PAGE_FRAME_SLOTS
		       ? DOS_EMS_PAGE_FRAME_OK
		       : DOS_EMS_PAGE_FRAME_FAULT;
}

static enum dos_vcpi_handoff_status handoff(
	kernel_object_handle_t context,
	const struct dos_vcpi_handoff_request *request,
	struct dos_cpu_state *state)
{
	++handoff_calls;
	if (context != TEST_VCPI_CONTEXT || request == NULL || state == NULL)
		return DOS_VCPI_HANDOFF_FAULT;
	return configured_handoff_status;
}

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

static void reset_runtime(void)
{
	runtime_snapshot = (struct x86_guest_memory_snapshot){
		.largest_free_pages = TEST_MANAGED_PAGES,
		.total_free_pages = TEST_MANAGED_PAGES,
		.managed_pages = TEST_MANAGED_PAGES,
		.highest_address = TEST_PHYSICAL_ADDRESS +
				   (uint64_t)TEST_MANAGED_PAGES *
					   X86_GUEST_PAGE_BYTES -
				   1u,
	};
	query_status = X86_GUEST_MEMORY_OK;
	live_lease = X86_GUEST_MEMORY_LEASE_INVALID;
	live_owner = KERNEL_OBJECT_HANDLE_INVALID;
	live_pages = 0u;
	allocate_calls = 0u;
	release_calls = 0u;
	handoff_calls = 0u;
	master_pic_base = 0x08u;
	slave_pic_base = 0x70u;
	configured_frame_acquire_status = DOS_EMS_PAGE_FRAME_OK;
	configured_frame_release_status = DOS_EMS_PAGE_FRAME_OK;
	frame_acquire_calls = 0u;
	frame_release_calls = 0u;
	frame_lease_live = 0u;
	configured_handoff_status = DOS_VCPI_HANDOFF_UNSUPPORTED;
}

static bool initialize_base_personality(struct dos_personality *personality)
{
	const struct dos_machine machine = {
		.ops = &machine_ops,
		.context = TEST_MACHINE_CONTEXT,
		.address_limit = DOS_REAL_MODE_ADDRESS_LIMIT,
		.a20_enabled = false,
		.poisoned = 0u,
	};
	const struct dos_memory_arena arena = {0};

	return dos_personality_initialize(
		       personality, TEST_PERSONALITY_IDENTITY,
		       TEST_MACHINE_IDENTITY, &machine, &arena,
		       TEST_RUNTIME_IDENTITY, 0x1234u, &drive_config) ==
	       DOS_PERSONALITY_READY;
}

static bool initialize_personality(struct dos_personality *personality,
				   const struct dos_vcpi_platform_ops *platform_ops)
{
	if (!initialize_base_personality(personality))
		return false;
	return dos_personality_set_ems(
		       personality, x86_ems_memory_runtime_operations(),
		       TEST_MEMORY_OWNER, &frame_binding, platform_ops,
		       platform_ops != NULL ? TEST_VCPI_CONTEXT
					    : KERNEL_OBJECT_HANDLE_INVALID,
		       &ems_config) == DOS_PERSONALITY_READY;
}

static struct dos_interrupt_result dispatch_ems(
	struct dos_personality *personality, struct dos_cpu_state *state)
{
	return dos_personality_interrupt(
		personality, &personality->int21.machine,
		TEST_MACHINE_IDENTITY, 0x67u, state);
}

static int test_runtime_resolution(void)
{
	struct dos_ems_page_frame_ops missing_mapper = frame_ops;
	struct x86_ems_runtime_binding binding = {
		.config.service.page_frame_segment = 0xaaaau,
		.page_frame.lease = 0xbbbbu,
		.acquired = 0xccu,
	};

	reset_runtime();
	missing_mapper.map = NULL;
	if (x86_ems_runtime_config_resolve(
		    &missing_mapper, TEST_FRAME_CONTEXT, &binding) !=
		    X86_EMS_RUNTIME_CONFIG_UNAVAILABLE ||
	    binding.config.service.page_frame_segment != 0xaaaau ||
	    binding.page_frame.lease != 0xbbbbu || binding.acquired != 0xccu ||
	    frame_acquire_calls != 0u)
		return 1;
	configured_frame_acquire_status = DOS_EMS_PAGE_FRAME_CONFLICT;
	if (x86_ems_runtime_config_resolve(
		    &frame_ops, TEST_FRAME_CONTEXT, &binding) !=
		    X86_EMS_RUNTIME_CONFIG_CONFLICT ||
	    binding.config.service.page_frame_segment != 0xaaaau ||
	    binding.page_frame.lease != 0xbbbbu || binding.acquired != 0xccu ||
	    frame_acquire_calls != 1u || frame_lease_live != 0u)
		return 2;
	configured_frame_acquire_status = DOS_EMS_PAGE_FRAME_OK;
	binding = (struct x86_ems_runtime_binding){0};
	if (x86_ems_runtime_config_resolve(
		    &frame_ops, TEST_FRAME_CONTEXT, &binding) !=
		    X86_EMS_RUNTIME_CONFIG_READY ||
	    binding.acquired != 1u ||
	    binding.config.service.page_frame_segment != 0xe000u ||
	    binding.page_frame.linear_address != 0xe0000u ||
	    binding.page_frame.byte_count != 0x10000u ||
	    binding.page_frame.lease != TEST_FRAME_LEASE ||
	    frame_acquire_calls != 2u || frame_lease_live != 1u)
		return 3;
	if (x86_ems_runtime_config_release(&binding) !=
		    X86_EMS_RUNTIME_CONFIG_READY ||
	    binding.acquired != 0u || frame_release_calls != 1u ||
	    frame_lease_live != 0u)
		return 4;
	return 0;
}

static int test_configuration_and_service(void)
{
	struct dos_ems_runtime_config invalid = ems_config;
	struct dos_ems_runtime_config snapshot = {0};
	struct dos_personality personality = {0};
	struct dos_cpu_state state = {
		.eax = 0x4000u,
		.mode = (uint32_t)DOS_CPU_VM86,
	};
	struct dos_interrupt_result result;
	const struct dos_machine machine = {
		.ops = &machine_ops,
		.context = TEST_MACHINE_CONTEXT,
		.address_limit = DOS_REAL_MODE_ADDRESS_LIMIT,
	};
	const struct dos_memory_arena arena = {0};

	reset_runtime();
	if (!dos_ems_runtime_config_is_valid(&ems_config))
		return 1;
	invalid.device_name[0] = (uint8_t)'e';
	if (dos_ems_runtime_config_is_valid(&invalid))
		return 2;
	invalid = ems_config;
	invalid.reserved[0] = 1u;
	if (dos_ems_runtime_config_is_valid(&invalid))
		return 3;
	if (dos_personality_initialize(
		    &personality, TEST_PERSONALITY_IDENTITY,
		    TEST_MACHINE_IDENTITY, &machine, &arena,
		    TEST_RUNTIME_IDENTITY, 0x1234u, &drive_config) !=
	    DOS_PERSONALITY_READY)
		return 4;
	result = dispatch_ems(&personality, &state);
	if (result.disposition != DOS_INTERRUPT_CHAIN ||
	    result.machine_status != DOS_MACHINE_OK)
		return 5;
	if (dos_personality_set_ems(
		    &personality, x86_ems_memory_runtime_operations(),
		    TEST_MEMORY_OWNER, &frame_binding, NULL,
		    KERNEL_OBJECT_HANDLE_INVALID,
		    &ems_config) != DOS_PERSONALITY_READY ||
	    !dos_personality_ems_config_snapshot(&personality, &snapshot) ||
	    snapshot.service.page_frame_segment !=
		    ems_config.service.page_frame_segment ||
	    snapshot.device_name[7] != (uint8_t)'0')
		return 6;
	state.eax = 0x4000u;
	result = dispatch_ems(&personality, &state);
	if (result.disposition != DOS_INTERRUPT_HANDLED ||
	    result.machine_status != DOS_MACHINE_OK ||
	    dos_register_high8(state.eax) != 0u)
		return 7;
	/* VCPI stays unavailable without a protected-execution backend. */
	state.eax = 0xde00u;
	result = dispatch_ems(&personality, &state);
	if (result.disposition != DOS_INTERRUPT_HANDLED ||
	    dos_register_high8(state.eax) != 0x84u || handoff_calls != 0u)
		return 8;
	state.eax = 0x4400u;
	result = dispatch_ems(&personality, &state);
	if (result.disposition != DOS_INTERRUPT_HANDLED ||
	    dos_register_high8(state.eax) != 0x83u || allocate_calls != 0u)
		return 9;
	state.eax = 0x4300u;
	dos_register_set_low16(&state.ebx, 2u);
	result = dispatch_ems(&personality, &state);
	if (result.disposition != DOS_INTERRUPT_HANDLED ||
	    dos_register_high8(state.eax) != 0u ||
	    dos_register_low16(state.edx) != 1u || allocate_calls != 1u ||
	    live_owner != TEST_MEMORY_OWNER || live_pages != 8u)
		return 10;
	state.eax = 0x4500u;
	dos_register_set_low16(&state.edx, 1u);
	result = dispatch_ems(&personality, &state);
	if (result.disposition != DOS_INTERRUPT_HANDLED ||
	    dos_register_high8(state.eax) != 0u || release_calls != 1u ||
	    live_lease != X86_GUEST_MEMORY_LEASE_INVALID)
		return 11;
	if (dos_personality_set_ems(
		    &personality, x86_ems_memory_runtime_operations(),
		    TEST_MEMORY_OWNER, &frame_binding, NULL,
		    KERNEL_OBJECT_HANDLE_INVALID,
		    &ems_config) != DOS_PERSONALITY_INVALID_ARGUMENT)
		return 12;
	return 0;
}

static int test_fault_and_transfer_mapping(void)
{
	struct dos_personality personality = {0};
	struct dos_cpu_state state = {
		.eax = 0x4200u,
		.mode = (uint32_t)DOS_CPU_VM86,
	};
	struct dos_interrupt_result result;
	struct dos_ems_runtime_config snapshot = {0};

	reset_runtime();
	if (!initialize_personality(&personality, &vcpi_ops))
		return 1;
	query_status = X86_GUEST_MEMORY_INVALID_MAP;
	result = dispatch_ems(&personality, &state);
	if (result.disposition != DOS_INTERRUPT_MACHINE_FAULT ||
	    result.machine_status != DOS_MACHINE_IO_FAULT)
		return 2;
	query_status = X86_GUEST_MEMORY_OK;
	state.eax = 0xde0cu;
	state.esi = 0x1000u;
	configured_handoff_status = DOS_VCPI_HANDOFF_FAULT;
	result = dispatch_ems(&personality, &state);
	if (result.disposition != DOS_INTERRUPT_MACHINE_FAULT ||
	    result.machine_status != DOS_MACHINE_IO_FAULT ||
	    handoff_calls != 1u)
		return 3;
	configured_handoff_status = DOS_VCPI_HANDOFF_TRANSFERRED;
	result = dispatch_ems(&personality, &state);
	if (result.disposition != DOS_INTERRUPT_EXECUTION_TRANSFERRED ||
	    result.machine_status != DOS_MACHINE_OK || handoff_calls != 2u)
		return 4;
	configured_handoff_status = DOS_VCPI_HANDOFF_UNCERTAIN;
	result = dispatch_ems(&personality, &state);
	if (result.disposition != DOS_INTERRUPT_MACHINE_FAULT ||
	    result.machine_status != DOS_MACHINE_STOPPED ||
	    handoff_calls != 3u ||
	    dos_personality_ems_config_snapshot(&personality, &snapshot))
		return 5;
	reset_runtime();
	personality = (struct dos_personality){0};
	if (!initialize_personality(&personality, NULL))
		return 6;
	state.eax = 0x4000u;
	state.mode = 0xffffffffu;
	result = dispatch_ems(&personality, &state);
	if (result.disposition != DOS_INTERRUPT_MACHINE_FAULT ||
	    result.machine_status != DOS_MACHINE_INVALID_ARGUMENT)
		return 7;
	query_status = X86_GUEST_MEMORY_POISONED;
	state.eax = 0x4200u;
	state.mode = (uint32_t)DOS_CPU_VM86;
	result = dispatch_ems(&personality, &state);
	if (result.disposition != DOS_INTERRUPT_MACHINE_FAULT ||
	    result.machine_status != DOS_MACHINE_STOPPED)
		return 8;
	return 0;
}

static int test_named_device(void)
{
	struct dos_personality personality = {0};
	static struct dos_personality rollback_personality;
	struct x86_ems_runtime_binding rollback_binding = {0};
	const struct iomgr_device_name name = {
		.bytes = ems_config.device_name,
		.length = DOS_EMS_DEVICE_NAME_BYTES,
	};
	struct iomgr_device_info info;
	iomgr_device_registration_handle_t registration =
		IOMGR_DEVICE_REGISTRATION_HANDLE_INVALID;
	iomgr_device_registration_handle_t duplicate =
		IOMGR_DEVICE_REGISTRATION_HANDLE_INVALID;
	iomgr_device_registration_handle_t rollback_registration = 0xfeedu;
	iomgr_device_handle_t device = IOMGR_DEVICE_HANDLE_INVALID;
	struct dos_cpu_state state = {
		.eax = 0x4000u,
		.mode = (uint32_t)DOS_CPU_VM86,
	};
	struct dos_interrupt_result result;
	size_t completed = 0xa5u;
	uint8_t byte = 0u;
	size_t index;

	reset_runtime();
	rollback_personality = (struct dos_personality){0};
	if (!initialize_personality(&personality, NULL))
		return 1;
	if (dos_ems_device_register(
		    &personality, TEST_DEVICE_IDENTITY, TEST_DEVICE_CONTEXT,
		    &registration) != IOMGR_NOT_INITIALIZED ||
	    iomgr_device_initialize() != IOMGR_OK)
		return 2;
	if (dos_ems_device_register(
		    &personality, TEST_DEVICE_IDENTITY, TEST_DEVICE_CONTEXT,
		    &registration) != IOMGR_OK ||
	    registration == IOMGR_DEVICE_REGISTRATION_HANDLE_INVALID)
		return 3;
	if (!initialize_base_personality(&rollback_personality) ||
	    x86_ems_runtime_config_resolve(
		    &frame_ops, TEST_FRAME_CONTEXT, &rollback_binding) !=
		    X86_EMS_RUNTIME_CONFIG_READY ||
	    dos_ems_runtime_publish(
		    &rollback_personality,
		    x86_ems_memory_runtime_operations(), TEST_MEMORY_OWNER,
		    &rollback_binding.page_frame, NULL,
		    KERNEL_OBJECT_HANDLE_INVALID, &rollback_binding.config,
		    TEST_DEVICE_IDENTITY + 0x10u,
		    TEST_DEVICE_CONTEXT + 0x10u, &rollback_registration) !=
		    DOS_EMS_PUBLICATION_CONFLICT ||
	    rollback_registration != 0xfeedu ||
	    rollback_personality.ems.initialized != 0u ||
	    frame_lease_live != 1u)
		return 4;
	result = dispatch_ems(&rollback_personality, &state);
	if (result.disposition != DOS_INTERRUPT_CHAIN ||
	    result.machine_status != DOS_MACHINE_OK ||
	    x86_ems_runtime_config_release(&rollback_binding) !=
		    X86_EMS_RUNTIME_CONFIG_READY ||
	    frame_lease_live != 0u || frame_release_calls != 1u)
		return 5;
	if (dos_ems_device_register(
		    &personality, TEST_DEVICE_IDENTITY, TEST_DEVICE_CONTEXT,
		    &duplicate) != IOMGR_ALREADY_EXISTS)
		return 6;
	if (iomgr_device_open(&name, &device) != IOMGR_OK ||
	    device == IOMGR_DEVICE_HANDLE_INVALID ||
	    iomgr_device_query_info(device, &info) != IOMGR_OK ||
	    info.identity != TEST_DEVICE_IDENTITY || info.capabilities != 0u ||
	    info.state != 0u || info.pending_read_bytes != 0u ||
	    info.name_length != DOS_EMS_DEVICE_NAME_BYTES)
		return 7;
	for (index = 0u; index < DOS_EMS_DEVICE_NAME_BYTES; ++index) {
		if (info.name[index] != ems_config.device_name[index])
			return 8;
	}
	if (iomgr_device_read(device, &byte, sizeof(byte), sizeof(byte),
			      &completed) != IOMGR_UNSUPPORTED ||
	    completed != 0xa5u ||
	    iomgr_device_control(device, 0u, NULL, 0u, 0u, NULL, 0u,
				 &completed) != IOMGR_UNSUPPORTED ||
	    iomgr_device_unregister(registration) != IOMGR_BUSY)
		return 9;
	if (iomgr_device_close(device) != IOMGR_OK ||
	    iomgr_device_unregister(registration) != IOMGR_OK ||
	    iomgr_device_open(&name, &device) != IOMGR_NOT_FOUND)
		return 10;
	return 0;
}

int dos_ems_integration_test_run(void);

int dos_ems_integration_test_run(void)
{
	int status;

	if (!x86_ems_runtime_config_candidate(&ems_config))
		return 1;
	status = test_runtime_resolution();
	if (status != 0)
		return 5 + status;
	status = test_configuration_and_service();
	if (status != 0)
		return 10 + status;
	status = test_fault_and_transfer_mapping();
	if (status != 0)
		return 30 + status;
	status = test_named_device();
	if (status != 0)
		return 50 + status;
	return 0;
}

#if !defined(DOSC32_QEMU_SYSTEM_TEST)
DOSC32_TEST_ENTRY(dos_ems_integration_test_run)
#endif

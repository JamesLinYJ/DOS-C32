// SPDX-License-Identifier: GPL-2.0-only
/* Generation-bound x86 guest address space with explicit lifetime proof. */
#include "x86_guest_space.h"

#include "../../config/x86-guest-space.h"
#include "address.h"
#include "x86_display.h"
#include "x86_guest_irq_router.h"
#include "x86_i8042.h"
#include "x86_io_resource.h"
#include "x86_legacy_bios.h"
#include "x86_legacy_chipset.h"
#include "memory/firmware_shadow.h"

#define X86_GUEST_SPACE_FIRST_GENERATION 1u
#define X86_PARAGRAPHS_PER_KIB 64u
#define X86_SYSTEM_CONTROL_A_PORT 0x0092u
#define X86_SYSTEM_CONTROL_A_RESET 0x01u
#define X86_SYSTEM_CONTROL_A_A20 0x02u
#define X86_PLATFORM_BASE_IO_RESOURCE_COUNT 1u
#define X86_PLATFORM_IO_RESOURCE_COUNT                                      \
	(X86_PLATFORM_BASE_IO_RESOURCE_COUNT + 1u +                         \
	 X86_LEGACY_CHIPSET_RESOURCE_COUNT + X86_I8042_RESOURCE_COUNT)

static_assert_expression(X86_REAL_MODE_LINEAR_LIMIT ==
				 DOS_REAL_MODE_ADDRESS_LIMIT,
			 "x86 HMA mapping must match the DOS address model");
static_assert_expression(CONFIG_X86_GUEST_IRQ_PRODUCER_CAPACITY >= 2u,
			 "guest IRQ topology needs PIT and i8042 producers");
static_assert_expression(CONFIG_X86_GUEST_IRQ_ROUTE_CAPACITY >= 1u,
			 "guest IRQ topology needs the explicit PIT route");
static_assert_expression(CONFIG_X86_GUEST_DEVICE_EVENT_PUMP_BUDGET > 0u,
			 "guest event pump budget must make progress");

struct x86_guest_space_owner {
	struct dos_machine machine;
	struct x86_paging_binding paging;
	struct x86_guest_irq_router irq_router;
	struct x86_guest_irq_producer_slot
		irq_producers[CONFIG_X86_GUEST_IRQ_PRODUCER_CAPACITY];
	struct x86_guest_irq_route_slot
		irq_routes[CONFIG_X86_GUEST_IRQ_ROUTE_CAPACITY];
	struct x86_guest_irq_producer_binding pit_producer;
	struct x86_guest_irq_producer_binding i8042_irq_producer;
	struct x86_guest_irq_route_binding pit_route;
	kernel_object_handle_t address_space_identity;
	kernel_object_handle_t machine_identity;
	kernel_object_handle_t irq_router_identity;
	kernel_object_handle_t i8042_irq_producer_identity;
	kernel_object_handle_t io_requester_identity;
	x86_io_resource_handle_t display_resource;
	x86_io_foreground_token_t display_foreground_token;
	struct x86_display_capability display;
	uint64_t generation;
	uint64_t i8042_generation;
	uint8_t initialized;
	uint8_t virtual_system_control_a;
	uint8_t display_available;
	uint8_t display_memory_granted;
	uint8_t display_cleanup_required;
	uint8_t lifecycle_started;
	uint8_t irq_router_published;
	uint8_t event_pump_active;
	uint8_t poisoned;
};

static struct x86_guest_space_owner owner;

static bool identity_is_valid(kernel_object_handle_t identity)
{
	return identity != 0u && identity != KERNEL_OBJECT_HANDLE_INVALID;
}

static bool reserved_is_zero(const uint8_t *bytes, size_t count)
{
	size_t index;

	for (index = 0u; index < count; ++index) {
		if (bytes[index] != 0u)
			return false;
	}
	return true;
}

static bool identities_are_distinct(
	const struct x86_guest_space_config *config)
{
	const kernel_object_handle_t identities[] = {
		config->address_space_identity,
		config->machine_identity,
		config->irq_router_identity,
		config->i8042_irq_producer_identity,
	};
	size_t left;
	size_t right;

	for (left = 0u; left < ARRAY_SIZE(identities); ++left) {
		if (!identity_is_valid(identities[left]))
			return false;
		for (right = left + 1u; right < ARRAY_SIZE(identities); ++right) {
			if (identities[left] == identities[right])
				return false;
		}
	}
	return true;
}

static bool guest_space_config_is_valid(
	const struct x86_guest_space_config *config)
{
	return config != NULL && identities_are_distinct(config) &&
	       reserved_is_zero(config->reserved, ARRAY_SIZE(config->reserved));
}

static bool guest_space_is_active(void)
{
	return owner.initialized == 1u && owner.poisoned == 0u &&
	       owner.machine.poisoned == 0u;
}

static bool context_matches(kernel_object_handle_t context)
{
	return guest_space_is_active() &&
	       context == owner.address_space_identity;
}

static kernel_object_handle_t display_io_requester(void)
{
	if (owner.display_available != 1u ||
	    owner.display_memory_granted != 1u ||
	    owner.display_cleanup_required != 0u ||
	    owner.display_foreground_token == X86_IO_FOREGROUND_TOKEN_INVALID)
		return KERNEL_OBJECT_HANDLE_INVALID;
	return owner.io_requester_identity;
}

#if defined(DOSC32_HOST_TEST)
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
#endif

static enum x86_io_resource_status foreground_acquire(
	x86_io_resource_handle_t resource,
	kernel_object_handle_t owner_identity,
	kernel_object_handle_t requester_identity,
	x86_io_foreground_token_t *token)
{
#if defined(DOSC32_HOST_TEST)
	return x86_guest_space_test_foreground_acquire(
		resource, owner_identity, requester_identity, token);
#else
	return x86_io_resource_foreground_acquire(
		resource, owner_identity, requester_identity, token);
#endif
}

static enum x86_io_resource_status foreground_release(
	x86_io_foreground_token_t token,
	kernel_object_handle_t requester_identity)
{
#if defined(DOSC32_HOST_TEST)
	return x86_guest_space_test_foreground_release(token,
						       requester_identity);
#else
	return x86_io_resource_foreground_release(token, requester_identity);
#endif
}

static enum x86_io_resource_status foreground_revoke(
	x86_io_resource_handle_t resource,
	kernel_object_handle_t owner_identity)
{
#if defined(DOSC32_HOST_TEST)
	return x86_guest_space_test_foreground_revoke(resource,
						      owner_identity);
#else
	return x86_io_resource_foreground_revoke(resource, owner_identity);
#endif
}

static void identity_copy_from(void *destination, uintptr_t source,
			       size_t count)
{
#if defined(DOSC32_HOST_TEST)
	uint8_t *output = destination;
	const uint8_t *input = (const uint8_t *)source;
	size_t index;

	for (index = 0u; index < count; ++index)
		output[index] = input[index];
#else
	__asm__ volatile("cld; rep movsb"
			 : "+D"(destination), "+S"(source), "+c"(count)
			 :
			 : "memory");
#endif
}

static void identity_copy_to(uintptr_t destination, const void *source,
			     size_t count)
{
#if defined(DOSC32_HOST_TEST)
	uint8_t *output = (uint8_t *)destination;
	const uint8_t *input = source;
	size_t index;

	for (index = 0u; index < count; ++index)
		output[index] = input[index];
#else
	__asm__ volatile("cld; rep movsb"
			 : "+D"(destination), "+S"(source), "+c"(count)
			 :
			 : "memory");
#endif
}

#if defined(DOSC32_HOST_TEST)
bool x86_guest_space_test_physical_map(
	uint32_t physical_address, size_t count, bool writable,
	struct native_mapping *mapping);
#endif

static bool guest_physical_map(uint32_t physical_address, size_t count,
			       bool writable, struct native_mapping *mapping)
{
#if defined(DOSC32_HOST_TEST)
	return x86_guest_space_test_physical_map(
		physical_address, count, writable, mapping);
#else
	(void)writable;
	return kernel_address_identity_map(physical_address, count, mapping) ==
		       ADDRESS_OK &&
	       mapping->length == count;
#endif
}

static bool guest_range_translates(uint32_t address, size_t count,
				   bool writable)
{
	uint32_t cursor = address;
	size_t remaining = count;

	while (remaining > 0u) {
		struct x86_paging_guest_translation translation;
		size_t chunk;

		if (!x86_firmware_shadow_translate(cursor, writable,
						   &translation) ||
		    translation.contiguous_bytes == 0u)
			return false;
		chunk = translation.contiguous_bytes;
		if (chunk > remaining)
			chunk = remaining;
		cursor += (uint32_t)chunk;
		remaining -= chunk;
	}
	return true;
}

static enum dos_machine_status identity_read(
	kernel_object_handle_t context, dos_linear_address_t address,
	void *destination, size_t destination_capacity, size_t count)
{
	uint8_t *output = destination;
	uint32_t cursor = address;
	size_t remaining = count;

	if (!context_matches(context) ||
	    address >= owner.paging.guest_linear_limit ||
	    count > (size_t)(owner.paging.guest_linear_limit - address))
		return DOS_MACHINE_ADDRESS_FAULT;
	if ((destination == NULL && count != 0u) || count > destination_capacity)
		return DOS_MACHINE_IO_FAULT;
	if (!x86_paging_binding_is_active(&owner.paging) ||
	    !guest_range_translates(address, count, false))
		return DOS_MACHINE_ADDRESS_FAULT;
	while (remaining > 0u) {
		struct x86_paging_guest_translation translation;
		struct native_mapping mapping;
		size_t chunk;

		if (!x86_firmware_shadow_translate(cursor, false, &translation) ||
		    translation.contiguous_bytes == 0u)
			return DOS_MACHINE_ADDRESS_FAULT;
		chunk = translation.contiguous_bytes;
		if (chunk > remaining)
			chunk = remaining;
		if (!guest_physical_map(translation.physical_address, chunk, false,
					&mapping))
			return DOS_MACHINE_ADDRESS_FAULT;
		identity_copy_from(output, (uintptr_t)mapping.pointer, chunk);
		output += chunk;
		cursor += (uint32_t)chunk;
		remaining -= chunk;
	}
	return DOS_MACHINE_OK;
}

static enum dos_machine_status identity_write(
	kernel_object_handle_t context, dos_linear_address_t address,
	const void *source, size_t source_capacity, size_t count)
{
	const uint8_t *input = source;
	uint32_t cursor = address;
	size_t remaining = count;

	if (!context_matches(context) ||
	    address >= owner.paging.guest_linear_limit ||
	    count > (size_t)(owner.paging.guest_linear_limit - address))
		return DOS_MACHINE_ADDRESS_FAULT;
	if ((source == NULL && count != 0u) || count > source_capacity)
		return DOS_MACHINE_IO_FAULT;
	if (!x86_paging_binding_is_active(&owner.paging) ||
	    !guest_range_translates(address, count, true))
		return DOS_MACHINE_ADDRESS_FAULT;
	while (remaining > 0u) {
		struct x86_paging_guest_translation translation;
		struct native_mapping mapping;
		size_t chunk;

		if (!x86_firmware_shadow_translate(cursor, true, &translation) ||
		    translation.contiguous_bytes == 0u)
			return DOS_MACHINE_ADDRESS_FAULT;
		chunk = translation.contiguous_bytes;
		if (chunk > remaining)
			chunk = remaining;
		if (!guest_physical_map(translation.physical_address, chunk, true,
					&mapping))
			return DOS_MACHINE_ADDRESS_FAULT;
		identity_copy_to((uintptr_t)mapping.pointer, input, chunk);
		input += chunk;
		cursor += (uint32_t)chunk;
		remaining -= chunk;
	}
	return DOS_MACHINE_OK;
}

static enum dos_machine_status machine_io_status(
	enum x86_io_resource_status status)
{
	if (status == X86_IO_RESOURCE_OK)
		return DOS_MACHINE_OK;
	if (status == X86_IO_RESOURCE_INVALID_ARGUMENT)
		return DOS_MACHINE_INVALID_ARGUMENT;
	if (status == X86_IO_RESOURCE_ACCESS_DENIED ||
	    status == X86_IO_RESOURCE_OWNERSHIP_DENIED)
		return DOS_MACHINE_IO_DENIED;
	return DOS_MACHINE_IO_FAULT;
}

static enum x86_io_callback_status system_control_a_read(
	kernel_object_handle_t context, uint16_t port, enum dos_io_width width,
	uint32_t *value)
{
	if (!context_matches(context) || port != X86_SYSTEM_CONTROL_A_PORT ||
	    width != DOS_IO_WIDTH_8 || value == NULL)
		return X86_IO_CALLBACK_FAULT;
	*value = (uint8_t)(
		owner.virtual_system_control_a |
		(owner.machine.a20_enabled ? X86_SYSTEM_CONTROL_A_A20 : 0u));
	return X86_IO_CALLBACK_OK;
}

static enum x86_io_callback_status system_control_a_write(
	kernel_object_handle_t context, uint16_t port, enum dos_io_width width,
	uint32_t value)
{
	uint8_t requested;

	if (!context_matches(context) || port != X86_SYSTEM_CONTROL_A_PORT ||
	    width != DOS_IO_WIDTH_8 || value > 0xffu)
		return X86_IO_CALLBACK_FAULT;
	requested = (uint8_t)value;
	/* Reset is a host operation.  Bit 1 changes only the guest transform. */
	if ((requested & X86_SYSTEM_CONTROL_A_RESET) != 0u)
		return X86_IO_CALLBACK_DENIED;
	owner.virtual_system_control_a =
		(uint8_t)(requested & (uint8_t)~X86_SYSTEM_CONTROL_A_A20);
	owner.machine.a20_enabled =
		(requested & X86_SYSTEM_CONTROL_A_A20) != 0u;
	return X86_IO_CALLBACK_OK;
}

static enum x86_io_callback_status native_port_read(
	kernel_object_handle_t context, uint16_t port, enum dos_io_width width,
	uint32_t *value)
{
#if defined(DOSC32_HOST_TEST)
	(void)context;
	(void)port;
	(void)width;
	(void)value;
	return X86_IO_CALLBACK_FAULT;
#else
	uint8_t value8;
	uint16_t value16;
	uint32_t value32;

	if (!context_matches(context) || value == NULL)
		return X86_IO_CALLBACK_FAULT;
	switch (width) {
	case DOS_IO_WIDTH_8:
		__asm__ volatile("inb %w1, %b0" : "=a"(value8) : "Nd"(port));
		*value = value8;
		return X86_IO_CALLBACK_OK;
	case DOS_IO_WIDTH_16:
		__asm__ volatile("inw %w1, %w0" : "=a"(value16) : "Nd"(port));
		*value = value16;
		return X86_IO_CALLBACK_OK;
	case DOS_IO_WIDTH_32:
		__asm__ volatile("inl %w1, %0" : "=a"(value32) : "Nd"(port));
		*value = value32;
		return X86_IO_CALLBACK_OK;
	}
	return X86_IO_CALLBACK_FAULT;
#endif
}

static enum x86_io_callback_status native_port_write(
	kernel_object_handle_t context, uint16_t port, enum dos_io_width width,
	uint32_t value)
{
#if defined(DOSC32_HOST_TEST)
	(void)context;
	(void)port;
	(void)width;
	(void)value;
	return X86_IO_CALLBACK_FAULT;
#else
	if (!context_matches(context))
		return X86_IO_CALLBACK_FAULT;
	switch (width) {
	case DOS_IO_WIDTH_8:
		__asm__ volatile("outb %b0, %w1"
				 :
				 : "a"((uint8_t)value), "Nd"(port));
		return X86_IO_CALLBACK_OK;
	case DOS_IO_WIDTH_16:
		__asm__ volatile("outw %w0, %w1"
				 :
				 : "a"((uint16_t)value), "Nd"(port));
		return X86_IO_CALLBACK_OK;
	case DOS_IO_WIDTH_32:
		__asm__ volatile("outl %0, %w1" : : "a"(value), "Nd"(port));
		return X86_IO_CALLBACK_OK;
	}
	return X86_IO_CALLBACK_FAULT;
#endif
}

static enum x86_guest_space_status pump_guest_device_events(
	size_t budget, size_t *processed);

static enum dos_machine_status registered_port_read(
	kernel_object_handle_t context, uint16_t port, enum dos_io_width width,
	uint32_t *value)
{
	enum dos_machine_status machine_status;
	enum x86_guest_space_status pump_status;
	enum x86_io_resource_status status;
	size_t processed;

	if (!context_matches(context) || value == NULL)
		return DOS_MACHINE_IO_FAULT;
	status = x86_io_resource_read(display_io_requester(), port, width,
				      value);
	machine_status = machine_io_status(status);
	if (machine_status != DOS_MACHINE_OK)
		return machine_status;
	pump_status = pump_guest_device_events(
		CONFIG_X86_GUEST_DEVICE_EVENT_PUMP_BUDGET, &processed);
	if (pump_status == X86_GUEST_SPACE_OK ||
	    pump_status == X86_GUEST_SPACE_DEVICE_EVENT_RETRY)
		return DOS_MACHINE_OK;
	return DOS_MACHINE_IO_FAULT;
}

static enum dos_machine_status registered_port_write(
	kernel_object_handle_t context, uint16_t port, enum dos_io_width width,
	uint32_t value)
{
	enum dos_machine_status machine_status;
	enum x86_guest_space_status pump_status;
	size_t processed;

	if (!context_matches(context))
		return DOS_MACHINE_IO_FAULT;
	machine_status = machine_io_status(x86_io_resource_write(
		display_io_requester(), port, width, value));
	if (machine_status != DOS_MACHINE_OK)
		return machine_status;
	pump_status = pump_guest_device_events(
		CONFIG_X86_GUEST_DEVICE_EVENT_PUMP_BUDGET, &processed);
	if (pump_status == X86_GUEST_SPACE_OK ||
	    pump_status == X86_GUEST_SPACE_DEVICE_EVENT_RETRY)
		return DOS_MACHINE_OK;
	return DOS_MACHINE_IO_FAULT;
}

static enum dos_machine_status virtual_set_a20(
	kernel_object_handle_t context, bool enabled)
{
	if (!context_matches(context))
		return DOS_MACHINE_IO_FAULT;
	owner.machine.a20_enabled = enabled;
	return DOS_MACHINE_OK;
}

static enum dos_machine_status virtual_query_a20(
	kernel_object_handle_t context, bool *enabled)
{
	if (!context_matches(context) || enabled == NULL)
		return DOS_MACHINE_IO_FAULT;
	*enabled = owner.machine.a20_enabled;
	return DOS_MACHINE_OK;
}

static bool unregister_platform_resources(
	const x86_io_resource_handle_t *resources, size_t count,
	kernel_object_handle_t machine_identity)
{
	bool complete = true;

	while (count > 0u) {
		--count;
		if (x86_io_resource_unregister(resources[count], machine_identity) !=
		    X86_IO_RESOURCE_OK)
			complete = false;
	}
	return complete;
}

static void poison_platform_devices(
	kernel_object_handle_t address_space_identity)
{
	enum x86_legacy_chipset_status chipset_status =
		x86_legacy_chipset_poison(address_space_identity);
	enum x86_i8042_status i8042_status =
		x86_i8042_poison(address_space_identity);

	(void)chipset_status;
	(void)i8042_status;
}

static bool abort_prepared_platform_devices(
	kernel_object_handle_t address_space_identity, bool i8042_prepared,
	bool chipset_prepared)
{
	bool complete = true;

	if (i8042_prepared &&
	    x86_i8042_abort(address_space_identity) != X86_I8042_OK)
		complete = false;
	if (chipset_prepared &&
	    x86_legacy_chipset_abort(address_space_identity) !=
		    X86_LEGACY_CHIPSET_OK)
		complete = false;
	if (!complete)
		poison_platform_devices(address_space_identity);
	return complete;
}

static bool register_platform_io_resources(
	kernel_object_handle_t address_space_identity,
	kernel_object_handle_t machine_identity,
	const struct x86_i8042_config *i8042_config,
	x86_io_resource_handle_t *display_resource,
	struct x86_display_capability *display,
	bool *display_available)
{
	struct x86_legacy_chipset_config chipset_config;
	struct x86_legacy_bios_snapshot platform;
	struct x86_io_resource_descriptor
		descriptors[X86_PLATFORM_IO_RESOURCE_COUNT] = {
			{
				.owner_identity = machine_identity,
				.callback_context = address_space_identity,
				.read = system_control_a_read,
				.write = system_control_a_write,
				.first_port = X86_SYSTEM_CONTROL_A_PORT,
				.last_port = X86_SYSTEM_CONTROL_A_PORT,
				.read_width_mask = X86_IO_WIDTH_MASK_8,
				.write_width_mask = X86_IO_WIDTH_MASK_8,
				.read_action = X86_IO_RESOURCE_ACTION_EMULATE,
				.write_action = X86_IO_RESOURCE_ACTION_EMULATE,
			},
		};
	x86_io_resource_handle_t resources[X86_PLATFORM_IO_RESOURCE_COUNT];
	struct x86_display_capability prepared_display = {0};
	enum x86_legacy_chipset_status chipset_status;
	enum x86_display_status display_status;
	enum x86_i8042_status i8042_status;
	enum x86_io_resource_status resource_status;
	size_t chipset_index;
	size_t descriptor_count = X86_PLATFORM_BASE_IO_RESOURCE_COUNT;
	size_t display_index = 0u;
	size_t i8042_index;

	if (i8042_config == NULL || display_resource == NULL || display == NULL ||
	    display_available == NULL || !x86_legacy_bios_snapshot(&platform))
		return false;
	display_status = x86_display_capability_prepare(&platform,
							&prepared_display);
	if (display_status == X86_DISPLAY_INVALID_ARGUMENT ||
	    display_status == X86_DISPLAY_INVALID_PLATFORM)
		return false;
	if (display_status == X86_DISPLAY_OK) {
		display_index = descriptor_count++;
		descriptors[display_index] =
			(struct x86_io_resource_descriptor){
				.owner_identity = machine_identity,
				.callback_context = address_space_identity,
				.read = native_port_read,
				.write = native_port_write,
				.first_port = prepared_display.io_first_port,
				.last_port = prepared_display.io_last_port,
				.read_width_mask = X86_IO_WIDTH_MASK_ALL,
				.write_width_mask = X86_IO_WIDTH_MASK_ALL,
				.read_action = X86_IO_RESOURCE_ACTION_NATIVE,
				.write_action = X86_IO_RESOURCE_ACTION_NATIVE,
				.flags = X86_IO_RESOURCE_FLAG_FOREGROUND,
			};
	}
	chipset_status = x86_legacy_chipset_policy_config(&platform,
							 &chipset_config);
	if (chipset_status != X86_LEGACY_CHIPSET_OK)
		return false;
	chipset_index = descriptor_count;
	chipset_status = x86_legacy_chipset_prepare(
		address_space_identity, machine_identity, &chipset_config,
		&descriptors[chipset_index],
		X86_LEGACY_CHIPSET_RESOURCE_COUNT);
	if (chipset_status != X86_LEGACY_CHIPSET_OK)
		return false;
	descriptor_count += X86_LEGACY_CHIPSET_RESOURCE_COUNT;
	i8042_index = descriptor_count;
	i8042_status = x86_i8042_prepare(
		address_space_identity, machine_identity, i8042_config,
		&descriptors[i8042_index], X86_I8042_RESOURCE_COUNT);
	if (i8042_status != X86_I8042_OK) {
		(void)abort_prepared_platform_devices(address_space_identity, false,
						      true);
		return false;
	}
	descriptor_count += X86_I8042_RESOURCE_COUNT;
	resource_status = x86_io_resource_register_batch(
		descriptors, descriptor_count, resources,
		ARRAY_SIZE(resources));
	if (resource_status != X86_IO_RESOURCE_OK) {
		(void)abort_prepared_platform_devices(address_space_identity, true,
						      true);
		return false;
	}
	chipset_status = x86_legacy_chipset_publish(address_space_identity);
	if (chipset_status != X86_LEGACY_CHIPSET_OK) {
		bool resources_released = unregister_platform_resources(
			resources, descriptor_count, machine_identity);
		bool devices_aborted = abort_prepared_platform_devices(
			address_space_identity, true, true);

		if (!resources_released || !devices_aborted)
			poison_platform_devices(address_space_identity);
		return false;
	}
	i8042_status = x86_i8042_publish(address_space_identity);
	if (i8042_status != X86_I8042_OK) {
		(void)unregister_platform_resources(resources, descriptor_count,
						    machine_identity);
		/* The chipset was already published; no exact retire exists here. */
		poison_platform_devices(address_space_identity);
		return false;
	}
	*display_resource = display_status == X86_DISPLAY_OK
				    ? resources[display_index]
				    : X86_IO_RESOURCE_HANDLE_INVALID;
	*display = prepared_display;
	*display_available = display_status == X86_DISPLAY_OK;
	return true;
}

static const struct dos_machine_ops guest_machine_ops = {
	.read_memory = identity_read,
	.write_memory = identity_write,
	.read_port = registered_port_read,
	.write_port = registered_port_write,
	.set_a20 = virtual_set_a20,
	.query_a20 = virtual_query_a20,
};

enum x86_guest_space_status x86_guest_space_initialize(
	const struct x86_guest_space_config *config)
{
	struct x86_i8042_snapshot i8042_snapshot;
	struct x86_paging_binding paging;
	struct dos_machine machine;
	struct x86_display_capability display;
	x86_io_resource_handle_t display_resource;
	enum x86_guest_irq_router_status router_status;
	bool display_available;
	bool initial_a20;

	if (!guest_space_config_is_valid(config))
		return X86_GUEST_SPACE_INVALID_ARGUMENT;
	if (owner.lifecycle_started != 0u || owner.initialized != 0u ||
	    owner.poisoned != 0u)
		return X86_GUEST_SPACE_INVALID_STATE;
	if (!x86_paging_snapshot(&paging))
		return X86_GUEST_SPACE_PAGING_MISMATCH;
	initial_a20 = (config->i8042.output_port &
		       X86_I8042_OUTPUT_PORT_A20) != 0u;
	if (dos_machine_configure(&machine, &guest_machine_ops,
				  config->address_space_identity,
				  paging.guest_linear_limit, initial_a20) !=
	    DOS_MACHINE_OK)
		return X86_GUEST_SPACE_INVALID_STATE;
	if (x86_firmware_shadow_initialize(
		    config->address_space_identity, config->machine_identity,
		    X86_GUEST_SPACE_FIRST_GENERATION, &paging) !=
	    X86_FIRMWARE_SHADOW_OK)
		return X86_GUEST_SPACE_INVALID_STATE;

	owner.lifecycle_started = 1u;
	owner.address_space_identity = config->address_space_identity;
	owner.machine_identity = config->machine_identity;
	owner.irq_router_identity = config->irq_router_identity;
	owner.i8042_irq_producer_identity =
		config->i8042_irq_producer_identity;
	x86_guest_irq_router_construct(&owner.irq_router);
	router_status = x86_guest_irq_router_initialize(
		&owner.irq_router, owner.irq_producers,
		ARRAY_SIZE(owner.irq_producers), owner.irq_routes,
		ARRAY_SIZE(owner.irq_routes));
	if (router_status != X86_GUEST_IRQ_ROUTER_OK) {
		owner.poisoned = 1u;
		return X86_GUEST_SPACE_INVALID_STATE;
	}
	if (x86_io_resource_registry_initialize(
		    config->address_space_identity) !=
	    X86_IO_RESOURCE_OK ||
	    !register_platform_io_resources(config->address_space_identity,
					    config->machine_identity,
					    &config->i8042,
					    &display_resource, &display,
					    &display_available)) {
		owner.poisoned = 1u;
		return X86_GUEST_SPACE_INVALID_STATE;
	}
	if (x86_i8042_snapshot(config->address_space_identity,
				 &i8042_snapshot) != X86_I8042_OK ||
	    i8042_snapshot.owner_identity != config->machine_identity ||
	    i8042_snapshot.generation == 0u) {
		poison_platform_devices(config->address_space_identity);
		owner.poisoned = 1u;
		return X86_GUEST_SPACE_INVALID_STATE;
	}

	owner.machine = machine;
	owner.paging = paging;
	owner.io_requester_identity = KERNEL_OBJECT_HANDLE_INVALID;
	owner.display_resource = display_resource;
	owner.display_foreground_token = X86_IO_FOREGROUND_TOKEN_INVALID;
	owner.display = display;
	owner.generation = X86_GUEST_SPACE_FIRST_GENERATION;
	owner.i8042_generation = i8042_snapshot.generation;
	owner.virtual_system_control_a = 0u;
	owner.display_available = (uint8_t)display_available;
	owner.display_memory_granted = 0u;
	owner.display_cleanup_required = 0u;
	owner.irq_router_published = 0u;
	owner.event_pump_active = 0u;
	owner.poisoned = 0u;
	owner.initialized = 1u;
	return X86_GUEST_SPACE_OK;
}

const struct dos_machine *x86_guest_space_machine(void)
{
	return guest_space_is_active() ? &owner.machine : NULL;
}

kernel_object_handle_t x86_guest_space_machine_identity(void)
{
	return guest_space_is_active() ? owner.machine_identity
				       : KERNEL_OBJECT_HANDLE_INVALID;
}

static enum x86_guest_space_status guest_router_status(
	enum x86_guest_irq_router_status status)
{
	if (status == X86_GUEST_IRQ_ROUTER_OK)
		return X86_GUEST_SPACE_OK;
	if (status == X86_GUEST_IRQ_ROUTER_INVALID_ARGUMENT)
		return X86_GUEST_SPACE_INVALID_ARGUMENT;
	if (status == X86_GUEST_IRQ_ROUTER_IDENTITY_MISMATCH)
		return X86_GUEST_SPACE_INTERRUPT_SOURCE_MISMATCH;
	if (status == X86_GUEST_IRQ_ROUTER_STALE_BINDING)
		return X86_GUEST_SPACE_STALE_BINDING;
	if (status == X86_GUEST_IRQ_ROUTER_CAPACITY_EXHAUSTED)
		return X86_GUEST_SPACE_CAPACITY_EXHAUSTED;
	if (status == X86_GUEST_IRQ_ROUTER_BUSY)
		return X86_GUEST_SPACE_DEVICE_EVENT_RETRY;
	if (status == X86_GUEST_IRQ_ROUTER_POISONED)
		return X86_GUEST_SPACE_DEVICE_FAULT;
	if (status == X86_GUEST_IRQ_ROUTER_INVALID_STATE)
		return X86_GUEST_SPACE_INVALID_STATE;
	return X86_GUEST_SPACE_INTERRUPT_FAULT;
}

static enum x86_guest_irq_sink_result chipset_sink_result(
	enum x86_legacy_chipset_status status)
{
	if (status == X86_LEGACY_CHIPSET_OK)
		return X86_GUEST_IRQ_SINK_OK;
	if (status == X86_LEGACY_CHIPSET_POISONED)
		return X86_GUEST_IRQ_SINK_POISONED;
	return X86_GUEST_IRQ_SINK_REJECTED;
}

static bool sink_identity_matches(
	kernel_object_handle_t sink_context_identity,
	kernel_object_handle_t router_identity)
{
	return owner.initialized == 1u && owner.poisoned == 0u &&
	       sink_context_identity == owner.address_space_identity &&
	       router_identity == owner.irq_router_identity;
}

static enum x86_guest_irq_sink_result guest_chipset_sink_bind(
	kernel_object_handle_t sink_context_identity,
	kernel_object_handle_t router_identity,
	const struct x86_legacy_chipset_source_config *config)
{
	if (!sink_identity_matches(sink_context_identity, router_identity))
		return X86_GUEST_IRQ_SINK_REJECTED;
	return chipset_sink_result(x86_legacy_chipset_source_bind(
		owner.address_space_identity, owner.machine_identity,
		router_identity, config));
}

static enum x86_guest_irq_sink_result guest_chipset_sink_submit(
	kernel_object_handle_t sink_context_identity,
	kernel_object_handle_t router_identity,
	const struct x86_legacy_chipset_source_event *event)
{
	if (!sink_identity_matches(sink_context_identity, router_identity))
		return X86_GUEST_IRQ_SINK_REJECTED;
	return chipset_sink_result(x86_legacy_chipset_source_submit(
		owner.address_space_identity, router_identity, event));
}

static enum x86_guest_irq_sink_result guest_chipset_sink_quiesce(
	kernel_object_handle_t sink_context_identity,
	kernel_object_handle_t router_identity)
{
	if (!sink_identity_matches(sink_context_identity, router_identity))
		return X86_GUEST_IRQ_SINK_REJECTED;
	return chipset_sink_result(x86_legacy_chipset_source_quiesce(
		owner.address_space_identity, router_identity));
}

static enum x86_guest_irq_sink_result guest_chipset_sink_resume(
	kernel_object_handle_t sink_context_identity,
	kernel_object_handle_t router_identity)
{
	if (!sink_identity_matches(sink_context_identity, router_identity))
		return X86_GUEST_IRQ_SINK_REJECTED;
	return chipset_sink_result(x86_legacy_chipset_source_resume(
		owner.address_space_identity, router_identity));
}

static enum x86_guest_irq_sink_result guest_chipset_sink_unbind(
	kernel_object_handle_t sink_context_identity,
	kernel_object_handle_t router_identity)
{
	if (!sink_identity_matches(sink_context_identity, router_identity))
		return X86_GUEST_IRQ_SINK_REJECTED;
	return chipset_sink_result(x86_legacy_chipset_source_unbind(
		owner.address_space_identity, router_identity));
}

static void poison_guest_devices(void)
{
	enum x86_guest_irq_router_status router_status =
		X86_GUEST_IRQ_ROUTER_OK;
	enum x86_legacy_chipset_status chipset_status;
	enum x86_i8042_status i8042_status;

	owner.poisoned = 1u;
	owner.machine.poisoned = 1u;
	i8042_status = x86_i8042_poison(owner.address_space_identity);
	chipset_status = x86_legacy_chipset_poison(
		owner.address_space_identity);
	if (owner.irq_router.phase != X86_GUEST_IRQ_ROUTER_EMPTY &&
	    owner.irq_router.phase != X86_GUEST_IRQ_ROUTER_UNINITIALIZED &&
	    owner.irq_router.phase != X86_GUEST_IRQ_ROUTER_POISONED_PHASE)
		router_status = x86_guest_irq_router_poison(
			&owner.irq_router, owner.irq_router_identity);
	(void)router_status;
	(void)chipset_status;
	(void)i8042_status;
}

static enum x86_guest_space_status published_router_status(
	enum x86_guest_irq_router_status status)
{
	if (status == X86_GUEST_IRQ_ROUTER_POISONED) {
		poison_guest_devices();
		return X86_GUEST_SPACE_DEVICE_FAULT;
	}
	return guest_router_status(status);
}

static bool rollback_prepared_irq_router(
	bool route_installed, bool pit_registered, bool i8042_registered)
{
	bool complete = true;

	if (route_installed &&
	    x86_guest_irq_native_route_uninstall(
		    &owner.irq_router, &owner.pit_producer,
		    &owner.pit_route) != X86_GUEST_IRQ_ROUTER_OK)
		complete = false;
	if (pit_registered) {
		if (x86_guest_irq_producer_quiesce(
			    &owner.irq_router, &owner.pit_producer) !=
		    X86_GUEST_IRQ_ROUTER_OK ||
		    x86_guest_irq_producer_unregister(
			    &owner.irq_router, &owner.pit_producer) !=
			    X86_GUEST_IRQ_ROUTER_OK)
			complete = false;
	}
	if (i8042_registered) {
		if (x86_guest_irq_producer_quiesce(
			    &owner.irq_router, &owner.i8042_irq_producer) !=
		    X86_GUEST_IRQ_ROUTER_OK ||
		    x86_guest_irq_producer_unregister(
			    &owner.irq_router, &owner.i8042_irq_producer) !=
			    X86_GUEST_IRQ_ROUTER_OK)
			complete = false;
	}
	if (complete &&
	    x86_guest_irq_router_abort(&owner.irq_router) !=
		    X86_GUEST_IRQ_ROUTER_OK)
		complete = false;
	if (!complete)
		poison_guest_devices();
	return complete;
}

enum x86_guest_space_status x86_guest_space_native_pit_bind(
	kernel_object_handle_t machine_identity,
	kernel_object_handle_t source_identity, uint64_t pit_input_quantum,
	bool pit_rate_calibrated,
	struct x86_guest_space_pit_binding *binding)
{
	const struct x86_guest_irq_router_config router_config = {
		.identity = owner.irq_router_identity,
		.sink_context_identity = owner.address_space_identity,
		.sink = {
			.bind = guest_chipset_sink_bind,
			.submit = guest_chipset_sink_submit,
			.quiesce = guest_chipset_sink_quiesce,
			.resume = guest_chipset_sink_resume,
			.unbind = guest_chipset_sink_unbind,
		},
		.pit_input_quantum = pit_input_quantum,
		.capabilities = X86_GUEST_IRQ_PRODUCER_CAPABILITIES,
		.pit_rate_calibrated = (uint8_t)pit_rate_calibrated,
		.reserved = {0u},
	};
	const struct x86_guest_irq_producer_config i8042_producer = {
		.identity = owner.i8042_irq_producer_identity,
		.capabilities = X86_GUEST_IRQ_PRODUCER_IRQ_EDGE,
		.allowed_guest_irqs = (uint16_t)((1u << 1u) | (1u << 12u)),
		.reserved = {0u},
	};
	const struct x86_guest_irq_producer_config pit_producer = {
		.identity = source_identity,
		.capabilities = X86_GUEST_IRQ_PRODUCER_PIT_CLOCK,
		.allowed_guest_irqs = 1u,
		.reserved = {0u},
	};
	const struct x86_guest_irq_native_route_config pit_route = {
		.native_kind = (uint8_t)X86_LEGACY_IRQ_EVENT_PIT_CLOCK,
		.native_irq = 0u,
		.guest_kind = (uint8_t)X86_GUEST_IRQ_EVENT_PIT_CLOCK,
		.guest_irq = 0u,
		.reserved = {0u},
	};
	enum x86_guest_irq_router_status status;
	bool i8042_registered = false;
	bool pit_registered = false;
	bool route_installed = false;

	if (!identity_is_valid(machine_identity) ||
	    !identity_is_valid(source_identity) || pit_input_quantum == 0u ||
	    binding == NULL)
		return X86_GUEST_SPACE_INVALID_ARGUMENT;
	if (owner.initialized != 1u || owner.poisoned != 0u ||
	    owner.irq_router_published != 0u)
		return X86_GUEST_SPACE_INVALID_STATE;
	if (machine_identity != owner.machine_identity)
		return X86_GUEST_SPACE_MACHINE_MISMATCH;
	status = x86_guest_irq_router_prepare(&owner.irq_router,
					     &router_config);
	if (status != X86_GUEST_IRQ_ROUTER_OK)
		return guest_router_status(status);
	status = x86_guest_irq_producer_register(
		&owner.irq_router, &i8042_producer,
		&owner.i8042_irq_producer);
	if (status != X86_GUEST_IRQ_ROUTER_OK)
		goto rollback;
	i8042_registered = true;
	status = x86_guest_irq_producer_register(
		&owner.irq_router, &pit_producer, &owner.pit_producer);
	if (status != X86_GUEST_IRQ_ROUTER_OK)
		goto rollback;
	pit_registered = true;
	status = x86_guest_irq_native_route_install(
		&owner.irq_router, &owner.pit_producer, &pit_route,
		&owner.pit_route);
	if (status != X86_GUEST_IRQ_ROUTER_OK)
		goto rollback;
	route_installed = true;
	status = x86_guest_irq_router_publish(&owner.irq_router);
	if (status != X86_GUEST_IRQ_ROUTER_OK)
		goto rollback;
	owner.irq_router_published = 1u;
	*binding = (struct x86_guest_space_pit_binding){
		.source_identity = source_identity,
		.guest_space_generation = owner.generation,
		.router_generation = owner.pit_producer.router_generation,
		.producer_generation = owner.pit_producer.producer_generation,
		.reserved = {0u},
	};
	return X86_GUEST_SPACE_OK;

rollback:
	if (status == X86_GUEST_IRQ_ROUTER_POISONED ||
	    !rollback_prepared_irq_router(route_installed, pit_registered,
					  i8042_registered)) {
		poison_guest_devices();
		return X86_GUEST_SPACE_DEVICE_FAULT;
	}
	return guest_router_status(status);
}

static enum x86_guest_space_status pit_binding_status(
	const struct x86_guest_space_pit_binding *binding)
{
	if (binding == NULL || !identity_is_valid(binding->source_identity) ||
	    !reserved_is_zero(binding->reserved, ARRAY_SIZE(binding->reserved)))
		return X86_GUEST_SPACE_INVALID_ARGUMENT;
	if (owner.initialized != 1u || owner.poisoned != 0u)
		return X86_GUEST_SPACE_INVALID_STATE;
	if (owner.irq_router_published == 0u ||
	    binding->guest_space_generation != owner.generation ||
	    binding->router_generation !=
		    owner.pit_producer.router_generation ||
	    binding->producer_generation !=
		    owner.pit_producer.producer_generation)
		return X86_GUEST_SPACE_STALE_BINDING;
	if (binding->source_identity !=
	    owner.pit_producer.producer_identity)
		return X86_GUEST_SPACE_INTERRUPT_SOURCE_MISMATCH;
	return X86_GUEST_SPACE_OK;
}

enum x86_guest_space_status x86_guest_space_native_pit_submit(
	const struct x86_guest_space_pit_binding *binding,
	uint64_t pit_input_ticks)
{
	struct x86_legacy_irq_event event;
	enum x86_guest_space_status binding_status =
		pit_binding_status(binding);

	if (binding_status != X86_GUEST_SPACE_OK)
		return binding_status;
	if (pit_input_ticks == 0u)
		return X86_GUEST_SPACE_INVALID_ARGUMENT;
	event = (struct x86_legacy_irq_event){
		.pit_input_ticks = pit_input_ticks,
		.source_identity = binding->source_identity,
		.kind = (uint8_t)X86_LEGACY_IRQ_EVENT_PIT_CLOCK,
		.irq = 0u,
		.pit_rate_calibrated = owner.irq_router.pit_rate_calibrated,
		.reserved = {0u},
	};
	return published_router_status(x86_guest_irq_route_native(
		&owner.irq_router, &owner.pit_producer, &event));
}

enum x86_guest_space_status x86_guest_space_native_pit_quiesce(
	const struct x86_guest_space_pit_binding *binding)
{
	enum x86_guest_space_status binding_status =
		pit_binding_status(binding);

	if (binding_status != X86_GUEST_SPACE_OK)
		return binding_status;
	return published_router_status(x86_guest_irq_router_quiesce(
		&owner.irq_router, owner.irq_router_identity));
}

enum x86_guest_space_status x86_guest_space_native_pit_resume(
	const struct x86_guest_space_pit_binding *binding)
{
	enum x86_guest_space_status binding_status =
		pit_binding_status(binding);

	if (binding_status != X86_GUEST_SPACE_OK)
		return binding_status;
	return published_router_status(x86_guest_irq_router_resume(
		&owner.irq_router, owner.irq_router_identity));
}

enum x86_guest_space_status x86_guest_space_native_pit_unbind(
	const struct x86_guest_space_pit_binding *binding)
{
	struct x86_i8042_snapshot i8042_snapshot;
	enum x86_guest_space_status binding_status =
		pit_binding_status(binding);
	bool complete = true;

	if (binding_status != X86_GUEST_SPACE_OK)
		return binding_status;
	if (owner.irq_router.phase != X86_GUEST_IRQ_ROUTER_QUIESCED)
		return X86_GUEST_SPACE_INVALID_STATE;
	if (x86_i8042_snapshot(owner.address_space_identity, &i8042_snapshot) !=
	    X86_I8042_OK) {
		poison_guest_devices();
		return X86_GUEST_SPACE_DEVICE_FAULT;
	}
	/* Upstream input must be gone before its downstream producer retires. */
	if (i8042_snapshot.input_source_bound != 0u)
		return X86_GUEST_SPACE_INVALID_STATE;
	if (x86_guest_irq_native_route_uninstall(
		    &owner.irq_router, &owner.pit_producer,
		    &owner.pit_route) != X86_GUEST_IRQ_ROUTER_OK)
		complete = false;
	if (complete &&
	    (x86_guest_irq_producer_quiesce(
		     &owner.irq_router, &owner.pit_producer) !=
		     X86_GUEST_IRQ_ROUTER_OK ||
	     x86_guest_irq_producer_unregister(
		     &owner.irq_router, &owner.pit_producer) !=
		     X86_GUEST_IRQ_ROUTER_OK))
		complete = false;
	if (complete &&
	    (x86_guest_irq_producer_quiesce(
		     &owner.irq_router, &owner.i8042_irq_producer) !=
		     X86_GUEST_IRQ_ROUTER_OK ||
	     x86_guest_irq_producer_unregister(
		     &owner.irq_router, &owner.i8042_irq_producer) !=
		     X86_GUEST_IRQ_ROUTER_OK))
		complete = false;
	if (complete &&
	    x86_guest_irq_router_retire(&owner.irq_router,
					owner.irq_router_identity) !=
		    X86_GUEST_IRQ_ROUTER_OK)
		complete = false;
	if (!complete) {
		poison_guest_devices();
		return X86_GUEST_SPACE_DEVICE_FAULT;
	}
	owner.irq_router_published = 0u;
	owner.pit_producer = (struct x86_guest_irq_producer_binding){0};
	owner.i8042_irq_producer =
		(struct x86_guest_irq_producer_binding){0};
	owner.pit_route = (struct x86_guest_irq_route_binding){0};
	return X86_GUEST_SPACE_OK;
}

static enum x86_guest_space_status guest_i8042_status(
	enum x86_i8042_status status)
{
	if (status == X86_I8042_OK || status == X86_I8042_NO_EVENT)
		return X86_GUEST_SPACE_OK;
	if (status == X86_I8042_INVALID_ARGUMENT)
		return X86_GUEST_SPACE_INVALID_ARGUMENT;
	if (status == X86_I8042_IDENTITY_MISMATCH)
		return X86_GUEST_SPACE_INTERRUPT_SOURCE_MISMATCH;
	if (status == X86_I8042_STALE_BINDING ||
	    status == X86_I8042_STALE_EVENT)
		return X86_GUEST_SPACE_STALE_BINDING;
	if (status == X86_I8042_CAPACITY_EXHAUSTED)
		return X86_GUEST_SPACE_CAPACITY_EXHAUSTED;
	if (status == X86_I8042_MODE_CHANGED)
		return X86_GUEST_SPACE_INPUT_MODE_CHANGED;
	if (status == X86_I8042_INPUT_DISABLED)
		return X86_GUEST_SPACE_IO_DENIED;
	if (status == X86_I8042_POISONED) {
		poison_guest_devices();
		return X86_GUEST_SPACE_DEVICE_FAULT;
	}
	return X86_GUEST_SPACE_INVALID_STATE;
}

static bool i8042_event_is_valid(const struct x86_i8042_event *event)
{
	if (event->controller_generation != owner.i8042_generation ||
	    event->sequence == 0u ||
	    !reserved_is_zero(event->reserved, ARRAY_SIZE(event->reserved)))
		return false;
	if (event->kind == (uint8_t)X86_I8042_EVENT_IRQ_REQUEST)
		return (event->irq == 1u || event->irq == 12u) &&
		       event->a20_enabled == 0u;
	if (event->kind == (uint8_t)X86_I8042_EVENT_A20_CHANGE)
		return event->irq == 0u && event->a20_enabled <= 1u;
	return false;
}

static enum x86_guest_space_status pump_guest_device_events(
	size_t budget, size_t *processed)
{
	struct x86_i8042_event event;
	enum x86_guest_space_status result = X86_GUEST_SPACE_OK;
	size_t completed = 0u;

	if (processed == NULL || budget == 0u)
		return X86_GUEST_SPACE_INVALID_ARGUMENT;
	if (owner.initialized != 1u || owner.poisoned != 0u)
		return X86_GUEST_SPACE_INVALID_STATE;
	if (owner.event_pump_active != 0u)
		return X86_GUEST_SPACE_DEVICE_EVENT_RETRY;
	owner.event_pump_active = 1u;
	while (completed < budget) {
		enum x86_i8042_status i8042_status = x86_i8042_event_peek(
			owner.address_space_identity, owner.machine_identity, &event);

		if (i8042_status == X86_I8042_NO_EVENT)
			break;
		if (i8042_status != X86_I8042_OK) {
			result = guest_i8042_status(i8042_status);
			goto out;
		}
		if (!i8042_event_is_valid(&event)) {
			poison_guest_devices();
			result = X86_GUEST_SPACE_DEVICE_FAULT;
			goto out;
		}
		if (event.kind == (uint8_t)X86_I8042_EVENT_IRQ_REQUEST) {
			const struct x86_guest_irq_event irq_event = {
				.kind = (uint8_t)X86_GUEST_IRQ_EVENT_IRQ_EDGE,
				.irq = event.irq,
				.reserved = {0u},
			};
			enum x86_guest_irq_router_status router_status;

			if (owner.irq_router_published == 0u) {
				result = X86_GUEST_SPACE_INVALID_STATE;
				goto out;
			}
			router_status = x86_guest_irq_submit(
				&owner.irq_router, &owner.i8042_irq_producer,
				&irq_event);
			if (router_status != X86_GUEST_IRQ_ROUTER_OK) {
				result = guest_router_status(router_status);
				if (router_status == X86_GUEST_IRQ_ROUTER_POISONED)
					poison_guest_devices();
				goto out;
			}
		} else if (virtual_set_a20(owner.address_space_identity,
					     event.a20_enabled != 0u) !=
			   DOS_MACHINE_OK) {
			poison_guest_devices();
			result = X86_GUEST_SPACE_DEVICE_FAULT;
			goto out;
		}
		if (x86_i8042_event_consume(owner.address_space_identity,
					     owner.machine_identity,
					     event.sequence) != X86_I8042_OK) {
			/* The IRQ or A20 transition was already published. */
			poison_guest_devices();
			result = X86_GUEST_SPACE_DEVICE_FAULT;
			goto out;
		}
		completed++;
	}
	if (completed == budget) {
		enum x86_i8042_status status = x86_i8042_event_peek(
			owner.address_space_identity, owner.machine_identity, &event);

		if (status == X86_I8042_OK)
			result = X86_GUEST_SPACE_DEVICE_EVENT_RETRY;
		else if (status != X86_I8042_NO_EVENT)
			result = guest_i8042_status(status);
	}

out:
	owner.event_pump_active = 0u;
	*processed = completed;
	return result;
}

enum x86_guest_space_status x86_guest_space_device_events_pump(
	kernel_object_handle_t machine_identity, size_t budget,
	size_t *processed)
{
	if (!identity_is_valid(machine_identity) || processed == NULL ||
	    budget == 0u)
		return X86_GUEST_SPACE_INVALID_ARGUMENT;
	if (owner.initialized != 1u || owner.poisoned != 0u)
		return X86_GUEST_SPACE_INVALID_STATE;
	if (machine_identity != owner.machine_identity)
		return X86_GUEST_SPACE_MACHINE_MISMATCH;
	return pump_guest_device_events(budget, processed);
}

enum x86_guest_space_status x86_guest_space_i8042_input_bind(
	kernel_object_handle_t machine_identity,
	kernel_object_handle_t source_identity,
	const struct x86_i8042_input_config *config,
	struct x86_i8042_input_binding *binding)
{
	if (!identity_is_valid(machine_identity) ||
	    !identity_is_valid(source_identity) || config == NULL ||
	    binding == NULL)
		return X86_GUEST_SPACE_INVALID_ARGUMENT;
	if (owner.initialized != 1u || owner.poisoned != 0u ||
	    owner.irq_router_published == 0u ||
	    owner.irq_router.phase != X86_GUEST_IRQ_ROUTER_ACTIVE)
		return X86_GUEST_SPACE_INVALID_STATE;
	if (machine_identity != owner.machine_identity)
		return X86_GUEST_SPACE_MACHINE_MISMATCH;
	return guest_i8042_status(x86_i8042_input_bind(
		owner.address_space_identity, owner.machine_identity,
		source_identity, config, binding));
}

enum x86_guest_space_status x86_guest_space_i8042_input_quiesce(
	kernel_object_handle_t machine_identity,
	const struct x86_i8042_input_binding *binding)
{
	if (!identity_is_valid(machine_identity) || binding == NULL)
		return X86_GUEST_SPACE_INVALID_ARGUMENT;
	if (owner.initialized != 1u || owner.poisoned != 0u)
		return X86_GUEST_SPACE_INVALID_STATE;
	if (machine_identity != owner.machine_identity)
		return X86_GUEST_SPACE_MACHINE_MISMATCH;
	return guest_i8042_status(x86_i8042_input_quiesce(
		owner.address_space_identity, owner.machine_identity, binding));
}

enum x86_guest_space_status x86_guest_space_i8042_input_resume(
	kernel_object_handle_t machine_identity,
	const struct x86_i8042_input_binding *binding)
{
	if (!identity_is_valid(machine_identity) || binding == NULL)
		return X86_GUEST_SPACE_INVALID_ARGUMENT;
	if (owner.initialized != 1u || owner.poisoned != 0u ||
	    owner.irq_router_published == 0u ||
	    owner.irq_router.phase != X86_GUEST_IRQ_ROUTER_ACTIVE)
		return X86_GUEST_SPACE_INVALID_STATE;
	if (machine_identity != owner.machine_identity)
		return X86_GUEST_SPACE_MACHINE_MISMATCH;
	return guest_i8042_status(x86_i8042_input_resume(
		owner.address_space_identity, owner.machine_identity, binding));
}

enum x86_guest_space_status x86_guest_space_i8042_input_unbind(
	kernel_object_handle_t machine_identity,
	const struct x86_i8042_input_binding *binding)
{
	if (!identity_is_valid(machine_identity) || binding == NULL)
		return X86_GUEST_SPACE_INVALID_ARGUMENT;
	if (owner.initialized != 1u || owner.poisoned != 0u)
		return X86_GUEST_SPACE_INVALID_STATE;
	if (machine_identity != owner.machine_identity)
		return X86_GUEST_SPACE_MACHINE_MISMATCH;
	return guest_i8042_status(x86_i8042_input_unbind(
		owner.address_space_identity, owner.machine_identity, binding));
}

enum x86_guest_space_status x86_guest_space_i8042_input_keyboard_mode(
	const struct x86_i8042_input_binding *binding,
	struct x86_i8042_keyboard_mode *mode)
{
	if (binding == NULL || mode == NULL)
		return X86_GUEST_SPACE_INVALID_ARGUMENT;
	if (owner.initialized != 1u || owner.poisoned != 0u ||
	    owner.irq_router_published == 0u ||
	    owner.irq_router.phase != X86_GUEST_IRQ_ROUTER_ACTIVE)
		return X86_GUEST_SPACE_INVALID_STATE;
	return guest_i8042_status(
		x86_i8042_input_keyboard_mode(binding, mode));
}

static enum x86_guest_space_status committed_input_delivery_status(
	enum x86_guest_space_status status)
{
	if (status == X86_GUEST_SPACE_DEVICE_EVENT_RETRY)
		return X86_GUEST_SPACE_INPUT_COMMITTED_DELIVERY_PENDING;
	return status;
}

enum x86_guest_space_status
x86_guest_space_i8042_input_inject_keyboard_sequence(
	const struct x86_i8042_input_binding *binding,
	const struct x86_i8042_keyboard_mode *mode, const uint8_t *values,
	size_t values_capacity, size_t count)
{
	enum x86_i8042_status status;
	size_t processed;

	if (binding == NULL || mode == NULL)
		return X86_GUEST_SPACE_INVALID_ARGUMENT;
	if (owner.initialized != 1u || owner.poisoned != 0u ||
	    owner.irq_router_published == 0u ||
	    owner.irq_router.phase != X86_GUEST_IRQ_ROUTER_ACTIVE)
		return X86_GUEST_SPACE_INVALID_STATE;
	status = x86_i8042_input_inject_keyboard_sequence(
		binding, mode, values, values_capacity, count);
	if (status != X86_I8042_OK)
		return guest_i8042_status(status);
	return committed_input_delivery_status(pump_guest_device_events(
		CONFIG_X86_GUEST_DEVICE_EVENT_PUMP_BUDGET, &processed));
}

enum x86_guest_space_status x86_guest_space_i8042_input_inject_keyboard(
	const struct x86_i8042_input_binding *binding,
	const struct x86_i8042_keyboard_mode *mode, uint8_t value)
{
	return x86_guest_space_i8042_input_inject_keyboard_sequence(
		binding, mode, &value, sizeof(value), 1u);
}

enum x86_guest_space_status x86_guest_space_i8042_input_inject_sequence(
	const struct x86_i8042_input_binding *binding,
	enum x86_i8042_input_kind kind, const uint8_t *values,
	size_t values_capacity, size_t count)
{
	enum x86_i8042_status status;
	size_t processed;

	if (binding == NULL)
		return X86_GUEST_SPACE_INVALID_ARGUMENT;
	if (owner.initialized != 1u || owner.poisoned != 0u ||
	    owner.irq_router_published == 0u ||
	    owner.irq_router.phase != X86_GUEST_IRQ_ROUTER_ACTIVE)
		return X86_GUEST_SPACE_INVALID_STATE;
	status = x86_i8042_input_inject_sequence(
		binding, kind, values, values_capacity, count);
	if (status != X86_I8042_OK)
		return guest_i8042_status(status);
	return committed_input_delivery_status(pump_guest_device_events(
		CONFIG_X86_GUEST_DEVICE_EVENT_PUMP_BUDGET, &processed));
}

enum x86_guest_space_status x86_guest_space_i8042_input_inject(
	const struct x86_i8042_input_binding *binding,
	enum x86_i8042_input_kind kind, uint8_t value)
{
	return x86_guest_space_i8042_input_inject_sequence(
		binding, kind, &value, sizeof(value), 1u);
}

static enum x86_guest_space_status interrupt_status(
	enum x86_legacy_chipset_status status)
{
	if (status == X86_LEGACY_CHIPSET_OK)
		return X86_GUEST_SPACE_OK;
	if (status == X86_LEGACY_CHIPSET_NO_INTERRUPT)
		return X86_GUEST_SPACE_NO_INTERRUPT;
	if (status == X86_LEGACY_CHIPSET_INVALID_ARGUMENT)
		return X86_GUEST_SPACE_INVALID_ARGUMENT;
	if (status == X86_LEGACY_CHIPSET_IDENTITY_MISMATCH)
		return X86_GUEST_SPACE_MACHINE_MISMATCH;
	if (status == X86_LEGACY_CHIPSET_INVALID_STATE)
		return X86_GUEST_SPACE_INVALID_STATE;
	if (status == X86_LEGACY_CHIPSET_STALE_INTERRUPT)
		return X86_GUEST_SPACE_STALE_BINDING;
	return X86_GUEST_SPACE_INTERRUPT_FAULT;
}

static enum x86_guest_space_status interrupt_owner_status(
	kernel_object_handle_t machine_identity)
{
	if (!identity_is_valid(machine_identity))
		return X86_GUEST_SPACE_INVALID_ARGUMENT;
	if (!guest_space_is_active())
		return X86_GUEST_SPACE_INVALID_STATE;
	if (machine_identity != owner.machine_identity)
		return X86_GUEST_SPACE_MACHINE_MISMATCH;
	return X86_GUEST_SPACE_OK;
}

enum x86_guest_space_status x86_guest_space_interrupt_prepare(
	kernel_object_handle_t machine_identity,
	struct x86_legacy_interrupt_claim *claim)
{
	enum x86_guest_space_status status =
		interrupt_owner_status(machine_identity);

	if (claim == NULL)
		return X86_GUEST_SPACE_INVALID_ARGUMENT;
	if (status != X86_GUEST_SPACE_OK)
		return status;
	return interrupt_status(x86_legacy_chipset_interrupt_prepare(
		owner.address_space_identity, owner.machine_identity, claim));
}

enum x86_guest_space_status x86_guest_space_interrupt_commit(
	kernel_object_handle_t machine_identity,
	const struct x86_legacy_interrupt_claim *claim)
{
	enum x86_guest_space_status status =
		interrupt_owner_status(machine_identity);

	if (claim == NULL)
		return X86_GUEST_SPACE_INVALID_ARGUMENT;
	if (status != X86_GUEST_SPACE_OK)
		return status;
	return interrupt_status(x86_legacy_chipset_interrupt_commit(
		owner.address_space_identity, owner.machine_identity, claim));
}

enum x86_guest_space_status x86_guest_space_interrupt_cancel(
	kernel_object_handle_t machine_identity,
	const struct x86_legacy_interrupt_claim *claim)
{
	enum x86_guest_space_status status =
		interrupt_owner_status(machine_identity);

	if (claim == NULL)
		return X86_GUEST_SPACE_INVALID_ARGUMENT;
	if (status != X86_GUEST_SPACE_OK)
		return status;
	return interrupt_status(x86_legacy_chipset_interrupt_cancel(
		owner.address_space_identity, owner.machine_identity, claim));
}

enum x86_guest_space_status x86_guest_space_interrupt_claim(
	kernel_object_handle_t machine_identity,
	struct x86_legacy_interrupt_claim *claim)
{
	enum x86_guest_space_status status =
		x86_guest_space_interrupt_prepare(machine_identity, claim);

	if (status != X86_GUEST_SPACE_OK)
		return status;
	return x86_guest_space_interrupt_commit(machine_identity, claim);
}

static enum x86_guest_space_status guest_space_io_status(
	enum x86_io_resource_status status)
{
	if (status == X86_IO_RESOURCE_OK)
		return X86_GUEST_SPACE_OK;
	if (status == X86_IO_RESOURCE_INVALID_ARGUMENT)
		return X86_GUEST_SPACE_INVALID_ARGUMENT;
	if (status == X86_IO_RESOURCE_ACCESS_DENIED ||
	    status == X86_IO_RESOURCE_OWNERSHIP_DENIED)
		return X86_GUEST_SPACE_IO_DENIED;
	return X86_GUEST_SPACE_IO_FAULT;
}

static void clear_display_lease(void)
{
	owner.io_requester_identity = KERNEL_OBJECT_HANDLE_INVALID;
	owner.display_foreground_token = X86_IO_FOREGROUND_TOKEN_INVALID;
	owner.display_memory_granted = 0u;
	owner.display_cleanup_required = 0u;
}

static void quarantine_display_lease(
	kernel_object_handle_t requester_identity,
	x86_io_foreground_token_t token)
{
	owner.io_requester_identity = requester_identity;
	owner.display_foreground_token = token;
	owner.display_memory_granted = 0u;
	owner.display_cleanup_required = 1u;
}

static void quarantine_display_mapping(void)
{
	owner.io_requester_identity = KERNEL_OBJECT_HANDLE_INVALID;
	owner.display_foreground_token = X86_IO_FOREGROUND_TOKEN_INVALID;
	owner.display_memory_granted = 0u;
	owner.display_cleanup_required = 1u;
}

enum display_memory_transition {
	DISPLAY_MEMORY_OK = 0,
	DISPLAY_MEMORY_MAPPING_MISMATCH,
	DISPLAY_MEMORY_CLEANUP_FAULT
};

static enum display_memory_transition grant_display_memory(void)
{
	size_t index;

	for (index = 0u; index < owner.display.memory_range_count; ++index) {
		const struct x86_display_memory_range *range =
			&owner.display.memory[index];

		if (!x86_paging_grant_legacy_video_range(range->base,
							 range->bytes)) {
			bool rollback_ok = true;

			while (index > 0u) {
				--index;
				range = &owner.display.memory[index];
				if (!x86_paging_revoke_legacy_video_range(
					    range->base, range->bytes))
					rollback_ok = false;
			}
			return rollback_ok ? DISPLAY_MEMORY_MAPPING_MISMATCH
					   : DISPLAY_MEMORY_CLEANUP_FAULT;
		}
	}
	return DISPLAY_MEMORY_OK;
}

static bool revoke_display_memory(void)
{
	bool released = true;
	size_t index = owner.display.memory_range_count;

	while (index > 0u) {
		const struct x86_display_memory_range *range;

		--index;
		range = &owner.display.memory[index];
		if (!x86_paging_revoke_legacy_video_range(range->base,
							  range->bytes))
			released = false;
	}
	return released;
}

enum x86_guest_space_status x86_guest_space_display_foreground_acquire(
	kernel_object_handle_t machine_identity,
	kernel_object_handle_t requester_identity,
	x86_io_foreground_token_t *token)
{
	x86_io_foreground_token_t prepared;
	enum display_memory_transition memory_status;
	enum x86_io_resource_status status;

	if (!identity_is_valid(machine_identity) ||
	    !identity_is_valid(requester_identity) || token == NULL)
		return X86_GUEST_SPACE_INVALID_ARGUMENT;
	if (!guest_space_is_active())
		return X86_GUEST_SPACE_INVALID_STATE;
	if (machine_identity != owner.machine_identity)
		return X86_GUEST_SPACE_MACHINE_MISMATCH;
	if (owner.display_available != 1u)
		return X86_GUEST_SPACE_IO_DENIED;
	if (owner.display_foreground_token != X86_IO_FOREGROUND_TOKEN_INVALID ||
	    owner.io_requester_identity != KERNEL_OBJECT_HANDLE_INVALID ||
	    owner.display_memory_granted != 0u ||
	    owner.display_cleanup_required != 0u)
		return X86_GUEST_SPACE_IO_DENIED;
	status = foreground_acquire(
		owner.display_resource, owner.machine_identity, requester_identity,
		&prepared);
	if (status != X86_IO_RESOURCE_OK)
		return guest_space_io_status(status);
	memory_status = grant_display_memory();
	if (memory_status != DISPLAY_MEMORY_OK) {
		status = foreground_revoke(
			owner.display_resource, owner.machine_identity);
		if (status == X86_IO_RESOURCE_OK &&
		    memory_status == DISPLAY_MEMORY_MAPPING_MISMATCH)
			return X86_GUEST_SPACE_PAGING_MISMATCH;
		if (status == X86_IO_RESOURCE_OK) {
			owner.io_requester_identity = KERNEL_OBJECT_HANDLE_INVALID;
			owner.display_foreground_token =
				X86_IO_FOREGROUND_TOKEN_INVALID;
			owner.display_memory_granted = 0u;
			owner.display_cleanup_required = 1u;
			return X86_GUEST_SPACE_PAGING_MISMATCH;
		}
		quarantine_display_lease(requester_identity, prepared);
		return guest_space_io_status(status);
	}
	owner.display_memory_granted = 1u;
	owner.io_requester_identity = requester_identity;
	owner.display_foreground_token = prepared;
	owner.display_cleanup_required = 0u;
	*token = prepared;
	return X86_GUEST_SPACE_OK;
}

enum x86_guest_space_status x86_guest_space_display_foreground_release(
	kernel_object_handle_t requester_identity,
	x86_io_foreground_token_t token)
{
	bool paging_released;
	enum x86_io_resource_status status;

	if (!identity_is_valid(requester_identity) ||
	    token == X86_IO_FOREGROUND_TOKEN_INVALID)
		return X86_GUEST_SPACE_INVALID_ARGUMENT;
	if (!guest_space_is_active())
		return X86_GUEST_SPACE_INVALID_STATE;
	if (requester_identity != owner.io_requester_identity ||
	    token != owner.display_foreground_token ||
	    (owner.display_memory_granted != 1u &&
	     owner.display_cleanup_required != 1u))
		return X86_GUEST_SPACE_IO_DENIED;
	paging_released = revoke_display_memory();
	owner.display_memory_granted = 0u;
	owner.display_cleanup_required = 1u;
	status = foreground_release(token, requester_identity);
	if (status != X86_IO_RESOURCE_OK)
		return guest_space_io_status(status);
	if (!paging_released) {
		quarantine_display_mapping();
		return X86_GUEST_SPACE_PAGING_MISMATCH;
	}
	clear_display_lease();
	return X86_GUEST_SPACE_OK;
}

enum x86_guest_space_status x86_guest_space_display_foreground_revoke(
	kernel_object_handle_t machine_identity)
{
	bool paging_released;
	enum x86_io_resource_status status;

	if (!identity_is_valid(machine_identity))
		return X86_GUEST_SPACE_INVALID_ARGUMENT;
	if (owner.initialized != 1u)
		return X86_GUEST_SPACE_INVALID_STATE;
	if (machine_identity != owner.machine_identity)
		return X86_GUEST_SPACE_MACHINE_MISMATCH;
	if (owner.display_available != 1u)
		return X86_GUEST_SPACE_IO_DENIED;
	if (owner.display_foreground_token == X86_IO_FOREGROUND_TOKEN_INVALID &&
	    owner.io_requester_identity == KERNEL_OBJECT_HANDLE_INVALID &&
	    owner.display_memory_granted == 0u &&
	    owner.display_cleanup_required == 0u)
		return X86_GUEST_SPACE_OK;
	paging_released = revoke_display_memory();
	owner.display_memory_granted = 0u;
	owner.display_cleanup_required = 1u;
	status = foreground_revoke(owner.display_resource,
				   owner.machine_identity);
	if (status != X86_IO_RESOURCE_OK)
		return guest_space_io_status(status);
	if (!paging_released) {
		quarantine_display_mapping();
		return X86_GUEST_SPACE_PAGING_MISMATCH;
	}
	clear_display_lease();
	return X86_GUEST_SPACE_OK;
}

enum x86_guest_space_status x86_guest_space_quarantine(
	kernel_object_handle_t machine_identity, const struct dos_machine *machine)
{
	enum x86_guest_space_status display_status = X86_GUEST_SPACE_OK;

	if (!identity_is_valid(machine_identity) || machine == NULL)
		return X86_GUEST_SPACE_INVALID_ARGUMENT;
	if (owner.initialized != 1u)
		return X86_GUEST_SPACE_INVALID_STATE;
	if (machine_identity != owner.machine_identity || machine != &owner.machine)
		return X86_GUEST_SPACE_MACHINE_MISMATCH;
	if (owner.display_available == 1u)
		display_status =
			x86_guest_space_display_foreground_revoke(machine_identity);
	if (owner.poisoned == 0u || owner.machine.poisoned == 0u)
		poison_guest_devices();
	return display_status == X86_GUEST_SPACE_OK
		       ? X86_GUEST_SPACE_OK
		       : X86_GUEST_SPACE_DEVICE_FAULT;
}

enum x86_guest_space_status x86_guest_space_conventional_end_segment(
	uint16_t *end_segment)
{
	struct x86_legacy_bios_snapshot platform;
	uint16_t prepared;

	if (end_segment == NULL)
		return X86_GUEST_SPACE_INVALID_ARGUMENT;
	if (!guest_space_is_active())
		return X86_GUEST_SPACE_INVALID_STATE;
	if (!x86_legacy_bios_snapshot(&platform))
		return X86_GUEST_SPACE_PAGING_MISMATCH;
	prepared = (uint16_t)(platform.conventional_kib *
			      X86_PARAGRAPHS_PER_KIB);
	*end_segment = prepared;
	return X86_GUEST_SPACE_OK;
}

enum x86_guest_space_status x86_guest_space_pin(
	kernel_object_handle_t machine_identity,
	const struct dos_machine *machine,
	struct x86_guest_space_binding *binding)
{
	struct x86_guest_space_binding prepared;

	if (binding == NULL || machine == NULL ||
	    !identity_is_valid(machine_identity))
		return X86_GUEST_SPACE_INVALID_ARGUMENT;
	if (!guest_space_is_active())
		return X86_GUEST_SPACE_INVALID_STATE;
	if (machine != &owner.machine || machine_identity != owner.machine_identity ||
	    machine->context != owner.address_space_identity ||
	    machine->address_limit != owner.paging.guest_linear_limit ||
	    machine->poisoned != 0u)
		return X86_GUEST_SPACE_MACHINE_MISMATCH;
	if (!x86_paging_binding_is_active(&owner.paging))
		return X86_GUEST_SPACE_PAGING_MISMATCH;

	prepared = (struct x86_guest_space_binding){
		.address_space_identity = owner.address_space_identity,
		.address_space_generation = owner.generation,
		.machine_identity = owner.machine_identity,
		.machine_context = owner.machine.context,
		.paging = owner.paging,
		.a20_enabled = (uint8_t)(machine->a20_enabled ? 1u : 0u),
		.reserved = {0u},
	};
	*binding = prepared;
	return X86_GUEST_SPACE_OK;
}

bool x86_guest_space_binding_is_active(
	const struct x86_guest_space_binding *binding,
	kernel_object_handle_t machine_identity,
	const struct dos_machine *machine)
{
	return binding != NULL && guest_space_is_active() &&
	       machine == &owner.machine &&
	       machine_identity == owner.machine_identity &&
	       machine->context == owner.machine.context &&
	       machine->address_limit == owner.machine.address_limit &&
	       machine->a20_enabled == owner.machine.a20_enabled &&
	       machine->poisoned == 0u &&
	       binding->address_space_identity == owner.address_space_identity &&
	       binding->address_space_generation == owner.generation &&
	       binding->machine_identity == owner.machine_identity &&
	       binding->machine_context == owner.machine.context &&
	       binding->a20_enabled <= 1u &&
	       reserved_is_zero(binding->reserved, ARRAY_SIZE(binding->reserved)) &&
	       binding->paging.generation == owner.paging.generation &&
	       binding->paging.page_directory == owner.paging.page_directory &&
	       binding->paging.guest_linear_limit ==
		       owner.paging.guest_linear_limit &&
	       x86_paging_binding_is_active(&binding->paging);
}

static enum x86_guest_space_status firmware_shadow_status(
	enum x86_firmware_shadow_status status)
{
	if (status == X86_FIRMWARE_SHADOW_OK)
		return X86_GUEST_SPACE_OK;
	if (status == X86_FIRMWARE_SHADOW_INVALID_ARGUMENT)
		return X86_GUEST_SPACE_INVALID_ARGUMENT;
	if (status == X86_FIRMWARE_SHADOW_MACHINE_MISMATCH)
		return X86_GUEST_SPACE_MACHINE_MISMATCH;
	if (status == X86_FIRMWARE_SHADOW_STALE_BINDING)
		return X86_GUEST_SPACE_STALE_BINDING;
	if (status == X86_FIRMWARE_SHADOW_NOT_APPLICABLE)
		return X86_GUEST_SPACE_IO_DENIED;
	if (status == X86_FIRMWARE_SHADOW_CAPACITY_EXHAUSTED ||
	    status == X86_FIRMWARE_SHADOW_NO_MEMORY)
		return X86_GUEST_SPACE_CAPACITY_EXHAUSTED;
	if (status == X86_FIRMWARE_SHADOW_RETRY)
		return X86_GUEST_SPACE_DEVICE_EVENT_RETRY;
	if (status == X86_FIRMWARE_SHADOW_PAGING_MISMATCH)
		return X86_GUEST_SPACE_PAGING_MISMATCH;
	if (status == X86_FIRMWARE_SHADOW_POISONED) {
		poison_guest_devices();
		return X86_GUEST_SPACE_DEVICE_FAULT;
	}
	return X86_GUEST_SPACE_INVALID_STATE;
}

enum x86_guest_space_status x86_guest_space_firmware_execution_acquire(
	kernel_object_handle_t machine_identity,
	struct x86_guest_space_firmware_binding *binding)
{
	if (!identity_is_valid(machine_identity) || binding == NULL)
		return X86_GUEST_SPACE_INVALID_ARGUMENT;
	if (!guest_space_is_active())
		return X86_GUEST_SPACE_INVALID_STATE;
	if (machine_identity != owner.machine_identity)
		return X86_GUEST_SPACE_MACHINE_MISMATCH;
	return firmware_shadow_status(x86_firmware_shadow_execution_acquire(
		machine_identity, binding));
}

enum x86_guest_space_status x86_guest_space_firmware_execution_release(
	kernel_object_handle_t machine_identity,
	const struct x86_guest_space_firmware_binding *binding)
{
	if (!identity_is_valid(machine_identity) || binding == NULL)
		return X86_GUEST_SPACE_INVALID_ARGUMENT;
	if (owner.initialized != 1u)
		return X86_GUEST_SPACE_INVALID_STATE;
	if (machine_identity != owner.machine_identity)
		return X86_GUEST_SPACE_MACHINE_MISMATCH;
	return firmware_shadow_status(x86_firmware_shadow_execution_release(
		machine_identity, binding));
}

enum x86_guest_space_status x86_guest_space_firmware_execution_quarantine(
	kernel_object_handle_t machine_identity,
	const struct x86_guest_space_firmware_binding *binding)
{
	if (!identity_is_valid(machine_identity) || binding == NULL)
		return X86_GUEST_SPACE_INVALID_ARGUMENT;
	if (owner.initialized != 1u)
		return X86_GUEST_SPACE_INVALID_STATE;
	if (machine_identity != owner.machine_identity)
		return X86_GUEST_SPACE_MACHINE_MISMATCH;
	return firmware_shadow_status(
		x86_firmware_shadow_execution_quarantine(machine_identity,
							 binding));
}

enum x86_guest_space_status x86_guest_space_firmware_write_fault(
	kernel_object_handle_t machine_identity,
	const struct x86_guest_space_firmware_binding *binding,
	uint32_t page_fault_error, uint32_t fault_address)
{
	if (!identity_is_valid(machine_identity) || binding == NULL)
		return X86_GUEST_SPACE_INVALID_ARGUMENT;
	if (!guest_space_is_active())
		return X86_GUEST_SPACE_INVALID_STATE;
	if (machine_identity != owner.machine_identity)
		return X86_GUEST_SPACE_MACHINE_MISMATCH;
	return firmware_shadow_status(x86_firmware_shadow_write_fault(
		machine_identity, binding, page_fault_error, fault_address));
}

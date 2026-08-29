// SPDX-License-Identifier: GPL-2.0-only
/*
 * I/O Manager named character-device owner
 *
 * DOS contract:   none; compatibility layers translate names and controls
 * Safety changes: fixed registries, generation handles, acquire-before-publish,
 *                 and per-instance quarantine after uncertain callbacks
 */
#include "iomgr_device.h"

#define IOMGR_DEVICE_REGISTRATION_SLOTS 16u
#define IOMGR_DEVICE_INSTANCE_SLOTS 32u
#define IOMGR_DEVICE_GENERATION_MAX 0xffffffffu
#ifdef IOMGR_DEVICE_TEST_GENERATION_MAX
#define IOMGR_DEVICE_GENERATION_LIMIT \
	((uint32_t)IOMGR_DEVICE_TEST_GENERATION_MAX)
#else
#define IOMGR_DEVICE_GENERATION_LIMIT IOMGR_DEVICE_GENERATION_MAX
#endif
#define IOMGR_DEVICE_HANDLE_KIND_MASK 0xc0000000u
#define IOMGR_DEVICE_REGISTRATION_KIND 0x40000000u
#define IOMGR_DEVICE_INSTANCE_KIND 0x80000000u
#define IOMGR_DEVICE_HANDLE_SLOT_MASK 0x3fffffffu
#define IOMGR_DEVICE_CAPABILITY_MASK                                    \
	(IOMGR_DEVICE_CAP_READ | IOMGR_DEVICE_CAP_WRITE |                \
	 IOMGR_DEVICE_CAP_CONTROL)
#define IOMGR_DEVICE_STATE_MASK                                         \
	(IOMGR_DEVICE_STATE_READ_READY | IOMGR_DEVICE_STATE_WRITE_READY | \
	 IOMGR_DEVICE_STATE_END_OF_INPUT)

static_assert_expression(IOMGR_DEVICE_REGISTRATION_SLOTS <=
			 IOMGR_DEVICE_HANDLE_SLOT_MASK,
	"device registration table exceeds handle slot field");
static_assert_expression(IOMGR_DEVICE_INSTANCE_SLOTS <=
			 IOMGR_DEVICE_HANDLE_SLOT_MASK,
	"device instance table exceeds handle slot field");
static_assert_expression(IOMGR_DEVICE_REGISTRATION_SLOTS <= 32u,
	"device registration bitmap requires at most 32 slots");
static_assert_expression(IOMGR_DEVICE_INSTANCE_SLOTS <= 32u,
	"device instance bitmap requires at most 32 slots");
static_assert_expression(IOMGR_DEVICE_GENERATION_LIMIT != 0u,
	"device handle generations require a nonzero limit");
static_assert_expression(IOMGR_DEVICE_GENERATION_LIMIT <=
			 IOMGR_DEVICE_GENERATION_MAX,
	"device test generation exceeds handle generation field");

enum device_registration_state {
	DEVICE_REGISTRATION_FREE = 0,
	DEVICE_REGISTRATION_RESERVED,
	DEVICE_REGISTRATION_LIVE,
	DEVICE_REGISTRATION_RETIRED
};

enum device_instance_state {
	DEVICE_INSTANCE_FREE = 0,
	DEVICE_INSTANCE_RESERVED,
	DEVICE_INSTANCE_LIVE,
	DEVICE_INSTANCE_IN_CALLBACK,
	DEVICE_INSTANCE_QUARANTINED,
	DEVICE_INSTANCE_RETIRED
};

struct device_registration_slot {
	enum device_registration_state state;
	uint32_t generation;
	bool open_quarantined;
	size_t name_length;
	uint8_t name[IOMGR_DEVICE_NAME_MAX_BYTES];
	struct iomgr_device_ops ops;
} __aligned(8);

struct device_instance_slot {
	enum device_instance_state state;
	uint32_t generation;
	iomgr_device_registration_handle_t registration;
	kernel_object_handle_t driver_context;
} __aligned(8);

static_assert_expression(__alignof__(struct device_registration_slot) == 8u,
	"device registration slot alignment changed");
static_assert_expression(__alignof__(struct device_instance_slot) == 8u,
	"device instance slot alignment changed");

static struct {
	bool initialized;
	uint32_t registration_bitmap;
	uint32_t instance_bitmap;
	struct device_registration_slot
		registrations[IOMGR_DEVICE_REGISTRATION_SLOTS];
	struct device_instance_slot instances[IOMGR_DEVICE_INSTANCE_SLOTS];
} device_manager;

static uint32_t slot_bit(size_t index)
{
	return (uint32_t)1u << index;
}

static kernel_object_handle_t make_handle(size_t index, uint32_t generation,
					  uint32_t kind)
{
	return ((uint64_t)generation << 32) | (uint64_t)kind |
	       (uint64_t)(index + 1u);
}

static bool name_is_valid(const struct iomgr_device_name *name)
{
	return name != NULL && name->bytes != NULL && name->length != 0u &&
	       name->length <= IOMGR_DEVICE_NAME_MAX_BYTES;
}

static bool names_match(const struct device_registration_slot *slot,
			const struct iomgr_device_name *name)
{
	size_t index;

	if (slot->name_length != name->length)
		return false;
	for (index = 0u; index < name->length; ++index) {
		if (slot->name[index] != name->bytes[index])
			return false;
	}
	return true;
}

static bool ops_are_valid(const struct iomgr_device_ops *ops)
{
	bool has_read;
	bool has_write;
	bool has_control;

	if (ops == NULL || ops->abi_version != IOMGR_DEVICE_ABI_VERSION ||
	    ops->reserved != 0u || ops->reserved2 != 0u ||
	    ops->identity == 0u ||
	    ops->identity == KERNEL_OBJECT_HANDLE_INVALID ||
	    ops->context == KERNEL_OBJECT_HANDLE_INVALID ||
	    (ops->capabilities & ~IOMGR_DEVICE_CAPABILITY_MASK) != 0u ||
	    ops->open == NULL || ops->close == NULL ||
	    ops->query_info == NULL)
		return false;
	has_read = ops->read != NULL;
	has_write = ops->write != NULL;
	has_control = ops->control != NULL;
	return has_read ==
		       ((ops->capabilities & IOMGR_DEVICE_CAP_READ) != 0u) &&
	       has_write ==
		       ((ops->capabilities & IOMGR_DEVICE_CAP_WRITE) != 0u) &&
	       has_control ==
		       ((ops->capabilities & IOMGR_DEVICE_CAP_CONTROL) != 0u);
}

static size_t reserve_registration(void)
{
	size_t index;

	for (index = 0u; index < IOMGR_DEVICE_REGISTRATION_SLOTS; ++index) {
		struct device_registration_slot *slot =
			&device_manager.registrations[index];
		uint32_t bit = slot_bit(index);

		if ((device_manager.registration_bitmap & bit) != 0u)
			continue;
		device_manager.registration_bitmap |= bit;
		if (slot->generation >= IOMGR_DEVICE_GENERATION_LIMIT) {
			slot->state = DEVICE_REGISTRATION_RETIRED;
			continue;
		}
		++slot->generation;
		slot->state = DEVICE_REGISTRATION_RESERVED;
		return index;
	}
	return IOMGR_DEVICE_REGISTRATION_SLOTS;
}

static size_t reserve_instance(void)
{
	size_t index;

	for (index = 0u; index < IOMGR_DEVICE_INSTANCE_SLOTS; ++index) {
		struct device_instance_slot *slot =
			&device_manager.instances[index];
		uint32_t bit = slot_bit(index);

		if ((device_manager.instance_bitmap & bit) != 0u)
			continue;
		device_manager.instance_bitmap |= bit;
		if (slot->generation >= IOMGR_DEVICE_GENERATION_LIMIT) {
			slot->state = DEVICE_INSTANCE_RETIRED;
			continue;
		}
		++slot->generation;
		slot->state = DEVICE_INSTANCE_RESERVED;
		return index;
	}
	return IOMGR_DEVICE_INSTANCE_SLOTS;
}

static enum iomgr_status resolve_registration(
	iomgr_device_registration_handle_t handle,
	struct device_registration_slot **registration)
{
	uint32_t encoded_slot;
	uint32_t generation;
	struct device_registration_slot *slot;

	if (!device_manager.initialized)
		return IOMGR_NOT_INITIALIZED;
	if (handle == 0u ||
	    handle == IOMGR_DEVICE_REGISTRATION_HANDLE_INVALID)
		return IOMGR_STALE_HANDLE;
	encoded_slot = (uint32_t)handle;
	generation = (uint32_t)(handle >> 32);
	if ((encoded_slot & IOMGR_DEVICE_HANDLE_KIND_MASK) !=
		IOMGR_DEVICE_REGISTRATION_KIND)
		return IOMGR_STALE_HANDLE;
	encoded_slot &= IOMGR_DEVICE_HANDLE_SLOT_MASK;
	if (encoded_slot == 0u ||
	    encoded_slot > IOMGR_DEVICE_REGISTRATION_SLOTS || generation == 0u)
		return IOMGR_STALE_HANDLE;
	if ((device_manager.registration_bitmap &
	     slot_bit(encoded_slot - 1u)) == 0u)
		return IOMGR_STALE_HANDLE;
	slot = &device_manager.registrations[encoded_slot - 1u];
	if (slot->generation != generation ||
	    slot->state != DEVICE_REGISTRATION_LIVE)
		return IOMGR_STALE_HANDLE;
	*registration = slot;
	return IOMGR_OK;
}

static enum iomgr_status resolve_instance(
	iomgr_device_handle_t handle, struct device_instance_slot **instance)
{
	uint32_t encoded_slot;
	uint32_t generation;
	struct device_instance_slot *slot;

	if (!device_manager.initialized)
		return IOMGR_NOT_INITIALIZED;
	if (handle == 0u || handle == IOMGR_DEVICE_HANDLE_INVALID)
		return IOMGR_STALE_HANDLE;
	encoded_slot = (uint32_t)handle;
	generation = (uint32_t)(handle >> 32);
	if ((encoded_slot & IOMGR_DEVICE_HANDLE_KIND_MASK) !=
		IOMGR_DEVICE_INSTANCE_KIND)
		return IOMGR_STALE_HANDLE;
	encoded_slot &= IOMGR_DEVICE_HANDLE_SLOT_MASK;
	if (encoded_slot == 0u || encoded_slot > IOMGR_DEVICE_INSTANCE_SLOTS ||
	    generation == 0u)
		return IOMGR_STALE_HANDLE;
	if ((device_manager.instance_bitmap & slot_bit(encoded_slot - 1u)) == 0u)
		return IOMGR_STALE_HANDLE;
	slot = &device_manager.instances[encoded_slot - 1u];
	if (slot->generation != generation)
		return IOMGR_STALE_HANDLE;
	if (slot->state == DEVICE_INSTANCE_QUARANTINED)
		return IOMGR_POISONED;
	if (slot->state == DEVICE_INSTANCE_IN_CALLBACK)
		return IOMGR_BUSY;
	if (slot->state != DEVICE_INSTANCE_LIVE)
		return IOMGR_STALE_HANDLE;
	*instance = slot;
	return IOMGR_OK;
}

static enum iomgr_status resolve_binding(
	iomgr_device_handle_t handle, struct device_instance_slot **instance,
	struct device_registration_slot **registration, bool allow_drain)
{
	enum iomgr_status status;

	status = resolve_instance(handle, instance);
	if (status != IOMGR_OK)
		return status;
	status = resolve_registration((*instance)->registration, registration);
	if (status == IOMGR_OK &&
	    (!(*registration)->open_quarantined || allow_drain))
		return IOMGR_OK;
	if (status == IOMGR_OK)
		return IOMGR_POISONED;
	(*instance)->state = DEVICE_INSTANCE_QUARANTINED;
	return IOMGR_UNCERTAIN;
}

static enum iomgr_status map_callback_status(
	enum iomgr_device_callback_status callback_status, bool *uncertain)
{
	*uncertain = false;
	switch (callback_status) {
	case IOMGR_DEVICE_CALLBACK_OK:
		return IOMGR_OK;
	case IOMGR_DEVICE_CALLBACK_INVALID_ARGUMENT:
		return IOMGR_INVALID_ARGUMENT;
	case IOMGR_DEVICE_CALLBACK_BUSY:
		return IOMGR_BUSY;
	case IOMGR_DEVICE_CALLBACK_UNSUPPORTED:
		return IOMGR_UNSUPPORTED;
	case IOMGR_DEVICE_CALLBACK_IO_ERROR:
		return IOMGR_IO_ERROR;
	case IOMGR_DEVICE_CALLBACK_NO_SPACE:
		return IOMGR_NO_SPACE;
	case IOMGR_DEVICE_CALLBACK_NO_RESOURCES:
		return IOMGR_NO_SLOT;
	case IOMGR_DEVICE_CALLBACK_UNCERTAIN:
	default:
		*uncertain = true;
		return IOMGR_UNCERTAIN;
	}
}

static enum iomgr_status finish_instance_callback(
	struct device_instance_slot *instance,
	enum iomgr_device_callback_status callback_status)
{
	bool uncertain;
	enum iomgr_status status =
		map_callback_status(callback_status, &uncertain);

	if (uncertain)
		instance->state = DEVICE_INSTANCE_QUARANTINED;
	else
		instance->state = DEVICE_INSTANCE_LIVE;
	return status;
}

static void begin_instance_callback(struct device_instance_slot *instance)
{
	instance->state = DEVICE_INSTANCE_IN_CALLBACK;
	__asm__ volatile("" ::: "memory");
}

static void release_instance(size_t index,
			     struct device_instance_slot *instance)
{
	instance->registration = IOMGR_DEVICE_REGISTRATION_HANDLE_INVALID;
	instance->driver_context = KERNEL_OBJECT_HANDLE_INVALID;
	instance->state = DEVICE_INSTANCE_FREE;
	__asm__ volatile("" ::: "memory");
	device_manager.instance_bitmap &= ~slot_bit(index);
}

enum iomgr_status iomgr_device_initialize(void)
{
	size_t index;

	if (device_manager.initialized)
		return IOMGR_ALREADY_INITIALIZED;
	device_manager.registration_bitmap = 0u;
	device_manager.instance_bitmap = 0u;
	for (index = 0u; index < IOMGR_DEVICE_REGISTRATION_SLOTS; ++index) {
		device_manager.registrations[index].state =
			DEVICE_REGISTRATION_FREE;
		device_manager.registrations[index].generation = 0u;
		device_manager.registrations[index].open_quarantined = false;
	}
	for (index = 0u; index < IOMGR_DEVICE_INSTANCE_SLOTS; ++index) {
		device_manager.instances[index].state = DEVICE_INSTANCE_FREE;
		device_manager.instances[index].generation = 0u;
	}
	device_manager.initialized = true;
	return IOMGR_OK;
}

enum iomgr_status iomgr_device_register(
	const struct iomgr_device_name *name,
	const struct iomgr_device_ops *ops,
	iomgr_device_registration_handle_t *registration)
{
	struct device_registration_slot *slot;
	size_t index;
	size_t slot_index;

	if (!device_manager.initialized)
		return IOMGR_NOT_INITIALIZED;
	if (registration == NULL || !name_is_valid(name) ||
	    !ops_are_valid(ops))
		return IOMGR_INVALID_ARGUMENT;
	for (index = 0u; index < IOMGR_DEVICE_REGISTRATION_SLOTS; ++index) {
		slot = &device_manager.registrations[index];
		if ((device_manager.registration_bitmap & slot_bit(index)) != 0u &&
		    slot->state == DEVICE_REGISTRATION_LIVE &&
		    names_match(slot, name))
			return IOMGR_ALREADY_EXISTS;
	}
	slot_index = reserve_registration();
	if (slot_index == IOMGR_DEVICE_REGISTRATION_SLOTS)
		return IOMGR_NO_SLOT;
	slot = &device_manager.registrations[slot_index];
	slot->open_quarantined = false;
	slot->name_length = name->length;
	for (index = 0u; index < IOMGR_DEVICE_NAME_MAX_BYTES; ++index) {
		slot->name[index] =
			index < name->length ? name->bytes[index] : 0u;
	}
	slot->ops = *ops;
	__asm__ volatile("" ::: "memory");
	slot->state = DEVICE_REGISTRATION_LIVE;
	*registration = make_handle(slot_index, slot->generation,
				    IOMGR_DEVICE_REGISTRATION_KIND);
	return IOMGR_OK;
}

enum iomgr_status iomgr_device_unregister(
	iomgr_device_registration_handle_t registration)
{
	struct device_registration_slot *slot;
	enum iomgr_status status;
	bool busy = false;
	size_t index;

	status = resolve_registration(registration, &slot);
	if (status != IOMGR_OK)
		return status;
	if (slot->open_quarantined)
		return IOMGR_POISONED;
	for (index = 0u; index < IOMGR_DEVICE_INSTANCE_SLOTS; ++index) {
		const struct device_instance_slot *instance =
			&device_manager.instances[index];

		if ((device_manager.instance_bitmap & slot_bit(index)) == 0u)
			continue;
		if (instance->registration != registration)
			continue;
		if (instance->state == DEVICE_INSTANCE_QUARANTINED)
			return IOMGR_POISONED;
		if (instance->state == DEVICE_INSTANCE_RESERVED ||
		    instance->state == DEVICE_INSTANCE_LIVE ||
		    instance->state == DEVICE_INSTANCE_IN_CALLBACK)
			busy = true;
	}
	if (busy)
		return IOMGR_BUSY;
	slot->name_length = 0u;
	slot->open_quarantined = false;
	slot->ops = (struct iomgr_device_ops){ 0 };
	__asm__ volatile("" ::: "memory");
	slot->state = DEVICE_REGISTRATION_FREE;
	device_manager.registration_bitmap &=
		~slot_bit(((uint32_t)registration &
			   IOMGR_DEVICE_HANDLE_SLOT_MASK) - 1u);
	return IOMGR_OK;
}

enum iomgr_status iomgr_device_open(const struct iomgr_device_name *name,
				    iomgr_device_handle_t *device)
{
	struct device_registration_slot *registration = NULL;
	struct device_instance_slot *instance;
	kernel_object_handle_t driver_context = KERNEL_OBJECT_HANDLE_INVALID;
	enum iomgr_device_callback_status callback_status;
	enum iomgr_status status;
	bool callback_uncertain;
	size_t registration_index;
	size_t instance_index;

	if (!device_manager.initialized)
		return IOMGR_NOT_INITIALIZED;
	if (device == NULL || !name_is_valid(name))
		return IOMGR_INVALID_ARGUMENT;
	for (registration_index = 0u;
	     registration_index < IOMGR_DEVICE_REGISTRATION_SLOTS;
	     ++registration_index) {
		struct device_registration_slot *candidate =
			&device_manager.registrations[registration_index];

		if ((device_manager.registration_bitmap &
		     slot_bit(registration_index)) != 0u &&
		    candidate->state == DEVICE_REGISTRATION_LIVE &&
		    names_match(candidate, name)) {
			registration = candidate;
			break;
		}
	}
	if (registration == NULL)
		return IOMGR_NOT_FOUND;
	if (registration->open_quarantined)
		return IOMGR_POISONED;
	instance_index = reserve_instance();
	if (instance_index == IOMGR_DEVICE_INSTANCE_SLOTS)
		return IOMGR_NO_SLOT;
	instance = &device_manager.instances[instance_index];
	instance->registration = make_handle(
		registration_index, registration->generation,
		IOMGR_DEVICE_REGISTRATION_KIND);
	callback_status = registration->ops.open(registration->ops.context,
						 &driver_context);
	status = map_callback_status(callback_status, &callback_uncertain);
	if (callback_uncertain)
		registration->open_quarantined = true;
	if (status != IOMGR_OK) {
		release_instance(instance_index, instance);
		return status;
	}
	if (driver_context == 0u ||
	    driver_context == KERNEL_OBJECT_HANDLE_INVALID) {
		registration->open_quarantined = true;
		release_instance(instance_index, instance);
		return IOMGR_UNCERTAIN;
	}
	instance->driver_context = driver_context;
	__asm__ volatile("" ::: "memory");
	instance->state = DEVICE_INSTANCE_LIVE;
	*device = make_handle(instance_index, instance->generation,
			      IOMGR_DEVICE_INSTANCE_KIND);
	return IOMGR_OK;
}

enum iomgr_status iomgr_device_close(iomgr_device_handle_t device)
{
	struct device_registration_slot *registration;
	struct device_instance_slot *instance;
	enum iomgr_device_callback_status callback_status;
	enum iomgr_status status;

	status = resolve_binding(device, &instance, &registration, true);
	if (status != IOMGR_OK)
		return status;
	begin_instance_callback(instance);
	callback_status = registration->ops.close(
		registration->ops.context, instance->driver_context);
	status = finish_instance_callback(instance, callback_status);
	if (status != IOMGR_OK)
		return status;
	release_instance(((uint32_t)device &
			  IOMGR_DEVICE_HANDLE_SLOT_MASK) - 1u,
			 instance);
	return IOMGR_OK;
}

enum iomgr_status iomgr_device_read(iomgr_device_handle_t device,
				    uint8_t *destination, size_t capacity,
				    size_t count, size_t *bytes_read)
{
	struct device_registration_slot *registration;
	struct device_instance_slot *instance;
	enum iomgr_device_callback_status callback_status;
	enum iomgr_status status;
	size_t completed = 0u;

	if (bytes_read == NULL || count > capacity ||
	    (capacity != 0u && destination == NULL))
		return IOMGR_INVALID_ARGUMENT;
	status = resolve_binding(device, &instance, &registration, false);
	if (status != IOMGR_OK)
		return status;
	if ((registration->ops.capabilities & IOMGR_DEVICE_CAP_READ) == 0u)
		return IOMGR_UNSUPPORTED;
	begin_instance_callback(instance);
	callback_status = registration->ops.read(
		registration->ops.context, instance->driver_context, destination,
		capacity, count, &completed);
	status = finish_instance_callback(instance, callback_status);
	if (status != IOMGR_OK)
		return status;
	if (completed > count) {
		instance->state = DEVICE_INSTANCE_QUARANTINED;
		return IOMGR_UNCERTAIN;
	}
	*bytes_read = completed;
	return IOMGR_OK;
}

enum iomgr_status iomgr_device_write(iomgr_device_handle_t device,
				     const uint8_t *source,
				     size_t source_capacity, size_t count,
				     size_t *bytes_written)
{
	struct device_registration_slot *registration;
	struct device_instance_slot *instance;
	enum iomgr_device_callback_status callback_status;
	enum iomgr_status status;
	size_t completed = 0u;

	if (bytes_written == NULL || count > source_capacity ||
	    (source_capacity != 0u && source == NULL))
		return IOMGR_INVALID_ARGUMENT;
	status = resolve_binding(device, &instance, &registration, false);
	if (status != IOMGR_OK)
		return status;
	if ((registration->ops.capabilities & IOMGR_DEVICE_CAP_WRITE) == 0u)
		return IOMGR_UNSUPPORTED;
	begin_instance_callback(instance);
	callback_status = registration->ops.write(
		registration->ops.context, instance->driver_context, source,
		source_capacity, count, &completed);
	status = finish_instance_callback(instance, callback_status);
	if (status != IOMGR_OK)
		return status;
	if (completed > count) {
		instance->state = DEVICE_INSTANCE_QUARANTINED;
		return IOMGR_UNCERTAIN;
	}
	*bytes_written = completed;
	return IOMGR_OK;
}

enum iomgr_status iomgr_device_control(
	iomgr_device_handle_t device, uint64_t operation, const uint8_t *input,
	size_t input_capacity, size_t input_count, uint8_t *output,
	size_t output_capacity, size_t *bytes_returned)
{
	struct device_registration_slot *registration;
	struct device_instance_slot *instance;
	enum iomgr_device_callback_status callback_status;
	enum iomgr_status status;
	size_t completed = 0u;

	if (bytes_returned == NULL || input_count > input_capacity ||
	    (input_capacity != 0u && input == NULL) ||
	    (output_capacity != 0u && output == NULL))
		return IOMGR_INVALID_ARGUMENT;
	status = resolve_binding(device, &instance, &registration, false);
	if (status != IOMGR_OK)
		return status;
	if ((registration->ops.capabilities & IOMGR_DEVICE_CAP_CONTROL) == 0u)
		return IOMGR_UNSUPPORTED;
	begin_instance_callback(instance);
	callback_status = registration->ops.control(
		registration->ops.context, instance->driver_context, operation,
		input, input_capacity, input_count, output, output_capacity,
		&completed);
	status = finish_instance_callback(instance, callback_status);
	if (status != IOMGR_OK)
		return status;
	if (completed > output_capacity) {
		instance->state = DEVICE_INSTANCE_QUARANTINED;
		return IOMGR_UNCERTAIN;
	}
	*bytes_returned = completed;
	return IOMGR_OK;
}

static bool query_result_is_valid(
	const struct iomgr_device_ops *ops,
	const struct iomgr_device_query_result *result)
{
	if (result->reserved != 0u ||
	    (result->state & ~IOMGR_DEVICE_STATE_MASK) != 0u)
		return false;
	if ((ops->capabilities & IOMGR_DEVICE_CAP_READ) == 0u &&
	    (result->pending_read_bytes != 0u ||
	     (result->state & (IOMGR_DEVICE_STATE_READ_READY |
			      IOMGR_DEVICE_STATE_END_OF_INPUT)) != 0u))
		return false;
	if ((ops->capabilities & IOMGR_DEVICE_CAP_WRITE) == 0u &&
	    (result->state & IOMGR_DEVICE_STATE_WRITE_READY) != 0u)
		return false;
	return true;
}

enum iomgr_status iomgr_device_query_info(iomgr_device_handle_t device,
					  struct iomgr_device_info *info)
{
	struct iomgr_device_query_result query = { 0 };
	struct iomgr_device_info prepared = { 0 };
	struct device_registration_slot *registration;
	struct device_instance_slot *instance;
	enum iomgr_device_callback_status callback_status;
	enum iomgr_status status;
	size_t index;

	if (info == NULL)
		return IOMGR_INVALID_ARGUMENT;
	status = resolve_binding(device, &instance, &registration, false);
	if (status != IOMGR_OK)
		return status;
	begin_instance_callback(instance);
	callback_status = registration->ops.query_info(
		registration->ops.context, instance->driver_context, &query);
	status = finish_instance_callback(instance, callback_status);
	if (status != IOMGR_OK)
		return status;
	if (!query_result_is_valid(&registration->ops, &query)) {
		instance->state = DEVICE_INSTANCE_QUARANTINED;
		return IOMGR_UNCERTAIN;
	}
	prepared.identity = registration->ops.identity;
	prepared.pending_read_bytes = query.pending_read_bytes;
	prepared.capabilities = registration->ops.capabilities;
	prepared.state = query.state;
	prepared.name_length = (uint16_t)registration->name_length;
	for (index = 0u; index < registration->name_length; ++index)
		prepared.name[index] = registration->name[index];
	*info = prepared;
	return IOMGR_OK;
}

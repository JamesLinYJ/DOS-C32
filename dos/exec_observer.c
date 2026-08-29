// SPDX-License-Identifier: GPL-2.0-only
/*
 * DOS EXEC observation exclusion
 *
 * Compatibility contract: no child executes before its complete PSP/image publication
 * Safety change: explicit executor ownership across callback-driven prepares
 */
#include "dos_exec_observer.h"

static bool identity_is_valid(kernel_object_handle_t identity)
{
	return identity != 0u && identity != KERNEL_OBJECT_HANDLE_INVALID;
}

static bool ops_are_complete(const struct dos_exec_observer_ops *ops)
{
	return ops != NULL && identity_is_valid(ops->identity) &&
	       ops->acquire != NULL && ops->release != NULL &&
	       ops->quarantine != NULL;
}

static bool reserved_bytes_are_zero(const struct dos_exec_observer *observer)
{
	size_t index;

	for (index = 0u; index < ARRAY_SIZE(observer->reserved); ++index) {
		if (observer->reserved[index] != 0u)
			return false;
	}
	return true;
}

static bool
observer_has_valid_encoding(const struct dos_exec_observer *observer)
{
	return observer != NULL && observer->constructed == 1u &&
	       observer->state <= (uint8_t)DOS_EXEC_OBSERVER_STATE_POISONED &&
	       reserved_bytes_are_zero(observer);
}

static enum dos_exec_observer_status
poison_uncertain_acquire(struct dos_exec_observer *observer,
			 const struct dos_exec_observer_ops *ops,
			 kernel_object_handle_t context, uint64_t generation)
{
	observer->adapter_identity = ops->identity;
	observer->context = context;
	observer->generation = generation;
	observer->state = (uint8_t)DOS_EXEC_OBSERVER_STATE_POISONED;
	(void)ops->quarantine(context, generation);
	return DOS_EXEC_OBSERVER_POISONED;
}

static enum dos_exec_observer_status
validate_adapter(const struct dos_exec_observer *observer,
		 const struct dos_exec_observer_ops *ops,
		 kernel_object_handle_t context)
{
	if (!ops_are_complete(ops))
		return DOS_EXEC_OBSERVER_INVALID_ARGUMENT;
	if (ops->identity != observer->adapter_identity)
		return DOS_EXEC_OBSERVER_IDENTITY_MISMATCH;
	if (context != observer->context)
		return DOS_EXEC_OBSERVER_CONTEXT_MISMATCH;
	return DOS_EXEC_OBSERVER_OK;
}

enum dos_exec_observer_status
dos_exec_observer_construct(struct dos_exec_observer *observer)
{
	if (observer == NULL)
		return DOS_EXEC_OBSERVER_INVALID_ARGUMENT;
	*observer = (struct dos_exec_observer)DOS_EXEC_OBSERVER_INITIALIZER;
	return DOS_EXEC_OBSERVER_OK;
}

enum dos_exec_observer_status
dos_exec_observer_acquire(struct dos_exec_observer *observer,
			  const struct dos_exec_observer_ops *ops,
			  kernel_object_handle_t context)
{
	uint64_t generation = 0u;
	enum dos_exec_observer_adapter_status adapter_status;

	if (!observer_has_valid_encoding(observer) || !ops_are_complete(ops) ||
	    context == KERNEL_OBJECT_HANDLE_INVALID)
		return DOS_EXEC_OBSERVER_INVALID_ARGUMENT;
	if (observer->state == DOS_EXEC_OBSERVER_STATE_POISONED)
		return DOS_EXEC_OBSERVER_POISONED;
	if (observer->state != DOS_EXEC_OBSERVER_STATE_IDLE)
		return DOS_EXEC_OBSERVER_INVALID_STATE;

	observer->state = DOS_EXEC_OBSERVER_STATE_ACQUIRING;
	adapter_status = ops->acquire(context, &generation);
	if (adapter_status == DOS_EXEC_OBSERVER_ADAPTER_BUSY) {
		if (generation != 0u)
			return poison_uncertain_acquire(observer, ops, context,
							generation);
		observer->state = DOS_EXEC_OBSERVER_STATE_IDLE;
		return DOS_EXEC_OBSERVER_BUSY;
	}
	if (adapter_status == DOS_EXEC_OBSERVER_ADAPTER_FAULT) {
		if (generation != 0u)
			return poison_uncertain_acquire(observer, ops, context,
							generation);
		observer->state = DOS_EXEC_OBSERVER_STATE_IDLE;
		return DOS_EXEC_OBSERVER_BACKEND_FAULT;
	}
	if (adapter_status != DOS_EXEC_OBSERVER_ADAPTER_OK || generation == 0u)
		return poison_uncertain_acquire(observer, ops, context,
						generation);
	observer->adapter_identity = ops->identity;
	observer->context = context;
	observer->generation = generation;
	observer->state = DOS_EXEC_OBSERVER_STATE_HELD;
	return DOS_EXEC_OBSERVER_OK;
}

enum dos_exec_observer_status
dos_exec_observer_validate_held(const struct dos_exec_observer *observer,
				const struct dos_exec_observer_ops *ops,
				kernel_object_handle_t context)
{
	enum dos_exec_observer_status status;

	if (!observer_has_valid_encoding(observer))
		return DOS_EXEC_OBSERVER_INVALID_ARGUMENT;
	if (observer->state == DOS_EXEC_OBSERVER_STATE_POISONED)
		return DOS_EXEC_OBSERVER_POISONED;
	if (observer->state != DOS_EXEC_OBSERVER_STATE_HELD ||
	    observer->generation == 0u)
		return DOS_EXEC_OBSERVER_INVALID_STATE;
	status = validate_adapter(observer, ops, context);
	return status;
}

enum dos_exec_observer_status
dos_exec_observer_release(struct dos_exec_observer *observer,
			  const struct dos_exec_observer_ops *ops,
			  kernel_object_handle_t context)
{
	enum dos_exec_observer_adapter_status adapter_status;
	enum dos_exec_observer_status status;

	if (!observer_has_valid_encoding(observer))
		return DOS_EXEC_OBSERVER_INVALID_ARGUMENT;
	if (observer->state == DOS_EXEC_OBSERVER_STATE_POISONED)
		return DOS_EXEC_OBSERVER_POISONED;
	if (observer->state == DOS_EXEC_OBSERVER_STATE_RELEASED)
		return DOS_EXEC_OBSERVER_OK;
	if (observer->state != DOS_EXEC_OBSERVER_STATE_HELD ||
	    observer->generation == 0u)
		return DOS_EXEC_OBSERVER_INVALID_STATE;
	status = validate_adapter(observer, ops, context);
	if (status != DOS_EXEC_OBSERVER_OK)
		return status;

	observer->state = DOS_EXEC_OBSERVER_STATE_RELEASING;
	adapter_status = ops->release(context, observer->generation);
	if (adapter_status != DOS_EXEC_OBSERVER_ADAPTER_OK) {
		observer->state = DOS_EXEC_OBSERVER_STATE_POISONED;
		(void)ops->quarantine(context, observer->generation);
		return DOS_EXEC_OBSERVER_POISONED;
	}
	observer->state = DOS_EXEC_OBSERVER_STATE_RELEASED;
	return DOS_EXEC_OBSERVER_OK;
}

enum dos_exec_observer_status
dos_exec_observer_poison(struct dos_exec_observer *observer,
			 const struct dos_exec_observer_ops *ops,
			 kernel_object_handle_t context)
{
	enum dos_exec_observer_adapter_status adapter_status;
	enum dos_exec_observer_status status;

	if (!observer_has_valid_encoding(observer))
		return DOS_EXEC_OBSERVER_INVALID_ARGUMENT;
	if (observer->state == DOS_EXEC_OBSERVER_STATE_POISONED)
		return DOS_EXEC_OBSERVER_OK;
	if (observer->state != DOS_EXEC_OBSERVER_STATE_HELD ||
	    observer->generation == 0u)
		return DOS_EXEC_OBSERVER_INVALID_STATE;
	status = validate_adapter(observer, ops, context);
	if (status != DOS_EXEC_OBSERVER_OK)
		return status;
	observer->state = DOS_EXEC_OBSERVER_STATE_POISONED;
	adapter_status = ops->quarantine(context, observer->generation);
	return adapter_status == DOS_EXEC_OBSERVER_ADAPTER_OK
		   ? DOS_EXEC_OBSERVER_OK
		   : DOS_EXEC_OBSERVER_BACKEND_FAULT;
}

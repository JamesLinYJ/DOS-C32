// SPDX-License-Identifier: GPL-2.0-only
/*
 * Standard named EMS endpoint behind the filesystem-neutral I/O Manager.
 *
 * The legacy strategy-request ABI is isolated from the 32-bit kernel. DOS
 * open/close semantics cross the generic device -> SFT -> JFT bridge, while
 * INT 67h remains owned by the DOS personality.
 */
#include "dos_ems_device.h"

#include "dos_personality.h"

static bool valid_identity(kernel_object_handle_t identity)
{
	return identity != 0u && identity != KERNEL_OBJECT_HANDLE_INVALID;
}

static enum iomgr_device_callback_status ems_device_open(
	kernel_object_handle_t context,
	kernel_object_handle_t *instance_context)
{
	if (!valid_identity(context) || instance_context == NULL)
		return IOMGR_DEVICE_CALLBACK_INVALID_ARGUMENT;
	*instance_context = context;
	return IOMGR_DEVICE_CALLBACK_OK;
}

static enum iomgr_device_callback_status ems_device_close(
	kernel_object_handle_t context,
	kernel_object_handle_t instance_context)
{
	return valid_identity(context) && instance_context == context
		       ? IOMGR_DEVICE_CALLBACK_OK
		       : IOMGR_DEVICE_CALLBACK_INVALID_ARGUMENT;
}

static enum iomgr_device_callback_status ems_device_query_info(
	kernel_object_handle_t context,
	kernel_object_handle_t instance_context,
	struct iomgr_device_query_result *result)
{
	if (!valid_identity(context) || instance_context != context ||
	    result == NULL)
		return IOMGR_DEVICE_CALLBACK_INVALID_ARGUMENT;
	*result = (struct iomgr_device_query_result){0};
	return IOMGR_DEVICE_CALLBACK_OK;
}

static enum iomgr_status register_configured_device(
	const struct dos_ems_runtime_config *config,
	kernel_object_handle_t device_identity,
	kernel_object_handle_t device_context,
	iomgr_device_registration_handle_t *registration)
{
	struct iomgr_device_name name;
	struct iomgr_device_ops ops;

	if (!valid_identity(device_identity) || !valid_identity(device_context) ||
	    registration == NULL || !dos_ems_runtime_config_is_valid(config))
		return IOMGR_INVALID_ARGUMENT;
	name = (struct iomgr_device_name){
		.bytes = config->device_name,
		.length = DOS_EMS_DEVICE_NAME_BYTES,
	};
	ops = (struct iomgr_device_ops){
		.abi_version = IOMGR_DEVICE_ABI_VERSION,
		.reserved = 0u,
		.identity = device_identity,
		.context = device_context,
		.capabilities = 0u,
		.reserved2 = 0u,
		.open = ems_device_open,
		.close = ems_device_close,
		.read = NULL,
		.write = NULL,
		.control = NULL,
		.query_info = ems_device_query_info,
	};
	return iomgr_device_register(&name, &ops, registration);
}

enum iomgr_status dos_ems_device_register(
	const struct dos_personality *personality,
	kernel_object_handle_t device_identity,
	kernel_object_handle_t device_context,
	iomgr_device_registration_handle_t *registration)
{
	struct dos_ems_runtime_config config;

	if (!valid_identity(device_identity) || !valid_identity(device_context) ||
	    registration == NULL ||
	    !dos_personality_ems_config_snapshot(personality, &config))
		return IOMGR_INVALID_ARGUMENT;
	return register_configured_device(&config, device_identity,
					  device_context, registration);
}

enum dos_ems_publication_status dos_ems_runtime_publish(
	struct dos_personality *personality,
	const struct dos_ems_page_ops *page_ops,
	kernel_object_handle_t page_context,
	const struct dos_ems_page_frame_binding *page_frame,
	const struct dos_vcpi_platform_ops *vcpi_ops,
	kernel_object_handle_t vcpi_context,
	const struct dos_ems_runtime_config *config,
	kernel_object_handle_t device_identity,
	kernel_object_handle_t device_context,
	iomgr_device_registration_handle_t *registration)
{
	struct dos_ems_manager prepared;
	iomgr_device_registration_handle_t prepared_registration =
		IOMGR_DEVICE_REGISTRATION_HANDLE_INVALID;
	enum dos_personality_status personality_status;
	enum dos_ems_status ems_status;
	enum iomgr_status device_status;

	if (personality == NULL || registration == NULL ||
	    !dos_ems_runtime_config_is_valid(config))
		return DOS_EMS_PUBLICATION_INVALID_ARGUMENT;
	dos_ems_construct(&prepared);
	ems_status = dos_ems_initialize(
		&prepared, page_ops, page_context, page_frame, vcpi_ops,
		vcpi_context, &config->service);
	if (ems_status == DOS_EMS_MEMORY_FAULT)
		return DOS_EMS_PUBLICATION_MACHINE_FAULT;
	if (ems_status == DOS_EMS_POISONED)
		return DOS_EMS_PUBLICATION_POISONED;
	if (ems_status != DOS_EMS_READY)
		return DOS_EMS_PUBLICATION_INVALID_ARGUMENT;
	device_status = register_configured_device(
		config, device_identity, device_context, &prepared_registration);
	if (device_status == IOMGR_ALREADY_EXISTS)
		return DOS_EMS_PUBLICATION_CONFLICT;
	if (device_status != IOMGR_OK)
		return device_status == IOMGR_INVALID_ARGUMENT
			       ? DOS_EMS_PUBLICATION_INVALID_ARGUMENT
			       : DOS_EMS_PUBLICATION_MACHINE_FAULT;
	personality_status = dos_personality_publish_ems(
		personality, &prepared, config);
	if (personality_status != DOS_PERSONALITY_READY) {
		if (iomgr_device_unregister(prepared_registration) != IOMGR_OK)
			return DOS_EMS_PUBLICATION_POISONED;
		return personality_status == DOS_PERSONALITY_MACHINE_FAULT
			       ? DOS_EMS_PUBLICATION_MACHINE_FAULT
			       : DOS_EMS_PUBLICATION_INVALID_ARGUMENT;
	}
	*registration = prepared_registration;
	return DOS_EMS_PUBLICATION_READY;
}

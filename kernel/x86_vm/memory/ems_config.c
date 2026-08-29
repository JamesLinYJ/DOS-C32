// SPDX-License-Identifier: GPL-2.0-only
/*
 * One auditable legacy-PC EMS candidate and validation boundary.
 *
 * E000h is only the preferred 64 KiB UMA window.  It becomes DOS-visible only
 * after an injected platform owner validates current page tables, mapping
 * capability and all device/resource conflicts, then returns an exclusive
 * lease.  No ROM scan or compiled address is treated as hardware discovery.
 */
#include "x86_ems_config.h"

#include "x86_paging.h"

static const struct dos_ems_runtime_config legacy_pc_ems_config = {
	.service = {
		.page_frame_segment = 0xe000u,
		.reserved = 0u,
		.reserved2 = 0u,
	},
	.device_name = {'E', 'M', 'M', 'X', 'X', 'X', 'X', '0'},
	.reserved = {0u},
};

static bool valid_identity(kernel_object_handle_t identity)
{
	return identity != 0u && identity != KERNEL_OBJECT_HANDLE_INVALID;
}

static bool complete_page_frame_ops(
	const struct dos_ems_page_frame_ops *ops)
{
	return ops != NULL && ops->acquire != NULL && ops->release != NULL &&
	       ops->map != NULL && ops->unmap != NULL;
}

static enum x86_ems_runtime_config_status map_acquire_status(
	enum dos_ems_page_frame_status status)
{
	switch (status) {
	case DOS_EMS_PAGE_FRAME_OK:
		return X86_EMS_RUNTIME_CONFIG_READY;
	case DOS_EMS_PAGE_FRAME_UNAVAILABLE:
		return X86_EMS_RUNTIME_CONFIG_UNAVAILABLE;
	case DOS_EMS_PAGE_FRAME_CONFLICT:
		return X86_EMS_RUNTIME_CONFIG_CONFLICT;
	case DOS_EMS_PAGE_FRAME_FAULT:
		return X86_EMS_RUNTIME_CONFIG_FAULT;
	case DOS_EMS_PAGE_FRAME_UNCERTAIN:
	default:
		return X86_EMS_RUNTIME_CONFIG_POISONED;
	}
}

bool x86_ems_runtime_config_candidate(struct dos_ems_runtime_config *config)
{
	if (config == NULL ||
	    !dos_ems_runtime_config_is_valid(&legacy_pc_ems_config))
		return false;
	*config = legacy_pc_ems_config;
	return true;
}

enum x86_ems_runtime_config_status x86_ems_runtime_config_resolve(
	const struct dos_ems_page_frame_ops *ops,
	kernel_object_handle_t context,
	struct x86_ems_runtime_binding *binding)
{
	struct x86_ems_runtime_binding prepared = {0};
	dos_ems_page_frame_lease_t lease = DOS_EMS_PAGE_FRAME_LEASE_INVALID;
	uint64_t frame_address;
	uint64_t frame_bytes = DOS_EMS_PAGE_BYTES *
			       DOS_EMS_PAGE_FRAME_SLOTS;
	enum dos_ems_page_frame_status frame_status;
	enum x86_ems_runtime_config_status status;

	if (binding == NULL)
		return X86_EMS_RUNTIME_CONFIG_INVALID_ARGUMENT;
	/* Missing any lifecycle or mapper operation means there is no EMS
	 * capability to publish, not a partially usable service. */
	if (!complete_page_frame_ops(ops))
		return X86_EMS_RUNTIME_CONFIG_UNAVAILABLE;
	if (!valid_identity(context) ||
	    !x86_ems_runtime_config_candidate(&prepared.config))
		return X86_EMS_RUNTIME_CONFIG_INVALID_ARGUMENT;
	frame_address = (uint64_t)prepared.config.service.page_frame_segment <<
			4u;
	/* Platform policy permits an EMS frame only in the non-video UMA.  The
	 * injected owner performs the finer firmware/device conflict checks. */
	if (frame_address < X86_DOS_VIDEO_LIMIT ||
	    frame_address >= X86_LEGACY_ROM_LIMIT ||
	    frame_bytes > X86_LEGACY_ROM_LIMIT - frame_address)
		return X86_EMS_RUNTIME_CONFIG_INVALID_ARGUMENT;
	frame_status = ops->acquire(context, frame_address, frame_bytes, &lease);
	status = map_acquire_status(frame_status);
	if (status != X86_EMS_RUNTIME_CONFIG_READY)
		return status;
	if (lease == DOS_EMS_PAGE_FRAME_LEASE_INVALID)
		return X86_EMS_RUNTIME_CONFIG_POISONED;
	prepared.page_frame = (struct dos_ems_page_frame_binding){
		.ops = ops,
		.context = context,
		.lease = lease,
		.linear_address = frame_address,
		.byte_count = frame_bytes,
	};
	prepared.acquired = 1u;
	*binding = prepared;
	return X86_EMS_RUNTIME_CONFIG_READY;
}

enum x86_ems_runtime_config_status x86_ems_runtime_config_release(
	struct x86_ems_runtime_binding *binding)
{
	enum dos_ems_page_frame_status frame_status;
	enum x86_ems_runtime_config_status status;

	if (binding == NULL || binding->acquired != 1u ||
	    !complete_page_frame_ops(binding->page_frame.ops) ||
	    !valid_identity(binding->page_frame.context) ||
	    binding->page_frame.lease == DOS_EMS_PAGE_FRAME_LEASE_INVALID)
		return X86_EMS_RUNTIME_CONFIG_INVALID_ARGUMENT;
	frame_status = binding->page_frame.ops->release(
		binding->page_frame.context, binding->page_frame.lease);
	status = map_acquire_status(frame_status);
	if (status == X86_EMS_RUNTIME_CONFIG_READY) {
		*binding = (struct x86_ems_runtime_binding){0};
		return X86_EMS_RUNTIME_CONFIG_READY;
	}
	/* A failed release retains the lease for an exact retry.  Uncertainty is
	 * sticky and the owner must quarantine the resource. */
	return status == X86_EMS_RUNTIME_CONFIG_POISONED
		       ? status
		       : X86_EMS_RUNTIME_CONFIG_FAULT;
}

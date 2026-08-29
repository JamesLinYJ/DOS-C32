// SPDX-License-Identifier: GPL-2.0-only
/* Explicit capability publication for the x86 VCPI execution backend. */
#include "x86_vcpi_execution.h"

static bool valid_identity(kernel_object_handle_t identity)
{
	return identity != 0u && identity != KERNEL_OBJECT_HANDLE_INVALID;
}

static bool complete_operations(const struct dos_vcpi_platform_ops *ops)
{
	return ops != NULL && ops->translate_low_page != NULL &&
	       ops->read_virtual_cr0 != NULL &&
	       ops->query_pic_mappings != NULL &&
	       ops->set_pic_mappings != NULL && ops->handoff != NULL;
}

void x86_vcpi_execution_construct(
	struct x86_vcpi_execution_binding *binding)
{
	uint32_t index;

	if (binding == NULL)
		return;
	binding->ops = NULL;
	binding->context = KERNEL_OBJECT_HANDLE_INVALID;
	binding->initialized = 0u;
	binding->constructed = 1u;
	for (index = 0u; index < ARRAY_SIZE(binding->reserved); ++index)
		binding->reserved[index] = 0u;
}

enum x86_vcpi_execution_status x86_vcpi_execution_bind(
	struct x86_vcpi_execution_binding *binding,
	const struct dos_vcpi_platform_ops *ops,
	kernel_object_handle_t context)
{
	if (binding == NULL || binding->constructed != 1u ||
	    !complete_operations(ops) || !valid_identity(context))
		return X86_VCPI_EXECUTION_INVALID_ARGUMENT;
	if (binding->initialized != 0u)
		return X86_VCPI_EXECUTION_INVALID_STATE;
	binding->ops = ops;
	binding->context = context;
	binding->initialized = 1u;
	return X86_VCPI_EXECUTION_OK;
}

enum x86_vcpi_execution_status x86_vcpi_execution_resolve(
	const struct x86_vcpi_execution_binding *binding,
	const struct dos_vcpi_platform_ops **ops,
	kernel_object_handle_t *context)
{
	if (binding == NULL || binding->constructed != 1u || ops == NULL ||
	    context == NULL)
		return X86_VCPI_EXECUTION_INVALID_ARGUMENT;
	if (binding->initialized != 1u ||
	    !complete_operations(binding->ops) ||
	    !valid_identity(binding->context))
		return X86_VCPI_EXECUTION_UNAVAILABLE;
	*ops = binding->ops;
	*context = binding->context;
	return X86_VCPI_EXECUTION_OK;
}

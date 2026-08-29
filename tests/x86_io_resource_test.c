// SPDX-License-Identifier: GPL-2.0-only
/* Host tests for the generation-bound x86 I/O resource registry. */
#include "x86_io_resource.h"

#define REGISTRY_IDENTITY ((kernel_object_handle_t)0x5245474953545259ull)
#define OWNER_IDENTITY ((kernel_object_handle_t)0x4f574e4552303031ull)
#define OTHER_OWNER_IDENTITY ((kernel_object_handle_t)0x4f574e4552303032ull)
#define REQUESTER_IDENTITY ((kernel_object_handle_t)0x5245515545535431ull)
#define OTHER_REQUESTER_IDENTITY                                           \
	((kernel_object_handle_t)0x5245515545535432ull)
#define CALLBACK_CONTEXT ((kernel_object_handle_t)0x43414c4c4241434bull)

static uint32_t model_read_value = 0x5au;
static uint32_t model_write_value;
static enum x86_io_callback_status model_read_status = X86_IO_CALLBACK_OK;
static enum x86_io_resource_status mutation_status;
static x86_io_resource_handle_t mutation_resource;

static enum x86_io_callback_status model_read(
	kernel_object_handle_t context, uint16_t port, enum dos_io_width width,
	uint32_t *value)
{
	if (context != CALLBACK_CONTEXT || port < 0x0200u || value == NULL ||
	    (width != DOS_IO_WIDTH_8 && width != DOS_IO_WIDTH_16))
		return X86_IO_CALLBACK_FAULT;
	*value = model_read_value;
	return model_read_status;
}

static enum x86_io_callback_status model_write(
	kernel_object_handle_t context, uint16_t port, enum dos_io_width width,
	uint32_t value)
{
	if (context != CALLBACK_CONTEXT || port < 0x0200u ||
	    (width != DOS_IO_WIDTH_8 && width != DOS_IO_WIDTH_16))
		return X86_IO_CALLBACK_FAULT;
	model_write_value = value;
	return X86_IO_CALLBACK_OK;
}

static enum x86_io_callback_status mutation_read(
	kernel_object_handle_t context, uint16_t port, enum dos_io_width width,
	uint32_t *value)
{
	if (context != CALLBACK_CONTEXT || port != 0x0400u ||
	    width != DOS_IO_WIDTH_8 || value == NULL)
		return X86_IO_CALLBACK_FAULT;
	mutation_status =
		x86_io_resource_unregister(mutation_resource, OWNER_IDENTITY);
	*value = 0xa5u;
	return X86_IO_CALLBACK_OK;
}

static struct x86_io_resource_descriptor absent_read_denied_write(
	uint16_t first, uint16_t last)
{
	return (struct x86_io_resource_descriptor){
		.owner_identity = OWNER_IDENTITY,
		.callback_context = 0u,
		.read = NULL,
		.write = NULL,
		.first_port = first,
		.last_port = last,
		.read_width_mask = X86_IO_WIDTH_MASK_8,
		.write_width_mask = X86_IO_WIDTH_MASK_8,
		.read_action = X86_IO_RESOURCE_ACTION_ABSENT,
		.write_action = X86_IO_RESOURCE_ACTION_DENY,
		.flags = 0u,
		.reserved = {0u},
	};
}

static struct x86_io_resource_descriptor emulated_resource(uint16_t first,
						     uint16_t last)
{
	return (struct x86_io_resource_descriptor){
		.owner_identity = OWNER_IDENTITY,
		.callback_context = CALLBACK_CONTEXT,
		.read = model_read,
		.write = model_write,
		.first_port = first,
		.last_port = last,
		.read_width_mask =
			X86_IO_WIDTH_MASK_8 | X86_IO_WIDTH_MASK_16,
		.write_width_mask =
			X86_IO_WIDTH_MASK_8 | X86_IO_WIDTH_MASK_16,
		.read_action = X86_IO_RESOURCE_ACTION_EMULATE,
		.write_action = X86_IO_RESOURCE_ACTION_EMULATE,
		.flags = 0u,
		.reserved = {0u},
	};
}

static struct x86_io_resource_descriptor foreground_resource(uint16_t first,
						       uint16_t last)
{
	struct x86_io_resource_descriptor descriptor =
		emulated_resource(first, last);

	descriptor.read_action = X86_IO_RESOURCE_ACTION_NATIVE;
	descriptor.write_action = X86_IO_RESOURCE_ACTION_NATIVE;
	descriptor.flags = X86_IO_RESOURCE_FLAG_FOREGROUND;
	return descriptor;
}

static int test_unregistered_probe_policy(void)
{
	uint32_t value = 0u;

	if (x86_io_resource_read(KERNEL_OBJECT_HANDLE_INVALID, 0x0888u,
				 DOS_IO_WIDTH_8, &value) != X86_IO_RESOURCE_OK ||
	    value != 0xffu)
		return 1;
	if (x86_io_resource_read(KERNEL_OBJECT_HANDLE_INVALID, 0x0888u,
				 DOS_IO_WIDTH_16, &value) != X86_IO_RESOURCE_OK ||
	    value != 0xffffu)
		return 2;
	if (x86_io_resource_read(KERNEL_OBJECT_HANDLE_INVALID, 0x0888u,
				 DOS_IO_WIDTH_32, &value) != X86_IO_RESOURCE_OK ||
	    value != 0xffffffffu)
		return 3;
	if (x86_io_resource_write(KERNEL_OBJECT_HANDLE_INVALID, 0x0888u,
				  DOS_IO_WIDTH_32, 0x12345678u) !=
	    X86_IO_RESOURCE_OK)
		return 4;
	value = 0x11223344u;
	if (x86_io_resource_read(KERNEL_OBJECT_HANDLE_INVALID, 0xffffu,
				 DOS_IO_WIDTH_16, &value) !=
		X86_IO_RESOURCE_ACCESS_DENIED ||
	    value != 0x11223344u)
		return 5;
	if (x86_io_resource_read(KERNEL_OBJECT_HANDLE_INVALID, 0x0888u,
				 (enum dos_io_width)3, &value) !=
		X86_IO_RESOURCE_INVALID_ARGUMENT ||
	    value != 0x11223344u)
		return 6;
	if (x86_io_resource_write(KERNEL_OBJECT_HANDLE_INVALID, 0x0888u,
				  (enum dos_io_width)3, 0u) !=
	    X86_IO_RESOURCE_INVALID_ARGUMENT)
		return 7;
	return 0;
}

static int test_registered_policy_and_overlap(void)
{
	struct x86_io_resource_descriptor descriptor =
		absent_read_denied_write(0x0100u, 0x010fu);
	struct x86_io_resource_descriptor overlap =
		absent_read_denied_write(0x0108u, 0x0110u);
	x86_io_resource_handle_t resource;
	x86_io_resource_handle_t unchanged = 0x1234u;
	uint32_t value = 0u;

	if (x86_io_resource_register(&descriptor, &resource) !=
	    X86_IO_RESOURCE_OK)
		return 1;
	if (x86_io_resource_read(REQUESTER_IDENTITY, 0x0100u,
				 DOS_IO_WIDTH_8, &value) != X86_IO_RESOURCE_OK ||
	    value != 0xffu)
		return 2;
	if (x86_io_resource_write(REQUESTER_IDENTITY, 0x0100u,
				  DOS_IO_WIDTH_8, 0x55u) !=
	    X86_IO_RESOURCE_ACCESS_DENIED)
		return 3;
	value = 0x99887766u;
	if (x86_io_resource_read(REQUESTER_IDENTITY, 0x0100u,
				 DOS_IO_WIDTH_16, &value) !=
		X86_IO_RESOURCE_ACCESS_DENIED ||
	    value != 0x99887766u)
		return 4;
	if (x86_io_resource_read(REQUESTER_IDENTITY, 0x010fu,
				 DOS_IO_WIDTH_16, &value) !=
		X86_IO_RESOURCE_ACCESS_DENIED)
		return 5;
	if (x86_io_resource_read(REQUESTER_IDENTITY, 0x00ffu,
				 DOS_IO_WIDTH_16, &value) !=
		X86_IO_RESOURCE_ACCESS_DENIED)
		return 6;
	if (x86_io_resource_register(&overlap, &unchanged) !=
		X86_IO_RESOURCE_OVERLAP ||
	    unchanged != 0x1234u)
		return 7;
	return 0;
}

static int test_emulated_callbacks_and_stale_handle(void)
{
	struct x86_io_resource_descriptor descriptor =
		emulated_resource(0x0200u, 0x0203u);
	struct x86_io_resource_view view;
	x86_io_resource_handle_t resource;
	x86_io_resource_handle_t replacement;
	uint32_t value = 0u;

	if (x86_io_resource_register(&descriptor, &resource) !=
	    X86_IO_RESOURCE_OK)
		return 1;
	if (x86_io_resource_query(resource, &view) != X86_IO_RESOURCE_OK ||
	    view.registry_identity != REGISTRY_IDENTITY ||
	    view.owner_identity != OWNER_IDENTITY || view.first_port != 0x0200u ||
	    view.last_port != 0x0203u || view.foreground_owned != 0u)
		return 2;
	model_read_value = 0x1234u;
	if (x86_io_resource_read(REQUESTER_IDENTITY, 0x0200u,
				 DOS_IO_WIDTH_16, &value) != X86_IO_RESOURCE_OK ||
	    value != 0x1234u)
		return 3;
	model_write_value = 0u;
	if (x86_io_resource_write(REQUESTER_IDENTITY, 0x0200u,
				  DOS_IO_WIDTH_16, 0x5678u) !=
		X86_IO_RESOURCE_OK ||
	    model_write_value != 0x5678u)
		return 4;
	model_read_status = X86_IO_CALLBACK_DENIED;
	value = 0xabcdef01u;
	if (x86_io_resource_read(REQUESTER_IDENTITY, 0x0200u,
				 DOS_IO_WIDTH_8, &value) !=
		X86_IO_RESOURCE_ACCESS_DENIED ||
	    value != 0xabcdef01u)
		return 5;
	model_read_status = X86_IO_CALLBACK_OK;
	model_read_value = 0x100u;
	if (x86_io_resource_read(REQUESTER_IDENTITY, 0x0200u,
				 DOS_IO_WIDTH_8, &value) !=
		X86_IO_RESOURCE_CALLBACK_FAULT ||
	    value != 0xabcdef01u)
		return 6;
	model_read_value = 0x5au;
	if (x86_io_resource_unregister(resource, OTHER_OWNER_IDENTITY) !=
	    X86_IO_RESOURCE_OWNERSHIP_DENIED)
		return 7;
	if (x86_io_resource_unregister(resource, OWNER_IDENTITY) !=
	    X86_IO_RESOURCE_OK)
		return 8;
	if (x86_io_resource_query(resource, &view) !=
	    X86_IO_RESOURCE_STALE_HANDLE)
		return 9;
	if (x86_io_resource_register(&descriptor, &replacement) !=
		X86_IO_RESOURCE_OK ||
	    replacement == resource)
		return 10;
	if (x86_io_resource_unregister(resource, OWNER_IDENTITY) !=
	    X86_IO_RESOURCE_STALE_HANDLE)
		return 11;
	if (x86_io_resource_unregister(replacement, OWNER_IDENTITY) !=
	    X86_IO_RESOURCE_OK)
		return 12;
	return 0;
}

static int test_foreground_ownership_and_revocation(void)
{
	struct x86_io_resource_descriptor descriptor =
		foreground_resource(0x0300u, 0x0303u);
	struct x86_io_resource_descriptor unsafe = descriptor;
	struct x86_io_resource_view view;
	x86_io_foreground_token_t token = 0x2222u;
	x86_io_foreground_token_t replacement;
	x86_io_resource_handle_t resource;
	x86_io_resource_handle_t unchanged = 0x1111u;
	uint32_t value = 0x87654321u;

	unsafe.flags = 0u;
	if (x86_io_resource_register(&unsafe, &unchanged) !=
		X86_IO_RESOURCE_INVALID_ARGUMENT ||
	    unchanged != 0x1111u)
		return 1;
	if (x86_io_resource_register(&descriptor, &resource) !=
	    X86_IO_RESOURCE_OK)
		return 2;
	if (x86_io_resource_read(REQUESTER_IDENTITY, 0x0300u,
				 DOS_IO_WIDTH_8, &value) !=
		X86_IO_RESOURCE_ACCESS_DENIED ||
	    value != 0x87654321u)
		return 3;
	if (x86_io_resource_foreground_acquire(
		resource, OTHER_OWNER_IDENTITY, REQUESTER_IDENTITY, &token) !=
		X86_IO_RESOURCE_OWNERSHIP_DENIED ||
	    token != 0x2222u)
		return 4;
	if (x86_io_resource_query(resource, &view) != X86_IO_RESOURCE_OK ||
	    view.foreground_owned != 0u ||
	    view.foreground_requester != KERNEL_OBJECT_HANDLE_INVALID)
		return 15;
	if (x86_io_resource_foreground_acquire(
		resource, OWNER_IDENTITY, REQUESTER_IDENTITY, &token) !=
		X86_IO_RESOURCE_OK ||
	    token == resource)
		return 5;
	if (x86_io_resource_query(resource, &view) != X86_IO_RESOURCE_OK ||
	    view.foreground_owned != 1u ||
	    view.foreground_requester != REQUESTER_IDENTITY)
		return 16;
	if (x86_io_resource_read(OTHER_REQUESTER_IDENTITY, 0x0300u,
				 DOS_IO_WIDTH_8, &value) !=
	    X86_IO_RESOURCE_ACCESS_DENIED)
		return 6;
	model_read_value = 0x6bu;
	if (x86_io_resource_read(REQUESTER_IDENTITY, 0x0300u,
				 DOS_IO_WIDTH_8, &value) != X86_IO_RESOURCE_OK ||
	    value != 0x6bu)
		return 7;
	if (x86_io_resource_foreground_release(token,
					       OTHER_REQUESTER_IDENTITY) !=
	    X86_IO_RESOURCE_OWNERSHIP_DENIED)
		return 8;
	if (x86_io_resource_query(resource, &view) != X86_IO_RESOURCE_OK ||
	    view.foreground_owned != 1u ||
	    view.foreground_requester != REQUESTER_IDENTITY)
		return 17;
	if (x86_io_resource_foreground_release(token, REQUESTER_IDENTITY) !=
	    X86_IO_RESOURCE_OK)
		return 9;
	if (x86_io_resource_query(resource, &view) != X86_IO_RESOURCE_OK ||
	    view.foreground_owned != 0u ||
	    view.foreground_requester != KERNEL_OBJECT_HANDLE_INVALID)
		return 18;
	if (x86_io_resource_foreground_release(token, REQUESTER_IDENTITY) !=
	    X86_IO_RESOURCE_STALE_HANDLE)
		return 10;
	if (x86_io_resource_foreground_acquire(
		resource, OWNER_IDENTITY, REQUESTER_IDENTITY, &replacement) !=
		X86_IO_RESOURCE_OK ||
	    replacement == token)
		return 11;
	if (x86_io_resource_foreground_revoke(resource, OWNER_IDENTITY) !=
	    X86_IO_RESOURCE_OK)
		return 12;
	if (x86_io_resource_query(resource, &view) != X86_IO_RESOURCE_OK ||
	    view.foreground_owned != 0u ||
	    view.foreground_requester != KERNEL_OBJECT_HANDLE_INVALID)
		return 19;
	if (x86_io_resource_foreground_release(replacement,
					       REQUESTER_IDENTITY) !=
	    X86_IO_RESOURCE_STALE_HANDLE)
		return 13;
	if (x86_io_resource_write(REQUESTER_IDENTITY, 0x0300u,
				  DOS_IO_WIDTH_8, 0x77u) !=
	    X86_IO_RESOURCE_ACCESS_DENIED)
		return 14;
	return 0;
}

static int test_callback_lifecycle_and_atomic_batch(void)
{
	struct x86_io_resource_descriptor mutation = {
		.owner_identity = OWNER_IDENTITY,
		.callback_context = CALLBACK_CONTEXT,
		.read = mutation_read,
		.write = NULL,
		.first_port = 0x0400u,
		.last_port = 0x0400u,
		.read_width_mask = X86_IO_WIDTH_MASK_8,
		.write_width_mask = 0u,
		.read_action = X86_IO_RESOURCE_ACTION_EMULATE,
		.write_action = X86_IO_RESOURCE_ACTION_DENY,
		.flags = 0u,
		.reserved = {0u},
	};
	struct x86_io_resource_descriptor batch[2] = {
		absent_read_denied_write(0x0500u, 0x0502u),
		absent_read_denied_write(0x0502u, 0x0504u),
	};
	x86_io_resource_handle_t outputs[2] = {0x1111u, 0x2222u};
	uint32_t value = 0u;

	if (x86_io_resource_register(&mutation, &mutation_resource) !=
	    X86_IO_RESOURCE_OK)
		return 1;
	mutation_status = X86_IO_RESOURCE_OK;
	if (x86_io_resource_read(REQUESTER_IDENTITY, 0x0400u,
				 DOS_IO_WIDTH_8, &value) != X86_IO_RESOURCE_OK ||
	    value != 0xa5u || mutation_status != X86_IO_RESOURCE_INVALID_STATE)
		return 2;
	if (x86_io_resource_unregister(mutation_resource, OWNER_IDENTITY) !=
	    X86_IO_RESOURCE_OK)
		return 3;
	if (x86_io_resource_register_batch(batch, 2u, outputs,
					   ARRAY_SIZE(outputs)) !=
		X86_IO_RESOURCE_OVERLAP ||
	    outputs[0] != 0x1111u || outputs[1] != 0x2222u)
		return 4;
	if (x86_io_resource_read(REQUESTER_IDENTITY, 0x0500u,
				 DOS_IO_WIDTH_8, &value) != X86_IO_RESOURCE_OK ||
	    value != 0xffu)
		return 5;
	return 0;
}

int main(void)
{
	int result;

	if (x86_io_resource_registry_initialize(0u) !=
	    X86_IO_RESOURCE_INVALID_ARGUMENT)
		return 1;
	if (x86_io_resource_registry_initialize(REGISTRY_IDENTITY) !=
		X86_IO_RESOURCE_OK ||
	    x86_io_resource_registry_identity() != REGISTRY_IDENTITY)
		return 2;
	if (x86_io_resource_registry_initialize(OTHER_OWNER_IDENTITY) !=
	    X86_IO_RESOURCE_INVALID_STATE)
		return 3;
	result = test_unregistered_probe_policy();
	if (result != 0)
		return 10 + result;
	result = test_registered_policy_and_overlap();
	if (result != 0)
		return 20 + result;
	result = test_emulated_callbacks_and_stale_handle();
	if (result != 0)
		return 40 + result;
	result = test_foreground_ownership_and_revocation();
	if (result != 0)
		return 60 + result;
	result = test_callback_lifecycle_and_atomic_batch();
	if (result != 0)
		return 80 + result;
	return 0;
}

/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Generation-bound x86 I/O-port resource registry.
 *
 * This is a kernel protection boundary, not a DOS-visible ABI.  A registered
 * range has one owner, direction-specific width/action policy, and optional
 * typed model or native-access callbacks.  Unregistered ports model an
 * undriven bus for probes without ever reaching host hardware.
 */
#ifndef DOSC32_X86_IO_RESOURCE_H
#define DOSC32_X86_IO_RESOURCE_H

#include "address.h"
#include "dos_machine.h"

/* A fixed registry capacity is policy, not discovered hardware state. */
#define X86_IO_RESOURCE_REGISTRY_CAPACITY 32u

#define X86_IO_WIDTH_MASK_8 0x01u
#define X86_IO_WIDTH_MASK_16 0x02u
#define X86_IO_WIDTH_MASK_32 0x04u
#define X86_IO_WIDTH_MASK_ALL                                                \
	(X86_IO_WIDTH_MASK_8 | X86_IO_WIDTH_MASK_16 | X86_IO_WIDTH_MASK_32)

#define X86_IO_RESOURCE_FLAG_FOREGROUND 0x01u
#define X86_IO_RESOURCE_FLAG_MASK X86_IO_RESOURCE_FLAG_FOREGROUND

typedef uint64_t x86_io_resource_handle_t;
typedef uint64_t x86_io_foreground_token_t;

#define X86_IO_RESOURCE_HANDLE_INVALID ((x86_io_resource_handle_t)0u)
#define X86_IO_FOREGROUND_TOKEN_INVALID ((x86_io_foreground_token_t)0u)

enum x86_io_resource_action {
	/* Reads return all ones; writes complete without a side effect. */
	X86_IO_RESOURCE_ACTION_ABSENT = 0,
	X86_IO_RESOURCE_ACTION_DENY,
	/* A software model services the operation through a typed callback. */
	X86_IO_RESOURCE_ACTION_EMULATE,
	/* A callback may issue native I/O only while foreground-owned. */
	X86_IO_RESOURCE_ACTION_NATIVE
};

enum x86_io_callback_status {
	X86_IO_CALLBACK_OK = 0,
	X86_IO_CALLBACK_DENIED,
	X86_IO_CALLBACK_FAULT
};

typedef enum x86_io_callback_status (*x86_io_read_callback_t)(
	kernel_object_handle_t context, uint16_t port,
	enum dos_io_width width, uint32_t *value);
typedef enum x86_io_callback_status (*x86_io_write_callback_t)(
	kernel_object_handle_t context, uint16_t port,
	enum dos_io_width width, uint32_t value);

struct x86_io_resource_descriptor {
	kernel_object_handle_t owner_identity;
	kernel_object_handle_t callback_context;
	x86_io_read_callback_t read;
	x86_io_write_callback_t write;
	uint16_t first_port;
	uint16_t last_port;
	uint8_t read_width_mask;
	uint8_t write_width_mask;
	uint8_t read_action;
	uint8_t write_action;
	uint8_t flags;
	uint8_t reserved[3];
} __aligned(8);

struct x86_io_resource_view {
	kernel_object_handle_t registry_identity;
	kernel_object_handle_t owner_identity;
	x86_io_resource_handle_t resource;
	kernel_object_handle_t foreground_requester;
	uint16_t first_port;
	uint16_t last_port;
	uint8_t read_width_mask;
	uint8_t write_width_mask;
	uint8_t read_action;
	uint8_t write_action;
	uint8_t flags;
	uint8_t foreground_owned;
	uint8_t reserved[2];
} __aligned(8);

enum x86_io_resource_status {
	X86_IO_RESOURCE_OK = 0,
	X86_IO_RESOURCE_INVALID_ARGUMENT,
	X86_IO_RESOURCE_INVALID_STATE,
	X86_IO_RESOURCE_CAPACITY_EXHAUSTED,
	X86_IO_RESOURCE_OVERLAP,
	X86_IO_RESOURCE_STALE_HANDLE,
	X86_IO_RESOURCE_OWNERSHIP_DENIED,
	X86_IO_RESOURCE_ACCESS_DENIED,
	X86_IO_RESOURCE_CALLBACK_FAULT
};

enum x86_io_resource_status x86_io_resource_registry_initialize(
	kernel_object_handle_t registry_identity) __must_check;
kernel_object_handle_t x86_io_resource_registry_identity(void) __must_check;

enum x86_io_resource_status x86_io_resource_register(
	const struct x86_io_resource_descriptor *descriptor,
	x86_io_resource_handle_t *resource) __must_check;
enum x86_io_resource_status x86_io_resource_register_batch(
	const struct x86_io_resource_descriptor *descriptors,
	size_t descriptor_count, x86_io_resource_handle_t *resources,
	size_t resource_capacity) __must_check;
enum x86_io_resource_status x86_io_resource_unregister(
	x86_io_resource_handle_t resource,
	kernel_object_handle_t owner_identity) __must_check;
enum x86_io_resource_status x86_io_resource_query(
	x86_io_resource_handle_t resource,
	struct x86_io_resource_view *view) __must_check;

enum x86_io_resource_status x86_io_resource_foreground_acquire(
	x86_io_resource_handle_t resource,
	kernel_object_handle_t owner_identity,
	kernel_object_handle_t requester_identity,
	x86_io_foreground_token_t *token) __must_check;
enum x86_io_resource_status x86_io_resource_foreground_release(
	x86_io_foreground_token_t token,
	kernel_object_handle_t requester_identity) __must_check;
enum x86_io_resource_status x86_io_resource_foreground_revoke(
	x86_io_resource_handle_t resource,
	kernel_object_handle_t owner_identity) __must_check;

enum x86_io_resource_status x86_io_resource_read(
	kernel_object_handle_t requester_identity, uint16_t port,
	enum dos_io_width width, uint32_t *value) __must_check;
enum x86_io_resource_status x86_io_resource_write(
	kernel_object_handle_t requester_identity, uint16_t port,
	enum dos_io_width width, uint32_t value) __must_check;

static_assert_expression(sizeof(x86_io_resource_handle_t) == 8u,
			 "I/O resource handles must remain 64-bit");
static_assert_expression(sizeof(x86_io_foreground_token_t) == 8u,
			 "foreground tokens must remain 64-bit");
static_assert_expression(sizeof(struct x86_io_resource_view) == 48u,
			 "I/O resource views must remain fixed width");

#endif

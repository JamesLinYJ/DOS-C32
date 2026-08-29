/* SPDX-License-Identifier: GPL-2.0-only */
/* Stable named character-device boundary for the DOS-C32 I/O Manager. */
#ifndef DOSC32_IOMGR_DEVICE_H
#define DOSC32_IOMGR_DEVICE_H

#include "iomgr.h"

#define IOMGR_DEVICE_ABI_VERSION 1u
#define IOMGR_DEVICE_NAME_MAX_BYTES 64u
#define IOMGR_DEVICE_REGISTRATION_HANDLE_INVALID \
	KERNEL_OBJECT_HANDLE_INVALID
#define IOMGR_DEVICE_HANDLE_INVALID KERNEL_OBJECT_HANDLE_INVALID
#define IOMGR_DEVICE_PENDING_BYTES_UNKNOWN ((uint64_t)-1)

#define IOMGR_DEVICE_CAP_READ (1u << 0)
#define IOMGR_DEVICE_CAP_WRITE (1u << 1)
#define IOMGR_DEVICE_CAP_CONTROL (1u << 2)

#define IOMGR_DEVICE_STATE_READ_READY (1u << 0)
#define IOMGR_DEVICE_STATE_WRITE_READY (1u << 1)
#define IOMGR_DEVICE_STATE_END_OF_INPUT (1u << 2)

typedef kernel_object_handle_t iomgr_device_registration_handle_t;
typedef kernel_object_handle_t iomgr_device_handle_t;

/* The registry copies these bytes and compares them exactly. */
struct iomgr_device_name {
	const uint8_t *bytes;
	size_t length;
};

/*
 * Operations are synchronous.  Until the kernel has a general scheduler lock
 * model, the registry owner serializes calls from distinct execution contexts.
 * Same-instance callback reentry is rejected with IOMGR_BUSY; callbacks must
 * not mutate their own registration through another device-manager API.
 */

/*
 * Every result except UNCERTAIN is exact.  A precise failure guarantees that
 * the callback did not change the instance or publish a partial result.  OK
 * may report an exact partial transfer.  UNCERTAIN causes the I/O Manager to
 * quarantine only the affected published instance.  If open cannot establish
 * a trusted instance identity, the registration enters drain-only quarantine:
 * existing instances may close, but no other operation or new open proceeds.
 */
enum iomgr_device_callback_status {
	IOMGR_DEVICE_CALLBACK_OK = 0,
	IOMGR_DEVICE_CALLBACK_INVALID_ARGUMENT,
	IOMGR_DEVICE_CALLBACK_BUSY,
	IOMGR_DEVICE_CALLBACK_UNSUPPORTED,
	IOMGR_DEVICE_CALLBACK_IO_ERROR,
	IOMGR_DEVICE_CALLBACK_NO_SPACE,
	IOMGR_DEVICE_CALLBACK_NO_RESOURCES,
	IOMGR_DEVICE_CALLBACK_UNCERTAIN
};

struct iomgr_device_query_result {
	uint64_t pending_read_bytes;
	uint32_t state;
	uint32_t reserved;
} __aligned(8);

struct iomgr_device_info {
	uint64_t identity;
	uint64_t pending_read_bytes;
	uint32_t capabilities;
	uint32_t state;
	uint16_t name_length;
	uint16_t reserved16;
	uint32_t reserved32;
	uint8_t name[IOMGR_DEVICE_NAME_MAX_BYTES];
} __aligned(8);

struct iomgr_device_ops {
	uint32_t abi_version;
	uint32_t reserved;
	uint64_t identity;
	kernel_object_handle_t context;
	uint32_t capabilities;
	uint32_t reserved2;
	enum iomgr_device_callback_status (*open)(
		kernel_object_handle_t context,
		kernel_object_handle_t *instance_context);
	enum iomgr_device_callback_status (*close)(
		kernel_object_handle_t context,
		kernel_object_handle_t instance_context);
	enum iomgr_device_callback_status (*read)(
		kernel_object_handle_t context,
		kernel_object_handle_t instance_context, uint8_t *destination,
		size_t capacity, size_t count, size_t *bytes_read);
	enum iomgr_device_callback_status (*write)(
		kernel_object_handle_t context,
		kernel_object_handle_t instance_context, const uint8_t *source,
		size_t source_capacity, size_t count, size_t *bytes_written);
	enum iomgr_device_callback_status (*control)(
		kernel_object_handle_t context,
		kernel_object_handle_t instance_context, uint64_t operation,
		const uint8_t *input, size_t input_capacity, size_t input_count,
		uint8_t *output, size_t output_capacity,
		size_t *bytes_returned);
	enum iomgr_device_callback_status (*query_info)(
		kernel_object_handle_t context,
		kernel_object_handle_t instance_context,
		struct iomgr_device_query_result *result);
} __aligned(8);

enum iomgr_status iomgr_device_initialize(void) __must_check;
enum iomgr_status iomgr_device_register(
	const struct iomgr_device_name *name,
	const struct iomgr_device_ops *ops,
	iomgr_device_registration_handle_t *registration) __must_check;
enum iomgr_status iomgr_device_unregister(
	iomgr_device_registration_handle_t registration) __must_check;
enum iomgr_status iomgr_device_open(
	const struct iomgr_device_name *name,
	iomgr_device_handle_t *device) __must_check;
enum iomgr_status iomgr_device_close(
	iomgr_device_handle_t device) __must_check;
enum iomgr_status iomgr_device_read(
	iomgr_device_handle_t device, uint8_t *destination, size_t capacity,
	size_t count, size_t *bytes_read) __must_check;
enum iomgr_status iomgr_device_write(
	iomgr_device_handle_t device, const uint8_t *source,
	size_t source_capacity, size_t count,
	size_t *bytes_written) __must_check;
enum iomgr_status iomgr_device_control(
	iomgr_device_handle_t device, uint64_t operation, const uint8_t *input,
	size_t input_capacity, size_t input_count, uint8_t *output,
	size_t output_capacity, size_t *bytes_returned) __must_check;
enum iomgr_status iomgr_device_query_info(
	iomgr_device_handle_t device,
	struct iomgr_device_info *info) __must_check;

static_assert_expression(sizeof(iomgr_device_registration_handle_t) == 8u,
	"I/O Manager device registration handles must remain 64-bit");
static_assert_expression(sizeof(iomgr_device_handle_t) == 8u,
	"I/O Manager device handles must remain 64-bit");
static_assert_expression(sizeof(enum iomgr_device_callback_status) == 4u,
	"I/O Manager device callback status must remain 32-bit");
static_assert_expression(sizeof(struct iomgr_device_query_result) == 16u,
	"I/O Manager device query result layout changed");
static_assert_expression(sizeof(struct iomgr_device_info) == 96u,
	"I/O Manager device info layout changed");
static_assert_expression(__alignof__(struct iomgr_device_query_result) == 8u,
	"I/O Manager device query result alignment changed");
static_assert_expression(__alignof__(struct iomgr_device_info) == 8u,
	"I/O Manager device info alignment changed");
static_assert_expression(__alignof__(struct iomgr_device_ops) == 8u,
	"I/O Manager device operations alignment changed");
static_assert_expression(
	__builtin_offsetof(struct iomgr_device_ops, abi_version) == 0u,
	"I/O Manager device ABI version offset changed");
static_assert_expression(
	__builtin_offsetof(struct iomgr_device_ops, reserved) == 4u,
	"I/O Manager device ABI reserved offset changed");
static_assert_expression(
	__builtin_offsetof(struct iomgr_device_ops, identity) == 8u,
	"I/O Manager device identity binding offset changed");
static_assert_expression(
	__builtin_offsetof(struct iomgr_device_ops, context) == 16u,
	"I/O Manager device context offset changed");
static_assert_expression(
	__builtin_offsetof(struct iomgr_device_ops, capabilities) == 24u,
	"I/O Manager device capabilities offset changed");
static_assert_expression(
	__builtin_offsetof(struct iomgr_device_ops, open) == 32u,
	"I/O Manager device open callback offset changed");
static_assert_expression(
	__builtin_offsetof(struct iomgr_device_query_result,
			   pending_read_bytes) == 0u,
	"I/O Manager device pending query offset changed");
static_assert_expression(
	__builtin_offsetof(struct iomgr_device_query_result, state) == 8u,
	"I/O Manager device query state offset changed");
static_assert_expression(
	__builtin_offsetof(struct iomgr_device_query_result, reserved) == 12u,
	"I/O Manager device query reserved offset changed");
static_assert_expression(
	__builtin_offsetof(struct iomgr_device_info, identity) == 0u,
	"I/O Manager device identity offset changed");
static_assert_expression(
	__builtin_offsetof(struct iomgr_device_info, pending_read_bytes) == 8u,
	"I/O Manager device pending byte offset changed");
static_assert_expression(
	__builtin_offsetof(struct iomgr_device_info, capabilities) == 16u,
	"I/O Manager device capability offset changed");
static_assert_expression(
	__builtin_offsetof(struct iomgr_device_info, state) == 20u,
	"I/O Manager device state offset changed");
static_assert_expression(
	__builtin_offsetof(struct iomgr_device_info, name_length) == 24u,
	"I/O Manager device name length offset changed");
static_assert_expression(
	__builtin_offsetof(struct iomgr_device_info, reserved16) == 26u,
	"I/O Manager device reserved16 offset changed");
static_assert_expression(
	__builtin_offsetof(struct iomgr_device_info, reserved32) == 28u,
	"I/O Manager device reserved32 offset changed");
static_assert_expression(
	__builtin_offsetof(struct iomgr_device_info, name) == 32u,
	"I/O Manager device name offset changed");

#endif

/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Canonical, generation-checked SFT registry.
 *
 * Compatibility contract: one shared SFT record backs every JFT reference
 * Safety changes: fixed slots, reserve-before-publish, sticky quarantine
 */
#ifndef DOSC32_DOS_SFT_ADAPTER_H
#define DOSC32_DOS_SFT_ADAPTER_H

#include "compiler.h"
#include "dos_error.h"
#include "dos_sft_batch.h"
#include "types.h"

#define DOS_SFT_REGISTRY_SLOT_COUNT 256u

enum dos_sft_registry_status {
	DOS_SFT_REGISTRY_READY = 0,
	DOS_SFT_REGISTRY_INVALID_ARGUMENT,
	DOS_SFT_REGISTRY_INVALID_STATE,
	DOS_SFT_REGISTRY_GENERATION_EXHAUSTED,
	DOS_SFT_REGISTRY_NO_SLOT,
	DOS_SFT_REGISTRY_STALE_REFERENCE,
	DOS_SFT_REGISTRY_CONTEXT_MISMATCH,
	DOS_SFT_REGISTRY_BACKEND_FAILURE,
	DOS_SFT_REGISTRY_POISONED
};

enum dos_sft_slot_state {
	DOS_SFT_SLOT_FREE = 0,
	DOS_SFT_SLOT_RESERVED,
	DOS_SFT_SLOT_PRESENT,
	DOS_SFT_SLOT_POISONED
};

enum dos_sft_backend_kind {
	DOS_SFT_BACKEND_STANDARD = 1,
	DOS_SFT_BACKEND_FILE,
	DOS_SFT_BACKEND_DEVICE
};

enum dos_sft_backend_close_status {
	DOS_SFT_BACKEND_CLOSE_OK = 0,
	DOS_SFT_BACKEND_CLOSE_EXACT_FAILURE,
	DOS_SFT_BACKEND_CLOSE_UNCERTAIN
};

/*
 * EXACT_FAILURE promises that backend ownership and state are unchanged and
 * writes the exact nonzero DOS error.  OK writes DOS_SUCCESS and proves that
 * ownership was released.  UNCERTAIN makes the represented SFT unusable.
 */
struct dos_sft_backend_close_ops {
	kernel_object_handle_t identity;
	kernel_object_handle_t context;
	enum dos_sft_backend_close_status (*close)(
		kernel_object_handle_t context,
		enum dos_sft_backend_kind backend_kind,
		kernel_object_handle_t backend_handle,
		enum dos_error *exact_error);
};

/* A newly published record owns exactly one SFT reference. */
struct dos_sft_registry_publish_record {
	kernel_object_handle_t backend_handle;
	uint64_t position;
	uint64_t size;
	uint16_t flags;
	uint16_t mode;
	uint16_t information;
	uint8_t backend_kind;
	uint8_t reserved;
} __aligned(8);

struct dos_sft_registry_view {
	dos_sft_reference_handle_t reference_handle;
	kernel_object_handle_t backend_handle;
	uint64_t position;
	uint64_t size;
	uint32_t references;
	uint32_t device_opens;
	uint16_t flags;
	uint16_t mode;
	uint16_t information;
	uint8_t backend_kind;
	uint8_t state;
} __aligned(8);

enum dos_sft_registry_status dos_sft_registry_initialize(
	kernel_object_handle_t adapter_identity,
	kernel_object_handle_t adapter_context) __must_check;

/* Bind one immutable backend-close authority for this registry lifetime. */
enum dos_sft_registry_status dos_sft_registry_bind_backend_close(
	kernel_object_handle_t context,
	const struct dos_sft_backend_close_ops *ops) __must_check;

/*
 * Bootstrap helper for standard and test SFTs at an exact SFN.  It is valid
 * only for a FREE slot.  Local logical device-open counts start equal to the
 * supplied references; network SFTs start with zero logical device opens.
 */
enum dos_sft_registry_status dos_sft_registry_install(
	uint8_t sfn, uint16_t flags, uint16_t mode,
	uint32_t initial_references) __must_check;

/* Reserve the lowest reusable SFN; outputs are unchanged on failure. */
enum dos_sft_registry_status dos_sft_registry_reserve(
	kernel_object_handle_t context, uint8_t *sfn,
	dos_sft_reference_handle_t *reference_handle) __must_check;
enum dos_sft_registry_status dos_sft_registry_publish(
	kernel_object_handle_t context,
	dos_sft_reference_handle_t reference_handle,
	const struct dos_sft_registry_publish_record *record) __must_check;
enum dos_sft_registry_status dos_sft_registry_cancel(
	kernel_object_handle_t context,
	dos_sft_reference_handle_t reference_handle) __must_check;

/* Inspect one live, open generation; output is unchanged on failure. */
enum dos_sft_registry_status dos_sft_registry_resolve(
	kernel_object_handle_t context, uint8_t sfn,
	struct dos_sft_registry_view *view) __must_check;
enum dos_sft_registry_status dos_sft_registry_inspect_open(
	kernel_object_handle_t context,
	dos_sft_reference_handle_t reference_handle,
	struct dos_sft_registry_view *view) __must_check;

/* Atomically publish the shared SFT cursor, size, and information word. */
enum dos_sft_registry_status dos_sft_registry_update_io(
	kernel_object_handle_t context,
	dos_sft_reference_handle_t reference_handle, uint64_t position,
	uint64_t size, uint16_t information) __must_check;

/*
 * A non-final close decrements the shared count exactly.  A final close calls
 * the bound backend before changing the record.  Exact backend failure leaves
 * the record unchanged; uncertainty quarantines it.  exact_error is set to
 * DOS_SUCCESS or the exact close error only when that result is known.
 */
enum dos_sft_registry_status dos_sft_registry_close_reference(
	kernel_object_handle_t context,
	dos_sft_reference_handle_t reference_handle,
	enum dos_error *exact_error) __must_check;
enum dos_sft_registry_status dos_sft_registry_poison(
	kernel_object_handle_t context,
	dos_sft_reference_handle_t reference_handle) __must_check;

/* Slot-index inspection is for bootstrap diagnostics and tests only. */
enum dos_sft_registry_status dos_sft_registry_inspect(
	uint8_t sfn, struct dos_sft_registry_view *view) __must_check;
const struct dos_sft_batch_ops *dos_sft_registry_ops(void) __must_check;
kernel_object_handle_t dos_sft_registry_context(void) __must_check;

static_assert_expression(sizeof(struct dos_sft_registry_publish_record) == 32u,
	"SFT publish records must stay data-model independent");
static_assert_expression(
	__alignof__(struct dos_sft_registry_publish_record) == 8u,
	"SFT publish-record alignment changed");
static_assert_expression(
	__builtin_offsetof(struct dos_sft_registry_publish_record,
			   backend_handle) == 0u,
	"SFT publish backend-handle offset changed");
static_assert_expression(
	__builtin_offsetof(struct dos_sft_registry_publish_record, position) ==
		8u,
	"SFT publish position offset changed");
static_assert_expression(
	__builtin_offsetof(struct dos_sft_registry_publish_record, flags) == 24u,
	"SFT publish flags offset changed");
static_assert_expression(
	__builtin_offsetof(struct dos_sft_registry_publish_record,
			   backend_kind) == 30u,
	"SFT publish backend-kind offset changed");
static_assert_expression(sizeof(struct dos_sft_registry_view) == 48u,
	"SFT inspection records must stay data-model independent");
static_assert_expression(__alignof__(struct dos_sft_registry_view) == 8u,
	"SFT inspection-record alignment changed");
static_assert_expression(
	__builtin_offsetof(struct dos_sft_registry_view, backend_handle) == 8u,
	"SFT inspection backend-handle offset changed");
static_assert_expression(
	__builtin_offsetof(struct dos_sft_registry_view, position) == 16u,
	"SFT inspection position offset changed");
static_assert_expression(
	__builtin_offsetof(struct dos_sft_registry_view, references) == 32u,
	"SFT inspection reference-count offset changed");
static_assert_expression(
	__builtin_offsetof(struct dos_sft_registry_view, information) == 44u,
	"SFT inspection information offset changed");
static_assert_expression(sizeof(enum dos_sft_backend_kind) == 4u,
	"SFT backend-kind callback ABI changed");
static_assert_expression(sizeof(enum dos_sft_backend_close_status) == 4u,
	"SFT backend-close callback ABI changed");

#endif

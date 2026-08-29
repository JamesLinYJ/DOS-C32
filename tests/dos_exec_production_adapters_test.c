// SPDX-License-Identifier: GPL-2.0-only
#include "dos_drive_visibility.h"
#include "test_entry.h"
#include "dos_exec_gate.h"
#include "dos_sft_adapter.h"

#define TEST_GATE_ID 0x101u
#define TEST_GATE_CONTEXT 0x102u
#define TEST_DRIVE_ID 0x201u
#define TEST_DRIVE_CONTEXT 0x202u
#define TEST_SFT_ID 0x301u
#define TEST_SFT_CONTEXT 0x302u
#define TEST_CLOSE_ID 0x401u
#define TEST_CLOSE_CONTEXT 0x402u
#define TEST_FILE_HANDLE 0x5000000000000001ull
#define TEST_DEVICE_HANDLE 0x5000000000000002ull

static enum dos_sft_backend_close_status configured_close_status;
static enum dos_error configured_close_error;
static kernel_object_handle_t closed_backend_handle;
static enum dos_sft_backend_kind closed_backend_kind;
static uint32_t backend_close_calls;

static enum dos_sft_backend_close_status test_backend_close(
	kernel_object_handle_t context,
	enum dos_sft_backend_kind backend_kind,
	kernel_object_handle_t backend_handle, enum dos_error *exact_error)
{
	if (context != TEST_CLOSE_CONTEXT || exact_error == NULL)
		return DOS_SFT_BACKEND_CLOSE_UNCERTAIN;
	++backend_close_calls;
	closed_backend_kind = backend_kind;
	closed_backend_handle = backend_handle;
	if (configured_close_status == DOS_SFT_BACKEND_CLOSE_OK)
		*exact_error = DOS_SUCCESS;
	else if (configured_close_status ==
		 DOS_SFT_BACKEND_CLOSE_EXACT_FAILURE)
		*exact_error = configured_close_error;
	return configured_close_status;
}

static const struct dos_sft_backend_close_ops close_ops = {
	.identity = TEST_CLOSE_ID,
	.context = TEST_CLOSE_CONTEXT,
	.close = test_backend_close,
};

static void configure_close(enum dos_sft_backend_close_status status,
			    enum dos_error error)
{
	configured_close_status = status;
	configured_close_error = error;
	closed_backend_handle = KERNEL_OBJECT_HANDLE_INVALID;
	closed_backend_kind = (enum dos_sft_backend_kind)0u;
	backend_close_calls = 0u;
}

static int test_drive_visibility(void)
{
	const struct dos_exec_drive_visibility_ops *ops;

	if (dos_drive_visibility_initialize(TEST_DRIVE_ID, TEST_DRIVE_CONTEXT,
					    2u, 1u << 2) !=
	    DOS_DRIVE_VISIBILITY_READY)
		return 1;
	ops = dos_drive_visibility_ops();
	if (ops == NULL || ops->identity != TEST_DRIVE_ID ||
	    dos_drive_visibility_context() != TEST_DRIVE_CONTEXT)
		return 2;
	if (ops->resolve(TEST_DRIVE_CONTEXT, 0u) != DOS_EXEC_DRIVE_VISIBLE ||
	    ops->resolve(TEST_DRIVE_CONTEXT, 3u) != DOS_EXEC_DRIVE_VISIBLE ||
	    ops->resolve(TEST_DRIVE_CONTEXT, 1u) != DOS_EXEC_DRIVE_INVALID ||
	    ops->resolve(TEST_DRIVE_CONTEXT, 27u) != DOS_EXEC_DRIVE_INVALID ||
	    ops->resolve(TEST_DRIVE_CONTEXT + 1u, 3u) != DOS_EXEC_DRIVE_FAULT)
		return 3;
	return 0;
}

static void unused_jft(struct dos_sft_jft20 *jft)
{
	size_t index;

	for (index = 0u; index < ARRAY_SIZE(jft->entries); ++index)
		jft->entries[index] = DOS_JFT_ENTRY_UNUSED;
}

static bool inspect_equals(uint8_t sfn, uint32_t references,
			   uint32_t device_opens)
{
	struct dos_sft_registry_view view = {0};

	return dos_sft_registry_inspect(sfn, &view) == DOS_SFT_REGISTRY_READY &&
	       view.state == (uint8_t)DOS_SFT_SLOT_PRESENT &&
	       view.references == references &&
	       view.device_opens == device_opens;
}

static bool inspect_state(uint8_t sfn, enum dos_sft_slot_state state)
{
	struct dos_sft_registry_view view = {0};

	return dos_sft_registry_inspect(sfn, &view) == DOS_SFT_REGISTRY_READY &&
	       view.state == (uint8_t)state;
}

static enum dos_sft_registry_status reserve_and_publish(
	enum dos_sft_backend_kind backend_kind,
	kernel_object_handle_t backend_handle, uint16_t flags, uint8_t *sfn,
	dos_sft_reference_handle_t *reference)
{
	struct dos_sft_registry_publish_record record = {
		.backend_handle = backend_handle,
		.position = 10u,
		.size = 20u,
		.flags = flags,
		.mode = 2u,
		.information = 0x42u,
		.backend_kind = (uint8_t)backend_kind,
		.reserved = 0u,
	};
	enum dos_sft_registry_status status;

	status = dos_sft_registry_reserve(TEST_SFT_CONTEXT, sfn, reference);
	if (status != DOS_SFT_REGISTRY_READY)
		return status;
	return dos_sft_registry_publish(TEST_SFT_CONTEXT, *reference, &record);
}

static int test_sft_registry(void)
{
	const struct dos_sft_batch_ops *ops;
	struct dos_sft_jft20 parent;
	struct dos_sft_jft20 child;
	dos_sft_batch_handle_t batch = DOS_SFT_BATCH_HANDLE_INVALID;

	if (dos_sft_registry_initialize(TEST_SFT_ID, TEST_SFT_CONTEXT) !=
		DOS_SFT_REGISTRY_READY ||
	    dos_sft_registry_bind_backend_close(TEST_SFT_CONTEXT + 1u,
						&close_ops) !=
		DOS_SFT_REGISTRY_CONTEXT_MISMATCH ||
	    dos_sft_registry_bind_backend_close(TEST_SFT_CONTEXT, &close_ops) !=
		DOS_SFT_REGISTRY_READY ||
	    dos_sft_registry_bind_backend_close(TEST_SFT_CONTEXT, &close_ops) !=
		DOS_SFT_REGISTRY_INVALID_STATE ||
	    dos_sft_registry_install(0u, 0u, 0u, 1u) !=
		DOS_SFT_REGISTRY_READY ||
	    dos_sft_registry_install(1u, DOS_SFT_FLAG_IS_NETWORK, 0u, 1u) !=
		DOS_SFT_REGISTRY_READY ||
	    dos_sft_registry_install(2u, DOS_SFT_FLAG_NO_INHERIT, 0u, 1u) !=
		DOS_SFT_REGISTRY_READY)
		return 1;
	ops = dos_sft_registry_ops();
	if (ops == NULL || ops->identity != TEST_SFT_ID ||
	    dos_sft_registry_context() != TEST_SFT_CONTEXT)
		return 2;

	unused_jft(&parent);
	parent.entries[0] = 0u;
	parent.entries[1] = 1u;
	parent.entries[2] = 2u;
	parent.entries[3] = 99u;
	if (dos_sft_batch_prepare(ops, TEST_SFT_CONTEXT, &parent, &batch) !=
		DOS_SFT_BATCH_OK ||
	    batch == DOS_SFT_BATCH_HANDLE_INVALID ||
	    dos_sft_batch_copy_child_jft(batch, &child) != DOS_SFT_BATCH_OK ||
	    child.entries[0] != 0u || child.entries[1] != 1u ||
	    child.entries[2] != DOS_JFT_ENTRY_UNUSED ||
	    child.entries[3] != DOS_JFT_ENTRY_UNUSED ||
	    !inspect_equals(0u, 2u, 2u) || !inspect_equals(1u, 2u, 0u) ||
	    !inspect_equals(2u, 1u, 1u))
		return 3;
	if (dos_sft_batch_abort(batch, ops, TEST_SFT_CONTEXT) !=
		DOS_SFT_BATCH_OK ||
	    !inspect_equals(0u, 1u, 1u) || !inspect_equals(1u, 1u, 0u) ||
	    dos_sft_batch_retire(batch) != DOS_SFT_BATCH_OK)
		return 4;

	/* A later acquire overflow must unwind every earlier local-device step. */
	if (dos_sft_registry_install(3u, DOS_SFT_FLAG_IS_NETWORK, 0u,
				     ~(uint32_t)0u) !=
		DOS_SFT_REGISTRY_READY)
		return 5;
	unused_jft(&parent);
	parent.entries[0] = 0u;
	parent.entries[1] = 3u;
	batch = DOS_SFT_BATCH_HANDLE_INVALID;
	if (dos_sft_batch_prepare(ops, TEST_SFT_CONTEXT, &parent, &batch) !=
		DOS_SFT_BATCH_ADAPTER_FAULT ||
	    batch != DOS_SFT_BATCH_HANDLE_INVALID ||
	    !inspect_equals(0u, 1u, 1u) ||
	    !inspect_equals(3u, ~(uint32_t)0u, 0u))
		return 6;
	return 0;
}

static int test_sft_reservation_and_shared_record(void)
{
	const struct dos_sft_batch_ops *ops = dos_sft_registry_ops();
	struct dos_sft_registry_publish_record record = {
		.backend_handle = TEST_FILE_HANDLE,
		.position = 10u,
		.size = 20u,
		.flags = 0u,
		.mode = 2u,
		.information = 0x42u,
		.backend_kind = (uint8_t)DOS_SFT_BACKEND_FILE,
		.reserved = 0u,
	};
	struct dos_sft_registry_view view = {0};
	struct dos_sft_registry_view untouched = {
		.reference_handle = 0x777u,
	};
	struct dos_sft_view alias = {
		.reference_handle = DOS_SFT_REFERENCE_HANDLE_INVALID,
	};
	dos_sft_reference_handle_t first = DOS_SFT_REFERENCE_HANDLE_INVALID;
	dos_sft_reference_handle_t second = DOS_SFT_REFERENCE_HANDLE_INVALID;
	dos_sft_reference_handle_t third = DOS_SFT_REFERENCE_HANDLE_INVALID;
	enum dos_error exact_error = DOS_ERROR_CRC;
	uint8_t first_sfn = DOS_JFT_ENTRY_UNUSED;
	uint8_t second_sfn = DOS_JFT_ENTRY_UNUSED;
	uint8_t third_sfn = DOS_JFT_ENTRY_UNUSED;

	if (ops == NULL ||
	    dos_sft_registry_reserve(TEST_SFT_CONTEXT, &first_sfn, &first) !=
		DOS_SFT_REGISTRY_READY ||
	    first_sfn != 4u || first == DOS_SFT_REFERENCE_HANDLE_INVALID ||
	    dos_sft_registry_inspect_open(TEST_SFT_CONTEXT, first, &view) !=
		DOS_SFT_REGISTRY_INVALID_STATE ||
	    dos_sft_registry_cancel(TEST_SFT_CONTEXT, first) !=
		DOS_SFT_REGISTRY_READY || !inspect_state(4u, DOS_SFT_SLOT_FREE))
		return 1;
	if (dos_sft_registry_reserve(TEST_SFT_CONTEXT, &second_sfn, &second) !=
		DOS_SFT_REGISTRY_READY ||
	    second_sfn != first_sfn || second == first ||
	    dos_sft_registry_publish(TEST_SFT_CONTEXT, first, &record) !=
		DOS_SFT_REGISTRY_STALE_REFERENCE ||
	    dos_sft_registry_publish(TEST_SFT_CONTEXT, second, &record) !=
		DOS_SFT_REGISTRY_READY)
		return 2;
	if (dos_sft_registry_resolve(TEST_SFT_CONTEXT + 1u, second_sfn,
				     &untouched) !=
		DOS_SFT_REGISTRY_CONTEXT_MISMATCH ||
	    untouched.reference_handle != 0x777u ||
	    dos_sft_registry_resolve(TEST_SFT_CONTEXT, second_sfn, &view) !=
		DOS_SFT_REGISTRY_READY ||
	    view.reference_handle != second ||
	    view.backend_handle != TEST_FILE_HANDLE || view.position != 10u ||
	    view.size != 20u || view.flags != 0u || view.mode != 2u ||
	    view.information != 0x42u ||
	    view.backend_kind != (uint8_t)DOS_SFT_BACKEND_FILE ||
	    view.references != 1u || view.device_opens != 1u)
		return 3;
	if (ops->lookup(TEST_SFT_CONTEXT, second_sfn, &alias) !=
		DOS_SFT_ADAPTER_OK ||
	    alias.reference_handle != second ||
	    dos_sft_registry_update_io(TEST_SFT_CONTEXT, alias.reference_handle,
				       77u, 100u, 0x1234u) !=
		DOS_SFT_REGISTRY_READY ||
	    dos_sft_registry_inspect_open(TEST_SFT_CONTEXT, second, &view) !=
		DOS_SFT_REGISTRY_READY ||
	    view.position != 77u || view.size != 100u ||
	    view.information != 0x1234u)
		return 4;

	/* Two JFT aliases observe one cursor and one SFT reference count. */
	if (ops->device_open(TEST_SFT_CONTEXT, second) != DOS_SFT_ADAPTER_OK ||
	    ops->reference_acquire(TEST_SFT_CONTEXT, second) !=
		DOS_SFT_ADAPTER_OK ||
	    !inspect_equals(second_sfn, 2u, 2u))
		return 5;
	configure_close(DOS_SFT_BACKEND_CLOSE_EXACT_FAILURE,
			DOS_ERROR_ACCESS_DENIED);
	if (dos_sft_registry_close_reference(TEST_SFT_CONTEXT, second,
					      &exact_error) !=
		DOS_SFT_REGISTRY_READY ||
	    exact_error != DOS_SUCCESS || backend_close_calls != 0u ||
	    !inspect_equals(second_sfn, 1u, 1u))
		return 6;
	exact_error = DOS_ERROR_CRC;
	if (dos_sft_registry_close_reference(TEST_SFT_CONTEXT, second,
					      &exact_error) !=
		DOS_SFT_REGISTRY_BACKEND_FAILURE ||
	    exact_error != DOS_ERROR_ACCESS_DENIED || backend_close_calls != 1u ||
	    closed_backend_handle != TEST_FILE_HANDLE ||
	    closed_backend_kind != DOS_SFT_BACKEND_FILE ||
	    !inspect_equals(second_sfn, 1u, 1u))
		return 7;
	configure_close(DOS_SFT_BACKEND_CLOSE_OK, DOS_SUCCESS);
	if (dos_sft_registry_close_reference(TEST_SFT_CONTEXT, second,
					      &exact_error) !=
		DOS_SFT_REGISTRY_READY ||
	    exact_error != DOS_SUCCESS || backend_close_calls != 1u ||
	    !inspect_state(second_sfn, DOS_SFT_SLOT_FREE) ||
	    dos_sft_registry_update_io(TEST_SFT_CONTEXT, second, 0u, 0u, 0u) !=
		DOS_SFT_REGISTRY_STALE_REFERENCE)
		return 8;
	if (dos_sft_registry_reserve(TEST_SFT_CONTEXT, &third_sfn, &third) !=
		DOS_SFT_REGISTRY_READY ||
	    third_sfn != second_sfn || third == second ||
	    dos_sft_registry_cancel(TEST_SFT_CONTEXT, third) !=
		DOS_SFT_REGISTRY_READY)
		return 9;
	return 0;
}

static int test_sft_uncertain_close_and_poison(void)
{
	struct dos_sft_registry_view untouched = {
		.reference_handle = 0x888u,
	};
	dos_sft_reference_handle_t reference = DOS_SFT_REFERENCE_HANDLE_INVALID;
	dos_sft_reference_handle_t reserved = DOS_SFT_REFERENCE_HANDLE_INVALID;
	enum dos_error exact_error = DOS_ERROR_CRC;
	uint8_t sfn = DOS_JFT_ENTRY_UNUSED;
	uint8_t poisoned_sfn = DOS_JFT_ENTRY_UNUSED;

	if (reserve_and_publish(DOS_SFT_BACKEND_DEVICE, TEST_DEVICE_HANDLE, 0u,
				&sfn, &reference) != DOS_SFT_REGISTRY_READY)
		return 1;
	configure_close(DOS_SFT_BACKEND_CLOSE_UNCERTAIN, DOS_ERROR_NOT_READY);
	if (dos_sft_registry_close_reference(TEST_SFT_CONTEXT, reference,
					      &exact_error) !=
		DOS_SFT_REGISTRY_POISONED ||
	    exact_error != DOS_ERROR_CRC || backend_close_calls != 1u ||
	    closed_backend_handle != TEST_DEVICE_HANDLE ||
	    closed_backend_kind != DOS_SFT_BACKEND_DEVICE ||
	    !inspect_state(sfn, DOS_SFT_SLOT_POISONED) ||
	    dos_sft_registry_inspect_open(TEST_SFT_CONTEXT, reference,
					  &untouched) !=
		DOS_SFT_REGISTRY_POISONED ||
	    untouched.reference_handle != 0x888u)
		return 2;
	if (dos_sft_registry_reserve(TEST_SFT_CONTEXT, &poisoned_sfn,
				     &reserved) != DOS_SFT_REGISTRY_READY ||
	    poisoned_sfn == sfn ||
	    dos_sft_registry_poison(TEST_SFT_CONTEXT, reserved) !=
		DOS_SFT_REGISTRY_READY ||
	    !inspect_state(poisoned_sfn, DOS_SFT_SLOT_POISONED) ||
	    dos_sft_registry_cancel(TEST_SFT_CONTEXT, reserved) !=
		DOS_SFT_REGISTRY_POISONED)
		return 3;
	return 0;
}

static int test_sft_exec_final_reference(void)
{
	const struct dos_sft_batch_ops *ops = dos_sft_registry_ops();
	struct dos_sft_view view = {
		.reference_handle = DOS_SFT_REFERENCE_HANDLE_INVALID,
	};

	if (ops == NULL ||
	    dos_sft_registry_install(6u, 0u, 0u, 1u) !=
		DOS_SFT_REGISTRY_READY ||
	    ops->lookup(TEST_SFT_CONTEXT, 6u, &view) != DOS_SFT_ADAPTER_OK ||
	    ops->reference_release(TEST_SFT_CONTEXT, view.reference_handle) !=
		DOS_SFT_ADAPTER_OK ||
	    !inspect_equals(6u, 0u, 1u))
		return 1;
	configure_close(DOS_SFT_BACKEND_CLOSE_EXACT_FAILURE,
			DOS_ERROR_NOT_READY);
	if (ops->device_close(TEST_SFT_CONTEXT, view.reference_handle) !=
		DOS_SFT_ADAPTER_FAULT ||
	    backend_close_calls != 1u ||
	    closed_backend_kind != DOS_SFT_BACKEND_STANDARD ||
	    !inspect_state(6u, DOS_SFT_SLOT_POISONED))
		return 2;
	if (dos_sft_registry_install(7u, 0u, 0u, 1u) !=
		DOS_SFT_REGISTRY_READY ||
	    ops->lookup(TEST_SFT_CONTEXT, 7u, &view) != DOS_SFT_ADAPTER_OK ||
	    ops->reference_release(TEST_SFT_CONTEXT, view.reference_handle) !=
		DOS_SFT_ADAPTER_OK)
		return 3;
	configure_close(DOS_SFT_BACKEND_CLOSE_OK, DOS_SUCCESS);
	if (ops->device_close(TEST_SFT_CONTEXT, view.reference_handle) !=
		DOS_SFT_ADAPTER_OK ||
	    backend_close_calls != 1u ||
	    !inspect_state(7u, DOS_SFT_SLOT_FREE))
		return 4;
	return 0;
}

static int test_exec_gate(void)
{
	const struct dos_exec_observer_ops *ops;
	uint64_t generation = 0u;
	uint64_t untouched = 0u;

	if (dos_exec_gate_initialize(TEST_GATE_ID, TEST_GATE_CONTEXT) !=
		DOS_EXEC_GATE_READY)
		return 1;
	ops = dos_exec_gate_ops();
	if (ops == NULL || ops->identity != TEST_GATE_ID ||
	    dos_exec_gate_context() != TEST_GATE_CONTEXT ||
	    dos_exec_gate_is_quarantined())
		return 2;
	if (ops->acquire(TEST_GATE_CONTEXT, &generation) !=
		DOS_EXEC_OBSERVER_ADAPTER_OK ||
	    generation == 0u ||
	    ops->acquire(TEST_GATE_CONTEXT, &untouched) !=
		DOS_EXEC_OBSERVER_ADAPTER_BUSY ||
	    untouched != 0u ||
	    ops->release(TEST_GATE_CONTEXT, generation + 1u) !=
		DOS_EXEC_OBSERVER_ADAPTER_FAULT ||
	    ops->release(TEST_GATE_CONTEXT, generation) !=
		DOS_EXEC_OBSERVER_ADAPTER_OK)
		return 3;
	generation = 0u;
	if (ops->acquire(TEST_GATE_CONTEXT, &generation) !=
		DOS_EXEC_OBSERVER_ADAPTER_OK ||
	    ops->quarantine(TEST_GATE_CONTEXT, generation) !=
		DOS_EXEC_OBSERVER_ADAPTER_OK ||
	    !dos_exec_gate_is_quarantined() ||
	    ops->acquire(TEST_GATE_CONTEXT, &untouched) !=
		DOS_EXEC_OBSERVER_ADAPTER_FAULT)
		return 4;
	return 0;
}

static int run_tests(void)
{
	int status = test_drive_visibility();

	if (status != 0)
		return 10 + status;
	status = test_sft_registry();
	if (status != 0)
		return 20 + status;
	status = test_sft_reservation_and_shared_record();
	if (status != 0)
		return 40 + status;
	status = test_sft_uncertain_close_and_poison();
	if (status != 0)
		return 60 + status;
	status = test_sft_exec_final_reference();
	if (status != 0)
		return 80 + status;
	status = test_exec_gate();
	return status == 0 ? 0 : 30 + status;
}

DOSC32_TEST_ENTRY(run_tests)

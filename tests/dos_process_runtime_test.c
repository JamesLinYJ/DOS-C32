// SPDX-License-Identifier: GPL-2.0-only
/* Freestanding tests for serialized CurrentPDB/DMAADD publication. */
#include "dos_process_runtime.h"
#include "test_entry.h"

#define TEST_FAILURE 1
#define TEST_SUCCESS 0
#define TEST_RUNTIME_IDENTITY ((kernel_object_handle_t)0x52554e54494d4531ull)

static int test_zero_psp_and_exec_publish(void)
{
	struct dos_process_runtime runtime = DOS_PROCESS_RUNTIME_INITIALIZER;
	struct dos_process_runtime_snapshot snapshot;
	struct dos_far_pointer16 initial_dta = {
	    .offset = 0x1234u,
	    .segment = 0u,
	};

	if (dos_process_runtime_initialize(&runtime, TEST_RUNTIME_IDENTITY, 0u,
					   initial_dta) !=
	    DOS_PROCESS_RUNTIME_OK)
		return TEST_FAILURE;
	if (dos_process_runtime_initialize(&runtime, TEST_RUNTIME_IDENTITY,
					   0x9999u, initial_dta) !=
		DOS_PROCESS_RUNTIME_INVALID_STATE ||
	    runtime.current_psp != 0u || runtime.generation != 1u)
		return TEST_FAILURE;
	if (dos_process_runtime_snapshot(&runtime, &snapshot) !=
	    DOS_PROCESS_RUNTIME_OK)
		return TEST_FAILURE;
	if (snapshot.current_psp != 0u || snapshot.dta.segment != 0u ||
	    snapshot.dta.offset != 0x1234u || snapshot.generation != 1u)
		return TEST_FAILURE;
	if (snapshot.runtime_identity != TEST_RUNTIME_IDENTITY)
		return TEST_FAILURE;
	if (dos_process_runtime_preflight_exec(&runtime, &snapshot) !=
		DOS_PROCESS_RUNTIME_OK ||
	    runtime.current_psp != 0u || runtime.dta.offset != 0x1234u ||
	    runtime.generation != 1u)
		return TEST_FAILURE;
	if (dos_process_runtime_publish_exec(&runtime, &snapshot, 0u) !=
	    DOS_PROCESS_RUNTIME_OK)
		return TEST_FAILURE;
	return runtime.current_psp == 0u && runtime.dta.segment == 0u &&
		       runtime.dta.offset == 0x0080u && runtime.generation == 2u
		   ? TEST_SUCCESS
		   : TEST_FAILURE;
}

static int test_explicit_construct_path(void)
{
	struct dos_process_runtime runtime = {
	    .generation = 0xffffu,
	    .dta = {.offset = 0xaaaau, .segment = 0xbbbbu},
	    .current_psp = 0xccccu,
	    .initialized = true,
	    .poisoned = true,
	    .constructed = false,
	};
	struct dos_far_pointer16 dta = {
	    .offset = 0x0080u,
	    .segment = 0x1234u,
	};

	if (dos_process_runtime_construct(&runtime) != DOS_PROCESS_RUNTIME_OK ||
	    !runtime.constructed || runtime.initialized || runtime.poisoned ||
	    runtime.generation != 0u ||
	    runtime.identity != KERNEL_OBJECT_HANDLE_INVALID)
		return TEST_FAILURE;
	runtime.generation = 7u;
	if (dos_process_runtime_initialize(&runtime, TEST_RUNTIME_IDENTITY,
					   0x1234u, dta) !=
		DOS_PROCESS_RUNTIME_INVALID_ARGUMENT ||
	    runtime.generation != 7u || runtime.initialized ||
	    runtime.poisoned ||
	    runtime.identity != KERNEL_OBJECT_HANDLE_INVALID)
		return TEST_FAILURE;
	runtime.generation = 0u;
	if (dos_process_runtime_initialize(
		&runtime, KERNEL_OBJECT_HANDLE_INVALID, 0x1234u, dta) !=
		DOS_PROCESS_RUNTIME_INVALID_ARGUMENT ||
	    runtime.initialized || runtime.generation != 0u ||
	    runtime.identity != KERNEL_OBJECT_HANDLE_INVALID)
		return TEST_FAILURE;
	if (dos_process_runtime_initialize(&runtime, TEST_RUNTIME_IDENTITY,
					   0x1234u,
					   dta) != DOS_PROCESS_RUNTIME_OK)
		return TEST_FAILURE;
	return runtime.generation == 1u && runtime.current_psp == 0x1234u &&
		       runtime.dta.offset == 0x0080u &&
		       runtime.dta.segment == 0x1234u
		   ? TEST_SUCCESS
		   : TEST_FAILURE;
}

static int test_stale_snapshot_is_side_effect_free(void)
{
	struct dos_process_runtime runtime = DOS_PROCESS_RUNTIME_INITIALIZER;
	struct dos_process_runtime_snapshot snapshot;
	struct dos_far_pointer16 initial_dta = {
	    .offset = 0x0080u,
	    .segment = 0x1000u,
	};
	struct dos_far_pointer16 changed_dta = {
	    .offset = 0x0200u,
	    .segment = 0x3000u,
	};
	uint64_t generation;

	if (dos_process_runtime_initialize(&runtime, TEST_RUNTIME_IDENTITY,
					   0x1000u, initial_dta) !=
	    DOS_PROCESS_RUNTIME_OK)
		return TEST_FAILURE;
	if (dos_process_runtime_snapshot(&runtime, &snapshot) !=
	    DOS_PROCESS_RUNTIME_OK)
		return TEST_FAILURE;
	if (dos_process_runtime_set_dta(&runtime, changed_dta) !=
	    DOS_PROCESS_RUNTIME_OK)
		return TEST_FAILURE;
	generation = runtime.generation;
	if (dos_process_runtime_preflight_exec(&runtime, &snapshot) !=
	    DOS_PROCESS_RUNTIME_STALE_SNAPSHOT)
		return TEST_FAILURE;
	if (dos_process_runtime_publish_exec(&runtime, &snapshot, 0x4000u) !=
	    DOS_PROCESS_RUNTIME_STALE_SNAPSHOT)
		return TEST_FAILURE;
	return runtime.generation == generation &&
		       runtime.current_psp == 0x1000u &&
		       runtime.dta.offset == changed_dta.offset &&
		       runtime.dta.segment == changed_dta.segment
		   ? TEST_SUCCESS
		   : TEST_FAILURE;
}

static int test_current_psp_update_invalidates_exec(void)
{
	struct dos_process_runtime runtime = DOS_PROCESS_RUNTIME_INITIALIZER;
	struct dos_process_runtime_snapshot snapshot;
	struct dos_far_pointer16 dta = {
	    .offset = 0x80u,
	    .segment = 0x1111u,
	};

	if (dos_process_runtime_initialize(&runtime, TEST_RUNTIME_IDENTITY,
					   0x1111u,
					   dta) != DOS_PROCESS_RUNTIME_OK)
		return TEST_FAILURE;
	if (dos_process_runtime_snapshot(&runtime, &snapshot) !=
	    DOS_PROCESS_RUNTIME_OK)
		return TEST_FAILURE;
	if (dos_process_runtime_set_current_psp(&runtime, 0u) !=
	    DOS_PROCESS_RUNTIME_OK)
		return TEST_FAILURE;
	if (runtime.dta.offset != 0x80u || runtime.dta.segment != 0x1111u)
		return TEST_FAILURE;
	return dos_process_runtime_publish_exec(&runtime, &snapshot, 0x2222u) ==
		       DOS_PROCESS_RUNTIME_STALE_SNAPSHOT
		   ? TEST_SUCCESS
		   : TEST_FAILURE;
}

static int test_generation_exhaustion_does_not_wrap(void)
{
	struct dos_process_runtime runtime = DOS_PROCESS_RUNTIME_INITIALIZER;
	struct dos_far_pointer16 dta = {
	    .offset = 0u,
	    .segment = 0u,
	};
	struct dos_process_runtime_snapshot snapshot = {
	    .generation = DOS_PROCESS_RUNTIME_GENERATION_MAX,
	    .runtime_identity = TEST_RUNTIME_IDENTITY,
	    .dta = {.offset = 0u, .segment = 0u},
	    .current_psp = 0x1111u,
	    .reserved = 0u,
	};

	if (dos_process_runtime_initialize(&runtime, TEST_RUNTIME_IDENTITY,
					   0x1111u,
					   dta) != DOS_PROCESS_RUNTIME_OK)
		return TEST_FAILURE;
	runtime.generation = DOS_PROCESS_RUNTIME_GENERATION_MAX;
	if (dos_process_runtime_preflight_exec(&runtime, &snapshot) !=
	    DOS_PROCESS_RUNTIME_GENERATION_EXHAUSTED)
		return TEST_FAILURE;
	if (dos_process_runtime_set_current_psp(&runtime, 0x2222u) !=
	    DOS_PROCESS_RUNTIME_GENERATION_EXHAUSTED)
		return TEST_FAILURE;
	return runtime.generation == DOS_PROCESS_RUNTIME_GENERATION_MAX &&
		       runtime.current_psp == 0x1111u
		   ? TEST_SUCCESS
		   : TEST_FAILURE;
}

static int test_poison_is_sticky(void)
{
	struct dos_process_runtime runtime = DOS_PROCESS_RUNTIME_INITIALIZER;
	struct dos_process_runtime_snapshot snapshot = {
	    .generation = 0xfeedu,
	    .runtime_identity = TEST_RUNTIME_IDENTITY,
	    .dta = {.offset = 0xbeefu, .segment = 0xcafeu},
	    .current_psp = 0xaaaau,
	    .reserved = 0u,
	};
	struct dos_far_pointer16 dta = {
	    .offset = 0u,
	    .segment = 0u,
	};

	if (dos_process_runtime_initialize(&runtime, TEST_RUNTIME_IDENTITY, 0u,
					   dta) != DOS_PROCESS_RUNTIME_OK)
		return TEST_FAILURE;
	if (dos_process_runtime_poison(&runtime) != DOS_PROCESS_RUNTIME_OK)
		return TEST_FAILURE;
	if (dos_process_runtime_initialize(&runtime, TEST_RUNTIME_IDENTITY, 1u,
					   dta) !=
		DOS_PROCESS_RUNTIME_POISONED ||
	    !runtime.poisoned || runtime.current_psp != 0u)
		return TEST_FAILURE;
	if (dos_process_runtime_set_current_psp(&runtime, 1u) !=
	    DOS_PROCESS_RUNTIME_POISONED)
		return TEST_FAILURE;
	if (dos_process_runtime_snapshot(&runtime, &snapshot) !=
	    DOS_PROCESS_RUNTIME_POISONED)
		return TEST_FAILURE;
	return snapshot.generation == 0xfeedu &&
		       snapshot.current_psp == 0xaaaau && runtime.poisoned
		   ? TEST_SUCCESS
		   : TEST_FAILURE;
}

static int test_malformed_encoding_fails_closed(void)
{
	struct dos_process_runtime runtime = DOS_PROCESS_RUNTIME_INITIALIZER;
	struct dos_process_runtime_snapshot snapshot = {
	    .generation = 0xfeedu,
	    .runtime_identity = TEST_RUNTIME_IDENTITY,
	    .dta = {.offset = 0xbeefu, .segment = 0xcafeu},
	    .current_psp = 0xaaaau,
	    .reserved = 0u,
	};
	struct dos_process_runtime_snapshot expected = snapshot;
	struct dos_far_pointer16 dta = {
	    .offset = 0x80u,
	    .segment = 0x1111u,
	};

	if (dos_process_runtime_initialize(&runtime, TEST_RUNTIME_IDENTITY,
					   0x1111u,
					   dta) != DOS_PROCESS_RUNTIME_OK)
		return TEST_FAILURE;
	runtime.identity = KERNEL_OBJECT_HANDLE_INVALID;
	if (dos_process_runtime_snapshot(&runtime, &snapshot) !=
		DOS_PROCESS_RUNTIME_INVALID_ARGUMENT ||
	    snapshot.generation != 0xfeedu)
		return TEST_FAILURE;
	runtime.identity = TEST_RUNTIME_IDENTITY;
	runtime.reserved[0] = 1u;
	if (dos_process_runtime_snapshot(&runtime, &snapshot) !=
		DOS_PROCESS_RUNTIME_INVALID_ARGUMENT ||
	    dos_process_runtime_set_current_psp(&runtime, 0x2222u) !=
		DOS_PROCESS_RUNTIME_INVALID_ARGUMENT ||
	    dos_process_runtime_preflight_exec(&runtime, &expected) !=
		DOS_PROCESS_RUNTIME_INVALID_ARGUMENT ||
	    dos_process_runtime_poison(&runtime) !=
		DOS_PROCESS_RUNTIME_INVALID_ARGUMENT ||
	    snapshot.generation != 0xfeedu || runtime.current_psp != 0x1111u ||
	    runtime.poisoned != 0u)
		return TEST_FAILURE;
	runtime.reserved[0] = 0u;
	runtime.initialized = 2u;
	return dos_process_runtime_set_dta(&runtime, dta) ==
		       DOS_PROCESS_RUNTIME_INVALID_ARGUMENT
		   ? TEST_SUCCESS
		   : TEST_FAILURE;
}

static int test_runtime_identity_prevents_cross_lifetime_snapshot(void)
{
	struct dos_process_runtime first = DOS_PROCESS_RUNTIME_INITIALIZER;
	struct dos_process_runtime second = DOS_PROCESS_RUNTIME_INITIALIZER;
	struct dos_process_runtime_snapshot snapshot;
	struct dos_far_pointer16 dta = {
	    .offset = 0x0080u,
	    .segment = 0x1111u,
	};

	if (dos_process_runtime_initialize(&first, TEST_RUNTIME_IDENTITY,
					   0x1111u,
					   dta) != DOS_PROCESS_RUNTIME_OK ||
	    dos_process_runtime_initialize(
		&second, (kernel_object_handle_t)0x52554e54494d4532ull, 0x1111u,
		dta) != DOS_PROCESS_RUNTIME_OK ||
	    dos_process_runtime_snapshot(&first, &snapshot) !=
		DOS_PROCESS_RUNTIME_OK)
		return TEST_FAILURE;
	if (dos_process_runtime_preflight_exec(&second, &snapshot) !=
	    DOS_PROCESS_RUNTIME_STALE_SNAPSHOT)
		return TEST_FAILURE;
	return second.generation == 1u && second.current_psp == 0x1111u &&
		       second.dta.offset == 0x0080u
		   ? TEST_SUCCESS
		   : TEST_FAILURE;
}

static int test_exact_parent_restore(void)
{
	struct dos_process_runtime runtime = DOS_PROCESS_RUNTIME_INITIALIZER;
	struct dos_process_runtime_snapshot parent;
	struct dos_process_runtime_snapshot child;
	struct dos_far_pointer16 parent_dta = {
		.offset = 0x3456u,
		.segment = 0x1111u,
	};

	if (dos_process_runtime_initialize(&runtime, TEST_RUNTIME_IDENTITY,
					   0x1111u, parent_dta) !=
		DOS_PROCESS_RUNTIME_OK ||
	    dos_process_runtime_snapshot(&runtime, &parent) !=
		DOS_PROCESS_RUNTIME_OK ||
	    dos_process_runtime_publish_exec(&runtime, &parent, 0x2222u) !=
		DOS_PROCESS_RUNTIME_OK ||
	    dos_process_runtime_snapshot(&runtime, &child) !=
		DOS_PROCESS_RUNTIME_OK ||
	    dos_process_runtime_restore_parent(&runtime, &child, &parent) !=
		DOS_PROCESS_RUNTIME_OK)
		return TEST_FAILURE;
	if (runtime.current_psp != 0x1111u ||
	    runtime.dta.offset != parent_dta.offset ||
	    runtime.dta.segment != parent_dta.segment ||
	    runtime.generation != child.generation + 1u)
		return TEST_FAILURE;
	return dos_process_runtime_restore_parent(&runtime, &child, &parent) ==
		       DOS_PROCESS_RUNTIME_STALE_SNAPSHOT
		? TEST_SUCCESS
		: TEST_FAILURE;
}

static int run_tests(void)
{
	if (test_zero_psp_and_exec_publish() != TEST_SUCCESS)
		return 1;
	if (test_explicit_construct_path() != TEST_SUCCESS)
		return 2;
	if (test_stale_snapshot_is_side_effect_free() != TEST_SUCCESS)
		return 3;
	if (test_current_psp_update_invalidates_exec() != TEST_SUCCESS)
		return 4;
	if (test_generation_exhaustion_does_not_wrap() != TEST_SUCCESS)
		return 5;
	if (test_poison_is_sticky() != TEST_SUCCESS)
		return 6;
	if (test_malformed_encoding_fails_closed() != TEST_SUCCESS)
		return 7;
	if (test_runtime_identity_prevents_cross_lifetime_snapshot() !=
	    TEST_SUCCESS)
		return 8;
	if (test_exact_parent_restore() != TEST_SUCCESS)
		return 9;
	return 0;
}

DOSC32_TEST_ENTRY(run_tests)

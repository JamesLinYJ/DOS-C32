// SPDX-License-Identifier: GPL-2.0-only
/* Freestanding tests for the DOS EXEC observation barrier. */
#include "dos_exec_observer.h"
#include "test_entry.h"

#define TEST_IDENTITY ((kernel_object_handle_t)0x4f42534552564552ull)
#define TEST_CONTEXT ((kernel_object_handle_t)0x4558454347544553ull)

static uint64_t next_generation;
static uint64_t held_generation;
static uint32_t acquire_calls;
static uint32_t release_calls;
static uint32_t quarantine_calls;
static const struct dos_exec_observer *observer_under_test;
static uint8_t quarantine_saw_poison;
static uint8_t publish_generation_on_failed_acquire;
static enum dos_exec_observer_adapter_status acquire_result;
static enum dos_exec_observer_adapter_status release_result;
static enum dos_exec_observer_adapter_status quarantine_result;

static enum dos_exec_observer_adapter_status
test_acquire(kernel_object_handle_t context, uint64_t *generation)
{
	++acquire_calls;
	if (context != TEST_CONTEXT || generation == NULL)
		return DOS_EXEC_OBSERVER_ADAPTER_FAULT;
	if (acquire_result != DOS_EXEC_OBSERVER_ADAPTER_OK) {
		if (publish_generation_on_failed_acquire != 0u) {
			*generation = next_generation;
			held_generation = next_generation;
		}
		return acquire_result;
	}
	*generation = next_generation;
	held_generation = next_generation;
	return DOS_EXEC_OBSERVER_ADAPTER_OK;
}

static enum dos_exec_observer_adapter_status
test_release(kernel_object_handle_t context, uint64_t generation)
{
	++release_calls;
	if (context != TEST_CONTEXT || generation != held_generation)
		return DOS_EXEC_OBSERVER_ADAPTER_FAULT;
	if (release_result == DOS_EXEC_OBSERVER_ADAPTER_OK)
		held_generation = 0u;
	return release_result;
}

static enum dos_exec_observer_adapter_status
test_quarantine(kernel_object_handle_t context, uint64_t generation)
{
	++quarantine_calls;
	if (observer_under_test != NULL &&
	    observer_under_test->state == DOS_EXEC_OBSERVER_STATE_POISONED)
		quarantine_saw_poison = 1u;
	if (context != TEST_CONTEXT || generation != held_generation)
		return DOS_EXEC_OBSERVER_ADAPTER_FAULT;
	held_generation = 0u;
	return quarantine_result;
}

static void reset_adapter(void)
{
	next_generation = 1u;
	held_generation = 0u;
	acquire_calls = 0u;
	release_calls = 0u;
	quarantine_calls = 0u;
	observer_under_test = NULL;
	quarantine_saw_poison = 0u;
	publish_generation_on_failed_acquire = 0u;
	acquire_result = DOS_EXEC_OBSERVER_ADAPTER_OK;
	release_result = DOS_EXEC_OBSERVER_ADAPTER_OK;
	quarantine_result = DOS_EXEC_OBSERVER_ADAPTER_OK;
}

static int test_normal_lifecycle(void)
{
	static const struct dos_exec_observer_ops ops = {
	    .identity = TEST_IDENTITY,
	    .acquire = test_acquire,
	    .release = test_release,
	    .quarantine = test_quarantine,
	};
	struct dos_exec_observer observer;

	reset_adapter();
	observer_under_test = &observer;
	if (dos_exec_observer_construct(&observer) != DOS_EXEC_OBSERVER_OK ||
	    dos_exec_observer_acquire(&observer, &ops, TEST_CONTEXT) !=
		DOS_EXEC_OBSERVER_OK ||
	    observer.state != DOS_EXEC_OBSERVER_STATE_HELD ||
	    observer.adapter_identity != TEST_IDENTITY ||
	    observer.context != TEST_CONTEXT || observer.generation != 1u ||
	    held_generation != 1u || acquire_calls != 1u)
		return 1;
	if (dos_exec_observer_validate_held(&observer, &ops, TEST_CONTEXT) !=
		DOS_EXEC_OBSERVER_OK ||
	    acquire_calls != 1u || release_calls != 0u)
		return 2;
	if (dos_exec_observer_acquire(&observer, &ops, TEST_CONTEXT) !=
		DOS_EXEC_OBSERVER_INVALID_STATE ||
	    acquire_calls != 1u)
		return 3;
	if (dos_exec_observer_release(&observer, &ops, TEST_CONTEXT) !=
		DOS_EXEC_OBSERVER_OK ||
	    observer.state != DOS_EXEC_OBSERVER_STATE_RELEASED ||
	    held_generation != 0u || release_calls != 1u)
		return 4;
	if (dos_exec_observer_release(&observer, &ops, TEST_CONTEXT) !=
		DOS_EXEC_OBSERVER_OK ||
	    release_calls != 1u)
		return 5;
	return 0;
}

static int test_adapter_failures(void)
{
	static const struct dos_exec_observer_ops ops = {
	    .identity = TEST_IDENTITY,
	    .acquire = test_acquire,
	    .release = test_release,
	    .quarantine = test_quarantine,
	};
	struct dos_exec_observer observer = DOS_EXEC_OBSERVER_INITIALIZER;

	reset_adapter();
	observer_under_test = &observer;
	acquire_result = DOS_EXEC_OBSERVER_ADAPTER_BUSY;
	if (dos_exec_observer_acquire(&observer, &ops, TEST_CONTEXT) !=
		DOS_EXEC_OBSERVER_BUSY ||
	    observer.state != DOS_EXEC_OBSERVER_STATE_IDLE ||
	    held_generation != 0u)
		return 1;
	acquire_result = DOS_EXEC_OBSERVER_ADAPTER_OK;
	next_generation = 7u;
	if (dos_exec_observer_acquire(&observer, &ops, TEST_CONTEXT) !=
	    DOS_EXEC_OBSERVER_OK)
		return 2;
	release_result = DOS_EXEC_OBSERVER_ADAPTER_FAULT;
	if (dos_exec_observer_release(&observer, &ops, TEST_CONTEXT) !=
		DOS_EXEC_OBSERVER_POISONED ||
	    observer.state != DOS_EXEC_OBSERVER_STATE_POISONED ||
	    release_calls != 1u || quarantine_calls != 1u ||
	    held_generation != 0u || quarantine_saw_poison == 0u)
		return 3;
	if (dos_exec_observer_release(&observer, &ops, TEST_CONTEXT) !=
		DOS_EXEC_OBSERVER_POISONED ||
	    release_calls != 1u || quarantine_calls != 1u)
		return 4;
	return 0;
}

static int test_identity_and_zero_generation(void)
{
	static const struct dos_exec_observer_ops ops = {
	    .identity = TEST_IDENTITY,
	    .acquire = test_acquire,
	    .release = test_release,
	    .quarantine = test_quarantine,
	};
	struct dos_exec_observer_ops wrong_ops = ops;
	struct dos_exec_observer observer = DOS_EXEC_OBSERVER_INITIALIZER;

	reset_adapter();
	observer_under_test = &observer;
	next_generation = 9u;
	if (dos_exec_observer_acquire(&observer, &ops, TEST_CONTEXT) !=
	    DOS_EXEC_OBSERVER_OK)
		return 1;
	wrong_ops.identity++;
	if (dos_exec_observer_release(&observer, &wrong_ops, TEST_CONTEXT) !=
		DOS_EXEC_OBSERVER_IDENTITY_MISMATCH ||
	    release_calls != 0u || held_generation != 9u)
		return 2;
	if (dos_exec_observer_release(&observer, &ops, TEST_CONTEXT + 1u) !=
		DOS_EXEC_OBSERVER_CONTEXT_MISMATCH ||
	    release_calls != 0u || held_generation != 9u)
		return 3;
	if (dos_exec_observer_poison(&observer, &ops, TEST_CONTEXT) !=
		DOS_EXEC_OBSERVER_OK ||
	    observer.state != DOS_EXEC_OBSERVER_STATE_POISONED ||
	    quarantine_calls != 1u || held_generation != 0u ||
	    quarantine_saw_poison == 0u)
		return 4;

	observer = (struct dos_exec_observer)DOS_EXEC_OBSERVER_INITIALIZER;
	reset_adapter();
	observer_under_test = &observer;
	next_generation = 0u;
	if (dos_exec_observer_acquire(&observer, &ops, TEST_CONTEXT) !=
		DOS_EXEC_OBSERVER_POISONED ||
	    observer.state != DOS_EXEC_OBSERVER_STATE_POISONED ||
	    quarantine_calls != 1u || quarantine_saw_poison == 0u)
		return 5;

	observer = (struct dos_exec_observer)DOS_EXEC_OBSERVER_INITIALIZER;
	reset_adapter();
	observer.adapter_identity = TEST_IDENTITY;
	observer.context = TEST_CONTEXT;
	observer.state = (uint8_t)DOS_EXEC_OBSERVER_STATE_HELD;
	if (dos_exec_observer_validate_held(&observer, &ops, TEST_CONTEXT) !=
		DOS_EXEC_OBSERVER_INVALID_STATE ||
	    dos_exec_observer_release(&observer, &ops, TEST_CONTEXT) !=
		DOS_EXEC_OBSERVER_INVALID_STATE ||
	    dos_exec_observer_poison(&observer, &ops, TEST_CONTEXT) !=
		DOS_EXEC_OBSERVER_INVALID_STATE ||
	    release_calls != 0u || quarantine_calls != 0u)
		return 6;
	return 0;
}

static int test_uncertain_acquire_is_quarantined(void)
{
	static const struct dos_exec_observer_ops ops = {
	    .identity = TEST_IDENTITY,
	    .acquire = test_acquire,
	    .release = test_release,
	    .quarantine = test_quarantine,
	};
	struct dos_exec_observer observer = DOS_EXEC_OBSERVER_INITIALIZER;

	reset_adapter();
	observer_under_test = &observer;
	next_generation = 11u;
	publish_generation_on_failed_acquire = 1u;
	acquire_result = DOS_EXEC_OBSERVER_ADAPTER_BUSY;
	if (dos_exec_observer_acquire(&observer, &ops, TEST_CONTEXT) !=
		DOS_EXEC_OBSERVER_POISONED ||
	    observer.state != DOS_EXEC_OBSERVER_STATE_POISONED ||
	    observer.generation != 11u || quarantine_calls != 1u ||
	    held_generation != 0u || quarantine_saw_poison == 0u)
		return 1;

	observer = (struct dos_exec_observer)DOS_EXEC_OBSERVER_INITIALIZER;
	reset_adapter();
	observer_under_test = &observer;
	acquire_result = (enum dos_exec_observer_adapter_status)99;
	if (dos_exec_observer_acquire(&observer, &ops, TEST_CONTEXT) !=
		DOS_EXEC_OBSERVER_POISONED ||
	    observer.state != DOS_EXEC_OBSERVER_STATE_POISONED ||
	    quarantine_calls != 1u || quarantine_saw_poison == 0u)
		return 2;

	observer = (struct dos_exec_observer)DOS_EXEC_OBSERVER_INITIALIZER;
	observer.constructed = 2u;
	if (dos_exec_observer_acquire(&observer, &ops, TEST_CONTEXT) !=
		DOS_EXEC_OBSERVER_INVALID_ARGUMENT ||
	    acquire_calls != 1u)
		return 3;
	return 0;
}

static int run_tests(void)
{
	int status;

	status = test_normal_lifecycle();
	if (status != 0)
		return 10 + status;
	status = test_adapter_failures();
	if (status != 0)
		return 20 + status;
	status = test_identity_and_zero_generation();
	if (status != 0)
		return 30 + status;
	status = test_uncertain_acquire_is_quarantined();
	if (status != 0)
		return 40 + status;
	return 0;
}

DOSC32_TEST_ENTRY(run_tests)

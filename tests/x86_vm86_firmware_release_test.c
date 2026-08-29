// SPDX-License-Identifier: GPL-2.0-only
/* Host-safe fault injection for VM86 firmware-shadow teardown policy. */
#include "vm86_firmware.h"

#include "test_entry.h"

#define TEST_MACHINE_IDENTITY 0x4d414348u

static const struct x86_guest_space_firmware_binding test_binding = {
	.address_space_identity = 0x41444452u,
	.address_space_generation = 7u,
	.machine_identity = TEST_MACHINE_IDENTITY,
	.execution_generation = 9u,
	.client_generation = 11u,
	.client_slot = 1u,
	.reserved = {0u},
};

static enum x86_guest_space_status terminal_release_status;
static enum x86_guest_space_status configured_quarantine_status;
static uint32_t release_retry_count;
static uint32_t release_calls;
static uint32_t quarantine_calls;
static bool arguments_valid;

static void configure(enum x86_guest_space_status terminal_status,
		      uint32_t retry_count,
		      enum x86_guest_space_status quarantine_status)
{
	terminal_release_status = terminal_status;
	configured_quarantine_status = quarantine_status;
	release_retry_count = retry_count;
	release_calls = 0u;
	quarantine_calls = 0u;
	arguments_valid = true;
}

enum x86_guest_space_status x86_guest_space_firmware_execution_release(
	kernel_object_handle_t machine_identity,
	const struct x86_guest_space_firmware_binding *binding)
{
	++release_calls;
	if (machine_identity != TEST_MACHINE_IDENTITY || binding != &test_binding)
		arguments_valid = false;
	if (release_calls <= release_retry_count)
		return X86_GUEST_SPACE_DEVICE_EVENT_RETRY;
	return terminal_release_status;
}

enum x86_guest_space_status x86_guest_space_firmware_execution_quarantine(
	kernel_object_handle_t machine_identity,
	const struct x86_guest_space_firmware_binding *binding)
{
	++quarantine_calls;
	if (machine_identity != TEST_MACHINE_IDENTITY || binding != &test_binding)
		arguments_valid = false;
	return configured_quarantine_status;
}

static int run_tests(void)
{
	configure(X86_GUEST_SPACE_OK, 0u, X86_GUEST_SPACE_DEVICE_FAULT);
	if (x86_vm86_firmware_execution_release(
		    TEST_MACHINE_IDENTITY, &test_binding) !=
		    DOS_EXEC_BACKEND_RELEASED ||
	    release_calls != 1u || quarantine_calls != 0u || !arguments_valid)
		return 1;

	configure(X86_GUEST_SPACE_OK, 2u, X86_GUEST_SPACE_DEVICE_FAULT);
	if (x86_vm86_firmware_execution_release(
		    TEST_MACHINE_IDENTITY, &test_binding) !=
		    DOS_EXEC_BACKEND_RELEASED ||
	    release_calls != 3u || quarantine_calls != 0u || !arguments_valid)
		return 2;

	/* Permanent zero-before-free failure consumes the bounded retry budget and
	 * then forces emergency alias revocation/quarantine. */
	configure(X86_GUEST_SPACE_DEVICE_EVENT_RETRY, (uint32_t)-1,
		  X86_GUEST_SPACE_DEVICE_FAULT);
	if (x86_vm86_firmware_execution_release(
		    TEST_MACHINE_IDENTITY, &test_binding) !=
		    DOS_EXEC_BACKEND_RELEASE_UNCERTAIN ||
	    release_calls != 3u || quarantine_calls != 1u || !arguments_valid)
		return 3;

	/* Mapping/binding corruption is not a special escape hatch: it takes the
	 * same forced-revocation path before the adapter returns uncertainty. */
	configure(X86_GUEST_SPACE_DEVICE_FAULT, 0u,
		  X86_GUEST_SPACE_DEVICE_FAULT);
	if (x86_vm86_firmware_execution_release(
		    TEST_MACHINE_IDENTITY, &test_binding) !=
		    DOS_EXEC_BACKEND_RELEASE_UNCERTAIN ||
	    release_calls != 1u || quarantine_calls != 1u || !arguments_valid)
		return 4;

	/* Even a failed quarantine response cannot be misreported as released. */
	configure(X86_GUEST_SPACE_PAGING_MISMATCH, 0u,
		  X86_GUEST_SPACE_INVALID_STATE);
	if (x86_vm86_firmware_execution_release(
		    TEST_MACHINE_IDENTITY, &test_binding) !=
		    DOS_EXEC_BACKEND_RELEASE_UNCERTAIN ||
	    release_calls != 1u || quarantine_calls != 1u || !arguments_valid)
		return 5;

	return 0;
}

DOSC32_TEST_ENTRY(run_tests)

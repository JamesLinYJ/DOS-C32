// SPDX-License-Identifier: GPL-2.0-only
/* Bounded fail-closed teardown for one VM86 firmware-shadow client. */
#include "vm86_firmware.h"

#include "../../config/x86-guest-space.h"

static_assert_expression(CONFIG_X86_GUEST_FIRMWARE_RELEASE_ATTEMPTS > 0u,
	"firmware release must have a bounded progress attempt");

enum dos_exec_backend_release_status x86_vm86_firmware_execution_release(
	kernel_object_handle_t machine_identity,
	const struct x86_guest_space_firmware_binding *binding)
{
	enum x86_guest_space_status firmware_status;
	enum x86_guest_space_status quarantine_status;
	uint32_t attempt;

	firmware_status = X86_GUEST_SPACE_DEVICE_EVENT_RETRY;
	for (attempt = 0u;
	     attempt < CONFIG_X86_GUEST_FIRMWARE_RELEASE_ATTEMPTS; ++attempt) {
		firmware_status = x86_guest_space_firmware_execution_release(
			machine_identity, binding);
		if (firmware_status != X86_GUEST_SPACE_DEVICE_EVENT_RETRY)
			break;
	}
	if (firmware_status == X86_GUEST_SPACE_OK)
		return DOS_EXEC_BACKEND_RELEASED;

	/*
	 * The session owner invokes backend release only once. Every uncertain
	 * lower result therefore takes the same fail-closed path: force-remove all
	 * still-active low firmware aliases, retain any backing which cannot be
	 * safely returned, and poison the guest before its backend slot is reused.
	 * Quarantine cannot turn an uncertain release back into an exact one.
	 */
	quarantine_status = x86_guest_space_firmware_execution_quarantine(
		machine_identity, binding);
	(void)quarantine_status;
	return DOS_EXEC_BACKEND_RELEASE_UNCERTAIN;
}

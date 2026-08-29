/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DOSC32_X86_GUEST_FAULT_UI_H
#define DOSC32_X86_GUEST_FAULT_UI_H

#include "dos_machine.h"
#include "types.h"

/* Presentation-only value object; it owns no guest or kernel resources. */
struct x86_guest_fault_snapshot {
	struct dos_cpu_state cpu;
	uint32_t step_status;
	uint32_t session_status;
	uint32_t event_kind;
	uint32_t machine_status;
	uint16_t port;
	uint8_t vector;
	uint8_t last_chained_vector;
	uint8_t io_width;
	bool io_write;
};

void x86_guest_fault_ui_show(const struct x86_guest_fault_snapshot *fault);

#endif

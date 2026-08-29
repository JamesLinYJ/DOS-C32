/* SPDX-License-Identifier: GPL-2.0-only */
/* Backend-neutral decoding of scalar real-mode IN/OUT instructions. */
#ifndef DOSC32_DOS_PORT_INSTRUCTION_H
#define DOSC32_DOS_PORT_INSTRUCTION_H

#include "compiler.h"
#include "exec_backend.h"
#include "types.h"

enum dos_port_instruction_status {
	DOS_PORT_INSTRUCTION_DECODED = 0,
	DOS_PORT_INSTRUCTION_NOT_HANDLED,
	DOS_PORT_INSTRUCTION_INVALID_ARGUMENT,
	DOS_PORT_INSTRUCTION_FETCH_FAULT
};

struct dos_port_instruction_result {
	struct dos_cpu_state state;
	struct dos_execution_event event;
	uint32_t status;
	uint32_t machine_status;
} __aligned(8);

static_assert_expression(sizeof(struct dos_port_instruction_result) == 80u,
			 "port instruction result must stay fixed width");

struct dos_port_instruction_result dos_port_instruction_decode(
	const struct dos_machine *machine,
	const struct dos_cpu_state *input_state) __must_check;

#endif

/* SPDX-License-Identifier: GPL-2.0-only */
/* Safe real-mode semantics for VM86-sensitive control instructions. */
#ifndef DOSC32_DOS_CONTROL_INSTRUCTION_H
#define DOSC32_DOS_CONTROL_INSTRUCTION_H

#include "compiler.h"
#include "dos_machine.h"
#include "types.h"

enum dos_control_instruction_status {
	DOS_CONTROL_INSTRUCTION_EMULATED = 0,
	/* HLT committed its architectural next IP and stopped execution. */
	DOS_CONTROL_INSTRUCTION_HALTED,
	DOS_CONTROL_INSTRUCTION_NOT_HANDLED,
	DOS_CONTROL_INSTRUCTION_INVALID_ARGUMENT,
	DOS_CONTROL_INSTRUCTION_MACHINE_FAULT,
	DOS_CONTROL_INSTRUCTION_ROLLBACK_FAILED
};

struct dos_control_instruction_result {
	struct dos_cpu_state state;
	uint32_t status;
	uint32_t machine_status;
} __aligned(8);

static_assert_expression(sizeof(struct dos_control_instruction_result) == 64u,
			 "control instruction result must stay fixed width");

struct dos_control_instruction_result dos_control_instruction_emulate(
	const struct dos_machine *machine, uint8_t opcode, bool operand32,
	const struct dos_cpu_state *input_state) __must_check;

#endif

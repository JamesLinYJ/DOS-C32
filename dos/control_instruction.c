// SPDX-License-Identifier: GPL-2.0-only
/*
 * Backend-neutral 16-bit semantics for the control instructions which trap in
 * VM86 when IOPL is below three.  The hardware backend remains responsible
 * for instruction decoding; this module owns stack transactions and logical
 * FLAGS only.
 */
#include "dos_control_instruction.h"

#define X86_OPCODE_PUSHF 0x9cu
#define X86_OPCODE_POPF 0x9du
#define X86_OPCODE_IRET 0xcfu
#define X86_OPCODE_HLT 0xf4u
#define X86_OPCODE_CLI 0xfau
#define X86_OPCODE_STI 0xfbu
#define DOS_CONTROL_STACK_WORD_BYTES 2u
#define DOS_CONTROL_STACK_DWORD_BYTES 4u
#define DOS_CONTROL_IRET_WORDS 3u
#define DOS_CONTROL_IRET_BYTES \
	(DOS_CONTROL_IRET_WORDS * DOS_CONTROL_STACK_WORD_BYTES)
#define DOS_EFLAGS_POP16_WRITABLE                                            \
	(DOS_EFLAGS_CF | DOS_EFLAGS_PF | DOS_EFLAGS_AF | DOS_EFLAGS_ZF |     \
	 DOS_EFLAGS_SF | DOS_EFLAGS_TF | DOS_EFLAGS_IF | DOS_EFLAGS_DF |     \
	 DOS_EFLAGS_OF | DOS_EFLAGS_IOPL | DOS_EFLAGS_NT)
#define DOS_EFLAGS_AC (1u << 18)
#define DOS_EFLAGS_ID (1u << 21)
#define DOS_EFLAGS_POP32_WRITABLE \
	(DOS_EFLAGS_POP16_WRITABLE | DOS_EFLAGS_AC | DOS_EFLAGS_ID)

static uint16_t read_le16(const uint8_t *bytes)
{
	return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8u);
}

static void write_le16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8u);
}

static uint32_t read_le32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8u) |
	       ((uint32_t)bytes[2] << 16u) | ((uint32_t)bytes[3] << 24u);
}

static void write_le32(uint8_t *bytes, uint32_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8u);
	bytes[2] = (uint8_t)(value >> 16u);
	bytes[3] = (uint8_t)(value >> 24u);
}

static struct dos_control_instruction_result control_result(
	enum dos_control_instruction_status status,
	enum dos_machine_status machine_status)
{
	struct dos_control_instruction_result result = {0};

	result.status = (uint32_t)status;
	result.machine_status = (uint32_t)machine_status;
	return result;
}

static bool state_has_real16_stack(const struct dos_cpu_state *state)
{
	return state != NULL &&
	       (state->mode == (uint32_t)DOS_CPU_REAL16 ||
		state->mode == (uint32_t)DOS_CPU_VM86) &&
	       (state->eip & 0xffff0000u) == 0u &&
	       (state->esp & 0xffff0000u) == 0u;
}

static uint32_t flags_after_pop16(uint32_t previous, uint16_t popped)
{
	uint32_t replacement =
		(previous & ~DOS_EFLAGS_POP16_WRITABLE) |
		((uint32_t)popped & DOS_EFLAGS_POP16_WRITABLE);

	return replacement | DOS_EFLAGS_RESERVED_ONE;
}

static void advance_ip(struct dos_cpu_state *state, bool operand32)
{
	state->eip = (uint16_t)(dos_register_low16(state->eip) +
				  (operand32 ? 2u : 1u));
}

static struct dos_control_instruction_result emulate_pushf(
	const struct dos_machine *machine, const struct dos_cpu_state *input,
	bool operand32)
{
	struct dos_control_instruction_result result;
	uint8_t replacement[DOS_CONTROL_STACK_DWORD_BYTES];
	uint8_t rollback[DOS_CONTROL_STACK_DWORD_BYTES];
	enum dos_machine_status machine_status;
	size_t stack_bytes = operand32 ? DOS_CONTROL_STACK_DWORD_BYTES
				       : DOS_CONTROL_STACK_WORD_BYTES;
	uint16_t stack_pointer =
		(uint16_t)(dos_register_low16(input->esp) -
			   stack_bytes);

	if (operand32)
		write_le32(replacement,
			   (input->eflags & ~DOS_EFLAGS_VM) |
				   DOS_EFLAGS_RESERVED_ONE);
	else
		write_le16(replacement, (uint16_t)(
			dos_register_low16(input->eflags) |
			DOS_EFLAGS_RESERVED_ONE));
	machine_status = dos_machine_replace_far(
		machine, input->ss, stack_pointer, replacement,
		stack_bytes, rollback, sizeof(rollback), stack_bytes);
	if (machine_status == DOS_MACHINE_ROLLBACK_FAILED)
		return control_result(DOS_CONTROL_INSTRUCTION_ROLLBACK_FAILED,
				      machine_status);
	if (machine_status != DOS_MACHINE_OK)
		return control_result(DOS_CONTROL_INSTRUCTION_MACHINE_FAULT,
				      machine_status);
	result = control_result(DOS_CONTROL_INSTRUCTION_EMULATED,
				DOS_MACHINE_OK);
	result.state = *input;
	result.state.esp = stack_pointer;
	advance_ip(&result.state, operand32);
	return result;
}

static struct dos_control_instruction_result emulate_popf(
	const struct dos_machine *machine, const struct dos_cpu_state *input,
	bool operand32)
{
	struct dos_control_instruction_result result;
	uint8_t encoded[DOS_CONTROL_STACK_DWORD_BYTES];
	enum dos_machine_status machine_status;
	size_t stack_bytes = operand32 ? DOS_CONTROL_STACK_DWORD_BYTES
				       : DOS_CONTROL_STACK_WORD_BYTES;

	machine_status = dos_machine_read_far(
		machine, input->ss, dos_register_low16(input->esp), encoded,
		sizeof(encoded), stack_bytes);
	if (machine_status != DOS_MACHINE_OK)
		return control_result(DOS_CONTROL_INSTRUCTION_MACHINE_FAULT,
				      machine_status);
	result = control_result(DOS_CONTROL_INSTRUCTION_EMULATED,
				DOS_MACHINE_OK);
	result.state = *input;
	result.state.esp = (uint16_t)(dos_register_low16(input->esp) +
					     stack_bytes);
	if (operand32)
		result.state.eflags =
			(input->eflags & ~DOS_EFLAGS_POP32_WRITABLE) |
			(read_le32(encoded) & DOS_EFLAGS_POP32_WRITABLE) |
			DOS_EFLAGS_RESERVED_ONE;
	else
		result.state.eflags =
			flags_after_pop16(input->eflags, read_le16(encoded));
	advance_ip(&result.state, operand32);
	return result;
}

static struct dos_control_instruction_result emulate_iret(
	const struct dos_machine *machine, const struct dos_cpu_state *input)
{
	struct dos_control_instruction_result result;
	uint8_t encoded[DOS_CONTROL_IRET_BYTES];
	enum dos_machine_status machine_status;

	machine_status = dos_machine_read_far(
		machine, input->ss, dos_register_low16(input->esp), encoded,
		sizeof(encoded), sizeof(encoded));
	if (machine_status != DOS_MACHINE_OK)
		return control_result(DOS_CONTROL_INSTRUCTION_MACHINE_FAULT,
				      machine_status);
	result = control_result(DOS_CONTROL_INSTRUCTION_EMULATED,
				DOS_MACHINE_OK);
	result.state = *input;
	result.state.eip = read_le16(encoded);
	result.state.cs = read_le16(encoded + DOS_CONTROL_STACK_WORD_BYTES);
	result.state.eflags = flags_after_pop16(
		input->eflags,
		read_le16(encoded + 2u * DOS_CONTROL_STACK_WORD_BYTES));
	result.state.esp = (uint16_t)(dos_register_low16(input->esp) +
					     DOS_CONTROL_IRET_BYTES);
	return result;
}

struct dos_control_instruction_result dos_control_instruction_emulate(
	const struct dos_machine *machine, uint8_t opcode, bool operand32,
	const struct dos_cpu_state *input_state)
{
	struct dos_control_instruction_result result;

	if (machine == NULL || !state_has_real16_stack(input_state))
		return control_result(DOS_CONTROL_INSTRUCTION_INVALID_ARGUMENT,
				      DOS_MACHINE_INVALID_ARGUMENT);
	switch (opcode) {
	case X86_OPCODE_PUSHF:
		return emulate_pushf(machine, input_state, operand32);
	case X86_OPCODE_POPF:
		return emulate_popf(machine, input_state, operand32);
	case X86_OPCODE_IRET:
		if (operand32)
			return control_result(
				DOS_CONTROL_INSTRUCTION_NOT_HANDLED,
				DOS_MACHINE_OK);
		return emulate_iret(machine, input_state);
	case X86_OPCODE_HLT:
		result = control_result(DOS_CONTROL_INSTRUCTION_HALTED,
					DOS_MACHINE_OK);
		result.state = *input_state;
		advance_ip(&result.state, operand32);
		return result;
	case X86_OPCODE_CLI:
	case X86_OPCODE_STI:
		result = control_result(DOS_CONTROL_INSTRUCTION_EMULATED,
					DOS_MACHINE_OK);
		result.state = *input_state;
		if (opcode == X86_OPCODE_CLI)
			result.state.eflags &= ~DOS_EFLAGS_IF;
		else
			result.state.eflags |= DOS_EFLAGS_IF;
		advance_ip(&result.state, operand32);
		return result;
	default:
		return control_result(DOS_CONTROL_INSTRUCTION_NOT_HANDLED,
				      DOS_MACHINE_OK);
	}
}

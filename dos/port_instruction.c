// SPDX-License-Identifier: GPL-2.0-only
/* Decode scalar 386 IN/OUT forms without granting any hardware permission. */
#include "dos_port_instruction.h"

#define X86_PREFIX_OPERAND_SIZE 0x66u
#define X86_OPCODE_IN_AL_IMMEDIATE 0xe4u
#define X86_OPCODE_IN_WORD_IMMEDIATE 0xe5u
#define X86_OPCODE_OUT_IMMEDIATE_AL 0xe6u
#define X86_OPCODE_OUT_IMMEDIATE_WORD 0xe7u
#define X86_OPCODE_IN_AL_DX 0xecu
#define X86_OPCODE_IN_WORD_DX 0xedu
#define X86_OPCODE_OUT_DX_AL 0xeeu
#define X86_OPCODE_OUT_DX_WORD 0xefu
#define X86_MAXIMUM_INSTRUCTION_BYTES 15u

static struct dos_port_instruction_result port_result(
	enum dos_port_instruction_status status,
	enum dos_machine_status machine_status)
{
	struct dos_port_instruction_result result = {0};

	result.status = (uint32_t)status;
	result.machine_status = (uint32_t)machine_status;
	return result;
}

static bool state_is_real16(const struct dos_cpu_state *state)
{
	return state != NULL &&
	       (state->mode == (uint32_t)DOS_CPU_REAL16 ||
		state->mode == (uint32_t)DOS_CPU_VM86) &&
	       (state->eip & 0xffff0000u) == 0u;
}

static enum dos_machine_status fetch_byte(const struct dos_machine *machine,
					  const struct dos_cpu_state *state,
					  uint8_t displacement,
					  uint8_t *value)
{
	uint16_t offset =
		(uint16_t)(dos_register_low16(state->eip) + displacement);

	return dos_machine_read_far(machine, state->cs, offset, value,
				    sizeof(*value), sizeof(*value));
}

static bool opcode_uses_immediate_port(uint8_t opcode)
{
	return opcode == X86_OPCODE_IN_AL_IMMEDIATE ||
	       opcode == X86_OPCODE_IN_WORD_IMMEDIATE ||
	       opcode == X86_OPCODE_OUT_IMMEDIATE_AL ||
	       opcode == X86_OPCODE_OUT_IMMEDIATE_WORD;
}

static bool opcode_is_scalar_port_io(uint8_t opcode)
{
	return opcode_uses_immediate_port(opcode) ||
	       opcode == X86_OPCODE_IN_AL_DX ||
	       opcode == X86_OPCODE_IN_WORD_DX ||
	       opcode == X86_OPCODE_OUT_DX_AL ||
	       opcode == X86_OPCODE_OUT_DX_WORD;
}

static bool opcode_is_write(uint8_t opcode)
{
	return opcode == X86_OPCODE_OUT_IMMEDIATE_AL ||
	       opcode == X86_OPCODE_OUT_IMMEDIATE_WORD ||
	       opcode == X86_OPCODE_OUT_DX_AL ||
	       opcode == X86_OPCODE_OUT_DX_WORD;
}

static bool opcode_is_byte_width(uint8_t opcode)
{
	return opcode == X86_OPCODE_IN_AL_IMMEDIATE ||
	       opcode == X86_OPCODE_OUT_IMMEDIATE_AL ||
	       opcode == X86_OPCODE_IN_AL_DX ||
	       opcode == X86_OPCODE_OUT_DX_AL;
}

struct dos_port_instruction_result dos_port_instruction_decode(
	const struct dos_machine *machine, const struct dos_cpu_state *input_state)
{
	struct dos_port_instruction_result result;
	enum dos_machine_status machine_status;
	uint8_t instruction_length = 0u;
	uint8_t opcode;
	uint8_t immediate_port = 0u;
	bool operand32 = false;

	if (machine == NULL || !state_is_real16(input_state))
		return port_result(DOS_PORT_INSTRUCTION_INVALID_ARGUMENT,
				   DOS_MACHINE_INVALID_ARGUMENT);
	machine_status =
		fetch_byte(machine, input_state, instruction_length, &opcode);
	if (machine_status != DOS_MACHINE_OK)
		return port_result(DOS_PORT_INSTRUCTION_FETCH_FAULT,
				   machine_status);
	++instruction_length;
	while (opcode == X86_PREFIX_OPERAND_SIZE &&
	       instruction_length < X86_MAXIMUM_INSTRUCTION_BYTES) {
		operand32 = true;
		machine_status = fetch_byte(machine, input_state,
					    instruction_length, &opcode);
		if (machine_status != DOS_MACHINE_OK)
			return port_result(DOS_PORT_INSTRUCTION_FETCH_FAULT,
					   machine_status);
		++instruction_length;
	}
	if (!opcode_is_scalar_port_io(opcode))
		return port_result(DOS_PORT_INSTRUCTION_NOT_HANDLED,
				   DOS_MACHINE_OK);
	if (opcode_uses_immediate_port(opcode)) {
		if (instruction_length >= X86_MAXIMUM_INSTRUCTION_BYTES)
			return port_result(DOS_PORT_INSTRUCTION_NOT_HANDLED,
					   DOS_MACHINE_OK);
		machine_status = fetch_byte(machine, input_state,
					    instruction_length,
					    &immediate_port);
		if (machine_status != DOS_MACHINE_OK)
			return port_result(DOS_PORT_INSTRUCTION_FETCH_FAULT,
					   machine_status);
		++instruction_length;
	}

	result = port_result(DOS_PORT_INSTRUCTION_DECODED, DOS_MACHINE_OK);
	result.state = *input_state;
	result.state.eip = (uint16_t)(dos_register_low16(input_state->eip) +
					      instruction_length);
	result.event.kind = (uint32_t)DOS_EXEC_EVENT_PORT_IO;
	result.event.port = opcode_uses_immediate_port(opcode)
				    ? (uint16_t)immediate_port
				    : dos_register_low16(input_state->edx);
	result.event.io_width = opcode_is_byte_width(opcode)
					? (uint8_t)DOS_IO_WIDTH_8
					: (operand32
						   ? (uint8_t)DOS_IO_WIDTH_32
						   : (uint8_t)DOS_IO_WIDTH_16);
	result.event.io_write = opcode_is_write(opcode) ? 1u : 0u;
	if (result.event.io_write != 0u) {
		if (result.event.io_width == (uint8_t)DOS_IO_WIDTH_8)
			result.event.value = dos_register_low8(input_state->eax);
		else if (result.event.io_width ==
			 (uint8_t)DOS_IO_WIDTH_16)
			result.event.value = dos_register_low16(input_state->eax);
		else
			result.event.value = input_state->eax;
	}
	return result;
}

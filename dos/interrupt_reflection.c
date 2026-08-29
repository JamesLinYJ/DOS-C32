// SPDX-License-Identifier: GPL-2.0-only
/*
 * Real-mode interrupt reflection for VM86 and interpreter backends.
 *
 * The backend has already advanced IP past INT imm8.  Reflection therefore
 * reads the four-byte IVT target and transactionally creates the architectural
 * six-byte IRET frame at SS:(SP-6).  No vector receives special treatment.
 */
#include "dos_interrupt_reflection.h"

#include "dos_vectors.h"

#define DOS_INTERRUPT_WORD_BYTES 2u

static uint16_t read_le16(const uint8_t *bytes)
{
	return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8u);
}

static void write_le16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8u);
}

static bool bytes_are_zero(const uint8_t *bytes, size_t count)
{
	size_t index;

	for (index = 0u; index < count; ++index) {
		if (bytes[index] != 0u)
			return false;
	}
	return true;
}

static struct dos_interrupt_reflection_result reflection_failure(
	enum dos_interrupt_reflection_status status,
	enum dos_machine_status machine_status)
{
	struct dos_interrupt_reflection_result result = {0};

	result.status = (uint32_t)status;
	result.machine_status = (uint32_t)machine_status;
	return result;
}

static bool state_can_receive_real_interrupt(const struct dos_cpu_state *state)
{
	return state != NULL &&
	       (state->mode == (uint32_t)DOS_CPU_REAL16 ||
		state->mode == (uint32_t)DOS_CPU_VM86) &&
	       (state->eip & 0xffff0000u) == 0u &&
	       (state->esp & 0xffff0000u) == 0u;
}

struct dos_interrupt_reflection_result dos_interrupt_reflect(
	const struct dos_machine *machine, uint8_t vector,
	const struct dos_cpu_state *post_interrupt_state)
{
	struct dos_interrupt_reflection_result result;
	uint8_t vector_bytes[DOS_INTERRUPT_VECTOR_BYTES];
	uint8_t frame[DOS_INTERRUPT_REFLECTION_FRAME_BYTES];
	uint8_t rollback[DOS_INTERRUPT_REFLECTION_FRAME_BYTES];
	enum dos_machine_status machine_status;
	size_t index;
	uint16_t new_stack_pointer;
	dos_linear_address_t vector_address;

	if (machine == NULL || !state_can_receive_real_interrupt(
					 post_interrupt_state))
		return reflection_failure(
			DOS_INTERRUPT_REFLECTION_INVALID_ARGUMENT,
			DOS_MACHINE_INVALID_ARGUMENT);

	vector_address =
		(dos_linear_address_t)vector * DOS_INTERRUPT_VECTOR_BYTES;
	machine_status = dos_machine_read(
		machine, vector_address, vector_bytes, sizeof(vector_bytes),
		sizeof(vector_bytes));
	if (machine_status != DOS_MACHINE_OK)
		return reflection_failure(DOS_INTERRUPT_REFLECTION_VECTOR_FAULT,
					  machine_status);

	new_stack_pointer = (uint16_t)(
		dos_register_low16(post_interrupt_state->esp) -
		DOS_INTERRUPT_REFLECTION_FRAME_BYTES);
	/* Final SP addresses IP, CS and FLAGS in IRET pop order. */
	write_le16(frame, dos_register_low16(post_interrupt_state->eip));
	write_le16(frame + DOS_INTERRUPT_WORD_BYTES,
		   post_interrupt_state->cs);
	write_le16(frame + 2u * DOS_INTERRUPT_WORD_BYTES,
		   dos_register_low16(post_interrupt_state->eflags));

	machine_status = dos_machine_replace_far(
		machine, post_interrupt_state->ss, new_stack_pointer, frame,
		sizeof(frame), rollback, sizeof(rollback), sizeof(frame));
	if (machine_status == DOS_MACHINE_ROLLBACK_FAILED)
		return reflection_failure(
			DOS_INTERRUPT_REFLECTION_ROLLBACK_FAILED, machine_status);
	if (machine_status != DOS_MACHINE_OK)
		return reflection_failure(DOS_INTERRUPT_REFLECTION_STACK_FAULT,
					  machine_status);

	result = (struct dos_interrupt_reflection_result){
		.state = *post_interrupt_state,
		.receipt = {
			.stack_segment = post_interrupt_state->ss,
			.stack_offset = new_stack_pointer,
			.valid = 1u,
			.reserved = {0u},
		},
		.status = (uint32_t)DOS_INTERRUPT_REFLECTION_OK,
		.machine_status = (uint32_t)DOS_MACHINE_OK,
	};
	for (index = 0u; index < ARRAY_SIZE(result.receipt.original); ++index)
		result.receipt.original[index] = rollback[index];
	result.state.esp = new_stack_pointer;
	result.state.eip = read_le16(vector_bytes);
	result.state.cs = read_le16(vector_bytes + DOS_INTERRUPT_WORD_BYTES);
	result.state.eflags &= ~(DOS_EFLAGS_TF | DOS_EFLAGS_IF);
	return result;
}

enum dos_machine_status dos_interrupt_reflection_rollback(
	const struct dos_machine *machine,
	const struct dos_interrupt_reflection_receipt *receipt)
{
	uint8_t scratch[DOS_INTERRUPT_REFLECTION_FRAME_BYTES];

	if (machine == NULL || receipt == NULL || receipt->valid != 1u ||
	    !bytes_are_zero(receipt->reserved, ARRAY_SIZE(receipt->reserved)))
		return DOS_MACHINE_INVALID_ARGUMENT;
	return dos_machine_replace_far(
		machine, receipt->stack_segment, receipt->stack_offset,
		receipt->original, sizeof(receipt->original), scratch,
		sizeof(scratch), sizeof(receipt->original));
}

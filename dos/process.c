// SPDX-License-Identifier: GPL-2.0-only
/*
 * DOS PSP and initial-process-state construction.
 *
 * Process-building follows MS-DOS-visible behavior. Added validation is
 * confined to malformed guest ranges, canonical upper-layer command
 * construction, and load plans that could otherwise wrap or partially
 * overwrite. Saved vectors retain their defined PSP order.
 */
#include "dos_process.h"

#include "dosc32_assert.h"
#include "overflow.h"
#include "string.h"

#define PSP_EXIT_INSTRUCTION_OFFSET 0x00u
#define PSP_BLOCK_END_OFFSET 0x02u
#define PSP_CPM_CALL_OFFSET 0x05u
#define PSP_EXIT_VECTOR_OFFSET 0x0au
#define PSP_CONTROL_C_VECTOR_OFFSET 0x0eu
#define PSP_CRITICAL_ERROR_VECTOR_OFFSET 0x12u
#define PSP_PARENT_OFFSET 0x16u
#define PSP_JFT_OFFSET 0x18u
#define PSP_ENVIRONMENT_OFFSET 0x2cu
#define PSP_JFT_LENGTH_OFFSET 0x32u
#define PSP_JFT_POINTER_OFFSET 0x34u
#define PSP_NEXT_PSP_OFFSET 0x38u
#define PSP_CALL_SYSTEM_OFFSET 0x50u

#define DOS_OPCODE_INT 0xcdu
#define DOS_OPCODE_FAR_CALL 0x9au
#define DOS_OPCODE_FAR_RETURN 0xcbu
#define DOS_ABORT_INTERRUPT 0x20u
#define DOS_COMMAND_INTERRUPT 0x21u
#define DOS_PSP_CPM_ENTRY_SEGMENT 0x000cu
#define DOS_PSP_CPM_MAX_PARAGRAPHS 0x0fffu
#define DOS_PSP_PARAGRAPHS 0x10u
#define DOS_PARAGRAPH_SHIFT 4u
#define DOS_PARAGRAPH_BYTES (1u << DOS_PARAGRAPH_SHIFT)
#define DOS_COM_LOAD_OFFSET 0x0100u
#define DOS_COM_ADDRESS_SPACE_BYTES 0x10000u
#define DOS_COM_MAXIMUM_IMAGE_BYTES 0xfeffu
#define DOS_INITIAL_EFLAGS 0x00000202u

static void bytes_clear(uint8_t *destination, size_t destination_capacity,
			size_t count)
{
	DOSC32_ASSERT(memset_s(destination, destination_capacity, 0, count) ==
		      MEMORY_OK);
}

static void bytes_copy(uint8_t *destination, size_t destination_capacity,
		       const uint8_t *source, size_t source_capacity,
		       size_t count)
{
	DOSC32_ASSERT(memcpy_s(destination, destination_capacity, source,
			       source_capacity, count) == MEMORY_OK);
}

static uint16_t read_le16(const uint8_t *source)
{
	return (uint16_t)source[0] | ((uint16_t)source[1] << 8);
}

static void write_le16(uint8_t *destination, uint16_t value)
{
	destination[0] = (uint8_t)value;
	destination[1] = (uint8_t)(value >> 8);
}

static void write_le32(uint8_t *destination, uint32_t value)
{
	destination[0] = (uint8_t)value;
	destination[1] = (uint8_t)(value >> 8);
	destination[2] = (uint8_t)(value >> 16);
	destination[3] = (uint8_t)(value >> 24);
}

static void write_far_address(uint8_t *destination,
			      struct dos_process_far_address address)
{
	write_le16(destination, address.offset);
	write_le16(destination + 2u, address.segment);
}

static enum dos_process_status stage_setmem_fields(
	struct dos_process_psp_image *staging, uint16_t block_end_segment,
	uint16_t parent_psp_segment, uint16_t environment_segment,
	struct dos_process_far_address terminate_vector,
	struct dos_process_far_address control_c_vector,
	struct dos_process_far_address critical_error_vector,
	const struct dos_sft_jft20 *jft)
{
	uint16_t block_paragraphs;
	uint16_t cpm_paragraphs;
	uint16_t cpm_offset;
	uint16_t cpm_segment;
	size_t index;

	if (staging == NULL || jft == NULL ||
	    block_end_segment <= staging->segment)
		return DOS_PROCESS_INVALID_ARGUMENT;
	block_paragraphs =
		(uint16_t)(block_end_segment - staging->segment);
	if (block_paragraphs < DOS_PSP_PARAGRAPHS)
		return DOS_PROCESS_INVALID_PSP;

	/* Process-state setup uses explicit little-endian byte writes. */
	write_le16(staging->bytes + PSP_EXIT_INSTRUCTION_OFFSET,
		   (uint16_t)((DOS_ABORT_INTERRUPT << 8) | DOS_OPCODE_INT));
	write_le16(staging->bytes + PSP_BLOCK_END_OFFSET, block_end_segment);
	cpm_paragraphs = block_paragraphs < DOS_PSP_CPM_MAX_PARAGRAPHS
			     ? block_paragraphs
			     : DOS_PSP_CPM_MAX_PARAGRAPHS;
	cpm_paragraphs = (uint16_t)(cpm_paragraphs - DOS_PSP_PARAGRAPHS);
	cpm_offset = (uint16_t)(cpm_paragraphs << DOS_PARAGRAPH_SHIFT);
	/* Intentional 16-bit wrap forms the far-call alias to linear C0h. */
	cpm_segment = (uint16_t)(DOS_PSP_CPM_ENTRY_SEGMENT - cpm_paragraphs);
	staging->bytes[PSP_CPM_CALL_OFFSET] = DOS_OPCODE_FAR_CALL;
	write_le16(staging->bytes + PSP_CPM_CALL_OFFSET + 1u, cpm_offset);
	write_le16(staging->bytes + PSP_CPM_CALL_OFFSET + 3u, cpm_segment);
	write_far_address(staging->bytes + PSP_EXIT_VECTOR_OFFSET,
			  terminate_vector);
	write_far_address(staging->bytes + PSP_CONTROL_C_VECTOR_OFFSET,
			  control_c_vector);
	write_far_address(staging->bytes + PSP_CRITICAL_ERROR_VECTOR_OFFSET,
			  critical_error_vector);
	write_le16(staging->bytes + PSP_PARENT_OFFSET, parent_psp_segment);
	for (index = 0u; index < DOS_PSP_DEFAULT_HANDLES; ++index)
		staging->bytes[PSP_JFT_OFFSET + index] = jft->entries[index];
	write_le16(staging->bytes + PSP_ENVIRONMENT_OFFSET,
		   environment_segment);
	write_le16(staging->bytes + PSP_JFT_LENGTH_OFFSET,
		   DOS_PSP_DEFAULT_HANDLES);
	write_le16(staging->bytes + PSP_JFT_POINTER_OFFSET, PSP_JFT_OFFSET);
	write_le16(staging->bytes + PSP_JFT_POINTER_OFFSET + 2u,
		   staging->segment);
	write_le32(staging->bytes + PSP_NEXT_PSP_OFFSET, 0xffffffffu);
	staging->bytes[PSP_CALL_SYSTEM_OFFSET] = DOS_OPCODE_INT;
	staging->bytes[PSP_CALL_SYSTEM_OFFSET + 1u] = DOS_COMMAND_INTERRUPT;
	staging->bytes[PSP_CALL_SYSTEM_OFFSET + 2u] = DOS_OPCODE_FAR_RETURN;
	return DOS_PROCESS_OK;
}

enum dos_process_status dos_process_prepare_initial_psp(
	const struct dos_process_initial_psp_request *request,
	struct dos_process_psp_image *image)
{
	struct dos_process_psp_image staging;
	struct dos_sft_jft20 initial_jft;
	enum dos_process_status status;
	size_t index;

	if (request == NULL || image == NULL)
		return DOS_PROCESS_INVALID_ARGUMENT;
	staging.segment = request->psp_segment;
	bytes_clear(staging.bytes, sizeof(staging.bytes), sizeof(staging.bytes));
	for (index = 0u; index < ARRAY_SIZE(initial_jft.entries); ++index)
		initial_jft.entries[index] = DOS_JFT_ENTRY_UNUSED;
	/* Standard input, output and error share SFT number zero. */
	initial_jft.entries[0] = 0u;
	initial_jft.entries[1] = 0u;
	initial_jft.entries[2] = 0u;
	status = stage_setmem_fields(
		&staging, request->block_end_segment, request->psp_segment,
		request->environment_segment,
		request->terminate_vector, request->control_c_vector,
		request->critical_error_vector, &initial_jft);
	if (status != DOS_PROCESS_OK)
		return status;
	staging.bytes[DOS_PSP_COMMAND_TAIL_OFFSET] = 0u;
	staging.bytes[DOS_PSP_COMMAND_TAIL_OFFSET + 1u] =
		DOS_PROCESS_COMMAND_CR;
	*image = staging;
	return DOS_PROCESS_OK;
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

/*
 * Preflight every physical chunk of a 16-bit far transfer before the first
 * backend callback.  dos_machine owns both offset and A20 wrapping; DOS
 * process code must not silently reinterpret a segmented object as one flat
 * native range.
 */
static enum dos_process_status
preflight_far_range(const struct dos_machine *machine,
		    struct dos_process_far_address address, uint32_t count,
		    dos_linear_address_t *linear_address)
{
	enum dos_machine_status machine_status;

	if (machine == NULL || machine->ops == NULL ||
	    machine->ops->read_memory == NULL ||
	    machine->ops->write_memory == NULL || linear_address == NULL ||
	    machine->address_limit == 0u ||
	    machine->address_limit > DOS_GUEST_32_ADDRESS_LIMIT)
		return DOS_PROCESS_INVALID_ARGUMENT;
	machine_status = dos_machine_validate_far(
	    machine, address.segment, address.offset, (size_t)count);
	if (machine_status == DOS_MACHINE_ADDRESS_FAULT)
		return DOS_PROCESS_MACHINE_FAULT;
	if (machine_status != DOS_MACHINE_OK)
		return DOS_PROCESS_INVALID_ARGUMENT;
	*linear_address = dos_far_to_linear(address.segment, address.offset,
					    machine->a20_enabled);
	return DOS_PROCESS_OK;
}

static enum dos_process_status
encode_raw_command_tail(const uint8_t *source, size_t source_length,
			struct dos_command_tail40 *tail)
{
	struct dos_command_tail40 staging;
	size_t index;

	if (source_length > DOS_COMMAND_TAIL_BYTES - 1u)
		return DOS_PROCESS_COMMAND_TAIL_TOO_LONG;
	bytes_clear((uint8_t *)(void *)&staging, sizeof(staging),
		    sizeof(staging));
	for (index = 0u; index < source_length; ++index) {
		if (source[index] == DOS_PROCESS_COMMAND_CR)
			return DOS_PROCESS_BAD_COMMAND_TAIL;
		staging.data[index] = source[index];
	}
	staging.length = (uint8_t)source_length;
	staging.data[source_length] = DOS_PROCESS_COMMAND_CR;
	bytes_copy((uint8_t *)(void *)tail, sizeof(*tail),
		   (const uint8_t *)(const void *)&staging, sizeof(staging),
		   sizeof(staging));
	return DOS_PROCESS_OK;
}

enum dos_process_status
dos_process_encode_command_tail(const uint8_t *source, size_t source_capacity,
				size_t source_length,
				struct dos_command_tail40 *tail)
{
	if (tail == NULL || source_length > source_capacity ||
	    (source_length != 0u && source == NULL))
		return DOS_PROCESS_INVALID_ARGUMENT;
	return encode_raw_command_tail(source, source_length, tail);
}

static enum dos_process_status
validate_psp_request(const struct dos_machine *machine,
		     const struct dos_process_psp_request *request,
		     dos_linear_address_t *child_linear,
		     dos_linear_address_t *command_linear,
		     dos_linear_address_t *first_fcb_linear,
		     dos_linear_address_t *second_fcb_linear)
{
	struct dos_process_far_address child;
	enum dos_process_status status;
	uint16_t block_paragraphs;

	if (request == NULL || child_linear == NULL || command_linear == NULL ||
	    first_fcb_linear == NULL || second_fcb_linear == NULL)
		return DOS_PROCESS_INVALID_ARGUMENT;
	if (request->block_end_segment <= request->psp_segment)
		return DOS_PROCESS_INVALID_PSP;
	block_paragraphs =
	    (uint16_t)(request->block_end_segment - request->psp_segment);
	if (block_paragraphs < DOS_PSP_PARAGRAPHS)
		return DOS_PROCESS_INVALID_PSP;

	child.segment = request->psp_segment;
	child.offset = 0u;
	status =
	    preflight_far_range(machine, child, DOS_PSP_SIZE, child_linear);
	if (status != DOS_PROCESS_OK)
		return status;
	status =
	    preflight_far_range(machine, request->first_fcb_source,
				DOS_PROCESS_FCB_PREFIX_BYTES, first_fcb_linear);
	if (status != DOS_PROCESS_OK)
		return status;
	status = preflight_far_range(machine, request->second_fcb_source,
				     DOS_PROCESS_FCB_PREFIX_BYTES,
				     second_fcb_linear);
	if (status != DOS_PROCESS_OK)
		return status;
	return preflight_far_range(machine, request->command_tail_source,
				   sizeof(struct dos_command_tail40),
				   command_linear);
}

static bool parent_snapshot_has_valid_encoding(
    const struct dos_process_parent_snapshot *snapshot)
{
	return snapshot != NULL &&
	       snapshot->machine_identity != KERNEL_OBJECT_HANDLE_INVALID &&
	       snapshot->machine_address_limit != 0u &&
	       snapshot->machine_address_limit <= DOS_GUEST_32_ADDRESS_LIMIT &&
	       snapshot->a20_enabled <= 1u && snapshot->captured == 1u &&
	       bytes_are_zero(snapshot->reserved, sizeof(snapshot->reserved)) &&
	       bytes_are_zero(snapshot->reserved_tail,
			      sizeof(snapshot->reserved_tail));
}

static enum dos_process_status validate_parent_snapshot_binding(
    const struct dos_machine *machine, kernel_object_handle_t machine_identity,
    const struct dos_process_parent_snapshot *snapshot,
    uint16_t expected_parent_psp)
{
	if (machine == NULL ||
	    machine_identity == KERNEL_OBJECT_HANDLE_INVALID ||
	    snapshot == NULL)
		return DOS_PROCESS_INVALID_ARGUMENT;
	if (!parent_snapshot_has_valid_encoding(snapshot) ||
	    snapshot->machine_identity != machine_identity ||
	    snapshot->machine_context != machine->context ||
	    snapshot->machine_address_limit != machine->address_limit ||
	    snapshot->a20_enabled != (machine->a20_enabled ? 1u : 0u) ||
	    snapshot->parent_psp_segment != expected_parent_psp)
		return DOS_PROCESS_STALE_SNAPSHOT;
	return DOS_PROCESS_OK;
}

enum dos_process_status dos_process_capture_parent_snapshot(
    const struct dos_machine *machine, kernel_object_handle_t machine_identity,
    uint16_t parent_psp_segment, struct dos_process_parent_snapshot *snapshot)
{
	struct dos_process_parent_snapshot staging = {0};
	struct dos_process_far_address parent_address = {
	    .segment = parent_psp_segment,
	    .offset = 0u,
	};
	struct dos_process_far_address parent_jft_address;
	dos_linear_address_t parent_linear;
	dos_linear_address_t parent_jft_linear;
	enum dos_process_status status;
	uint16_t parent_jft_length;
	size_t handles_to_read;
	size_t index;

	if (snapshot == NULL ||
	    machine_identity == KERNEL_OBJECT_HANDLE_INVALID)
		return DOS_PROCESS_INVALID_ARGUMENT;
	status = preflight_far_range(machine, parent_address, DOS_PSP_SIZE,
				     &parent_linear);
	if (status != DOS_PROCESS_OK)
		return status;
	(void)parent_linear;

	staging.machine_identity = machine_identity;
	staging.machine_context = machine->context;
	staging.machine_address_limit = machine->address_limit;
	staging.parent_psp_segment = parent_psp_segment;
	staging.a20_enabled = machine->a20_enabled ? 1u : 0u;
	staging.captured = 1u;
	for (index = 0u; index < DOS_PSP_DEFAULT_HANDLES; ++index)
		staging.parent_jft.entries[index] = DOS_JFT_ENTRY_UNUSED;

	/* Child creation first copies the complete parent PSP. */
	if (dos_machine_read_far(machine, parent_psp_segment, 0u,
				 staging.parent_psp, sizeof(staging.parent_psp),
				 sizeof(staging.parent_psp)) != DOS_MACHINE_OK)
		return DOS_PROCESS_MACHINE_FAULT;

	parent_jft_length =
	    read_le16(staging.parent_psp + PSP_JFT_LENGTH_OFFSET);
	handles_to_read = parent_jft_length < DOS_PSP_DEFAULT_HANDLES
			      ? (size_t)parent_jft_length
			      : (size_t)DOS_PSP_DEFAULT_HANDLES;
	if (handles_to_read != 0u) {
		parent_jft_address.offset =
		    read_le16(staging.parent_psp + PSP_JFT_POINTER_OFFSET);
		parent_jft_address.segment =
		    read_le16(staging.parent_psp + PSP_JFT_POINTER_OFFSET + 2u);
		status = preflight_far_range(machine, parent_jft_address,
					     (uint32_t)handles_to_read,
					     &parent_jft_linear);
		if (status != DOS_PROCESS_OK)
			return status;
		if (dos_machine_read_far(machine, parent_jft_address.segment,
					 parent_jft_address.offset,
					 staging.parent_jft.entries,
					 sizeof(staging.parent_jft.entries),
					 handles_to_read) != DOS_MACHINE_OK)
			return DOS_PROCESS_MACHINE_FAULT;
		(void)parent_jft_linear;
	}

	bytes_copy((uint8_t *)(void *)snapshot, sizeof(*snapshot),
		   (const uint8_t *)(const void *)&staging, sizeof(staging),
		   sizeof(staging));
	return DOS_PROCESS_OK;
}

static bool staged_psp_alias_offset(const struct dos_machine *machine,
				    const struct dos_process_psp_image *staging,
				    struct dos_process_far_address source,
				    size_t source_index, size_t *psp_offset)
{
	uint16_t source_offset =
	    (uint16_t)(source.offset + (uint16_t)source_index);
	dos_linear_address_t source_linear = dos_far_to_linear(
	    source.segment, source_offset, machine->a20_enabled);
	size_t index;

	for (index = 0u; index < DOS_PSP_SIZE; ++index) {
		dos_linear_address_t child_linear = dos_far_to_linear(
		    staging->segment, (uint16_t)index, machine->a20_enabled);

		if (child_linear == source_linear) {
			*psp_offset = index;
			return true;
		}
	}
	return false;
}

/*
 * Child creation copies the full parent PSP. If a forged CurrentPDB makes parent
 * and child overlap physically, a later copied word observes earlier child
 * words.  Captured parent bytes provide the initial view; staging provides bytes
 * written by preceding iterations.
 */
static void
stage_parent_psp_copy(const struct dos_machine *machine,
		      const struct dos_process_parent_snapshot *parent_snapshot,
		      struct dos_process_psp_image *staging)
{
	struct dos_process_far_address source = {
	    .segment = parent_snapshot->parent_psp_segment,
	    .offset = 0u,
	};
	size_t word_offset;

	for (word_offset = 0u; word_offset < DOS_PSP_SIZE; word_offset += 2u) {
		uint8_t source_word[2];
		size_t byte_index;

		for (byte_index = 0u; byte_index < 2u; ++byte_index) {
			size_t alias_offset;
			size_t source_index = word_offset + byte_index;

			if (staged_psp_alias_offset(machine, staging, source,
						    source_index,
						    &alias_offset) &&
			    alias_offset < word_offset)
				source_word[byte_index] =
				    staging->bytes[alias_offset];
			else
				source_word[byte_index] =
				    parent_snapshot->parent_psp[source_index];
		}
		staging->bytes[word_offset] = source_word[0];
		staging->bytes[word_offset + 1u] = source_word[1];
	}
}

static bool
source_aliases_staged_psp(const struct dos_machine *machine,
			  const struct dos_process_psp_image *staging,
			  struct dos_process_far_address source, size_t count)
{
	size_t index;

	for (index = 0u; index < count; ++index) {
		size_t alias_offset;

		if (staged_psp_alias_offset(machine, staging, source, index,
					    &alias_offset))
			return true;
	}
	return false;
}

/*
 * Emulate forward REP MOVSB into the private PSP image.  A bulk read is
 * safe when the source cannot name the staged child.  On alias, each byte is
 * resolved after the preceding destination byte has become visible, which
 * preserves forward-copy self-overlap without exposing partial guest state.
 */
static enum dos_process_status
stage_rep_movsb_to_psp(const struct dos_machine *machine,
		       struct dos_process_far_address source,
		       struct dos_process_psp_image *staging,
		       size_t destination_offset, size_t count)
{
	uint8_t source_bytes[DOS_PSP_SIZE];
	size_t index;

	if (destination_offset > DOS_PSP_SIZE ||
	    count > DOS_PSP_SIZE - destination_offset)
		return DOS_PROCESS_INVALID_ARGUMENT;
	if (!source_aliases_staged_psp(machine, staging, source, count)) {
		if (dos_machine_read_far(machine, source.segment, source.offset,
					 source_bytes, sizeof(source_bytes),
					 count) != DOS_MACHINE_OK)
			return DOS_PROCESS_MACHINE_FAULT;
		bytes_copy(staging->bytes + destination_offset,
			   sizeof(staging->bytes) - destination_offset,
			   source_bytes, sizeof(source_bytes), count);
		return DOS_PROCESS_OK;
	}

	for (index = 0u; index < count; ++index) {
		size_t alias_offset;
		uint8_t value;
		uint16_t source_offset =
		    (uint16_t)(source.offset + (uint16_t)index);

		if (staged_psp_alias_offset(machine, staging, source, index,
					    &alias_offset)) {
			value = staging->bytes[alias_offset];
		} else if (dos_machine_read_far(machine, source.segment,
						source_offset, &value,
						sizeof(value), sizeof(value)) !=
			   DOS_MACHINE_OK) {
			return DOS_PROCESS_MACHINE_FAULT;
		}
		staging->bytes[destination_offset + index] = value;
	}
	return DOS_PROCESS_OK;
}

enum dos_process_status dos_process_prepare_psp_from_snapshot(
    const struct dos_machine *machine, kernel_object_handle_t machine_identity,
    const struct dos_process_parent_snapshot *parent_snapshot,
    const struct dos_process_psp_request *request,
    const struct dos_sft_jft20 *child_jft, struct dos_process_psp_image *image)
{
	struct dos_process_psp_image staging;
	dos_linear_address_t child_linear;
	dos_linear_address_t command_linear;
	dos_linear_address_t first_fcb_linear;
	dos_linear_address_t second_fcb_linear;
	enum dos_process_status status;

	if (request == NULL || child_jft == NULL || image == NULL)
		return DOS_PROCESS_INVALID_ARGUMENT;
	status = validate_parent_snapshot_binding(machine, machine_identity,
						  parent_snapshot,
						  request->parent_psp_segment);
	if (status != DOS_PROCESS_OK)
		return status;
	status = validate_psp_request(machine, request, &child_linear,
				      &command_linear, &first_fcb_linear,
				      &second_fcb_linear);
	if (status != DOS_PROCESS_OK)
		return status;
	(void)child_linear;
	(void)command_linear;
	(void)first_fcb_linear;
	(void)second_fcb_linear;

	/* Child creation begins with a full 256-byte parent PSP copy. */
	staging.segment = request->psp_segment;
	stage_parent_psp_copy(machine, parent_snapshot, &staging);

	status = stage_setmem_fields(
		&staging, request->block_end_segment,
		request->parent_psp_segment, request->environment_segment,
		request->terminate_vector, request->control_c_vector,
		request->critical_error_vector, child_jft);
	if (status != DOS_PROCESS_OK)
		return status;

	/*
	 * EXEC copies and mutates each range before reading the next one.
	 * The staged overlay preserves that order when a caller buffer aliases
	 * the new PSP, including through A20 physical wrapping.
	 */
	status = stage_rep_movsb_to_psp(machine, request->first_fcb_source,
					&staging, DOS_PSP_FIRST_FCB_OFFSET,
					DOS_PROCESS_FCB_PREFIX_BYTES);
	if (status != DOS_PROCESS_OK)
		return status;
	bytes_clear(staging.bytes + DOS_PSP_FIRST_FCB_OFFSET +
			DOS_PROCESS_FCB_PREFIX_BYTES,
		    sizeof(staging.bytes) - DOS_PSP_FIRST_FCB_OFFSET -
			DOS_PROCESS_FCB_PREFIX_BYTES,
		    4u);
	status = stage_rep_movsb_to_psp(machine, request->second_fcb_source,
					&staging, DOS_PSP_SECOND_FCB_OFFSET,
					DOS_PROCESS_FCB_PREFIX_BYTES);
	if (status != DOS_PROCESS_OK)
		return status;
	bytes_clear(staging.bytes + DOS_PSP_SECOND_FCB_OFFSET +
			DOS_PROCESS_FCB_PREFIX_BYTES,
		    sizeof(staging.bytes) - DOS_PSP_SECOND_FCB_OFFSET -
			DOS_PROCESS_FCB_PREFIX_BYTES,
		    4u);
	/*
	 * EXEC copies exactly 128 bytes from the caller. COMMAND
	 * normally constructs count/data/CR, but INT 21h/4Bh does not normalize
	 * or clear the bytes after CR; preserving them is guest-visible ABI.
	 */
	status = stage_rep_movsb_to_psp(machine, request->command_tail_source,
					&staging, DOS_PSP_COMMAND_TAIL_OFFSET,
					sizeof(struct dos_command_tail40));
	if (status != DOS_PROCESS_OK)
		return status;

	/* Publish only after every guest read and transformation succeeded. */
	image->segment = staging.segment;
	bytes_copy(image->bytes, sizeof(image->bytes), staging.bytes,
		   sizeof(staging.bytes), sizeof(image->bytes));
	return DOS_PROCESS_OK;
}

enum dos_process_status
dos_process_prepare_psp(const struct dos_machine *machine,
			const struct dos_process_psp_request *request,
			struct dos_process_psp_image *image)
{
	const kernel_object_handle_t wrapper_identity = 0u;
	struct dos_process_parent_snapshot parent_snapshot;
	struct dos_sft_jft20 child_jft;
	dos_linear_address_t child_linear;
	dos_linear_address_t command_linear;
	dos_linear_address_t first_fcb_linear;
	dos_linear_address_t second_fcb_linear;
	enum dos_process_status status;
	size_t index;

	if (request == NULL || image == NULL)
		return DOS_PROCESS_INVALID_ARGUMENT;
	if ((request->inheritable_handle_mask &
	     ~DOS_PROCESS_INHERITABLE_HANDLE_MASK) != 0u)
		return DOS_PROCESS_INVALID_ARGUMENT;
	/* Preserve the old wrapper's no-callback validation boundary. */
	status = validate_psp_request(machine, request, &child_linear,
				      &command_linear, &first_fcb_linear,
				      &second_fcb_linear);
	if (status != DOS_PROCESS_OK)
		return status;
	(void)child_linear;
	(void)command_linear;
	(void)first_fcb_linear;
	(void)second_fcb_linear;

	status = dos_process_capture_parent_snapshot(
	    machine, wrapper_identity, request->parent_psp_segment,
	    &parent_snapshot);
	if (status != DOS_PROCESS_OK)
		return status;
	for (index = 0u; index < DOS_PSP_DEFAULT_HANDLES; ++index) {
		uint32_t bit = (uint32_t)1u << index;

		child_jft.entries[index] =
		    (request->inheritable_handle_mask & bit) != 0u
			? parent_snapshot.parent_jft.entries[index]
			: DOS_JFT_ENTRY_UNUSED;
	}
	return dos_process_prepare_psp_from_snapshot(machine, wrapper_identity,
						     &parent_snapshot, request,
						     &child_jft, image);
}

enum dos_process_status
dos_process_psp_set_jft20(struct dos_process_psp_image *image,
			  const struct dos_sft_jft20 *child_jft)
{
	size_t index;

	if (image == NULL || child_jft == NULL)
		return DOS_PROCESS_INVALID_ARGUMENT;
	if (read_le16(image->bytes + PSP_JFT_LENGTH_OFFSET) !=
		DOS_PSP_DEFAULT_HANDLES ||
	    read_le16(image->bytes + PSP_JFT_POINTER_OFFSET) !=
		PSP_JFT_OFFSET ||
	    read_le16(image->bytes + PSP_JFT_POINTER_OFFSET + 2u) !=
		image->segment)
		return DOS_PROCESS_INVALID_PSP;
	for (index = 0u; index < DOS_PSP_DEFAULT_HANDLES; ++index)
		image->bytes[PSP_JFT_OFFSET + index] =
		    child_jft->entries[index];
	return DOS_PROCESS_OK;
}

enum dos_process_status
dos_process_commit_psp(const struct dos_machine *machine,
		       const struct dos_process_psp_image *image)
{
	uint8_t rollback[DOS_PSP_SIZE];
	struct dos_process_far_address destination;
	dos_linear_address_t linear;
	enum dos_machine_status machine_status;
	enum dos_process_status status;

	if (image == NULL)
		return DOS_PROCESS_INVALID_ARGUMENT;
	destination.segment = image->segment;
	destination.offset = 0u;
	status =
	    preflight_far_range(machine, destination, DOS_PSP_SIZE, &linear);
	if (status != DOS_PROCESS_OK)
		return status;
	machine_status = dos_machine_replace(
	    machine, linear, image->bytes, sizeof(image->bytes), rollback,
	    sizeof(rollback), sizeof(image->bytes));
	if (machine_status == DOS_MACHINE_ROLLBACK_FAILED)
		return DOS_PROCESS_MACHINE_POISONED;
	if (machine_status != DOS_MACHINE_OK)
		return DOS_PROCESS_MACHINE_FAULT;
	return DOS_PROCESS_OK;
}

enum dos_process_status
dos_process_build_psp(const struct dos_machine *machine,
		      const struct dos_process_psp_request *request)
{
	struct dos_process_psp_image image;
	enum dos_process_status status;

	status = dos_process_prepare_psp(machine, request, &image);
	if (status != DOS_PROCESS_OK)
		return status;
	return dos_process_commit_psp(machine, &image);
}

static enum dos_process_status
validate_plan_geometry(uint16_t psp_segment, uint16_t block_paragraphs,
		       uint16_t *block_end_segment,
		       uint64_t *block_begin_linear, uint64_t *block_end_linear)
{
	uint64_t block_end;

	if (block_end_segment == NULL || block_begin_linear == NULL ||
	    block_end_linear == NULL || block_paragraphs == 0u)
		return DOS_PROCESS_INVALID_ARGUMENT;
	if (block_paragraphs < DOS_PSP_PARAGRAPHS)
		return DOS_PROCESS_INVALID_PSP;
	if (check_add_overflow((uint64_t)psp_segment,
			       (uint64_t)block_paragraphs, &block_end) ||
	    block_end > 0xffffu)
		return DOS_PROCESS_RANGE_OVERFLOW;
	if (check_mul_overflow((uint64_t)psp_segment, (uint64_t)16u,
			       block_begin_linear) ||
	    check_mul_overflow(block_end, (uint64_t)16u, block_end_linear))
		return DOS_PROCESS_RANGE_OVERFLOW;
	*block_end_segment = (uint16_t)block_end;
	return DOS_PROCESS_OK;
}

static bool launch_mode_is_valid(enum dos_process_launch_mode launch_mode)
{
	return launch_mode == DOS_PROCESS_LAUNCH_EXECUTE ||
	       launch_mode == DOS_PROCESS_LAUNCH_LOAD_ONLY;
}

static bool
allocation_plans_equal(const struct dos_process_allocation_plan *left,
		       const struct dos_process_allocation_plan *right)
{
	return left->format == right->format &&
	       left->available_paragraphs == right->available_paragraphs &&
	       left->block_paragraphs == right->block_paragraphs &&
	       left->load_high == right->load_high &&
	       left->reserved == right->reserved;
}

static void initialize_cpu_state(struct dos_cpu_state *state,
				 uint16_t initial_ax, uint16_t psp_segment,
				 uint16_t cs, uint16_t ip, uint16_t ss,
				 uint16_t sp)
{
	*state = (struct dos_cpu_state){0};
	/* EXEC leaves these low words visible across the handoff.
	 */
	state->eax = initial_ax;
	state->ebx = initial_ax;
	state->edx = psp_segment;
	state->esi = ip;
	state->edi = sp;
	state->esp = sp;
	state->eip = ip;
	state->eflags = DOS_INITIAL_EFLAGS;
	state->cs = cs;
	state->ss = ss;
	state->ds = psp_segment;
	state->es = psp_segment;
	state->mode = (uint32_t)DOS_CPU_REAL16;
}

static enum dos_process_status
validate_com_image_plan(const struct dos_load_plan *image_plan)
{
	uint64_t expected_paragraphs;

	if (!dos_load_plan_has_valid_encoding(image_plan) ||
	    image_plan->target_kind != (uint8_t)DOS_LOAD_TARGET_PROCESS ||
	    image_plan->image_file_offset != 0u ||
	    image_plan->file_size != image_plan->image_size ||
	    image_plan->image_size == 0u ||
	    image_plan->image_size > DOS_COM_MAXIMUM_IMAGE_BYTES ||
	    image_plan->initial_ip != DOS_COM_LOAD_OFFSET ||
	    image_plan->relocation_count != 0u)
		return DOS_PROCESS_BAD_IMAGE_RANGE;
	expected_paragraphs =
	    (image_plan->image_size + DOS_PARAGRAPH_BYTES - 1u) >>
	    DOS_PARAGRAPH_SHIFT;
	if (image_plan->minimum_image_paragraphs != expected_paragraphs)
		return DOS_PROCESS_BAD_IMAGE_RANGE;
	return DOS_PROCESS_OK;
}

enum dos_process_status dos_process_plan_com(
    const struct dos_load_plan *image_plan,
    const struct dos_process_allocation_plan *allocation_plan,
    uint16_t psp_segment, enum dos_process_launch_mode launch_mode,
    uint16_t initial_ax, struct dos_com_process_plan *process_plan)
{
	struct dos_process_allocation_plan expected_allocation;
	struct dos_com_process_plan staging;
	uint16_t block_end_segment;
	uint16_t block_paragraphs;
	uint64_t block_begin_linear;
	uint64_t block_end_linear;
	uint64_t block_bytes;
	uint64_t load_linear;
	uint64_t read_capacity;
	uint64_t stack_pointer;
	enum dos_process_status status;

	if (image_plan == NULL || allocation_plan == NULL ||
	    process_plan == NULL || !launch_mode_is_valid(launch_mode))
		return DOS_PROCESS_INVALID_ARGUMENT;
	if (!dos_process_allocation_plan_has_valid_encoding(allocation_plan))
		return DOS_PROCESS_INVALID_ARGUMENT;
	if (image_plan->format != DOS_IMAGE_COM)
		return DOS_PROCESS_WRONG_IMAGE_FORMAT;
	status = validate_com_image_plan(image_plan);
	if (status != DOS_PROCESS_OK)
		return status;
	status = dos_process_select_allocation(
	    image_plan, allocation_plan->available_paragraphs,
	    &expected_allocation);
	if (status != DOS_PROCESS_OK)
		return status;
	if (!allocation_plans_equal(allocation_plan, &expected_allocation))
		return DOS_PROCESS_INVALID_ARGUMENT;
	block_paragraphs = allocation_plan->block_paragraphs;
	status = validate_plan_geometry(psp_segment, block_paragraphs,
					&block_end_segment, &block_begin_linear,
					&block_end_linear);
	if (status != DOS_PROCESS_OK)
		return status;
	(void)block_end_linear;
	if (check_mul_overflow((uint64_t)block_paragraphs, (uint64_t)16u,
			       &block_bytes))
		return DOS_PROCESS_RANGE_OVERFLOW;
	if (block_bytes <= DOS_COM_LOAD_OFFSET)
		return DOS_PROCESS_NOT_ENOUGH_MEMORY;
	read_capacity = block_bytes >= DOS_COM_ADDRESS_SPACE_BYTES
			    ? DOS_COM_ADDRESS_SPACE_BYTES - DOS_COM_LOAD_OFFSET
			    : block_bytes - DOS_COM_LOAD_OFFSET;
	/* EXEC requires the read to finish short of the requested capacity. */
	if (image_plan->image_size >= read_capacity)
		return DOS_PROCESS_NOT_ENOUGH_MEMORY;
	if (image_plan->image_size > 0xffffffffull ||
	    read_capacity > 0xffffffffull ||
	    check_add_overflow(block_begin_linear,
			       (uint64_t)DOS_COM_LOAD_OFFSET, &load_linear) ||
	    load_linear > 0xffffffffull)
		return DOS_PROCESS_RANGE_OVERFLOW;
	/* SP derives from the requested read size, not file length. */
	if (check_add_overflow(read_capacity, (uint64_t)0xfeu,
			       &stack_pointer) ||
	    stack_pointer > 0xffffu || stack_pointer < 2u)
		return DOS_PROCESS_BAD_IMAGE_RANGE;

	staging = (struct dos_com_process_plan){0};
	staging.psp_segment = psp_segment;
	staging.block_end_segment = block_end_segment;
	staging.load_segment = (uint16_t)(psp_segment + DOS_PSP_PARAGRAPHS);
	staging.load_offset = 0u;
	staging.load_linear_address = (dos_linear_address_t)load_linear;
	staging.image_size = (uint32_t)image_plan->image_size;
	staging.read_capacity = (uint32_t)read_capacity;
	staging.stack_sentinel_offset = (uint16_t)stack_pointer;
	staging.stack_sentinel_value = 0u;
	staging.load_only_stack_pointer = (uint16_t)(stack_pointer - 2u);
	staging.load_only_stack_value = initial_ax;
	staging.launch_mode = (uint8_t)launch_mode;
	initialize_cpu_state(&staging.initial_state, initial_ax, psp_segment,
			     psp_segment, DOS_COM_LOAD_OFFSET, psp_segment,
			     (uint16_t)stack_pointer);
	*process_plan = staging;
	return DOS_PROCESS_OK;
}

static bool linear_word_is_in_block(uint64_t block_begin, uint64_t block_end,
				    uint16_t segment, uint16_t offset)
{
	uint64_t linear = ((uint64_t)segment << DOS_PARAGRAPH_SHIFT) + offset;

	/* A word beginning at ffffH crosses the real-mode segment limit. */
	return offset <= 0xfffeu && linear >= block_begin &&
	       linear < block_end && block_end - linear >= 2u;
}

/* Validate the exact wrapped value object produced by dos_loader. */
static enum dos_process_status
validate_mz_image_plan(const struct dos_load_plan *image_plan,
		       uint64_t *resident_paragraphs)
{
	if (resident_paragraphs == NULL ||
	    !dos_load_plan_has_valid_encoding(image_plan) ||
	    image_plan->target_kind != (uint8_t)DOS_LOAD_TARGET_PROCESS)
		return DOS_PROCESS_INVALID_ARGUMENT;
	if (!dos_load_plan_has_wrapped_mz_geometry(image_plan))
		return DOS_PROCESS_BAD_IMAGE_RANGE;
	/*
	 * EXEC leaves the wrapped AX result. Keep it as the
	 * allocation input; checked wide arithmetic below is only containment.
	 */
	*resident_paragraphs = image_plan->minimum_image_paragraphs;
	return DOS_PROCESS_OK;
}

enum dos_process_status dos_process_select_allocation(
    const struct dos_load_plan *image_plan, uint16_t available_paragraphs,
    struct dos_process_allocation_plan *allocation_plan)
{
	struct dos_process_allocation_plan staging;
	uint64_t resident_paragraphs;
	uint64_t bare_paragraphs;
	uint64_t minimum_paragraphs;
	uint64_t desired_paragraphs;
	uint64_t block_bytes;
	uint64_t read_capacity;
	enum dos_process_status status;

	if (allocation_plan == NULL ||
	    !dos_load_plan_has_valid_encoding(image_plan) ||
	    image_plan->target_kind != (uint8_t)DOS_LOAD_TARGET_PROCESS)
		return DOS_PROCESS_INVALID_ARGUMENT;
	if (available_paragraphs == 0u)
		return DOS_PROCESS_NOT_ENOUGH_MEMORY;

	staging = (struct dos_process_allocation_plan){0};
	staging.format = image_plan->format;
	staging.available_paragraphs = available_paragraphs;
	if (image_plan->format == DOS_IMAGE_COM) {
		status = validate_com_image_plan(image_plan);
		if (status != DOS_PROCESS_OK)
			return status;
		block_bytes = (uint64_t)available_paragraphs
			      << DOS_PARAGRAPH_SHIFT;
		if (block_bytes <= DOS_COM_LOAD_OFFSET)
			return DOS_PROCESS_NOT_ENOUGH_MEMORY;
		read_capacity =
		    block_bytes >= DOS_COM_ADDRESS_SPACE_BYTES
			? DOS_COM_ADDRESS_SPACE_BYTES - DOS_COM_LOAD_OFFSET
			: block_bytes - DOS_COM_LOAD_OFFSET;
		if (image_plan->image_size >= read_capacity)
			return DOS_PROCESS_NOT_ENOUGH_MEMORY;
		/* EXEC always requests the largest available COM block. */
		staging.block_paragraphs = available_paragraphs;
		staging.load_high = 0u;
		*allocation_plan = staging;
		return DOS_PROCESS_OK;
	}
	if (image_plan->format != DOS_IMAGE_MZ)
		return DOS_PROCESS_WRONG_IMAGE_FORMAT;

	status = validate_mz_image_plan(image_plan, &resident_paragraphs);
	if (status != DOS_PROCESS_OK)
		return status;
	if (check_add_overflow(resident_paragraphs,
			       (uint64_t)DOS_PSP_PARAGRAPHS,
			       &bare_paragraphs) ||
	    bare_paragraphs > 0xffffu)
		return DOS_PROCESS_BAD_IMAGE_RANGE;
	/* EXEC insists on at least 11h paragraphs even for an empty image.
	 */
	if (available_paragraphs < DOS_PSP_PARAGRAPHS + 1u ||
	    bare_paragraphs > available_paragraphs)
		return DOS_PROCESS_NOT_ENOUGH_MEMORY;

	staging.load_high = image_plan->load_high;
	if (image_plan->load_high) {
		/* max_extra == 0 takes exec_BX_max before the min-BSS check. */
		staging.block_paragraphs = available_paragraphs;
		*allocation_plan = staging;
		return DOS_PROCESS_OK;
	}
	if (check_add_overflow(bare_paragraphs,
			       (uint64_t)image_plan->minimum_extra_paragraphs,
			       &minimum_paragraphs) ||
	    minimum_paragraphs > 0xffffu ||
	    minimum_paragraphs > available_paragraphs)
		return DOS_PROCESS_NOT_ENOUGH_MEMORY;
	/* EXEC falls back to the largest block if bare+max carries or is
	 * larger than the available block. */
	if (check_add_overflow(bare_paragraphs,
			       (uint64_t)image_plan->maximum_extra_paragraphs,
			       &desired_paragraphs) ||
	    desired_paragraphs > 0xffffu ||
	    desired_paragraphs > available_paragraphs)
		staging.block_paragraphs = available_paragraphs;
	else
		staging.block_paragraphs = (uint16_t)desired_paragraphs;
	*allocation_plan = staging;
	return DOS_PROCESS_OK;
}

enum dos_process_status dos_process_plan_mz(
    const struct dos_load_plan *image_plan,
    const struct dos_process_allocation_plan *allocation_plan,
    uint16_t psp_segment, enum dos_process_launch_mode launch_mode,
    uint16_t initial_ax, struct dos_mz_process_plan *process_plan)
{
	struct dos_process_allocation_plan expected_allocation;
	struct dos_mz_process_plan staging;
	uint16_t block_end_segment;
	uint16_t block_paragraphs;
	uint64_t block_begin_linear;
	uint64_t block_end_linear;
	uint64_t program_begin_linear;
	uint64_t resident_paragraphs;
	uint64_t resident_bytes;
	uint64_t load_segment64;
	uint64_t load_linear;
	uint64_t image_end;
	uint64_t resident_end;
	uint64_t entry_linear;
	enum dos_process_status status;
	uint16_t cs;
	uint16_t ss;
	uint16_t first_push_offset;
	uint16_t second_push_offset;

	if (image_plan == NULL || allocation_plan == NULL ||
	    process_plan == NULL || !launch_mode_is_valid(launch_mode))
		return DOS_PROCESS_INVALID_ARGUMENT;
	if (!dos_process_allocation_plan_has_valid_encoding(allocation_plan))
		return DOS_PROCESS_INVALID_ARGUMENT;
	if (image_plan->format != DOS_IMAGE_MZ)
		return DOS_PROCESS_WRONG_IMAGE_FORMAT;
	status = validate_mz_image_plan(image_plan, &resident_paragraphs);
	if (status != DOS_PROCESS_OK)
		return status;
	status = dos_process_select_allocation(
	    image_plan, allocation_plan->available_paragraphs,
	    &expected_allocation);
	if (status != DOS_PROCESS_OK)
		return status;
	if (!allocation_plans_equal(allocation_plan, &expected_allocation))
		return DOS_PROCESS_INVALID_ARGUMENT;
	block_paragraphs = allocation_plan->block_paragraphs;
	status = validate_plan_geometry(psp_segment, block_paragraphs,
					&block_end_segment, &block_begin_linear,
					&block_end_linear);
	if (status != DOS_PROCESS_OK)
		return status;
	if (check_add_overflow(block_begin_linear, (uint64_t)DOS_PSP_SIZE,
			       &program_begin_linear) ||
	    check_mul_overflow(resident_paragraphs,
			       (uint64_t)DOS_PARAGRAPH_BYTES, &resident_bytes))
		return DOS_PROCESS_RANGE_OVERFLOW;

	if (image_plan->load_high) {
		if (resident_paragraphs > block_end_segment)
			return DOS_PROCESS_BAD_IMAGE_RANGE;
		load_segment64 =
		    (uint64_t)block_end_segment - resident_paragraphs;
	} else {
		if (check_add_overflow((uint64_t)psp_segment,
				       (uint64_t)DOS_PSP_PARAGRAPHS,
				       &load_segment64))
			return DOS_PROCESS_RANGE_OVERFLOW;
	}
	if (load_segment64 > 0xffffu ||
	    check_mul_overflow(load_segment64, (uint64_t)16u, &load_linear) ||
	    check_add_overflow(load_linear, image_plan->image_size,
			       &image_end) ||
	    check_add_overflow(load_linear, resident_bytes, &resident_end) ||
	    load_linear < program_begin_linear || image_end > resident_end ||
	    resident_end > block_end_linear)
		return DOS_PROCESS_BAD_IMAGE_RANGE;
	/*
	 * EXEC performs both relocations with a 16-bit ADD.
	 * Preserve that modulo-65536 register behavior here; the independent
	 * entry and stack range checks below decide whether the wrapped guest
	 * addresses are safe for this unpublished allocation.
	 */
	cs = (uint16_t)(load_segment64 + (uint64_t)image_plan->initial_cs);
	ss = (uint16_t)(load_segment64 + (uint64_t)image_plan->initial_ss);
	/* EXEC0 performs PUSH/PUSH/RETF; EXEC1 writes only at SS:SP-2. */
	first_push_offset = (uint16_t)(image_plan->initial_sp - 2u);
	second_push_offset = (uint16_t)(image_plan->initial_sp - 4u);
	if (!linear_word_is_in_block(program_begin_linear, block_end_linear, ss,
				     first_push_offset))
		return DOS_PROCESS_BAD_IMAGE_RANGE;
	if (launch_mode == DOS_PROCESS_LAUNCH_EXECUTE &&
	    (check_add_overflow((uint64_t)cs << DOS_PARAGRAPH_SHIFT,
				(uint64_t)image_plan->initial_ip,
				&entry_linear) ||
	     entry_linear < program_begin_linear ||
	     entry_linear >= block_end_linear ||
	     !linear_word_is_in_block(program_begin_linear, block_end_linear,
				      ss, second_push_offset)))
		return DOS_PROCESS_BAD_IMAGE_RANGE;

	staging = (struct dos_mz_process_plan){0};
	staging.psp_segment = psp_segment;
	staging.block_end_segment = block_end_segment;
	staging.load_segment = (uint16_t)load_segment64;
	staging.load_offset = 0u;
	staging.load_linear_address = (dos_linear_address_t)load_linear;
	staging.image_file_offset = image_plan->image_file_offset;
	staging.image_size = (uint32_t)image_plan->image_size;
	staging.resident_paragraphs = (uint32_t)resident_paragraphs;
	staging.relocation_factor = (uint16_t)load_segment64;
	staging.relocation_count = image_plan->relocation_count;
	staging.relocation_table_offset = image_plan->relocation_table_offset;
	staging.load_only_stack_pointer = first_push_offset;
	staging.load_only_stack_value = initial_ax;
	staging.load_high = image_plan->load_high;
	staging.launch_mode = (uint8_t)launch_mode;
	initialize_cpu_state(&staging.initial_state, initial_ax, psp_segment,
			     cs, image_plan->initial_ip, ss,
			     image_plan->initial_sp);
	*process_plan = staging;
	return DOS_PROCESS_OK;
}

static bool initial_ax_fields_are_consistent(const struct dos_cpu_state *state,
					     uint16_t stack_value)
{
	return state->eax == (uint32_t)stack_value &&
	       state->ebx == (uint32_t)stack_value;
}

enum dos_process_status
dos_process_finalize_com_initial_ax(struct dos_com_process_plan *process_plan,
				    uint16_t initial_ax)
{
	if (!dos_com_process_plan_has_valid_encoding(process_plan) ||
	    !initial_ax_fields_are_consistent(
		&process_plan->initial_state,
		process_plan->load_only_stack_value))
		return DOS_PROCESS_INVALID_ARGUMENT;
	process_plan->load_only_stack_value = initial_ax;
	process_plan->initial_state.eax = initial_ax;
	process_plan->initial_state.ebx = initial_ax;
	return DOS_PROCESS_OK;
}

enum dos_process_status
dos_process_finalize_mz_initial_ax(struct dos_mz_process_plan *process_plan,
				   uint16_t initial_ax)
{
	if (!dos_mz_process_plan_has_valid_encoding(process_plan) ||
	    !initial_ax_fields_are_consistent(
		&process_plan->initial_state,
		process_plan->load_only_stack_value))
		return DOS_PROCESS_INVALID_ARGUMENT;
	process_plan->load_only_stack_value = initial_ax;
	process_plan->initial_state.eax = initial_ax;
	process_plan->initial_state.ebx = initial_ax;
	return DOS_PROCESS_OK;
}

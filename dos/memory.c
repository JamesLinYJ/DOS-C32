// SPDX-License-Identifier: GPL-2.0-only
/*
 * DOS MCB arena manager
 *
 * Compatibility contract: first/best/last fit, forward coalescing, paragraph segments,
 * M/Z signatures, PSP ownership, and resize maximum in BX.
 * Safety changes: bounded walks, checked segment arithmetic, transactional MCB
 * writes, reverse-order split unwind, and sticky rollback poison.
 */
#include "dos_memory.h"

#define MCB_BYTES DOS_MEMORY_MCB_BYTES
#define MCB_SIGNATURE_NORMAL 0x4du
#define MCB_SIGNATURE_END 0x5au
#define MCB_OWNER_FREE 0u
#define MCB_OWNER_NAME_OFFSET 8u
#define MCB_MAXIMUM_WALK 65536u

struct mcb_record {
	uint8_t bytes[MCB_BYTES];
};

struct allocation_candidates {
	bool found;
	uint16_t first;
	uint16_t best;
	uint16_t best_size;
	uint16_t last;
	uint16_t maximum;
};

static bool arena_identity_is_valid(kernel_object_handle_t identity)
{
	return identity != 0u && identity != KERNEL_OBJECT_HANDLE_INVALID;
}

static bool arena_flags_have_valid_encoding(
    const struct dos_memory_arena *arena)
{
	return arena != NULL && arena->initialized <= 1u &&
	       arena->machine_poisoned <= 1u && arena->constructed == 1u &&
	       arena->reserved[0] == 0u && arena->reserved[1] == 0u;
}

static uint16_t read_le16(const uint8_t *bytes)
{
	return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static void write_le16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
}

static uint8_t mcb_signature(const struct mcb_record *record)
{
	return record->bytes[0];
}

static uint16_t mcb_owner(const struct mcb_record *record)
{
	return read_le16(record->bytes + 1u);
}

static uint16_t mcb_size(const struct mcb_record *record)
{
	return read_le16(record->bytes + 3u);
}

static void mcb_set_signature(struct mcb_record *record, uint8_t signature)
{
	record->bytes[0] = signature;
}

static void mcb_set_owner(struct mcb_record *record, uint16_t owner)
{
	write_le16(record->bytes + 1u, owner);
}

static void mcb_set_size(struct mcb_record *record, uint16_t size)
{
	write_le16(record->bytes + 3u, size);
}

static void mcb_patch_owner_name(struct mcb_record *record, const uint8_t *name,
				 uint8_t count)
{
	size_t index;

	for (index = 0u; index < (size_t)count; ++index)
		record->bytes[MCB_OWNER_NAME_OFFSET + index] = name[index];
}

static void mcb_set_owner_name(struct mcb_record *record,
			       const uint8_t name[DOS_MEMORY_OWNER_NAME_BYTES])
{
	mcb_patch_owner_name(record, name, DOS_MEMORY_OWNER_NAME_BYTES);
}

static bool
owner_name_patch_is_valid(const struct dos_memory_owner_name_patch *name_patch)
{
	size_t index;

	if (name_patch == NULL || name_patch->count == 0u ||
	    name_patch->count > DOS_MEMORY_OWNER_NAME_BYTES)
		return false;
	for (index = 0u; index < ARRAY_SIZE(name_patch->reserved); ++index) {
		if (name_patch->reserved[index] != 0u)
			return false;
	}
	return true;
}

static bool mcb_signature_is_valid(const struct mcb_record *record)
{
	return mcb_signature(record) == MCB_SIGNATURE_NORMAL ||
	       mcb_signature(record) == MCB_SIGNATURE_END;
}

static bool mcb_extent_is_valid(uint16_t segment,
				const struct mcb_record *record)
{
	return (uint32_t)mcb_size(record) <= 0xffffu - (uint32_t)segment;
}

static enum dos_memory_status
arena_is_ready(const struct dos_memory_arena *arena,
	       const struct dos_machine *machine)
{
	if (!arena_flags_have_valid_encoding(arena) || machine == NULL)
		return DOS_MEMORY_INVALID_ARGUMENT;
	if (!arena_identity_is_valid(arena->identity) ||
	    arena->generation == 0u)
		return DOS_MEMORY_INVALID_ARGUMENT;
	if (arena->machine_poisoned == 1u)
		return DOS_MEMORY_MACHINE_POISONED;
	if (arena->initialized != 1u)
		return DOS_MEMORY_INVALID_ARGUMENT;
	return DOS_MEMORY_OK;
}

enum dos_memory_status
dos_memory_arena_construct(struct dos_memory_arena *arena,
			   kernel_object_handle_t identity)
{
	if (arena == NULL || !arena_identity_is_valid(identity))
		return DOS_MEMORY_INVALID_ARGUMENT;
	*arena =
	    (struct dos_memory_arena)DOS_MEMORY_ARENA_INITIALIZER(identity);
	return DOS_MEMORY_OK;
}

static enum dos_memory_status
read_record_bytes(const struct dos_machine *machine, uint16_t segment,
		  struct mcb_record *record)
{
	enum dos_machine_status status = dos_machine_read_far(
	    machine, segment, 0u, record, sizeof(*record), sizeof(*record));

	return status == DOS_MACHINE_OK ? DOS_MEMORY_OK
					: DOS_MEMORY_MACHINE_FAULT;
}

static enum dos_memory_status read_mcb(const struct dos_machine *machine,
				       uint16_t segment,
				       struct mcb_record *record)
{
	enum dos_memory_status status =
	    read_record_bytes(machine, segment, record);

	if (status != DOS_MEMORY_OK)
		return status;
	return mcb_signature_is_valid(record) &&
		       mcb_extent_is_valid(segment, record)
		   ? DOS_MEMORY_OK
		   : DOS_MEMORY_ARENA_DAMAGED;
}

/* Ordinary partial writes are repaired by dos_machine_replace. */
static enum dos_memory_status
replace_record_bytes(struct dos_memory_arena *arena,
		     const struct dos_machine *machine, uint16_t segment,
		     const struct mcb_record *record)
{
	struct mcb_record rollback;
	enum dos_machine_status status = dos_machine_replace(
	    machine, dos_far_to_linear(segment, 0u, machine->a20_enabled),
	    record, sizeof(*record), &rollback, sizeof(rollback),
	    sizeof(*record));

	if (status == DOS_MACHINE_OK)
		return DOS_MEMORY_OK;
	if (status == DOS_MACHINE_ROLLBACK_FAILED) {
		arena->machine_poisoned = 1u;
		return DOS_MEMORY_MACHINE_POISONED;
	}
	return DOS_MEMORY_MACHINE_FAULT;
}

static enum dos_memory_status next_mcb_segment(uint16_t segment,
					       const struct mcb_record *record,
					       uint16_t *next)
{
	uint32_t value = (uint32_t)segment + (uint32_t)mcb_size(record) + 1u;

	if (value > 0xffffu || value <= segment)
		return DOS_MEMORY_ARENA_DAMAGED;
	*next = (uint16_t)value;
	return DOS_MEMORY_OK;
}

/* Create the hidden header, publish the old header, then unwind in reverse. */
static enum dos_memory_status
commit_split(struct dos_memory_arena *arena, const struct dos_machine *machine,
	     uint16_t current_segment, const struct mcb_record *current_after,
	     uint16_t split_segment, const struct mcb_record *split_before,
	     const struct mcb_record *split_after)
{
	enum dos_memory_status publish_status;
	enum dos_memory_status unwind_status;

	publish_status =
	    replace_record_bytes(arena, machine, split_segment, split_after);
	if (publish_status != DOS_MEMORY_OK)
		return publish_status;
	publish_status = replace_record_bytes(arena, machine, current_segment,
					      current_after);
	if (publish_status == DOS_MEMORY_OK)
		return DOS_MEMORY_OK;

	unwind_status =
	    replace_record_bytes(arena, machine, split_segment, split_before);
	if (publish_status == DOS_MEMORY_MACHINE_POISONED ||
	    unwind_status != DOS_MEMORY_OK) {
		arena->machine_poisoned = 1u;
		return DOS_MEMORY_MACHINE_POISONED;
	}
	return publish_status;
}

static enum dos_memory_status
coalesce_forward(struct dos_memory_arena *arena,
		 const struct dos_machine *machine, uint16_t segment,
		 struct mcb_record *record)
{
	uint32_t steps = 0u;

	while (mcb_signature(record) != MCB_SIGNATURE_END) {
		struct mcb_record following;
		uint16_t following_segment;
		uint32_t combined_size;
		enum dos_memory_status status;

		if (++steps > MCB_MAXIMUM_WALK)
			return DOS_MEMORY_ARENA_DAMAGED;
		status = next_mcb_segment(segment, record, &following_segment);
		if (status != DOS_MEMORY_OK)
			return status;
		status = read_mcb(machine, following_segment, &following);
		if (status != DOS_MEMORY_OK)
			return status;
		if (mcb_owner(&following) != MCB_OWNER_FREE)
			break;
		combined_size = (uint32_t)mcb_size(record) +
				(uint32_t)mcb_size(&following) + 1u;
		if (combined_size > 0xffffu)
			return DOS_MEMORY_ARENA_DAMAGED;
		mcb_set_size(record, (uint16_t)combined_size);
		mcb_set_signature(record, mcb_signature(&following));
		status = replace_record_bytes(arena, machine, segment, record);
		if (status != DOS_MEMORY_OK)
			return status;
	}
	return DOS_MEMORY_OK;
}

enum dos_memory_status
dos_memory_arena_initialize_checked(struct dos_memory_arena *arena,
				    const struct dos_machine *machine,
				    uint16_t head_segment, uint16_t end_segment)
{
	struct mcb_record record = {{0u}};
	uint32_t paragraphs;
	uint64_t generation;
	enum dos_memory_status status;

	if (!arena_flags_have_valid_encoding(arena) || machine == NULL ||
	    !arena_identity_is_valid(arena->identity) ||
	    end_segment <= head_segment)
		return DOS_MEMORY_INVALID_ARGUMENT;
	if (arena->machine_poisoned == 1u)
		return DOS_MEMORY_MACHINE_POISONED;
	if (arena->generation == DOS_MEMORY_GENERATION_MAX)
		return DOS_MEMORY_GENERATION_EXHAUSTED;
	paragraphs = (uint32_t)end_segment - (uint32_t)head_segment - 1u;
	if (paragraphs > 0xffffu)
		return DOS_MEMORY_INVALID_ARGUMENT;
	mcb_set_signature(&record, MCB_SIGNATURE_END);
	mcb_set_owner(&record, MCB_OWNER_FREE);
	mcb_set_size(&record, (uint16_t)paragraphs);
	status = replace_record_bytes(arena, machine, head_segment, &record);
	if (status != DOS_MEMORY_OK)
		return status;
	generation = arena->generation + 1u;
	arena->generation = generation;
	arena->head_segment = head_segment;
	arena->strategy = DOS_ALLOC_FIRST_FIT;
	arena->initialized = 1u;
	arena->machine_poisoned = 0u;
	arena->constructed = 1u;
	return DOS_MEMORY_OK;
}

enum dos_memory_status
dos_memory_arena_validate_checked(const struct dos_memory_arena *arena,
				  const struct dos_machine *machine)
{
	uint16_t segment;
	uint32_t steps;
	enum dos_memory_status status = arena_is_ready(arena, machine);

	if (status != DOS_MEMORY_OK)
		return status;
	segment = arena->head_segment;
	for (steps = 0u; steps < MCB_MAXIMUM_WALK; ++steps) {
		struct mcb_record record;
		uint16_t next;

		status = read_mcb(machine, segment, &record);
		if (status != DOS_MEMORY_OK)
			return status;
		if (mcb_signature(&record) == MCB_SIGNATURE_END)
			return DOS_MEMORY_OK;
		status = next_mcb_segment(segment, &record, &next);
		if (status != DOS_MEMORY_OK)
			return status;
		segment = next;
	}
	return DOS_MEMORY_ARENA_DAMAGED;
}

enum dos_memory_status dos_memory_arena_poison(struct dos_memory_arena *arena)
{
	if (!arena_flags_have_valid_encoding(arena) ||
	    arena->initialized != 1u ||
	    !arena_identity_is_valid(arena->identity) ||
	    arena->generation == 0u)
		return DOS_MEMORY_INVALID_ARGUMENT;
	arena->machine_poisoned = 1u;
	return DOS_MEMORY_OK;
}

static enum dos_memory_status
find_candidates(struct dos_memory_arena *arena,
		const struct dos_machine *machine, uint16_t requested,
		struct allocation_candidates *candidates)
{
	uint16_t segment = arena->head_segment;
	uint32_t steps;

	*candidates = (struct allocation_candidates){0};
	for (steps = 0u; steps < MCB_MAXIMUM_WALK; ++steps) {
		struct mcb_record record;
		uint16_t next;
		uint16_t size;
		enum dos_memory_status status =
		    read_mcb(machine, segment, &record);

		if (status != DOS_MEMORY_OK)
			return status;
		if (mcb_owner(&record) == MCB_OWNER_FREE) {
			status =
			    coalesce_forward(arena, machine, segment, &record);
			if (status != DOS_MEMORY_OK)
				return status;
			size = mcb_size(&record);
			if (size > candidates->maximum)
				candidates->maximum = size;
			if (size >= requested) {
				if (!candidates->found) {
					candidates->first = segment;
					candidates->best = segment;
					candidates->best_size = size;
					candidates->found = true;
				} else if (size < candidates->best_size) {
					candidates->best = segment;
					candidates->best_size = size;
				}
				candidates->last = segment;
			}
		}
		if (mcb_signature(&record) == MCB_SIGNATURE_END)
			return DOS_MEMORY_OK;
		status = next_mcb_segment(segment, &record, &next);
		if (status != DOS_MEMORY_OK)
			return status;
		segment = next;
	}
	return DOS_MEMORY_ARENA_DAMAGED;
}

enum dos_memory_status
dos_memory_query_maximum_checked(struct dos_memory_arena *arena,
				 const struct dos_machine *machine,
				 uint16_t *maximum_available)
{
	struct allocation_candidates candidates;
	enum dos_memory_status status;

	if (maximum_available == NULL)
		return DOS_MEMORY_INVALID_ARGUMENT;
	status = arena_is_ready(arena, machine);
	if (status != DOS_MEMORY_OK)
		return status;
	status = find_candidates(arena, machine, 0xffffu, &candidates);
	if (status != DOS_MEMORY_OK)
		return status;
	*maximum_available = candidates.maximum;
	return DOS_MEMORY_OK;
}

static enum dos_memory_status
allocate_low(struct dos_memory_arena *arena, const struct dos_machine *machine,
	     uint16_t segment, uint16_t requested, uint16_t owner,
	     const uint8_t *owner_name, uint16_t *block_segment)
{
	struct mcb_record current_before;
	struct mcb_record current_after;
	uint16_t original_size;
	enum dos_memory_status status =
	    read_mcb(machine, segment, &current_before);

	if (status != DOS_MEMORY_OK)
		return status;
	original_size = mcb_size(&current_before);
	if (requested > original_size)
		return DOS_MEMORY_ARENA_DAMAGED;
	current_after = current_before;
	mcb_set_owner(&current_after, owner);
	if (owner_name != NULL)
		mcb_set_owner_name(&current_after, owner_name);
	if (requested < original_size) {
		struct mcb_record remainder_before;
		struct mcb_record remainder_after;
		uint32_t remainder_value =
		    (uint32_t)segment + (uint32_t)requested + 1u;
		uint16_t remainder_segment;

		if (remainder_value > 0xffffu)
			return DOS_MEMORY_ARENA_DAMAGED;
		remainder_segment = (uint16_t)remainder_value;
		status = read_record_bytes(machine, remainder_segment,
					   &remainder_before);
		if (status != DOS_MEMORY_OK)
			return status;
		remainder_after = remainder_before;
		mcb_set_signature(&remainder_after,
				  mcb_signature(&current_before));
		mcb_set_owner(&remainder_after, MCB_OWNER_FREE);
		mcb_set_size(&remainder_after,
			     (uint16_t)(original_size - requested - 1u));
		mcb_set_signature(&current_after, MCB_SIGNATURE_NORMAL);
		mcb_set_size(&current_after, requested);
		status = commit_split(arena, machine, segment, &current_after,
				      remainder_segment, &remainder_before,
				      &remainder_after);
		if (status == DOS_MEMORY_OK)
			*block_segment = (uint16_t)(segment + 1u);
		return status;
	}
	status = replace_record_bytes(arena, machine, segment, &current_after);
	if (status == DOS_MEMORY_OK)
		*block_segment = (uint16_t)(segment + 1u);
	return status;
}

static enum dos_memory_status
allocate_high(struct dos_memory_arena *arena, const struct dos_machine *machine,
	      uint16_t segment, uint16_t requested, uint16_t owner,
	      const uint8_t *owner_name, uint16_t *block_segment)
{
	struct mcb_record lower_before;
	struct mcb_record lower_after;
	uint16_t original_size;
	uint16_t difference;
	enum dos_memory_status status =
	    read_mcb(machine, segment, &lower_before);

	if (status != DOS_MEMORY_OK)
		return status;
	original_size = mcb_size(&lower_before);
	if (requested > original_size)
		return DOS_MEMORY_ARENA_DAMAGED;
	if (requested == original_size)
		return allocate_low(arena, machine, segment, requested, owner,
				    owner_name, block_segment);
	difference = (uint16_t)(original_size - requested);
	{
		struct mcb_record upper_before;
		struct mcb_record upper_after;
		uint32_t upper_value = (uint32_t)segment + difference;
		uint16_t upper_segment;

		if (upper_value > 0xffffu)
			return DOS_MEMORY_ARENA_DAMAGED;
		upper_segment = (uint16_t)upper_value;
		status =
		    read_record_bytes(machine, upper_segment, &upper_before);
		if (status != DOS_MEMORY_OK)
			return status;
		upper_after = upper_before;
		mcb_set_signature(&upper_after, mcb_signature(&lower_before));
		mcb_set_size(&upper_after, requested);
		mcb_set_owner(&upper_after, owner);
		if (owner_name != NULL)
			mcb_set_owner_name(&upper_after, owner_name);
		lower_after = lower_before;
		mcb_set_signature(&lower_after, MCB_SIGNATURE_NORMAL);
		mcb_set_size(&lower_after, (uint16_t)(difference - 1u));
		mcb_set_owner(&lower_after, MCB_OWNER_FREE);
		status =
		    commit_split(arena, machine, segment, &lower_after,
				 upper_segment, &upper_before, &upper_after);
		if (status == DOS_MEMORY_OK)
			*block_segment = (uint16_t)(upper_segment + 1u);
	}
	return status;
}

static enum dos_memory_status
allocate_checked_common(struct dos_memory_arena *arena,
			const struct dos_machine *machine, uint16_t owner_psp,
			const uint8_t *owner_name, uint16_t paragraphs,
			struct dos_memory_allocation_result *result)
{
	struct allocation_candidates candidates;
	uint16_t selected;
	enum dos_memory_status status;

	if (result != NULL)
		*result = (struct dos_memory_allocation_result){0};
	if (result == NULL)
		return DOS_MEMORY_INVALID_ARGUMENT;
	status = arena_is_ready(arena, machine);
	if (status != DOS_MEMORY_OK)
		return status;
	status = find_candidates(arena, machine, paragraphs, &candidates);
	if (status != DOS_MEMORY_OK)
		return status;
	result->maximum_available = candidates.maximum;
	if (!candidates.found)
		return DOS_MEMORY_NOT_ENOUGH_MEMORY;
	if (arena->strategy == DOS_ALLOC_FIRST_FIT)
		selected = candidates.first;
	else if (arena->strategy == DOS_ALLOC_BEST_FIT)
		selected = candidates.best;
	else
		selected = candidates.last;
	if (arena->strategy > DOS_ALLOC_BEST_FIT)
		status = allocate_high(arena, machine, selected, paragraphs,
				       owner_psp, owner_name,
				       &result->block_segment);
	else
		status =
		    allocate_low(arena, machine, selected, paragraphs,
				 owner_psp, owner_name, &result->block_segment);
	return status;
}

enum dos_memory_status
dos_memory_allocate_checked(struct dos_memory_arena *arena,
			    const struct dos_machine *machine,
			    uint16_t owner_psp, uint16_t paragraphs,
			    struct dos_memory_allocation_result *result)
{
	return allocate_checked_common(arena, machine, owner_psp, NULL,
				       paragraphs, result);
}

enum dos_memory_status dos_memory_allocate_named_checked(
    struct dos_memory_arena *arena, const struct dos_machine *machine,
    const struct dos_memory_owner_identity *owner, uint16_t paragraphs,
    struct dos_memory_allocation_result *result)
{
	if (result != NULL)
		*result = (struct dos_memory_allocation_result){0};
	if (owner == NULL || owner->psp_segment == MCB_OWNER_FREE)
		return DOS_MEMORY_INVALID_ARGUMENT;
	return allocate_checked_common(arena, machine, owner->psp_segment,
				       owner->name, paragraphs, result);
}

static enum dos_memory_status locate_mcb(const struct dos_memory_arena *arena,
					 const struct dos_machine *machine,
					 uint16_t wanted_segment,
					 struct mcb_record *wanted_record)
{
	uint16_t segment = arena->head_segment;
	uint32_t steps;

	for (steps = 0u; steps < MCB_MAXIMUM_WALK; ++steps) {
		struct mcb_record record;
		uint16_t next;
		enum dos_memory_status status =
		    read_mcb(machine, segment, &record);

		if (status != DOS_MEMORY_OK)
			return status;
		if (segment == wanted_segment) {
			*wanted_record = record;
			return DOS_MEMORY_OK;
		}
		if (wanted_segment < segment ||
		    mcb_signature(&record) == MCB_SIGNATURE_END)
			return DOS_MEMORY_INVALID_BLOCK;
		status = next_mcb_segment(segment, &record, &next);
		if (status != DOS_MEMORY_OK)
			return status;
		segment = next;
	}
	return DOS_MEMORY_ARENA_DAMAGED;
}

static enum dos_memory_status
free_checked_common(struct dos_memory_arena *arena,
		    const struct dos_machine *machine, uint16_t block_segment,
		    bool verify_owner, uint16_t expected_owner)
{
	struct mcb_record record;
	uint16_t header_segment;
	enum dos_memory_status status = arena_is_ready(arena, machine);

	if (status != DOS_MEMORY_OK)
		return status;
	if (verify_owner && expected_owner == MCB_OWNER_FREE)
		return DOS_MEMORY_INVALID_ARGUMENT;
	header_segment = (uint16_t)(block_segment - 1u);
	if (verify_owner)
		status = locate_mcb(arena, machine, header_segment, &record);
	else {
		status = read_record_bytes(machine, header_segment, &record);
		if (status == DOS_MEMORY_OK && !mcb_signature_is_valid(&record))
			status = DOS_MEMORY_INVALID_BLOCK;
	}
	if (status != DOS_MEMORY_OK)
		return status;
	if (verify_owner && mcb_owner(&record) != expected_owner)
		return DOS_MEMORY_OWNER_MISMATCH;
	mcb_set_owner(&record, MCB_OWNER_FREE);
	return replace_record_bytes(arena, machine, header_segment, &record);
}

enum dos_memory_status
dos_memory_free_checked(struct dos_memory_arena *arena,
			const struct dos_machine *machine,
			uint16_t block_segment)
{
	return free_checked_common(arena, machine, block_segment, false, 0u);
}

enum dos_memory_status
dos_memory_free_owned_checked(struct dos_memory_arena *arena,
			      const struct dos_machine *machine,
			      uint16_t block_segment, uint16_t expected_owner)
{
	return free_checked_common(arena, machine, block_segment, true,
				   expected_owner);
}

bool dos_memory_owner_rebind_value_has_valid_encoding(
    const struct dos_memory_owner_rebind_value *value)
{
	struct mcb_record record;
	size_t index;

	if (value == NULL || value->expected_owner == MCB_OWNER_FREE ||
	    value->new_owner == MCB_OWNER_FREE)
		return false;
	for (index = 0u; index < ARRAY_SIZE(value->reserved); ++index) {
		if (value->reserved[index] != 0u)
			return false;
	}
	for (index = 0u; index < ARRAY_SIZE(record.bytes); ++index)
		record.bytes[index] = value->replacement_bytes[index];
	return mcb_signature_is_valid(&record) &&
	       mcb_extent_is_valid(value->header_segment, &record) &&
	       mcb_owner(&record) == value->new_owner;
}

static enum dos_memory_status prepare_owner_rebind_checked_common(
    const struct dos_memory_arena *arena, const struct dos_machine *machine,
    uint16_t block_segment, uint16_t expected_owner, uint16_t new_owner,
    const uint8_t *new_name, uint8_t new_name_count,
    struct dos_memory_owner_rebind_value *value)
{
	struct dos_memory_owner_rebind_value prepared = {0};
	struct mcb_record record;
	uint16_t header_segment;
	enum dos_memory_status status;
	size_t index;

	if (value == NULL || expected_owner == MCB_OWNER_FREE ||
	    new_owner == MCB_OWNER_FREE ||
	    (new_name == NULL && new_name_count != 0u) ||
	    (new_name != NULL &&
	     (new_name_count == 0u ||
	      new_name_count > DOS_MEMORY_OWNER_NAME_BYTES)))
		return DOS_MEMORY_INVALID_ARGUMENT;
	status = arena_is_ready(arena, machine);
	if (status != DOS_MEMORY_OK)
		return status;
	header_segment = (uint16_t)(block_segment - 1u);
	status = locate_mcb(arena, machine, header_segment, &record);
	if (status != DOS_MEMORY_OK)
		return status;
	if (mcb_owner(&record) != expected_owner)
		return DOS_MEMORY_OWNER_MISMATCH;
	mcb_set_owner(&record, new_owner);
	if (new_name != NULL)
		mcb_patch_owner_name(&record, new_name, new_name_count);
	prepared.header_segment = header_segment;
	prepared.expected_owner = expected_owner;
	prepared.new_owner = new_owner;
	for (index = 0u; index < ARRAY_SIZE(record.bytes); ++index)
		prepared.replacement_bytes[index] = record.bytes[index];
	if (!dos_memory_owner_rebind_value_has_valid_encoding(&prepared))
		return DOS_MEMORY_ARENA_DAMAGED;
	*value = prepared;
	return DOS_MEMORY_OK;
}

enum dos_memory_status dos_memory_prepare_owner_rebind_checked(
    const struct dos_memory_arena *arena, const struct dos_machine *machine,
    uint16_t block_segment, uint16_t expected_owner, uint16_t new_owner,
    struct dos_memory_owner_rebind_value *value)
{
	return prepare_owner_rebind_checked_common(
	    arena, machine, block_segment, expected_owner, new_owner, NULL, 0u,
	    value);
}

enum dos_memory_status dos_memory_prepare_owner_name_patch_rebind_checked(
    const struct dos_memory_arena *arena, const struct dos_machine *machine,
    uint16_t block_segment, uint16_t expected_owner, uint16_t new_owner,
    const struct dos_memory_owner_name_patch *name_patch,
    struct dos_memory_owner_rebind_value *value)
{
	uint8_t stable_name[DOS_MEMORY_OWNER_NAME_BYTES];
	uint8_t stable_count;
	size_t index;

	if (!owner_name_patch_is_valid(name_patch))
		return DOS_MEMORY_INVALID_ARGUMENT;
	stable_count = name_patch->count;
	for (index = 0u; index < ARRAY_SIZE(stable_name); ++index)
		stable_name[index] = name_patch->bytes[index];
	return prepare_owner_rebind_checked_common(
	    arena, machine, block_segment, expected_owner, new_owner,
	    stable_name, stable_count, value);
}

static enum dos_memory_status transfer_owner_checked_common(
    struct dos_memory_arena *arena, const struct dos_machine *machine,
    uint16_t block_segment, uint16_t expected_owner, uint16_t new_owner,
    const uint8_t *new_name, uint8_t new_name_count)
{
	struct dos_memory_owner_rebind_value value;
	struct mcb_record replacement;
	enum dos_memory_status status;
	size_t index;

	status = prepare_owner_rebind_checked_common(
	    arena, machine, block_segment, expected_owner, new_owner, new_name,
	    new_name_count, &value);
	if (status != DOS_MEMORY_OK)
		return status;
	for (index = 0u; index < ARRAY_SIZE(replacement.bytes); ++index)
		replacement.bytes[index] = value.replacement_bytes[index];
	return replace_record_bytes(arena, machine, value.header_segment,
				    &replacement);
}

enum dos_memory_status dos_memory_transfer_owner_checked(
    struct dos_memory_arena *arena, const struct dos_machine *machine,
    uint16_t block_segment, uint16_t expected_owner, uint16_t new_owner)
{
	return transfer_owner_checked_common(
	    arena, machine, block_segment, expected_owner, new_owner, NULL, 0u);
}

enum dos_memory_status dos_memory_transfer_named_owner_checked(
    struct dos_memory_arena *arena, const struct dos_machine *machine,
    uint16_t block_segment, uint16_t expected_owner,
    const struct dos_memory_owner_identity *new_owner)
{
	uint8_t stable_name[DOS_MEMORY_OWNER_NAME_BYTES];
	uint16_t stable_owner;
	size_t index;

	if (new_owner == NULL || new_owner->psp_segment == MCB_OWNER_FREE ||
	    expected_owner == MCB_OWNER_FREE)
		return DOS_MEMORY_INVALID_ARGUMENT;
	stable_owner = new_owner->psp_segment;
	for (index = 0u; index < DOS_MEMORY_OWNER_NAME_BYTES; ++index)
		stable_name[index] = new_owner->name[index];
	return transfer_owner_checked_common(
	    arena, machine, block_segment, expected_owner, stable_owner,
	    stable_name, DOS_MEMORY_OWNER_NAME_BYTES);
}

enum dos_memory_status dos_memory_transfer_owner_name_patch_checked(
    struct dos_memory_arena *arena, const struct dos_machine *machine,
    uint16_t block_segment, uint16_t expected_owner, uint16_t new_owner,
    const struct dos_memory_owner_name_patch *name_patch)
{
	uint8_t stable_name[DOS_MEMORY_OWNER_NAME_BYTES];
	uint8_t stable_count;
	size_t index;

	if (expected_owner == MCB_OWNER_FREE || new_owner == MCB_OWNER_FREE ||
	    !owner_name_patch_is_valid(name_patch))
		return DOS_MEMORY_INVALID_ARGUMENT;
	stable_count = name_patch->count;
	for (index = 0u; index < DOS_MEMORY_OWNER_NAME_BYTES; ++index)
		stable_name[index] = name_patch->bytes[index];
	return transfer_owner_checked_common(arena, machine, block_segment,
					     expected_owner, new_owner,
					     stable_name, stable_count);
}

enum dos_memory_status
dos_memory_resize_checked(struct dos_memory_arena *arena,
			  const struct dos_machine *machine,
			  uint16_t block_segment, uint16_t current_psp,
			  uint16_t paragraphs, uint16_t *maximum_available)
{
	struct mcb_record record;
	uint16_t header_segment;
	uint16_t ignored_segment;
	enum dos_memory_status status;

	if (maximum_available != NULL)
		*maximum_available = 0u;
	if (maximum_available == NULL)
		return DOS_MEMORY_INVALID_ARGUMENT;
	status = arena_is_ready(arena, machine);
	if (status != DOS_MEMORY_OK)
		return status;
	header_segment = (uint16_t)(block_segment - 1u);
	/* SETBLOCK checks ES-1 directly rather than requiring the
	 * target to be reachable from arena_head. */
	status = read_mcb(machine, header_segment, &record);
	if (status != DOS_MEMORY_OK)
		return status;
	status = coalesce_forward(arena, machine, header_segment, &record);
	if (status != DOS_MEMORY_OK)
		return status;
	*maximum_available = mcb_size(&record);
	if (paragraphs > mcb_size(&record))
		return DOS_MEMORY_NOT_ENOUGH_MEMORY;
	/* SETBLOCK falls through the owner update and writes
	 * CurrentPDB even when the block previously had another owner. */
	return allocate_low(arena, machine, header_segment, paragraphs,
			    current_psp, NULL, &ignored_segment);
}

enum dos_memory_status
dos_memory_free_process_checked(struct dos_memory_arena *arena,
				const struct dos_machine *machine,
				uint16_t owner_psp)
{
	uint16_t segment;
	uint32_t steps;
	bool changed = false;
	enum dos_memory_status status = arena_is_ready(arena, machine);

	if (status != DOS_MEMORY_OK)
		return status;
	segment = arena->head_segment;
	for (steps = 0u; steps < MCB_MAXIMUM_WALK; ++steps) {
		struct mcb_record record;
		uint16_t next;

		status = read_mcb(machine, segment, &record);
		if (status != DOS_MEMORY_OK) {
			if (changed) {
				arena->machine_poisoned = 1u;
				return DOS_MEMORY_MACHINE_POISONED;
			}
			return status;
		}
		if (mcb_owner(&record) == owner_psp) {
			mcb_set_owner(&record, MCB_OWNER_FREE);
			status = replace_record_bytes(arena, machine, segment,
						      &record);
			if (status != DOS_MEMORY_OK) {
				if (changed &&
				    status != DOS_MEMORY_MACHINE_POISONED) {
					arena->machine_poisoned = 1u;
					return DOS_MEMORY_MACHINE_POISONED;
				}
				return status;
			}
			changed = true;
		}
		if (mcb_signature(&record) == MCB_SIGNATURE_END)
			return DOS_MEMORY_OK;
		status = next_mcb_segment(segment, &record, &next);
		if (status != DOS_MEMORY_OK) {
			if (changed) {
				arena->machine_poisoned = 1u;
				return DOS_MEMORY_MACHINE_POISONED;
			}
			return status;
		}
		segment = next;
	}
	return DOS_MEMORY_ARENA_DAMAGED;
}

enum dos_memory_status
dos_memory_get_strategy_checked(const struct dos_memory_arena *arena,
				uint8_t *strategy)
{
	if (!arena_flags_have_valid_encoding(arena) || strategy == NULL)
		return DOS_MEMORY_INVALID_ARGUMENT;
	if (arena->machine_poisoned == 1u)
		return DOS_MEMORY_MACHINE_POISONED;
	if (arena->initialized != 1u ||
	    !arena_identity_is_valid(arena->identity) ||
	    arena->generation == 0u)
		return DOS_MEMORY_INVALID_ARGUMENT;
	*strategy = arena->strategy;
	return DOS_MEMORY_OK;
}

enum dos_memory_status
dos_memory_set_strategy_checked(struct dos_memory_arena *arena,
				uint8_t strategy)
{
	if (!arena_flags_have_valid_encoding(arena))
		return DOS_MEMORY_INVALID_ARGUMENT;
	if (arena->machine_poisoned == 1u)
		return DOS_MEMORY_MACHINE_POISONED;
	if (arena->initialized != 1u ||
	    !arena_identity_is_valid(arena->identity) ||
	    arena->generation == 0u)
		return DOS_MEMORY_INVALID_ARGUMENT;
	arena->strategy = strategy;
	return DOS_MEMORY_OK;
}

static enum dos_error status_to_dos_error(enum dos_memory_status status,
					  enum dos_error invalid_error)
{
	switch (status) {
	case DOS_MEMORY_OK:
		return DOS_SUCCESS;
	case DOS_MEMORY_INVALID_ARGUMENT:
		return invalid_error;
	case DOS_MEMORY_INVALID_BLOCK:
		return DOS_ERROR_INVALID_BLOCK;
	case DOS_MEMORY_NOT_ENOUGH_MEMORY:
		return DOS_ERROR_NOT_ENOUGH_MEMORY;
	case DOS_MEMORY_OWNER_MISMATCH:
		return DOS_ERROR_ACCESS_DENIED;
	case DOS_MEMORY_IDENTITY_MISMATCH:
	case DOS_MEMORY_GENERATION_EXHAUSTED:
	case DOS_MEMORY_ARENA_DAMAGED:
	case DOS_MEMORY_MACHINE_FAULT:
	case DOS_MEMORY_MACHINE_POISONED:
		return DOS_ERROR_ARENA_TRASHED;
	}
	return DOS_ERROR_ARENA_TRASHED;
}

enum dos_error dos_memory_arena_initialize(struct dos_memory_arena *arena,
					   const struct dos_machine *machine,
					   uint16_t head_segment,
					   uint16_t end_segment)
{
	return status_to_dos_error(
	    dos_memory_arena_initialize_checked(arena, machine, head_segment,
						end_segment),
	    DOS_ERROR_INVALID_PARAMETER);
}

enum dos_error dos_memory_arena_validate(const struct dos_memory_arena *arena,
					 const struct dos_machine *machine)
{
	return status_to_dos_error(
	    dos_memory_arena_validate_checked(arena, machine),
	    DOS_ERROR_INVALID_PARAMETER);
}

enum dos_error dos_memory_allocate(struct dos_memory_arena *arena,
				   const struct dos_machine *machine,
				   uint16_t owner_psp, uint16_t paragraphs,
				   uint16_t *block_segment,
				   uint16_t *maximum_available)
{
	struct dos_memory_allocation_result result = {0};
	enum dos_memory_status status;

	if (block_segment != NULL)
		*block_segment = 0u;
	if (maximum_available != NULL)
		*maximum_available = 0u;
	if (block_segment == NULL || maximum_available == NULL)
		return DOS_ERROR_INVALID_PARAMETER;
	status = dos_memory_allocate_checked(arena, machine, owner_psp,
					     paragraphs, &result);
	*block_segment = result.block_segment;
	*maximum_available = result.maximum_available;
	return status_to_dos_error(status, DOS_ERROR_INVALID_PARAMETER);
}

enum dos_error dos_memory_free(struct dos_memory_arena *arena,
			       const struct dos_machine *machine,
			       uint16_t block_segment)
{
	return status_to_dos_error(
	    dos_memory_free_checked(arena, machine, block_segment),
	    DOS_ERROR_INVALID_BLOCK);
}

enum dos_error dos_memory_resize(struct dos_memory_arena *arena,
				 const struct dos_machine *machine,
				 uint16_t block_segment, uint16_t current_psp,
				 uint16_t paragraphs,
				 uint16_t *maximum_available)
{
	return status_to_dos_error(
	    dos_memory_resize_checked(arena, machine, block_segment,
				      current_psp, paragraphs,
				      maximum_available),
	    DOS_ERROR_INVALID_BLOCK);
}

enum dos_error dos_memory_free_process(struct dos_memory_arena *arena,
				       const struct dos_machine *machine,
				       uint16_t owner_psp)
{
	return status_to_dos_error(
	    dos_memory_free_process_checked(arena, machine, owner_psp),
	    DOS_ERROR_INVALID_PARAMETER);
}

enum dos_error dos_memory_get_strategy(const struct dos_memory_arena *arena,
				       uint8_t *strategy)
{
	return status_to_dos_error(
	    dos_memory_get_strategy_checked(arena, strategy),
	    DOS_ERROR_INVALID_PARAMETER);
}

enum dos_error dos_memory_set_strategy(struct dos_memory_arena *arena,
				       uint8_t strategy)
{
	return status_to_dos_error(
	    dos_memory_set_strategy_checked(arena, strategy),
	    DOS_ERROR_INVALID_PARAMETER);
}

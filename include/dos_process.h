/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Safe DOS process-image construction.
 *
 * DOS guest addresses remain integer segment:offset values.  No value in
 * this interface is a disguised native pointer.  PSP preparation is read
 * only; committing the prepared image performs one bounded guest write.
 */
#ifndef DOSC32_DOS_PROCESS_H
#define DOSC32_DOS_PROCESS_H

#include "compiler.h"
#include "dos_abi.h"
#include "dos_loader.h"
#include "dos_machine.h"
#include "dos_sft_batch.h"
#include "types.h"

#define DOS_PROCESS_FCB_PREFIX_BYTES 12u
#define DOS_PROCESS_INHERITABLE_HANDLE_MASK                                    \
	((uint32_t)((1u << DOS_PSP_DEFAULT_HANDLES) - 1u))

#define DOS_PROCESS_COMMAND_CR 0x0du

enum dos_process_status {
	DOS_PROCESS_OK = 0,
	DOS_PROCESS_INVALID_ARGUMENT,
	DOS_PROCESS_MACHINE_FAULT,
	/* A failed rollback leaves guest memory indeterminate; stop the
	   backend. */
	DOS_PROCESS_MACHINE_POISONED,
	DOS_PROCESS_RANGE_OVERFLOW,
	DOS_PROCESS_INVALID_PSP,
	DOS_PROCESS_BAD_COMMAND_TAIL,
	DOS_PROCESS_COMMAND_TAIL_TOO_LONG,
	DOS_PROCESS_WRONG_IMAGE_FORMAT,
	DOS_PROCESS_NOT_ENOUGH_MEMORY,
	DOS_PROCESS_BAD_IMAGE_RANGE,
	/* A fixed parent snapshot belongs to another guest-machine lifetime. */
	DOS_PROCESS_STALE_SNAPSHOT
};

enum dos_process_launch_mode {
	DOS_PROCESS_LAUNCH_EXECUTE = 0,
	DOS_PROCESS_LAUNCH_LOAD_ONLY
};

static inline bool dos_process_launch_value_is_valid(uint8_t launch_mode)
{
	return launch_mode == (uint8_t)DOS_PROCESS_LAUNCH_EXECUTE ||
	       launch_mode == (uint8_t)DOS_PROCESS_LAUNCH_LOAD_ONLY;
}

/* A decoded guest far address, never a native pointer. */
struct dos_process_far_address {
	uint16_t segment;
	uint16_t offset;
};

/*
 * Inputs corresponding to the EXEC0/EXEC1 parameter block after the INT 21h
 * dispatcher has validated the call itself.  EXEC copies all 128 source
 * command-tail bytes verbatim; canonical length/data/CR construction is an
 * optional upper-layer policy provided by dos_process_encode_command_tail().
 *
 * inheritable_handle_mask exists only for the compatibility wrapper
 * dos_process_prepare_psp().  The full EXEC path captures a parent snapshot,
 * lets dos_sft_batch_prepare() produce the exact child JFT, and passes that
 * value to dos_process_prepare_psp_from_snapshot().
 */
struct dos_process_psp_request {
	uint16_t psp_segment;
	uint16_t block_end_segment;
	uint16_t parent_psp_segment;
	uint16_t environment_segment;
	struct dos_process_far_address terminate_vector;
	struct dos_process_far_address control_c_vector;
	struct dos_process_far_address critical_error_vector;
	struct dos_process_far_address command_tail_source;
	struct dos_process_far_address first_fcb_source;
	struct dos_process_far_address second_fcb_source;
	uint32_t inheritable_handle_mask;
};

/*
 * Native staging metadata followed by exactly one guest PSP image.  Segment
 * zero is a simulated DOS value, not a native NULL sentinel.
 */
struct dos_process_psp_image {
	uint16_t segment;
	uint8_t bytes[DOS_PSP_SIZE];
};

/*
 * Initial DOS/COMMAND PDB. Unlike an EXEC child it has no parent snapshot or
 * parameter block: initialization installs CON
 * SFNs 0..2, marks JFT 3..19 unused, and COMMAND makes itself its parent.
 * Unspecified padding is deterministically cleared as a safety improvement.
 */
struct dos_process_initial_psp_request {
	uint16_t psp_segment;
	uint16_t block_end_segment;
	uint16_t environment_segment;
	struct dos_process_far_address terminate_vector;
	struct dos_process_far_address control_c_vector;
	struct dos_process_far_address critical_error_vector;
};

static_assert_expression(sizeof(struct dos_process_initial_psp_request) == 18,
			 "initial PSP requests must stay fixed width");

enum dos_process_status dos_process_prepare_initial_psp(
	const struct dos_process_initial_psp_request *request,
	struct dos_process_psp_image *image) __must_check;

/*
 * One immutable read of the parent PSP and the first twenty parent JFT bytes.
 * These values are captured before consuming the caller's FCBs and command
 * tail. The EXEC coordinator captures this while its observation
 * owner is held, then gives parent_jft to the SFT batch exactly once.
 *
 * No field is a native pointer.  machine_identity names a generation-pinned
 * backend lifetime; context is only its fixed-width callback key.  Segment
 * zero remains a valid parent PSP value.
 */
struct dos_process_parent_snapshot {
	kernel_object_handle_t machine_identity;
	kernel_object_handle_t machine_context;
	uint64_t machine_address_limit;
	uint16_t parent_psp_segment;
	uint8_t a20_enabled;
	uint8_t captured;
	uint8_t reserved[4];
	uint8_t parent_psp[DOS_PSP_SIZE];
	struct dos_sft_jft20 parent_jft;
	uint8_t reserved_tail[4];
};

static_assert_expression(sizeof(struct dos_process_parent_snapshot) == 312,
			 "parent snapshot must be data-model independent");
static_assert_expression(__builtin_offsetof(struct dos_process_parent_snapshot,
					    parent_psp) == 32,
			 "parent PSP snapshot offset changed");
static_assert_expression(__builtin_offsetof(struct dos_process_parent_snapshot,
					    parent_jft) == 288,
			 "parent JFT snapshot offset changed");

/*
 * Encode an exact, bounded raw command line.  source_length is the count DOS
 * exposes at PSP:80h and therefore cannot exceed 126: byte 127 is reserved
 * for the mandatory CR.  A CR inside the counted bytes is malformed.  The
 * output is unchanged on error.
 */
enum dos_process_status
dos_process_encode_command_tail(const uint8_t *source, size_t source_capacity,
				size_t source_length,
				struct dos_command_tail40 *tail) __must_check;

/*
 * Capture only the parent values consumed by $Dup_PDB.  The output is
 * unchanged on error.  The caller holds the EXEC observation owner across
 * this call, SFT preparation, PSP preparation, and final seal/abort.
 */
enum dos_process_status dos_process_capture_parent_snapshot(
    const struct dos_machine *machine, kernel_object_handle_t machine_identity,
    uint16_t parent_psp_segment,
    struct dos_process_parent_snapshot *snapshot) __must_check;

/*
 * Construct from one captured parent and one fully prepared child JFT.  Only
 * the two FCB prefixes and raw command tail are read here in their DOS-defined
 * FCB1, FCB2, command-tail order.
 * The image is unchanged on error and no parent guest byte is read again.
 */
enum dos_process_status dos_process_prepare_psp_from_snapshot(
    const struct dos_machine *machine, kernel_object_handle_t machine_identity,
    const struct dos_process_parent_snapshot *parent_snapshot,
    const struct dos_process_psp_request *request,
    const struct dos_sft_jft20 *child_jft,
    struct dos_process_psp_image *image) __must_check;

/*
 * Compatibility convenience wrapper: capture a parent, apply the legacy
 * mask, and construct.  New EXEC composition uses the two explicit calls
 * above so SFT acquisition and PSP bytes share the same captured JFT.
 */
enum dos_process_status
dos_process_prepare_psp(const struct dos_machine *machine,
			const struct dos_process_psp_request *request,
			struct dos_process_psp_image *image) __must_check;

/*
 * Bind the exact generation-pinned SFT batch result into a prepared PSP.
 * This is a value-only staging operation and performs no guest callback.
 * It also verifies that the image still describes its canonical 20-byte JFT
 * at PSP:0018h.  The image is unchanged on every error.
 */
enum dos_process_status
dos_process_psp_set_jft20(struct dos_process_psp_image *image,
			  const struct dos_sft_jft20 *child_jft) __must_check;

/* Transactional replacement; validation errors never reach the guest write. */
enum dos_process_status
dos_process_commit_psp(const struct dos_machine *machine,
		       const struct dos_process_psp_image *image) __must_check;

enum dos_process_status dos_process_build_psp(
    const struct dos_machine *machine,
    const struct dos_process_psp_request *request) __must_check;

/*
 * EXEC first queries the largest block after allocating the environment, then
 * selects an exact request under the arena critical section. This value
 * object crosses that boundary without retaining a native arena pointer.  The
 * caller must allocate exactly block_paragraphs before building a process
 * plan; overlays use a separate, future target-capacity contract.
 */
struct dos_process_allocation_plan {
	uint8_t format;
	uint8_t load_high;
	uint16_t reserved;
	uint16_t available_paragraphs;
	uint16_t block_paragraphs;
};

enum dos_process_status dos_process_select_allocation(
    const struct dos_load_plan *image_plan, uint16_t available_paragraphs,
    struct dos_process_allocation_plan *allocation_plan) __must_check;

/*
 * COM and MZ plans are deliberately different types.  This prevents the
 * historical COM PSP-relative state from being confused with an MZ load
 * module's relocation factor and header-relative CS/SS values.
 */
struct dos_com_process_plan {
	uint16_t psp_segment;
	uint16_t block_end_segment;
	uint16_t load_segment;
	uint16_t load_offset;
	dos_linear_address_t load_linear_address;
	uint32_t image_size;
	uint32_t read_capacity;
	uint16_t stack_sentinel_offset;
	uint16_t stack_sentinel_value;
	uint16_t load_only_stack_pointer;
	uint16_t load_only_stack_value;
	uint8_t launch_mode;
	uint8_t reserved8;
	uint16_t reserved16;
	struct dos_cpu_state initial_state;
};

struct dos_mz_process_plan {
	uint16_t psp_segment;
	uint16_t block_end_segment;
	uint16_t load_segment;
	uint16_t load_offset;
	dos_linear_address_t load_linear_address;
	uint32_t reserved0;
	file_offset_t image_file_offset;
	uint32_t image_size;
	/* Wrapped 16-bit resident-paragraph value. */
	uint32_t resident_paragraphs;
	uint16_t relocation_factor;
	uint16_t relocation_count;
	uint16_t relocation_table_offset;
	uint16_t load_only_stack_pointer;
	uint16_t load_only_stack_value;
	uint8_t load_high;
	uint8_t launch_mode;
	uint32_t reserved1;
	struct dos_cpu_state initial_state;
};

static inline bool dos_process_allocation_plan_has_valid_encoding(
    const struct dos_process_allocation_plan *plan)
{
	return plan != NULL && dos_image_format_value_is_valid(plan->format) &&
	       plan->load_high <= 1u && plan->reserved == 0u;
}

static inline bool
dos_com_process_plan_has_valid_encoding(const struct dos_com_process_plan *plan)
{
	return plan != NULL &&
	       dos_process_launch_value_is_valid(plan->launch_mode) &&
	       plan->reserved8 == 0u && plan->reserved16 == 0u &&
	       dos_cpu_mode_value_is_valid(plan->initial_state.mode);
}

static inline bool
dos_mz_process_plan_has_valid_encoding(const struct dos_mz_process_plan *plan)
{
	return plan != NULL && plan->load_high <= 1u &&
	       dos_process_launch_value_is_valid(plan->launch_mode) &&
	       plan->reserved0 == 0u && plan->reserved1 == 0u &&
	       dos_cpu_mode_value_is_valid(plan->initial_state.mode);
}

static_assert_expression(sizeof(struct dos_process_allocation_plan) == 8,
			 "allocation plan must be data-model independent");
static_assert_expression(__builtin_offsetof(struct dos_process_allocation_plan,
					    available_paragraphs) == 4,
			 "allocation-plan paragraph offset changed");
static_assert_expression(sizeof(struct dos_com_process_plan) == 88,
			 "COM process plan must be data-model independent");
static_assert_expression(__builtin_offsetof(struct dos_com_process_plan,
					    launch_mode) == 28,
			 "COM launch-mode offset changed");
static_assert_expression(__builtin_offsetof(struct dos_com_process_plan,
					    initial_state) == 32,
			 "COM initial-state offset changed");
static_assert_expression(sizeof(struct dos_mz_process_plan) == 104,
			 "MZ process plan must be data-model independent");
static_assert_expression(__builtin_offsetof(struct dos_mz_process_plan,
					    image_file_offset) == 16,
			 "MZ file-offset field moved");
static_assert_expression(__builtin_offsetof(struct dos_mz_process_plan,
					    load_high) == 42,
			 "MZ flags offset changed");
static_assert_expression(__builtin_offsetof(struct dos_mz_process_plan,
					    initial_state) == 48,
			 "MZ initial-state offset changed");

enum dos_process_status
dos_process_plan_com(const struct dos_load_plan *image_plan,
		     const struct dos_process_allocation_plan *allocation_plan,
		     uint16_t psp_segment,
		     enum dos_process_launch_mode launch_mode,
		     uint16_t initial_ax,
		     struct dos_com_process_plan *process_plan) __must_check;

enum dos_process_status dos_process_plan_mz(
    const struct dos_load_plan *image_plan,
    const struct dos_process_allocation_plan *allocation_plan,
    uint16_t psp_segment, enum dos_process_launch_mode launch_mode,
    uint16_t initial_ax, struct dos_mz_process_plan *process_plan) __must_check;

/*
 * EXEC computes the default AX only after copying both caller FCBs into the
 * private PSP. These pure value updates let the coordinator build and load
 * compatible geometry with a placeholder, then bind the drive-visible
 * final AX before the EXEC1 stack write or EXEC0 backend handoff.  They call
 * no guest/backend operation and leave the plan unchanged on error.
 */
enum dos_process_status
dos_process_finalize_com_initial_ax(struct dos_com_process_plan *process_plan,
				    uint16_t initial_ax) __must_check;
enum dos_process_status
dos_process_finalize_mz_initial_ax(struct dos_mz_process_plan *process_plan,
				   uint16_t initial_ax) __must_check;

#endif

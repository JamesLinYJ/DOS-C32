// SPDX-License-Identifier: GPL-2.0-only
/*
 * Freestanding probe for a user-supplied DOS executable.
 *
 * The sample bytes are linked into this temporary test binary by
 * probe-dos-program.sh; they are never copied into the DOS-C32 repository.
 * This exercises the production C classifier, allocation policy, resident
 * loader, MZ relocator, stack validator, and EXEC0 handoff value.
 */
#include "dos_exec_handoff.h"
#include "dos_image_load.h"
#include "dos_relocator.h"

#define PROBE_GUEST_CAPACITY DOS_REAL_MODE_ADDRESS_LIMIT
#define PROBE_READER_CONTEXT ((kernel_object_handle_t)0x50524f424546494cull)
#define PROBE_MACHINE_CONTEXT ((kernel_object_handle_t)0x50524f42454d454dull)
#define PROBE_LEASE_HANDLE ((kernel_object_handle_t)0x0000000100000001ull)
#define PROBE_ARENA_IDENTITY ((kernel_object_handle_t)0x50524f424541524eull)
#define PROBE_PSP_SEGMENT 0x1000u
#define PROBE_AVAILABLE_PARAGRAPHS 0x8000u
#define PROBE_LEASE_OWNER 0x0008u

extern const uint8_t _binary_dos_program_start[];
extern const uint8_t _binary_dos_program_end[];

static uint8_t guest_memory[PROBE_GUEST_CAPACITY];

static size_t program_size(void)
{
	return (size_t)(_binary_dos_program_end - _binary_dos_program_start);
}

static enum dos_image_read_status
probe_image_read(kernel_object_handle_t context, file_offset_t offset,
		 void *destination, size_t destination_capacity, size_t count,
		 size_t *bytes_read)
{
	uint8_t *output = (uint8_t *)destination;
	size_t image_size = program_size();
	size_t index;

	if (bytes_read == NULL)
		return DOS_IMAGE_READ_IO_ERROR;
	*bytes_read = 0u;
	if (context != PROBE_READER_CONTEXT || destination == NULL ||
	    count > destination_capacity || offset > image_size ||
	    count > image_size - (size_t)offset)
		return DOS_IMAGE_READ_IO_ERROR;
	for (index = 0u; index < count; ++index)
		output[index] =
			_binary_dos_program_start[(size_t)offset + index];
	*bytes_read = count;
	return DOS_IMAGE_READ_OK;
}

static enum dos_machine_status
probe_machine_read(kernel_object_handle_t context,
		   dos_linear_address_t linear_address, void *destination,
		   size_t destination_capacity, size_t count)
{
	uint8_t *output = (uint8_t *)destination;
	size_t index;

	if (context != PROBE_MACHINE_CONTEXT || destination == NULL ||
	    count > destination_capacity ||
	    linear_address > sizeof(guest_memory) ||
	    count > sizeof(guest_memory) - (size_t)linear_address)
		return DOS_MACHINE_ADDRESS_FAULT;
	for (index = 0u; index < count; ++index)
		output[index] = guest_memory[(size_t)linear_address + index];
	return DOS_MACHINE_OK;
}

static enum dos_machine_status
probe_machine_write(kernel_object_handle_t context,
		    dos_linear_address_t linear_address, const void *source,
		    size_t source_capacity, size_t count)
{
	const uint8_t *input = (const uint8_t *)source;
	size_t index;

	if (context != PROBE_MACHINE_CONTEXT || source == NULL ||
	    count > source_capacity || linear_address > sizeof(guest_memory) ||
	    count > sizeof(guest_memory) - (size_t)linear_address)
		return DOS_MACHINE_ADDRESS_FAULT;
	for (index = 0u; index < count; ++index)
		guest_memory[(size_t)linear_address + index] = input[index];
	return DOS_MACHINE_OK;
}

static const struct dos_machine_ops probe_machine_ops = {
	.read_memory = probe_machine_read,
	.write_memory = probe_machine_write,
	.read_port = NULL,
	.write_port = NULL,
	.set_a20 = NULL,
};

static struct dos_memory_lease_view
make_lease(const struct dos_process_allocation_plan *allocation)
{
	struct dos_memory_lease_view lease = {
		.handle.value = PROBE_LEASE_HANDLE,
		.machine_context = PROBE_MACHINE_CONTEXT,
		.arena_identity = PROBE_ARENA_IDENTITY,
		.arena_generation = 1u,
		.guest_segment = PROBE_PSP_SEGMENT,
		.paragraphs = allocation->block_paragraphs,
		.owner = PROBE_LEASE_OWNER,
	};

	return lease;
}

static int probe_mz(const struct dos_image_reader *reader,
		    const struct dos_machine *machine,
		    const struct dos_load_plan *image_plan,
		    const struct dos_process_allocation_plan *allocation)
{
	struct dos_mz_process_plan process_plan;
	struct dos_memory_lease_view lease = make_lease(allocation);
	struct dos_image_load_result load_result;
	struct dos_relocator_request relocation;
	struct dos_relocator_result relocation_result;
	struct dos_exec_handoff_plan handoff;

	if (dos_process_plan_mz(image_plan, allocation, PROBE_PSP_SEGMENT,
				DOS_PROCESS_LAUNCH_EXECUTE, 0u,
				&process_plan) != DOS_PROCESS_OK)
		return 5;
	if (dos_image_load_mz_resident(reader, machine, image_plan,
				       &process_plan, &lease,
				       &load_result) != DOS_IMAGE_LOAD_OK)
		return 6;
	if (load_result.lease_handle != lease.handle.value ||
	    load_result.resident_bytes != process_plan.image_size)
		return 10;
	if (dos_image_load_prepare_mz_stack(machine, &process_plan, &lease) !=
	    DOS_IMAGE_LOAD_OK)
		return 7;
	relocation = (struct dos_relocator_request){
		.relocation_table_offset = process_plan.relocation_table_offset,
		.resident_size = (uint64_t)process_plan.resident_paragraphs *
				 DOS_EXEC_PARAGRAPH_BYTES,
		.resident_linear_address = process_plan.load_linear_address,
		.relocation_count = process_plan.relocation_count,
		.relocation_factor = process_plan.relocation_factor,
	};
	if (dos_relocator_apply(reader, machine, &relocation,
				&relocation_result) != DOS_RELOCATOR_OK ||
	    relocation_result.applied_entries != process_plan.relocation_count)
		return 8;
	if (dos_exec_handoff_prepare_mz(&process_plan, &handoff) !=
		    DOS_EXEC_HANDOFF_OK ||
	    !dos_exec_handoff_plan_has_valid_encoding(&handoff))
		return 9;
	return 0;
}

static int probe_com(const struct dos_image_reader *reader,
		     const struct dos_machine *machine,
		     const struct dos_load_plan *image_plan,
		     const struct dos_process_allocation_plan *allocation)
{
	struct dos_com_process_plan process_plan;
	struct dos_memory_lease_view lease = make_lease(allocation);
	struct dos_image_load_result load_result;
	struct dos_exec_handoff_plan handoff;

	if (dos_process_plan_com(image_plan, allocation, PROBE_PSP_SEGMENT,
				 DOS_PROCESS_LAUNCH_EXECUTE, 0u,
				 &process_plan) != DOS_PROCESS_OK)
		return 5;
	if (dos_image_load_com_resident(reader, machine, image_plan,
					&process_plan, &lease,
					&load_result) != DOS_IMAGE_LOAD_OK)
		return 6;
	if (load_result.lease_handle != lease.handle.value ||
	    load_result.file_bytes_written != image_plan->file_size)
		return 10;
	if (dos_image_load_prepare_com_stack(machine, &process_plan, &lease) !=
	    DOS_IMAGE_LOAD_OK)
		return 7;
	if (dos_exec_handoff_prepare_com(&process_plan, &handoff) !=
		    DOS_EXEC_HANDOFF_OK ||
	    !dos_exec_handoff_plan_has_valid_encoding(&handoff))
		return 9;
	return 0;
}

static int run_probe(void)
{
	struct dos_image_reader reader = {
		.context = PROBE_READER_CONTEXT,
		.size = program_size(),
		.read = probe_image_read,
	};
	struct dos_machine machine = {
		.ops = &probe_machine_ops,
		.context = PROBE_MACHINE_CONTEXT,
		.address_limit = sizeof(guest_memory),
		.a20_enabled = false,
	};
	struct dos_load_plan image_plan;
	struct dos_process_allocation_plan allocation;

	if (reader.size == 0u)
		return 1;
	if (dos_loader_inspect(&reader, &image_plan) != DOS_LOADER_OK)
		return 2;
	if (!dos_load_plan_has_inspected_encoding(&image_plan))
		return 3;
	if (dos_process_select_allocation(&image_plan,
					  PROBE_AVAILABLE_PARAGRAPHS,
					  &allocation) != DOS_PROCESS_OK)
		return 4;
	if (image_plan.format == (uint8_t)DOS_IMAGE_MZ)
		return probe_mz(&reader, &machine, &image_plan, &allocation);
	if (image_plan.format == (uint8_t)DOS_IMAGE_COM)
		return probe_com(&reader, &machine, &image_plan, &allocation);
	return 3;
}

void _start(void) __attribute__((noreturn, force_align_arg_pointer));

void _start(void)
{
	int status = run_probe();

#if __SIZEOF_POINTER__ == 8
	__asm__ volatile("syscall"
			 :
			 : "a"((uint64_t)60u), "D"((uint64_t)(uint32_t)status)
			 : "rcx", "r11", "memory");
#else
	__asm__ volatile("int $0x80"
			 :
			 : "a"((uint32_t)1u), "b"((uint32_t)status)
			 : "memory");
#endif
	__builtin_unreachable();
}

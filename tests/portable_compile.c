// SPDX-License-Identifier: GPL-2.0-only
/* Compile-only guard for the shared 32/64-bit data-model boundary. */
#include "address.h"
#include "arena.h"
#include "ata_block.h"
#include "block_device.h"
#include "convert.h"
#include "ctype.h"
#include "dos_abi.h"
#include "dos_environment.h"
#include "dos_environment_view.h"
#include "dos_ems.h"
#include "dos_exec_file_lease.h"
#include "dos_exec_name.h"
#include "dos_exec_observer.h"
#include "dos_exec_parameter.h"
#include "dos_exec_seal.h"
#include "dos_exec_transaction.h"
#include "dos_image_load.h"
#include "dos_int21.h"
#include "dos_loader.h"
#include "dos_machine.h"
#include "dos_memory.h"
#include "dos_memory_lease.h"
#include "dos_personality.h"
#include "dos_process.h"
#include "dos_process_runtime.h"
#include "dos_relocator.h"
#include "dos_sft_batch.h"
#include "dos_vectors.h"
#include "dos_vcpi.h"
#include "dosc32_assert.h"
#include "exec_backend.h"
#include "format.h"
#include "overflow.h"
#include "string.h"
#include "types.h"
#include "x86_io_resource.h"

uintptr_t portable_compile_probe(struct dos_cpu_state *state,
				 const struct dos_mz_header40 *header);

uintptr_t portable_compile_probe(struct dos_cpu_state *state,
				 const struct dos_mz_header40 *header)
{
	uint64_t end;

	if (state == NULL || header == NULL)
		return 0;
	if (check_add_overflow((uint64_t)header->pages,
			       (uint64_t)header->header_paragraphs, &end))
		return 0;
	DOSC32_ASSERT(sizeof(kernel_address_t) == 8u);
	return (uintptr_t)state + (uintptr_t)(end != 0u);
}

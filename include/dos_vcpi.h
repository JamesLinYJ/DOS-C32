/* SPDX-License-Identifier: GPL-2.0-only */
/* Typed platform boundary for the VCPI 1.0 services carried by INT 67h. */
#ifndef DOSC32_DOS_VCPI_H
#define DOSC32_DOS_VCPI_H

#include "compiler.h"
#include "dos_machine.h"
#include "types.h"

/* Values fixed by the VCPI 1.0 contract, not platform discovery results. */
#define DOS_VCPI_VERSION_MAJOR 1u
#define DOS_VCPI_VERSION_MINOR 0u
#define DOS_VCPI_FIRST_MEGABYTE_PAGES 256u

enum dos_vcpi_platform_status {
	DOS_VCPI_PLATFORM_OK = 0,
	DOS_VCPI_PLATFORM_UNSUPPORTED,
	DOS_VCPI_PLATFORM_FAULT,
	DOS_VCPI_PLATFORM_UNCERTAIN
};

enum dos_vcpi_handoff_kind {
	DOS_VCPI_HANDOFF_GET_INTERFACE = 1,
	DOS_VCPI_HANDOFF_ENTER_PROTECTED,
	DOS_VCPI_HANDOFF_RETURN_TO_V86
};

/*
 * Pointer-free request passed to the protected-execution owner.  DE01 uses
 * the two far pointers; real-mode DE0C uses switch_data_linear.  The backend
 * must validate and copy every guest structure before changing CPU state.
 */
struct dos_vcpi_handoff_request {
	uint32_t kind;
	uint32_t caller_mode;
	uint16_t page_table_segment;
	uint16_t page_table_offset;
	uint16_t descriptor_segment;
	uint16_t descriptor_offset;
	uint32_t switch_data_linear;
	uint8_t reserved[12];
} __aligned(8);

enum dos_vcpi_handoff_status {
	/* DE01 completed synchronously and its register outputs are valid. */
	DOS_VCPI_HANDOFF_COMPLETED = 0,
	/* DE0C transferred execution and must not return through the old frame. */
	DOS_VCPI_HANDOFF_TRANSFERRED,
	DOS_VCPI_HANDOFF_UNSUPPORTED,
	DOS_VCPI_HANDOFF_FAULT,
	DOS_VCPI_HANDOFF_UNCERTAIN
};

typedef enum dos_vcpi_platform_status (*dos_vcpi_translate_low_page_fn)(
	kernel_object_handle_t context, uint16_t page,
	uint64_t *physical_address);
typedef enum dos_vcpi_platform_status (*dos_vcpi_read_cr0_fn)(
	kernel_object_handle_t context, uint32_t *virtual_cr0);
typedef enum dos_vcpi_platform_status (*dos_vcpi_query_pic_mappings_fn)(
	kernel_object_handle_t context, uint8_t *master_base,
	uint8_t *slave_base);
typedef enum dos_vcpi_platform_status (*dos_vcpi_set_pic_mappings_fn)(
	kernel_object_handle_t context, uint8_t master_base,
	uint8_t slave_base);
typedef enum dos_vcpi_handoff_status (*dos_vcpi_handoff_fn)(
	kernel_object_handle_t context,
	const struct dos_vcpi_handoff_request *request,
	struct dos_cpu_state *state);

struct dos_vcpi_platform_ops {
	dos_vcpi_translate_low_page_fn translate_low_page;
	dos_vcpi_read_cr0_fn read_virtual_cr0;
	dos_vcpi_query_pic_mappings_fn query_pic_mappings;
	dos_vcpi_set_pic_mappings_fn set_pic_mappings;
	dos_vcpi_handoff_fn handoff;
};

static_assert_expression(sizeof(struct dos_vcpi_handoff_request) == 32u,
			 "VCPI handoff request layout changed");
static_assert_expression(__alignof__(struct dos_vcpi_handoff_request) == 8u,
			 "VCPI handoff request alignment changed");

#endif

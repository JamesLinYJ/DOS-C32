/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * DOS guest-machine boundary
 *
 * Compatibility contract: expose a 16-bit x86 register and memory model without
 *                 tying DOS services to VM86 or an instruction interpreter
 * Safety changes: guest addresses are integers, all transfers are bounded,
 *                 and port access is policy-mediated
 */
#ifndef DOSC32_DOS_MACHINE_H
#define DOSC32_DOS_MACHINE_H

#include "address.h"
#include "compiler.h"
#include "types.h"

#define DOS_REAL_MODE_ADDRESS_LIMIT 0x00110000u
#define DOS_A20_WRAP_ADDRESS 0x00100000u
#define DOS_GUEST_32_ADDRESS_LIMIT 0x100000000ull

enum dos_cpu_mode {
	DOS_CPU_REAL16 = 0,
	DOS_CPU_VM86,
	DOS_CPU_PROTECTED16,
	DOS_CPU_PROTECTED32
};

/*
 * Full-width general registers preserve 386 DOS-program behavior.  DOS calls
 * normally consume their low halves, but the upper halves must not be lost at
 * an interrupt boundary.
 */
struct dos_cpu_state {
	uint32_t eax;
	uint32_t ebx;
	uint32_t ecx;
	uint32_t edx;
	uint32_t esi;
	uint32_t edi;
	uint32_t ebp;
	uint32_t esp;
	uint32_t eip;
	uint32_t eflags;
	uint16_t cs;
	uint16_t ss;
	uint16_t ds;
	uint16_t es;
	uint16_t fs;
	uint16_t gs;
	/* Stable value-object field; validate against enum dos_cpu_mode. */
	uint32_t mode;
};

static inline bool dos_cpu_mode_value_is_valid(uint32_t mode)
{
	return mode == (uint32_t)DOS_CPU_REAL16 ||
	       mode == (uint32_t)DOS_CPU_VM86 ||
	       mode == (uint32_t)DOS_CPU_PROTECTED16 ||
	       mode == (uint32_t)DOS_CPU_PROTECTED32;
}

static_assert_expression(sizeof(struct dos_cpu_state) == 56,
			 "CPU state must be data-model independent");
static_assert_expression(__builtin_offsetof(struct dos_cpu_state, mode) == 52,
			 "CPU mode offset changed");

#define DOS_EFLAGS_CF (1u << 0)
#define DOS_EFLAGS_RESERVED_ONE (1u << 1)
#define DOS_EFLAGS_PF (1u << 2)
#define DOS_EFLAGS_AF (1u << 4)
#define DOS_EFLAGS_ZF (1u << 6)
#define DOS_EFLAGS_SF (1u << 7)
#define DOS_EFLAGS_TF (1u << 8)
#define DOS_EFLAGS_IF (1u << 9)
#define DOS_EFLAGS_DF (1u << 10)
#define DOS_EFLAGS_OF (1u << 11)
#define DOS_EFLAGS_IOPL (3u << 12)
#define DOS_EFLAGS_NT (1u << 14)
#define DOS_EFLAGS_VM (1u << 17)

static inline uint16_t dos_register_low16(uint32_t value)
{
	return (uint16_t)(value & 0xffffu);
}

static inline uint8_t dos_register_low8(uint32_t value)
{
	return (uint8_t)(value & 0xffu);
}

static inline uint8_t dos_register_high8(uint32_t value)
{
	return (uint8_t)((value >> 8) & 0xffu);
}

static inline void dos_register_set_low16(uint32_t *value, uint16_t low)
{
	*value = (*value & 0xffff0000u) | (uint32_t)low;
}

static inline void dos_register_set_low8(uint32_t *value, uint8_t low)
{
	*value = (*value & 0xffffff00u) | (uint32_t)low;
}

static inline void dos_register_set_high8(uint32_t *value, uint8_t high)
{
	*value = (*value & 0xffff00ffu) | ((uint32_t)high << 8);
}

static inline dos_linear_address_t
dos_far_to_linear(uint16_t segment, uint16_t offset, bool a20_enabled)
{
	dos_linear_address_t linear =
	    ((dos_linear_address_t)segment << 4) + (dos_linear_address_t)offset;

	return a20_enabled ? linear : linear & 0x000fffffu;
}

enum dos_machine_status {
	DOS_MACHINE_OK = 0,
	DOS_MACHINE_INVALID_ARGUMENT,
	DOS_MACHINE_ADDRESS_FAULT,
	DOS_MACHINE_IO_DENIED,
	DOS_MACHINE_IO_FAULT,
	DOS_MACHINE_ROLLBACK_FAILED,
	DOS_MACHINE_UNSUPPORTED,
	DOS_MACHINE_STOPPED
};

enum dos_io_width {
	DOS_IO_WIDTH_8 = 1,
	DOS_IO_WIDTH_16 = 2,
	DOS_IO_WIDTH_32 = 4
};

struct dos_machine;

struct dos_machine_ops {
	enum dos_machine_status (*read_memory)(
	    kernel_object_handle_t context, dos_linear_address_t linear_address,
	    void *destination, size_t destination_capacity, size_t count);
	enum dos_machine_status (*write_memory)(
	    kernel_object_handle_t context, dos_linear_address_t linear_address,
	    const void *source, size_t source_capacity, size_t count);
	enum dos_machine_status (*read_port)(kernel_object_handle_t context,
					     uint16_t port,
					     enum dos_io_width width,
					     uint32_t *value);
	enum dos_machine_status (*write_port)(kernel_object_handle_t context,
					      uint16_t port,
					      enum dos_io_width width,
					      uint32_t value);
	enum dos_machine_status (*set_a20)(kernel_object_handle_t context,
					   bool enabled);
	enum dos_machine_status (*query_a20)(kernel_object_handle_t context,
					     bool *enabled);
};

struct dos_machine {
	const struct dos_machine_ops *ops;
	kernel_object_handle_t context;
	uint64_t address_limit;
	bool a20_enabled;
	/* Sticky after an A20 transition whose final hardware state is unknown. */
	uint8_t poisoned;
};

enum dos_machine_status dos_machine_configure(struct dos_machine *machine,
					      const struct dos_machine_ops *ops,
					      kernel_object_handle_t context,
					      uint64_t address_limit,
					      bool a20_enabled) __must_check;

enum dos_machine_status dos_machine_read(const struct dos_machine *machine,
					 dos_linear_address_t linear_address,
					 void *destination,
					 size_t destination_capacity,
					 size_t count) __must_check;
enum dos_machine_status dos_machine_write(const struct dos_machine *machine,
					  dos_linear_address_t linear_address,
					  const void *source,
					  size_t source_capacity,
					  size_t count) __must_check;
/*
 * Replace one guest range transactionally using caller-owned rollback space.
 * On a failed replacement, the original bytes are restored when possible.
 * This is a data transaction, not an execution lock: the caller/backend must
 * already exclude concurrent guest execution and interrupt observation.
 */
enum dos_machine_status
dos_machine_replace(const struct dos_machine *machine,
		    dos_linear_address_t linear_address, const void *source,
		    size_t source_capacity, void *rollback,
		    size_t rollback_capacity, size_t count) __must_check;
/* Validate every physical chunk produced by 16-bit offset and A20 wrapping. */
enum dos_machine_status
dos_machine_validate_far(const struct dos_machine *machine, uint16_t segment,
			 uint16_t offset, size_t count) __must_check;
enum dos_machine_status dos_machine_read_far(const struct dos_machine *machine,
					     uint16_t segment, uint16_t offset,
					     void *destination,
					     size_t destination_capacity,
					     size_t count) __must_check;
enum dos_machine_status dos_machine_write_far(const struct dos_machine *machine,
					      uint16_t segment, uint16_t offset,
					      const void *source,
					      size_t source_capacity,
					      size_t count) __must_check;
/*
 * Transactional replacement over the same segmented/A20 chunk model.  The
 * caller/backend must provide the same execution serialization required by
 * dos_machine_replace().
 */
enum dos_machine_status
dos_machine_replace_far(const struct dos_machine *machine, uint16_t segment,
			uint16_t offset, const void *source,
			size_t source_capacity, void *rollback,
			size_t rollback_capacity, size_t count) __must_check;
enum dos_machine_status dos_machine_read_port(const struct dos_machine *machine,
					      uint16_t port,
					      enum dos_io_width width,
					      uint32_t *value) __must_check;
enum dos_machine_status
dos_machine_write_port(const struct dos_machine *machine, uint16_t port,
		       enum dos_io_width width, uint32_t value) __must_check;
/*
 * The machine is sticky-stopped if the backend does not complete an A20
 * transition normally: its physical wrap state can no longer be proven.
 */
enum dos_machine_status dos_machine_set_a20(struct dos_machine *machine,
					    bool enabled) __must_check;
/* Query the backend-owned state; an unprovable result stops the machine. */
enum dos_machine_status dos_machine_query_a20(struct dos_machine *machine,
					      bool *enabled) __must_check;

#endif

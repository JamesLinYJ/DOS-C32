/* SPDX-License-Identifier: GPL-2.0-only */
/* Safe access to the simulated real-mode interrupt vector table. */
#ifndef DOSC32_DOS_VECTORS_H
#define DOSC32_DOS_VECTORS_H

#include "compiler.h"
#include "dos_abi.h"
#include "dos_machine.h"
#include "types.h"

#define DOS_INTERRUPT_TERMINATE 0x22u
#define DOS_INTERRUPT_CONTROL_C 0x23u
#define DOS_INTERRUPT_CRITICAL_ERROR 0x24u
#define DOS_INTERRUPT_VECTOR_BYTES 4u

enum dos_vector_status {
	DOS_VECTOR_OK = 0,
	DOS_VECTOR_INVALID_ARGUMENT,
	DOS_VECTOR_MACHINE_FAULT,
	DOS_VECTOR_ROLLBACK_FAILED
};

enum dos_vector_status
dos_vector_get(const struct dos_machine *machine, uint8_t vector,
	       struct dos_far_pointer16 *address) __must_check;
enum dos_vector_status
dos_vector_set(const struct dos_machine *machine, uint8_t vector,
	       struct dos_far_pointer16 address) __must_check;

#endif

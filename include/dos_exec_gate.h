/* SPDX-License-Identifier: GPL-2.0-only */
/* Production serialization gate for one DOS EXEC namespace. */
#ifndef DOSC32_DOS_EXEC_GATE_H
#define DOSC32_DOS_EXEC_GATE_H

#include "compiler.h"
#include "dos_exec_observer.h"
#include "types.h"

enum dos_exec_gate_status {
	DOS_EXEC_GATE_READY = 0,
	DOS_EXEC_GATE_INVALID_ARGUMENT,
	DOS_EXEC_GATE_INVALID_STATE,
	DOS_EXEC_GATE_POISONED
};

/*
 * DOS has one process-global CurrentPDB/DTA/SFT namespace.  This gate gives
 * its multi-step EXEC rewrite one exclusive generation without retaining a
 * native pointer in a transaction.  A quarantine is sticky by design.
 */
enum dos_exec_gate_status dos_exec_gate_initialize(
	kernel_object_handle_t adapter_identity,
	kernel_object_handle_t adapter_context) __must_check;
const struct dos_exec_observer_ops *dos_exec_gate_ops(void) __must_check;
kernel_object_handle_t dos_exec_gate_context(void) __must_check;
bool dos_exec_gate_is_quarantined(void) __must_check;

#endif

/* SPDX-License-Identifier: GPL-2.0-only */
/* Checked resizing of the DOS job file table (JFT). */
#ifndef DOSC32_DOS_JFT_H
#define DOSC32_DOS_JFT_H

#include "compiler.h"
#include "dos_machine.h"
#include "dos_memory.h"
#include "types.h"

#define DOS_JFT_MINIMUM_HANDLES 20u
#define DOS_JFT_MAXIMUM_HANDLES 0xfffeu
#define DOS_JFT_UNUSED 0xffu

/*
 * DOS-visible failures are deliberately distinct from protection failures.
 * The INT 21h boundary maps the first three errors to AX=4, AX=8 and AX=1.
 */
enum dos_jft_status {
	DOS_JFT_OK = 0,
	DOS_JFT_TOO_MANY_OPEN_FILES,
	DOS_JFT_NOT_ENOUGH_MEMORY,
	DOS_JFT_INVALID_FUNCTION,
	DOS_JFT_INVALID_ARGUMENT,
	DOS_JFT_INVALID_STATE,
	DOS_JFT_ARENA_FAULT,
	DOS_JFT_MACHINE_FAULT,
	DOS_JFT_MACHINE_POISONED
};

/*
 * Safe C implementation of INT 21h AH=67h JFT resizing.
 * requested_handles is clamped to twenty internally.  The dispatcher must
 * preserve the guest-visible BX value because the saved caller value is restored,
 * so $ExtHandle's internal clamp is not observable.  Equal-size requests are
 * true no-ops and do not inspect the JFT pointer or arena.
 *
 * The caller owns the exclusive DOS execution interval for the complete call.
 * Guest execution, IRQ observers and other arena/JFT mutations must remain
 * quiesced.  The PSP and JFT are the only sources of process-visible truth;
 * this interface retains no guest pointer or mirrored handle count.
 */
enum dos_jft_status dos_jft_resize_checked(
	struct dos_memory_arena *arena, const struct dos_machine *machine,
	uint16_t current_psp, uint16_t requested_handles) __must_check;

#endif

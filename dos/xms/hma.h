/* SPDX-License-Identifier: GPL-2.0-only */
/* Private single-lease High Memory Area state machine. */
#ifndef DOSC32_DOS_XMS_HMA_H
#define DOSC32_DOS_XMS_HMA_H

#include "dos_xms.h"

enum dos_xms_status dos_xms_hma_report_version(
	struct dos_xms_manager *manager, const struct dos_machine *machine,
	struct dos_cpu_state *state) __must_check;
enum dos_xms_status dos_xms_hma_request(
	struct dos_xms_manager *manager, const struct dos_machine *machine,
	struct dos_cpu_state *state) __must_check;
enum dos_xms_status dos_xms_hma_release(
	struct dos_xms_manager *manager, const struct dos_machine *machine,
	struct dos_cpu_state *state) __must_check;

#endif

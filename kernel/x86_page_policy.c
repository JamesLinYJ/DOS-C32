// SPDX-License-Identifier: GPL-2.0-only
/* Pure access policy shared by i386 paging construction and host tests. */
#include "x86_paging.h"

#include "x86_memory_map.h"

enum x86_page_access x86_boot_page_access(uint32_t linear_address)
{
	if (linear_address < X86_LEGACY_VIDEO_BASE)
		return X86_PAGE_GUEST_READ_WRITE;
	/* Native VGA memory is granted only with the foreground display lease. */
	if (linear_address < X86_DOS_VIDEO_LIMIT)
		return X86_PAGE_SUPERVISOR_READ_WRITE;
	/* Firmware/option-ROM bytes stay visible and executable, but a guest must
	 * not gain write authority merely because a board may implement shadowing. */
	if (linear_address < X86_LEGACY_ROM_LIMIT)
		return X86_PAGE_GUEST_READ_ONLY;
	/* The A20-enabled real-mode aperture includes the HMA. */
	if (linear_address < X86_REAL_MODE_LINEAR_LIMIT)
		return X86_PAGE_GUEST_READ_WRITE;
	return X86_PAGE_SUPERVISOR_READ_WRITE;
}

bool x86_boot_page_is_present(const struct x86_boot_info *boot_info,
			      uint32_t linear_address)
{
	uint32_t page = linear_address & ~(X86_PAGE_BYTES - 1u);

	/* E820 does not consistently enumerate conventional RAM, VGA, or ROM, so
	 * those architectural legacy apertures remain mapped by type-specific
	 * policy.  HMA is different: it is ordinary RAM above 1 MiB and must have
	 * firmware-proven backing before XMS can publish it. */
	if (page < X86_LEGACY_ROM_LIMIT)
		return true;
	if (page >= X86_BOOT_IDENTITY_LIMIT)
		return false;
	return x86_memory_map_range_is_usable(boot_info, page,
					      X86_PAGE_BYTES);
}

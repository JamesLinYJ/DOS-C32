# SPDX-License-Identifier: GPL-2.0-only
#
# XMS policy defaults.  Architectural HMA addresses and XMS register widths
# remain ABI constants; this file owns deployment policy that may vary.

# Minimum AH=01h request accepted for the single HMA lease, in bytes.
# Zero matches the default XMS/Himem behavior; deployments may raise it.
DOS_XMS_HMA_MINIMUM_BYTES ?= 0

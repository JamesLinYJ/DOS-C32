/* SPDX-License-Identifier: GPL-2.0-only */
/* Deliberate guest #UD used only to verify the VM86 protection display. */
.code16
.section .text
.global _start

_start:
	ud2

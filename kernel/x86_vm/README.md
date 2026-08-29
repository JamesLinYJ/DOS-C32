<!-- SPDX-License-Identifier: GPL-2.0-only -->

# Internal x86 virtual-machine manager

This directory owns architecture-specific execution containment for legacy DOS
and Windows code. It is an internal kernel boundary, not a DOS-visible
personality and not a user-facing product mode.

The naming layers are deliberate:

- `x86_vm` is the subsystem which owns virtual-machine protection policy.
- `x86_guest_*` names per-guest state, address spaces and fault snapshots.
- `x86_vm86_*` names only the direct Intel VM86 hardware backend.
- `chipset/` owns isolated guest PIC, PIT, RTC and i8042 port-state machines;
  it never forwards those legacy-controller operations to native hardware.
  The i8042 keeps discovered keyboard/mouse identity in its owner instance,
  accepts input through a generation-bound source, and exposes acknowledgeable
  IRQ/A20 events rather than calling the native PIC or reset controls.
- `irq/` owns explicit native descriptors/actions and generation-bound guest
  producers/routes. The guest PIC alone resolves the visible vector; no native
  vector or same-number IRQ fallback crosses into application state.
- `platform/native_input.c` is a native i8042 backend for the
  kernel-wide `kernel/input/serio/` bus. Platform discovery supplies presence,
  ports, status masks, identities, queues and the read callback. Its IRQ action
  returns HANDLED/NONE/FAULT without EOI or guest-IRQ reflection. Optional
  process-context writes use an explicit bounded input-buffer wait; the backend
  does not probe translation or rewrite the controller command byte.
- future protected-mode and interpreter backends remain peers of `vm86.c`.

The DOS personality, I/O Manager and application UI remain outside this
directory. A backend emits typed execution events and never implements private
INT 21h behavior. Normal application text must not expose VM, guest, host or
backend terminology; compile-gated serial diagnostics may do so.

The direct VM86 backend keeps physical IF enabled for supervisor scheduling
and tracks application IF separately.  Pending modeled PIC state survives
logical CLI and all execution exits.  The present 1193-input-clock native PIT
quantum is an uncalibrated scheduler policy, not wall-clock truth; see
`docs/X86-VM-RESOURCE-MODEL.md` for the exact current limits.

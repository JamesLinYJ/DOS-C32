<!-- SPDX-License-Identifier: GPL-2.0-only -->

[简体中文](README.zh-CN.md)

# DOS-C32

DOS-C32 is an in-progress 32-bit operating system that provides MS-DOS
compatibility with a protected i386 kernel. It boots through legacy BIOS and
runs unmodified 16-bit DOS COM/MZ applications behind a typed execution
boundary. UEFI and application-specific compatibility hacks are deliberately
out of scope.

The repository is licensed as `GPL-2.0-only`. Microsoft Diagnostics, Windows
files, and other proprietary test fixtures are not included.

## Current status

The image boots through SeaBIOS, enters a protected 32-bit i386 kernel, mounts
its FAT16 volume, starts the built-in command environment, and exercises the
production VM86 service path in the boot test. The typed DOS ABI, memory,
PSP/JFT/SFT, COM/MZ loading, relocation, EXEC0/EXEC1 preparation, and a subset
of INT 21h are implemented and covered by host and boot tests.

This is not yet a complete DOS replacement. The production shell does not yet
cover every COMMAND builtin or DOS program lifecycle; some DOS and BIOS
services, IRQ/device coverage, TSR behavior, EMS/VCPI, and Windows 3.2
protected-mode execution remain incomplete. Real COM/MZ programs already use
the production EXEC and x86 guest path rather than a test-only loader.

The evidence-based status is maintained in
[docs/COMPATIBILITY.md](docs/COMPATIBILITY.md). The ordered implementation plan
is [PLAN.md](PLAN.md).

## Build and verify

Required tools include GCC with 32-bit support, GNU binutils, `dosfstools`,
`mtools`, and QEMU's i386 system emulator.

```sh
make image
make check
make run
```

QEMU boots visibly on macOS and defaults to 256 MiB. Override the machine size
without changing kernel code, for example `QEMU_MEMORY_MIB=64 make run`. The
configured early mapping aperture is only a capacity ceiling; each boot derives
the mapped high-water mark, usable page pool, reserved holes, and XMS report
from the validated BIOS E820 map.

The output image is `build/msdos-c32.img`. `make check` also dual-compiles the
portable boundaries for 32- and 64-bit data models, rejects forbidden APIs and
acceptance-program fingerprints, validates the FAT16 image, and boots an
isolated self-test image in QEMU.

For source-level debugging:

```sh
make run-gdb
# In another terminal:
gdb -x debug/kernel.gdb
```

See [docs/DEBUGGING.md](docs/DEBUGGING.md) for logs and breakpoint details.

## Design rules

- MS-DOS program-visible behavior is the compatibility contract. Internal
  design choices must not replace its ABI or error semantics.
- Guest pointers remain explicit 16:16 or fixed-width guest-linear values.
  Kernel identities, offsets, and persistent handles are 64-bit values; native
  pointers never enter a DOS ABI structure.
- x86 guest hardware access is authorized per resource. Shared or dangerous I/O is
  mediated, and program names never widen access.
- The internal virtual-machine manager is transparent to normal DOS and
  Windows software. It does not add a private guest-visible ABI, and ordinary
  user messages describe applications and system protection rather than
  virtualization backends.
- Bounded APIs, checked arithmetic, immutable identities, generation handles,
  reversible preparation, and fail-closed poison states are required at trust
  boundaries.

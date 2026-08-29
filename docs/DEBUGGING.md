<!-- SPDX-License-Identifier: GPL-2.0-only -->

# Kernel and x86 virtual-machine debugging

The default `make image` build is the production image and sets
`CONFIG_BOOT_SELFTESTS=0` and `CONFIG_X86_VM_ACCEPTANCE_DIAGNOSTICS=0`. Host
tests are separate executables under `tests/`; the direct VM86 integration
probe is compiled only by `make boot-check`
into `build/boot-selftest/`. `make production-audit` rejects known self-test
symbols and acceptance-trace text in the default kernel, so debugging fixtures
cannot silently become runtime dependencies.

For a bounded compatibility investigation, build a separate diagnostic image:

```sh
make acceptance-diagnostic-image
```

This sets `CONFIG_X86_VM_ACCEPTANCE_DIAGNOSTICS=1` only in
`build/acceptance-diagnostic/`. It enables serial-only I/O path receipts,
direct-VM86 call frames, and a fixed-size recent-interrupt ring. None of that
code or data is present in the production image. These internal terms never
become a DOS-visible virtualization ABI or normal user-facing message.

DOS-C32 uses QEMU's i386 machine with its default SeaBIOS firmware.  DOSBox is
not part of the kernel-debugging path, and no UEFI firmware or JIT backend is
required.

Build the image and start the virtual machine paused:

```sh
make run-gdb
```

Then connect from a second terminal:

```sh
gdb -x debug/kernel.gdb
```

The debugger first sees the reset/BIOS address space.  The command file places
breakpoints at the low staging entry, the post-copy high entry, and `kmain`.
The ELF symbols deliberately describe the runtime kernel at `0x00400000`; the
small Legacy-BIOS staging entry remains linked at `0x00010000`.

QEMU writes debug-port output to `build/debugcon.log` and CPU interrupt/error
traces to `build/qemu.log`.  Those traces are diagnostic evidence only and may
not define DOS-visible behavior.

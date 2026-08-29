<!-- SPDX-License-Identifier: GPL-2.0-only -->

# DOS-C32 implementation plan

Status date: 2026-08-30

Work is complete only when the relevant automated tests pass and the real boot
or application path demonstrates the required behavior. A compiled interface
or an opening setup screen is not acceptance by itself.

## Product goal

DOS-C32 is a protected 32-bit i386 operating system with MS-DOS compatibility.
It boots through legacy BIOS, runs unmodified 16-bit DOS COM and MZ programs,
and keeps normal applications unaware of the internal execution boundary. The
primary acceptance target is an installed Microsoft Windows 3.2 system that
reaches and operates in 386 Enhanced Mode.

The shipped system is genuinely multi-file. `IO.SYS` loads the independent
`DOSKRNL.SYS` payload from FAT, and the kernel starts the independent native
`COMMAND.COM`. New shipped components are added only when they have a bounded
loading ABI, independent lifecycle, explicit ownership, and tested rollback.

## Non-negotiable rules

1. MS-DOS-visible calls, structures, flags, errors, ordering, fixed-width
   wrapping, and application behavior are compatibility requirements.
2. Implementation safety may contain a failure internally but must not invent
   a different DOS-visible result.
3. Compatibility is implementation-wide. No branch may recognize a program by
   name, hash, header accident, instruction sequence, entry point, or timing.
4. New production C is freestanding GNU11 and warning-clean. C++ is permitted
   only with explicit ownership, no exceptions or RTTI, and no implicit heap or
   order-dependent global lifetime. Assembly remains limited to exact
   architectural entry and transition operations.
5. Guest, disk, and DOS ABI fields use exact-width integers. Canonical internal
   capacities, offsets, LBAs, time values, generations, and address-space
   limits use explicit 64-bit types, with checked narrowing at the ABI boundary.
6. Untrusted buffers carry readable or writable capacities. Arithmetic is
   checked before narrowing, and disk data is decoded bytewise.
7. Multi-object changes validate and acquire into private state, publish only
   after all preconditions succeed, and unwind in reverse order. Uncertain
   restoration poisons or quarantines the affected owner.
8. Native pointers never become ABI handles or proof of authority. Registries
   use stable numeric handles with immutable lifetime identities and
   non-wrapping generations.
9. Read, write, execute, and native pass-through are distinct capabilities.
   A successful read never grants a write.
10. Actual RAM, device, disk, geometry, and display facts come from validated
    runtime discovery. Compiled bounds are capacities, not detected hardware.
11. Filesystem policy remains independent from the block driver and DOS ABI.
    The I/O Manager exposes typed volume, object, directory, handle, and
    transaction operations; a concrete format remains private to its driver.
12. Hard-IRQ, early-boot, fault, panic, and rollback paths do not allocate or
    block. Dynamic storage becomes available only after its owner is published.
13. Proprietary application and installation fixtures remain outside version
    control and are used only for acceptance testing.

## Compatibility architecture

```text
 native 32-bit program    16-bit COM/MZ       Windows 3.2
          |                    |                   |
          +---------- execution backend ----------+
                              |
                       machine boundary
              CPU state | memory | IRQ | I/O
                              |
                       DOS personality
       INT 20/21/25/26/27/2F, PSP, MCB, JFT/SFT, devices
                              |
               I/O Manager | console | clock
                              |
                 filesystem and block drivers
                              |
                       i386 platform
```

The DOS personality owns visible DOS behavior. The machine boundary owns guest
registers, memory, interrupts, and port access. Execution backends select how a
program runs without changing what an interrupt service means.

Normal real-mode programs use hardware VM86 where possible. Instructions that
cannot execute safely are trapped and handled by typed machine services. The
same DOS service implementation is shared by hardware-assisted and emulated
execution. Windows protected modes use a controlled transition or CPU model
behind the same boundary; real control registers and native interrupt
controllers are never handed over.

## EXEC transaction contract

INT 21h/4Bh decodes the DOS ABI and delegates to one transaction owner. The
required preparation order is:

1. validate the requested operation and open the executable;
2. validate environment selection and reserve its storage when applicable;
3. classify COM/MZ and reserve the load range;
4. read the unpublished image and apply checked relocations;
5. prepare inherited JFT/SFT state as one reversible batch;
6. stage PSP, FCB overlays, command tail, initial registers, and stack;
7. preflight MCB ownership, interrupt vectors, result writes, DTA,
   `CurrentPDB`, and backend handoff;
8. publish with `CurrentPDB` last, or unwind every acquisition in reverse.

The transaction uses fixed generation-owned slots and never persists native
pointers. Guest writes are journaled. Guest execution and IRQ observers remain
excluded from the first unpublished image write through complete publication
or discard. No filesystem, device, or network callback runs during the final
serialized publication step.

EXEC0 becomes irreversible only after the child executes its first
instruction. EXEC1 returns with the child state published. EXEC3 shares the
validated file and image prefix but does not allocate an environment or child
process and does not publish process-global state.

## Storage architecture

The I/O Manager owns mounted volumes, named objects, open files, directory
searches, devices, and transactions. DOS adapters convert INT 21h/CDS/DPB/JFT/
SFT requests to these generic operations. Filesystem drivers expose only
`mount`, `open`, `read`, `write`, `allocate`, `enumerate`, `flush`, and teardown
semantics required by the common interface.

FAT-specific cluster hints, FSInfo, table-sector caches, mirror handling, and
directory layout remain private. Cache entries bind to immutable volume
identity and media generation. A mutation publishes the directory entry last,
records before-images, rolls back in reverse, and quarantines the volume if
restoration cannot be proved.

Future exFAT, NTFS, ISO 9660, UDF, ReFS, ext2, and memory-backed drivers must
fit the same generic boundary. Read-only support is reported honestly and is
never simulated through a different on-disk format.

## Hardware and protection

Each application environment receives an immutable resource snapshot. Private
RAM may be directly mapped. Firmware ROM, display apertures, PIC, PIT, RTC,
i8042, DMA, disks, reset, and A20 are independently modeled or mediated.
Unknown hardware facts grant no access. Reads and writes have separate policy;
safe absent-device reads may return realistic bus values, while dangerous or
stateful operations cross an owned device model.

The fault UI is presentation only. A recoverable application fault terminates
or discards that execution context, restores display palette and attributes,
clears the screen, and returns to a clean COMMAND session. A kernel integrity
failure remains fail-closed.

## Milestones

### 1. Toolchain, ABI, and safe runtime

- [x] Establish freestanding GNU11 i386 and portable 64-bit data-model builds.
- [x] Provide checked memory, string, formatting, parsing, overflow, and 64-bit
      arithmetic helpers.
- [x] Reject forbidden unbounded APIs and compiler-runtime surprises.
- [ ] Finish the native Ring-3 C32 image ABI and program runtime.

### 2. Boot, paging, and system packaging

- [x] Build the independent `IO.SYS`, `DOSKRNL.SYS`, and `COMMAND.COM` system
      files into a FAT16 image.
- [x] Boot through MBR/VBR and move the kernel into protected high memory.
- [x] Derive mapped and allocatable RAM from validated E820 data.
- [ ] Add equivalent floppy/superfloppy packaging without changing kernel
      ownership or storage semantics.

### 3. DOS personality and process model

- [x] Implement typed DOS registers, pointers, PSP/PDB, MCB, JFT/SFT, DTA,
      vectors, errors, and the tested INT 21h subset.
- [x] Implement COM/MZ planning, loading, relocation, environment, stack,
      inheritance, transaction publication, and executable backend ownership.
- [ ] Complete EXEC3 resident writes and remaining INT 21h services.
- [ ] Complete termination, TSR, parent restoration, and COMMAND scheduling.

### 4. I/O Manager and filesystems

- [x] Publish generic device, volume, file, directory-search, and transaction
      interfaces with generation-owned handles.
- [x] Mount and read FAT12/16/32 and perform tested FAT16 directory mutation.
- [x] Bind the boot ATA device through validated firmware and IDENTIFY facts.
- [ ] Complete FAT12/FAT32 mutation, directory expansion, caching, and injected
      failure coverage.
- [ ] Implement additional filesystem drivers without widening the common ABI.

### 5. Machine execution and virtual hardware

- [x] Enter and recover hardware VM86 with typed events and precise state.
- [x] Implement software interrupt reflection, virtual flags, scalar port I/O,
      firmware-page isolation, guest PIC/PIT, i8042, and 8237A register state.
- [x] Implement XMS runtime totals, allocation, movement, resizing, HMA, and A20
      ownership for the tested functions.
- [ ] Integrate native keyboard IRQ capture, decoding, focus, virtual PS/2
      encoding, and guest IRQ delivery.
- [ ] Complete BIOS services, string I/O, MMIO handling, EMS/VCPI/DPMI, UMB,
      and protected-mode transitions.

### 6. COMMAND and user environment

- [x] Provide the current builtin set, bounded current-directory handling,
      modern messages, and generic current-directory/PATH executable search.
- [ ] Complete traditional COMMAND syntax, builtins, batch behavior,
      redirection, pipes, environment behavior, and child lifecycle.
- [ ] Package optional drivers and utilities as real independently loaded
      components only when their module boundaries are complete.

### 7. Windows 3.2 acceptance

- [x] Build C: and merged installation VHD fixtures outside version control.
- [ ] Complete installation, reboot into the installed system, and validate
      keyboard, display, disk, timing, DOS services, XMS, and error recovery.
- [ ] Reach and operate Standard Mode without private application exceptions.
- [ ] Reach and operate 386 Enhanced Mode with virtualized control state,
      descriptors, paging, interrupts, VMM/VxD services, and nested VM86.

## Verification policy

For every behavior change:

1. run the smallest relevant host test;
2. run both supported data-model compile checks;
3. build the production image and run image/symbol audits;
4. boot visibly in QEMU when the machine or application boundary changes;
5. use serial logs, monitor state, registers, and decoded text memory before
   taking screenshots;
6. update `docs/COMPATIBILITY.md` with exact tested scope and remaining work.

Acceptance requires an installed Windows 3.2 system operating in 386 Enhanced
Mode on DOS-C32. A booting shell, a setup window, or isolated unit coverage is
an intermediate milestone only.

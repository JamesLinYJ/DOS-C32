<!-- SPDX-License-Identifier: GPL-2.0-only -->

# DOS-C32 agent rules

These rules apply to the whole repository. More specific `AGENTS.md` files may
add constraints for a subsystem, but may not weaken the architecture, safety,
compatibility, or verification rules below.

## Product contract

DOS-C32 preserves the MS-DOS program-visible contract: INT 21h, PSP/PDB,
COM/MZ execution, VM86 behavior, FAT-visible structures, error codes, flags,
and Windows 3.x compatibility.  Its protection model separates Ring 3 from
Ring 0, treats user pointers as untrusted, mediates hardware access by
resource, and contains application faults without compromising the kernel or
machine.

Internal subsystems use explicit ownership, bounded interfaces, checked
arithmetic, generation-bound handles, acquire-before-publish, reverse unwind,
and auditable failure.  An external operating system or test program must
never replace MS-DOS-visible semantics with a private ABI. Do not fingerprint
an executable, program name, checksum, instruction sequence, or installer to
grant behavior or access.

## Execution architecture

- The shipped DOS is a real multi-file system, not one flat kernel blob copied
  under several compatibility names.  The current minimum system set is
  `IO.SYS`, `DOSKRNL.SYS`, and `COMMAND.COM`: `IO.SYS` loads the independent
  `DOSKRNL.SYS` payload by its FAT directory entry and cluster chain, and the
  kernel starts the independent native `COMMAND.COM`.  Do not reintroduce an
  `MSDOS.SYS` alias unless a demonstrated external compatibility contract
  requires an explicitly documented packaging profile.
- Do not split files merely to increase the component count.  A subsystem may
  become a separate shipped file only when it has a versioned and bounded
  loading ABI, independent initialization and teardown, explicit ownership,
  and a testable failure/rollback boundary.  A separately built file must be
  genuinely loaded and used; duplicate payloads, empty stubs, and decorative
  wrappers are forbidden.  Tightly coupled interrupt dispatch, process and
  memory invariants, and I/O Manager ownership remain in `DOSKRNL.SYS` until a
  real module boundary is proved.  Natural future component candidates include
  XMS, block/filesystem drivers, and optional compatibility services.
- The BIOS/MBR/VBR path, the minimum protected-mode/VM86 entry trampoline,
  and traditional DOS programs are the only permitted 16-bit code.
- Keep boot media independent: partitioned hard disks use MBR to FAT VBR,
  while floppy/superfloppy images boot directly from a FAT VBR. Drive, DPB,
  removable-media, and geometry behavior belongs to per-drive instances, not
  ATA-specific branches in DOS services.
- Keep filesystem policy independent from both the block driver and DOS ABI.
  INT 21h, CDS, DPB, JFT, and SFT code uses a typed mounted-volume interface;
  it must not branch on FAT, NTFS, exFAT, optical media, ATA, or a fixed C:
  drive. FAT12/16/32, exFAT, NTFS, ISO 9660, UDF, and future ReFS support live
  in separate drivers behind that interface. A read-only medium or development
  stage is reported honestly; do not emulate one filesystem through another or
  claim support by recognizing only its volume signature.
- The kernel, filesystem, drivers, COMMAND, new utilities, policy, and service
  implementations use 32-bit protected-mode GNU11 C or explicitly bounded
  freestanding C++ unless an exact x86 architectural operation requires a
  small assembly entry. Do not add a Rust toolchain or Rust production modules
  without a new explicit decision.
- New native programs use the C32 image ABI and run at Ring 3. Kernel services
  cross the checked system-call boundary; DOS COM/MZ children run in VM86.
- Keep assembly entries minimal and policy-free. Parsing, validation,
  allocation, dispatch, recovery, and presentation belong in C.
- An x86 guest fault terminates or recovers the affected execution session,
  never the kernel.
  The full-screen fault UI is a presentation module, not the fault-policy
  owner. Recovery clears the screen, restores the normal VGA palette and
  attributes, and returns to a clean COMMAND session.
- The internal virtual-machine manager is transparent compatibility machinery.
  DOS programs and Windows must not need a DOS-C32-private virtualization ABI,
  and normal user-facing text must describe applications and system protection,
  not VM86, guests, hosts, or backend selection. Compile-gated serial diagnostics
  may name the exact backend and CPU mode.

## Source and module organization

- Every independent subsystem gets its own directory from the beginning.
  Examples: `dos/xms/`, `dos/drive/`, `kernel/x86_vm/`, or `fs/fat/` as the
  relevant tree grows. Do not accumulate unrelated implementation files in a
  parent directory.
- The filesystem-neutral named-I/O layer is the I/O Manager. Its namespace,
  volume, file, directory, device, handle, and transaction contracts live in
  `storage/core/` and use public `iomgr_*` names from `include/iomgr*.h`. It is
  purpose-built for this kernel and does not expose filesystem-specific inode,
  directory-cache, page-cache, or private error APIs. Concrete
  on-disk formats live in `storage/fat/`, `storage/exfat/`, `storage/ntfs/`,
  `storage/iso9660/`, `storage/udf/`, or `storage/refs/`; they never expose
  private on-disk state through the common interface.
- Public cross-module interfaces live in `include/`. Private headers,
  allocators, encoders, and helpers stay in the subsystem directory.
- A subsystem has one clear state owner. Other modules use typed callbacks or
  immutable snapshots; they do not mirror mutable global state.
- Prefer bounded fixed tables, bitmaps, stable numeric handles, and explicit
  generations over native pointers or an early kernel heap. A bitmap is the
  single source of allocation truth when a fixed handle table is used.
- A general kernel heap is permitted after the physical-page allocator and
  heap owner have been published.  Use it where dynamic topology or object
  lifetime genuinely benefits (for example device registries, VFS objects,
  and driver-private state); complexity is acceptable when it implements a
  complete reusable subsystem.  Hard-IRQ, early-boot, panic, and rollback
  paths must not allocate or block.  Heap-backed objects still require typed
  ownership, explicit teardown, stale-reference protection, checked sizes,
  deterministic allocation-failure handling, and preallocated emergency or
  bounded queues where forward progress depends on them.  Native addresses
  never become a persistent ABI handle or proof of authority.
- A compiled capacity is not detected hardware. Page-table/bitmap ceilings,
  fixed handle counts, ABI widths, and architectural page sizes may be bounded
  constants; actual RAM ranges, usable high-water marks, holes, disk geometry,
  and device capabilities must come from validated boot/device discovery and
  remain runtime state. Put platform-policy defaults in the relevant `config/`
  file instead of scattering literals through kernel sources.
- Distinguish protocol constants from machine instances. An 8259 port number,
  FAT field offset, page size, or DOS error code may be fixed by an external
  contract; whether that controller, volume, page, port aperture, or capability
  exists on this boot must come from an owner-backed runtime observation.
- Readability, writability, executability, and native pass-through are separate
  capabilities. A successful read must never silently authorize a write, and
  Ring-0 copy helpers must revalidate the active mapping plus the requested
  direction before dereferencing any guest or user address.
- Frontends such as XMS, EMS, protected execution, and future filesystems must
  consume the runtime owner/query interface. They may not restate a private
  `MEMORY_LIMIT`, capacity, or geometry that can diverge from the underlying
  owner.
- Separate DOS ABI decoding, platform memory/device backends, policy, and UI.
  In particular, XMS discovery/control ABI, XMS handle management, the
  supervisor memory backend, and its three-byte VM86 call gate remain
  independent layers.
- If a module becomes large, split by responsibility inside its directory;
  do not hide coupling by creating generic `util.c` or `misc.c` files.

## Compatibility and test fixtures

- MS-DOS-visible behavior, data layouts, flags, error numbers, ordering, and
  intentional fixed-width wrapping are compatibility requirements.
- Implementation technique must not leak a private process, path, permission,
  device, filesystem, or error ABI into DOS-visible behavior.
- Preserve required SPDX identifiers and license texts.
- Do not copy proprietary Windows, MS-DOS, MSD, or setup binaries into the
  repository. Local proprietary fixtures may be used only for acceptance tests
  and remain outside version control.

## C and ABI requirements

- The libc32 and native C baseline is GNU11, which includes C99 language
  support. Freestanding C++ code must disable exceptions and RTTI and must not
  use implicit heap allocation, hidden ownership, unbounded containers, or
  global constructors/destructors with order-dependent lifetime. Use fixed
  capacity storage or caller-provided buffers and make every ownership and
  destruction boundary explicit. Keep the complete production codebase
  freestanding and warning-clean.
- Keep the execution ABI and the internal data model separate.  The current
  kernel executes as i386 protected-mode code, but internal byte counts, file
  offsets, LBAs, capacities, time values, object generations, and address-space
  limits use explicit 64-bit types unless a narrower hardware or DOS ABI field
  is intrinsic to the contract.  Convert to a 16- or 32-bit ABI field only at
  the boundary, after an explicit range check.  This rule exists so the same C
  model can move to an x86_64 supervisor without redesigning storage or object
  ownership.
- Freestanding operations that an i386 compiler would normally delegate to a
  hosted runtime, including 64-bit division, must live in `libc32` behind a
  checked public interface. Do not leak compiler-helper trap semantics into
  ordinary checked kernel code.
- Guest-visible data uses explicit fixed-width fields and byte encoders.
  Never expose a native pointer, host layout, or implicit compiler padding.
- Decode disk/guest structures bytewise with little-endian helpers. Add
  compile-time size and offset assertions for stable layouts.
- Every untrusted buffer carries a readable or writable capacity. Use the
  safe libc32 APIs and checked arithmetic; do not use source-unbounded string
  or formatting functions.
- Validate a complete operation before publishing mutable state. Roll back in
  reverse order. If restoration cannot be proved, poison/quarantine the
  affected guest or object rather than continuing with uncertain state.
- Reads and writes have different hardware policy. Safe absent-device reads
  may return realistic bus values; dangerous or stateful reads are mediated.
  Writes require per-resource policy and must never reach host hardware merely
  because a guest requested them.

See `docs/CODING_STYLE.md` for the detailed safe-C contract; it is mandatory.

## Testing and debugging

- A build is not acceptance. Run the smallest relevant host tests, build the
  real image, and then run a representative x86-guest/Windows fixture when behavior
  crosses that boundary.
- Prefer serial logs, QEMU monitor state, debugger breakpoints, registers, and
  decoded VGA text memory. Inspect screenshots only when text/state evidence
  cannot determine the graphical result.
- During interactive local work, every QEMU boot must use a visible Cocoa
  display so the user can watch it continuously. Keep serial logging and a
  monitor socket enabled at the same time. Headless mode is permitted only for
  explicit automated/CI tests; scripts must make the display selectable with
  `QEMU_DISPLAY`.
- Add concise, removable diagnostics at subsystem boundaries when needed.
  Do not leave high-volume per-instruction logging in normal builds.
- Do not call a Windows milestone complete because Setup opens. Acceptance for
  this project is an installed Windows 3.2 system that reaches and operates in
  386 Enhanced Mode on DOS-C32.
- Keep `docs/COMPATIBILITY.md` evidence-based. Update a service to complete
  only after tests cover its visible registers, flags, memory, errors, and
  failure boundary.

## Change discipline

- Preserve unrelated user changes in the dirty worktree. Never reset or
  discard them to simplify an implementation.
- Fix root causes and shared DOS semantics. Do not add fixture-specific
  branches, silent fallbacks, or compatibility hacks.
- Use `apply_patch` for source edits. Keep generated images, logs, extracted
  materials, and proprietary fixtures out of commits.
- When a test boot stops, report the exact service/vector/register evidence,
  implement the general contract, add regression coverage, and resume from
  the same acceptance path.

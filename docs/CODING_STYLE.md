<!-- SPDX-License-Identifier: GPL-2.0-only -->

# DOS-C32 coding and safety rules

DOS-C32 preserves MS-DOS behavior and ABI while enforcing strict internal
safety and ownership rules.

## Compatibility contract

1. DOS-visible behavior, data layouts, flags, error numbers, state transitions,
   callback ordering, partial counts, and intentional fixed-width wrapping are
   compatibility requirements.
2. A deliberate incompatibility must be documented next to the code and
   covered by a regression test.
3. Implementation technique may improve safety, ownership, and failure
   containment, but must not change an externally observable DOS result.
4. Internal models use checked arithmetic, bounded APIs, explicit object
   lifecycles, slot-plus-generation handles, reverse unwind, sticky poison, and
   acquire-before-publish. Private process, path, permission, signal,
   filesystem, and error semantics must not leak into DOS-visible behavior.
5. A checked native calculation proves the surrounding range without replacing
   a compatibility-required 8- or 16-bit wrap with rejection or widening.

## Style

The C tree uses tabs at eight columns, puts function opening braces on the next
line, keeps control-block opening braces on the same line, and normally stays
within 80 columns. Names are descriptive lower-case `snake_case`, while macros
and enumerators are upper-case. Deep nesting is split into helpers. Clever
expressions and hidden side effects are rejected. `.clang-format` is a review
aid rather than authority for a mechanical whole-tree rewrite.

DOS terminology is retained where it carries compatibility meaning:
BPB, DPB, CDS, JFT/JFN, SFT, PSP/PDB, DTA, FCB, IFS, and INT 21h function names.
Historical Hungarian-like type casts, `BEGIN`/`END` macros, implicit-int C,
and segment arithmetic are not retained.

## Mandatory safety properties

- No `gets`, `strcpy`, `strncpy`, `strcat`, `strncat`, source-unbounded
  `strscpy`, `sprintf`, `vsprintf`, `scanf`, `sscanf`, `tmpnam`, `mktemp`, or
  `alloca`.
- C strings crossing a trust boundary carry both readable source capacity and
  writable destination capacity.  `strscpy_s` never probes beyond the source
  object, returns an explicitly checked truncation result, rejects overlap,
  and always terminates a valid non-empty destination on truncation.
- Public path and command parsers validate a NUL inside the supplied readable
  extent before parsing, then carry an explicit length/end pointer through
  every interior slice.  A pointer into a larger local array is accompanied by
  its exact remaining capacity rather than the capacity of the original base.
- Raw memory copies are allowed only after the caller proves both ranges.
  Disk data is decoded with explicit little-endian accessors, never by casting
  an untrusted byte buffer to a packed structure.
- Addition and multiplication used for sizes, LBAs, clusters, or offsets use
  checked-overflow helpers before the result is narrowed.
- Intentional DOS 16-bit wrapping is expressed with an explicit fixed-width
  conversion and a nearby compatibility note; accidental signed or unsigned
  overflow is never used to emulate assembly flags.
- `memcpy_s`, `memset_s`, and `memcmp_s` require both capacities and a checked
  result. Fixed-width DOS fields use `memcpy_and_pad_s` or `strtomem_pad_s`, so
  truncation and padding are explicit rather than hidden in `strncpy`
  semantics.
- FAT walks have a volume-derived step limit and reject free, reserved, bad,
  out-of-range, and cyclic clusters.
- Public buffers carry capacities.  Sector I/O uses a 512-byte typed sector,
  not an unqualified `void *`.
- Mutable operation state is request-local.  Historical globals such as
  `ThisSFT`, `ThisCDS`, and `CURBUF` are not copied into the C core.
- On-disk mutations update both FATs and use an order that cannot create a
  cross-linked live file after interruption.
- DOS-facing functions return MS-DOS error values. Internal
  helpers use private status enums and map explicitly at the DOS API boundary.
- Multi-object updates use acquire-before-publish: prepare into
  private state, keep the publication critical section short, unwind acquired
  resources in reverse order, and enter a sticky poisoned state if rollback
  cannot prove restoration.  No filesystem, device, or network callback runs
  while a process/arena publication lock is held.
- Persistent registries use 64-bit slot-plus-generation handles.  A raw DOS
  segment, guest linear address, or native pointer is never an ownership token.
- A constructed runtime or lease/coordinator table owns its immutable 64-bit
  lifetime identity.  Callers bind to the identity read from that object; a
  parallel caller-supplied integer is never accepted as proof that two native
  objects represent the same lifetime.
- Cross-data-model persistent aggregates have an explicit alignment contract
  as well as size and offset assertions.  Aggregates containing canonical
  64-bit handles use `__aligned(8)` when they can be embedded or persisted, so
  an i386 build cannot silently give a future 64-bit parent layout different
  padding.  This native alignment rule never widens a DOS ABI field.
- State objects with a documented lifecycle are established by their
  initializer or construct/initialize entry point.  Do not use an incidental
  all-zero byte pattern as a substitute when zero is not the valid initialized
  state, and never inspect indeterminate storage to guess whether construction
  happened.
- One constructed MCB arena keeps one immutable generation-pinned identity.
  Its lease table is initialized once; before the arena MCB range/generation or
  either object lifetime is replaced, the owner stops acquisition and drains
  every non-terminal lease.  Reconstructing an object or zeroing a table is
  never a cleanup shortcut and never clears poison while another handle can
  still name the old lifetime.
- Resident loading, relocation, stack preparation, and PSP staging run under
  one EXEC observation exclusion from the first guest write through whole-lease
  publish or discard.  Guest execution and IRQ/interrupt observers remain
  quiesced for that interval.  This is executor ownership, not a spinlock held
  across file, device, network, or guest-memory callbacks.
- An SFT rollback failure poisons the complete generation-pinned adapter and
  affected machine/process context.  The EXEC coordinator rejects later EXEC,
  CLOSE, and device operations; quarantining only the fixed batch slot is
  insufficient.
- Zero is a valid DOS segment, handle field, or guest address whenever the
  original ABI permits it.  Such integers are never rejected merely because a
  native C pointer would use zero as `NULL`.
- Human-readable English belongs in `dos-ui`, not in DOS ABI dispatch.  A text
  revision may improve clarity, grammar, and terminology, but it may not alter
  numeric errors, register flags, command parsing, or command effects. Dynamic
  values are emitted separately with explicit lengths; format strings do not
  become an unbounded message interface.

## Required file header

Source files use this shape:

```c
// SPDX-License-Identifier: GPL-2.0-only
/*
 * DOS-C32 module name
 *
 * DOS contract:   concise description of visible inputs and outputs
 * Safety changes: bounded buffers, checked arithmetic, request-local state
 */
```

## Verification

All builds use warnings as errors, i386 code generation, no hosted C library,
and a link-time size/layout check. Host tests exercise malformed BPBs, FAT
chains and paths. Image tests use `fsck.fat` and mtools. ABI structures have
compile-time size and offset assertions against their stable public layouts.

<!-- SPDX-License-Identifier: GPL-2.0-only -->

# libc32

`libc32` is the freestanding C runtime for DOS-C32. It has its own checked
interfaces and keeps implementation details separate from MS-DOS-visible
behavior.

The runtime is built in two layers:

- `libc32-core` is freestanding and usable by the kernel.  It owns compiler
  runtime symbols, checked memory/string operations, character handling,
  bounded formatting, integer conversion, assertions, and overflow helpers.
- `libc32-dos` is the future program-facing layer.  It wraps typed DOS-C32
  system calls for console, files, directories, time, process control, and
  heap management.  Its boundary maps failures to the MS-DOS error
  table and INT 21h carry/AX convention.

## API policy

Raw `memcpy`, `memset`, and `memcmp` symbols exist only because GCC may emit
them for aggregate operations.  They are not declared in the public headers.
System and application code uses `memcpy_s`, `memset_s`, and `memcmp_s` with
explicit readable and writable capacities.

Ordinary strings crossing an object boundary use `strscpy_s` with explicit
destination and source capacities; fixed-width non-string DOS fields use
`strtomem_pad_s` or `memcpy_and_pad_s`.  The legacy destination-only
source-unbounded `strscpy` entry point is intentionally absent.
Unbounded copy, concatenation, formatting, and input functions are not
provided.

Planned modules:

```text
libc32-core
  memory/string   checked byte and string primitives
  ctype           locale-neutral DOS ASCII classification
  format          snprintf/vsnprintf only
  convert         checked integer parsing and formatting
  math64          checked 64-bit division and i386 compiler helpers
  assert          freestanding diagnostics
  allocator       bounded arena interface

libc32-dos
  crt0            32-bit program startup
  console         stdin/stdout/stderr over DOS handles
  file/dir        DOS handle and find-first/find-next wrappers
  time            DOS date/time conversion
  process         exec/exit/environment wrappers
  heap            process allocator backed by the DOS memory API
```

The first bootable image links `libc32-core` statically into the kernel.  Once
the native 32-bit executable ABI exists, the same source is also built as the
program runtime.

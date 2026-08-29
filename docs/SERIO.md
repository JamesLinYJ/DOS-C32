<!-- SPDX-License-Identifier: GPL-2.0-only -->

# DOS-C32 serio ownership

The serio subsystem is the generic bus for byte-oriented input ports. Its ABI
is internal to DOS-C32 and does not alter guest-visible behavior or binary
layout.

Ports and drivers are caller-owned and must be explicitly constructed. A
registry uses caller-provided pointer arrays. The repository currently has a
bootstrap bump arena and guest-page allocators, but no reclaiming general
kernel heap suitable for device-core lifetime. A future allocator may provide
larger registry arrays and port queues, then replace registry storage while all
ports are prepared or quiesced and no IRQ reference is live. Early boot and
hard IRQ paths never allocate or block; exhaustion is explicit and old FIFO
entries are preserved.

Registration matches four fixed ID fields with an explicit count; `0xff` means
any, while all-zero is a valid exact ID. Parent identity plus generation forms
a bounded tree. Single-port teardown refuses live children; subtree teardown
removes deepest children first. Binding calls connect, publishes the initialized
driver under the IRQ guard, then calls open. Failure unwinds in reverse. Unbind
withdraws IRQ visibility before close/disconnect, and live IRQ references gate
teardown.

`serio_interrupt` invokes the bound driver's interrupt callback directly. Only
DEFER enters the preallocated per-port FIFO. Unbound bytes are counted and
rejected rather than saved for an unrelated future driver. Deferred records
include port, driver and binding generations; the pump drops stale records
instead of crossing ownership. Raw bytes, controller status and parity,
timeout or frame flags are retained without scan-set or layout interpretation.

FIFO backpressure has two deliberately different results. Before a hardware
source consumes its data register, `serio_port_receive_preflight` returns
`SERIO_RETRY` when no raw-record slot can preserve the next byte. That is a
zero-commit result: the byte remains owned by hardware, and process context
drains decoded/raw work before polling the source again. The native i8042
boundary exposes that exact case as `X86_NATIVE_INPUT_SOURCE_BACKPRESSURE`, so
it cannot be confused with a consumed byte safely represented in the raw FIFO.
If a source byte has
already been consumed and direct delivery plus deferral still cannot represent
it, `serio_interrupt` returns `SERIO_STREAM_LOST`, increments a non-reusing loss
epoch, and closes only that port. It never disguises the discontinuity as an
ordinary retry.

Process-context recovery names the exact loss epoch. It discards the queued raw
suffix, calls the still-generation-bound driver's reconnect operation, and
reopens receipt only after that operation succeeds. A bounded intermediate
failure is `SERIO_RECOVERY_PENDING`; a stale epoch cannot recover a newer loss.
If the owner exhausts its recovery budget or the driver cannot reset its state,
`serio_port_isolate_stream` makes isolation terminal for that binding. The
native IRQ result remains handled for loss and isolation so controller EOI is
not skipped; recovery policy never runs in hard IRQ. The legacy input runtime
records the exact epoch, drains older decoded work, makes one budgeted reconnect
attempt, and performs at most one active OBF capture per pump call after making
space. This covers controllers that do not produce a second edge for a byte
held during zero-commit backpressure. Terminal isolation first names the exact
epoch, then removes IRQ1 and disables the keyboard interface. Controller output
is boundedly drained before command-byte readback so an abandoned scan byte
cannot be mistaken for the readback response; this per-stream failure does not
poison unrelated input owners.

The native i8042 backend owns no fixed machine instance. Platform configuration
supplies validated presence, ports, status masks, endpoint IDs, FIFO storage,
IRQ serialization and a read callback. It reads status once and, for OBF only,
data once. Its IRQ API returns HANDLED, NONE or FAULT. Optional process-context
port writes wait a configured number of status samples for input-buffer-empty;
keyboard data uses the data port and auxiliary data uses the standard D4 route
command followed by the data port. Every write returns transport status and an
independent `ZERO_COMMIT`, `COMMITTED`, or `UNCERTAIN` fact. A caller may replay
only `SERIO_RETRY` with `ZERO_COMMIT`. Keyboard preflight failures are zero
commit, while a failed data callback is uncertain. For auxiliary output, any
failure before attempting D4 is zero commit; attempting D4 starts the uncertain
interval, which ends only when the data callback succeeds and reports a
committed byte. ATKBD keeps device NAK retry counts per byte, retains a separate
transaction-wide write ceiling, and makes committed/uncertain history sticky for
the endpoint generation. The legacy input runtime restores firmware controller
state only when no byte crossed or may have crossed this boundary; otherwise it
quarantines the controller. The backend does not initialize the controller,
change its command byte, infer scan-code conversion state, issue EOI, reflect vectors or
inject a guest.

Native raw scan bytes do not feed the guest i8042 directly. The AT keyboard
driver decodes set-1/set-2 bytes into the normalized input core; focus selects
either the native console or the virtual PS/2 encoder, which uses the guest
keyboard's current scan-set and conversion epoch. Native conversion state is
therefore independent of a Windows-controlled virtual keyboard.

The implementation is split by stable responsibility into `registry.c`,
`binding.c` and `dispatch.c`; controller policy remains outside the bus.

<!-- SPDX-License-Identifier: GPL-2.0-only -->

# x86 virtual machine resource model

DOS-C32 does not classify executables as safe or trusted.  Each x86 guest
machine receives an immutable snapshot of the resources discovered and assigned
when it is created, and every direct mapping or modeled
access derives from a device capability with a stable identity and ownership
generation.  The map cannot be widened while guest execution is runnable.

For memory, `config/legacy-bios.mk` defines only the maximum early identity-map
and bitmap aperture. The BIOS E820 handoff determines the actual usable pages,
reserved holes, allocation bounds, XMS totals, and highest reported physical
address on every boot. Early paging maps the E820-derived high-water mark,
rounded to an i386 page-table boundary and clipped to the configured capacity;
it does not treat the configured ceiling as installed RAM.

## Access classes

| Resource | Default | Direct access requirement |
| --- | --- | --- |
| private conventional/UMB RAM | guest read/write/execute | pages belong only to that VM |
| legacy BIOS ROM/shadow window (`C0000h`-`FFFFFh`) | guest read/execute; write faults | active execution-tree firmware-shadow lease; the first precise write gets a private COW page and never writes the physical firmware mapping |
| discovered legacy display memory and ports | modeled/shared | validated DCC capability plus exclusive foreground-display ownership |
| serial or printer port | modeled/denied | exclusive device ownership and explicit capability |
| keyboard data | injected events | the 8042 controller itself remains mediated because it controls A20/reset |
| PIC, PIT and RTC | modeled | never direct while the native kernel owns interrupts/time |
| ATA/floppy controller | DOS/BIOS block model | never direct while native filesystems or caches own the medium |
| ISA 8237 DMA | guest-only register model; transfers denied until bound | no native access; an actual transfer additionally needs an independently owned, range-checked device endpoint |
| bus-master DMA | denied | no direct access on 386 without an IOMMU containment boundary |
| A20, reset and power controls | modeled | never directly writable by a guest |

Port-mapped I/O first traps through the VM86 IOPL/TSS boundary and becomes a
typed `DOS_EXEC_EVENT_PORT_IO`.  Scalar byte/word/dword IN/OUT requests carry
the port, direction, width and value.  `dos-machine` then dispatches to the
capability/device policy: success resumes the generation-bound session, denial
stops it without inventing a result, and uncertain device failure fails closed.
Future exclusive direct PIO is an optimization of the same capability map via
the TSS I/O bitmap, not a different compatibility mode.

## ISA DMA ownership

The PC/AT primary and secondary 8237A controllers are private register state,
not native pass-through. The model owns the exact controller ranges
`0000h`-`000Fh` and `00C0h`-`00DFh`, plus only the real page-register ranges
`0081h`-`0083h`, `0087h`, `0089h`-`008Bh`, and `008Fh`. It deliberately does
not claim POST/delay port `0080h` or the unused gaps in that page aperture.

Address and count bytes share the controller flip-flop; command, software
request, single/all mask, mode, clear-flip-flop, master-clear, status and
temporary-register behavior remain per controller. The secondary controller
uses word-addressed channels, masks page bit zero, and starts with channel 4
as the unmasked cascade for primary channels 0-3. Status reads acknowledge only
terminal-count bits. Reads of write-only controller registers return the
undriven-bus value, and secondary odd-port aliases follow the PC-compatible
decode without touching native hardware.

Programming these registers alone never reads or writes guest memory and never
asserts a native DMA request. A future floppy, sound, or other ISA device must
bind a separate generation-owned endpoint that validates channel direction,
mode, page/address/count range, boundary rules, device buffer capacity and
guest mapping before committing a bounded transfer. Bus-master DMA remains a
different capability and stays denied without containment.

## Legacy interrupt and clock ownership

The native 8259A/8254 pair is a supervisor scheduling source only.  On each
native IRQ, the native owner acknowledges its own PIC and emits either an
elapsed-PIT-clock event or a typed external IRQ edge.  The generation-bound
guest chipset owner consumes that event, advances its private 8254 state, sets
the appropriate private 8259A IRR bit, and resolves a deliverable vector from
the guest's current ICW vector bases, IMR, ISR, priority and EOI state.  A
native vector is never reflected into the application and guest writes to
`20h/21h/A0h/A1h/40h-43h` never reach native hardware.

Physical IF remains enabled during VM86 execution so the clock source can
advance even while the application has logical IF clear.  Logical IF controls
only whether a modeled pending interrupt may be claimed.  A pending IRR bit is
therefore retained across CLI/POPF/IRET and execution exits and is reconsidered
before each VM86 entry and after every emulated control instruction.

The current native scheduler reload is 1193 PIT input clocks, approximately
one millisecond, and is deliberately reported as uncalibrated.  The API accepts
an explicit 64-bit elapsed-clock count, so a future monotonic clock owner can
replace this sampling source without changing guest PIT or PIC semantics.
Multiple guest channel-0 expirations in one source quantum are counted, while
the edge-triggered PIC naturally coalesces them into one pending IRR bit.

Current limits are explicit: delayed or physically coalesced native IRQ0
events cannot yet be reconstructed from wall time; PIT gate ownership and
modes 1/5 are not active; exact STI one-instruction interrupt shadow is not yet
modeled; the isolated i8042 model and native serio/i8042 capture backend exist,
but native IRQ registration, AT-keyboard decoding, input focus, virtual PS/2
encoding and the guest PIC-event adapter are not connected; and the RTC remains a stable fallback,
not a live time source. RTC periodic/alarm interrupts are not advertised.

The i8042 model owns ports 60h and 64h with separate byte-read and byte-write
policies. Controller status, command byte, input/output ports, keyboard device
commands and keyboard/auxiliary output bytes are bounded private state. The
runtime configuration contract requires the platform owner to supply actual
port-presence, keyboard identity, initial A20, lock and device state; the model
does not infer those facts from its compiled capacity. A generation-bound
input source injects scan bytes at the execution-owner serialization boundary.
IRQ1, IRQ12 and A20 changes enter a typed FIFO and remain peekable until the
downstream owner explicitly consumes the exact sequence. Capacity exhaustion
leaves the requested transition unpublished. Guest reset-low writes are
recorded and forced back to reset-high without reaching native hardware.

Native controller input is separately owned by the DOS-C32 serio bus. Platform
configuration supplies presence, ports, status masks, identities, bounded
queues and an IRQ-safe read callback. The IRQ action reads status once and data
once only for output-full, then directly calls the currently bound driver.
Only that driver may request bounded deferred delivery. Unbound bytes are not
saved for a future binding; deferred records carry port, driver and binding
generations so rebinding cannot consume an old stream. Registry storage may be
replaced only while ports are not active and no IRQ reference is live.
An optional process-context write path waits a platform-configured bound for
input-buffer-empty and reports timeout without an unbounded spin. It does not
rewrite the native controller command byte or claim a scan-code conversion
mode.

Raw native scan bytes are deliberately not a production input to the guest
i8042. Firmware may leave the native controller in a converted scan-code mode
while Windows
independently programs its virtual command byte and keyboard scan set. The
required pipeline is native serio -> AT-keyboard protocol driver -> normalized
key event/focus owner -> virtual PS/2 encoder -> guest i8042 -> typed guest IRQ.

Memory-mapped resources use paging.  The physical `C0000h`-`FFFFFh` BIOS
ROM/shadow window is guest-readable and executable but remains read-only.
Some PC firmware places writable low runtime state in the same reserved
aperture as option ROM and BIOS code, so a blanket read-only policy would
manufacture faults that a real-mode machine does not generate.  DOS-C32
therefore handles only a present, user-mode write-protection fault in this
window while an exact execution-tree firmware-shadow lease is active.  The
faulting page is copied to unused private backing, the guest PTE alone is
replaced with a user-writable COW mapping, the TLB is flushed, and the original
instruction is retried without advancing guest state.  The physical firmware
page and unrelated option-ROM/MMIO pages are never made writable.

Firmware shadows are owned independently of display foreground state.  Ring-0
guest-memory copies resolve the active guest PTE rather than assuming that a
legacy linear address is still identity-mapped.  Process-tree cleanup restores
every original PTE, flushes TLB entries, clears private backing, and invalidates
the lease before COMMAND regains ownership.  The VM86 adapter consumes a small
bounded release-retry budget; any non-exact result force-revokes remaining low
aliases and quarantines uncertain backing before its slot can be reused.
Capacity exhaustion, a stale owner, or an uncertain post-publication cleanup
poisons or quarantines the guest space instead of widening the aperture. The HMA
retains guest access, while the
architectural video window remains supervisor-only by default.  The
display capability owner accepts only BIOS DCC values defined by the MS-DOS
hardware-detection contract. MDA gets `B0000h`-`B7FFFh` and `03B0h`-`03BFh`;
CGA gets `B8000h`-`BFFFFh` and `03D0h`-`03DFh`; EGA/VGA gets the switchable
`A0000h`-`BFFFFh` window and `03B0h`-`03DFh`. MCGA receives its 64 KiB graphics
and character-generator range plus its CGA-compatible `B8000h` text aperture
and `03C0h`-`03DFh` control/CRTC family, independent of attached monitor type.
Foreground publication grants those exact page ranges transactionally;
an unknown DCC, inconsistent capability mask, or inconsistent BDA text console
registers no native display resource and maps no display page for user access.
Dangerous/shared MMIO outside these explicit windows remains absent or
read-only and later feeds a typed MMIO fault decoder. Kernel, page-table, GDT,
IDT, TSS and native stack pages are always supervisor-only.

## Information consistency

BIOS data, interrupt services and device queries must report the resource view
the guest actually owns.  A directly assigned physical device reports its real
properties; a disk image or modeled device reports that model's properties.
No program-specific name, hash, instruction sequence or timing can alter the
reported view.

## User-visible transparency

The virtual-machine manager is an internal protection and compatibility
boundary, not a new DOS-visible personality. Programs discover standard DOS,
BIOS, EMS, VCPI, DPMI and Windows contracts only. Production UI describes a
stopped application and a protected system; backend names and CPU modes are
restricted to compile-gated developer diagnostics.

## Windows 3.2 path

Windows 3.2 is not granted blanket hardware privilege.  Its foreground VM may
own the discovered display ranges while keyboard, timing, disk and global interrupt controls
remain modeled.  The first compatibility target is standard mode (`WIN /S`):
DOS services, XMS/A20, BIOS devices and a 16-bit protected-mode selector/mode
transition backend.  The later 386 Enhanced Mode target adds a virtualized
CR0/CR3, GDT/LDT/IDT, page tables, VMM/VxD services and nested VM86 state; the
real host control registers and interrupt controllers are never handed over.

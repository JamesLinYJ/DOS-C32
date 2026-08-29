<!-- SPDX-License-Identifier: GPL-2.0-only -->

# x86 interrupt ownership

This directory separates two domains that must not be conflated.

`x86_native_irq_dispatch` owns native vector descriptors and registered native
actions. The platform controller supplies explicit vector-to-hardware-IRQ
descriptions and `begin`/`end` callbacks. `begin` classifies a delivery as real
or spurious without sending its final EOI. Registered actions run only for a
real delivery. `end` then applies the controller-specific EOI rule. This lets an
8259 backend implement IRQ7 with no EOI and spurious IRQ15 with only the master
cascade EOI, without teaching the generic dispatcher any 8259 port or IRQ
number.

`x86_guest_irq_router` is the sole source bound to the guest chipset. Native
sources and emulated-device sources register as distinct generation-bound
producers. A native event reaches the guest only through an installed route;
there is no same-number fallback. Consequently the normal PC wiring is:

1. native IRQ0 action observes elapsed PIT ticks and uses the explicit PIT
   route to submit a guest clock event;
2. native IRQ1 action drains the native i8042 into serio and does not submit a
   guest interrupt;
3. the guest i8042 consumes the selected serio byte and submits a typed guest
   IRQ1 or IRQ12 producer event;
4. IRQ7/IRQ15 observations classified as spurious never reach any action or
   guest producer.

Both registries accept caller-owned storage. Early boot may use static arrays;
after the page allocator and kernel heap are published, device discovery may
allocate larger arrays from the heap. Storage origin is not part of routing
semantics. No hard-IRQ path allocates or blocks.

Topology publication is serialized through lifecycle state. Producer, route,
action, and storage changes are accepted only while prepared or quiesced, never
while active. Quiesce closes local dispatch before asking the sink/controller
to mask and drain the source. Resume reopens the backend before publishing the
active state. Guest-sink retirement calls `unbind`; a teardown failure poisons
the router rather than pretending the old sink capability disappeared.

Sink/controller callbacks are transactional: `REJECTED` from bind or resume
means no backend state changed; `OK` means the requested transition completed.
Failure after a successful acquire, failure to quiesce/unbind, an invalid
callback result, or an uncertain controller `end`/EOI poisons the owner.

## Integration sequence

1. Construct and initialize both registries with storage selected by the boot
   owner. Prepare native line descriptions only from the selected platform
   controller instance.
2. Register the PIT and native-i8042 actions while the dispatcher is prepared.
   Prepare guest producers and install only the PIT native route.
3. Bind the guest router as the guest chipset's sole source, then publish the
   native dispatcher. Unmask IRQ0/IRQ1 only through the controller `resume`
   callback after their actions exist.
4. In the architectural trap path, pass every controller vector to
   `x86_native_irq_dispatch_vector()`. Do not branch on IRQ1 and do not call the
   old same-number guest translator.
5. Split native PIC acknowledgement into `begin` (read ISR/classify spurious)
   and `end` (EOI according to the observation and action completion). Keep the
   platform controller instance, masks, and presence outside this generic core.
6. For teardown or topology growth, quiesce first, mutate or replace storage,
   resume if continuing, or unregister all children and retire in reverse
   order.

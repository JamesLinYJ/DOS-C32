<!-- SPDX-License-Identifier: GPL-2.0-only -->

# DOS-C32 compatibility matrix

Status date: 2026-08-30

This is an evidence table, not a promise inferred from the existence of an
API. `complete` means the listed automated tests pass for the stated scope.
`partial` identifies the remaining compatibility work. MS-DOS-visible
behavior, ABI, errors, ordering, and intentional fixed-width wrapping remain
the external contract.

## Machine, process, and executable path

| Component | Automated evidence | Status |
| --- | --- | --- |
| Legacy BIOS bootstrap | `tests/verify-image.sh`, `tests/boot-smoke.sh` | complete for the current i386 image: `IO.SYS` finds and loads the independent `DOSKRNL.SYS` payload by its FAT directory entry and cluster chain, and the kernel starts the independent `COMMAND.COM`; no UEFI path exists |
| High kernel placement and paging | paging, display, guest-space, image, boot, and firmware-release tests | partial: high placement, E820-derived bounds, supervisor-only kernel pages, display capabilities, and isolated firmware writes are covered; fully private conventional-memory ownership and all A20-dependent mappings remain |
| i386 VM86 entry and typed exit | boot smoke tests plus the x86 machine, guest-space, and instruction tests | partial: preparation, run, release, software interrupts, 16-bit control instructions, scalar port I/O, precise state, and policy mediation are covered; string I/O, complete hardware-IRQ delivery, MMIO decoding, and BIOS ROM services remain |
| Guest 16:16 memory and A20 | `tests/test-dos-machine.sh` | complete for offset wrap, A20 wrap, range preflight, state transitions, rollback, and uncertain-state quarantine |
| Guest ISA DMA register model | `tests/test-x86-legacy-chipset.sh`, `tests/test-x86-guest-space.sh`, `tests/test-x86-io-resource.sh` | complete for isolated 8237A register programming and exact port ownership; actual device-triggered transfers remain disabled until an independently owned endpoint validates the entire transfer |
| Guest i8042 controller | `tests/test-x86-i8042.sh` | partial: controller and keyboard command behavior, FIFOs, IRQ events, A20 events, and reset containment are covered; native input and guest interrupt integration remain |
| Native byte-stream input bus and i8042 capture | `tests/test-serio-native-input.sh` | partial: lifecycle, exact binding, IRQ delivery, bounded writes, deferral, loss recovery, rollback, and teardown are covered; native IRQ integration remains |
| Native console keyboard consumer | `tests/test-keyboard-console.sh` | partial: key state, BIOS values, repeat/release, modifiers, bounded queues, focus flushing, ownership, and teardown are covered; runtime publication and process-context wait integration remain |
| XMS and HMA ownership | XMS, x86 memory, and paging-policy tests | partial: the tested core functions, global HMA lease, runtime memory totals, A20 locks, moves, resizing, rollback, and quarantine are covered; UMB functions and installed Windows acceptance remain |
| COM/MZ classification, loading, relocation, stack, and environment | loader, relocation, image-load, environment, parameter, overlay, and transaction tests | partial: EXEC0/EXEC1 preparation and publication are covered; EXEC3 resident writes and remaining runtime integration are pending |
| MCB allocation and ownership | memory, lease, process, and transaction tests | partial: fit policy, exact bytes, generation ownership, replacement, rollback, and poison are covered; process cleanup and complete runtime composition remain |
| PSP/PDB, JFT/SFT, DTA, and CurrentPDB | process, runtime, SFT-batch, seal, and transaction tests | partial: construction, inheritance, publication ordering, generation checks, and rollback are covered; complete termination and device-table lifecycle remain |
| EXEC0/EXEC1 transaction | executor, INT 21h, native adapter, handoff, backend-session, and transaction tests | partial: load-only publication and executable child-session handoff are covered; COMMAND scheduling and complete terminate/return integration remain |
| External diagnostic program gate | `make probe-dos-program DOS_PROGRAM=...` | partial: an external MZ diagnostic reaches classification, allocation, loading, relocation, stack validation, and handoff without adding fixture bytes to the repository; continued execution remains |
| 16-bit execution | boot VM86 probe and host backend tests | partial: ordinary instructions, software interrupts, virtual flags, scalar port I/O, and nested session transfer share one loop; broader BIOS, device, IRQ, termination, and DOS-service coverage remains |

## Storage and COMMAND

| Component | Automated evidence | Status |
| --- | --- | --- |
| I/O Manager FAT12/16/32 mount, path, search, positional reads, and directory creation | FAT driver, discovery, table, volume, and shell-capacity tests | partial: DOS, COMMAND, EXEC, and native C32 loading use one generic path; FAT16 mutation is covered, while FAT12/FAT32 mutation, directory expansion, and more injected rollback cases remain |
| ATA boot-device write capability | `tests/test-ata-device.sh`, `tests/test-ata-block.sh` | complete for the current firmware-bound 512-byte LBA28 PIO adapter, including explicit writable and read-only policies; removable-media sensing and additional device capabilities remain |
| Boot and COMMAND presentation | shell-capacity, boot-smoke, and FAT-corruption tests | complete for the currently implemented messages and bounded dynamic output; numeric errors, flags, parsing, command effects, and 8.3 behavior remain independent of presentation text |
| COMMAND builtins and external resolution | shell capacity, path, and external-command tests | partial: the current builtin set and bounded current-directory/PATH search are covered; the complete traditional command surface and lifecycle remain |

## INT 21h dispatcher

| AH | Service | Evidence and status |
| --- | --- | --- |
| 00h | Compatible terminate | `tests/test-dos-int21.sh`; typed exit signal complete, common teardown pending |
| 25h | Set interrupt vector | `tests/test-dos-int21.sh`; complete with serialized access and rollback poison |
| 30h | Get DOS version | `tests/test-dos-int21.sh` and boot VM86 test; returns compatibility identity 6.23 with the expected OEM and user-number register layout |
| 35h | Get interrupt vector | `tests/test-dos-int21.sh`; complete |
| 39h | Create directory | INT 21h and FAT driver tests; complete for bounded path decoding and FAT16 transaction behavior |
| 48h | Allocate memory | INT 21h and memory tests; complete for visible CF/AX/BX behavior and private failure containment |
| 49h | Free memory | same evidence and boundary as 48h |
| 4Ah | Resize memory | same evidence and boundary as 48h, including BX only on grow failure |
| 4Bh | Load/execute | transaction and executor tests; AL=0 is integrated, while AL=1 and AL=3 remain pending |
| 4Ch | Terminate with code | `tests/test-dos-int21.sh`; typed exit signal complete, resource and parent restoration pending |
| 4Dh | Get child return tuple | `tests/test-dos-int21.sh`; complete for return-once behavior |
| 50h | Set current PSP/PDB | `tests/test-dos-int21.sh`; complete through the shared runtime owner |
| 51h | Get current PSP/PDB | `tests/test-dos-int21.sh`; complete through the shared runtime owner |
| 58h | Get/set allocation strategy | `tests/test-dos-int21.sh`; implemented subfunctions complete |
| 59h | Get extended error | error and INT 21h tests; complete for the implemented error table |
| 62h | Get current PSP | `tests/test-dos-int21.sh`; complete through the shared runtime owner |
| other entries | Dispatcher coverage | pending entries return an internal `UNIMPLEMENTED` result without fabricating a DOS ABI answer |

## DOS-visible layouts

| Layout family | Evidence | Status |
| --- | --- | --- |
| BPB, directory entry, DPB, CDS | ABI and portable-data-model tests | fixed-width layout complete; services using all fields remain |
| PSP/PDB, FCB, DTA, EXEC parameter block | ABI and process tests | layout complete; full lifecycle remains |
| MCB/arena | ABI, memory, and memory-lease tests | layout, identities, lease lifecycle, rollback poison, generations, and stale-handle rejection complete; complete coordinator drain remains |
| JFT/SFT and device headers | ABI and SFT-batch tests | layout and EXEC inheritance transaction complete; full SFT manager and device-chain behavior remain |

Update this matrix in the same change that adds a service test. A row may move
to `complete` only when visible outputs, flags, errors, and failure boundaries
are exercised, not merely when a C function compiles.

# SPDX-License-Identifier: GPL-2.0-only
#
# Legacy-BIOS image policy.  These values are passed to the boot sector,
# protected-mode entry, linker, image formatter, and verifier.  Keep DOS ABI
# constants beside the code that owns that ABI; keep build geometry here.

BIOS_BOOT_LOAD_ADDRESS := 0x7c00
BIOS_SECTOR_BYTES := 512
BIOS_READ_RETRIES := 3

KERNEL_LOAD_BASE := 0x10000
IO_SYSTEM_LOAD_BASE := 0x8000
FAT_HIDDEN_SECTORS ?= 0
# Firmware stages the compact kernel file below conventional-memory limits.
# The protected-mode trampoline then copies its runtime payload above 1 MiB,
# leaving the DOS 0000:0000..9fff:ffff conventional address space available
# for isolated guests. These are host layout policy, not DOS-visible values.
KERNEL_RUNTIME_BASE := 0x00400000
KERNEL_STACK_FLOOR := 0x007e0000
KERNEL_STACK_TOP := 0x007ff000

# Capacity policy, not detected RAM.  The kernel maps only the portion of this
# aperture that the current BIOS E820 map says is usable.  Keeping these values
# here prevents allocator and page-table sources from growing separate limits.
X86_BOOT_IDENTITY_FLOOR ?= 0x00800000
X86_BOOT_IDENTITY_CEILING ?= 0x10000000

# Native diagnostic UART policy. The actual port candidates and console
# geometry are firmware-discovered from the BDA; these values only bound how
# the kernel configures and waits for a validated 16550-compatible endpoint.
X86_SERIAL_BAUD_DIVISOR ?= 1
X86_SERIAL_POLL_LIMIT ?= 1000000

# Native ATA PIO wait policy. Controller ports and device selection are not
# policy: they come from the validated EDD/DPTE boot-device locator.
X86_ATA_POLL_LIMIT ?= 2000000

# Platform authorization for ATA WRITE SECTORS: 0 is read-only, 1 allows
# writes after a successful ordinary (non-ATAPI) IDENTIFY.  This is not a
# claim that media write protection was detected.
X86_ATA_WRITE_POLICY ?= 1

# Early A20 transition policy.  The entry code detects the current state after
# every method; these values bound slow legacy-controller polling and delayed
# gate propagation without turning a board-specific mechanism into a fact.
# The defaults provide conservative x86 boot tolerances.
X86_A20_ENABLE_ATTEMPTS ?= 255
X86_A20_SHORT_TEST_LOOPS ?= 32
X86_A20_LONG_TEST_LOOPS ?= 2097152
X86_A20_KBC_POLL_LIMIT ?= 100000
X86_A20_KBC_ABSENT_SAMPLES ?= 32

# Bounded early-stack-guard hardware-random retries.  Source quality remains
# runtime-observed and the guard is still diversified on CPUs without RDRAND.
X86_STACK_GUARD_RDRAND_ATTEMPTS ?= 32

# The BIOS date and time calls are sampled date/time/date so a midnight edge
# cannot publish a torn wall-clock snapshot.  This only bounds retries; the
# resulting RTC capability is published solely when one stable sample validates.
X86_BOOT_RTC_SNAPSHOT_ATTEMPTS ?= 4

FAT_SECTORS_PER_CLUSTER := 4
FAT_RESERVED_SECTORS := 1024
FAT_COPIES := 2
FAT_ROOT_ENTRIES := 512
FAT_SECTORS_PER_COPY := 31
FAT_MEDIA_DESCRIPTOR := 0xf8

BIOS_HEADS := 16
BIOS_SECTORS_PER_TRACK := 63
BIOS_DRIVE_NUMBER := 128

BYTES_PER_KIB := 1024
IMAGE_KIB := 16384
IMAGE_VOLUME_ID := 400C0032

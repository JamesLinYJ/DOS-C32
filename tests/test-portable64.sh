#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
c32_dir=$(CDPATH= cd -- "$test_dir/.." && pwd)
portable_cc=${CC:-gcc}
temporary_dir=$(mktemp -d "${TMPDIR:-/tmp}/dos-c32-portable.XXXXXX")
memory_floor=${X86_BOOT_IDENTITY_FLOOR:-0x00800000}
memory_ceiling=${X86_BOOT_IDENTITY_CEILING:-0x10000000}
ata_write_policy=${X86_ATA_WRITE_POLICY:-1}

cleanup()
{
	rm -f -- "$temporary_dir"/*.o
	rmdir -- "$temporary_dir"
}
trap cleanup EXIT HUP INT TERM

common_flags="-m64 -march=x86-64 -std=gnu11 -O2 -ffreestanding -fno-builtin -fno-pie -fno-pic -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables -Wall -Wextra -Werror -Wundef -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wvla -Wformat=2 -Wcast-align=strict -Wnull-dereference -DCONFIG_X86_BOOT_IDENTITY_FLOOR=$memory_floor -DCONFIG_X86_BOOT_IDENTITY_CEILING=$memory_ceiling -DCONFIG_DOS_XMS_HMA_MINIMUM_BYTES=0 -DCONFIG_X86_ATA_WRITE_POLICY=$ata_write_policy"

for source in \
	"$c32_dir/libc32/assert.c" \
	"$c32_dir/libc32/string.c" \
	"$c32_dir/libc32/ctype.c" \
	"$c32_dir/libc32/format.c" \
	"$c32_dir/libc32/convert.c" \
	"$c32_dir/libc32/math64.c" \
	"$c32_dir/libc32/arena.c" \
	"$c32_dir/kernel/block_device.c" \
	"$c32_dir/kernel/ata_device.c" \
	"$c32_dir/kernel/ata_block.c" \
	"$c32_dir/kernel/x86_vm/io/resource.c" \
	"$c32_dir/kernel/x86_vm/chipset/policy.c" \
	"$c32_dir/kernel/x86_vm/chipset/owner.c" \
	"$c32_dir/kernel/x86_vm/chipset/dma.c" \
	"$c32_dir/kernel/x86_vm/chipset/pic.c" \
	"$c32_dir/kernel/x86_vm/chipset/pit.c" \
	"$c32_dir/kernel/x86_vm/chipset/rtc.c" \
	"$c32_dir/kernel/x86_vm/platform/boot_storage.c" \
	"$c32_dir/kernel/x86_vm/platform/legacy_bios.c" \
	"$c32_dir/kernel/x86_vm/memory/ems_config.c" \
	"$c32_dir/dos/machine.c" \
	"$c32_dir/dos/loader.c" \
	"$c32_dir/dos/image_load.c" \
	"$c32_dir/dos/environment.c" \
	"$c32_dir/dos/environment/view.c" \
	"$c32_dir/dos/relocator.c" \
	"$c32_dir/dos/process.c" \
	"$c32_dir/dos/process_runtime.c" \
	"$c32_dir/dos/termination.c" \
	"$c32_dir/dos/exec_observer.c" \
	"$c32_dir/dos/exec_gate.c" \
	"$c32_dir/dos/exec_transaction.c" \
	"$c32_dir/dos/exec_executor.c" \
	"$c32_dir/dos/exec_int21.c" \
	"$c32_dir/dos/exec_native.c" \
	"$c32_dir/dos/exec_journal.c" \
	"$c32_dir/dos/exec_file_lease.c" \
	"$c32_dir/dos/exec_name.c" \
	"$c32_dir/dos/exec_parameter.c" \
	"$c32_dir/dos/exec_overlay.c" \
	"$c32_dir/dos/exec_seal.c" \
	"$c32_dir/dos/sft_batch.c" \
	"$c32_dir/dos/sft_adapter.c" \
	"$c32_dir/dos/drive_visibility.c" \
	"$c32_dir/dos/drive/config.c" \
	"$c32_dir/dos/error.c" \
	"$c32_dir/dos/memory.c" \
	"$c32_dir/dos/memory_lease.c" \
	"$c32_dir/dos/vectors.c" \
	"$c32_dir/dos/interrupt_reflection.c" \
	"$c32_dir/dos/control_instruction.c" \
	"$c32_dir/dos/port_instruction.c" \
	"$c32_dir/dos/ems/core.c" \
	"$c32_dir/dos/ems/vcpi.c" \
	"$c32_dir/dos/ems/iomgr_device.c" \
	"$c32_dir/dos/xms/hma.c" \
	"$c32_dir/dos/xms/manager.c" \
	"$c32_dir/dos/nls/package.c" \
	"$c32_dir/dos/nls/country_file.c" \
	"$c32_dir/dos/jft/resize.c" \
	"$c32_dir/dos/int21.c" \
	"$c32_dir/dos/runtime_owner.c" \
	"$c32_dir/storage/core/manager.c" \
	"$c32_dir/storage/core/device.c" \
	"$c32_dir/storage/core/discovery.c" \
	"$c32_dir/storage/core/exec_adapter.c" \
	"$c32_dir/storage/core/transaction.c" \
	"$c32_dir/storage/fat/entry.c" \
	"$c32_dir/storage/fat/boot.c" \
	"$c32_dir/storage/fat/driver.c" \
	"$c32_dir/storage/fat/named.c" \
	"$test_dir/portable_compile.c"
do
	object="$temporary_dir/$(basename "${source%.c}").o"
	# shellcheck disable=SC2086 # Deliberate compiler argument list.
	"$portable_cc" $common_flags -I"$c32_dir/include" -c "$source" -o "$object"
done

echo "portable compile passed: libc32 and DOS boundaries support x86_64 ABI"

#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
c32_dir=$(CDPATH= cd -- "$test_dir/.." && pwd)
block_cc=${CC:-gcc}
temporary_dir=$(mktemp -d "${TMPDIR:-/tmp}/dos-c32-ata-block.XXXXXX")

cleanup()
{
	rm -f -- "$temporary_dir"/*
	rmdir -- "$temporary_dir"
}
trap cleanup EXIT HUP INT TERM

common_flags="-std=gnu11 -O2 -ffreestanding -fno-builtin -fno-pie -fno-pic -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables -Wall -Wextra -Werror -Wundef -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wvla -Wformat=2 -Wcast-align=strict -Wnull-dereference"

for policy in 0 1
do
	for model in 32 64
	do
		case "$model" in
		32) architecture_flags="-m32 -march=i386 -msoft-float -mno-mmx -mno-sse -mno-sse2" ;;
		64) architecture_flags="-m64 -march=x86-64" ;;
		esac
		for source in "$c32_dir/kernel/object_identity.c" \
			"$c32_dir/kernel/block_device.c" \
			"$c32_dir/kernel/x86_vm/platform/boot_storage.c" \
			"$c32_dir/kernel/ata_block.c" \
			"$test_dir/ata_block_test.c"
		do
			object="$temporary_dir/$(basename "${source%.c}")-$policy-$model.o"
			# shellcheck disable=SC2086 # Deliberate compiler argument lists.
			"$block_cc" $architecture_flags $common_flags \
				-DCONFIG_X86_ATA_WRITE_POLICY=$policy \
				-I"$c32_dir/include" -c "$source" -o "$object"
		done
		# shellcheck disable=SC2086 # Deliberate compiler argument lists.
		"$block_cc" $architecture_flags -nostdlib -static -no-pie \
			-Wl,-e,_start -Wl,--build-id=none \
			"$temporary_dir/object_identity-$policy-$model.o" \
			"$temporary_dir/block_device-$policy-$model.o" \
			"$temporary_dir/boot_storage-$policy-$model.o" \
			"$temporary_dir/ata_block-$policy-$model.o" \
			"$temporary_dir/ata_block_test-$policy-$model.o" \
			-o "$temporary_dir/ata-block-p$policy-m$model"
	done
done

"$temporary_dir/ata-block-p0-m64"
"$temporary_dir/ata-block-p1-m64"
m32_result="linked"
if command -v qemu-i386 >/dev/null 2>&1; then
	qemu-i386 "$temporary_dir/ata-block-p0-m32"
	qemu-i386 "$temporary_dir/ata-block-p1-m32"
	m32_result="executed"
fi

echo "ATA block adapter tests passed: m64 executed, m32 $m32_result"
echo "Covered typed read-only and write-enabled policy"

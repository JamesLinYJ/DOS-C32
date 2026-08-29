#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
c32_dir=$(CDPATH= cd -- "$test_dir/.." && pwd)
paging_cc=${CC:-gcc}
temporary_dir=$(mktemp -d "${TMPDIR:-/tmp}/dos-c32-paging-policy.XXXXXX")

cleanup()
{
	rm -f -- "$temporary_dir"/*
	rmdir -- "$temporary_dir"
}
trap cleanup EXIT HUP INT TERM

common_flags="-std=gnu11 -O2 -ffreestanding -fno-builtin -fno-pie"
common_flags="$common_flags -fno-pic -fno-stack-protector"
common_flags="$common_flags -fno-asynchronous-unwind-tables"
common_flags="$common_flags -fno-unwind-tables -Wall -Wextra -Werror"
common_flags="$common_flags -Wundef -Wshadow -Wstrict-prototypes"
common_flags="$common_flags -Wmissing-prototypes -Wvla -Wformat=2"
common_flags="$common_flags -Wcast-align=strict -Wnull-dereference"
floor=${X86_BOOT_IDENTITY_FLOOR:-0x00800000}
ceiling=${X86_BOOT_IDENTITY_CEILING:-0x10000000}
paging_config="-DX86_PAGING_HOST_TEST=1"
paging_config="$paging_config -DCONFIG_X86_BOOT_IDENTITY_FLOOR=$floor"
paging_config="$paging_config -DCONFIG_X86_BOOT_IDENTITY_CEILING=$ceiling"

for model in 32 64
do
	case "$model" in
	32)
		architecture_flags="-m32 -march=i386 -msoft-float"
		architecture_flags="$architecture_flags -mno-mmx -mno-sse -mno-sse2"
		;;
	64) architecture_flags="-m64 -march=x86-64" ;;
	esac
	for source in "$c32_dir/kernel/x86_vm/memory/map.c" \
		"$c32_dir/kernel/x86_page_policy.c" \
		"$c32_dir/kernel/x86_paging.c" \
		"$test_dir/x86_paging_policy_test.c"
	do
		object="$temporary_dir/$(basename "${source%.c}")-$model.o"
		# shellcheck disable=SC2086 # Deliberate compiler argument lists.
		"$paging_cc" $architecture_flags $common_flags $paging_config \
			-I"$c32_dir/include" -c "$source" -o "$object"
	done
	# shellcheck disable=SC2086 # Deliberate compiler argument lists.
	"$paging_cc" $architecture_flags -nostdlib -static -no-pie \
		-Wl,-e,_start -Wl,--build-id=none \
		"$temporary_dir/x86_page_policy-$model.o" \
		"$temporary_dir/x86_paging-$model.o" \
		"$temporary_dir/map-$model.o" \
		"$temporary_dir/x86_paging_policy_test-$model.o" \
		-o "$temporary_dir/x86-paging-policy-m$model"
done

"$temporary_dir/x86-paging-policy-m64"
if command -v qemu-i386 >/dev/null 2>&1; then
	qemu-i386 "$temporary_dir/x86-paging-policy-m32"
fi

echo "x86 paging tests passed: foreground video aperture," \
	"BIOS shadow, high isolation"

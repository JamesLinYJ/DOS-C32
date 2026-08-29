#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
cc=${CC:-gcc}
temporary_dir=$(mktemp -d "${TMPDIR:-/tmp}/dos-c32-guest-space.XXXXXX")

cleanup()
{
	rm -f -- "$temporary_dir"/*
	rmdir -- "$temporary_dir"
}
trap cleanup EXIT HUP INT TERM

common_flags="-DDOSC32_HOST_TEST=1 -std=gnu11 -O2 -ffreestanding"
common_flags="$common_flags -fno-builtin -fno-pie -fno-pic"
common_flags="$common_flags -fno-stack-protector -fno-unwind-tables"
common_flags="$common_flags -fno-asynchronous-unwind-tables"
common_flags="$common_flags -Wall -Wextra -Werror -Wundef -Wshadow"
common_flags="$common_flags -Wstrict-prototypes -Wmissing-prototypes -Wvla"
common_flags="$common_flags -Wformat=2 -Wcast-align=strict -Wnull-dereference"
common_flags="$common_flags -DCONFIG_X86_GUEST_DEVICE_EVENT_PUMP_BUDGET=1u"
common_flags="$common_flags -DCONFIG_X86_GUEST_FIRMWARE_CLIENT_CAPACITY=2u"
common_flags="$common_flags -DCONFIG_X86_GUEST_FIRMWARE_SHADOW_PAGE_CAPACITY=2u"
floor=${X86_BOOT_IDENTITY_FLOOR:-0x00800000}
ceiling=${X86_BOOT_IDENTITY_CEILING:-0x10000000}
paging_config="-DCONFIG_X86_BOOT_IDENTITY_FLOOR=$floor"
paging_config="$paging_config -DCONFIG_X86_BOOT_IDENTITY_CEILING=$ceiling"

for model in 32 64; do
	case "$model" in
	32) architecture_flags="-m32 -march=i386 -msoft-float -mno-mmx \
		-mno-sse -mno-sse2" ;;
	64) architecture_flags="-m64 -march=x86-64" ;;
	esac
	for variant in present absent corrupted exhausted quarantined; do
		extra_flags=
		set --
		if [ "$variant" = absent ]; then
			extra_flags=-DX86_GUEST_SPACE_ABSENT_DISPLAY_TEST=1
		elif [ "$variant" = corrupted ]; then
			extra_flags=-DX86_GUEST_SPACE_FIRMWARE_CORRUPTION_TEST=1
		elif [ "$variant" = exhausted ]; then
			extra_flags=-DX86_GUEST_SPACE_FIRMWARE_EXHAUSTION_TEST=1
		elif [ "$variant" = quarantined ]; then
			extra_flags=-DX86_GUEST_SPACE_QUARANTINE_TEST=1
		fi
		while IFS= read -r source; do
			[ -n "$source" ] || continue
			object="$temporary_dir/$(basename "${source%.c}")-$variant-$model.o"
			# shellcheck disable=SC2086 # Deliberate compiler lists.
			"$cc" $architecture_flags $common_flags $paging_config \
				$extra_flags -I"$root_dir/include" -I"$root_dir/tests" \
				-I"$root_dir/kernel/x86_vm" \
				-c "$source" -o "$object"
			set -- "$@" "$object"
		done <<EOF
$root_dir/libc32/math64.c
$root_dir/libc32/string.c
$root_dir/kernel/x86_vm/io/resource.c
$root_dir/kernel/x86_vm/chipset/policy.c
$root_dir/kernel/x86_vm/chipset/owner.c
$root_dir/kernel/x86_vm/chipset/dma.c
$root_dir/kernel/x86_vm/chipset/pic.c
$root_dir/kernel/x86_vm/chipset/pit.c
$root_dir/kernel/x86_vm/chipset/rtc.c
$root_dir/kernel/x86_vm/chipset/i8042.c
$root_dir/kernel/x86_vm/irq/guest_router.c
$root_dir/kernel/x86_vm/irq/guest_topology.c
$root_dir/kernel/x86_vm/irq/guest_dispatch.c
$root_dir/kernel/x86_vm/platform/display.c
$root_dir/kernel/x86_vm/memory/firmware_shadow.c
$root_dir/kernel/x86_vm/vm86_firmware.c
$root_dir/kernel/x86_vm/guest_space.c
$root_dir/tests/x86_guest_space_test.c
EOF
		binary="$temporary_dir/x86-guest-space-$variant-m$model"
		# shellcheck disable=SC2086 # Deliberate compiler lists.
		"$cc" $architecture_flags -nostdlib -static -no-pie \
			-Wl,-e,_start -Wl,--build-id=none "$@" -o "$binary"
		if [ "$model" = 64 ]; then
			"$binary"
		fi
	done
done

echo "x86 guest-space tests passed: m64 executed, m32 linked only"
echo "Covered firmware COW/translation/retry/quarantine, display, i8042 and rollback"

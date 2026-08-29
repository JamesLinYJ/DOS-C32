#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
c32_dir=$(CDPATH= cd -- "$test_dir/.." && pwd)
release_cc=${CC:-gcc}
temporary_dir=$(mktemp -d "${TMPDIR:-/tmp}/dos-c32-vm86-release.XXXXXX")

cleanup()
{
	status=$?
	trap - EXIT HUP INT TERM
	rm -f -- "$temporary_dir"/* || status=$?
	rmdir -- "$temporary_dir" || status=$?
	exit "$status"
}
trap cleanup EXIT HUP INT TERM

common_flags="-DDOSC32_HOST_TEST=1 -std=gnu11 -O2 -ffreestanding"
common_flags="$common_flags -fno-builtin -fno-pie -fno-pic"
common_flags="$common_flags -fno-stack-protector -fno-unwind-tables"
common_flags="$common_flags -fno-asynchronous-unwind-tables"
common_flags="$common_flags -Wall -Wextra -Werror -Wundef -Wshadow"
common_flags="$common_flags -Wstrict-prototypes -Wmissing-prototypes -Wvla"
common_flags="$common_flags -Wformat=2 -Wcast-align=strict -Wnull-dereference"
common_flags="$common_flags -DCONFIG_X86_GUEST_FIRMWARE_RELEASE_ATTEMPTS=3u"
identity_floor=${X86_BOOT_IDENTITY_FLOOR:-0x00800000}
identity_ceiling=${X86_BOOT_IDENTITY_CEILING:-0x10000000}
common_flags="$common_flags -DCONFIG_X86_BOOT_IDENTITY_FLOOR=$identity_floor"
common_flags="$common_flags -DCONFIG_X86_BOOT_IDENTITY_CEILING=$identity_ceiling"

for model in 32 64; do
	case "$model" in
	32) architecture_flags="-m32 -march=i386 -msoft-float -mno-mmx \
		-mno-sse -mno-sse2" ;;
	64) architecture_flags="-m64 -march=x86-64" ;;
	esac
	set --
	for source in \
		"$c32_dir/kernel/x86_vm/vm86_firmware.c" \
		"$test_dir/x86_vm86_firmware_release_test.c"
	do
		object="$temporary_dir/$(basename "${source%.c}")-$model.o"
		# shellcheck disable=SC2086 # Deliberate compiler lists.
		"$release_cc" $architecture_flags $common_flags \
			-I"$c32_dir/include" -I"$c32_dir/kernel/x86_vm" \
			-I"$test_dir" -c "$source" -o "$object"
		set -- "$@" "$object"
	done
	binary="$temporary_dir/x86-vm86-firmware-release-$model"
	# shellcheck disable=SC2086 # Deliberate compiler lists.
	"$release_cc" $architecture_flags -nostdlib -static -no-pie \
		-Wl,-e,_start -Wl,--build-id=none "$@" -o "$binary"
	if [ "$model" = 64 ]; then
		"$binary"
	fi
done

echo "x86 VM86 firmware release tests passed: bounded retry and fail-closed quarantine"

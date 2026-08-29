#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
c32_dir=$(CDPATH= cd -- "$test_dir/.." && pwd)
guest_ps2_cc=${CC:-gcc}
temporary_dir=$(mktemp -d "${TMPDIR:-/tmp}/dos-c32-guest-ps2.XXXXXX")

cleanup()
{
	rm -f -- "$temporary_dir"/*.o "$temporary_dir"/guest-ps2-m32 \
		"$temporary_dir"/guest-ps2-m64
	rmdir -- "$temporary_dir"
}
trap cleanup EXIT HUP INT TERM

common_flags="-std=gnu11 -O2 -ffreestanding -fno-builtin -fno-pie"
common_flags="$common_flags -fno-pic -fno-stack-protector"
common_flags="$common_flags -fno-asynchronous-unwind-tables -fno-unwind-tables"
common_flags="$common_flags -Wall -Wextra -Werror -Wundef -Wshadow"
common_flags="$common_flags -Wstrict-prototypes -Wmissing-prototypes -Wvla"
common_flags="$common_flags -Wformat=2 -Wcast-align=strict -Wnull-dereference"
common_flags="$common_flags -DCONFIG_X86_BOOT_IDENTITY_FLOOR=0x00400000u"
common_flags="$common_flags -DCONFIG_X86_BOOT_IDENTITY_CEILING=0x10000000u"

for model in 32 64
do
	case "$model" in
	32) architecture_flags="-m32 -march=i386 -msoft-float -mno-mmx \
		-mno-sse -mno-sse2" ;;
	64) architecture_flags="-m64 -march=x86-64" ;;
	esac
	objects=""
	object_index=0
	for source in \
		"$c32_dir/kernel/input/core/lifecycle.c" \
		"$c32_dir/kernel/input/core/routing.c" \
		"$c32_dir/kernel/input/keyboard/guest_ps2/maps.c" \
		"$c32_dir/kernel/input/keyboard/guest_ps2/encode.c" \
		"$c32_dir/kernel/input/keyboard/guest_ps2/lifecycle.c" \
		"$c32_dir/kernel/input/keyboard/guest_ps2/dispatch.c" \
		"$test_dir/guest_ps2_keyboard_test.c"
	do
		object="$temporary_dir/object-$object_index-$model.o"
		object_index=$((object_index + 1))
		# shellcheck disable=SC2086 # Deliberate compiler argument lists.
		"$guest_ps2_cc" $architecture_flags $common_flags \
			-I"$c32_dir/include" -I"$test_dir" \
			-c "$source" -o "$object"
		objects="$objects $object"
	done
	# shellcheck disable=SC2086 # Deliberate object/argument lists.
	"$guest_ps2_cc" $architecture_flags -nostdlib -static -no-pie \
		-Wl,-e,_start -Wl,--build-id=none $objects \
		-o "$temporary_dir/guest-ps2-m$model"
done

"$temporary_dir/guest-ps2-m64"
m32_result="linked only"
if command -v qemu-i386 >/dev/null 2>&1; then
	qemu-i386 "$temporary_dir/guest-ps2-m32"
	m32_result="executed"
fi

echo "guest PS/2 keyboard tests passed: m64 executed, m32 $m32_result"
echo "Covered set1/set2, translation, E0/Pause/PrintScreen, repeat, atomic mode epochs, FIFO backpressure, committed-pending delivery and focus generations"

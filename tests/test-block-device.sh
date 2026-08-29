#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
c32_dir=$(CDPATH= cd -- "$test_dir/.." && pwd)
block_cc=${CC:-gcc}
temporary_dir=$(mktemp -d "${TMPDIR:-/tmp}/dos-c32-block.XXXXXX")

cleanup()
{
	rm -f -- "$temporary_dir"/*.o "$temporary_dir"/block-device-m32 \
			"$temporary_dir"/block-device-m64 \
			"$temporary_dir"/block-device-generation-m32 \
			"$temporary_dir"/block-device-generation-m64
	rmdir -- "$temporary_dir"
}
trap cleanup EXIT HUP INT TERM

common_flags="-std=gnu11 -O2 -ffreestanding -fno-builtin -fno-pie -fno-pic -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables -Wall -Wextra -Werror -Wundef -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wvla -Wformat=2 -Wcast-align=strict -Wnull-dereference"

for model in 32 64
do
	case "$model" in
	32) architecture_flags="-m32 -march=i386 -msoft-float -mno-mmx -mno-sse -mno-sse2" ;;
	64) architecture_flags="-m64 -march=x86-64" ;;
	esac
	for source in "$c32_dir/kernel/block_device.c" \
		"$test_dir/block_device_test.c"
	do
		object="$temporary_dir/$(basename "${source%.c}")-$model.o"
		# shellcheck disable=SC2086 # Deliberate compiler argument lists.
		"$block_cc" $architecture_flags $common_flags \
			-I"$c32_dir/include" -c "$source" -o "$object"
	done
	# shellcheck disable=SC2086 # Deliberate compiler argument lists.
	"$block_cc" $architecture_flags -nostdlib -static -no-pie \
			-Wl,-e,_start -Wl,--build-id=none \
		"$temporary_dir/block_device-$model.o" \
			"$temporary_dir/block_device_test-$model.o" \
			-o "$temporary_dir/block-device-m$model"
	for source in "$c32_dir/kernel/block_device.c" \
		"$test_dir/block_device_generation_test.c"
	do
		object="$temporary_dir/generation-$(basename "${source%.c}")-$model.o"
		# shellcheck disable=SC2086 # Deliberate compiler argument lists.
		"$block_cc" $architecture_flags $common_flags \
			-DBLOCK_DEVICE_TEST_GENERATION_MAX=2u \
			-I"$c32_dir/include" -c "$source" -o "$object"
	done
	# shellcheck disable=SC2086 # Deliberate compiler argument lists.
	"$block_cc" $architecture_flags -nostdlib -static -no-pie \
		-Wl,-e,_start -Wl,--build-id=none \
		"$temporary_dir/generation-block_device-$model.o" \
		"$temporary_dir/generation-block_device_generation_test-$model.o" \
		-o "$temporary_dir/block-device-generation-m$model"
done

"$temporary_dir/block-device-m64"
"$temporary_dir/block-device-generation-m64"
if command -v qemu-i386 >/dev/null 2>&1; then
	qemu-i386 "$temporary_dir/block-device-m32"
	qemu-i386 "$temporary_dir/block-device-generation-m32"
fi

echo "block-device tests passed: callback validation, canonical geometry, non-wrapping handles (m64 run, m32 link)"

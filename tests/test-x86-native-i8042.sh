#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root_dir=$(CDPATH= cd -- "$test_dir/.." && pwd)
test_cc=${CC:-gcc}
temporary_dir=$(mktemp -d "${TMPDIR:-/tmp}/dos-c32-native-i8042.XXXXXX")

cleanup()
{
	rm -f -- "$temporary_dir"/*.o "$temporary_dir"/native-i8042-m32 \
		"$temporary_dir"/native-i8042-m64
	rmdir -- "$temporary_dir"
}
trap cleanup EXIT HUP INT TERM

common_flags="-DDOSC32_HOST_TEST=1 -std=gnu11 -O2 -ffreestanding"
common_flags="$common_flags -fno-builtin -fno-pie -fno-pic"
common_flags="$common_flags -fno-stack-protector -fno-asynchronous-unwind-tables"
common_flags="$common_flags -fno-unwind-tables -Wall -Wextra -Werror -Wundef"
common_flags="$common_flags -Wshadow -Wstrict-prototypes -Wmissing-prototypes"
common_flags="$common_flags -Wvla -Wformat=2 -Wcast-align=strict"
common_flags="$common_flags -Wnull-dereference"

for model in 32 64
do
	case "$model" in
	32) architecture_flags="-m32 -march=i386 -msoft-float -mno-mmx \
		-mno-sse -mno-sse2" ;;
	64) architecture_flags="-m64 -march=x86-64" ;;
	esac
	for source in \
		"$root_dir/kernel/x86_vm/platform/native_i8042.c" \
		"$test_dir/x86_native_i8042_test.c"
	do
		object="$temporary_dir/$(basename "${source%.c}")-$model.o"
		# shellcheck disable=SC2086 # Deliberate compiler argument lists.
		"$test_cc" $architecture_flags $common_flags \
			-I"$root_dir/include" -I"$test_dir" \
			-c "$source" -o "$object"
	done
	# shellcheck disable=SC2086 # Deliberate compiler argument lists.
	"$test_cc" $architecture_flags -nostdlib -static -no-pie \
		-Wl,-e,_start -Wl,--build-id=none \
		"$temporary_dir/native_i8042-$model.o" \
		"$temporary_dir/x86_native_i8042_test-$model.o" \
		-o "$temporary_dir/native-i8042-m$model"
done

"$temporary_dir/native-i8042-m64"
m32_result="linked only"
if command -v qemu-i386 >/dev/null 2>&1; then
	qemu-i386 "$temporary_dir/native-i8042-m32"
	m32_result="executed"
fi

echo "x86 native-i8042 tests passed: m64 executed, m32 $m32_result"
echo "Covered stable CTR reads, stage/publish, quiesce drain, exact restore and poison"

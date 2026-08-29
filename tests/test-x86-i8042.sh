#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
c32_dir=$(CDPATH= cd -- "$test_dir/.." && pwd)
i8042_cc=${CC:-gcc}
temporary_dir=$(mktemp -d "${TMPDIR:-/tmp}/dos-c32-i8042.XXXXXX")

cleanup()
{
	rm -f -- "$temporary_dir"/*.o "$temporary_dir"/i8042-m32 \
		"$temporary_dir"/i8042-m64
	rmdir -- "$temporary_dir"
}
trap cleanup EXIT HUP INT TERM

common_flags="-std=gnu11 -O2 -ffreestanding -fno-builtin -fno-pie"
common_flags="$common_flags -fno-pic -fno-stack-protector"
common_flags="$common_flags -fno-asynchronous-unwind-tables -fno-unwind-tables"
common_flags="$common_flags -Wall -Wextra -Werror -Wundef -Wshadow"
common_flags="$common_flags -Wstrict-prototypes -Wmissing-prototypes -Wvla"
common_flags="$common_flags -Wformat=2 -Wcast-align=strict -Wnull-dereference"

for model in 32 64
do
	case "$model" in
	32) architecture_flags="-m32 -march=i386 -msoft-float -mno-mmx \
		-mno-sse -mno-sse2" ;;
	64) architecture_flags="-m64 -march=x86-64" ;;
	esac
	for source in \
		"$c32_dir/kernel/x86_vm/io/resource.c" \
		"$c32_dir/kernel/x86_vm/chipset/i8042.c" \
		"$test_dir/x86_i8042_test.c"
	do
		object="$temporary_dir/$(basename "${source%.c}")-$model.o"
		# shellcheck disable=SC2086 # Deliberate compiler argument lists.
		"$i8042_cc" $architecture_flags $common_flags \
			-I"$c32_dir/include" -I"$test_dir" \
			-c "$source" -o "$object"
	done
	# shellcheck disable=SC2086 # Deliberate compiler argument lists.
	"$i8042_cc" $architecture_flags -nostdlib -static -no-pie \
		-Wl,-e,_start -Wl,--build-id=none \
		"$temporary_dir/resource-$model.o" \
		"$temporary_dir/i8042-$model.o" \
		"$temporary_dir/x86_i8042_test-$model.o" \
		-o "$temporary_dir/i8042-m$model"
done

"$temporary_dir/i8042-m64"
m32_result="linked only"
if command -v qemu-i386 >/dev/null 2>&1; then
	qemu-i386 "$temporary_dir/i8042-m32"
	m32_result="executed"
fi

echo "x86 i8042 tests passed: m64 executed, m32 $m32_result"
echo "Covered ports 60h/64h, IRQ1/12, A20, input lifecycle and FIFO bounds"

#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
c32_dir=$(CDPATH= cd -- "$test_dir/.." && pwd)
input_cc=${CC:-gcc}
temporary_dir=$(mktemp -d "${TMPDIR:-/tmp}/dos-c32-input-core.XXXXXX")

cleanup()
{
	rm -f -- "$temporary_dir"/*.o "$temporary_dir"/input-core-m32 \
		"$temporary_dir"/input-core-m64
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
		"$c32_dir/kernel/input/core/lifecycle.c" \
		"$c32_dir/kernel/input/core/routing.c" \
		"$test_dir/input_core_test.c"
	do
		object="$temporary_dir/$(basename "${source%.c}")-$model.o"
		# shellcheck disable=SC2086 # Deliberate compiler argument lists.
		"$input_cc" $architecture_flags $common_flags \
			-I"$c32_dir/include" -I"$test_dir" \
			-c "$source" -o "$object"
	done
	# shellcheck disable=SC2086 # Deliberate compiler argument lists.
	"$input_cc" $architecture_flags -nostdlib -static -no-pie \
		-Wl,-e,_start -Wl,--build-id=none \
		"$temporary_dir/lifecycle-$model.o" \
		"$temporary_dir/routing-$model.o" \
		"$temporary_dir/input_core_test-$model.o" \
		-o "$temporary_dir/input-core-m$model"
done

"$temporary_dir/input-core-m64"
m32_result="linked only"
if command -v qemu-i386 >/dev/null 2>&1; then
	qemu-i386 "$temporary_dir/input-core-m32"
	m32_result="executed"
fi

echo "input-core tests passed: m64 executed, m32 $m32_result"
echo "Covered focus generations, deferred FIFO, stale-domain drops and teardown"

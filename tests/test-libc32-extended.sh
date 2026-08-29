#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
c32_dir=$(CDPATH= cd -- "$test_dir/.." && pwd)
extended_cc=${CC:-gcc}
temporary_dir=$(mktemp -d "${TMPDIR:-/tmp}/dos-c32-extended.XXXXXX")
test_elf="$temporary_dir/libc32-extended-test"

cleanup()
{
	rm -f -- "$temporary_dir"/*.o "$test_elf"
	rmdir -- "$temporary_dir"
}
trap cleanup EXIT HUP INT TERM

common_flags="-m64 -march=x86-64 -std=gnu11 -O2 -ffreestanding -fno-builtin -fno-pie -fno-pic -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables -Wall -Wextra -Werror -Wundef -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wvla -Wformat=2 -Wcast-align=strict -Wnull-dereference"

for source in "$c32_dir/libc32/assert.c" "$c32_dir/libc32/convert.c" \
	"$c32_dir/libc32/math64.c" \
	"$c32_dir/libc32/arena.c" \
	"$test_dir/libc32_extended_test.c"
do
	object="$temporary_dir/$(basename "${source%.c}").o"
	# shellcheck disable=SC2086 # Deliberate compiler argument list.
	"$extended_cc" $common_flags -I"$c32_dir/include" -c "$source" \
		-o "$object"
done
"$extended_cc" -m64 -nostdlib -no-pie -Wl,-e,_start \
	-Wl,--build-id=none "$temporary_dir/assert.o" \
	"$temporary_dir/convert.o" "$temporary_dir/math64.o" \
	"$temporary_dir/arena.o" \
	"$temporary_dir/libc32_extended_test.o" -o "$test_elf"
"$test_elf"

echo "libc32 extended tests passed: assertions, checked 64-bit division, conversion and address arena"

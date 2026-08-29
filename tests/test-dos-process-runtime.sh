#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
c32_dir=$(CDPATH= cd -- "$test_dir/.." && pwd)
runtime_cc=${CC:-gcc}
temporary_dir=$(mktemp -d "${TMPDIR:-/tmp}/dos-c32-runtime.XXXXXX")
test_elf="$temporary_dir/dos-process-runtime-test"
test_elf32="$temporary_dir/dos-process-runtime-test32"

cleanup()
{
	rm -f -- "$temporary_dir"/*.o "$test_elf" "$test_elf32"
	rmdir -- "$temporary_dir"
}
trap cleanup EXIT HUP INT TERM

warning_flags="-Wall -Wextra -Werror -Wundef -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wvla -Wformat=2 -Wcast-align=strict -Wnull-dereference"
freestanding_flags="-std=gnu11 -O2 -ffreestanding -fno-builtin -fno-pie -fno-pic -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables"

# shellcheck disable=SC2086 # Deliberate compiler argument lists.
"$runtime_cc" -m64 -march=x86-64 $freestanding_flags $warning_flags \
	-I"$c32_dir/include" "$c32_dir/dos/process_runtime.c" \
	"$test_dir/dos_process_runtime_test.c" -nostdlib -no-pie \
	-Wl,-e,_start -Wl,--build-id=none -o "$test_elf"
"$test_elf"

# shellcheck disable=SC2086 # Deliberate compiler argument lists.
"$runtime_cc" -m32 -march=i386 $freestanding_flags $warning_flags \
	-I"$c32_dir/include" "$c32_dir/dos/process_runtime.c" \
	"$test_dir/dos_process_runtime_test.c" -nostdlib -no-pie \
	-Wl,-e,_start -Wl,--build-id=none -o "$test_elf32"

echo "dos-process-runtime tests passed: fixed layout, lifetime identity, zero PSP, stale snapshot, publish and poison (m64 run, m32 link)"

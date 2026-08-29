#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
c32_dir=$(CDPATH= cd -- "$test_dir/.." && pwd)
machine_cc=${CC:-gcc}
temporary_dir=$(mktemp -d "${TMPDIR:-/tmp}/dos-c32-machine.XXXXXX")
test_elf="$temporary_dir/dos-machine-test"

cleanup()
{
	rm -f -- "$temporary_dir"/*.o "$test_elf"
	rmdir -- "$temporary_dir"
}
trap cleanup EXIT HUP INT TERM

common_flags="-m64 -march=x86-64 -std=gnu11 -O2 -ffreestanding -fno-builtin -fno-pie -fno-pic -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables -Wall -Wextra -Werror -Wundef -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wvla -Wformat=2 -Wcast-align=strict -Wnull-dereference"

# shellcheck disable=SC2086 # Deliberate compiler argument list.
"$machine_cc" $common_flags -I"$c32_dir/include" \
	-c "$c32_dir/dos/machine.c" -o "$temporary_dir/machine.o"
# shellcheck disable=SC2086 # Deliberate compiler argument list.
"$machine_cc" $common_flags -I"$c32_dir/include" \
	-c "$test_dir/dos_machine_test.c" -o "$temporary_dir/test.o"
"$machine_cc" -m64 -nostdlib -no-pie -Wl,-e,_start -Wl,--build-id=none \
	"$temporary_dir/machine.o" "$temporary_dir/test.o" -o "$test_elf"
"$test_elf"

echo "dos-machine tests passed: bounded ranges, transactional writes, staged ports and fail-closed A20"

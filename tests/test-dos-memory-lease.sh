#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
c32_dir=$(CDPATH= cd -- "$test_dir/.." && pwd)
lease_cc=${CC:-gcc}
temporary_dir=$(mktemp -d "${TMPDIR:-/tmp}/dos-c32-memory-lease.XXXXXX")
test_elf="$temporary_dir/dos-memory-lease-test"
test_elf32="$temporary_dir/dos-memory-lease-test-32"

cleanup()
{
	rm -f -- "$temporary_dir"/*.o "$test_elf" "$test_elf32"
	rmdir -- "$temporary_dir"
}
trap cleanup EXIT HUP INT TERM

warning_flags="-std=gnu11 -O2 -ffreestanding -fno-builtin -fno-pie -fno-pic -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables -Wall -Wextra -Werror -Wundef -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wvla -Wformat=2 -Wcast-align=strict -Wnull-dereference"

for source in "$c32_dir/dos/machine.c" "$c32_dir/dos/memory.c" \
	"$c32_dir/dos/memory_lease.c" "$test_dir/dos_memory_lease_test.c"
do
	object="$temporary_dir/$(basename "${source%.c}").m64.o"
	# shellcheck disable=SC2086 # Deliberate compiler argument list.
	"$lease_cc" -m64 -march=x86-64 $warning_flags -I"$c32_dir/include" \
		-c "$source" -o "$object"
done
"$lease_cc" -m64 -nostdlib -no-pie -Wl,-e,_start \
	-Wl,--build-id=none "$temporary_dir/machine.m64.o" \
	"$temporary_dir/memory.m64.o" "$temporary_dir/memory_lease.m64.o" \
	"$temporary_dir/dos_memory_lease_test.m64.o" -o "$test_elf"
"$test_elf"

for source in "$c32_dir/dos/machine.c" "$c32_dir/dos/memory.c" \
	"$c32_dir/dos/memory_lease.c" "$test_dir/dos_memory_lease_test.c"
do
	object="$temporary_dir/$(basename "${source%.c}").m32.o"
	# shellcheck disable=SC2086 # Deliberate compiler argument list.
	"$lease_cc" -m32 -march=i386 $warning_flags -I"$c32_dir/include" \
		-c "$source" -o "$object"
done

"$lease_cc" -m32 -nostdlib -no-pie -Wl,-e,_start \
	-Wl,--build-id=none "$temporary_dir/machine.m32.o" \
	"$temporary_dir/memory.m32.o" "$temporary_dir/memory_lease.m32.o" \
	"$temporary_dir/dos_memory_lease_test.m32.o" -o "$test_elf32"

echo "dos-memory-lease tests passed: fixed layout/encoding, table-lifetime ABA, generation-bound MCB rebind, no-callback publish, poison (m64 run, m32 link)"

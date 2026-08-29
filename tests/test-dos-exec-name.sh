#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
c32_dir=$(CDPATH= cd -- "$test_dir/.." && pwd)
exec_name_cc=${CC:-gcc}
temporary_dir=$(mktemp -d "${TMPDIR:-/tmp}/dos-c32-exec-name.XXXXXX")
test_elf="$temporary_dir/dos-exec-name-test"
test_elf32="$temporary_dir/dos-exec-name-test32"

cleanup()
{
	rm -f -- "$temporary_dir"/*.o "$test_elf" "$test_elf32"
	rmdir -- "$temporary_dir"
}
trap cleanup EXIT HUP INT TERM

warning_flags="-Wall -Wextra -Werror -Wundef -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wvla -Wformat=2 -Wcast-align=strict -Wnull-dereference"
freestanding_flags="-std=gnu11 -O2 -ffreestanding -fno-builtin -fno-pie -fno-pic -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables"
common64_flags="-m64 -march=x86-64 $freestanding_flags $warning_flags"
common32_flags="-m32 -march=i386 $freestanding_flags $warning_flags"

for source in \
	"$c32_dir/dos/machine.c" \
	"$c32_dir/dos/exec_name.c" \
	"$test_dir/dos_exec_name_test.c"
do
	object="$temporary_dir/$(basename "${source%.c}").o"
	# shellcheck disable=SC2086 # Deliberate compiler argument lists.
	"$exec_name_cc" $common64_flags -I"$c32_dir/include" -c "$source" \
		-o "$object"
done
"$exec_name_cc" -m64 -nostdlib -no-pie -Wl,-e,_start -Wl,--build-id=none \
	"$temporary_dir/exec_name.o" "$temporary_dir/dos_exec_name_test.o" \
	"$temporary_dir/machine.o" \
	-o "$test_elf"
"$test_elf"

for source in \
	"$c32_dir/dos/machine.c" \
	"$c32_dir/dos/exec_name.c" \
	"$test_dir/dos_exec_name_test.c"
do
	object="$temporary_dir/$(basename "${source%.c}")32.o"
	# shellcheck disable=SC2086 # Deliberate compiler argument lists.
	"$exec_name_cc" $common32_flags -I"$c32_dir/include" -c "$source" \
		-o "$object"
done
"$exec_name_cc" -m32 -nostdlib -no-pie -Wl,-e,_start -Wl,--build-id=none \
	"$temporary_dir/exec_name32.o" \
	"$temporary_dir/machine32.o" \
	"$temporary_dir/dos_exec_name_test32.o" -o "$test_elf32"

echo "dos-exec-name tests passed: exact guest DStrLen boundary, wrap/A20, DOS owner patch and unchanged plan on faults (m64 run, m32 link)"

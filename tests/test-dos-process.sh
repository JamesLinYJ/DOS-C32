#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
c32_dir=$(CDPATH= cd -- "$test_dir/.." && pwd)
process_cc=${CC:-gcc}
temporary_dir=$(mktemp -d "${TMPDIR:-/tmp}/dos-c32-process.XXXXXX")
test_elf="$temporary_dir/dos-process-test"
test_elf32="$temporary_dir/dos-process-test32"

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
	"$c32_dir/libc32/assert.c" \
	"$c32_dir/libc32/string.c" \
	"$c32_dir/dos/machine.c" \
	"$c32_dir/dos/memory.c" \
	"$c32_dir/dos/process.c" \
	"$c32_dir/dos/process_runtime.c" \
	"$c32_dir/dos/termination.c" \
	"$test_dir/dos_process_test.c"
do
	object="$temporary_dir/$(basename "${source%.c}").o"
	# shellcheck disable=SC2086 # Deliberate compiler argument lists.
	"$process_cc" $common64_flags -I"$c32_dir/include" -c "$source" \
		-o "$object"
done
"$process_cc" -m64 -nostdlib -no-pie -Wl,-e,_start -Wl,--build-id=none \
	"$temporary_dir/assert.o" "$temporary_dir/string.o" \
	"$temporary_dir/machine.o" "$temporary_dir/memory.o" \
	"$temporary_dir/process.o" "$temporary_dir/process_runtime.o" \
	"$temporary_dir/termination.o" \
	"$temporary_dir/dos_process_test.o" -o "$test_elf"
"$test_elf"

# The first deliverable is i386.  Compile and link the complete freestanding
# harness there as an ABI check; the behavioral run remains native x86-64
# because some sandboxes intentionally block 32-bit int 80h execution.
for source in \
	"$c32_dir/libc32/assert.c" \
	"$c32_dir/libc32/string.c" \
	"$c32_dir/dos/machine.c" \
	"$c32_dir/dos/memory.c" \
	"$c32_dir/dos/process.c" \
	"$c32_dir/dos/process_runtime.c" \
	"$c32_dir/dos/termination.c" \
	"$test_dir/dos_process_test.c"
do
	object="$temporary_dir/$(basename "${source%.c}")32.o"
	# shellcheck disable=SC2086 # Deliberate compiler argument lists.
	"$process_cc" $common32_flags -I"$c32_dir/include" -c "$source" \
		-o "$object"
done
"$process_cc" -m32 -nostdlib -no-pie -Wl,-e,_start -Wl,--build-id=none \
	"$temporary_dir/assert32.o" "$temporary_dir/string32.o" \
	"$temporary_dir/machine32.o" "$temporary_dir/memory32.o" \
	"$temporary_dir/process32.o" "$temporary_dir/process_runtime32.o" \
	"$temporary_dir/termination32.o" \
	"$temporary_dir/dos_process_test32.o" -o "$test_elf32"

echo "dos-process tests passed: DOS-visible PSP/JFT/FCB/tail transaction and COM/MZ plans (m64 run, m32 link)"

#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
c32_dir=$(CDPATH= cd -- "$test_dir/.." && pwd)
sft_cc=${CC:-gcc}
temporary_dir=$(mktemp -d "${TMPDIR:-/tmp}/dos-c32-sft-batch.XXXXXX")
test_elf="$temporary_dir/dos-sft-batch-test"
test_elf32="$temporary_dir/dos-sft-batch-test32"

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
	"$c32_dir/dos/sft_batch.c" \
	"$test_dir/dos_sft_batch_test.c"
do
	object="$temporary_dir/$(basename "${source%.c}").o"
	# shellcheck disable=SC2086 # Deliberate compiler argument lists.
	"$sft_cc" $common64_flags -I"$c32_dir/include" -c "$source" \
		-o "$object"
done
"$sft_cc" -m64 -nostdlib -no-pie -Wl,-e,_start -Wl,--build-id=none \
	"$temporary_dir/sft_batch.o" "$temporary_dir/dos_sft_batch_test.o" \
	-o "$test_elf"
"$test_elf"

# i386 is the first boot target.  Compile and link the same freestanding
# harness there; native execution remains x86-64 for restrictive sandboxes.
for source in \
	"$c32_dir/dos/sft_batch.c" \
	"$test_dir/dos_sft_batch_test.c"
do
	object="$temporary_dir/$(basename "${source%.c}")32.o"
	# shellcheck disable=SC2086 # Deliberate compiler argument lists.
	"$sft_cc" $common32_flags -I"$c32_dir/include" -c "$source" \
		-o "$object"
done
"$sft_cc" -m32 -nostdlib -no-pie -Wl,-e,_start -Wl,--build-id=none \
	"$temporary_dir/sft_batch32.o" \
	"$temporary_dir/dos_sft_batch_test32.o" -o "$test_elf32"

echo "dos-sft-batch tests passed: inheritance filters, reverse unwind, poison, pure preflight, commit and ABA (m64 run, m32 link)"

#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
c32_dir=$(CDPATH= cd -- "$test_dir/.." && pwd)
int21_cc=${CC:-gcc}
temporary_dir=$(mktemp -d "${TMPDIR:-/tmp}/dos-c32-int21.XXXXXX")
test_elf="$temporary_dir/dos-int21-test"

cleanup()
{
	rm -f -- "$temporary_dir"/*.o "$test_elf"
	rmdir -- "$temporary_dir"
}
trap cleanup EXIT HUP INT TERM

warning_flags="-Wall -Wextra -Werror -Wundef -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wvla -Wformat=2 -Wcast-align=strict -Wnull-dereference"
freestanding_flags="-std=gnu11 -O2 -ffreestanding -fno-builtin -fno-pie -fno-pic -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables"
common64_flags="-m64 -march=x86-64 $freestanding_flags $warning_flags"
common32_flags="-m32 -march=i386 -msoft-float -mno-mmx -mno-sse -mno-sse2 $freestanding_flags $warning_flags"

for source in \
	"$c32_dir/dos/machine.c" \
	"$c32_dir/dos/memory.c" \
	"$c32_dir/dos/jft/resize.c" \
	"$c32_dir/dos/error.c" \
	"$c32_dir/dos/vectors.c" \
	"$c32_dir/dos/process_runtime.c" \
	"$c32_dir/dos/find/record.c" \
	"$c32_dir/dos/nls/package.c" \
	"$c32_dir/dos/sft_adapter.c" \
	"$c32_dir/storage/core/device.c" \
	"$c32_dir/dos/drive/config.c" \
	"$c32_dir/dos/int21.c" \
	"$test_dir/dos_int21_test.c"
do
	object="$temporary_dir/$(basename "${source%.c}").o"
	# shellcheck disable=SC2086 # Deliberate compiler argument lists.
	"$int21_cc" $common64_flags -I"$c32_dir/include" -c "$source" \
		-o "$object"
done
"$int21_cc" -m64 -nostdlib -no-pie -Wl,-e,_start -Wl,--build-id=none \
	"$temporary_dir/machine.o" "$temporary_dir/memory.o" \
	"$temporary_dir/resize.o" \
	"$temporary_dir/error.o" "$temporary_dir/vectors.o" \
	"$temporary_dir/process_runtime.o" \
	"$temporary_dir/record.o" \
	"$temporary_dir/package.o" \
	"$temporary_dir/sft_adapter.o" \
	"$temporary_dir/device.o" \
	"$temporary_dir/config.o" \
	"$temporary_dir/int21.o" \
	"$temporary_dir/dos_int21_test.o" -o "$test_elf"
"$test_elf"

# Compile every participant for the first-stage i386 data model as well.
for source in \
	"$c32_dir/dos/machine.c" \
	"$c32_dir/dos/memory.c" \
	"$c32_dir/dos/jft/resize.c" \
	"$c32_dir/dos/error.c" \
	"$c32_dir/dos/vectors.c" \
	"$c32_dir/dos/process_runtime.c" \
	"$c32_dir/dos/find/record.c" \
	"$c32_dir/dos/nls/package.c" \
	"$c32_dir/dos/sft_adapter.c" \
	"$c32_dir/storage/core/device.c" \
	"$c32_dir/dos/drive/config.c" \
	"$c32_dir/dos/int21.c" \
	"$test_dir/dos_int21_test.c"
do
	object="$temporary_dir/$(basename "${source%.c}")-32.o"
	# shellcheck disable=SC2086 # Deliberate compiler argument lists.
	"$int21_cc" $common32_flags -I"$c32_dir/include" -c "$source" \
		-o "$object"
done

echo "dos-int21 tests passed: ABI, unified CurrentPDB runtime, memory, errors, BadCall and composition"

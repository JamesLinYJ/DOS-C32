#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
c32_dir=$(CDPATH= cd -- "$test_dir/.." && pwd)
termination_cc=${CC:-gcc}
temporary_dir=$(mktemp -d "${TMPDIR:-/tmp}/dos-c32-termination.XXXXXX")

cleanup()
{
	rm -f -- "$temporary_dir"/*.o \
		"$temporary_dir"/dos-termination-m32 \
		"$temporary_dir"/dos-termination-m64
	rmdir -- "$temporary_dir"
}
trap cleanup EXIT HUP INT TERM

common_flags="-std=gnu11 -O2 -ffreestanding -fno-builtin -fno-pie -fno-pic -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables -Wall -Wextra -Werror -Wundef -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wvla -Wformat=2 -Wcast-align=strict -Wnull-dereference"

for model in 32 64
do
	case "$model" in
	32) architecture_flags="-m32 -march=i386 -msoft-float -mno-mmx -mno-sse -mno-sse2" ;;
	64) architecture_flags="-m64 -march=x86-64" ;;
	esac
	for source in \
		"$c32_dir/dos/machine.c" \
		"$c32_dir/dos/memory.c" \
		"$c32_dir/dos/process_runtime.c" \
		"$c32_dir/dos/termination.c" \
		"$test_dir/dos_termination_test.c"
	do
		object="$temporary_dir/$(basename "${source%.c}")-$model.o"
		# shellcheck disable=SC2086 # Deliberate compiler argument lists.
		"$termination_cc" $architecture_flags $common_flags \
			-I"$c32_dir/include" -I"$test_dir" -c "$source" \
			-o "$object"
	done
	# shellcheck disable=SC2086 # Deliberate compiler argument lists.
	"$termination_cc" $architecture_flags -nostdlib -static -no-pie \
		-Wl,-e,_start -Wl,--build-id=none \
		"$temporary_dir/machine-$model.o" \
		"$temporary_dir/memory-$model.o" \
		"$temporary_dir/process_runtime-$model.o" \
		"$temporary_dir/termination-$model.o" \
		"$temporary_dir/dos_termination_test-$model.o" \
		-o "$temporary_dir/dos-termination-m$model"
done

"$temporary_dir/dos-termination-m64"
if command -v qemu-i386 >/dev/null 2>&1; then
	qemu-i386 "$temporary_dir/dos-termination-m32"
fi

echo "DOS termination tests passed: full dynamic JFT preflight, bounded reverse close and poison"

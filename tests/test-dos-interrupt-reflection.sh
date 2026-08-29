#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
c32_dir=$(CDPATH= cd -- "$test_dir/.." && pwd)
reflection_cc=${CC:-gcc}
temporary_dir=$(mktemp -d "${TMPDIR:-/tmp}/dos-c32-reflection.XXXXXX")

cleanup()
{
	rm -f -- "$temporary_dir"/*.o "$temporary_dir"/reflection-test-*
	rmdir -- "$temporary_dir"
}
trap cleanup EXIT HUP INT TERM

common_flags="-std=gnu11 -O2 -ffreestanding -fno-builtin -fno-pie -fno-pic -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables -Wall -Wextra -Werror -Wundef -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wvla -Wformat=2 -Wcast-align=strict -Wnull-dereference"

for model in 64 32
do
	objects=""
	for source in "$c32_dir/dos/machine.c" \
		"$c32_dir/dos/interrupt_reflection.c" \
		"$test_dir/dos_interrupt_reflection_test.c"
	do
		object="$temporary_dir/$(basename "${source%.c}").m$model.o"
		# shellcheck disable=SC2086 # Deliberate compiler argument list.
		"$reflection_cc" -m$model $common_flags -I"$c32_dir/include" \
			-c "$source" -o "$object"
		objects="$objects $object"
	done
	# shellcheck disable=SC2086 # Deliberate object list.
	"$reflection_cc" -m$model -nostdlib -no-pie -Wl,-e,_start \
		-Wl,--build-id=none $objects \
		-o "$temporary_dir/reflection-test-$model"
done

"$temporary_dir/reflection-test-64"
if command -v qemu-i386 >/dev/null 2>&1
then
	qemu-i386 "$temporary_dir/reflection-test-32"
fi

echo "DOS interrupt reflection tests passed: IVT, IRET frame, wraps and rollback"

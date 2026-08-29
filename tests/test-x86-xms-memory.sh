#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
c32_dir=$(CDPATH= cd -- "$test_dir/.." && pwd)
test_cc=${CC:-gcc}
temporary_dir=$(mktemp -d "${TMPDIR:-/tmp}/dos-c32-x86-xms.XXXXXX")

cleanup()
{
	rm -f -- "$temporary_dir"/*.o "$temporary_dir"/x86-xms-memory-*
	rmdir -- "$temporary_dir"
}
trap cleanup EXIT HUP INT TERM

common_flags="-std=gnu11 -O2 -ffreestanding -fno-builtin -fno-pie -fno-pic -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables -Wall -Wextra -Werror -Wundef -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wvla -Wformat=2 -Wcast-align=strict -Wnull-dereference"
memory_floor=${X86_BOOT_IDENTITY_FLOOR:-0x00800000}
memory_ceiling=${X86_BOOT_IDENTITY_CEILING:-0x10000000}
memory_config="-DCONFIG_X86_BOOT_IDENTITY_FLOOR=$memory_floor -DCONFIG_X86_BOOT_IDENTITY_CEILING=$memory_ceiling"

for model in 64 32
do
	case "$model" in
	32) architecture_flags="-m32 -march=i386 -msoft-float -mno-mmx -mno-sse -mno-sse2" ;;
	64) architecture_flags="-m64 -march=x86-64" ;;
	esac
	objects=""
	for source in "$c32_dir/libc32/string.c" \
		"$c32_dir/kernel/x86_vm/memory/xms.c" \
		"$test_dir/x86_xms_memory_test.c"
	do
		object="$temporary_dir/$(basename "${source%.c}").m$model.o"
		# shellcheck disable=SC2086 # Deliberate compiler argument list.
		"$test_cc" $architecture_flags $common_flags $memory_config \
			-I"$c32_dir/include" -I"$test_dir" \
			-c "$source" -o "$object"
		objects="$objects $object"
	done
	# shellcheck disable=SC2086 # Deliberate object list.
	"$test_cc" $architecture_flags -nostdlib -static -no-pie \
		-Wl,-e,_start -Wl,--build-id=none $objects \
		-o "$temporary_dir/x86-xms-memory-$model"
done

"$temporary_dir/x86-xms-memory-64"
if command -v qemu-i386 >/dev/null 2>&1
then
	qemu-i386 "$temporary_dir/x86-xms-memory-32"
fi

echo "x86 XMS adapter tests passed: exact active HMA mapping (m32/m64)"

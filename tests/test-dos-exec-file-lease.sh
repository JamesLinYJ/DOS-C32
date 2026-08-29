#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
c32_dir=$(CDPATH= cd -- "$test_dir/.." && pwd)
lease_cc=${CC:-gcc}
temporary_dir=$(mktemp -d "${TMPDIR:-/tmp}/dos-c32-exec-file-lease.XXXXXX")

cleanup()
{
	rm -f -- "$temporary_dir"/*.o "$temporary_dir"/dos-exec-file-lease-test-*
	rmdir -- "$temporary_dir"
}
trap cleanup EXIT HUP INT TERM

warning_flags="-std=gnu11 -O2 -ffreestanding -fno-builtin -fno-pie -fno-pic -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables -Wall -Wextra -Werror -Wundef -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wvla -Wformat=2 -Wcast-align=strict -Wnull-dereference"

for model in 64 32
do
	objects=""
	for source in "$c32_dir/dos/exec_file_lease.c" \
		"$test_dir/dos_exec_file_lease_test.c"
	do
		object="$temporary_dir/$(basename "${source%.c}").m$model.o"
		# shellcheck disable=SC2086 # Deliberate compiler argument list.
		"$lease_cc" -m$model $warning_flags -I"$c32_dir/include" \
			-c "$source" -o "$object"
		objects="$objects $object"
	done
	# shellcheck disable=SC2086 # Deliberate object list.
	"$lease_cc" -m$model -nostdlib -no-pie -Wl,-e,_start \
		-Wl,--build-id=none $objects \
		-o "$temporary_dir/dos-exec-file-lease-test-$model"
done

"$temporary_dir/dos-exec-file-lease-test-64"

echo "dos-exec-file-lease tests passed: OPEN detail, IOCTL/read/CLOSE, device, retained/uncertain, identity and ABA (m64 run, m32 link)"

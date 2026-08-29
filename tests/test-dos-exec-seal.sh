#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
c32_dir=$(CDPATH= cd -- "$test_dir/.." && pwd)
seal_cc=${CC:-gcc}
temporary_dir=$(mktemp -d "${TMPDIR:-/tmp}/dos-c32-exec-seal.XXXXXX")

cleanup()
{
	rm -f -- "$temporary_dir"/*.o "$temporary_dir"/dos-exec-seal-test-*
	rmdir -- "$temporary_dir"
}
trap cleanup EXIT HUP INT TERM

common_flags="-std=gnu11 -O2 -ffreestanding -fno-builtin -fno-pie -fno-pic -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables -Wall -Wextra -Werror -Wundef -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wvla -Wformat=2 -Wcast-align=strict -Wnull-dereference"

for model in 64 32
do
	objects=""
	for source in "$c32_dir/dos/machine.c" \
		"$c32_dir/dos/memory.c" \
		"$c32_dir/dos/memory_lease.c" \
		"$c32_dir/dos/process_runtime.c" \
		"$c32_dir/dos/exec_observer.c" \
		"$c32_dir/dos/exec_journal.c" \
		"$c32_dir/dos/sft_batch.c" \
		"$c32_dir/dos/exec_seal.c" \
		"$test_dir/dos_exec_seal_test.c"
	do
		object="$temporary_dir/$(basename "${source%.c}").m$model.o"
		# shellcheck disable=SC2086 # Deliberate compiler argument list.
		"$seal_cc" -m$model $common_flags -I"$c32_dir/include" \
			-c "$source" -o "$object"
		objects="$objects $object"
	done
	# shellcheck disable=SC2086 # Deliberate object list.
	"$seal_cc" -m$model -nostdlib -no-pie -Wl,-e,_start \
		-Wl,--build-id=none $objects \
		-o "$temporary_dir/dos-exec-seal-test-$model"
done

"$temporary_dir/dos-exec-seal-test-64"

echo "dos-exec-seal tests passed: pure preflight, ordered publish, poison (m64 run, m32 link)"

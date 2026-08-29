#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
c32_dir=$(CDPATH= cd -- "$test_dir/.." && pwd)
transaction_cc=${CC:-gcc}
temporary_dir=$(mktemp -d "${TMPDIR:-/tmp}/dos-c32-exec-transaction.XXXXXX")

cleanup()
{
	rm -f -- "$temporary_dir"/*.o "$temporary_dir"/dos-exec-transaction-test-*
	rmdir -- "$temporary_dir"
}
trap cleanup EXIT HUP INT TERM

warning_flags="-std=gnu11 -O2 -ffreestanding -fno-builtin -fno-pie -fno-pic -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables -Wall -Wextra -Werror -Wundef -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wvla -Wformat=2 -Wcast-align=strict -Wnull-dereference"

for model in 64 32
do
	objects=""
	for source in "$c32_dir/dos/machine.c" \
		"$c32_dir/dos/loader.c" \
		"$c32_dir/dos/memory.c" \
		"$c32_dir/dos/memory_lease.c" \
		"$c32_dir/dos/environment.c" \
		"$c32_dir/dos/exec_backend_session.c" \
		"$c32_dir/dos/exec_handoff.c" \
		"$c32_dir/dos/exec_journal.c" \
		"$c32_dir/dos/exec_name.c" \
		"$c32_dir/dos/exec_parameter.c" \
		"$c32_dir/dos/exec_seal.c" \
		"$c32_dir/dos/exec_transaction.c" \
		"$c32_dir/dos/exec_executor.c" \
		"$c32_dir/dos/exec_int21.c" \
		"$c32_dir/dos/exec_native.c" \
		"$c32_dir/dos/exec_file_lease.c" \
		"$c32_dir/dos/exec_observer.c" \
		"$c32_dir/dos/image_load.c" \
		"$c32_dir/dos/process.c" \
		"$c32_dir/dos/process_runtime.c" \
		"$c32_dir/dos/relocator.c" \
		"$c32_dir/dos/sft_batch.c" \
		"$c32_dir/libc32/assert.c" \
		"$c32_dir/libc32/string.c" \
		"$test_dir/dos_exec_transaction_test.c"
	do
		object="$temporary_dir/$(basename "${source%.c}").m$model.o"
		# shellcheck disable=SC2086 # Deliberate compiler argument list.
		"$transaction_cc" -m$model $warning_flags -I"$c32_dir/include" \
			-c "$source" -o "$object"
		objects="$objects $object"
	done
	# shellcheck disable=SC2086 # Deliberate object list.
	"$transaction_cc" -m$model -nostdlib -no-pie -Wl,-e,_start \
		-Wl,--build-id=none $objects \
		-o "$temporary_dir/dos-exec-transaction-test-$model"
done

"$temporary_dir/dos-exec-transaction-test-64"

echo "dos-exec-transaction tests passed: ordered load through EXEC1 seal and EXEC0 handoff, batched PSP, reverse abort, retirement, identities and poison (m64 run, m32 link)"

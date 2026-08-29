#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
c32_dir=$(CDPATH= cd -- "$test_dir/.." && pwd)
session_cc=${CC:-gcc}
temporary_dir=$(mktemp -d "${TMPDIR:-/tmp}/dos-c32-backend-session.XXXXXX")

cleanup()
{
	status=$?
	trap - EXIT HUP INT TERM
	rm -f -- "$temporary_dir"/*.o "$temporary_dir"/backend-session-test-* || \
		status=$?
	rmdir -- "$temporary_dir" || status=$?
	exit "$status"
}
trap cleanup EXIT HUP INT TERM

common_flags="-std=gnu11 -O2 -ffreestanding -fno-builtin -fno-pie -fno-pic -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables -Wall -Wextra -Werror -Wundef -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wvla -Wformat=2 -Wcast-align=strict -Wnull-dereference -DCONFIG_DOS_XMS_HMA_MINIMUM_BYTES=0"

for model in 64 32
do
	objects=""
	for source in "$c32_dir/libc32/string.c" \
		"$c32_dir/dos/machine.c" \
		"$c32_dir/dos/exec_journal.c" \
		"$c32_dir/dos/exec_handoff.c" \
		"$c32_dir/dos/exec_backend_session.c" \
		"$c32_dir/dos/memory.c" \
		"$c32_dir/dos/error.c" \
		"$c32_dir/dos/vectors.c" \
		"$c32_dir/dos/interrupt_reflection.c" \
		"$c32_dir/dos/process_runtime.c" \
		"$c32_dir/dos/find/record.c" \
		"$c32_dir/dos/xms/hma.c" \
		"$c32_dir/dos/xms/manager.c" \
		"$c32_dir/dos/nls/package.c" \
		"$c32_dir/dos/jft/resize.c" \
		"$c32_dir/dos/sft_adapter.c" \
		"$c32_dir/storage/core/device.c" \
	"$c32_dir/dos/drive/config.c" \
	"$c32_dir/dos/int21.c" \
		"$c32_dir/dos/ems/core.c" \
		"$c32_dir/dos/ems/vcpi.c" \
		"$c32_dir/dos/personality.c" \
		"$c32_dir/dos/execution_loop.c" \
		"$test_dir/dos_exec_backend_session_test.c"
	do
		object="$temporary_dir/$(basename "${source%.c}").m$model.o"
		# shellcheck disable=SC2086 # Deliberate compiler argument list.
		"$session_cc" -m$model $common_flags -I"$c32_dir/include" \
			-c "$source" -o "$object"
		objects="$objects $object"
	done
	# shellcheck disable=SC2086 # Deliberate object list.
	"$session_cc" -m$model -nostdlib -no-pie -Wl,-e,_start \
		-Wl,--build-id=none $objects \
		-o "$temporary_dir/backend-session-test-$model"
done

"$temporary_dir/backend-session-test-64"

echo "dos-exec-backend-session tests passed: DOS/A20/port resume, typed EMS transfer, ABA and poison (m64 run, m32 link)"

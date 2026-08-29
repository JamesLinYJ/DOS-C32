#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
c32_dir=$(CDPATH= cd -- "$test_dir/.." && pwd)
test_cc=${CC:-gcc}
temporary_dir=$(mktemp -d "${TMPDIR:-/tmp}/dos-c32-runtime-owner.XXXXXX")

cleanup()
{
	rm -f -- "$temporary_dir"/*.o "$temporary_dir"/runtime-owner-*
	rmdir -- "$temporary_dir"
}
trap cleanup EXIT HUP INT TERM

common_flags="-std=gnu11 -O2 -ffreestanding -fno-builtin -fno-pie -fno-pic -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables -Wall -Wextra -Werror -Wundef -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wvla -Wformat=2 -Wcast-align=strict -Wnull-dereference -DCONFIG_DOS_XMS_HMA_MINIMUM_BYTES=0"

for model in 64 32
do
	case "$model" in
	32) architecture_flags="-m32 -march=i386 -msoft-float -mno-mmx -mno-sse -mno-sse2" ;;
	64) architecture_flags="-m64 -march=x86-64" ;;
	esac
	objects=""
	for source in \
		"$c32_dir/libc32/assert.c" \
		"$c32_dir/libc32/string.c" \
		"$c32_dir/dos/machine.c" \
		"$c32_dir/dos/error.c" \
		"$c32_dir/dos/loader.c" \
		"$c32_dir/dos/memory.c" \
		"$c32_dir/dos/memory_lease.c" \
		"$c32_dir/dos/environment.c" \
		"$c32_dir/dos/exec_backend_session.c" \
		"$c32_dir/dos/exec_handoff.c" \
		"$c32_dir/dos/exec_journal.c" \
		"$c32_dir/dos/exec_name.c" \
		"$c32_dir/dos/exec_parameter.c" \
		"$c32_dir/dos/exec_overlay.c" \
		"$c32_dir/dos/exec_seal.c" \
		"$c32_dir/dos/exec_transaction.c" \
		"$c32_dir/dos/exec_file_lease.c" \
		"$c32_dir/dos/exec_observer.c" \
		"$c32_dir/dos/image_load.c" \
		"$c32_dir/dos/process.c" \
		"$c32_dir/dos/process_runtime.c" \
		"$c32_dir/dos/relocator.c" \
		"$c32_dir/dos/sft_batch.c" \
		"$c32_dir/dos/sft_adapter.c" \
		"$c32_dir/dos/vectors.c" \
		"$c32_dir/dos/find/record.c" \
		"$c32_dir/dos/xms/hma.c" \
		"$c32_dir/dos/xms/manager.c" \
		"$c32_dir/dos/nls/package.c" \
		"$c32_dir/dos/jft/resize.c" \
		"$c32_dir/storage/core/device.c" \
	"$c32_dir/dos/drive/config.c" \
	"$c32_dir/dos/int21.c" \
		"$c32_dir/dos/ems/core.c" \
		"$c32_dir/dos/ems/vcpi.c" \
		"$c32_dir/dos/personality.c" \
		"$c32_dir/dos/runtime_owner.c" \
		"$test_dir/dos_runtime_owner_test.c"
	do
		object="$temporary_dir/$(basename "${source%.c}").m$model.o"
		# shellcheck disable=SC2086 # Deliberate compiler argument list.
		"$test_cc" $architecture_flags $common_flags -I"$c32_dir/include" \
			-c "$source" -o "$object"
		objects="$objects $object"
	done
	# shellcheck disable=SC2086 # Deliberate object list.
	"$test_cc" $architecture_flags -nostdlib -static -no-pie \
		-Wl,-e,_start -Wl,--build-id=none $objects \
		-o "$temporary_dir/runtime-owner-$model"
done

"$temporary_dir/runtime-owner-64"
if command -v qemu-i386 >/dev/null 2>&1
then
	qemu-i386 "$temporary_dir/runtime-owner-32"
fi

echo "DOS runtime owner tests passed: initial MCB/PDB and unified EXEC services"

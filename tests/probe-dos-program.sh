#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

if [ "$#" -ne 1 ]; then
	echo "usage: $0 DOS_PROGRAM" >&2
	exit 2
fi

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
c32_dir=$(CDPATH= cd -- "$test_dir/.." && pwd)
program=$1
probe_cc=${CC:-gcc}
probe_ld=${LD:-ld}
temporary_dir=$(mktemp -d "${TMPDIR:-/tmp}/dos-c32-program-probe.XXXXXX")

cleanup()
{
	rm -f -- "$temporary_dir"/*
	rmdir -- "$temporary_dir"
}
trap cleanup EXIT HUP INT TERM

if [ ! -f "$program" ] || [ ! -r "$program" ]; then
	echo "DOS program is not a readable regular file: $program" >&2
	exit 2
fi

cp -- "$program" "$temporary_dir/dos_program"

common_flags="-std=gnu11 -O2 -ffreestanding -fno-builtin -fno-pie -fno-pic -fno-stack-protector -ffunction-sections -fdata-sections -fno-asynchronous-unwind-tables -fno-unwind-tables -Wall -Wextra -Werror -Wundef -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wvla -Wformat=2 -Wcast-align=strict -Wnull-dereference"
sources="dos/machine.c dos/loader.c dos/process.c dos/image_load.c dos/relocator.c dos/exec_handoff.c tests/dos_program_probe.c"

for model in 32 64
do
	case "$model" in
	32)
		architecture_flags="-m32 -march=i386 -msoft-float -mno-mmx -mno-sse -mno-sse2"
		binary_emulation=elf_i386
		;;
	64)
		architecture_flags="-m64 -march=x86-64"
		binary_emulation=elf_x86_64
		;;
	esac
	objects=
	for source in $sources
	do
		object="$temporary_dir/$(basename "${source%.c}")-$model.o"
		# shellcheck disable=SC2086 # Deliberate compiler argument lists.
		"$probe_cc" $architecture_flags $common_flags \
			-I"$c32_dir/include" -c "$c32_dir/$source" -o "$object"
		objects="$objects $object"
	done
	(
		cd "$temporary_dir"
		"$probe_ld" -r -b binary -m "$binary_emulation" \
			-o "dos-program-$model.o" dos_program
	)
	# shellcheck disable=SC2086 # Deliberate compiler argument lists.
	"$probe_cc" $architecture_flags -nostdlib -static -no-pie \
		-Wl,-e,_start -Wl,--build-id=none -Wl,--gc-sections \
		$objects "$temporary_dir/dos-program-$model.o" \
		-o "$temporary_dir/dos-program-probe-m$model"
done

set +e
"$temporary_dir/dos-program-probe-m64"
probe_status=$?
set -e
if [ "$probe_status" -ne 0 ]; then
	case "$probe_status" in
	1) stage="fixture extent" ;;
	2) stage="COM/MZ classification" ;;
	3) stage="inspected-plan validation" ;;
	4) stage="DOS allocation policy" ;;
	5) stage="process geometry" ;;
	6) stage="resident image load" ;;
	7) stage="initial stack validation" ;;
	8) stage="MZ relocation" ;;
	9) stage="EXEC0 handoff" ;;
	10) stage="load-result validation" ;;
	*) stage="unknown probe stage" ;;
	esac
	echo "DOS program probe failed during $stage (status $probe_status)" >&2
	exit 1
fi

if command -v qemu-i386 >/dev/null 2>&1; then
	qemu-i386 "$temporary_dir/dos-program-probe-m32"
fi

echo "DOS program fixture: $(sha256sum "$program" | awk '{print $1}')"
echo "EXEC probe passed: classify, allocate, load, relocate, validate stack, prepare handoff"
echo "Execution, interrupt reflection, and DOS service coverage are separate pending gates."

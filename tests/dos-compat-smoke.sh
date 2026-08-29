#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

if [ "$#" -ne 2 ]; then
	echo "usage: $0 IMAGE ARENA.EXE" >&2
	exit 2
fi

image=$1
program=$2
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
mcopy=${MCOPY:-mcopy}
if [ ! -f "$program" ]; then
	echo "compatibility program is unavailable: $program" >&2
	exit 2
fi
if ! command -v qemu-system-i386 >/dev/null 2>&1; then
	echo "DOS compatibility smoke test skipped: qemu-system-i386 is unavailable"
	exit 0
fi

temporary_dir=$(mktemp -d "${TMPDIR:-/tmp}/dos-c32-compat.XXXXXX")
test_image="$temporary_dir/msdos.img"
autoexec="$temporary_dir/AUTOEXEC.BAT"
serial_log="$temporary_dir/serial.log"
qemu_display=${QEMU_DISPLAY:-}
qemu_memory_mib=${QEMU_MEMORY_MIB:-256}

if [ -z "$qemu_display" ]; then
	case "$(uname -s)" in
	Darwin) qemu_display="cocoa,show-cursor=on" ;;
	*) qemu_display=none ;;
	esac
fi

cleanup()
{
	rm -f -- "$test_image" "$autoexec" "$serial_log"
	rmdir -- "$temporary_dir"
}
trap cleanup EXIT HUP INT TERM

cp -- "$image" "$test_image"
printf '@ECHO OFF\nARENA\n' > "$autoexec"
"$mcopy" -o -i "$test_image" "$program" ::/ARENA.EXE
"$mcopy" -o -i "$test_image" "$autoexec" ::/AUTOEXEC.BAT

set +e
TMPDIR="$temporary_dir" "$script_dir/run-with-timeout.sh" 8 \
	qemu-system-i386 -machine pc,accel=tcg -m "${qemu_memory_mib}M" \
	-drive "file=$test_image,format=raw,if=ide,index=0,media=disk,snapshot=on" \
	-boot c -display "$qemu_display" -monitor none -no-reboot -no-shutdown \
	-serial "file:$serial_log" >/dev/null 2>&1
qemu_status=$?
set -e

if [ "$qemu_status" -ne 0 ] && [ "$qemu_status" -ne 124 ]; then
	echo "QEMU failed during DOS compatibility test: status $qemu_status" >&2
	exit 1
fi
for expected in "Block 1001 owner 0000 size 0010" \
	"Block 1012    free    size 0013" "C:\\>"; do
	if ! grep -Fq "$expected" "$serial_log"; then
		echo "DOS compatibility output is missing: $expected" >&2
		sed -n '1,240p' "$serial_log" >&2
		exit 1
	fi
done
for forbidden in "EXEC diagnostic:" "X86 execution diagnostic:" "Program stopped" \
	"Memory arena is trashed"; do
	if grep -Fq "$forbidden" "$serial_log"; then
		echo "DOS compatibility output contains failure: $forbidden" >&2
		sed -n '1,240p' "$serial_log" >&2
		exit 1
	fi
done

echo "DOS compatibility smoke test passed: ARENA.EXE enumerated the live MCB chain"

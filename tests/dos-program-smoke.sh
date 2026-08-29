#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

if [ "$#" -ne 2 ]; then
	echo "usage: $0 IMAGE PROGRAM.COM" >&2
	exit 2
fi

image=$1
program=$2
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
mcopy=${MCOPY:-mcopy}
if ! command -v qemu-system-i386 >/dev/null 2>&1; then
	echo "DOS program smoke test skipped: qemu-system-i386 is unavailable"
	exit 0
fi

temporary_dir=$(mktemp -d "${TMPDIR:-/tmp}/dos-c32-program.XXXXXX")
test_image="$temporary_dir/program.img"
autoexec="$temporary_dir/AUTOEXEC.BAT"
serial_log="$temporary_dir/serial.log"
debug_log="$temporary_dir/debugcon.log"
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
	rm -f -- "$test_image" "$autoexec" "$serial_log" "$debug_log"
	rmdir -- "$temporary_dir"
}
trap cleanup EXIT HUP INT TERM

cp -- "$image" "$test_image"
printf '@ECHO OFF\nHELLO\n' > "$autoexec"
"$mcopy" -o -i "$test_image" "$program" ::/HELLO.COM
"$mcopy" -o -i "$test_image" "$autoexec" ::/AUTOEXEC.BAT

set +e
TMPDIR="$temporary_dir" "$script_dir/run-with-timeout.sh" 8 \
	qemu-system-i386 -machine pc,accel=tcg -m "${qemu_memory_mib}M" \
	-drive "file=$test_image,format=raw,if=ide,index=0,media=disk,snapshot=on" \
	-boot c -display "$qemu_display" -monitor none -no-reboot -no-shutdown \
	-serial "file:$serial_log" -debugcon "file:$debug_log" \
	-global isa-debugcon.iobase=0xe9 >/dev/null 2>&1
qemu_status=$?
set -e

if [ "$qemu_status" -ne 0 ] && [ "$qemu_status" -ne 124 ]; then
	echo "QEMU failed during DOS program smoke test: status $qemu_status" >&2
	exit 1
fi
for expected in "DOS-C32 REAL COM OK" "C:\\>"; do
	if ! grep -Fq "$expected" "$serial_log"; then
		echo "DOS program output is missing: $expected" >&2
		sed -n '1,200p' "$serial_log" >&2
		exit 1
	fi
done
for forbidden in "DOS-C32 REAL COM FAILED" "EXEC diagnostic:" \
	"X86 execution diagnostic:" "Program stopped"; do
	if grep -Fq "$forbidden" "$serial_log"; then
		echo "DOS program output contains failure: $forbidden" >&2
		sed -n '1,200p' "$serial_log" >&2
		exit 1
	fi
done

echo "DOS program smoke test passed: COM/PSP/MCB/INT21/VM86/termination"

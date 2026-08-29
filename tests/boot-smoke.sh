#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

if [ "$#" -ne 1 ]; then
	echo "usage: $0 IMAGE" >&2
	exit 2
fi

image=$1
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if ! command -v qemu-system-i386 >/dev/null 2>&1; then
	echo "boot smoke test skipped: qemu-system-i386 is unavailable"
	exit 0
fi

temporary_dir=$(mktemp -d "${TMPDIR:-/tmp}/dos-c32-boot.XXXXXX")
serial_log="$temporary_dir/serial.log"
debug_log="$temporary_dir/debugcon.log"
monitor_socket="$temporary_dir/monitor.sock"
qemu_display=${QEMU_DISPLAY:-}
qemu_memory_mib=${QEMU_MEMORY_MIB:-256}
identity_floor=${X86_BOOT_IDENTITY_FLOOR:-0x00800000}
identity_ceiling=${X86_BOOT_IDENTITY_CEILING:-0x10000000}

identity_floor_mib=$((identity_floor / 1024 / 1024))
identity_ceiling_mib=$((identity_ceiling / 1024 / 1024))
expected_identity_mib=$qemu_memory_mib
if [ "$expected_identity_mib" -lt "$identity_floor_mib" ]; then
	expected_identity_mib=$identity_floor_mib
fi
expected_identity_mib=$(((expected_identity_mib + 3) / 4 * 4))
if [ "$expected_identity_mib" -gt "$identity_ceiling_mib" ]; then
	expected_identity_mib=$identity_ceiling_mib
fi

if [ -z "$qemu_display" ]; then
	case "$(uname -s)" in
	Darwin) qemu_display="cocoa,show-cursor=on" ;;
	*) qemu_display=none ;;
	esac
fi

cleanup()
{
	rm -f -- "$serial_log" "$debug_log" "$monitor_socket"
	rmdir -- "$temporary_dir"
}
trap cleanup EXIT HUP INT TERM

set +e
TMPDIR="$temporary_dir" "$script_dir/run-with-timeout.sh" 8 qemu-system-i386 \
	-machine pc,accel=tcg \
	-m "${qemu_memory_mib}M" \
	-drive "file=$image,format=raw,if=ide,index=0,media=disk,snapshot=on" \
	-boot c \
	-display "$qemu_display" \
	-monitor "unix:$monitor_socket,server=on,wait=off" \
	-no-reboot \
	-no-shutdown \
	-serial "file:$serial_log" \
	-debugcon "file:$debug_log" \
	-global isa-debugcon.iobase=0xe9 \
	>/dev/null 2>&1
qemu_status=$?
set -e

if [ "$qemu_status" -ne 0 ] && [ "$qemu_status" -ne 124 ]; then
	echo "QEMU failed during boot smoke test: status $qemu_status" >&2
	exit 1
fi

for expected in \
	"DOS-C32: an MS-DOS-compatible protected-mode system" \
	"Platform: 32-bit i386, Legacy BIOS | License: GPL-2.0-only" \
	"DOS-C32 compatibility target: MS-DOS" \
	"Memory: detected " \
	"Memory: managed " \
	"identity map $expected_identity_mib MiB" \
	"Volume label for drive C: DOSC32" \
	"COMMAND.COM" \
	"README.TXT" \
	"AUTOEXEC.BAT" \
	"WELCOME.TXT" \
	"Files: 5" \
	"C:\\>"
do
	if ! grep -Fq "$expected" "$serial_log"; then
		echo "boot output is missing: $expected" >&2
		sed -n '1,160p' "$serial_log" >&2
		exit 1
	fi
done

for forbidden in "Boot failed:" "Could not mount the boot volume:" \
	"kernel stack corruption"; do
	if grep -Fq "$forbidden" "$serial_log"; then
		echo "boot output contains failure: $forbidden" >&2
		exit 1
	fi
done

if ! grep -Fq "DOS-C32: an MS-DOS-compatible protected-mode system" \
	"$debug_log"; then
	echo "port E9 debug mirror did not receive the kernel banner" >&2
	exit 1
fi

echo "boot smoke test passed: SeaBIOS/ATA/FAT16/AUTOEXEC/COMMAND"

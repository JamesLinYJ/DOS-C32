#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Prove that Ring-3 COMMAND can synchronously launch and resume after VM86.
set -eu

if [ "$#" -ne 2 ]; then
	echo "usage: $0 IMAGE PROGRAM.COM" >&2
	exit 2
fi

image=$1
program=$2
mcopy=${MCOPY:-mcopy}
mmd=${MMD:-mmd}
if ! command -v qemu-system-i386 >/dev/null 2>&1; then
	echo "C32 COMMAND/VM86 test skipped: qemu-system-i386 is unavailable"
	exit 0
fi
if ! command -v nc >/dev/null 2>&1; then
	echo "C32 COMMAND/VM86 test skipped: nc with Unix sockets is unavailable"
	exit 0
fi

temporary_dir=$(mktemp -d "${TMPDIR:-/tmp}/dos-c32-command.XXXXXX")
test_image="$temporary_dir/command.img"
autoexec="$temporary_dir/AUTOEXEC.BAT"
batch_file="$temporary_dir/RUNTEST.BAT"
serial_log="$temporary_dir/serial.log"
debug_log="$temporary_dir/debugcon.log"
monitor_socket="$temporary_dir/monitor.sock"
qemu_pid=
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
	if [ -n "$qemu_pid" ] && kill -0 "$qemu_pid" 2>/dev/null; then
		kill -TERM "$qemu_pid" 2>/dev/null || true
		wait "$qemu_pid" 2>/dev/null || true
	fi
	rm -f -- "$test_image" "$autoexec" "$serial_log" "$debug_log" \
		"$monitor_socket" "$batch_file"
	rmdir -- "$temporary_dir"
}
trap cleanup EXIT HUP INT TERM

cp -- "$image" "$test_image"
printf '@ECHO OFF\n' > "$autoexec"
printf '@ECHO OFF\r\nCD \\TESTDIR\r\nHELLO\r\n' > "$batch_file"
"$mmd" -i "$test_image" ::/TESTDIR
"$mcopy" -o -i "$test_image" "$program" ::/TESTDIR/HELLO.COM
"$mcopy" -o -i "$test_image" "$batch_file" ::/RUNTEST.BAT
"$mcopy" -o -i "$test_image" "$autoexec" ::/AUTOEXEC.BAT

qemu-system-i386 -machine pc,accel=tcg -m "${qemu_memory_mib}M" \
	-drive "file=$test_image,format=raw,if=ide,index=0,media=disk,snapshot=on" \
	-boot c -display "$qemu_display" \
	-monitor "unix:$monitor_socket,server=on,wait=off" \
	-no-reboot -no-shutdown -serial "file:$serial_log" \
	-debugcon "file:$debug_log" -global isa-debugcon.iobase=0xe9 \
	>/dev/null 2>&1 &
qemu_pid=$!

ready=0
attempt=0
while [ "$attempt" -lt 100 ]; do
	if [ -S "$monitor_socket" ] && [ -f "$serial_log" ] &&
	   grep -Fq "DOS-C32 COMMAND.COM (32-bit protected mode)" "$serial_log" &&
	   grep -Fq "C:\\>" "$serial_log"; then
		ready=1
		break
	fi
	if ! kill -0 "$qemu_pid" 2>/dev/null; then
		break
	fi
	attempt=$((attempt + 1))
	sleep 0.1
done
if [ "$ready" -ne 1 ]; then
	echo "C32 COMMAND did not reach its prompt" >&2
	sed -n '1,180p' "$serial_log" >&2 || true
	exit 1
fi

printf 'sendkey r\nsendkey u\nsendkey n\nsendkey t\nsendkey e\nsendkey s\nsendkey t\nsendkey ret\n' |
	nc -U -w 1 "$monitor_socket" >/dev/null 2>&1 || true

observed=0
attempt=0
while [ "$attempt" -lt 100 ]; do
	if grep -Fq "DOS-C32 REAL COM OK" "$serial_log"; then
		observed=1
		break
	fi
	if ! kill -0 "$qemu_pid" 2>/dev/null; then
		break
	fi
	attempt=$((attempt + 1))
	sleep 0.1
done
if [ "$observed" -ne 1 ]; then
	echo "Ring-3 COMMAND did not launch HELLO.COM through VM86" >&2
	sed -n '1,240p' "$serial_log" >&2
	exit 1
fi
if ! grep -Fq 'C:\TESTDIR>' "$serial_log"; then
	echo "Ring-3 COMMAND did not preserve BAT current-directory state" >&2
	exit 1
fi

# Run a second process generation from the restored COMMAND context. The Enter
# scan-code tail is intentionally delivered after focus changes to the guest,
# proving that a private firmware-page shadow from the first generation was
# revoked and can be acquired again without stale state.
printf 'sendkey h\nsendkey e\nsendkey l\nsendkey l\nsendkey o\nsendkey ret\n' |
	nc -U -w 1 "$monitor_socket" >/dev/null 2>&1 || true
observed=0
attempt=0
while [ "$attempt" -lt 100 ]; do
	if [ "$(grep -Fc "DOS-C32 REAL COM OK" "$serial_log" || true)" -ge 2 ]; then
		observed=1
		break
	fi
	if ! kill -0 "$qemu_pid" 2>/dev/null; then
		break
	fi
	attempt=$((attempt + 1))
	sleep 0.1
done
if [ "$observed" -ne 1 ]; then
	echo "VM86 firmware shadow did not survive release/reacquire" >&2
	sed -n '1,280p' "$serial_log" >&2
	exit 1
fi
for forbidden in "DOS-C32 REAL COM FAILED" "X86 execution diagnostic:" \
	"protected COMMAND fault"; do
	if grep -Fq "$forbidden" "$serial_log"; then
		echo "C32 COMMAND/VM86 output contains failure: $forbidden" >&2
		exit 1
	fi
done

echo "C32 COMMAND/VM86 test passed: BAT/CD -> VM86 -> firmware-shadow reacquire -> resume"

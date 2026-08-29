#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Exercise SeaBIOS INT 16h waiting, HLT wakeup and IRQ1 delivery in a real boot.
set -eu

if [ "$#" -ne 2 ]; then
	echo "usage: $0 IMAGE KEYIRQ.COM" >&2
	exit 2
fi

image=$1
program=$2
qemu=${QEMU_SYSTEM_I386:-qemu-system-i386}
mcopy=${MCOPY:-mcopy}

if ! command -v "$qemu" >/dev/null 2>&1; then
	echo "BIOS keyboard IRQ test skipped: $qemu is unavailable"
	exit 0
fi
if ! command -v nc >/dev/null 2>&1; then
	echo "BIOS keyboard IRQ test skipped: nc with Unix sockets is unavailable"
	exit 0
fi

temporary_dir=$(mktemp -d "${TMPDIR:-/tmp}/dos-c32-bios-key.XXXXXX")
test_image="$temporary_dir/bios-key.img"
autoexec="$temporary_dir/AUTOEXEC.BAT"
serial_log="$temporary_dir/serial.log"
debug_log="$temporary_dir/debugcon.log"
qemu_log="$temporary_dir/qemu.log"
monitor_log="$temporary_dir/monitor.log"
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

show_log()
{
	log_name=$1
	log_path=$2
	echo "--- $log_name ---" >&2
	if [ -s "$log_path" ]; then
		sed -n '1,260p' "$log_path" >&2
	else
		echo "(empty)" >&2
	fi
}

fail_test()
{
	echo "BIOS keyboard IRQ test failed: $1" >&2
	show_log "serial" "$serial_log"
	show_log "QEMU" "$qemu_log"
	show_log "monitor" "$monitor_log"
	show_log "debugcon" "$debug_log"
	exit 1
}

fail_if_qemu_exited()
{
	if kill -0 "$qemu_pid" 2>/dev/null; then
		return 0
	fi
	set +e
	wait "$qemu_pid"
	qemu_status=$?
	set -e
	qemu_pid=
	fail_test "$1; QEMU exited with status $qemu_status"
}

cleanup()
{
	if [ -n "$qemu_pid" ] && kill -0 "$qemu_pid" 2>/dev/null; then
		kill -TERM "$qemu_pid" 2>/dev/null || true
		wait "$qemu_pid" 2>/dev/null || true
	fi
	rm -f -- "$test_image" "$autoexec" "$serial_log" "$debug_log" \
		"$qemu_log" "$monitor_log" "$monitor_socket"
	rmdir -- "$temporary_dir"
}
trap cleanup EXIT HUP INT TERM

cp -- "$image" "$test_image"
printf '@ECHO OFF\r\nKEYIRQ.COM\r\n' > "$autoexec"
"$mcopy" -o -i "$test_image" "$program" ::/KEYIRQ.COM
"$mcopy" -o -i "$test_image" "$autoexec" ::/AUTOEXEC.BAT

"$qemu" -machine pc,accel=tcg -m "${qemu_memory_mib}M" \
	-drive "file=$test_image,format=raw,if=ide,index=0,media=disk,snapshot=on" \
	-boot c -display "$qemu_display" \
	-monitor "unix:$monitor_socket,server=on,wait=off" \
	-no-reboot -no-shutdown -serial "file:$serial_log" \
	-debugcon "file:$debug_log" -global isa-debugcon.iobase=0xe9 \
	>"$qemu_log" 2>&1 &
qemu_pid=$!

ready=0
attempt=0
while [ "$attempt" -lt 200 ]; do
	if [ -S "$monitor_socket" ] && [ -f "$serial_log" ] &&
	   grep -Fq "BIOS KEY IRQ READY" "$serial_log"; then
		ready=1
		break
	fi
	fail_if_qemu_exited "QEMU stopped before READY"
	attempt=$((attempt + 1))
	sleep 0.1
done
if [ "$ready" -ne 1 ]; then
	fail_test "READY was not observed within 20 seconds"
fi

# AUTOEXEC launches the COM without preceding keyboard input.  An outcome
# before this injection means INT 16h did not remain in its firmware wait.
sleep 0.2
if grep -Fq "BIOS KEY IRQ PASS" "$serial_log" ||
   grep -Fq "BIOS KEY IRQ FAIL" "$serial_log"; then
	fail_test "INT 16h returned before monitor input was injected"
fi

printf 'sendkey a\n' |
	nc -U -w 1 "$monitor_socket" >"$monitor_log" 2>&1 || true

observed=0
attempt=0
while [ "$attempt" -lt 100 ]; do
	if grep -Fq "BIOS KEY IRQ FAIL" "$serial_log"; then
		fail_test "COM reported a firmware-vector or key-value mismatch"
	fi
	if grep -Fq "BIOS KEY IRQ PASS AX=1E61" "$serial_log"; then
		observed=1
		break
	fi
	fail_if_qemu_exited "QEMU stopped while waiting for the key result"
	attempt=$((attempt + 1))
	sleep 0.1
done
if [ "$observed" -ne 1 ]; then
	fail_test "scan-code/ASCII result 1E61 was not observed within 10 seconds"
fi

# A prompt after PASS proves that INT 21h/4C returned control to COMMAND.
resumed=0
attempt=0
while [ "$attempt" -lt 50 ]; do
	if awk '
		/BIOS KEY IRQ PASS AX=1E61/ { passed = 1; next }
		passed && index($0, "C:\\>") { found = 1 }
		END { exit found ? 0 : 1 }
	' "$serial_log"; then
		resumed=1
		break
	fi
	fail_if_qemu_exited "QEMU stopped before COMMAND resumed"
	attempt=$((attempt + 1))
	sleep 0.1
done
if [ "$resumed" -ne 1 ]; then
	fail_test "COMMAND did not resume after INT 21h/4C"
fi

echo "BIOS keyboard IRQ test passed: IVT hook -> SeaBIOS INT 16h -> IRQ1 -> AX=1E61"

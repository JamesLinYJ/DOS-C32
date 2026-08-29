#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Serial-first regression for the E820-derived runtime memory topology.
set -eu

if [ "$#" -ne 1 ]; then
	echo "usage: $0 IMAGE" >&2
	exit 2
fi

image=$1
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if ! command -v qemu-system-i386 >/dev/null 2>&1; then
	echo "QEMU memory-topology test skipped: qemu-system-i386 unavailable"
	exit 0
fi
if [ ! -r "$image" ]; then
	echo "QEMU memory-topology image is not readable: $image" >&2
	exit 2
fi

mib_bytes=1048576
page_bytes=4096
page_table_bytes=$((4 * mib_bytes))
qemu_display=${QEMU_DISPLAY:-}
qemu_memory_mib=${QEMU_MEMORY_MIB:-256}
identity_floor=${X86_BOOT_IDENTITY_FLOOR:-0x00800000}
identity_ceiling=${X86_BOOT_IDENTITY_CEILING:-0x10000000}

case "$qemu_memory_mib" in
''|*[!0-9]*)
	echo "QEMU_MEMORY_MIB must be a positive decimal integer" >&2
	exit 2
	;;
esac
if ! printf '%s\n' "$identity_floor" | grep -Eq \
	'^(0[xX][0-9A-Fa-f]+|[0-9]+)$' ||
   ! printf '%s\n' "$identity_ceiling" | grep -Eq \
	'^(0[xX][0-9A-Fa-f]+|[0-9]+)$'; then
	echo "identity aperture values must be decimal or hexadecimal integers" >&2
	exit 2
fi

identity_floor_value=$((identity_floor))
identity_ceiling_value=$((identity_ceiling))
floor_mib=$(((identity_floor_value + mib_bytes - 1) / mib_bytes))
ceiling_mib=$((identity_ceiling_value / mib_bytes))
if [ "$qemu_memory_mib" -le $((floor_mib + 4)) ] ||
   [ "$identity_floor_value" -ge "$identity_ceiling_value" ] ||
   [ $((identity_ceiling_value % page_table_bytes)) -ne 0 ]; then
	echo "RAM/aperture is too small or misaligned for this boot test" >&2
	exit 2
fi

# Keep one sample well below the aperture.  The other sample remains the exact
# caller-selected QEMU_MEMORY_MIB value, so Make/environment policy is honored.
probe_memory_mib=$((floor_mib + 32))
if [ "$probe_memory_mib" -ge $((ceiling_mib - 4)) ]; then
	probe_memory_mib=$((floor_mib +
		(ceiling_mib - floor_mib) / 2))
fi
requested_effective_mib=$qemu_memory_mib
if [ "$requested_effective_mib" -gt "$ceiling_mib" ]; then
	requested_effective_mib=$ceiling_mib
fi
probe_effective_mib=$probe_memory_mib
if [ $((requested_effective_mib - probe_effective_mib)) -lt 0 ]; then
	effective_difference=$((probe_effective_mib -
		requested_effective_mib))
else
	effective_difference=$((requested_effective_mib -
		probe_effective_mib))
fi
if [ "$effective_difference" -lt 8 ]; then
	if [ $((probe_memory_mib + 32)) -lt "$ceiling_mib" ]; then
		probe_memory_mib=$((probe_memory_mib + 32))
	elif [ $((probe_memory_mib - 16)) -gt $((floor_mib + 4)) ]; then
		probe_memory_mib=$((probe_memory_mib - 16))
	else
		echo "cannot select two distinct memory topologies" >&2
		exit 2
	fi
fi

if [ -z "$qemu_display" ]; then
	case "$(uname -s)" in
	Darwin) qemu_display="cocoa,show-cursor=on" ;;
	*) qemu_display=none ;;
	esac
fi

temporary_dir=$(mktemp -d "${TMPDIR:-/tmp}/dos-c32-memory.XXXXXX")
requested_log="$temporary_dir/requested.log"
probe_log="$temporary_dir/probe.log"
recognition_log="$temporary_dir/recognition.log"
requested_monitor="$temporary_dir/requested.monitor"
probe_monitor="$temporary_dir/probe.monitor"
recognition_monitor="$temporary_dir/recognition.monitor"

cleanup()
{
	rm -f -- "$requested_log" "$probe_log" "$recognition_log" \
		"$requested_monitor" "$probe_monitor" "$recognition_monitor"
	rmdir -- "$temporary_dir"
}
trap cleanup EXIT HUP INT TERM

run_topology()
{
	case_name=$1
	memory_mib=$2
	serial_log=$3
	monitor_socket=$4

	set +e
	TMPDIR="$temporary_dir" "$script_dir/run-with-timeout.sh" 8 \
		qemu-system-i386 \
		-machine pc,accel=tcg \
		-m "${memory_mib}M" \
		-drive "file=$image,format=raw,if=ide,index=0,media=disk,snapshot=on" \
		-boot c \
		-display "$qemu_display" \
		-monitor "unix:$monitor_socket,server=on,wait=off" \
		-no-reboot \
		-no-shutdown \
		-serial "file:$serial_log" \
		>/dev/null 2>&1
	qemu_status=$?
	set -e
	if [ "$qemu_status" -ne 0 ] && [ "$qemu_status" -ne 124 ]; then
		echo "QEMU $case_name topology failed: status $qemu_status" >&2
		exit 1
	fi
}

parse_and_validate()
{
	case_name=$1
	memory_mib=$2
	serial_log=$3
	detected_fields=$(tr -d '\r' < "$serial_log" | awk '
		$1 == "Memory:" && $2 == "detected" &&
		$4 == "MiB" && $5 == "usable" && $6 == "in" &&
		$8 == "E820" && $9 == "extents" {
			print $3, $7
		}')
	managed_fields=$(tr -d '\r' < "$serial_log" | awk '
		$1 == "Memory:" && $2 == "managed" &&
		$4 == "MiB," && $5 == "high" &&
		$7 == "identity" && $8 == "map" && $10 == "MiB" {
			high = $6
			sub(/,$/, "", high)
			print $3, high, $9
		}')
	set -- $detected_fields $managed_fields
	if [ "$#" -ne 5 ]; then
		echo "serial log has no unique memory topology: $case_name" >&2
		sed -n '1,160p' "$serial_log" >&2
		exit 1
	fi
	parsed_detected_mib=$1
	parsed_extent_count=$2
	parsed_pool_mib=$3
	parsed_high_hex=$4
	parsed_identity_mib=$5
	case "$parsed_detected_mib:$parsed_extent_count:$parsed_pool_mib:$parsed_identity_mib" in
	*[!0-9:]*|:*|*:)
		echo "serial memory counts are malformed: $case_name" >&2
		exit 1
		;;
	esac
	if [ "$parsed_detected_mib" -gt "$memory_mib" ] ||
	   [ $((parsed_detected_mib + 4)) -lt "$memory_mib" ] ||
	   [ "$parsed_extent_count" -eq 0 ] ||
	   [ "$parsed_extent_count" -gt 64 ]; then
		echo "detected E820 memory does not follow supplied RAM: $case_name" >&2
		sed -n '1,160p' "$serial_log" >&2
		exit 1
	fi
	if ! printf '%s\n' "$parsed_high_hex" |
		grep -Eq '^0x[0-9A-Fa-f]{16}$'; then
		echo "serial highest address is malformed: $case_name:" \
			"$parsed_high_hex" >&2
		sed -n '1,160p' "$serial_log" >&2
		exit 1
	fi

	parsed_high_value=$((parsed_high_hex))
	high_limit=$((parsed_high_value + 1))
	ram_limit=$((memory_mib * mib_bytes))
	effective_limit=$ram_limit
	if [ "$effective_limit" -gt "$identity_ceiling_value" ]; then
		effective_limit=$identity_ceiling_value
	fi
	if [ "$high_limit" -le "$identity_floor_value" ] ||
	   [ "$high_limit" -gt "$effective_limit" ] ||
	   [ $((high_limit % page_bytes)) -ne 0 ] ||
	   [ $((effective_limit - high_limit)) -ge "$page_table_bytes" ]; then
		echo "E820 high address does not track RAM/aperture: $case_name" >&2
		exit 1
	fi

	expected_identity=$(((high_limit + page_table_bytes - 1) /
		page_table_bytes * page_table_bytes))
	if [ "$expected_identity" -gt "$identity_ceiling_value" ]; then
		expected_identity=$identity_ceiling_value
	fi
	reported_identity=$((parsed_identity_mib * mib_bytes))
	if [ "$reported_identity" -ne "$expected_identity" ]; then
		echo "identity map does not follow the E820 high water mark: $case_name" >&2
		exit 1
	fi

	managed_span=$((high_limit - identity_floor_value))
	reported_free_floor=$((parsed_pool_mib * mib_bytes))
	if [ "$parsed_pool_mib" -eq 0 ] ||
	   [ "$reported_free_floor" -gt "$managed_span" ] ||
	   [ $((managed_span - reported_free_floor)) -ge "$page_table_bytes" ]; then
		echo "startup free pool does not match the E820 managed span: $case_name" >&2
		exit 1
	fi
}

run_topology requested "$qemu_memory_mib" "$requested_log" \
	"$requested_monitor"
parse_and_validate requested "$qemu_memory_mib" "$requested_log"
requested_pool_mib=$parsed_pool_mib
requested_detected_mib=$parsed_detected_mib
requested_high_value=$parsed_high_value
requested_high_hex=$parsed_high_hex
requested_identity_mib=$parsed_identity_mib

run_topology probe "$probe_memory_mib" "$probe_log" "$probe_monitor"
parse_and_validate probe "$probe_memory_mib" "$probe_log"
probe_pool_mib=$parsed_pool_mib
probe_detected_mib=$parsed_detected_mib
probe_high_value=$parsed_high_value
probe_high_hex=$parsed_high_hex
probe_identity_mib=$parsed_identity_mib

requested_limit=$((qemu_memory_mib * mib_bytes))
if [ "$requested_limit" -gt "$identity_ceiling_value" ]; then
	requested_limit=$identity_ceiling_value
fi
probe_limit=$((probe_memory_mib * mib_bytes))
if [ "$requested_limit" -gt "$probe_limit" ]; then
	if [ "$requested_high_value" -le "$probe_high_value" ] ||
	   [ "$requested_pool_mib" -le "$probe_pool_mib" ] ||
	   [ "$requested_identity_mib" -lt "$probe_identity_mib" ]; then
		echo "larger effective RAM did not produce a larger E820 topology" >&2
		exit 1
	fi
elif [ "$requested_limit" -lt "$probe_limit" ]; then
	if [ "$requested_high_value" -ge "$probe_high_value" ] ||
	   [ "$requested_pool_mib" -ge "$probe_pool_mib" ] ||
	   [ "$requested_identity_mib" -gt "$probe_identity_mib" ]; then
		echo "smaller effective RAM did not produce a smaller E820 topology" >&2
		exit 1
	fi
else
	echo "selected memory topologies have the same effective limit" >&2
	exit 2
fi

# Recognition is independent from the compiled allocator/page-table aperture.
# Supply more RAM than the current ceiling and prove the firmware topology sees
# it while the managed pool remains safely capped by mapped pages.
recognition_memory_mib=$((ceiling_mib + 64))
if [ "$recognition_memory_mib" -eq "$qemu_memory_mib" ]; then
	recognition_memory_mib=$((recognition_memory_mib + 64))
fi
run_topology recognition "$recognition_memory_mib" "$recognition_log" \
	"$recognition_monitor"
parse_and_validate recognition "$recognition_memory_mib" "$recognition_log"
recognition_detected_mib=$parsed_detected_mib
recognition_pool_mib=$parsed_pool_mib
recognition_high_hex=$parsed_high_hex
recognition_identity_mib=$parsed_identity_mib
if [ "$recognition_detected_mib" -le "$ceiling_mib" ] ||
   [ "$recognition_identity_mib" -ne "$ceiling_mib" ] ||
   [ "$recognition_pool_mib" -ge "$recognition_detected_mib" ]; then
	echo "above-aperture RAM was not separated from managed capacity" >&2
	exit 1
fi

echo "QEMU E820 topology passed: RAM ${qemu_memory_mib} MiB ->" \
	"detected ${requested_detected_mib} MiB, free ${requested_pool_mib} MiB," \
	"high ${requested_high_hex}," \
	"map ${requested_identity_mib} MiB; RAM ${probe_memory_mib} MiB ->" \
	"detected ${probe_detected_mib} MiB, free ${probe_pool_mib} MiB," \
	"high ${probe_high_hex}, map ${probe_identity_mib} MiB; RAM" \
	"${recognition_memory_mib} MiB -> detected ${recognition_detected_mib} MiB," \
	"free ${recognition_pool_mib} MiB, high ${recognition_high_hex}," \
	"map ${recognition_identity_mib} MiB"

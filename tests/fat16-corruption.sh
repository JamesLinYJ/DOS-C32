#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

if [ "$#" -ne 1 ]; then
	echo "usage: $0 IMAGE" >&2
	exit 2
fi

source_image=$1
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if ! command -v qemu-system-i386 >/dev/null 2>&1; then
	echo "FAT16 corruption test skipped: qemu-system-i386 is unavailable"
	exit 0
fi

for command_name in cp dd find grep mcopy mshowfat od sed tr; do
	if ! command -v "$command_name" >/dev/null 2>&1; then
		echo "missing FAT16 corruption test tool: $command_name" >&2
		exit 1
	fi
done

temporary_dir=$(mktemp -d "${TMPDIR:-/tmp}/dos-c32-fat16.XXXXXX")
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
	find "$temporary_dir" -type f -delete
	rmdir -- "$temporary_dir"
}
trap cleanup EXIT HUP INT TERM

write_at()
{
	image=$1
	offset=$2
	bytes=$3
	printf '%b' "$bytes" | dd of="$image" bs=1 seek="$offset" \
		conv=notrunc status=none
}

boot_and_expect()
{
	case_name=$1
	image=$2
	expected=$3
	serial_log="$temporary_dir/$case_name.serial"
	debug_log="$temporary_dir/$case_name.debug"

	set +e
	TMPDIR="$temporary_dir" "$script_dir/run-with-timeout.sh" 5 qemu-system-i386 \
		-machine pc,accel=tcg -m "${qemu_memory_mib}M" \
		-drive "file=$image,format=raw,if=ide,index=0,media=disk,snapshot=on" \
		-boot c -display "$qemu_display" -monitor none -no-reboot -no-shutdown \
		-serial "file:$serial_log" -debugcon "file:$debug_log" \
		-global isa-debugcon.iobase=0xe9 >/dev/null 2>&1
	qemu_status=$?
	set -e
	if [ "$qemu_status" -ne 0 ] && [ "$qemu_status" -ne 124 ]; then
		echo "$case_name: QEMU failed with status $qemu_status" >&2
		exit 1
	fi
	if ! grep -Fq "$expected" "$serial_log" &&
	   ! grep -Fq "$expected" "$debug_log"; then
		echo "$case_name: missing diagnostic: $expected" >&2
		sed -n '1,100p' "$serial_log" >&2
		sed -n '1,100p' "$debug_log" >&2
		exit 1
	fi
}

media_image="$temporary_dir/media.img"
cp -- "$source_image" "$media_image"
write_at "$media_image" 21 '\160'
boot_and_expect media "$media_image" \
	"Could not mount the boot volume: filesystem or partition metadata is corrupt."

root_image="$temporary_dir/root.img"
cp -- "$source_image" "$root_image"
write_at "$root_image" 17 '\001\002'
boot_and_expect root-alignment "$root_image" \
	"Bad FAT16 geometry"

signature_image="$temporary_dir/signature.img"
cp -- "$source_image" "$signature_image"
write_at "$signature_image" 19 '\000\000'
write_at "$signature_image" 38 '\000'
boot_and_expect extended-signature "$signature_image" \
	"Could not mount the boot volume: filesystem or partition metadata is corrupt."

capacity_image="$temporary_dir/capacity.img"
cp -- "$source_image" "$capacity_image"
write_at "$capacity_image" 19 '\000\000'
write_at "$capacity_image" 32 '\000\000\001\000'
boot_and_expect volume-capacity "$capacity_image" \
	"Could not mount the boot volume: filesystem or partition metadata is corrupt."

long_image="$temporary_dir/long.img"
payload="$temporary_dir/AUTOEXEC.BAT"
dd if=/dev/zero of="$payload" bs=4096 count=1 status=none
cp -- "$source_image" "$long_image"
mcopy -o -i "$long_image" "$payload" ::/AUTOEXEC.BAT
cluster=$(mshowfat -i "$long_image" ::/AUTOEXEC.BAT |
	sed -n 's/^[^<]*<\([0-9][0-9]*\).*/\1/p')
case "$cluster" in
	''|*[!0-9]*)
		echo "could not determine AUTOEXEC.BAT first cluster" >&2
		exit 1
		;;
esac
reserved_sectors=$(od -An -tu2 -j14 -N2 "$long_image" | tr -d ' \n')
fat_entry_offset=$((reserved_sectors * 512 + cluster * 2))

free_image="$temporary_dir/free.img"
cp -- "$long_image" "$free_image"
write_at "$free_image" "$fat_entry_offset" '\000\000'
boot_and_expect chain-free "$free_image" \
	"AUTOEXEC.BAT failed: filesystem metadata is corrupt."

reserved_image="$temporary_dir/reserved.img"
cp -- "$long_image" "$reserved_image"
write_at "$reserved_image" "$fat_entry_offset" '\360\377'
boot_and_expect chain-reserved "$reserved_image" \
	"AUTOEXEC.BAT failed: filesystem metadata is corrupt."

bad_image="$temporary_dir/bad.img"
cp -- "$long_image" "$bad_image"
write_at "$bad_image" "$fat_entry_offset" '\367\377'
boot_and_expect chain-bad "$bad_image" \
	"AUTOEXEC.BAT failed: filesystem metadata is corrupt."

eoc_image="$temporary_dir/eoc.img"
cp -- "$long_image" "$eoc_image"
write_at "$eoc_image" "$fat_entry_offset" '\370\377'
boot_and_expect chain-eoc "$eoc_image" \
	"AUTOEXEC.BAT failed: filesystem metadata is corrupt."

echo "FAT16 corruption tests passed: BPB bounds and chain markers"

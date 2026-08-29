#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

if [ "$#" -ne 2 ]; then
	echo "usage: $0 WINDOWS32.7Z OUTPUT.VHD" >&2
	exit 2
fi

archive=$1
output=$2
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
c32_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
bsdtar_tool=${BSDTAR:-bsdtar}
mcopy_tool=${MCOPY:-mcopy}
mmd_tool=${MMD:-mmd}
qemu_img_tool=${QEMU_IMG:-qemu-img}
qemu_system_tool=${QEMU_SYSTEM_I386:-qemu-system-i386}
qemu_display=${QEMU_DISPLAY:-}
qemu_memory_mib=${QEMU_MEMORY_MIB:-256}
image_kib=${WINDOWS32_IMAGE_KIB:-65536}
fat_sectors=${WINDOWS32_FAT_SECTORS:-128}

if [ -z "$qemu_display" ]; then
	case "$(uname -s)" in
	Darwin) qemu_display="cocoa,show-cursor=on" ;;
	*) qemu_display=none ;;
	esac
fi

if [ ! -f "$archive" ]; then
	echo "Windows 3.2 archive not found: $archive" >&2
	exit 2
fi
case "$output" in
/*) ;;
*) output="$c32_dir/$output" ;;
esac
output_dir=$(dirname -- "$output")
mkdir -p "$output_dir"

for tool in "$bsdtar_tool" "$mcopy_tool" "$mmd_tool" \
	"$qemu_img_tool" "$qemu_system_tool"; do
	if ! command -v "$tool" >/dev/null 2>&1; then
		echo "required Windows VHD tool is unavailable: $tool" >&2
		exit 2
	fi
done

temporary_dir=$(mktemp -d "${TMPDIR:-/tmp}/dos-c32-windows32.XXXXXX")
media_dir="$temporary_dir/media"
merged_dir="$temporary_dir/merged"
payload_dir="$temporary_dir/payload"
candidate_vhd="$temporary_dir/windows32.vhd"
serial_log="$output.boot.log"
monitor_socket="$temporary_dir/qemu-monitor.sock"
build_relative=build/windows32/windows32-build

cleanup()
{
	rm -rf -- "$temporary_dir"
}
trap cleanup EXIT HUP INT TERM

mkdir -p "$media_dir" "$merged_dir" "$payload_dir"
"$bsdtar_tool" -xf "$archive" -C "$media_dir"
disk1=$(find "$media_dir" -type f -name DISK1.img -print -quit)
if [ -z "$disk1" ]; then
	echo "archive does not contain DISK1.img" >&2
	exit 1
fi
disk_dir=$(dirname -- "$disk1")

expected_files=0
for disk_name in DISK1 DISK2 DISK3 DISK4 DISK5 DISK6 DISK7 DISK8 DISK9 \
	DISKA DISKB; do
	disk="$disk_dir/$disk_name.img"
	stage="$merged_dir/$disk_name"
	if [ ! -f "$disk" ] || [ "$(wc -c < "$disk")" -ne 1474560 ]; then
		echo "missing or invalid 1.44 MB installation disk: $disk_name.img" >&2
		exit 1
	fi
	mkdir -p "$stage"
	"$mcopy_tool" -s -m -i "$disk" '::*' "$stage"
	disk_files=$(find "$stage" -type f | wc -l | tr -d ' ')
	if [ "$disk_files" -eq 0 ]; then
		echo "installation disk is empty: $disk_name.img" >&2
		exit 1
	fi
	if [ ! -f "$stage/$disk_name" ]; then
		echo "installation disk marker is missing: $disk_name" >&2
		exit 1
	fi
	for source in "$stage"/*; do
		if [ "$(basename -- "$source")" = "$disk_name" ]; then
			continue
		fi
		cp -p -- "$source" "$payload_dir/"
		expected_files=$((expected_files + 1))
	done
done

${MAKE:-make} -C "$c32_dir" BUILD="$build_relative" \
	IMAGE_KIB="$image_kib" FAT_SECTORS_PER_COPY="$fat_sectors" image
raw_image="$c32_dir/$build_relative/msdos-c32.img"
if [ ! -f "$raw_image" ]; then
	echo "DOS-C32 raw system image was not built" >&2
	exit 1
fi

"$mmd_tool" -i "$raw_image" ::/WINSETUP
"$mcopy_tool" -o -m -i "$raw_image" "$payload_dir"/* ::/WINSETUP/
"$mcopy_tool" -o -m -i "$raw_image" \
	"$c32_dir/assets/WINSETUP.BAT" ::/WINSETUP.BAT
"$mcopy_tool" -o -m -i "$raw_image" \
	"$c32_dir/assets/WINDOWS32.TXT" ::/WINDOWS32.TXT
"$script_dir/sync-fat-mirrors.sh" "$raw_image"

actual_files=$("${MDIR:-mdir}" -b -i "$raw_image" ::/WINSETUP | \
	wc -l | tr -d ' ')
if [ "$actual_files" -ne "$expected_files" ]; then
	echo "merged file count mismatch: expected $expected_files, got $actual_files" >&2
	exit 1
fi
for required in SETUP.EXE KRNL386.EX_ WIN386.EX_ HIMEM.SY_; do
	if ! "${MDIR:-mdir}" -b -i "$raw_image" "::/WINSETUP/$required" \
		>/dev/null 2>&1; then
		echo "merged VHD source is missing: $required" >&2
		exit 1
	fi
done

"$qemu_img_tool" convert -f raw -O vpc -o subformat=fixed \
	"$raw_image" "$candidate_vhd"
if ! "$qemu_img_tool" info -f vpc --output=json "$candidate_vhd" |
	grep -Eq '"format"[[:space:]]*:[[:space:]]*"vpc"'; then
	echo "generated disk is not a VPC/VHD image" >&2
	exit 1
fi
if ! "${MDIR:-mdir}" -b -i "$candidate_vhd" ::/WINSETUP/SETUP.EXE \
	>/dev/null 2>&1; then
	echo "generated VHD does not expose C:\\WINSETUP\\SETUP.EXE" >&2
	exit 1
fi
mv -f -- "$candidate_vhd" "$output"

rm -f -- "$serial_log"
set +e
"$script_dir/run-with-timeout.sh" 8 \
	"$qemu_system_tool" -machine pc,accel=tcg -cpu 486 \
	-m "${qemu_memory_mib}M" \
	-drive "file=$output,format=vpc,if=ide,index=0,media=disk,snapshot=on" \
	-boot c -display "$qemu_display" \
	-monitor "unix:$monitor_socket,server=on,wait=off" \
	-no-reboot -no-shutdown \
	-serial "file:$serial_log" >/dev/null 2>&1
qemu_status=$?
set -e
if [ "$qemu_status" -ne 0 ] && [ "$qemu_status" -ne 124 ]; then
	echo "QEMU failed while booting the generated VHD: $qemu_status" >&2
	exit 1
fi
if ! grep -Fq 'C:\>' "$serial_log"; then
	echo "generated Windows VHD did not reach the DOS-C32 prompt" >&2
	sed -n '1,160p' "$serial_log" >&2
	exit 1
fi

echo "Windows 3.2 VHD ready: $output"
echo "Merged $expected_files files from 11 installation disks into C:\\WINSETUP"
echo "Boot smoke passed; run WINSETUP.BAT inside DOS-C32 to start Setup"

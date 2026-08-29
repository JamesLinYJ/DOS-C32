#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

if [ "$#" -ne 1 ]; then
	echo "usage: $0 FAT_IMAGE" >&2
	exit 2
fi

image=$1
for tool in cmp dd od tr wc; do
	if ! command -v "$tool" >/dev/null 2>&1; then
		echo "missing FAT mirror tool: $tool" >&2
		exit 2
	fi
done
if [ ! -f "$image" ]; then
	echo "FAT image not found: $image" >&2
	exit 2
fi

temporary_dir=$(mktemp -d "${TMPDIR:-/tmp}/dos-c32-fat-mirror.XXXXXX")
primary="$temporary_dir/primary.fat"
mirror="$temporary_dir/mirror.fat"
cleanup()
{
	rm -f -- "$primary" "$mirror"
	rmdir -- "$temporary_dir"
}
trap cleanup EXIT HUP INT TERM

bytes_per_sector=$(od -An -tu2 -j11 -N2 "$image" | tr -d ' \n')
reserved_sectors=$(od -An -tu2 -j14 -N2 "$image" | tr -d ' \n')
fat_count=$(od -An -tu1 -j16 -N1 "$image" | tr -d ' \n')
sectors_per_fat=$(od -An -tu2 -j22 -N2 "$image" | tr -d ' \n')
if [ "$bytes_per_sector" -ne 512 ] || [ "$reserved_sectors" -eq 0 ] ||
   [ "$fat_count" -eq 0 ] || [ "$sectors_per_fat" -eq 0 ]; then
	echo "unsupported or invalid FAT mirror geometry in $image" >&2
	exit 1
fi

fat_bytes=$((sectors_per_fat * bytes_per_sector))
dd if="$image" of="$primary" bs=512 skip="$reserved_sectors" \
	count="$sectors_per_fat" status=none
if [ "$(wc -c < "$primary")" -ne "$fat_bytes" ]; then
	echo "primary FAT is truncated in $image" >&2
	exit 1
fi

copy=1
while [ "$copy" -lt "$fat_count" ]; do
	seek=$((reserved_sectors + copy * sectors_per_fat))
	dd if="$primary" of="$image" bs=512 seek="$seek" \
		count="$sectors_per_fat" conv=notrunc status=none
	dd if="$image" of="$mirror" bs=512 skip="$seek" \
		count="$sectors_per_fat" status=none
	if ! cmp -s "$primary" "$mirror"; then
		echo "FAT mirror $copy verification failed in $image" >&2
		exit 1
	fi
	copy=$((copy + 1))
done

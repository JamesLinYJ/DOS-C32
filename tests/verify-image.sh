#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

if [ "$#" -ne 10 ]; then
	echo "usage: $0 IMAGE KERNEL_BIN IMAGE_KIB SECTOR_BYTES \
RESERVED_SECTORS FAT_COPIES FAT_SECTORS LOAD_BASE RUNTIME_BASE STACK_FLOOR" >&2
	exit 2
fi

image=$1
kernel=$2
expected_image_kib=$3
expected_sector_bytes=$4
expected_reserved_sectors=$5
expected_fat_count=$6
expected_fat_sectors=$7
expected_load_base=$8
expected_runtime_base=$9
expected_stack_floor=${10}
fsck_fat=${FSCK_FAT:-fsck.fat}
readelf_tool=${READELF:-readelf}
nm_tool=${NM:-nm}
mcopy_tool=${MCOPY:-mcopy}

# FAT16 BPB offsets from the public on-disk format.
bpb_bytes_per_sector_offset=11
bpb_reserved_sectors_offset=14
bpb_fat_count_offset=16
bpb_total_sectors16_offset=19
bpb_fat_sectors16_offset=22
bpb_total_sectors32_offset=32
bytes_per_kib=1024
boot_signature_bytes=2

build_dir=$(CDPATH= cd -- "$(dirname -- "$kernel")" && pwd)
kernel_elf="$build_dir/kernel.elf"
temporary=$(mktemp "${TMPDIR:-/tmp}/dos-c32-image.XXXXXX")
temporary_mirror=$(mktemp "${TMPDIR:-/tmp}/dos-c32-fat-mirror.XXXXXX")

cleanup()
{
	rm -f -- "$temporary" "$temporary_mirror"
}
trap cleanup EXIT HUP INT TERM

for command_name in od cmp mdir "$mcopy_tool"; do
	if ! command -v "$command_name" >/dev/null 2>&1; then
		echo "missing image verification tool: $command_name" >&2
		exit 1
	fi
done
for command_name in "$fsck_fat" "$readelf_tool" "$nm_tool"; do
	if ! command -v "$command_name" >/dev/null 2>&1; then
		echo "missing image verification tool: $command_name" >&2
		exit 1
	fi
done

if [ ! -f "$image" ] || [ ! -f "$kernel" ] || [ ! -f "$kernel_elf" ]; then
	echo "image, kernel binary, or kernel ELF is missing" >&2
	exit 1
fi

image_size=$(wc -c < "$image")
kernel_size=$(wc -c < "$kernel")
expected_image_size=$((expected_image_kib * bytes_per_kib))
if [ "$image_size" -ne "$expected_image_size" ]; then
	echo "unexpected image size: $image_size" >&2
	exit 1
fi

boot_signature_offset=$((expected_sector_bytes - boot_signature_bytes))
boot_signature=$(od -An -tx1 -j"$boot_signature_offset" \
	-N"$boot_signature_bytes" "$image" | tr -d ' \n')
if [ "$boot_signature" != 55aa ]; then
	echo "invalid boot signature: $boot_signature" >&2
	exit 1
fi

bytes_per_sector=$(od -An -tu2 -j"$bpb_bytes_per_sector_offset" -N2 \
	"$image" | tr -d ' \n')
reserved_sectors=$(od -An -tu2 -j"$bpb_reserved_sectors_offset" -N2 \
	"$image" | tr -d ' \n')
fat_count=$(od -An -tu1 -j"$bpb_fat_count_offset" -N1 "$image" |
	tr -d ' \n')
total_sectors16=$(od -An -tu2 -j"$bpb_total_sectors16_offset" -N2 \
	"$image" | tr -d ' \n')
total_sectors32=$(od -An -tu4 -j"$bpb_total_sectors32_offset" -N4 \
	"$image" | tr -d ' \n')
fat_sectors=$(od -An -tu2 -j"$bpb_fat_sectors16_offset" -N2 \
	"$image" | tr -d ' \n')
expected_total_sectors=$((expected_image_size / expected_sector_bytes))
if [ "$expected_total_sectors" -le 65535 ]; then
	expected_total_sectors16=$expected_total_sectors
	expected_total_sectors32=0
else
	expected_total_sectors16=0
	expected_total_sectors32=$expected_total_sectors
fi
if [ "$bytes_per_sector" -ne "$expected_sector_bytes" ] ||
   [ "$reserved_sectors" -ne "$expected_reserved_sectors" ] ||
   [ "$fat_count" -ne "$expected_fat_count" ] ||
   [ "$total_sectors16" -ne "$expected_total_sectors16" ] ||
   [ "$total_sectors32" -ne "$expected_total_sectors32" ] ||
   [ "$fat_sectors" -ne "$expected_fat_sectors" ]; then
	echo "BPB mismatch: sector=$bytes_per_sector \
reserved=$reserved_sectors fats=$fat_count \
total16=$total_sectors16 total32=$total_sectors32 \
fat-sectors=$fat_sectors" >&2
	exit 1
fi

dd if="$image" of="$temporary" bs="$bytes_per_sector" \
	skip="$reserved_sectors" count="$fat_sectors" status=none
copy=1
while [ "$copy" -lt "$fat_count" ]; do
	mirror_offset=$((reserved_sectors + copy * fat_sectors))
	dd if="$image" of="$temporary_mirror" bs="$bytes_per_sector" \
		skip="$mirror_offset" count="$fat_sectors" status=none
	if ! cmp -s "$temporary" "$temporary_mirror"; then
		echo "FAT mirror $copy differs from the primary FAT" >&2
		exit 1
	fi
	copy=$((copy + 1))
done

max_kernel_size=$(((reserved_sectors - 1) * bytes_per_sector))
if [ "$kernel_size" -le 0 ] || [ "$kernel_size" -gt "$max_kernel_size" ]; then
	echo "kernel does not fit reserved sectors: $kernel_size > $max_kernel_size bytes" >&2
	exit 1
fi

# The destination is a securely pre-created file owned by this test.  For a
# DOS-to-Unix copy, mcopy uses -n (not -o) to suppress the overwrite prompt.
"$mcopy_tool" -n -i "$image" ::/DOSKRNL.SYS "$temporary"
if ! cmp -s "$kernel" "$temporary"; then
	echo "DOSKRNL.SYS differs from the compiled kernel payload" >&2
	exit 1
fi

if ! "$readelf_tool" -h "$kernel_elf" | grep -q 'Class:[[:space:]]*ELF32'; then
	echo "kernel is not ELF32" >&2
	exit 1
fi
if ! "$readelf_tool" -h "$kernel_elf" | grep -q 'Machine:[[:space:]]*Intel 80386'; then
	echo "kernel is not an Intel 80386 image" >&2
	exit 1
fi
if "$readelf_tool" -lW "$kernel_elf" | grep -Eq 'LOAD[[:space:]].*RWE'; then
	echo "kernel ELF contains a writable executable LOAD segment" >&2
	exit 1
fi
if "$readelf_tool" -lW "$kernel_elf" |
	grep -Eq 'GNU_STACK[[:space:]].*[[:space:]]E([[:space:]]|$)'; then
	echo "kernel ELF requests an executable stack" >&2
	exit 1
fi
if "$nm_tool" -u "$kernel_elf" | grep -q .; then
	echo "kernel has unresolved symbols" >&2
	"$nm_tool" -u "$kernel_elf" >&2
	exit 1
fi

elf_entry=$("$readelf_tool" -h "$kernel_elf" |
	awk '/Entry point address:/ { print $4 }')
runtime_start=$("$nm_tool" -n "$kernel_elf" |
	awk '$3 == "__kernel_runtime_start" { print "0x" $1 }')
runtime_end=$("$nm_tool" -n "$kernel_elf" |
	awk '$3 == "__kernel_runtime_end" { print "0x" $1 }')
staging_end=$("$nm_tool" -n "$kernel_elf" |
	awk '$3 == "__kernel_staging_file_end" { print "0x" $1 }')
if [ $((elf_entry)) -ne $((expected_load_base)) ] ||
   [ $((runtime_start)) -ne $((expected_runtime_base)) ] ||
   [ $((runtime_end)) -ge $((expected_stack_floor)) ] ||
   [ $((staging_end)) -gt $((expected_load_base + max_kernel_size)) ]; then
	echo "kernel staging/runtime layout mismatch: entry=$elf_entry \
runtime=$runtime_start..$runtime_end staging-end=$staging_end" >&2
	exit 1
fi

"$fsck_fat" -n "$image" >/dev/null
listing=$(mdir -a -b -i "$image" ::/)
for expected in IO.SYS DOSKRNL.SYS COMMAND.COM README.TXT AUTOEXEC.BAT \
WELCOME.TXT COUNTRY.SYS; do
	if ! printf '%s\n' "$listing" | grep -q "/$expected"; then
		echo "missing FAT root file: $expected" >&2
		exit 1
	fi
done
if printf '%s\n' "$listing" | grep -q '/MSDOS.SYS'; then
	echo "obsolete compatibility alias is present: MSDOS.SYS" >&2
	exit 1
fi

echo "image verification passed: IO.SYS/DOSKRNL.SYS/COMMAND.COM/COUNTRY.SYS, ELF32/i386 staged-high W^X kernel, FAT16"

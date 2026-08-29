#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
c32_dir=$(CDPATH= cd -- "$test_dir/.." && pwd)
country_cc=${CC:-gcc}
temporary_dir=$(mktemp -d "${TMPDIR:-/tmp}/dos-c32-country.XXXXXX")
test_elf="$temporary_dir/dos-country-file-test"

cleanup()
{
	rm -f -- "$temporary_dir"/*.o "$test_elf"
	rmdir -- "$temporary_dir"
}
trap cleanup EXIT HUP INT TERM

warnings="-Wall -Wextra -Werror -Wundef -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wvla -Wformat=2 -Wcast-align=strict -Wnull-dereference"
freestanding="-std=gnu11 -O2 -ffreestanding -fno-builtin -fno-pie -fno-pic -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables"

build_model()
{
	model=$1
	arch=$2
	for source in \
		"$c32_dir/libc32/string.c" \
		"$c32_dir/dos/nls/package.c" \
		"$c32_dir/dos/nls/country_file.c" \
		"$test_dir/dos_country_file_test.c"
	do
		object="$temporary_dir/$(basename "${source%.c}")-$model.o"
		# shellcheck disable=SC2086 # Deliberate compiler argument lists.
		"$country_cc" $arch $freestanding $warnings -I"$c32_dir/include" \
			-I"$test_dir" -c "$source" -o "$object"
	done
}

build_model 64 "-m64 -march=x86-64"
"$country_cc" -m64 -nostdlib -no-pie -Wl,-e,_start -Wl,--build-id=none \
	"$temporary_dir/string-64.o" "$temporary_dir/package-64.o" \
	"$temporary_dir/country_file-64.o" \
	"$temporary_dir/dos_country_file_test-64.o" -o "$test_elf"
"$test_elf"

build_model 32 "-m32 -march=i386 -msoft-float -mno-mmx -mno-sse -mno-sse2"

echo "COUNTRY.SYS tests passed: deterministic encode, 64-bit geometry, CRC and transactional parse"

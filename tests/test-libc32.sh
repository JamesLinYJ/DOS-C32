#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu
ulimit -c 0

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
c32_dir=$(CDPATH= cd -- "$test_dir/.." && pwd)
libc32_cc=${CC:-gcc}
test_tmp=$(mktemp -d "${TMPDIR:-/tmp}/dos-c32-libc32.XXXXXX")
cleanup()
{
	rm -f -- "$test_tmp"/*.o "$test_tmp"/libc32-test-m32 \
		"$test_tmp"/libc32-test-m64 \
		"$test_tmp"/libc32-hosted-o2
	rmdir -- "$test_tmp"
}
trap cleanup EXIT HUP INT TERM

compile_common="-std=gnu11 -O2 -ffreestanding -fno-builtin -fno-pie -fno-pic -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables -Wall -Wextra -Werror -Wundef -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wvla -Wformat=2 -Wcast-align=strict -Wnull-dereference"

for model in 32 64
do
	case "$model" in
	32) architecture_flags="-m32 -march=i386 -mtune=i386 -msoft-float -mno-mmx -mno-sse -mno-sse2" ;;
	64) architecture_flags="-m64 -march=x86-64" ;;
	esac
	for source in "$test_dir/libc32_test.c" "$c32_dir/libc32/string.c"
	do
		object="$test_tmp/$(basename "${source%.c}")-m$model.o"
		# shellcheck disable=SC2086 # Deliberate compiler argument lists.
		"$libc32_cc" $architecture_flags $compile_common \
			-I"$c32_dir/include" -c "$source" -o "$object"
	done
	# shellcheck disable=SC2086 # Deliberate compiler argument lists.
	"$libc32_cc" $architecture_flags -nostdlib -static -no-pie \
		-Wl,-e,_start -Wl,--build-id=none \
		"$test_tmp/libc32_test-m$model.o" \
		"$test_tmp/string-m$model.o" \
		-o "$test_tmp/libc32-test-m$model"
done

"$test_tmp/libc32-test-m64"
if command -v qemu-i386 >/dev/null 2>&1; then
	qemu-i386 "$test_tmp/libc32-test-m32"
fi

hosted_flags="-std=gnu11 -O2 -DDOSC32_HOSTED_TYPES -Wall -Wextra -Werror -Wundef -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wvla -Wformat=2"
# Deliberately omit -ffreestanding and -fno-builtin: this exercises the flags
# used by optimized host tools that link libc32's raw memory definitions.
# shellcheck disable=SC2086 # Deliberate compiler argument list.
"$libc32_cc" $hosted_flags -I"$c32_dir/include" \
	"$test_dir/libc32_hosted_optimization_test.c" \
	"$c32_dir/libc32/string.c" -o "$test_tmp/libc32-hosted-o2"
"$test_tmp/libc32-hosted-o2"

echo "libc32-core tests passed: bounded and optimized hosted memory primitives"

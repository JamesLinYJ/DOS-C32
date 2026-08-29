#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
include_dir=$(CDPATH= cd -- "$script_dir/../include" && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/dos-c32-abi.XXXXXX")
trap 'rm -rf -- "$build_dir"' EXIT HUP INT TERM

cc=${CC:-gcc}
source_file="$script_dir/dos_abi_test.c"
common_flags="-std=gnu11 -O2 -ffreestanding -fno-builtin -fno-pie -fno-stack-protector -fcf-protection=none -Wall -Wextra -Werror -Wundef -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wcast-align=strict"
link_flags="-nostdlib -static -no-pie -Wl,--build-id=none"

# Deliberately build and link both native data models.  Header assertions and
# every byte-test expression are therefore compiled in each model.  Execute
# the native 64-bit image below; seccomp-only CI sandboxes commonly reject the
# IA32 int 80h exit path even when their compiler and linker support -m32.
# shellcheck disable=SC2086
"$cc" -m32 $common_flags -I"$include_dir" "$source_file" $link_flags \
	-o "$build_dir/dos-abi-m32"
# shellcheck disable=SC2086
"$cc" -m64 $common_flags -I"$include_dir" "$source_file" $link_flags \
	-o "$build_dir/dos-abi-m64"

"$build_dir/dos-abi-m64"

if command -v qemu-i386 >/dev/null 2>&1; then
	qemu-i386 "$build_dir/dos-abi-m32"
fi

echo "DOS ABI tests compiled under GCC -m32/-m64; byte tests passed natively"

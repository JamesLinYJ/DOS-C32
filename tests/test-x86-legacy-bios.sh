#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
cc=${CC:-cc}
temporary_dir=$(mktemp -d "${TMPDIR:-/tmp}/dos-c32-bios.XXXXXX")

cleanup()
{
	rm -f -- "$temporary_dir/x86-legacy-bios-test"
	rmdir -- "$temporary_dir"
}
trap cleanup EXIT HUP INT TERM

"$cc" -std=gnu11 -Wall -Wextra -Werror -Wundef -Wshadow \
	-Wstrict-prototypes -Wmissing-prototypes -Wvla \
	-I"$root_dir/include" \
	"$root_dir/tests/x86_legacy_bios_test.c" \
	"$root_dir/kernel/x86_vm/platform/boot_storage.c" \
	"$root_dir/kernel/x86_vm/platform/legacy_bios.c" \
	-o "$temporary_dir/x86-legacy-bios-test"
"$temporary_dir/x86-legacy-bios-test"

echo "x86 legacy BIOS discovery tests passed"

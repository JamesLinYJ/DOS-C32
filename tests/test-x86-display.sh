#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
display_cc=${CC:-gcc}
temporary_dir=$(mktemp -d "${TMPDIR:-/tmp}/dos-c32-display.XXXXXX")

cleanup()
{
	rm -f -- "$temporary_dir"/*.o "$temporary_dir"/x86-display-test-*
	rmdir -- "$temporary_dir"
}
trap cleanup EXIT HUP INT TERM

warning_flags="-std=gnu11 -O2 -Wall -Wextra -Werror -Wundef -Wshadow"
warning_flags="$warning_flags -Wstrict-prototypes -Wmissing-prototypes -Wvla"
warning_flags="$warning_flags -Wcast-align=strict -Wnull-dereference"

for model in 32 64; do
	# shellcheck disable=SC2086 # Deliberate compiler argument list.
	"$display_cc" -m$model $warning_flags -I"$root_dir/include" \
		-I"$root_dir/tests" -c \
		"$root_dir/kernel/x86_vm/platform/display.c" \
		-o "$temporary_dir/display-$model.o"
	# shellcheck disable=SC2086 # Deliberate compiler argument list.
	"$display_cc" -m$model $warning_flags -I"$root_dir/include" \
		-I"$root_dir/tests" -c "$root_dir/tests/x86_display_test.c" \
		-o "$temporary_dir/test-$model.o"
	"$display_cc" -m$model -nostdlib -no-pie -Wl,-e,_start \
		"$temporary_dir/display-$model.o" "$temporary_dir/test-$model.o" \
		-o "$temporary_dir/x86-display-test-$model"
done

"$temporary_dir/x86-display-test-64"

echo "x86 display capability tests passed: DCC classes and exact apertures (m64 run, m32 link)"

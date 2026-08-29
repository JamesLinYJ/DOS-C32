#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
c32_dir=$(CDPATH= cd -- "$test_dir/.." && pwd)
vm86_cc=${CC:-gcc}
temporary_dir=$(mktemp -d "${TMPDIR:-/tmp}/dos-c32-vm86-halt.XXXXXX")

cleanup()
{
	status=$?
	trap - EXIT HUP INT TERM
	rm -f -- "$temporary_dir"/* || status=$?
	rmdir -- "$temporary_dir" || status=$?
	exit "$status"
}
trap cleanup EXIT HUP INT TERM

common_flags="-DDOSC32_HOST_TEST=1 -DCONFIG_BOOT_SELFTESTS=0"
common_flags="$common_flags -DCONFIG_X86_VM_ACCEPTANCE_DIAGNOSTICS=0"
common_flags="$common_flags -std=gnu11 -O2 -ffreestanding -fno-builtin"
common_flags="$common_flags -fno-pie -fno-pic -fno-stack-protector"
common_flags="$common_flags -fno-unwind-tables -fno-asynchronous-unwind-tables"
common_flags="$common_flags -Wall -Wextra -Werror -Wundef -Wshadow"
common_flags="$common_flags -Wstrict-prototypes -Wmissing-prototypes -Wvla"
common_flags="$common_flags -Wformat=2 -Wcast-align=strict -Wnull-dereference"
identity_floor=${X86_BOOT_IDENTITY_FLOOR:-0x00800000}
identity_ceiling=${X86_BOOT_IDENTITY_CEILING:-0x10000000}
common_flags="$common_flags -DCONFIG_X86_BOOT_IDENTITY_FLOOR=$identity_floor"
common_flags="$common_flags -DCONFIG_X86_BOOT_IDENTITY_CEILING=$identity_ceiling"

for model in 32 64; do
	case "$model" in
	32) architecture_flags="-m32 -march=i386 -msoft-float -mno-mmx \
		-mno-sse -mno-sse2" ;;
	64) architecture_flags="-m64 -march=x86-64" ;;
	esac
	set --
	for source in \
		"$c32_dir/dos/machine.c" \
		"$c32_dir/dos/control_instruction.c" \
		"$c32_dir/dos/interrupt_reflection.c" \
		"$c32_dir/dos/port_instruction.c" \
		"$c32_dir/kernel/x86_vm/vm86.c" \
		"$test_dir/x86_vm86_halt_test.c"
	do
		object="$temporary_dir/$(basename "${source%.c}")-$model.o"
		# shellcheck disable=SC2086 # Deliberate compiler lists.
		"$vm86_cc" $architecture_flags $common_flags \
			-I"$c32_dir/include" -I"$c32_dir/kernel/x86_vm" \
			-I"$test_dir" -c "$source" -o "$object"
		set -- "$@" "$object"
	done
	binary="$temporary_dir/x86-vm86-halt-$model"
	# shellcheck disable=SC2086 # Deliberate compiler lists.
	"$vm86_cc" $architecture_flags -nostdlib -static -no-pie \
		-Wl,-e,_start -Wl,--build-id=none "$@" -o "$binary"
	if [ "$model" = 64 ]; then
		"$binary"
	fi
done

echo "x86 VM86 HLT tests passed: m64 executed, m32 linked only"
echo "Covered halted generations, IRQ eligibility/rollback and STI shadow retirement"

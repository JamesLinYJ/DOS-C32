#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
c32_dir=$(CDPATH= cd -- "$test_dir/.." && pwd)
test_cc=${CC:-gcc}
temporary_dir=$(mktemp -d "${TMPDIR:-/tmp}/dos-c32-ems.XXXXXX")
qemu_memory_mib=${QEMU_MEMORY_MIB:-256}
qemu_display=${QEMU_DISPLAY:-}

if [ -z "$qemu_display" ]; then
	case "$(uname -s)" in
	Darwin) qemu_display="cocoa,show-cursor=on" ;;
	*) qemu_display=none ;;
	esac
fi

cleanup()
{
	rm -f -- "$temporary_dir"/*.o "$temporary_dir"/dos-ems-*
	rmdir -- "$temporary_dir"
}
trap cleanup EXIT HUP INT TERM

common_flags="-std=gnu11 -O2 -ffreestanding -fno-builtin -fno-pie -fno-pic -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables -Wall -Wextra -Werror -Wundef -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wvla -Wformat=2 -Wcast-align=strict -Wnull-dereference"

for model in 64 32
do
	case "$model" in
	32) architecture_flags="-m32 -march=i386 -msoft-float -mno-mmx -mno-sse -mno-sse2 -Wa,--noexecstack" ;;
	64) architecture_flags="-m64 -march=x86-64" ;;
	esac
	objects=""
	for source in "$c32_dir/dos/ems/core.c" \
		"$c32_dir/dos/ems/vcpi.c" "$test_dir/dos_ems_test.c"
	do
		object="$temporary_dir/$(basename "${source%.c}").m$model.o"
		# shellcheck disable=SC2086 # Deliberate compiler argument list.
		"$test_cc" $architecture_flags $common_flags \
			-I"$c32_dir/include" -I"$c32_dir/dos/ems" \
			-I"$test_dir" -c "$source" -o "$object"
		objects="$objects $object"
	done
	# shellcheck disable=SC2086 # Deliberate object list.
	"$test_cc" $architecture_flags -nostdlib -static -no-pie \
		-Wl,-e,_start -Wl,--build-id=none $objects \
		-o "$temporary_dir/dos-ems-$model"
done

"$temporary_dir/dos-ems-64"
if command -v qemu-i386 >/dev/null 2>&1
then
	qemu-i386 "$temporary_dir/dos-ems-32"
elif command -v qemu-system-i386 >/dev/null 2>&1
then
	# Execute the same freestanding m32 C through a tiny automated test kernel.
	# The debug-exit device returns host status 1 when the guest writes zero.
	# shellcheck disable=SC2086 # Deliberate compiler argument list.
	"$test_cc" -m32 -march=i386 -msoft-float -mno-mmx -mno-sse \
		-mno-sse2 -Wa,--noexecstack $common_flags \
		-DDOSC32_QEMU_SYSTEM_TEST=1 \
		-I"$c32_dir/include" -I"$c32_dir/dos/ems" -I"$test_dir" \
		-c "$test_dir/dos_ems_test.c" \
		-o "$temporary_dir/dos_ems_test.system.o"
	"$test_cc" -m32 -march=i386 -c "$test_dir/dos_ems_test_boot.S" \
		-o "$temporary_dir/dos_ems_test_boot.o"
	"$test_cc" -m32 -nostdlib -static -no-pie \
		-Wl,-T,"$test_dir/dos_ems_test.ld" -Wl,--build-id=none \
		"$temporary_dir/core.m32.o" \
		"$temporary_dir/vcpi.m32.o" \
		"$temporary_dir/dos_ems_test.system.o" \
		"$temporary_dir/dos_ems_test_boot.o" \
		-o "$temporary_dir/dos-ems-system-32"
	set +e
	"$test_dir/run-with-timeout.sh" 10 qemu-system-i386 \
		-machine pc,accel=tcg -m "${qemu_memory_mib}M" \
		-display "$qemu_display" -serial none \
		-monitor none -no-reboot \
		-device isa-debug-exit,iobase=0xf4,iosize=0x04 \
		-kernel "$temporary_dir/dos-ems-system-32"
	qemu_status=$?
	set -e
	if [ "$qemu_status" -ne 1 ]; then
		echo "m32 EMS/VCPI system test failed: status $qemu_status" >&2
		exit 1
	fi
else
	echo "m32 execution skipped: no qemu-i386 or qemu-system-i386" >&2
	exit 1
fi

echo "DOS EMS/VCPI tests passed: EMS 4.0 allocation and VCPI 1.0 services"

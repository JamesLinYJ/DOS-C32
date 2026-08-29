#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root_dir=$(CDPATH= cd -- "$test_dir/.." && pwd)
test_cc=${CC:-"$test_dir/test-cc"}
temporary_dir=$(mktemp -d "${TMPDIR:-/tmp}/dos-c32-legacy-input.XXXXXX")

cleanup()
{
	rm -f -- "$temporary_dir"/*.o "$temporary_dir"/legacy-input-m32 \
		"$temporary_dir"/legacy-input-m64
	rmdir -- "$temporary_dir"
}
trap cleanup EXIT HUP INT TERM

common_flags="-DDOSC32_HOST_TEST=1 -std=gnu11 -O2 -ffreestanding"
common_flags="$common_flags -fno-builtin -fno-pie -fno-pic"
common_flags="$common_flags -fno-stack-protector -fno-asynchronous-unwind-tables"
common_flags="$common_flags -fno-unwind-tables -Wall -Wextra -Werror -Wundef"
common_flags="$common_flags -Wshadow -Wstrict-prototypes -Wmissing-prototypes"
common_flags="$common_flags -Wvla -Wformat=2 -Wcast-align=strict"
common_flags="$common_flags -Wnull-dereference"
common_flags="$common_flags -DCONFIG_X86_BOOT_IDENTITY_FLOOR=0x00800000"
common_flags="$common_flags -DCONFIG_X86_BOOT_IDENTITY_CEILING=0x10000000"

compile_source()
{
	model=$1
	architecture_flags=$2
	source=$3
	name=$4
	# shellcheck disable=SC2086 # Deliberate compiler argument lists.
	"$test_cc" $architecture_flags $common_flags \
		-I"$root_dir/include" -I"$test_dir" \
		-c "$root_dir/$source" -o "$temporary_dir/$name-$model.o"
}

for model in 32 64
do
	case "$model" in
	32) architecture_flags="-m32 -march=i386 -msoft-float -mno-mmx \
		-mno-sse -mno-sse2" ;;
	64) architecture_flags="-m64 -march=x86-64" ;;
	esac
	compile_source "$model" "$architecture_flags" \
		kernel/input/serio/registry.c serio_registry
	compile_source "$model" "$architecture_flags" \
		kernel/input/serio/binding.c serio_binding
	compile_source "$model" "$architecture_flags" \
		kernel/input/serio/dispatch.c serio_dispatch
	compile_source "$model" "$architecture_flags" \
		kernel/input/core/lifecycle.c input_lifecycle
	compile_source "$model" "$architecture_flags" \
		kernel/input/core/routing.c input_routing
	compile_source "$model" "$architecture_flags" \
		kernel/input/keyboard/atkbd/maps.c atkbd_maps
	compile_source "$model" "$architecture_flags" \
		kernel/input/keyboard/atkbd/decode.c atkbd_decode
	compile_source "$model" "$architecture_flags" \
		kernel/input/keyboard/atkbd/lifecycle.c atkbd_lifecycle
	compile_source "$model" "$architecture_flags" \
		kernel/input/keyboard/atkbd/interrupt.c atkbd_interrupt
	compile_source "$model" "$architecture_flags" \
		kernel/input/keyboard/atkbd/command.c atkbd_command
	compile_source "$model" "$architecture_flags" \
		kernel/input/keyboard/guest_ps2/maps.c guest_maps
	compile_source "$model" "$architecture_flags" \
		kernel/input/keyboard/guest_ps2/encode.c guest_encode
	compile_source "$model" "$architecture_flags" \
		kernel/input/keyboard/guest_ps2/lifecycle.c guest_lifecycle
	compile_source "$model" "$architecture_flags" \
		kernel/input/keyboard/guest_ps2/dispatch.c guest_dispatch
	compile_source "$model" "$architecture_flags" \
		kernel/input/console/keymap_us.c console_keymap
	compile_source "$model" "$architecture_flags" \
		kernel/keyboard.c keyboard_console
	compile_source "$model" "$architecture_flags" \
		kernel/x86_vm/platform/native_i8042.c native_i8042
	compile_source "$model" "$architecture_flags" \
		kernel/x86_vm/platform/native_input.c native_input
	compile_source "$model" "$architecture_flags" \
		kernel/x86_vm/platform/legacy_input_runtime.c legacy_runtime
	compile_source "$model" "$architecture_flags" \
		tests/x86_legacy_input_runtime_test.c runtime_test
	# shellcheck disable=SC2086 # Deliberate compiler argument lists.
	"$test_cc" $architecture_flags -nostdlib -static -no-pie \
		-Wl,-e,_start -Wl,--build-id=none \
		"$temporary_dir/serio_registry-$model.o" \
		"$temporary_dir/serio_binding-$model.o" \
		"$temporary_dir/serio_dispatch-$model.o" \
		"$temporary_dir/input_lifecycle-$model.o" \
		"$temporary_dir/input_routing-$model.o" \
		"$temporary_dir/atkbd_maps-$model.o" \
		"$temporary_dir/atkbd_decode-$model.o" \
		"$temporary_dir/atkbd_lifecycle-$model.o" \
		"$temporary_dir/atkbd_interrupt-$model.o" \
		"$temporary_dir/atkbd_command-$model.o" \
		"$temporary_dir/guest_maps-$model.o" \
		"$temporary_dir/guest_encode-$model.o" \
		"$temporary_dir/guest_lifecycle-$model.o" \
		"$temporary_dir/guest_dispatch-$model.o" \
		"$temporary_dir/console_keymap-$model.o" \
		"$temporary_dir/keyboard_console-$model.o" \
		"$temporary_dir/native_i8042-$model.o" \
		"$temporary_dir/native_input-$model.o" \
		"$temporary_dir/legacy_runtime-$model.o" \
		"$temporary_dir/runtime_test-$model.o" \
		-o "$temporary_dir/legacy-input-m$model"
done

"$temporary_dir/legacy-input-m64"
m32_result="linked only"
if command -v qemu-i386 >/dev/null 2>&1; then
	qemu-i386 "$temporary_dir/legacy-input-m32"
	m32_result="executed"
fi

echo "x86 legacy-input runtime tests passed: m64 executed, m32 $m32_result"
echo "Covered tri-state write rollback/quarantine, EOI-safe source backpressure, exact loss recovery/isolation, focus, FIFO order and ATKBD resend"

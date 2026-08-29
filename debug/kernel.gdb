# SPDX-License-Identifier: GPL-2.0-only
# Run from the repository root after `make run-gdb`.
set pagination off
set confirm off
set architecture i386
file build/kernel.elf
target remote :1234

# The BIOS starts at the reset vector.  Continue to the low staging entry;
# high linked symbols become valid after boot/entry.S copies the runtime image.
break *_start
break relocated_entry
break kmain

define dosc32-layout
  printf "staging entry:  0x%08x\n", __kernel_staging_start
  printf "runtime start:  0x%08x\n", __kernel_runtime_start
  printf "runtime end:    0x%08x\n", __kernel_runtime_end
end

echo Connected to paused Legacy-BIOS guest. Use `continue` to reach _start.\n

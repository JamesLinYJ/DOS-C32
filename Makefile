# SPDX-License-Identifier: GPL-2.0-only
SHELL := /bin/bash

HOST_SYSTEM := $(shell uname -s)
HOST_MACHINE := $(shell uname -m)
HOST_CC ?= cc

ifeq ($(HOST_SYSTEM)-$(HOST_MACHINE),Darwin-arm64)
CROSS_COMPILE ?= i686-elf-
HOST_TEST_CC ?= $(abspath tests/test-cc)
MKFS_FAT ?= /opt/homebrew/sbin/mkfs.fat
FSCK_FAT ?= /opt/homebrew/sbin/fsck.fat
MCOPY ?= /opt/homebrew/bin/mcopy
MATTRIB ?= /opt/homebrew/bin/mattrib
QEMU_DISPLAY ?= cocoa,show-cursor=on
else
CROSS_COMPILE ?=
HOST_TEST_CC ?= gcc
MKFS_FAT ?= mkfs.fat
FSCK_FAT ?= fsck.fat
MCOPY ?= mcopy
MATTRIB ?= mattrib
QEMU_DISPLAY ?= none
endif

export QEMU_DISPLAY
QEMU_MEMORY_MIB ?= 256
export QEMU_MEMORY_MIB

CC := $(CROSS_COMPILE)gcc
LD := $(CROSS_COMPILE)ld
OBJCOPY := $(CROSS_COMPILE)objcopy
AR := $(CROSS_COMPILE)ar
NM := $(CROSS_COMPILE)nm
READELF := $(CROSS_COMPILE)readelf
AS := $(CROSS_COMPILE)as
OPTIMIZATION ?= -O2
BOOT_SELFTESTS ?= 0
X86_VM_ACCEPTANCE_DIAGNOSTICS ?= 0
X86_DEBUGCON ?= 0
X86_VGA_DAC_PALETTE ?= 0

include config/legacy-bios.mk
include config/xms.mk

export X86_BOOT_IDENTITY_FLOOR
export X86_BOOT_IDENTITY_CEILING
export DOS_XMS_HMA_MINIMUM_BYTES

BUILD := build
IMAGE := $(BUILD)/msdos-c32.img
HARD_DISK_IMAGE := $(BUILD)/msdos-c32-hdd.img
MBR_OBJECT := $(BUILD)/boot/mbr.o
MBR_FILE := $(BUILD)/mbr.bin
PARTITION_START_LBA ?= 2048
HARD_DISK_IMAGE_KIB ?= 65536
HARD_DISK_FAT_SECTORS ?= 128
IO_SYSTEM_OBJECT := $(BUILD)/boot/io_loader.o
IO_SYSTEM_FILE := $(BUILD)/IO.SYS
DOS_KERNEL_FILE := $(BUILD)/DOSKRNL.SYS
COMMAND_C_OBJECT := $(BUILD)/command/command.o
COMMAND_DIRECTORY_OBJECT := $(BUILD)/command/directory.o
COMMAND_PATH_OBJECT := $(BUILD)/command/path.o
COMMAND_EXTERNAL_OBJECT := $(BUILD)/command/external_command.o
COMMAND_DOS_PATH_OBJECT := $(BUILD)/command/dos_path.o
COMMAND_STRING_OBJECT := $(BUILD)/command/string.o
COMMAND_OBJECTS := $(COMMAND_C_OBJECT) $(COMMAND_DIRECTORY_OBJECT) \
	$(COMMAND_PATH_OBJECT) $(COMMAND_EXTERNAL_OBJECT) \
	$(COMMAND_DOS_PATH_OBJECT) $(COMMAND_STRING_OBJECT)
COMMAND_FILE := $(BUILD)/COMMAND.COM
COUNTRY_FILE := $(BUILD)/COUNTRY.SYS
COUNTRY_TOOL := $(BUILD)/tools/mkcountry
DOS_PROGRAM_SMOKE_OBJECT := $(BUILD)/tests/dos-program-smoke.o
DOS_PROGRAM_SMOKE_COM := $(BUILD)/HELLO.COM
DOS_HARDWARE_PROBE_OBJECT := $(BUILD)/tests/dos-hardware-probe.o
DOS_HARDWARE_PROBE_COM := $(BUILD)/HWPROBE.COM
DOS_BIOS_KEYBOARD_IRQ_OBJECT := $(BUILD)/tests/dos-bios-keyboard-irq.o
DOS_BIOS_KEYBOARD_IRQ_COM := $(BUILD)/KEYIRQ.COM

CPPFLAGS := -Iinclude
CPPFLAGS += -DCONFIG_BOOT_SELFTESTS=$(BOOT_SELFTESTS)
CPPFLAGS += -DCONFIG_X86_VM_ACCEPTANCE_DIAGNOSTICS=$(X86_VM_ACCEPTANCE_DIAGNOSTICS)
CPPFLAGS += -DCONFIG_X86_DEBUGCON=$(X86_DEBUGCON)
CPPFLAGS += -DCONFIG_X86_VGA_DAC_PALETTE=$(X86_VGA_DAC_PALETTE)
CPPFLAGS += -DCONFIG_X86_BOOT_IDENTITY_FLOOR=$(X86_BOOT_IDENTITY_FLOOR)
CPPFLAGS += -DCONFIG_X86_BOOT_IDENTITY_CEILING=$(X86_BOOT_IDENTITY_CEILING)
CPPFLAGS += -DCONFIG_DOS_XMS_HMA_MINIMUM_BYTES=$(DOS_XMS_HMA_MINIMUM_BYTES)
CPPFLAGS += -DCONFIG_X86_SERIAL_BAUD_DIVISOR=$(X86_SERIAL_BAUD_DIVISOR)
CPPFLAGS += -DCONFIG_X86_SERIAL_POLL_LIMIT=$(X86_SERIAL_POLL_LIMIT)
CPPFLAGS += -DCONFIG_X86_ATA_POLL_LIMIT=$(X86_ATA_POLL_LIMIT)
CPPFLAGS += -DCONFIG_X86_ATA_WRITE_POLICY=$(X86_ATA_WRITE_POLICY)
CFLAGS := -m32 -march=i386 -mtune=i386 -msoft-float -std=gnu11 \
	$(OPTIMIZATION) -g \
	-ffreestanding -fno-builtin -fcf-protection=none \
	-fno-pic -fno-pie -fstack-protector-strong \
	-mstack-protector-guard=global -fno-asynchronous-unwind-tables \
	-fno-unwind-tables -fomit-frame-pointer \
	-fno-delete-null-pointer-checks -fno-strict-aliasing \
	-mno-mmx -mno-sse -mno-sse2 -Wall -Wextra -Werror -Wundef -Wshadow \
	-Wstrict-prototypes -Wmissing-prototypes -Wvla -Wformat=2 \
	-Wcast-align=strict -Wnull-dereference
COMMAND_CFLAGS := -m32 -march=i386 -std=gnu11 -Os -ffreestanding \
	-fno-builtin -fno-pic -fno-pie -fno-stack-protector \
	-ffunction-sections -fdata-sections \
	-fno-asynchronous-unwind-tables -fno-unwind-tables \
	-Wall -Wextra -Werror -Wundef -Wshadow -Wstrict-prototypes \
	-Wmissing-prototypes -Wvla
ASFLAGS := -m32 -march=i386 -mtune=i386 -msoft-float -ffreestanding \
	-fno-pic -fno-pie -fcf-protection=none

C_SOURCES := \
	kernel/main.c \
	kernel/object_identity.c \
	kernel/console.c \
	kernel/input/core/lifecycle.c \
	kernel/input/core/routing.c \
	kernel/input/serio/registry.c \
	kernel/input/serio/binding.c \
	kernel/input/serio/dispatch.c \
	kernel/input/keyboard/atkbd/maps.c \
	kernel/input/keyboard/atkbd/decode.c \
	kernel/input/keyboard/atkbd/lifecycle.c \
	kernel/input/keyboard/atkbd/interrupt.c \
	kernel/input/keyboard/atkbd/command.c \
	kernel/input/keyboard/guest_ps2/maps.c \
	kernel/input/keyboard/guest_ps2/encode.c \
	kernel/input/keyboard/guest_ps2/lifecycle.c \
	kernel/input/keyboard/guest_ps2/dispatch.c \
	kernel/input/console/keymap_us.c \
	kernel/input/console/wait_x86.c \
	kernel/x86_vm/fault_ui.c \
	kernel/keyboard.c \
	kernel/block_device.c \
	kernel/ata_device.c \
	kernel/ata.c \
	kernel/ata_block.c \
	kernel/stack_protector.c \
	kernel/x86_vm/guest_space.c \
	kernel/x86_vm/irq/guest_router.c \
	kernel/x86_vm/irq/guest_topology.c \
	kernel/x86_vm/irq/guest_dispatch.c \
	kernel/x86_vm/irq/native_dispatch.c \
	kernel/x86_vm/irq/native_action.c \
	kernel/x86_vm/irq/native_vector.c \
	kernel/x86_vm/io/resource.c \
	kernel/x86_vm/chipset/policy.c \
	kernel/x86_vm/chipset/owner.c \
	kernel/x86_vm/chipset/dma.c \
	kernel/x86_vm/chipset/pic.c \
	kernel/x86_vm/chipset/pit.c \
	kernel/x86_vm/chipset/rtc.c \
	kernel/x86_vm/chipset/i8042.c \
	kernel/x86_vm/platform/boot_storage.c \
	kernel/x86_vm/platform/display.c \
	kernel/x86_vm/platform/legacy_bios.c \
	kernel/x86_vm/platform/legacy_pic.c \
	kernel/x86_vm/platform/native_i8042.c \
	kernel/x86_vm/platform/native_input.c \
	kernel/x86_vm/platform/legacy_input_runtime.c \
	kernel/x86_vm/platform/vcpi.c \
	kernel/x86_vm/memory/map.c \
	kernel/x86_vm/memory/physical.c \
	kernel/x86_vm/memory/runtime.c \
	kernel/x86_vm/memory/firmware_shadow.c \
	kernel/x86_vm/memory/ems.c \
	kernel/x86_vm/memory/ems_config.c \
	kernel/x86_vm/memory/xms.c \
	kernel/x86_page_policy.c \
	kernel/x86_paging.c \
	kernel/x86_legacy_irq.c \
	kernel/x86_runtime.c \
	kernel/x86_user.c \
	kernel/x86_vm/vm86_firmware.c \
	kernel/x86_vm/vm86.c \
	kernel/c32_image.c \
	kernel/c32_process.c \
	dos/machine.c \
	dos/loader.c \
	dos/image_load.c \
	dos/environment.c \
	dos/environment/view.c \
	dos/relocator.c \
	dos/process.c \
	dos/process_runtime.c \
	dos/termination.c \
	dos/exec_observer.c \
	dos/exec_gate.c \
	dos/exec_transaction.c \
	dos/exec_executor.c \
	dos/exec_int21.c \
	dos/exec_native.c \
	dos/exec_journal.c \
	dos/exec_handoff.c \
	dos/exec_backend_session.c \
	dos/execution_loop.c \
	dos/exec_file_lease.c \
	dos/exec_name.c \
	dos/exec_parameter.c \
	dos/exec_overlay.c \
	dos/exec_seal.c \
	dos/sft_batch.c \
	dos/sft_adapter.c \
	dos/drive_visibility.c \
	dos/drive/config.c \
	dos/drive/dpb.c \
	dos/find/record.c \
	dos/ui.c \
	dos/error.c \
	dos/memory.c \
	dos/jft/resize.c \
	dos/memory_lease.c \
	dos/vectors.c \
	dos/interrupt_reflection.c \
	dos/control_instruction.c \
	dos/port_instruction.c \
	dos/ems/core.c \
	dos/ems/vcpi.c \
	dos/ems/iomgr_device.c \
	dos/xms/hma.c \
	dos/xms/manager.c \
	dos/nls/package.c \
	dos/nls/country_file.c \
	dos/int21.c \
	dos/personality.c \
	dos/runtime_owner.c \
	dos/path.c \
	storage/core/manager.c \
	storage/core/device.c \
	storage/core/discovery.c \
	storage/core/exec_adapter.c \
	storage/core/transaction.c \
	storage/fat/boot.c \
	storage/fat/driver.c \
	storage/fat/entry.c \
	storage/fat/named.c \
	storage/fat/mutation.c \
	shell/external_command.c \
	shell/shell.c
ifeq ($(BOOT_SELFTESTS),1)
C_SOURCES += kernel/x86_vm/vm86_selftest.c
endif
LIBC32_SOURCES := \
	libc32/assert.c \
	libc32/string.c \
	libc32/ctype.c \
	libc32/format.c \
	libc32/convert.c \
	libc32/math64.c \
	libc32/arena.c
ASM_SOURCES := boot/entry.S boot/x86_traps.S
OBJECTS := $(patsubst %.c,$(BUILD)/%.o,$(C_SOURCES)) \
	$(patsubst %.S,$(BUILD)/%.o,$(ASM_SOURCES))
LIBC32_OBJECTS := $(patsubst %.c,$(BUILD)/%.o,$(LIBC32_SOURCES))
LIBC32_ARCHIVE := $(BUILD)/libc32-core.a
DEPFILES := $(OBJECTS:.o=.d) $(LIBC32_OBJECTS:.o=.d)
IOMGR_DEVICE_TEST_SOURCES := storage/core/device.c \
	tests/iomgr_device_test.c

.PHONY: all image hard-disk-image kernel system-files libc32 check libc32-test \
	libc32-extended-test \
	qemu-memory-topology-test \
	portable64-check object-identity-test block-device-test ata-device-test \
	ata-block-test \
	iomgr-test \
	iomgr-discovery-test \
	iomgr-device-test \
	iomgr-transaction-test \
	fat-table-test fat-volume-test fat-driver-test \
	x86-paging-policy-test x86-guest-memory-test x86-ems-memory-test \
	x86-xms-memory-test \
	x86-io-resource-test x86-legacy-irq-test x86-legacy-pic-test \
	x86-legacy-chipset-test \
	x86-i8042-test x86-native-i8042-test input-core-test \
	serio-native-input-test atkbd-test keyboard-console-test \
	guest-ps2-keyboard-test x86-legacy-input-runtime-test \
	x86-interrupt-router-test \
	x86-guest-space-test x86-vm86-firmware-release-test x86-vm86-halt-test \
	x86-boot-storage-test x86-legacy-bios-test x86-display-test \
	shell-capacity-test external-command-test command-path-test \
	command-directory-test \
	dos-abi-test \
	dos-error-test \
	dos-machine-test \
	dos-ems-test dos-ems-integration-test \
	dos-xms-test \
	dos-interrupt-reflection-test \
	dos-control-instruction-test \
	dos-port-instruction-test \
	dos-memory-test \
	dos-memory-lease-test \
	dos-jft-test \
	dos-vectors-test \
	dos-drive-test \
	dos-dpb-test \
	dos-country-file-test \
	dos-int21-test \
	dos-termination-test \
	dos-process-test \
	dos-process-runtime-test \
	dos-runtime-owner-test \
	dos-exec-observer-test \
	dos-exec-production-adapters-test \
	dos-exec-transaction-test \
	dos-exec-journal-test \
	dos-exec-handoff-test \
	dos-exec-backend-session-test \
	dos-exec-file-lease-test \
	dos-exec-name-test \
	dos-exec-parameter-test \
	dos-exec-overlay-test \
	dos-exec-seal-test \
	dos-sft-batch-test \
	dos-environment-test \
	dos-environment-view-test \
	dos-relocator-test \
	dos-loader-test \
	dos-image-load-test \
	probe-dos-program \
	dos-program-smoke-test dos-hardware-probe-test \
	dos-bios-keyboard-irq-test dos-compat-test \
	c32-command-vm86-test \
	fat16-corruption-test forbidden-api symbol-audit production-audit \
	acceptance-hack-audit \
	boot-check acceptance-diagnostic-image windows32-vhd clean run \
	run-debug run-gdb help

all: image

image: $(IMAGE)

kernel: $(BUILD)/kernel.elf $(DOS_KERNEL_FILE)

system-files: $(IO_SYSTEM_FILE) $(DOS_KERNEL_FILE) $(COMMAND_FILE) \
	$(COUNTRY_FILE)

$(COUNTRY_TOOL): tools/mkcountry.c dos/nls/package.c \
	dos/nls/country_file.c libc32/string.c include/dos_country_file.h \
	include/dos_nls.h include/types.h include/string.h Makefile
	@mkdir -p $(@D)
	$(HOST_CC) -std=gnu11 -O2 -DDOSC32_HOSTED_TYPES -Iinclude \
		-Wall -Wextra -Werror -Wundef -Wshadow -Wstrict-prototypes \
		-Wmissing-prototypes -Wvla -Wformat=2 \
		tools/mkcountry.c dos/nls/package.c dos/nls/country_file.c \
		libc32/string.c -o $@

$(COUNTRY_FILE): $(COUNTRY_TOOL)
	$(COUNTRY_TOOL) $@

$(COMMAND_C_OBJECT): command/command.c command/directory.h command/path.h \
	include/c32_syscall.h include/dos_path.h \
	include/shell_external_command.h include/string.h include/types.h Makefile
$(COMMAND_DIRECTORY_OBJECT): command/directory.c command/directory.h \
	include/compiler.h include/string.h include/types.h Makefile
$(COMMAND_PATH_OBJECT): command/path.c command/path.h include/compiler.h \
	include/string.h include/types.h Makefile
$(COMMAND_EXTERNAL_OBJECT): shell/external_command.c \
	include/shell_external_command.h include/dos_path.h include/string.h \
	include/types.h Makefile
$(COMMAND_DOS_PATH_OBJECT): dos/path.c include/dos_path.h include/string.h \
	include/types.h Makefile
$(COMMAND_STRING_OBJECT): libc32/string.c include/string.h include/overflow.h \
	include/types.h Makefile

$(COMMAND_OBJECTS):
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Icommand $(COMMAND_CFLAGS) -c $< -o $@

$(COMMAND_FILE): $(COMMAND_OBJECTS) command/command.ld
	$(LD) -m elf_i386 --gc-sections -T command/command.ld \
		--oformat binary -o $@ $(COMMAND_OBJECTS)

$(MBR_OBJECT): boot/mbr.S Makefile
	@mkdir -p $(@D)
	@total_sectors=$$(( $(HARD_DISK_IMAGE_KIB) * 2 )); \
	partition_sectors=$$(( total_sectors - $(PARTITION_START_LBA) )); \
	$(CC) $(ASFLAGS) -DPARTITION_START_LBA=$(PARTITION_START_LBA) \
		-DPARTITION_SECTORS=$$partition_sectors -c $< -o $@

$(MBR_FILE): $(MBR_OBJECT) boot/mbr.ld
	$(LD) -m elf_i386 -T boot/mbr.ld --oformat binary -o $@ $<
	@test "$$(wc -c < $@)" -eq 512

hard-disk-image: $(MBR_FILE)
	@partition_sectors=$$(( $(HARD_DISK_IMAGE_KIB) * 2 - \
		$(PARTITION_START_LBA) )); \
	partition_kib=$$(( partition_sectors / 2 )); \
	$(MAKE) BUILD=$(BUILD)/hdd-volume IMAGE_KIB=$$partition_kib \
		FAT_HIDDEN_SECTORS=$(PARTITION_START_LBA) \
		FAT_SECTORS_PER_COPY=$(HARD_DISK_FAT_SECTORS) image
	dd if=/dev/zero of=$(HARD_DISK_IMAGE) bs=1024 \
		count=$(HARD_DISK_IMAGE_KIB) status=none
	dd if=$(MBR_FILE) of=$(HARD_DISK_IMAGE) bs=512 seek=0 \
		conv=notrunc status=none
	dd if=$(BUILD)/hdd-volume/msdos-c32.img of=$(HARD_DISK_IMAGE) \
		bs=512 seek=$(PARTITION_START_LBA) conv=notrunc status=none
	@echo "Built MBR/FAT16 hard-disk image: $(HARD_DISK_IMAGE)"

libc32: $(LIBC32_ARCHIVE)

$(DOS_PROGRAM_SMOKE_OBJECT): tests/dos-program-smoke.S
	@mkdir -p $(@D)
	$(AS) --32 $< -o $@

$(DOS_PROGRAM_SMOKE_COM): $(DOS_PROGRAM_SMOKE_OBJECT)
	$(LD) -m elf_i386 -Ttext 0x100 --oformat binary -o $@ $<

$(DOS_HARDWARE_PROBE_OBJECT): tests/dos-hardware-probe.S
	@mkdir -p $(@D)
	$(AS) --32 $< -o $@

$(DOS_HARDWARE_PROBE_COM): $(DOS_HARDWARE_PROBE_OBJECT)
	$(LD) -m elf_i386 -Ttext 0x100 --oformat binary -o $@ $<

$(DOS_BIOS_KEYBOARD_IRQ_OBJECT): tests/dos-bios-keyboard-irq.S
	@mkdir -p $(@D)
	$(AS) --32 $< -o $@

$(DOS_BIOS_KEYBOARD_IRQ_COM): $(DOS_BIOS_KEYBOARD_IRQ_OBJECT)
	$(LD) -m elf_i386 -Ttext 0x100 --oformat binary -o $@ $<

# Keep unused freestanding library entry points out of the fixed BIOS load
# window while retaining every ordinary DOS module text section.
$(LIBC32_OBJECTS): CFLAGS += -ffunction-sections -fdata-sections

$(BUILD)/%.o: %.c Makefile
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.o: %.S
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(ASFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/boot/entry.o: CPPFLAGS += \
	-DKERNEL_LOAD_BASE=$(KERNEL_LOAD_BASE) \
	-DKERNEL_RUNTIME_BASE=$(KERNEL_RUNTIME_BASE) \
	-DKERNEL_STACK_FLOOR=$(KERNEL_STACK_FLOOR) \
	-DKERNEL_STACK_TOP=$(KERNEL_STACK_TOP) \
	-DCONFIG_X86_A20_ENABLE_ATTEMPTS=$(X86_A20_ENABLE_ATTEMPTS) \
	-DCONFIG_X86_A20_SHORT_TEST_LOOPS=$(X86_A20_SHORT_TEST_LOOPS) \
	-DCONFIG_X86_A20_LONG_TEST_LOOPS=$(X86_A20_LONG_TEST_LOOPS) \
	-DCONFIG_X86_A20_KBC_POLL_LIMIT=$(X86_A20_KBC_POLL_LIMIT) \
	-DCONFIG_X86_A20_KBC_ABSENT_SAMPLES=$(X86_A20_KBC_ABSENT_SAMPLES) \
	-DCONFIG_X86_STACK_GUARD_RDRAND_ATTEMPTS=$(X86_STACK_GUARD_RDRAND_ATTEMPTS) \
	-DCONFIG_X86_BOOT_RTC_SNAPSHOT_ATTEMPTS=$(X86_BOOT_RTC_SNAPSHOT_ATTEMPTS)
$(BUILD)/boot/entry.o: config/legacy-bios.mk Makefile

$(LIBC32_ARCHIVE): $(LIBC32_OBJECTS)
	@mkdir -p $(@D)
	$(AR) rcsD $@ $^

$(BUILD)/kernel.elf: linker.ld $(OBJECTS) $(LIBC32_ARCHIVE)
	@mkdir -p $(@D)
	@load_limit=$$(( $(KERNEL_LOAD_BASE) + \
		($(FAT_RESERVED_SECTORS) - 1) * $(BIOS_SECTOR_BYTES) )); \
	$(LD) -m elf_i386 --gc-sections -z noexecstack -T linker.ld \
		--defsym=__kernel_load_base=$(KERNEL_LOAD_BASE) \
		--defsym=__kernel_load_limit=$$load_limit \
		--defsym=__kernel_runtime_base=$(KERNEL_RUNTIME_BASE) \
		--defsym=__kernel_stack_floor=$(KERNEL_STACK_FLOOR) \
		--defsym=__kernel_stack_top=$(KERNEL_STACK_TOP) \
		-Map $(BUILD)/kernel.map -o $@ \
		$(OBJECTS) $(LIBC32_ARCHIVE)

$(DOS_KERNEL_FILE): $(BUILD)/kernel.elf
	$(OBJCOPY) -O binary $< $@
	@bytes=$$(wc -c < $@); \
	max=$$((($(FAT_RESERVED_SECTORS) - 1) * $(BIOS_SECTOR_BYTES))); \
	if (( bytes == 0 || bytes > max )); then \
		echo "DOSKRNL.SYS is $$bytes bytes; staging area allows $$max" >&2; \
		exit 1; \
	fi

$(IO_SYSTEM_OBJECT): boot/io_loader.S config/legacy-bios.mk Makefile
	@mkdir -p $(@D)
	@load_limit=$$(( $(KERNEL_LOAD_BASE) + \
		($(FAT_RESERVED_SECTORS) - 1) * $(BIOS_SECTOR_BYTES) )); \
	sector_paragraphs=$$(( $(BIOS_SECTOR_BYTES) / 16 )); \
	$(CC) $(ASFLAGS) -DBIOS_SECTOR_BYTES=$(BIOS_SECTOR_BYTES) \
		-DBIOS_SECTOR_PARAGRAPHS=$$sector_paragraphs \
		-DBIOS_READ_RETRIES=$(BIOS_READ_RETRIES) \
		-DCONFIG_X86_DEBUGCON=$(X86_DEBUGCON) \
		-DKERNEL_LOAD_BASE=$(KERNEL_LOAD_BASE) \
		-DKERNEL_LOAD_LIMIT=$$load_limit -c $< -o $@

$(IO_SYSTEM_FILE): $(IO_SYSTEM_OBJECT) boot/io_loader.ld
	$(LD) -m elf_i386 -T boot/io_loader.ld --oformat binary -o $@ $<
	@bytes=$$(wc -c < $@); \
	max=$$(( $(FAT_SECTORS_PER_CLUSTER) * $(BIOS_SECTOR_BYTES) )); \
	if (( bytes == 0 || bytes > max )); then \
		echo "IO.SYS is $$bytes bytes; stage one allows $$max" >&2; \
		exit 1; \
	fi

$(BUILD)/boot/boot.o: boot/boot.S $(IO_SYSTEM_FILE) \
	config/legacy-bios.mk Makefile
	@mkdir -p $(@D)
	@image_sectors=$$(( $(IMAGE_KIB) * $(BYTES_PER_KIB) / \
		$(BIOS_SECTOR_BYTES) )); \
	sector_paragraphs=$$(( $(BIOS_SECTOR_BYTES) / 16 )); \
	io_max_bytes=$$(( $(FAT_SECTORS_PER_CLUSTER) * \
		$(BIOS_SECTOR_BYTES) )); \
	$(CC) $(ASFLAGS) \
		-DBIOS_BOOT_LOAD_ADDRESS=$(BIOS_BOOT_LOAD_ADDRESS) \
		-DBIOS_SECTOR_BYTES=$(BIOS_SECTOR_BYTES) \
		-DBIOS_SECTOR_PARAGRAPHS=$$sector_paragraphs \
		-DBIOS_READ_RETRIES=$(BIOS_READ_RETRIES) \
		-DCONFIG_X86_DEBUGCON=$(X86_DEBUGCON) \
		-DIO_LOAD_BASE=$(IO_SYSTEM_LOAD_BASE) \
		-DIO_MAX_BYTES=$$io_max_bytes \
		-DFAT_SECTORS_PER_CLUSTER=$(FAT_SECTORS_PER_CLUSTER) \
		-DFAT_RESERVED_SECTORS=$(FAT_RESERVED_SECTORS) \
		-DFAT_COPIES=$(FAT_COPIES) \
		-DFAT_ROOT_ENTRIES=$(FAT_ROOT_ENTRIES) \
		-DFAT_SECTORS_PER_COPY=$(FAT_SECTORS_PER_COPY) \
		-DFAT_MEDIA_DESCRIPTOR=$(FAT_MEDIA_DESCRIPTOR) \
		-DFAT_HIDDEN_SECTORS=$(FAT_HIDDEN_SECTORS) \
		-DBIOS_HEADS=$(BIOS_HEADS) \
		-DBIOS_SECTORS_PER_TRACK=$(BIOS_SECTORS_PER_TRACK) \
		-DBIOS_DRIVE_NUMBER=$(BIOS_DRIVE_NUMBER) \
		-DIMAGE_SECTORS=$$image_sectors \
		-DIMAGE_VOLUME_ID=0x$(IMAGE_VOLUME_ID) -c $< -o $@

$(BUILD)/boot.bin: boot/boot.ld $(BUILD)/boot/boot.o
	$(LD) -m elf_i386 -T boot/boot.ld \
		--defsym=__bios_boot_load_address=$(BIOS_BOOT_LOAD_ADDRESS) \
		--oformat binary -o $@ $(BUILD)/boot/boot.o
	@test "$$(wc -c < $@)" -eq $(BIOS_SECTOR_BYTES)
	@signature_offset=$$(( $(BIOS_SECTOR_BYTES) - 2 )); \
	test "$$(od -An -tx1 -j$$signature_offset -N2 $@ | tr -d ' \n')" = \
		"55aa"

$(IMAGE): $(BUILD)/boot.bin $(IO_SYSTEM_FILE) $(DOS_KERNEL_FILE) \
	$(COMMAND_FILE) $(COUNTRY_FILE) \
	assets/README.TXT assets/AUTOEXEC.BAT assets/WELCOME.TXT
	@mkdir -p $(@D)
	rm -f $@
	$(MKFS_FAT) -C -a -F 16 -S $(BIOS_SECTOR_BYTES) \
		-s $(FAT_SECTORS_PER_CLUSTER) -R $(FAT_RESERVED_SECTORS) \
		-r $(FAT_ROOT_ENTRIES) -f $(FAT_COPIES) \
		-g $(BIOS_HEADS)/$(BIOS_SECTORS_PER_TRACK) \
		-D $(BIOS_DRIVE_NUMBER) -i $(IMAGE_VOLUME_ID) -n DOSC32 \
		--invariant $@ $(IMAGE_KIB)
	dd if=$(BUILD)/boot.bin of=$@ bs=$(BIOS_SECTOR_BYTES) seek=0 \
		conv=notrunc status=none
	$(MCOPY) -o -i $@ $(IO_SYSTEM_FILE) ::/IO.SYS
	$(MCOPY) -o -i $@ $(DOS_KERNEL_FILE) ::/DOSKRNL.SYS
	$(MCOPY) -o -i $@ $(COMMAND_FILE) ::/COMMAND.COM
	$(MCOPY) -o -i $@ $(COUNTRY_FILE) ::/COUNTRY.SYS
	$(MATTRIB) -i $@ +h +s +r ::/IO.SYS
	$(MATTRIB) -i $@ +h +s +r ::/DOSKRNL.SYS
	$(MCOPY) -o -i $@ assets/README.TXT ::/README.TXT
	$(MCOPY) -o -i $@ assets/AUTOEXEC.BAT ::/AUTOEXEC.BAT
	$(MCOPY) -o -i $@ assets/WELCOME.TXT ::/WELCOME.TXT
	./tests/sync-fat-mirrors.sh $@
	@echo "Built BIOS/i386 image: $@"

libc32-test:
	CC="$(HOST_TEST_CC)" ./tests/test-libc32.sh

libc32-extended-test:
	CC="$(HOST_TEST_CC)" ./tests/test-libc32-extended.sh

portable64-check:
	CC="$(HOST_TEST_CC)" ./tests/test-portable64.sh

object-identity-test:
	CC="$(HOST_TEST_CC)" ./tests/test-object-identity.sh

x86-paging-policy-test:
	CC="$(HOST_TEST_CC)" ./tests/test-x86-paging-policy.sh

x86-guest-memory-test:
	CC="$(HOST_TEST_CC)" ./tests/test-x86-guest-memory.sh

x86-ems-memory-test:
	CC="$(HOST_TEST_CC)" ./tests/test-x86-ems-memory.sh

x86-xms-memory-test:
	CC="$(HOST_TEST_CC)" ./tests/test-x86-xms-memory.sh

x86-io-resource-test:
	CC="$(HOST_TEST_CC)" ./tests/test-x86-io-resource.sh

x86-legacy-chipset-test:
	CC="$(HOST_TEST_CC)" ./tests/test-x86-legacy-chipset.sh

x86-vm86-halt-test:
	CC="$(HOST_TEST_CC)" ./tests/test-x86-vm86-halt.sh

x86-i8042-test:
	CC="$(HOST_TEST_CC)" ./tests/test-x86-i8042.sh

x86-native-i8042-test:
	CC="$(HOST_TEST_CC)" ./tests/test-x86-native-i8042.sh

serio-native-input-test:
	CC="$(HOST_TEST_CC)" ./tests/test-serio-native-input.sh

input-core-test:
	CC="$(HOST_TEST_CC)" ./tests/test-input-core.sh

atkbd-test:
	CC="$(HOST_TEST_CC)" ./tests/test-atkbd.sh

keyboard-console-test:
	CC="$(HOST_TEST_CC)" ./tests/test-keyboard-console.sh

guest-ps2-keyboard-test:
	CC="$(HOST_TEST_CC)" ./tests/test-guest-ps2-keyboard.sh

x86-legacy-input-runtime-test:
	CC="$(HOST_TEST_CC)" ./tests/test-x86-legacy-input-runtime.sh

x86-interrupt-router-test:
	CC="$(HOST_TEST_CC)" ./tests/test-x86-interrupt-router.sh

x86-legacy-irq-test:
	CC="$(HOST_TEST_CC)" ./tests/test-x86-legacy-irq.sh

x86-legacy-pic-test:
	CC="$(HOST_TEST_CC)" ./tests/test-x86-legacy-pic.sh

x86-guest-space-test:
	CC="$(HOST_TEST_CC)" ./tests/test-x86-guest-space.sh

x86-vm86-firmware-release-test:
	CC="$(HOST_TEST_CC)" ./tests/test-x86-vm86-firmware-release.sh

x86-legacy-bios-test:
	CC="$(HOST_TEST_CC)" ./tests/test-x86-legacy-bios.sh

x86-display-test:
	CC="$(HOST_TEST_CC)" ./tests/test-x86-display.sh

x86-boot-storage-test:
	CC="$(HOST_TEST_CC)" ./tests/test-x86-boot-storage.sh

block-device-test:
	CC="$(HOST_TEST_CC)" ./tests/test-block-device.sh

ata-device-test:
	CC="$(HOST_TEST_CC)" ./tests/test-ata-device.sh

ata-block-test:
	CC="$(HOST_TEST_CC)" ./tests/test-ata-block.sh

iomgr-test:
	CC="$(HOST_TEST_CC)" ./tests/test-iomgr.sh

iomgr-discovery-test:
	CC="$(HOST_TEST_CC)" ./tests/test-iomgr-discovery.sh

iomgr-device-test: $(IOMGR_DEVICE_TEST_SOURCES) include/iomgr_device.h \
		include/iomgr.h tests/test_entry.h
	@set -eu; \
	temporary_dir=$$(mktemp -d \
		"$${TMPDIR:-/tmp}/dos-c32-iomgr-device.XXXXXX"); \
	cleanup() { \
		rm -f -- "$$temporary_dir"/*.o \
			"$$temporary_dir"/iomgr-device-m32 \
			"$$temporary_dir"/iomgr-device-m64 \
			"$$temporary_dir"/iomgr-device-retirement-m32 \
			"$$temporary_dir"/iomgr-device-retirement-m64; \
		rmdir -- "$$temporary_dir"; \
	}; \
	trap cleanup EXIT HUP INT TERM; \
	common_flags="-std=gnu11 -O2 -ffreestanding -fno-builtin \
		-fno-pie -fno-pic -fno-stack-protector \
		-fno-asynchronous-unwind-tables -fno-unwind-tables \
		-Wall -Wextra -Werror -Wundef -Wshadow -Wstrict-prototypes \
		-Wmissing-prototypes -Wvla -Wformat=2 -Wcast-align=strict \
		-Wnull-dereference"; \
	for model in 32 64; do \
		case "$$model" in \
		32) architecture_flags="-m32 -march=i386 -msoft-float \
			-mno-mmx -mno-sse -mno-sse2" ;; \
		64) architecture_flags="-m64 -march=x86-64" ;; \
		esac; \
		for source in $(IOMGR_DEVICE_TEST_SOURCES); do \
			object="$$temporary_dir/$$(basename \
				"$${source%.c}")-$$model.o"; \
			"$(HOST_TEST_CC)" $$architecture_flags $$common_flags \
				-Iinclude -Itests -c "$$source" -o "$$object"; \
		done; \
		"$(HOST_TEST_CC)" $$architecture_flags -nostdlib -static \
			-no-pie -Wl,-e,_start -Wl,--build-id=none \
			"$$temporary_dir/device-$$model.o" \
			"$$temporary_dir/iomgr_device_test-$$model.o" \
			-o "$$temporary_dir/iomgr-device-m$$model"; \
		for source in $(IOMGR_DEVICE_TEST_SOURCES); do \
			object="$$temporary_dir/retirement-$$(basename \
				"$${source%.c}")-$$model.o"; \
			"$(HOST_TEST_CC)" $$architecture_flags $$common_flags \
				-DIOMGR_DEVICE_TEST_GENERATION_MAX=2u \
				-Iinclude -Itests -c "$$source" -o "$$object"; \
		done; \
		"$(HOST_TEST_CC)" $$architecture_flags -nostdlib -static \
			-no-pie -Wl,-e,_start -Wl,--build-id=none \
			"$$temporary_dir/retirement-device-$$model.o" \
			"$$temporary_dir/retirement-iomgr_device_test-$$model.o" \
			-o "$$temporary_dir/iomgr-device-retirement-m$$model"; \
	done; \
	"$$temporary_dir/iomgr-device-m64"; \
	"$$temporary_dir/iomgr-device-retirement-m64"; \
	m32_result="linked only"; \
	if command -v qemu-i386 >/dev/null 2>&1; then \
		qemu-i386 "$$temporary_dir/iomgr-device-m32"; \
		qemu-i386 "$$temporary_dir/iomgr-device-retirement-m32"; \
		m32_result="executed"; \
	fi; \
	echo "I/O Manager device tests passed: m64 executed, m32 $$m32_result"; \
	echo "Covered capacity, handle domains, retirement and quarantine"

iomgr-transaction-test:
	CC="$(HOST_TEST_CC)" ./tests/test-iomgr-transaction.sh

fat-table-test:
	CC="$(HOST_TEST_CC)" ./tests/test-fat-table.sh

fat-volume-test:
	CC="$(HOST_TEST_CC)" ./tests/test-fat-volume.sh

fat-driver-test:
	CC="$(HOST_TEST_CC)" ./tests/test-fat-driver.sh

shell-capacity-test:
	CC="$(HOST_TEST_CC)" ./tests/test-shell-capacity.sh

external-command-test:
	CC="$(HOST_TEST_CC)" ./tests/test-external-command.sh

command-path-test:
	CC="$(HOST_TEST_CC)" ./tests/test-command-path.sh

command-directory-test:
	CC="$(HOST_TEST_CC)" ./tests/test-command-directory.sh

dos-abi-test:
	CC="$(HOST_TEST_CC)" ./tests/test-dos-abi.sh

dos-error-test:
	CC="$(HOST_TEST_CC)" ./tests/test-dos-error.sh

dos-machine-test:
	CC="$(HOST_TEST_CC)" ./tests/test-dos-machine.sh

dos-xms-test:
	CC="$(HOST_TEST_CC)" ./tests/test-dos-xms.sh

dos-ems-test:
	CC="$(HOST_TEST_CC)" ./tests/test-dos-ems.sh

dos-ems-integration-test:
	CC="$(HOST_TEST_CC)" ./tests/test-dos-ems-integration.sh

dos-interrupt-reflection-test:
	CC="$(HOST_TEST_CC)" ./tests/test-dos-interrupt-reflection.sh

dos-control-instruction-test:
	CC="$(HOST_TEST_CC)" ./tests/test-dos-control-instruction.sh

dos-port-instruction-test:
	CC="$(HOST_TEST_CC)" ./tests/test-dos-port-instruction.sh

dos-memory-test:
	CC="$(HOST_TEST_CC)" ./tests/test-dos-memory.sh

dos-memory-lease-test:
	CC="$(HOST_TEST_CC)" ./tests/test-dos-memory-lease.sh

dos-jft-test:
	CC="$(HOST_TEST_CC)" ./tests/test-dos-jft.sh

dos-vectors-test:
	CC="$(HOST_TEST_CC)" ./tests/test-dos-vectors.sh

dos-drive-test:
	CC="$(HOST_TEST_CC)" ./tests/test-dos-drive.sh

dos-dpb-test:
	CC="$(HOST_TEST_CC)" ./tests/test-dos-dpb.sh

dos-int21-test:
	CC="$(HOST_TEST_CC)" ./tests/test-dos-int21.sh

dos-termination-test:
	CC="$(HOST_TEST_CC)" ./tests/test-dos-termination.sh

dos-country-file-test:
	CC="$(HOST_TEST_CC)" ./tests/test-dos-country-file.sh

dos-process-test:
	CC="$(HOST_TEST_CC)" ./tests/test-dos-process.sh

dos-process-runtime-test:
	CC="$(HOST_TEST_CC)" ./tests/test-dos-process-runtime.sh

dos-runtime-owner-test:
	CC="$(HOST_TEST_CC)" ./tests/test-dos-runtime-owner.sh

dos-exec-observer-test:
	CC="$(HOST_TEST_CC)" ./tests/test-dos-exec-observer.sh

dos-exec-production-adapters-test:
	CC="$(HOST_TEST_CC)" ./tests/test-dos-exec-production-adapters.sh

dos-exec-transaction-test:
	CC="$(HOST_TEST_CC)" ./tests/test-dos-exec-transaction.sh

dos-exec-journal-test:
	CC="$(HOST_TEST_CC)" ./tests/test-dos-exec-journal.sh

dos-exec-handoff-test:
	CC="$(HOST_TEST_CC)" ./tests/test-dos-exec-handoff.sh

dos-exec-backend-session-test:
	CC="$(HOST_TEST_CC)" ./tests/test-dos-exec-backend-session.sh

dos-exec-file-lease-test:
	CC="$(HOST_TEST_CC)" ./tests/test-dos-exec-file-lease.sh

dos-exec-name-test:
	CC="$(HOST_TEST_CC)" ./tests/test-dos-exec-name.sh

dos-exec-parameter-test:
	CC="$(HOST_TEST_CC)" ./tests/test-dos-exec-parameter.sh

dos-exec-overlay-test:
	CC="$(HOST_TEST_CC)" ./tests/test-dos-exec-overlay.sh

dos-exec-seal-test:
	CC="$(HOST_TEST_CC)" ./tests/test-dos-exec-seal.sh

dos-sft-batch-test:
	CC="$(HOST_TEST_CC)" ./tests/test-dos-sft-batch.sh

dos-environment-test:
	CC="$(HOST_TEST_CC)" ./tests/test-dos-environment.sh

dos-environment-view-test:
	CC="$(HOST_TEST_CC)" ./tests/test-dos-environment-view.sh

dos-relocator-test:
	CC="$(HOST_TEST_CC)" ./tests/test-dos-relocator.sh

dos-loader-test:
	CC="$(HOST_TEST_CC)" ./tests/test-dos-loader.sh

dos-image-load-test:
	CC="$(HOST_TEST_CC)" ./tests/test-dos-image-load.sh

probe-dos-program:
	@if [[ -z "$(DOS_PROGRAM)" ]]; then \
		echo "usage: make probe-dos-program DOS_PROGRAM=/path/to/program.exe" >&2; \
		exit 2; \
	fi
	CC="$(HOST_TEST_CC)" ./tests/probe-dos-program.sh "$(DOS_PROGRAM)"

fat16-corruption-test:
	$(MAKE) BUILD=build/fat16-corruption X86_DEBUGCON=1 image
	./tests/fat16-corruption.sh build/fat16-corruption/msdos-c32.img

forbidden-api:
	@if rg -n --glob '*.[ch]' \
		-e '\b(gets|strcpy|strncpy|strcat|strncat|strscpy)\s*\(' \
		-e '\b(sprintf|vsprintf|scanf|sscanf|tmpnam|mktemp|alloca)\s*\(' \
		. ; then \
		echo "forbidden C API found" >&2; \
		exit 1; \
	fi

symbol-audit: $(OBJECTS) $(LIBC32_ARCHIVE)
	@for object in $(OBJECTS); do \
		raw=$$($(NM) -u "$$object" | awk '$$1 == "U" { print $$2 }' | \
			grep -E '^(memcpy|memmove|memset|memcmp)$$' || true); \
		if [[ -n "$$raw" ]]; then \
			echo "raw compiler memory ABI referenced by $$object: $$raw" >&2; \
			exit 1; \
		fi; \
	done
	@unexpected=$$(comm -23 \
		<($(NM) -u $(LIBC32_ARCHIVE) | \
			awk '$$1 == "U" { print $$2 }' | sort -u) \
		<($(NM) -g --defined-only $(LIBC32_ARCHIVE) | \
			awk 'NF >= 3 { print $$3 }' | sort -u) | \
		grep -Ev '^(__stack_chk_fail|__stack_chk_guard)$$' || true); \
	if [[ -n "$$unexpected" ]]; then \
		echo "unexpected libc32-core runtime dependency: $$unexpected" >&2; \
		exit 1; \
	fi
	@# Test translation units may invoke a raw primitive to exercise its ABI.
	@# Production code remains subject to this audit, and every linked
	@# kernel object is independently checked above for unresolved raw calls.
	@if rg -n --glob '*.[ch]' --glob '!tests/**' \
		--glob '!libc32/string.c' \
		'\b(memcpy|memmove|memset|memcmp)\s*\(' . ; then \
		echo "raw memory primitive used outside libc32/string.c" >&2; \
		exit 1; \
	fi

production-audit: $(BUILD)/kernel.elf
	@if [[ "$(BOOT_SELFTESTS)" != "0" ]]; then \
		echo "production-audit requires BOOT_SELFTESTS=0" >&2; \
		exit 2; \
	fi
	@if [[ "$(X86_VM_ACCEPTANCE_DIAGNOSTICS)" != "0" ]]; then \
		echo "production-audit requires X86_VM_ACCEPTANCE_DIAGNOSTICS=0" >&2; \
		exit 2; \
	fi
	@if [[ "$(X86_DEBUGCON)" != "0" ]]; then \
		echo "production-audit requires X86_DEBUGCON=0" >&2; \
		exit 2; \
	fi
	@if [[ "$(X86_VGA_DAC_PALETTE)" != "0" ]]; then \
		echo "production-audit requires X86_VGA_DAC_PALETTE=0" >&2; \
		exit 2; \
	fi
	@if $(NM) $(BUILD)/kernel.elf | grep -E \
		'(x86_.*_self_test|breakpoint_(expected|observed)|SELFTEST_)'; then \
		echo "test-only symbol linked into production kernel" >&2; \
		exit 1; \
	fi
	@if strings $(BUILD)/kernel.elf | grep -E \
		'(^\[(iomgr|vm86-(call|frame|recent))\]|^X86 execution diagnostic:)'; then \
		echo "acceptance diagnostic text linked into production kernel" >&2; \
		exit 1; \
	fi

acceptance-hack-audit:
	@if rg -n --glob '*.[ch]' --glob '!tests/**' \
		-e 'MSD[.]EXE' -e 'fe95172aa455ff52' -e '165932' \
		-e 'PKLITE' . ; then \
		echo "acceptance-program fingerprint found in production code" >&2; \
		exit 1; \
	fi

boot-check:
	$(MAKE) BUILD=build/boot-selftest BOOT_SELFTESTS=1 \
		X86_DEBUGCON=1 image
	./tests/boot-smoke.sh build/boot-selftest/msdos-c32.img

qemu-memory-topology-test: $(IMAGE) tests/qemu-memory-topology.sh \
		tests/run-with-timeout.sh
	./tests/qemu-memory-topology.sh $(IMAGE)

acceptance-diagnostic-image:
	$(MAKE) BUILD=build/acceptance-diagnostic \
		X86_VM_ACCEPTANCE_DIAGNOSTICS=1 image

dos-program-smoke-test: $(IMAGE) $(DOS_PROGRAM_SMOKE_COM)
	MCOPY="$(MCOPY)" ./tests/dos-program-smoke.sh \
		$(IMAGE) $(DOS_PROGRAM_SMOKE_COM)

c32-command-vm86-test: $(IMAGE) $(DOS_PROGRAM_SMOKE_COM)
	MCOPY="$(MCOPY)" ./tests/c32-command-vm86.sh \
		$(IMAGE) $(DOS_PROGRAM_SMOKE_COM)

dos-hardware-probe-test: $(IMAGE) $(DOS_HARDWARE_PROBE_COM)
	MCOPY="$(MCOPY)" ./tests/dos-hardware-probe.sh \
		$(IMAGE) $(DOS_HARDWARE_PROBE_COM)

dos-bios-keyboard-irq-test: $(IMAGE) $(DOS_BIOS_KEYBOARD_IRQ_COM)
	MCOPY="$(MCOPY)" ./tests/dos-bios-keyboard-irq.sh \
		$(IMAGE) $(DOS_BIOS_KEYBOARD_IRQ_COM)

dos-compat-test: $(IMAGE)
	@if [[ -z "$(DOS_COMPAT_ARENA)" ]]; then \
		echo "set DOS_COMPAT_ARENA to a compatible ARENA.EXE path" >&2; \
		exit 2; \
	fi
	MCOPY="$(MCOPY)" ./tests/dos-compat-smoke.sh \
		$(IMAGE) "$(DOS_COMPAT_ARENA)"

windows32-vhd:
	@if [[ -z "$(WINDOWS32_ARCHIVE)" ]]; then \
		echo "set WINDOWS32_ARCHIVE to the local Windows 3.2 .7z path" >&2; \
		exit 2; \
	fi
	BSDTAR="$(or $(BSDTAR),bsdtar)" MCOPY="$(MCOPY)" \
		MMD="$(dir $(MCOPY))mmd" QEMU_IMG="$(or $(QEMU_IMG),qemu-img)" \
		QEMU_SYSTEM_I386="$(or $(QEMU_SYSTEM_I386),qemu-system-i386)" \
		./tests/build-windows32-vhd.sh "$(WINDOWS32_ARCHIVE)" \
		"$(or $(WINDOWS32_VHD),build/windows32/windows32-3.2.vhd)"

check: libc32-test libc32-extended-test portable64-check \
	object-identity-test \
	x86-paging-policy-test x86-guest-memory-test x86-ems-memory-test \
	x86-xms-memory-test \
	x86-io-resource-test x86-legacy-irq-test x86-legacy-pic-test \
	x86-legacy-chipset-test \
	x86-i8042-test x86-native-i8042-test input-core-test \
	serio-native-input-test atkbd-test keyboard-console-test \
	guest-ps2-keyboard-test x86-legacy-input-runtime-test \
	x86-interrupt-router-test \
	x86-guest-space-test x86-vm86-firmware-release-test x86-vm86-halt-test \
	x86-boot-storage-test x86-legacy-bios-test x86-display-test \
	block-device-test ata-device-test ata-block-test iomgr-test \
	iomgr-discovery-test \
	iomgr-device-test \
	iomgr-transaction-test \
	fat-table-test fat-volume-test fat-driver-test \
	shell-capacity-test \
	external-command-test command-path-test command-directory-test \
	dos-abi-test dos-error-test dos-machine-test dos-ems-test \
	dos-ems-integration-test dos-xms-test \
	dos-interrupt-reflection-test dos-control-instruction-test \
	dos-port-instruction-test \
	dos-memory-test dos-memory-lease-test dos-jft-test dos-vectors-test \
	dos-drive-test \
	dos-country-file-test dos-int21-test dos-termination-test \
	dos-process-test \
	dos-process-runtime-test dos-runtime-owner-test dos-exec-observer-test \
	dos-exec-production-adapters-test \
	dos-exec-transaction-test dos-exec-journal-test dos-exec-handoff-test \
	dos-exec-backend-session-test \
	dos-exec-file-lease-test dos-exec-name-test \
	dos-exec-parameter-test dos-exec-overlay-test dos-exec-seal-test \
	dos-sft-batch-test \
	dos-environment-test dos-environment-view-test \
	dos-relocator-test dos-loader-test \
	dos-image-load-test \
	forbidden-api symbol-audit production-audit acceptance-hack-audit \
	$(IMAGE) boot-check \
	dos-program-smoke-test c32-command-vm86-test \
	dos-bios-keyboard-irq-test fat16-corruption-test
	FSCK_FAT="$(FSCK_FAT)" READELF="$(READELF)" NM="$(NM)" \
		MCOPY="$(MCOPY)" \
		./tests/verify-image.sh $(IMAGE) $(DOS_KERNEL_FILE) $(IMAGE_KIB) \
		$(BIOS_SECTOR_BYTES) $(FAT_RESERVED_SECTORS) $(FAT_COPIES) \
		$(FAT_SECTORS_PER_COPY) $(KERNEL_LOAD_BASE) \
		$(KERNEL_RUNTIME_BASE) $(KERNEL_STACK_FLOOR)

run: $(IMAGE)
	@if command -v qemu-system-i386 >/dev/null 2>&1; then \
		exec qemu-system-i386 -machine pc,accel=tcg \
			-m $(QEMU_MEMORY_MIB)M \
			-drive format=raw,file=$(IMAGE) -boot c \
			-display "$(QEMU_DISPLAY)" -serial stdio; \
	else \
		echo "qemu-system-i386 is not installed; use: dosbox -c 'boot $(abspath $(IMAGE))'"; \
		exit 1; \
	fi

run-debug: $(IMAGE)
	qemu-system-i386 -machine pc,accel=tcg -m $(QEMU_MEMORY_MIB)M \
		-drive format=raw,file=$(IMAGE) -boot c \
		-display "$(QEMU_DISPLAY)" \
		-no-reboot -no-shutdown -serial stdio -debugcon file:$(BUILD)/debugcon.log

# Start paused under SeaBIOS.  In another terminal run:
#   gdb -x debug/kernel.gdb
run-gdb: $(IMAGE) $(BUILD)/kernel.elf
	qemu-system-i386 -machine pc,accel=tcg -m $(QEMU_MEMORY_MIB)M \
		-drive format=raw,file=$(IMAGE) -boot c \
		-display "$(QEMU_DISPLAY)" \
		-S -gdb tcp::1234 -no-reboot -no-shutdown -serial stdio \
		-debugcon file:$(BUILD)/debugcon.log \
		-d guest_errors,int -D $(BUILD)/qemu.log

clean:
	rm -rf $(BUILD)

help:
	@echo "make            Build the 32-bit BIOS disk image"
	@echo "make libc32     Build the freestanding libc32-core archive"
	@echo "make check      Validate layout, FAT metadata, and kernel architecture"
	@echo "make boot-check Build an isolated self-test image and boot it in QEMU"
	@echo "make dos-program-smoke-test  Run a real 16-bit COM through EXEC/VM86"
	@echo "make c32-command-vm86-test  Launch VM86 COM from 32-bit COMMAND"
	@echo "make acceptance-diagnostic-image  Build opt-in x86 execution/I/O trace image"
	@echo "make dos-hardware-probe-test Verify BIOS configuration and ROM visibility"
	@echo "make dos-bios-keyboard-irq-test Verify SeaBIOS INT 16h/HLT/IRQ1 input"
	@echo "make windows32-vhd Build a local VHD from all Windows 3.2 disks"
	@echo "make run        Boot with QEMU (when installed)"
	@echo "make run-gdb    Start paused SeaBIOS/QEMU for debug/kernel.gdb"

-include $(DEPFILES)

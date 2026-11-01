# Yousj OS - standalone UEFI bootloader build.
#
# This Makefile is deliberately separate from the top-level kernel
# Makefile (coordinator's territory). All outputs go to $(OUT),
# which defaults OUTSIDE the repo so parallel work in ../build is
# never touched:
#
#   make -C bootloader            # compile-check + EFI binary if the
#                                 # mingw-w64 cross compiler is present
#   make -C bootloader efi        # BOOTX64.EFI (requires mingw-w64)
#   make -C bootloader esp        # + a bootable ESP FAT image
#   make -C bootloader run-uefi   # + boot it in QEMU/OVMF, serial on
#                                 # stdio (needs qemu + OVMF + mtools)
#
# Toolchain notes: the EFI binary is produced with the mingw-w64
# cross compiler because the sandbox's native objcopy has no EFI
# target. The C code itself is toolchain-neutral (self-written UEFI
# types, no gnu-efi); `check` compiles it with the host gcc in
# freestanding mode so the sources are always verifiable, even on a
# machine without the cross compiler.

OUT       ?= /tmp/yousj-bootloader-build
KERNEL_ELF ?= ../build/yousj-kernel.elf

HOSTCC    ?= gcc
MINGWCC   ?= x86_64-w64-mingw32-gcc
BASEFLAGS := -ffreestanding -fno-stack-protector -m64 -mno-red-zone \
             -Wall -Wextra -I ../kernel/include -I .
CHECKFLAGS := $(BASEFLAGS) -fno-pic
EFIFLAGS   := $(BASEFLAGS) -fshort-wchar -std=gnu11

SRCS := efi_main.c boot_info.c

.PHONY: all check efi esp run-uefi clean
all: check
	@if command -v $(MINGWCC) >/dev/null 2>&1; then \
	    $(MAKE) --no-print-directory efi OUT=$(OUT); \
	else \
	    echo "note: $(MINGWCC) not found, EFI binary skipped (check only)"; \
	fi

check:
	mkdir -p $(OUT)
	$(HOSTCC) $(CHECKFLAGS) -c efi_main.c -o $(OUT)/efi_main.o
	$(HOSTCC) $(CHECKFLAGS) -c boot_info.c -o $(OUT)/boot_info.o
	@echo "bootloader check OK: sources compile freestanding"

efi:
	mkdir -p $(OUT)
	$(MINGWCC) $(EFIFLAGS) -nostdlib \
	    -Wl,--subsystem,10 -Wl,--entry,efi_main \
	    -o $(OUT)/BOOTX64.EFI $(SRCS)
	@echo "EFI binary: $(OUT)/BOOTX64.EFI"

# ESP image: BOOTX64.EFI both at the project path and at the UEFI
# fallback path (so OVMF boots it without any NVRAM boot entries),
# kernel at \EFI\YOUSJ\YOUSJ.KRN where the loader looks first.
esp: efi
	@test -f "$(KERNEL_ELF)" || { echo "kernel ELF not found: $(KERNEL_ELF) (build it with the top-level 'make kernel' first, or pass KERNEL_ELF=...)"; exit 1; }
	dd if=/dev/zero of=$(OUT)/esp.img bs=1M count=64 status=none
	mformat -i $(OUT)/esp.img -F ::
	mmd -i $(OUT)/esp.img ::/EFI ::/EFI/YOUSJ ::/EFI/BOOT
	mcopy -i $(OUT)/esp.img $(OUT)/BOOTX64.EFI ::/EFI/YOUSJ/BOOTX64.EFI
	mcopy -i $(OUT)/esp.img $(OUT)/BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI
	mcopy -i $(OUT)/esp.img $(KERNEL_ELF) ::/EFI/YOUSJ/YOUSJ.KRN
	@echo "ESP image: $(OUT)/esp.img"

OVMF_CODE ?= $(firstword $(wildcard /usr/share/OVMF/OVMF_CODE_4M.fd \
                                    /usr/share/OVMF/OVMF_CODE.fd))
OVMF_VARS ?= $(firstword $(wildcard /usr/share/OVMF/OVMF_VARS_4M.fd \
                                    /usr/share/OVMF/OVMF_VARS.fd))

# Expected serial output: the "YOUSJ-UEFI: boot services exited..."
# breadcrumb, then the kernel's own lines (PMM/VMM/sched self-tests)
# ending with the "halting for now" line. kmain halts, so the
# timeout is what ends the run; grep the log to judge PASS/FAIL.
run-uefi: esp
	@test -n "$(OVMF_CODE)" || { echo "OVMF firmware not found (Debian/Ubuntu package: ovmf)"; exit 1; }
	cp $(OVMF_VARS) $(OUT)/vars.fd
	timeout 20 qemu-system-x86_64 -m 128 \
	    -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
	    -drive if=pflash,format=raw,file=$(OUT)/vars.fd \
	    -drive format=raw,file=$(OUT)/esp.img \
	    -display none -serial stdio -monitor none -no-reboot; \
	test $$? -eq 124

clean:
	rm -rf $(OUT)

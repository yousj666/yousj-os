# Yousj OS - top-level build.
# Stage 1: compile-check the kernel core freestanding, and run the
# host-side unit tests (tests/) which compile the same kernel sources
# hosted, so allocator logic is verified without needing to boot.
CC      ?= gcc
CFLAGS  := -ffreestanding -fno-stack-protector -fno-pic -m64 -mno-red-zone \
           -Wall -Wextra -I kernel/include -I kernel
HOSTCFLAGS := -Wall -Wextra -I kernel/include
BUILD   := build

KERNEL_OBJS := $(BUILD)/kmain.o $(BUILD)/pmm.o $(BUILD)/vmm.o $(BUILD)/vmm_arch.o $(BUILD)/serial.o

.PHONY: check test clean
check: $(KERNEL_OBJS)
	@echo "check OK: kernel core compiles freestanding"

test: $(BUILD)/test_pmm $(BUILD)/test_vmm
	./$(BUILD)/test_pmm
	./$(BUILD)/test_vmm

$(BUILD)/kmain.o: kernel/arch/x86_64/kmain.c kernel/include/yousj/*.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/vmm_arch.o: kernel/arch/x86_64/vmm_arch.c kernel/include/yousj/*.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/serial.o: kernel/drivers/serial.c kernel/drivers/serial.h kernel/include/yousj/*.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/pmm.o: kernel/mm/pmm.c kernel/include/yousj/*.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/vmm.o: kernel/mm/vmm.c kernel/include/yousj/*.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/test_pmm: tests/test_pmm.c kernel/mm/pmm.c kernel/include/yousj/*.h
	mkdir -p $(BUILD)
	$(CC) $(HOSTCFLAGS) tests/test_pmm.c kernel/mm/pmm.c -o $@

$(BUILD)/test_vmm: tests/test_vmm.c kernel/mm/vmm.c kernel/mm/pmm.c kernel/include/yousj/*.h
	mkdir -p $(BUILD)
	$(CC) $(HOSTCFLAGS) tests/test_vmm.c kernel/mm/vmm.c kernel/mm/pmm.c -o $@

clean:
	rm -rf $(BUILD)

# --- QEMU Multiboot smoke boot (bring-up only; product chain is UEFI) ---
# QEMU's -kernel loader rejects ELF64, so the boot is two pieces:
# a 32-bit stub (ELF32, linked at 4 MiB) that QEMU loads directly,
# and the ELF64 kernel passed as a Multiboot module (-initrd), which
# the stub's mini ELF loader places at 1 MiB before entering long mode.
KERNEL := $(BUILD)/yousj-kernel.elf
STUB := $(BUILD)/yousj-stub.elf
CFLAGS32 := -m32 -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone \
            -Wall -Wextra -I kernel/include -I kernel
KERNEL_LINK_OBJS := $(BUILD)/entry64.o $(BUILD)/kmain.o \
                    $(BUILD)/serial.o $(BUILD)/pmm.o $(BUILD)/vmm.o $(BUILD)/vmm_arch.o
STUB_LINK_OBJS := $(BUILD)/stub.o $(BUILD)/stub_main.o $(BUILD)/stub_bootinfo.o

.PHONY: kernel run
kernel: $(KERNEL) $(STUB)
	@echo "kernel OK: $(KERNEL) + $(STUB)"

$(KERNEL): $(KERNEL_LINK_OBJS) kernel/arch/x86_64/link.ld
	ld -T kernel/arch/x86_64/link.ld -o $@ $(KERNEL_LINK_OBJS)

$(STUB): $(STUB_LINK_OBJS) kernel/arch/x86_64/stub.ld
	ld -m elf_i386 -T kernel/arch/x86_64/stub.ld -o $@ $(STUB_LINK_OBJS)

$(BUILD)/entry64.o: kernel/arch/x86_64/entry64.S
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/stub.o: kernel/arch/x86_64/stub.S
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS32) -c $< -o $@

$(BUILD)/stub_main.o: kernel/arch/x86_64/stub.c kernel/include/yousj/*.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS32) -c $< -o $@

$(BUILD)/stub_bootinfo.o: kernel/arch/x86_64/boot_multiboot.c kernel/include/yousj/*.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS32) -c $< -o $@

# Boots the stub+kernel in QEMU with the serial console on stdio.
# kmain halts at the end of its self-tests, so timeout ends the run
# after 5 seconds; the serial lines printed before that are the result.
run: $(KERNEL) $(STUB)
	timeout 5 qemu-system-x86_64 -m 128 -kernel $(STUB) -initrd $(KERNEL) \
		-display none -serial stdio -monitor none -no-reboot; \
	test $$? -eq 124

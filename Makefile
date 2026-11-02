# Yousj OS - top-level build.
# Stage 1: compile-check the kernel core freestanding, and run the
# host-side unit tests (tests/) which compile the same kernel sources
# hosted, so allocator logic is verified without needing to boot.
CC      ?= gcc
CFLAGS  := -DYOUSJ_KERNEL -ffreestanding -fno-stack-protector -fno-pic -m64 -mno-red-zone \
           -mcmodel=kernel -mno-sse -mno-mmx \
           -Wall -Wextra -I kernel/include -I kernel
HOSTCFLAGS := -Wall -Wextra -I kernel/include
BUILD   := build

KERNEL_OBJS := $(BUILD)/kmain.o $(BUILD)/pmm.o $(BUILD)/vmm.o $(BUILD)/vmm_arch.o $(BUILD)/syscall.o $(BUILD)/signal.o $(BUILD)/syscall_entry.o $(BUILD)/m3_user_bin.o $(BUILD)/elf.o $(BUILD)/init_elf_bin.o $(BUILD)/hello_elf_bin.o $(BUILD)/sh_elf_bin.o $(BUILD)/sigtest_elf_bin.o $(BUILD)/serial.o $(BUILD)/sched.o $(BUILD)/context_switch.o $(BUILD)/interrupts.o $(BUILD)/isr.o $(BUILD)/gdt.o

.PHONY: check test clean
check: $(KERNEL_OBJS)
	@echo "check OK: kernel core compiles freestanding"

test: $(BUILD)/test_pmm $(BUILD)/test_vmm $(BUILD)/test_sched
	./$(BUILD)/test_pmm
	./$(BUILD)/test_vmm
	./$(BUILD)/test_sched

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

$(BUILD)/sched.o: kernel/sched/sched.c kernel/include/yousj/*.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/context_switch.o: kernel/arch/x86_64/context_switch.S
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/interrupts.o: kernel/arch/x86_64/interrupts.c kernel/include/yousj/*.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/gdt.o: kernel/arch/x86_64/gdt.c kernel/include/yousj/*.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/syscall.o: kernel/syscall/syscall.c kernel/include/yousj/*.h kernel/drivers/serial.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/signal.o: kernel/syscall/signal.c kernel/include/yousj/*.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/syscall_entry.o: kernel/arch/x86_64/syscall_entry.S
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# First ring-3 program: flat binary at YOUSJ_USER_CODE_VA, embedded
# into the kernel image for the M3 smoke.
$(BUILD)/m3_user.bin: userland/m3_user.S
	mkdir -p $(BUILD)
	as --64 -o $(BUILD)/m3_user_asm.o $<
	ld -Ttext=0x8000000000 --oformat=binary -o $@ $(BUILD)/m3_user_asm.o

$(BUILD)/m3_user_bin.o: $(BUILD)/m3_user.bin
	cd $(BUILD) && objcopy -I binary -O elf64-x86-64 m3_user.bin m3_user_bin.o

$(BUILD)/elf.o: kernel/exec/elf.c kernel/include/yousj/*.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# M4 init: a freestanding static ELF, embedded into the kernel
# image as bytes (the kernel-side ELF loader parses it from there;
# SCHED_DESIGN sec. 7.1 transition option (b)).
$(BUILD)/yousj-init.elf: userland/yousj_init.c userland/crt0.S userland/init.ld
	mkdir -p $(BUILD)
	gcc -ffreestanding -fno-stack-protector -fno-pie -static -nostdlib -mno-sse -mno-sse2 -mno-mmx -mcmodel=large -O2 -Wall -Wextra -Wl,-T,userland/init.ld -Wl,--build-id=none -o $@ userland/crt0.S userland/yousj_init.c

$(BUILD)/init_elf_bin.o: $(BUILD)/yousj-init.elf
	cd $(BUILD) && objcopy -I binary -O elf64-x86-64 yousj-init.elf init_elf_bin.o

$(BUILD)/hello.elf: userland/hello.c userland/crt0.S userland/init.ld
	mkdir -p $(BUILD)
	gcc -ffreestanding -fno-stack-protector -fno-pie -static -nostdlib -mno-sse -mno-sse2 -mno-mmx -mcmodel=large -O2 -Wall -Wextra -Wl,-T,userland/init.ld -Wl,--build-id=none -o $@ userland/crt0.S userland/hello.c

$(BUILD)/hello_elf_bin.o: $(BUILD)/hello.elf
	cd $(BUILD) && objcopy -I binary -O elf64-x86-64 hello.elf hello_elf_bin.o

$(BUILD)/sigtest.elf: userland/sigtest.c userland/crt0.S userland/init.ld
	mkdir -p $(BUILD)
	gcc -ffreestanding -fno-stack-protector -fno-pie -static -nostdlib -mno-sse -mno-sse2 -mno-mmx -mcmodel=large -O2 -Wall -Wextra -Wl,-T,userland/init.ld -Wl,--build-id=none -o $@ userland/crt0.S userland/sigtest.c

$(BUILD)/sigtest_elf_bin.o: $(BUILD)/sigtest.elf
	cd $(BUILD) && objcopy -I binary -O elf64-x86-64 sigtest.elf sigtest_elf_bin.o

$(BUILD)/sh.elf: userland/sh.c userland/crt0.S userland/init.ld
	mkdir -p $(BUILD)
	gcc -ffreestanding -fno-stack-protector -fno-pie -static -nostdlib -mno-sse -mno-sse2 -mno-mmx -mcmodel=large -O2 -Wall -Wextra -Wl,-T,userland/init.ld -Wl,--build-id=none -o $@ userland/crt0.S userland/sh.c

$(BUILD)/sh_elf_bin.o: $(BUILD)/sh.elf
	cd $(BUILD) && objcopy -I binary -O elf64-x86-64 sh.elf sh_elf_bin.o

$(BUILD)/isr.o: kernel/arch/x86_64/isr.S
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/test_sched: tests/test_sched.c kernel/sched/sched.c kernel/mm/pmm.c kernel/include/yousj/*.h
	mkdir -p $(BUILD)
	$(CC) $(HOSTCFLAGS) tests/test_sched.c kernel/sched/sched.c kernel/mm/pmm.c -o $@

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
                    $(BUILD)/serial.o $(BUILD)/pmm.o $(BUILD)/vmm.o $(BUILD)/vmm_arch.o $(BUILD)/syscall.o $(BUILD)/signal.o $(BUILD)/syscall_entry.o $(BUILD)/m3_user_bin.o $(BUILD)/elf.o $(BUILD)/init_elf_bin.o $(BUILD)/hello_elf_bin.o $(BUILD)/sh_elf_bin.o $(BUILD)/sigtest_elf_bin.o \
                    $(BUILD)/sched.o $(BUILD)/context_switch.o $(BUILD)/interrupts.o $(BUILD)/isr.o $(BUILD)/gdt.o
STUB_LINK_OBJS := $(BUILD)/stub.o $(BUILD)/stub_main.o $(BUILD)/stub_bootinfo.o

.PHONY: kernel run
kernel: $(KERNEL) $(STUB)
	@echo "kernel OK: $(KERNEL) + $(STUB)"

$(KERNEL): $(KERNEL_LINK_OBJS) kernel/arch/x86_64/link.ld
	ld -T kernel/arch/x86_64/link.ld -o $@ $(KERNEL_LINK_OBJS)
	@nm $@ | python3 -c "import sys; d={p[2]:int(p[0],16) for p in (l.split() for l in sys.stdin) if len(p)==3}; s=d['__kernel_end']-d['__kernel_start']; print(f'kernel image span: {s} bytes'); sys.exit('span exceeds the Multiboot stub 2 MiB early-map window' if s>2097152 else None)"

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

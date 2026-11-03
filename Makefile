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

KERNEL_OBJS := $(BUILD)/kmain.o $(BUILD)/pmm.o $(BUILD)/vmm.o $(BUILD)/vmm_arch.o $(BUILD)/fpu.o $(BUILD)/fs.o $(BUILD)/yfs.o $(BUILD)/ata.o $(BUILD)/syscall.o $(BUILD)/signal.o $(BUILD)/time.o $(BUILD)/syscall_entry.o $(BUILD)/m3_user_bin.o $(BUILD)/elf.o $(BUILD)/initramfs_bin.o $(BUILD)/serial.o $(BUILD)/sched.o $(BUILD)/context_switch.o $(BUILD)/interrupts.o $(BUILD)/isr.o $(BUILD)/gdt.o

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

$(BUILD)/time.o: kernel/time.c kernel/include/yousj/*.h kernel/drivers/serial.h
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

$(BUILD)/fpu.o: kernel/arch/x86_64/fpu.c kernel/include/yousj/*.h
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

$(BUILD)/hello.elf: userland/hello.c userland/crt0.S userland/init.ld
	mkdir -p $(BUILD)
	gcc -ffreestanding -fno-stack-protector -fno-pie -static -nostdlib -mno-sse -mno-sse2 -mno-mmx -mcmodel=large -O2 -Wall -Wextra -Wl,-T,userland/init.ld -Wl,--build-id=none -o $@ userland/crt0.S userland/hello.c

$(BUILD)/sigtest.elf: userland/sigtest.c userland/crt0.S userland/init.ld
	mkdir -p $(BUILD)
	gcc -ffreestanding -fno-stack-protector -fno-pie -static -nostdlib -mno-sse -mno-sse2 -mno-mmx -mcmodel=large -O2 -Wall -Wextra -Wl,-T,userland/init.ld -Wl,--build-id=none -o $@ userland/crt0.S userland/sigtest.c

# fxtest is built WITHOUT -mno-sse on purpose (M8): it is the
# acceptance program for the FPU/SSE support itself.
$(BUILD)/fxtest.elf: userland/fxtest.c userland/crt0.S userland/init.ld
	mkdir -p $(BUILD)
	gcc -ffreestanding -fno-stack-protector -fno-pie -static -nostdlib -mcmodel=large -O2 -Wall -Wextra -Wl,-T,userland/init.ld -Wl,--build-id=none -o $@ userland/crt0.S userland/fxtest.c

$(BUILD)/fs.o: kernel/fs/fs.c kernel/include/yousj/*.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/yfs.o: kernel/fs/yfs.c kernel/include/yousj/*.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/ata.o: kernel/drivers/ata.c kernel/include/yousj/*.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/initramfs.cpio: $(BUILD)/yousj-init.elf $(BUILD)/hello.elf $(BUILD)/sh.elf $(BUILD)/sigtest.elf $(BUILD)/fxtest.elf $(BUILD)/fstest.elf $(BUILD)/cat.elf $(BUILD)/pipetest.elf $(BUILD)/mkdir.elf $(BUILD)/ls.elf $(BUILD)/rm.elf $(BUILD)/rmdir.elf $(BUILD)/mv.elf $(BUILD)/bigfile.elf $(BUILD)/mmtest.elf $(BUILD)/polltest.elf $(BUILD)/ln.elf $(BUILD)/readlink.elf $(BUILD)/epolltest.elf $(BUILD)/churn.elf $(BUILD)/stattest.elf $(BUILD)/brktest.elf $(BUILD)/timetest.elf $(BUILD)/faulttest.elf $(BUILD)/proctest.elf $(BUILD)/utimetest.elf $(BUILD)/orphantest.elf $(BUILD)/pidtest.elf $(BUILD)/dirfdtest.elf $(BUILD)/inotifytest.elf $(BUILD)/futextest.elf $(BUILD)/musltest.elf tools/mkinitramfs.py $(wildcard rootfs/etc/*)
	python3 tools/mkinitramfs.py $(BUILD)

$(BUILD)/initramfs_bin.o: $(BUILD)/initramfs.cpio
	cd $(BUILD) && objcopy -I binary -O elf64-x86-64 initramfs.cpio initramfs_bin.o

$(BUILD)/fstest.elf: userland/fstest.c userland/crt0.S userland/init.ld
	mkdir -p $(BUILD)
	gcc -ffreestanding -fno-stack-protector -fno-pie -static -nostdlib -mno-sse -mno-sse2 -mno-mmx -mcmodel=large -O2 -Wall -Wextra -Wl,-T,userland/init.ld -Wl,--build-id=none -o $@ userland/crt0.S userland/fstest.c

$(BUILD)/cat.elf: userland/cat.c userland/crt0.S userland/init.ld
	mkdir -p $(BUILD)
	gcc -ffreestanding -fno-stack-protector -fno-pie -static -nostdlib -mno-sse -mno-sse2 -mno-mmx -mcmodel=large -O2 -Wall -Wextra -Wl,-T,userland/init.ld -Wl,--build-id=none -o $@ userland/crt0.S userland/cat.c

$(BUILD)/pipetest.elf: userland/pipetest.c userland/crt0.S userland/init.ld
	mkdir -p $(BUILD)
	gcc -ffreestanding -fno-stack-protector -fno-pie -static -nostdlib -mno-sse -mno-sse2 -mno-mmx -mcmodel=large -O2 -Wall -Wextra -Wl,-T,userland/init.ld -Wl,--build-id=none -o $@ userland/crt0.S userland/pipetest.c

$(BUILD)/mkdir.elf: userland/mkdir.c userland/crt0.S userland/init.ld
	mkdir -p $(BUILD)
	gcc -ffreestanding -fno-stack-protector -fno-pie -static -nostdlib -mno-sse -mno-sse2 -mno-mmx -mcmodel=large -O2 -Wall -Wextra -Wl,-T,userland/init.ld -Wl,--build-id=none -o $@ userland/crt0.S userland/mkdir.c

$(BUILD)/ls.elf: userland/ls.c userland/crt0.S userland/init.ld
	mkdir -p $(BUILD)
	gcc -ffreestanding -fno-stack-protector -fno-pie -static -nostdlib -mno-sse -mno-sse2 -mno-mmx -mcmodel=large -O2 -Wall -Wextra -Wl,-T,userland/init.ld -Wl,--build-id=none -o $@ userland/crt0.S userland/ls.c

$(BUILD)/rm.elf: userland/rm.c userland/crt0.S userland/init.ld
	mkdir -p $(BUILD)
	gcc -ffreestanding -fno-stack-protector -fno-pie -static -nostdlib -mno-sse -mno-sse2 -mno-mmx -mcmodel=large -O2 -Wall -Wextra -Wl,-T,userland/init.ld -Wl,--build-id=none -o $@ userland/crt0.S userland/rm.c

$(BUILD)/rmdir.elf: userland/rmdir.c userland/crt0.S userland/init.ld
	mkdir -p $(BUILD)
	gcc -ffreestanding -fno-stack-protector -fno-pie -static -nostdlib -mno-sse -mno-sse2 -mno-mmx -mcmodel=large -O2 -Wall -Wextra -Wl,-T,userland/init.ld -Wl,--build-id=none -o $@ userland/crt0.S userland/rmdir.c

$(BUILD)/mv.elf: userland/mv.c userland/crt0.S userland/init.ld
	mkdir -p $(BUILD)
	gcc -ffreestanding -fno-stack-protector -fno-pie -static -nostdlib -mno-sse -mno-sse2 -mno-mmx -mcmodel=large -O2 -Wall -Wextra -Wl,-T,userland/init.ld -Wl,--build-id=none -o $@ userland/crt0.S userland/mv.c

$(BUILD)/bigfile.elf: userland/bigfile.c userland/crt0.S userland/init.ld
	mkdir -p $(BUILD)
	gcc -ffreestanding -fno-stack-protector -fno-pie -static -nostdlib -mno-sse -mno-sse2 -mno-mmx -mcmodel=large -O2 -Wall -Wextra -Wl,-T,userland/init.ld -Wl,--build-id=none -o $@ userland/crt0.S userland/bigfile.c

$(BUILD)/mmtest.elf: userland/mmtest.c userland/crt0.S userland/init.ld
	mkdir -p $(BUILD)
	gcc -ffreestanding -fno-stack-protector -fno-pie -static -nostdlib -mno-sse -mno-sse2 -mno-mmx -mcmodel=large -O2 -Wall -Wextra -Wl,-T,userland/init.ld -Wl,--build-id=none -o $@ userland/crt0.S userland/mmtest.c

$(BUILD)/polltest.elf: userland/polltest.c userland/crt0.S userland/init.ld
	mkdir -p $(BUILD)
	gcc -ffreestanding -fno-stack-protector -fno-pie -static -nostdlib -mno-sse -mno-sse2 -mno-mmx -mcmodel=large -O2 -Wall -Wextra -Wl,-T,userland/init.ld -Wl,--build-id=none -o $@ userland/crt0.S userland/polltest.c

$(BUILD)/ln.elf: userland/ln.c userland/crt0.S userland/init.ld
	mkdir -p $(BUILD)
	gcc -ffreestanding -fno-stack-protector -fno-pie -static -nostdlib -mno-sse -mno-sse2 -mno-mmx -mcmodel=large -O2 -Wall -Wextra -Wl,-T,userland/init.ld -Wl,--build-id=none -o $@ userland/crt0.S userland/ln.c

$(BUILD)/readlink.elf: userland/readlink.c userland/crt0.S userland/init.ld
	mkdir -p $(BUILD)
	gcc -ffreestanding -fno-stack-protector -fno-pie -static -nostdlib -mno-sse -mno-sse2 -mno-mmx -mcmodel=large -O2 -Wall -Wextra -Wl,-T,userland/init.ld -Wl,--build-id=none -o $@ userland/crt0.S userland/readlink.c

$(BUILD)/epolltest.elf: userland/epolltest.c userland/crt0.S userland/init.ld
	mkdir -p $(BUILD)
	gcc -ffreestanding -fno-stack-protector -fno-pie -static -nostdlib -mno-sse -mno-sse2 -mno-mmx -mcmodel=large -O2 -Wall -Wextra -Wl,-T,userland/init.ld -Wl,--build-id=none -o $@ userland/crt0.S userland/epolltest.c

$(BUILD)/churn.elf: userland/churn.c userland/crt0.S userland/init.ld
	mkdir -p $(BUILD)
	gcc -ffreestanding -fno-stack-protector -fno-pie -static -nostdlib -mno-sse -mno-sse2 -mno-mmx -mcmodel=large -O2 -Wall -Wextra -Wl,-T,userland/init.ld -Wl,--build-id=none -o $@ userland/crt0.S userland/churn.c

$(BUILD)/stattest.elf: userland/stattest.c userland/crt0.S userland/init.ld
	mkdir -p $(BUILD)
	gcc -ffreestanding -fno-stack-protector -fno-pie -static -nostdlib -mno-sse -mno-sse2 -mno-mmx -mcmodel=large -O2 -Wall -Wextra -Wl,-T,userland/init.ld -Wl,--build-id=none -o $@ userland/crt0.S userland/stattest.c

$(BUILD)/brktest.elf: userland/brktest.c userland/crt0.S userland/init.ld
	mkdir -p $(BUILD)
	gcc -ffreestanding -fno-stack-protector -fno-pie -static -nostdlib -mno-sse -mno-sse2 -mno-mmx -mcmodel=large -O2 -Wall -Wextra -Wl,-T,userland/init.ld -Wl,--build-id=none -o $@ userland/crt0.S userland/brktest.c

$(BUILD)/timetest.elf: userland/timetest.c userland/crt0.S userland/init.ld
	mkdir -p $(BUILD)
	gcc -ffreestanding -fno-stack-protector -fno-pie -static -nostdlib -mno-sse -mno-sse2 -mno-mmx -mcmodel=large -O2 -Wall -Wextra -Wl,-T,userland/init.ld -Wl,--build-id=none -o $@ userland/crt0.S userland/timetest.c

$(BUILD)/faulttest.elf: userland/faulttest.c userland/crt0.S userland/init.ld
	mkdir -p $(BUILD)
	gcc -ffreestanding -fno-stack-protector -fno-pie -static -nostdlib -mno-sse -mno-sse2 -mno-mmx -mcmodel=large -O2 -Wall -Wextra -Wl,-T,userland/init.ld -Wl,--build-id=none -o $@ userland/crt0.S userland/faulttest.c

$(BUILD)/proctest.elf: userland/proctest.c userland/crt0.S userland/init.ld
	mkdir -p $(BUILD)
	gcc -ffreestanding -fno-stack-protector -fno-pie -static -nostdlib -mno-sse -mno-sse2 -mno-mmx -mcmodel=large -O2 -Wall -Wextra -Wl,-T,userland/init.ld -Wl,--build-id=none -o $@ userland/crt0.S userland/proctest.c

$(BUILD)/utimetest.elf: userland/utimetest.c userland/crt0.S userland/init.ld
	mkdir -p $(BUILD)
	gcc -ffreestanding -fno-stack-protector -fno-pie -static -nostdlib -mno-sse -mno-sse2 -mno-mmx -mcmodel=large -O2 -Wall -Wextra -Wl,-T,userland/init.ld -Wl,--build-id=none -o $@ userland/crt0.S userland/utimetest.c

$(BUILD)/dirfdtest.elf: userland/dirfdtest.c userland/crt0.S userland/init.ld
	mkdir -p $(BUILD)
	gcc -ffreestanding -fno-stack-protector -fno-pie -static -nostdlib -mno-sse -mno-sse2 -mno-mmx -mcmodel=large -O2 -Wall -Wextra -Wl,-T,userland/init.ld -Wl,--build-id=none -o $@ userland/crt0.S userland/dirfdtest.c

$(BUILD)/pidtest.elf: userland/pidtest.c userland/crt0.S userland/init.ld
	mkdir -p $(BUILD)
	gcc -ffreestanding -fno-stack-protector -fno-pie -static -nostdlib -mno-sse -mno-sse2 -mno-mmx -mcmodel=large -O2 -Wall -Wextra -Wl,-T,userland/init.ld -Wl,--build-id=none -o $@ userland/crt0.S userland/pidtest.c

$(BUILD)/inotifytest.elf: userland/inotifytest.c userland/crt0.S userland/init.ld
	mkdir -p $(BUILD)
	gcc -ffreestanding -fno-stack-protector -fno-pie -static -nostdlib -mno-sse -mno-sse2 -mno-mmx -mcmodel=large -O2 -Wall -Wextra -Wl,-T,userland/init.ld -Wl,--build-id=none -o $@ userland/crt0.S userland/inotifytest.c

$(BUILD)/futextest.elf: userland/futextest.c userland/crt0.S userland/init.ld
	mkdir -p $(BUILD)
	gcc -ffreestanding -fno-stack-protector -fno-pie -static -nostdlib -mno-sse -mno-sse2 -mno-mmx -mcmodel=large -O2 -Wall -Wextra -Wl,-T,userland/init.ld -Wl,--build-id=none -o $@ userland/crt0.S userland/futextest.c

# M56: musltest is a REAL libc binary (musl 1.2.5, static),
# built against the extracted Alpine musl-dev tree in
# build/musl/usr (fetch: musl-dev apk from dl-cdn.alpinelinux.org,
# see README). Not a freestanding program.
MUSL_USR = $(CURDIR)/build/musl/usr
$(BUILD)/musltest.elf: userland/musltest.c $(MUSL_USR)/lib/libc.a
	mkdir -p $(BUILD)
	gcc -static-pie -fpie -O2 -nostdinc -isystem $(MUSL_USR)/include -nostdlib $(MUSL_USR)/lib/rcrt1.o $(MUSL_USR)/lib/crti.o userland/musltest.c -L$(MUSL_USR)/lib -lc $(MUSL_USR)/lib/crtn.o -o $@

$(BUILD)/orphantest.elf: userland/orphantest.c userland/crt0.S userland/init.ld
	mkdir -p $(BUILD)
	gcc -ffreestanding -fno-stack-protector -fno-pie -static -nostdlib -mno-sse -mno-sse2 -mno-mmx -mcmodel=large -O2 -Wall -Wextra -Wl,-T,userland/init.ld -Wl,--build-id=none -o $@ userland/crt0.S userland/orphantest.c

$(BUILD)/sh.elf: userland/sh.c userland/crt0.S userland/init.ld
	mkdir -p $(BUILD)
	gcc -ffreestanding -fno-stack-protector -fno-pie -static -nostdlib -mno-sse -mno-sse2 -mno-mmx -mcmodel=large -O2 -Wall -Wextra -Wl,-T,userland/init.ld -Wl,--build-id=none -o $@ userland/crt0.S userland/sh.c

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
                    $(BUILD)/serial.o $(BUILD)/pmm.o $(BUILD)/vmm.o $(BUILD)/vmm_arch.o $(BUILD)/fpu.o $(BUILD)/fs.o $(BUILD)/yfs.o $(BUILD)/ata.o $(BUILD)/syscall.o $(BUILD)/signal.o $(BUILD)/time.o $(BUILD)/syscall_entry.o $(BUILD)/m3_user_bin.o $(BUILD)/elf.o $(BUILD)/initramfs_bin.o \
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

# Yousj OS - top-level build.
# Stage 1: compile-check the kernel core freestanding, and run the
# host-side unit tests (tests/) which compile the same kernel sources
# hosted, so allocator logic is verified without needing to boot.
CC      ?= gcc
CFLAGS  := -ffreestanding -fno-stack-protector -fno-pic -m64 -mno-red-zone \
           -Wall -Wextra -I kernel/include
HOSTCFLAGS := -Wall -Wextra -I kernel/include
BUILD   := build

KERNEL_OBJS := $(BUILD)/kmain.o $(BUILD)/pmm.o

.PHONY: check test clean
check: $(KERNEL_OBJS)
	@echo "check OK: kernel core compiles freestanding"

test: $(BUILD)/test_pmm
	./$(BUILD)/test_pmm

$(BUILD)/kmain.o: kernel/arch/x86_64/kmain.c kernel/include/yousj/*.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/pmm.o: kernel/mm/pmm.c kernel/include/yousj/*.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/test_pmm: tests/test_pmm.c kernel/mm/pmm.c kernel/include/yousj/*.h
	mkdir -p $(BUILD)
	$(CC) $(HOSTCFLAGS) tests/test_pmm.c kernel/mm/pmm.c -o $@

clean:
	rm -rf $(BUILD)

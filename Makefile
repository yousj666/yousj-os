# Yousj OS - top-level build. Stage 0: compile-check the kernel core.
CC      ?= gcc
CFLAGS  := -ffreestanding -fno-stack-protector -fno-pic -m64 -mno-red-zone \
           -Wall -Wextra -I kernel/include
BUILD   := build

.PHONY: check clean
check: $(BUILD)/kmain.o
	@echo "check OK: kernel core compiles freestanding"

$(BUILD)/kmain.o: kernel/arch/x86_64/kmain.c kernel/include/yousj/*.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD)

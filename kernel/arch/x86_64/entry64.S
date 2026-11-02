/* Yousj OS - 64-bit kernel entry, shared by both boot chains.
 * Written by Muse (coordinator). The kernel image is mapped at
 * its higher-half link base by the chain's early tables before
 * either entry is reached.
 *
 * _kentry64 (Multiboot chain): the 32-bit stub has loaded this
 * ELF's segments to their physical addresses, built struct
 * yousj_boot_info, and stored its pointer at physical 0x9008.
 *
 * _kentry64_rdi (UEFI chain): the bootloader jumps here with the
 * boot_info pointer already in RDI (see bootloader/efi_main.c).
 *
 * Both then do the same: reload data segments with our own
 * selectors, switch to the kernel stack in this image's BSS, and
 * call kmain(). */

    .section .bss
    .align 16
kernel_stack_bottom:
    .skip 16384
kernel_stack_top:

    .section .text
    .code64
    .global _kentry64
_kentry64:
    mov 0x9008, %rdi          /* boot_info pointer left by the stub */

    .global _kentry64_rdi
_kentry64_rdi:
    /* Data segments: selector 0x10 is a flat data descriptor in
     * both chains' hand-off GDTs (and in ours, once gdt_init()
     * runs inside kmain). */
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss
    mov $kernel_stack_top, %rsp
    call kmain

hang:
    cli
    hlt
    jmp hang

.section .note.GNU-stack, "", @progbits

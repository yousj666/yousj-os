/* Yousj OS - 64-bit kernel entry for the QEMU Multiboot smoke boot.
 * Written by Muse (coordinator). The 32-bit stub (stub.S/stub.c)
 * has already loaded this ELF's segments to their physical addresses,
 * built struct yousj_boot_info, switched to long mode with an
 * identity map, and stored the boot_info pointer at physical 0x9008.
 * All we do here is set up the kernel stack and call kmain(). */

    .section .bss
    .align 16
kernel_stack_bottom:
    .skip 16384
kernel_stack_top:

    .section .text
    .code64
    .global _kentry64
_kentry64:
    /* Data segments: selector 0x10 in the stub's GDT (still the
     * active GDT at this point) is its flat data descriptor. */
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss
    mov $kernel_stack_top, %rsp
    mov 0x9008, %rdi          /* boot_info pointer left by the stub */
    call kmain

hang:
    cli
    hlt
    jmp hang

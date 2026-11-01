/* Yousj OS - 32-bit Multiboot stub for the QEMU smoke boot.
 * Written by Muse (coordinator). Bring-up tool only; the product
 * bootloader is the UEFI application in bootloader/DESIGN.md.
 *
 * Why this exists: QEMU's -kernel Multiboot loader only accepts
 * 32-bit ELF images ("Cannot load x86-64 image"), while the kernel
 * is ELF64. So QEMU loads THIS stub via -kernel and the kernel ELF
 * as a Multiboot module via -initrd. The stub:
 *   1. calls stub_main() (stub.c), which loads the kernel's ELF64
 *      segments to 1 MiB, builds struct yousj_boot_info from the
 *      Multiboot memory map, and returns the kernel entry address;
 *   2. builds a 1 GiB identity map (2 MiB pages), enters long mode;
 *   3. stores the boot_info pointer at physical 0x9008 and far-jumps
 *      straight to the kernel's 64-bit entry (entry64.S).
 *
 * This file is pure 32-bit code on purpose: it must assemble into an
 * ELF32 object, which cannot hold 64-bit relocations, so the final
 * jump goes through a memory far-pointer whose offset field is
 * patched at runtime with the entry address stub_main() returned.
 * The stub is linked at 4 MiB, inside the range that
 * mb1_build_boot_info() marks reserved, so the PMM never hands the
 * stub's memory (including the boot_info buffer) to anyone.
 */

    .section .multiboot, "a"
    .align 4
    .long 0x1BADB002
    .long 0x00000003
    .long -(0x1BADB002 + 0x00000003)

    .section .bss
    .align 16
stub_stack_bottom:
    .skip 16384
stub_stack_top:

    .align 4096
stub_pml4:
    .skip 4096
stub_pdpt:
    .skip 4096
stub_pd:
    .skip 4096

    .align 8
mb_info_saved:
    .skip 4
entry_saved:
    .skip 4

    .section .data
    .align 8
stub_gdt:
    .quad 0
    .quad 0x00AF9A000000FFFF     /* code64 */
    .quad 0x00CF92000000FFFF     /* data */
stub_gdt_end:
stub_gdt_ptr:
    .word stub_gdt_end - stub_gdt - 1
    .long stub_gdt

far_ptr:                          /* patched at runtime, see below */
far_offset:
    .long 0
far_selector:
    .word 0x08

    .section .text
    .code32
    .global _stub_start
_stub_start:
    cli
    mov $stub_stack_top, %esp
    mov %ebx, mb_info_saved

    push %ebx
    call stub_main
    add $4, %esp
    test %eax, %eax
    jz stub_hang
    mov %eax, entry_saved

    /* Publish the boot_info pointer for entry64.S at 0x9008. */
    mov g_boot_info_phys, %eax
    mov %eax, 0x9008
    movl $0, 0x900C

    /* Identity map: 512 x 2 MiB pages via stub_pd. */
    mov $stub_pd, %edi
    mov $0x83, %eax
    mov $512, %ecx
1:
    mov %eax, (%edi)
    movl $0, 4(%edi)
    add $0x200000, %eax
    add $8, %edi
    loop 1b

    mov $stub_pd, %eax
    or $0x3, %eax
    mov %eax, stub_pdpt
    movl $0, stub_pdpt + 4
    mov $stub_pdpt, %eax
    or $0x3, %eax
    mov %eax, stub_pml4
    movl $0, stub_pml4 + 4

    lgdt stub_gdt_ptr

    mov %cr4, %eax
    or $0x20, %eax               /* CR4.PAE */
    mov %eax, %cr4

    mov $stub_pml4, %eax
    mov %eax, %cr3

    mov $0xC0000080, %ecx        /* EFER MSR */
    rdmsr
    or $0x100, %eax              /* EFER.LME */
    wrmsr

    mov %cr0, %eax
    or $0x80000000, %eax         /* CR0.PG: long mode active */
    mov %eax, %cr0

    /* Far-jump through the patched pointer to the kernel's 64-bit
     * entry. Loading CS with the code64 descriptor completes the
     * transition; entry64.S reloads the data segments itself. */
    mov entry_saved, %eax
    mov %eax, far_offset
    ljmp *far_ptr

stub_hang:
    cli
    hlt
    jmp stub_hang

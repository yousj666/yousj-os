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
 *      Multiboot memory map, and publishes the kernel's 64-bit
 *      virtual entry address in g_kernel_entry;
 *   2. builds the early page tables -- a 1 GiB identity map (2 MiB
 *      pages) PLUS the kernel image mapped at its higher-half
 *      link base 0xFFFFFFFF80000000 (4 KiB pages from physical
 *      1 MiB: the image is only 1 MiB-aligned, so a 2 MiB page
 *      cannot represent it; four PTs cover an 8 MiB window,
 *      the maximum image span) -- and enters long mode;
 *   3. stores the boot_info pointer at physical 0x9008 and
 *      far-jumps to a 64-bit trampoline (below) that jumps to the
 *      full 64-bit entry address.
 *
 * This file is mostly 32-bit code on purpose: it must assemble
 * into an ELF32 object, which cannot hold 64-bit relocations.
 * The far jump therefore targets the trampoline (a 32-bit stub
 * address) through a memory far-pointer, and the trampoline --
 * already running in 64-bit mode -- loads the entry address
 * stub_main() published and jumps to it. The stub is linked at
 * 12 MiB, inside the range mb1_build_boot_info() marks
 * reserved, so the PMM never hands the stub's memory (including
 * the boot_info buffer) to anyone.
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
stub_pdpt_hi:
    .skip 4096
stub_pd_hi:
    .skip 4096
stub_pt_hi:
    .skip 4096
stub_pt_hi2:
    .skip 4096
stub_pt_hi3:
    .skip 4096
stub_pt_hi4:
    .skip 4096

    .align 8
mb_info_saved:
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

    /* Higher-half image map: PDPT index 510 of PML4 entry 511
     * covers [0xFFFFFFFF80000000, +1 GiB); PD entries 0..3
     * point at PTs whose 512 entries each map 2 MiB of the
     * image from physical 1 MiB at 4 KiB granularity (1 MiB
     * is not 2 MiB aligned, so huge pages cannot be used
     * here). M58: the window grew 2 MiB -> 8 MiB when the
     * initramfs (musl loader aboard) pushed the image span
     * past 2 MiB. */
    mov $stub_pt_hi, %edi
    mov $0x100003, %eax
    mov $512, %ecx
1:
    mov %eax, (%edi)
    movl $0, 4(%edi)
    add $0x1000, %eax
    add $8, %edi
    loop 1b

    mov $stub_pt_hi2, %edi
    mov $0x300003, %eax
    mov $512, %ecx
1:
    mov %eax, (%edi)
    movl $0, 4(%edi)
    add $0x1000, %eax
    add $8, %edi
    loop 1b

    mov $stub_pt_hi3, %edi
    mov $0x500003, %eax
    mov $512, %ecx
1:
    mov %eax, (%edi)
    movl $0, 4(%edi)
    add $0x1000, %eax
    add $8, %edi
    loop 1b

    mov $stub_pt_hi4, %edi
    mov $0x700003, %eax
    mov $512, %ecx
1:
    mov %eax, (%edi)
    movl $0, 4(%edi)
    add $0x1000, %eax
    add $8, %edi
    loop 1b

    mov $stub_pt_hi, %eax
    or $0x3, %eax
    mov %eax, stub_pd_hi
    movl $0, stub_pd_hi + 4
    mov $stub_pt_hi2, %eax
    or $0x3, %eax
    mov %eax, stub_pd_hi + 8
    movl $0, stub_pd_hi + 12
    mov $stub_pt_hi3, %eax
    or $0x3, %eax
    mov %eax, stub_pd_hi + 16
    movl $0, stub_pd_hi + 20
    mov $stub_pt_hi4, %eax
    or $0x3, %eax
    mov %eax, stub_pd_hi + 24
    movl $0, stub_pd_hi + 28

    mov $stub_pd_hi, %eax
    or $0x3, %eax
    mov %eax, stub_pdpt_hi + 4080   /* PDPT index 510 x 8 */
    movl $0, stub_pdpt_hi + 4084
    mov $stub_pdpt_hi, %eax
    or $0x3, %eax
    mov %eax, stub_pml4 + 4088      /* PML4 index 511 x 8 */
    movl $0, stub_pml4 + 4092

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

    /* Far-jump through the patched pointer to the 64-bit
     * trampoline below (a 32-bit stub address -- an ELF32 object
     * cannot name the kernel's 64-bit entry in a far pointer).
     * Loading CS with the code64 descriptor completes the
     * transition to 64-bit mode; the trampoline finishes the
     * trip to the kernel's higher-half entry. */
    mov $tramp64, %eax
    mov %eax, far_offset
    ljmp *far_ptr

stub_hang:
    cli
    hlt
    jmp stub_hang

    /* 64-bit trampoline: runs at the stub's identity address in
     * long mode. Loads the full 64-bit kernel entry published by
     * stub_main() in g_kernel_entry (two 32-bit halves in .data of
     * stub.c's translation unit -- a plain 8-byte load through a
     * 32-bit address needs no 64-bit relocation) and jumps. */
    .code64
tramp64:
    mov $g_kernel_entry, %esi
    mov (%rsi), %rax
    jmp *%rax
    .code32

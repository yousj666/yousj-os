/* Yousj OS - kernel-owned GDT (stage 1 prerequisite for M3).
 *
 * Written from scratch by Muse (coordinator). Until now the kernel
 * borrowed whatever GDT the boot environment left behind -- the
 * Multiboot stub's under QEMU -kernel, the firmware's under UEFI.
 * That silently breaks the moment an IDT gate fires under UEFI:
 * OVMF's GDT does not have a 64-bit code segment at selector 0x08,
 * so the first timer IRQ escalates to a triple fault instead of
 * reaching our handler (diagnosed 2026-11-01: UEFI boot ran clean
 * through M1, then died at the first tick).
 *
 * gdt_init() installs our own table and reloads all segment
 * registers, making the layout below the ONLY layout the kernel
 * ever sees, on every boot path:
 *
 *   0x00  null
 *   0x08  kernel code, 64-bit, ring 0   (IDT gates point here)
 *   0x10  kernel data, ring 0
 *
 * User segments (0x18/0x20 + TSS at 0x28) arrive with M3's Ring 3
 * work, per docs/SYSCALL_DESIGN.md sec. 3.2, extending this table
 * in the reserved slots without renumbering the kernel pair.
 */
#ifndef YOUSJ_GDT_H
#define YOUSJ_GDT_H

#define YOUSJ_GDT_KERNEL_CODE 0x08
#define YOUSJ_GDT_KERNEL_DATA 0x10

void gdt_init(void);

#endif /* YOUSJ_GDT_H */

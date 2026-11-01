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
 *   0x18  user data, ring 3             (M3, SYSCALL_DESIGN 3.2)
 *   0x20  user code, 64-bit, ring 3
 *   0x28  TSS (two slots)
 */
#ifndef YOUSJ_GDT_H
#define YOUSJ_GDT_H

#define YOUSJ_GDT_KERNEL_CODE 0x08
#define YOUSJ_GDT_KERNEL_DATA 0x10
#define YOUSJ_GDT_USER_DATA   0x18
#define YOUSJ_GDT_USER_CODE   0x20
#define YOUSJ_GDT_TSS         0x28

void gdt_init(void);

/* Point the TSS's ring-0 stack at the given kernel stack top.
 * Called on every task switch (via the syscall layer's
 * arch_on_task_switch hook) so an interrupt out of ring 3 always
 * lands on the CURRENT task's kernel stack. */
void tss_set_rsp0(uint64_t rsp0);

#endif /* YOUSJ_GDT_H */

/* Yousj OS - kernel GDT + TSS installation.
 * Written by Muse (coordinator). Final layout per
 * docs/SYSCALL_DESIGN.md sec. 3.2 (ratified on adoption):
 *
 *   0x00 null
 *   0x08 kernel code64 (ring 0)   -- IDT gates and STAR point here
 *   0x10 kernel data   (ring 0)
 *   0x18 user data     (ring 3)   -- sysret SS = 0x10 + 8
 *   0x20 user code64   (ring 3)   -- sysret CS = 0x10 + 16
 *   0x28 TSS (16 bytes, two slots)
 *
 * Descriptor encodings are the architectural long-mode formats:
 * flat, base 0; the user pair differs from the kernel pair only in
 * the DPL bits. The TSS gives ring transitions (interrupts out of
 * ring 3 in particular) their kernel stack via RSP0; sched.c keeps
 * RSP0 pointing at the current task's kernel stack through the
 * arch_on_task_switch hook in the syscall layer.
 */
#include <yousj/types.h>
#include <yousj/gdt.h>

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

struct tss {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

static struct tss g_tss;

static uint64_t g_gdt[8] = {
    0,                      /* 0x00 null */
    0x00AF9A000000FFFFULL,  /* 0x08 kernel code64 */
    0x00CF92000000FFFFULL,  /* 0x10 kernel data */
    0x00CFF2000000FFFFULL,  /* 0x18 user data (DPL 3) */
    0x00AFFA000000FFFFULL,  /* 0x20 user code64 (DPL 3) */
    0,                      /* 0x28 TSS low (filled in gdt_init) */
    0,                      /*      TSS high */
    0
};

void tss_set_rsp0(uint64_t rsp0)
{
    g_tss.rsp0 = rsp0;
}

void gdt_init(void)
{
    struct gdt_ptr ptr;
    uint64_t base = (uint64_t)(uintptr_t)&g_tss;
    uint64_t limit = sizeof(g_tss) - 1;
    uint32_t i;

    /* TSS: all zeroes except the I/O-map base (== size: no map). */
    uint8_t *t = (uint8_t *)&g_tss;
    for (i = 0; i < sizeof(g_tss); i++)
        t[i] = 0;
    g_tss.iomap_base = (uint16_t)sizeof(g_tss);

    /* 64-bit TSS descriptor: access byte 0x89 = present, type 9
     * (available TSS); granularity bit clear (byte-granular limit). */
    g_gdt[5] = (limit & 0xFFFFULL)
             | ((base & 0xFFFFULL) << 16)
             | (((base >> 16) & 0xFFULL) << 32)
             | (0x89ULL << 40)
             | (((base >> 24) & 0xFFULL) << 56);
    g_gdt[6] = base >> 32;

    ptr.limit = (uint16_t)(sizeof(g_gdt) - 1);
    ptr.base = (uint64_t)(uintptr_t)&g_gdt;

    __asm__ volatile(
        "lgdt %0\n\t"
        "pushq %1\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        "mov %2, %%ds\n\t"
        "mov %2, %%es\n\t"
        "mov %2, %%fs\n\t"
        "mov %2, %%gs\n\t"
        "mov %2, %%ss\n\t"
        :
        : "m"(ptr), "i"(YOUSJ_GDT_KERNEL_CODE), "r"((uint16_t)YOUSJ_GDT_KERNEL_DATA)
        : "rax", "memory");

    __asm__ volatile("ltr %0" : : "r"((uint16_t)YOUSJ_GDT_TSS));
}

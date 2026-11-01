/* Yousj OS - kernel GDT installation. Written by Muse (coordinator).
 * The descriptor encodings below are the architectural formats for
 * 64-bit code and data segments (same values the boot stub used for
 * its own table -- flat, base 0, no limit checks in long mode).
 */
#include <yousj/types.h>
#include <yousj/gdt.h>

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/* Slots 0x18..0x38 are reserved (zero) for the M3 user segments and
 * the TSS descriptor (which takes two slots); see gdt.h. */
static uint64_t g_gdt[8] = {
    0,                      /* 0x00 null */
    0x00AF9A000000FFFFULL,  /* 0x08 kernel code64: L=1, P, DPL0, exec/read */
    0x00CF92000000FFFFULL,  /* 0x10 kernel data: P, DPL0, read/write */
    0, 0, 0, 0, 0
};

void gdt_init(void)
{
    struct gdt_ptr ptr;
    ptr.limit = (uint16_t)(sizeof(g_gdt) - 1);
    ptr.base = (uint64_t)(uintptr_t)&g_gdt;

    __asm__ volatile(
        "lgdt %0\n\t"
        /* Reload CS by far-returning to the next instruction. */
        "pushq %1\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        /* Reload every data segment with the kernel data selector. */
        "mov %2, %%ds\n\t"
        "mov %2, %%es\n\t"
        "mov %2, %%fs\n\t"
        "mov %2, %%gs\n\t"
        "mov %2, %%ss\n\t"
        :
        : "m"(ptr), "i"(YOUSJ_GDT_KERNEL_CODE), "r"((uint16_t)YOUSJ_GDT_KERNEL_DATA)
        : "rax", "memory");
}

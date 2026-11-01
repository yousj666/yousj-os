/* Yousj OS - x86-64 VMM hardware primitives. Written by Muse.
 *
 * These are the ONLY VMM functions that touch real hardware state.
 * Nothing calls them yet: loading a new CR3 waits until the
 * bootloader paging contract is pinned down (docs/VMM_DESIGN.md
 * sec. 2.3 / 6.2). They exist now so the switch step, when scheduled,
 * is a call site change rather than new code written under pressure.
 */
#include <yousj/vmm.h>

void vmm_load_cr3(uint64_t pml4_phys)
{
    __asm__ volatile("mov %0, %%cr3" : : "r"(pml4_phys) : "memory");
}

uint64_t vmm_read_cr3(void)
{
    uint64_t value;
    __asm__ volatile("mov %%cr3, %0" : "=r"(value));
    return value;
}

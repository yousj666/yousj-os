/* Yousj OS - x86-64 VMM hardware primitives and bring-up.
 * Written by Muse (coordinator).
 *
 * vmm_load_cr3()/vmm_read_cr3() are the only VMM functions that
 * touch real hardware state. vmm_hw_init() performs the M6 layout
 * switch: it takes over paging from the boot chain's early tables
 * (identity + image-at-base) into a kernel-owned master space in
 * the final layout of VMM_DESIGN.md sec. 3/6.2 -- HHDM direct map
 * of all RAM, kernel image at its higher-half link base with
 * per-section permissions, no identity map at all. Virtual
 * addresses do not change across the switch (the early tables
 * already map the image at its base), only the tables do.
 */
#include <yousj/vmm.h>
#include <yousj/pmm.h>

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

/* Architectural constants: EFER MSR address and its NXE bit;
 * CR0's WP bit. Enabling NXE matters before any PTE with the NX
 * bit is ever loaded -- with NXE off, bit 63 is reserved and the
 * first such mapping would fault. WP makes read-only kernel pages
 * actually read-only for ring 0 as well. */
#define MSR_EFER 0xC0000080u
#define EFER_NXE (1ULL << 11)
#define CR0_WP   (1ULL << 16)

static void cpu_enable_nxe_wp(void)
{
    uint32_t lo, hi;
    uint64_t cr0;

    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(MSR_EFER));
    lo |= (uint32_t)EFER_NXE;
    __asm__ volatile("wrmsr" : : "c"(MSR_EFER), "a"(lo), "d"(hi));

    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= CR0_WP;
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
}

static struct vmm_space g_master_space;

/* Linker-script image bounds (kernel/arch/x86_64/link.ld). The
 * physical counterpart of a virtual address in the image is
 * va - KERNEL_V2P_DELTA (image loaded at 1 MiB). */
extern const uint8_t __kernel_start[];
extern const uint8_t __kernel_end[];
extern const uint8_t __text_start[];
extern const uint8_t __text_end[];
extern const uint8_t __rodata_start[];
extern const uint8_t __rodata_end[];
#define KERNEL_V2P_DELTA 0xFFFFFFFF7FF00000ULL /* KERNEL_BASE - 1 MiB */
#define KERNEL_IMAGE_PHYS 0x100000ULL

static uint64_t page_up(uint64_t v)
{
    return (v + YOUSJ_PAGE_SIZE - 1) & ~(YOUSJ_PAGE_SIZE - 1);
}

int vmm_hw_init(uint64_t ram_top_phys)
{
    uint64_t kstart = (uint64_t)(uintptr_t)__kernel_start;
    uint64_t kend = page_up((uint64_t)(uintptr_t)__kernel_end);
    int rc;

    cpu_enable_nxe_wp();

    rc = vmm_create_space(&g_master_space);
    if (rc != 0)
        return rc;
    vmm_set_kernel_master(&g_master_space);

    /* HHDM: every byte of RAM reachable at HHDM_BASE + phys,
     * supervisor RW, NX (vmm_pte_encode sets NX without EXEC).
     * Mapped in 128 MiB chunks: one vmm_map() call's intermediate
     * tables (<= 66 here: PDPT + PD + 64 PTs) must stay inside
     * the mapper's rollback log, however large RAM gets. */
    {
        uint64_t phys;
        for (phys = 0; phys < ram_top_phys; phys += 0x8000000ULL) {
            uint64_t chunk = ram_top_phys - phys;
            if (chunk > 0x8000000ULL)
                chunk = 0x8000000ULL;
            rc = vmm_map(&g_master_space, YOUSJ_HHDM_BASE + phys,
                         phys, chunk,
                         YOUSJ_VMM_READ | YOUSJ_VMM_WRITE);
            if (rc != 0)
                return rc;
        }
    }

    /* Kernel image at its linked base: RW over the whole span
     * first, then tighten -- text loses W and gains X, rodata
     * loses W; data/BSS keep the RW they already have. The
     * ranges are page-rounded outward; sections are page-aligned
     * and page-separated by the linker script, so rounding never
     * bleeds one section's permissions into another. */
    rc = vmm_map(&g_master_space, kstart, KERNEL_IMAGE_PHYS,
                 kend - kstart,
                 YOUSJ_VMM_READ | YOUSJ_VMM_WRITE);
    if (rc != 0)
        return rc;
    rc = vmm_protect(&g_master_space,
                     (uint64_t)(uintptr_t)__text_start,
                     page_up((uint64_t)(uintptr_t)__text_end) -
                         (uint64_t)(uintptr_t)__text_start,
                     YOUSJ_VMM_READ | YOUSJ_VMM_EXEC);
    if (rc != 0)
        return rc;
    rc = vmm_protect(&g_master_space,
                     (uint64_t)(uintptr_t)__rodata_start,
                     page_up((uint64_t)(uintptr_t)__rodata_end) -
                         (uint64_t)(uintptr_t)__rodata_start,
                     YOUSJ_VMM_READ);
    if (rc != 0)
        return rc;

    /* The switch itself. The return path out of this function
     * executes at higher-half addresses that the new tables map
     * identically, and every live stack lives in the image's BSS
     * (both boot chains enter through entry64.S), so dropping
     * the early identity map is safe. From the next instruction
     * on, page tables are reached through the HHDM. */
    vmm_load_cr3(g_master_space.pml4_phys);
    vmm_use_hhdm_table_access();
    return 0;
}

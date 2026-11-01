/* Yousj OS - x86-64 VMM hardware primitives and bring-up.
 * Written by Muse (coordinator).
 *
 * vmm_load_cr3()/vmm_read_cr3() are the only VMM functions that
 * touch real hardware state. vmm_hw_init() performs hardware step 1
 * of the VMM plan: it takes over paging from the boot stub's tables
 * into a kernel-owned master space, WITHOUT changing any virtual
 * addresses yet (identity map), so the switch is provable in
 * isolation. The higher-half + HHDM layout from VMM_DESIGN.md
 * sec. 6.2 arrives with the UEFI bootloader chain.
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

/* The kernel image lives at 1 MiB and is far below 2 MiB total in
 * this stage; until the linker script exports image bound symbols,
 * the executable window is this fixed range (data pages inside it
 * being executable for now is a documented bring-up compromise --
 * the rest of RAM is mapped without EXEC... see vmm_hw_init: the
 * base identity pass maps RW only, and this window is then raised
 * to RWX via vmm_protect). */
#define KERNEL_IMAGE_START 0x100000ULL
#define KERNEL_IMAGE_END   0x300000ULL

int vmm_hw_init(uint64_t ram_top_phys)
{
    uint64_t phys;
    int rc;

    cpu_enable_nxe_wp();

    rc = vmm_create_space(&g_master_space);
    if (rc != 0)
        return rc;
    vmm_set_kernel_master(&g_master_space);

    for (phys = 0; phys < ram_top_phys; phys += YOUSJ_PAGE_SIZE) {
        rc = vmm_map(&g_master_space, phys, phys, YOUSJ_PAGE_SIZE,
                     YOUSJ_VMM_READ | YOUSJ_VMM_WRITE);
        if (rc != 0)
            return rc;
    }
    rc = vmm_protect(&g_master_space, KERNEL_IMAGE_START,
                     KERNEL_IMAGE_END - KERNEL_IMAGE_START,
                     YOUSJ_VMM_READ | YOUSJ_VMM_WRITE | YOUSJ_VMM_EXEC);
    if (rc != 0)
        return rc;

    vmm_load_cr3(g_master_space.pml4_phys);
    return 0;
}

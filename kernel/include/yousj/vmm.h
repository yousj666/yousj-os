/* Yousj OS - virtual memory manager (x86-64 four-level paging).
 *
 * Written from scratch by Muse (kernel core). Interface approved by
 * the coordinator on 2026-11-01, implementing steps 1-2 of the landing
 * order in docs/VMM_DESIGN.md (design by auxiliary side chat C):
 * pure logic + page-table walking first, host-unit-tested; the CR3
 * switch and HHDM bring-up (vmm_init in the design) come later, once
 * the bootloader's paging hand-off is pinned down.
 *
 * Adopted from the design, as coordinator decisions:
 *   - Higher-half kernel layout: user space is the canonical low half
 *     (PML4 0-255), kernel space the high half (256-511), with the
 *     direct map (HHDM) at 0xFFFF800000000000 and the kernel image at
 *     0xFFFFFFFF80000000. Since M6 the kernel is linked and runs in
 *     the higher half; there is no identity map anywhere (the boot
 *     chains' early tables are the only place one ever existed, and
 *     they are discarded at vmm_hw_init()).
 *   - Abstract flags (READ/WRITE/EXEC/USER) at the API; hardware PTE
 *     bits stay inside vmm.c except for the pure encode/decode helpers
 *     below, which exist so the translation can be unit-tested.
 *   - Error style: 0 on success, negative errno-compatible values on
 *     failure (-EINVAL, -ENOMEM, -EEXIST, -ENOENT below).
 *   - vmm_map() is all-or-nothing: on any failure the address space is
 *     left exactly as it was (pre-validated existence, creation log
 *     with rollback for intermediate page tables).
 *   - vmm_unmap() frees EMPTY intermediate page-table pages back to
 *     the allocator, but never frees the data pages themselves: data
 *     page ownership belongs to the caller, page tables to the VMM.
 *
 * Walking never dereferences a physical address directly. Every table
 * access goes through a registered backend (alloc/free a table page,
 * physical->pointer), so the identical code runs in the kernel (PMM +
 * the currently-valid mapping) and in host tests (fake RAM array).
 */
#ifndef YOUSJ_VMM_H
#define YOUSJ_VMM_H

#include <yousj/types.h>
#include <yousj/pmm.h> /* YOUSJ_PAGE_SIZE */

/* errno-compatible magnitudes (returned negated). */
#define YOUSJ_ENOENT 2
#define YOUSJ_ENOMEM 12
#define YOUSJ_EEXIST 17
#define YOUSJ_EINVAL 22

/* Abstract mapping flags for vmm_map()/vmm_protect()/vmm_translate(). */
#define YOUSJ_VMM_READ  0x1u
#define YOUSJ_VMM_WRITE 0x2u
#define YOUSJ_VMM_EXEC  0x4u
#define YOUSJ_VMM_USER  0x8u

/* Address-space layout (adopted constants from VMM_DESIGN.md sec. 3). */
#define YOUSJ_USER_END        0x0000800000000000ULL /* canonical low-half end */
#define YOUSJ_HHDM_BASE       0xFFFF800000000000ULL
#define YOUSJ_KERNEL_BASE     0xFFFFFFFF80000000ULL

/* Hardware PTE bits (x86-64 architectural definitions). */
#define YOUSJ_PTE_PRESENT     (1ULL << 0)
#define YOUSJ_PTE_WRITABLE    (1ULL << 1)
#define YOUSJ_PTE_USER        (1ULL << 2)
#define YOUSJ_PTE_DIRTY       (1ULL << 6)
#define YOUSJ_PTE_HUGE        (1ULL << 7)
#define YOUSJ_PTE_NX          (1ULL << 63)
#define YOUSJ_PTE_ADDR_MASK   0x000FFFFFFFFFF000ULL

#define YOUSJ_VMM_ENTRIES 512 /* entries per table = one page / 8 bytes */

/* Virtual address decomposition (pure, unit-tested). */
static inline uint32_t vmm_pml4_index(uint64_t virt) { return (uint32_t)((virt >> 39) & 0x1FF); }
static inline uint32_t vmm_pdpt_index(uint64_t virt) { return (uint32_t)((virt >> 30) & 0x1FF); }
static inline uint32_t vmm_pd_index(uint64_t virt)   { return (uint32_t)((virt >> 21) & 0x1FF); }
static inline uint32_t vmm_pt_index(uint64_t virt)   { return (uint32_t)((virt >> 12) & 0x1FF); }

/* Pure helpers: encode a leaf PTE from a page-aligned physical address
 * and abstract flags; decode a leaf PTE back to abstract flags. */
uint64_t vmm_pte_encode(uint64_t phys_page, uint32_t abstract_flags);
uint32_t vmm_pte_decode_flags(uint64_t pte);

struct vmm_space {
    uint64_t pml4_phys; /* physical address of the PML4 page (the CR3 value) */
    uint32_t refs;      /* M51: tasks referencing this space
                         * (CLONE_VM); born at 1 in
                         * vmm_create_space, destroyed at 0 */
};

/* Backend: how the VMM gets page-table pages and touches them.
 * alloc_page returns a physical page address or 0 (like the PMM);
 * free_page releases one; table_ptr turns a table's physical address
 * into a pointer to its 512 entries in the caller's address space. */
struct vmm_backend {
    uint64_t  (*alloc_page)(void);
    void      (*free_page)(uint64_t phys_page);
    uint64_t *(*table_ptr)(uint64_t phys_table);
};
void vmm_use_backend(const struct vmm_backend *backend);

/* Register the in-kernel backend (PMM + table access). Table access
 * starts out identity-based, which is valid while the boot chain's
 * early tables are in effect; vmm_hw_init() switches it to HHDM
 * access (see below) in the same breath as it loads the master
 * space's CR3, and every mapping in effect from then on (master and
 * user spaces alike) contains the HHDM. */
void vmm_use_kernel_backend(void);

/* Switch the kernel backend's page-table access from identity
 * (phys == virt) to HHDM (YOUSJ_HHDM_BASE + phys). Called exactly
 * once, by vmm_hw_init() immediately after the CR3 switch; the
 * HHDM must be mapped in the newly loaded space. */
void vmm_use_hhdm_table_access(void);

/* Kernel pointer to a physical frame, through the HHDM. Valid
 * everywhere after vmm_hw_init(): the HHDM is part of the kernel
 * half that vmm_create_space() shares into every user space, so
 * this works under any task's CR3. ALL kernel accesses to PMM
 * frames (ELF loading, fork copies, kernel stacks, zero pages)
 * go through here -- a raw physical address is never a pointer. */
static inline void *yousj_phys_to_virt(uint64_t phys)
{
    return (void *)(uintptr_t)(YOUSJ_HHDM_BASE + phys);
}

/* Create an empty address space: a zeroed PML4, with entries 256-511
 * copied from the kernel master space when one has been designated.
 * Storage for the struct itself is the caller's (there is no kernel
 * heap yet). Returns 0, -ENOMEM, or -EINVAL. */
int  vmm_create_space(struct vmm_space *out);
/* Free every page-table page of the user half and the PML4 itself.
 * Data pages are NOT freed (see header note). Destroying the kernel
 * master space is refused silently (a bug; bring-up halts elsewhere). */
void vmm_destroy_space(struct vmm_space *space);
/* M5: pool of caller-owned spaces for dynamically created tasks
 * (clone/execve). vmm_space_alloc() hands out a zeroed slot;
 * after vmm_destroy_space(), return it with vmm_space_release(). */
struct vmm_space *vmm_space_alloc(void);
void              vmm_space_release(struct vmm_space *space);
/* Free the DATA frames mapped in the user window (PML4 index of
 * YOUSJ_USER_BASE) -- destroy_space() alone frees only the page
 * TABLES. Call before vmm_destroy_space(), while no task runs
 * on the space. Assumes 4 KiB leaves, which is all the user
 * window ever gets in this layout. */
void              vmm_free_user_data(struct vmm_space *space);
/* Deep-copy the user window (all present 4 KiB leaves, with
 * their permissions) from src into the fresh dst space -- the
 * fork copy for clone (M5). Full copy; COW is a later
 * optimisation. Returns 0 or -YOUSJ_ENOMEM. */
int               vmm_clone_user_window(struct vmm_space *dst,
                                        struct vmm_space *src);

void               vmm_set_kernel_master(struct vmm_space *space);
struct vmm_space  *vmm_kernel_master(void);
/* (M6 retired vmm_share_kernel_identity(): with the kernel in the
 * higher half, vmm_create_space()'s copy of master entries
 * 256-511 is all a user space needs to run kernel code after a
 * CR3 switch -- text via entry 511, HHDM and stacks via 256.) */

int  vmm_map(struct vmm_space *space, uint64_t virt, uint64_t phys,
             uint64_t length, uint32_t flags);
int  vmm_unmap(struct vmm_space *space, uint64_t virt, uint64_t length);
/* On success *phys_out is the mapped PAGE base (add virt & 0xFFF for
 * the byte address) and *flags_out the abstract flags. -ENOENT when
 * any level is not present. Either out-pointer may be NULL. */
int  vmm_translate(struct vmm_space *space, uint64_t virt,
                   uint64_t *phys_out, uint32_t *flags_out);
int  vmm_protect(struct vmm_space *space, uint64_t virt, uint64_t length,
                 uint32_t flags);
/* M17: like vmm_translate but also yields the RAW leaf PTE,
 * so callers can see hardware bits (DIRTY) the abstract
 * flags hide. */
int  vmm_query_raw(struct vmm_space *space, uint64_t virt,
                   uint64_t *phys_out, uint64_t *raw_out);
/* M17: invalidate one TLB entry on this CPU (arch glue). */
void vmm_flush_tlb_va(uint64_t va);

/* Load a space's PML4 into CR3 (arch code,
 * kernel/arch/x86_64/vmm_arch.c). Called by vmm_hw_init() for the
 * master space and by the scheduler on every task switch. */
void vmm_load_cr3(uint64_t pml4_phys);
uint64_t vmm_read_cr3(void);

/* Hardware bring-up (arch glue, vmm_arch.c): enable EFER.NXE +
 * CR0.WP, build the kernel master space in the final layout --
 * HHDM direct map of [0, ram_top_phys) at YOUSJ_HHDM_BASE plus the
 * kernel image at YOUSJ_KERNEL_BASE with per-section permissions,
 * and NO identity map -- register it via vmm_set_kernel_master(),
 * load it into CR3 and switch page-table access to the HHDM.
 * Execution continues at the same (higher-half) addresses across
 * the switch: the boot chains map the image at its linked base in
 * their early tables, and every live stack at this point lives in
 * the kernel image's BSS, so nothing references the discarded
 * identity map. Returns 0 or a negative errno from the mapping
 * layer. */
int vmm_hw_init(uint64_t ram_top_phys);

#endif /* YOUSJ_VMM_H */

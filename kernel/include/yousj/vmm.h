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
 *     0xFFFFFFFF80000000. Identity mapping is a transition tool only.
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

/* Register the in-kernel backend (PMM + identity table access, valid
 * while the boot/early identity mapping is in effect). */
void vmm_use_kernel_backend(void);

/* Create an empty address space: a zeroed PML4, with entries 256-511
 * copied from the kernel master space when one has been designated.
 * Storage for the struct itself is the caller's (there is no kernel
 * heap yet). Returns 0, -ENOMEM, or -EINVAL. */
int  vmm_create_space(struct vmm_space *out);
/* Free every page-table page of the user half and the PML4 itself.
 * Data pages are NOT freed (see header note). Destroying the kernel
 * master space is refused silently (a bug; bring-up halts elsewhere). */
void vmm_destroy_space(struct vmm_space *space);

void               vmm_set_kernel_master(struct vmm_space *space);
struct vmm_space  *vmm_kernel_master(void);

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

/* Load a space's PML4 into CR3 (arch code, kernel/arch/x86_64/vmm_arch.c).
 * Deliberately NOT called anywhere yet: switching CR3 waits for the
 * bootloader paging contract (VMM_DESIGN.md sec. 2.3 / 6.2). */
void vmm_load_cr3(uint64_t pml4_phys);
uint64_t vmm_read_cr3(void);

#endif /* YOUSJ_VMM_H */

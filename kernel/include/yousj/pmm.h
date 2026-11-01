/* Yousj OS - physical memory manager (page frame allocator).
 *
 * Written from scratch by Muse (kernel core). Stage 1, server-first.
 *
 * The PMM hands out whole 4 KiB physical page frames. It learns which
 * memory exists, and which of it is usable, solely from the memory map
 * in struct yousj_boot_info (see boot.h) -- the bootloader is the only
 * source of truth. Anything not explicitly YOUSJ_MEM_USABLE starts out
 * reserved, so the kernel image, bootloader data and MMIO holes are
 * safe as long as the bootloader reports them as non-usable (its job
 * under the boot contract).
 *
 * Allocation failure is reported as address 0. Page frame 0 is never
 * handed out (it is reserved unconditionally), so 0 is a safe sentinel.
 *
 * Early-boot assumption (stage 1): the allocator's bitmap lives in the
 * kernel's own BSS, so no bootstrap allocation is needed. How much RAM
 * that bitmap can describe is capped by YOUSJ_PMM_MAX_PAGES below;
 * memory above the cap is reported via pmm_truncated() and ignored
 * until a later stage moves the bitmap into managed memory itself.
 */
#ifndef YOUSJ_PMM_H
#define YOUSJ_PMM_H

#include <yousj/types.h>
#include <yousj/boot.h>

#define YOUSJ_PAGE_SIZE   4096ULL
#define YOUSJ_PAGE_SHIFT  12
#define YOUSJ_PAGE_MASK   (~(YOUSJ_PAGE_SIZE - 1ULL))

/* Two bitmaps (used + usable) of this size each in BSS:
 * 2,097,152 pages = 8 GiB of physical RAM. */
#define YOUSJ_PMM_BITMAP_BYTES (256ULL * 1024ULL)
#define YOUSJ_PMM_MAX_PAGES    (YOUSJ_PMM_BITMAP_BYTES * 8ULL)

/* Build the allocator from the boot memory map. Safe to call again to
 * re-initialise (used by host-side unit tests). */
void pmm_init(const struct yousj_boot_info *boot_info);

/* Allocate one free page frame. Returns its physical address, or 0. */
uint64_t pmm_alloc_page(void);

/* Allocate `count` consecutive free page frames (for page tables and
 * other structures that need a contiguous run). Returns the physical
 * base address, or 0 if no run that large exists. */
uint64_t pmm_alloc_pages(uint64_t count);

/* Release a page frame previously returned by pmm_alloc_page().
 * Returns 0 on success, -1 if the address is not an allocated,
 * page-aligned frame inside the managed range (double free included). */
int pmm_free_page(uint64_t phys_addr);

/* Release a contiguous run from pmm_alloc_pages(). Same contract. */
int pmm_free_pages(uint64_t phys_addr, uint64_t count);

/* Reserve / release an arbitrary physical range (page granularity,
 * edges round outward for reserve, inward for release). Used during
 * bring-up for ranges the boot map did not describe; normal code
 * should not need these. */
void pmm_reserve_region(uint64_t base, uint64_t length);
void pmm_release_region(uint64_t base, uint64_t length);

uint64_t pmm_total_page_count(void);
uint64_t pmm_free_page_count(void);
uint64_t pmm_used_page_count(void);

/* True if the boot map described more RAM than the bitmap can hold. */
bool pmm_truncated(void);

#endif /* YOUSJ_PMM_H */

/* Yousj OS - host-side unit test for the VMM. Run with `make test`.
 * The VMM under test is the exact freestanding kernel/mm/vmm.c,
 * driven through its backend interface: page tables live in a fake
 * RAM array (66 MiB, same layout as test_pmm.c) and table pages come
 * from the real PMM -- plus a fault-injection counter on the backend
 * allocator to prove vmm_map() rolls back cleanly on ENOMEM.
 *
 * Fake layout: usable 0x100000-0x4000000 (63 MiB) and 0x4100000-
 * 0x4200000 (1 MiB), reserved hole 0x4000000-0x4100000.
 */
#include <stdio.h>
#include <string.h>
#include <yousj/vmm.h>

static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

static _Alignas(4096) uint8_t fake_ram[0x4200000];

/* --- backend with fault injection --------------------------------- */
static int alloc_calls;
static int fail_at = -1; /* 1-based backend alloc call to fail; -1 = never */

static uint64_t backend_alloc(void)
{
    alloc_calls++;
    if (fail_at > 0 && alloc_calls == fail_at)
        return 0;
    return pmm_alloc_page();
}
static void backend_free(uint64_t phys) { (void)pmm_free_page(phys); }
static uint64_t *backend_table_ptr(uint64_t phys)
{
    return (uint64_t *)(void *)(fake_ram + phys);
}
static const struct vmm_backend backend = {
    backend_alloc, backend_free, backend_table_ptr
};

static void build_map(struct yousj_boot_info *bi)
{
    memset(bi, 0, sizeof(*bi));
    bi->magic = YOUSJ_BOOT_MAGIC;
    bi->version = 1;
    bi->mem_entry_count = 4;
    bi->mem[0] = (struct yousj_mem_entry){ 0x000000,  0x100000,  YOUSJ_MEM_RESERVED, 0 };
    bi->mem[1] = (struct yousj_mem_entry){ 0x100000,  0x3F00000, YOUSJ_MEM_USABLE,   0 };
    bi->mem[2] = (struct yousj_mem_entry){ 0x4000000, 0x100000,  YOUSJ_MEM_RESERVED, 0 };
    bi->mem[3] = (struct yousj_mem_entry){ 0x4100000, 0x100000,  YOUSJ_MEM_USABLE,   0 };
}

int main(void)
{
    static struct yousj_boot_info bi;
    struct vmm_space sp, s2, master;
    uint64_t phys_out, d, d2, dk, run, before;
    uint32_t flags_out;

    build_map(&bi);
    pmm_init(&bi);
    vmm_use_backend(&backend);

    /* 1. Address decomposition (design sec. 8.2 item 1). */
    CHECK(vmm_pml4_index(0) == 0 && vmm_pdpt_index(0) == 0 &&
          vmm_pd_index(0) == 0 && vmm_pt_index(0) == 0);
    CHECK(vmm_pd_index(0x400000) == 2 && vmm_pt_index(0x400000) == 0);
    CHECK(vmm_pt_index(0x401000) == 1);
    CHECK(vmm_pd_index(0x200000) == 1);
    CHECK(vmm_pml4_index(YOUSJ_HHDM_BASE) == 256);
    CHECK(vmm_pml4_index(YOUSJ_KERNEL_BASE) == 511);
    CHECK(vmm_pdpt_index(YOUSJ_KERNEL_BASE) == 510);

    /* 2. PTE encode/decode round-trips (design sec. 8.2 item 2). */
    {
        uint64_t pte = vmm_pte_encode(0x12345000ULL,
                                      YOUSJ_VMM_READ | YOUSJ_VMM_WRITE);
        CHECK((pte & YOUSJ_PTE_ADDR_MASK) == 0x12345000ULL);
        CHECK(pte & YOUSJ_PTE_PRESENT);
        CHECK(pte & YOUSJ_PTE_WRITABLE);
        CHECK(!(pte & YOUSJ_PTE_USER));
        CHECK(pte & YOUSJ_PTE_NX); /* no EXEC requested */
        CHECK(vmm_pte_decode_flags(pte) ==
              (YOUSJ_VMM_READ | YOUSJ_VMM_WRITE));
        pte = vmm_pte_encode(0x2000, YOUSJ_VMM_READ | YOUSJ_VMM_EXEC |
                                     YOUSJ_VMM_USER);
        CHECK(!(pte & YOUSJ_PTE_NX));
        CHECK(vmm_pte_decode_flags(pte) ==
              (YOUSJ_VMM_READ | YOUSJ_VMM_EXEC | YOUSJ_VMM_USER));
    }

    /* 3. Create + single map + translate + protect + EEXIST/EINVAL. */
    CHECK(vmm_create_space(&sp) == 0);
    CHECK(sp.pml4_phys != 0 && (sp.pml4_phys & 0xFFF) == 0);
    d = pmm_alloc_page();
    CHECK(d != 0);
    CHECK(vmm_map(&sp, 0x400000, d, YOUSJ_PAGE_SIZE,
                  YOUSJ_VMM_READ | YOUSJ_VMM_WRITE) == 0);
    CHECK(vmm_translate(&sp, 0x400000, &phys_out, &flags_out) == 0);
    CHECK(phys_out == d);
    CHECK(flags_out == (YOUSJ_VMM_READ | YOUSJ_VMM_WRITE));
    CHECK(vmm_translate(&sp, 0x800000, &phys_out, &flags_out) == -YOUSJ_ENOENT);
    /* Duplicate map fails and preserves the original. */
    d2 = pmm_alloc_page();
    CHECK(vmm_map(&sp, 0x400000, d2, YOUSJ_PAGE_SIZE,
                  YOUSJ_VMM_READ) == -YOUSJ_EEXIST);
    CHECK(vmm_translate(&sp, 0x400000, &phys_out, NULL) == 0 && phys_out == d);
    /* Bad parameters. */
    CHECK(vmm_map(&sp, 0x400123, d2, YOUSJ_PAGE_SIZE, YOUSJ_VMM_READ) == -YOUSJ_EINVAL);
    CHECK(vmm_map(&sp, 0x500000, d2, 0, YOUSJ_VMM_READ) == -YOUSJ_EINVAL);
    CHECK(vmm_map(&sp, YOUSJ_USER_END - YOUSJ_PAGE_SIZE, d2,
                  2 * YOUSJ_PAGE_SIZE, YOUSJ_VMM_READ) == -YOUSJ_EINVAL);
    /* Protect: mapped -> ok and flags change; unmapped -> ENOENT. */
    CHECK(vmm_protect(&sp, 0x400000, YOUSJ_PAGE_SIZE,
                      YOUSJ_VMM_READ | YOUSJ_VMM_EXEC) == 0);
    CHECK(vmm_translate(&sp, 0x400000, NULL, &flags_out) == 0);
    CHECK(flags_out == (YOUSJ_VMM_READ | YOUSJ_VMM_EXEC));
    CHECK(vmm_protect(&sp, 0x800000, YOUSJ_PAGE_SIZE,
                      YOUSJ_VMM_READ) == -YOUSJ_ENOENT);
    /* Unmap with a hole in the range is allowed (design sec. 5.3). */
    CHECK(vmm_unmap(&sp, 0x400000, 3 * YOUSJ_PAGE_SIZE) == 0);
    CHECK(vmm_translate(&sp, 0x400000, &phys_out, NULL) == -YOUSJ_ENOENT);

    /* 4. A 4 MiB range crossing PT/PD boundaries; empty intermediate
     * tables are returned to the PMM by unmap (design sec. 8.2-6). */
    run = pmm_alloc_pages(1024);
    CHECK(run != 0);
    before = pmm_free_page_count();
    CHECK(vmm_map(&sp, 0x1000000, run, 0x400000,
                  YOUSJ_VMM_READ | YOUSJ_VMM_WRITE | YOUSJ_VMM_USER) == 0);
    {
        uint64_t samples[4] = { 0, 511, 512, 1023 };
        int i;
        for (i = 0; i < 4; i++) {
            CHECK(vmm_translate(&sp, 0x1000000 + samples[i] * YOUSJ_PAGE_SIZE,
                                &phys_out, &flags_out) == 0);
            CHECK(phys_out == run + samples[i] * YOUSJ_PAGE_SIZE);
            CHECK(flags_out & YOUSJ_VMM_USER);
        }
    }
    CHECK(vmm_unmap(&sp, 0x1000000, 0x400000) == 0);
    CHECK(vmm_translate(&sp, 0x1000000, &phys_out, NULL) == -YOUSJ_ENOENT);
    CHECK(pmm_free_page_count() == before); /* table pages all returned */
    CHECK(pmm_free_pages(run, 1024) == 0);  /* data pages were ours to free */

    /* 5. Fault injection: 2nd table allocation of a fresh 3-table
     * walk fails; map must return -ENOMEM and leave no trace. */
    d = pmm_alloc_page();
    CHECK(d != 0);
    before = pmm_free_page_count();
    alloc_calls = 0;
    fail_at = 2;
    CHECK(vmm_map(&sp, 0x2000000000ULL, d, YOUSJ_PAGE_SIZE,
                  YOUSJ_VMM_READ) == -YOUSJ_ENOMEM);
    fail_at = -1;
    CHECK(vmm_translate(&sp, 0x2000000000ULL, &phys_out, NULL) == -YOUSJ_ENOENT);
    CHECK(pmm_free_page_count() == before); /* rolled back, no leaked table */
    /* And the same map now succeeds, proving rollback left it clean. */
    CHECK(vmm_map(&sp, 0x2000000000ULL, d, YOUSJ_PAGE_SIZE,
                  YOUSJ_VMM_READ) == 0);
    CHECK(vmm_unmap(&sp, 0x2000000000ULL, YOUSJ_PAGE_SIZE) == 0);
    CHECK(pmm_free_page(d) == 0);

    /* 6. Kernel master sharing + space isolation + destroy rules. */
    CHECK(vmm_create_space(&master) == 0);
    dk = pmm_alloc_page();
    CHECK(dk != 0);
    CHECK(vmm_map(&master, YOUSJ_KERNEL_BASE, dk, YOUSJ_PAGE_SIZE,
                  YOUSJ_VMM_READ | YOUSJ_VMM_EXEC) == 0);
    vmm_set_kernel_master(&master);
    CHECK(vmm_create_space(&s2) == 0);
    /* s2 sees the master's kernel half... */
    CHECK(vmm_translate(&s2, YOUSJ_KERNEL_BASE, &phys_out, NULL) == 0);
    CHECK(phys_out == dk);
    /* ...but user halves stay private. */
    d2 = pmm_alloc_page();
    CHECK(vmm_map(&s2, 0x400000, d2, YOUSJ_PAGE_SIZE, YOUSJ_VMM_READ) == 0);
    CHECK(vmm_translate(&sp, 0x400000, &phys_out, NULL) == -YOUSJ_ENOENT);
    /* Destroying s2 must not touch the shared kernel tables. */
    vmm_destroy_space(&s2);
    CHECK(vmm_translate(&master, YOUSJ_KERNEL_BASE, &phys_out, NULL) == 0);
    CHECK(phys_out == dk);
    /* The master itself cannot be destroyed while designated. */
    vmm_destroy_space(&master);
    CHECK(master.pml4_phys != 0);
    vmm_set_kernel_master(NULL);
    vmm_destroy_space(&master);
    CHECK(master.pml4_phys == 0);
    CHECK(pmm_free_page(dk) == 0);

    /* 7. destroy_space frees table pages but never data pages. */
    before = pmm_free_page_count();
    {
        struct vmm_space tmp;
        uint64_t data = pmm_alloc_page();
        uint64_t after_map;
        CHECK(data != 0);
        CHECK(vmm_create_space(&tmp) == 0);
        CHECK(vmm_map(&tmp, 0x400000, data, YOUSJ_PAGE_SIZE,
                      YOUSJ_VMM_READ) == 0);
        after_map = pmm_free_page_count();
        CHECK(before - 1 - after_map >= 3); /* pml4 + at least PDPT/PD/PT... (data accounted) */
        vmm_destroy_space(&tmp);
        /* Table pages returned; the data page is still allocated and
         * can be freed by its owner -- exactly once. */
        CHECK(pmm_free_page(data) == 0);
        CHECK(pmm_free_page_count() == before);
    }

    vmm_destroy_space(&sp);
    CHECK(pmm_free_page(d2) == 0);

    if (failures == 0)
        printf("test_vmm: all checks PASS (indices, PTE round-trip, map/translate/protect, rollback, sharing, destroy accounting)\n");
    else
        printf("test_vmm: %d FAILURES\n", failures);
    return failures ? 1 : 0;
}

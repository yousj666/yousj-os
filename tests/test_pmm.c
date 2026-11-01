/* Yousj OS - host-side unit test for the PMM. Run with `make test`.
 * Uses a fake boot_info memory map; the PMM under test is the exact
 * freestanding kernel/mm/pmm.c, compiled hosted here.
 *
 * Fake physical layout (64 MiB + a reserved tail):
 *   0x000000 - 0x100000   RESERVED  (legacy 1 MiB)
 *   0x100000 - 0x4000000  USABLE    (63 MiB)
 *   0x4000000- 0x4100000  RESERVED  (hole: must never be allocated)
 *   0x4100000- 0x4200000  USABLE    (1 MiB more, past the hole)
 * Total described: 0x4200000 = 66 MiB = 16896 pages.
 * Expected free after init: 63 MiB + 1 MiB = 64 MiB = 16384 pages.
 */
#include <stdio.h>
#include <string.h>
#include <yousj/pmm.h>

static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

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
    uint64_t a, b, run, i, j;
    uint64_t seen[128];

    build_map(&bi);
    pmm_init(&bi);

    /* After init: totals and the hole must be respected. */
    CHECK(pmm_total_page_count() == 16896);
    CHECK(pmm_free_page_count() == 16384);
    CHECK(pmm_used_page_count() == 16896 - 16384);
    CHECK(!pmm_truncated());

    /* Single allocation: lowest usable frame, page aligned, not zero. */
    a = pmm_alloc_page();
    CHECK(a == 0x100000);
    CHECK(pmm_free_page_count() == 16383);
    b = pmm_alloc_page();
    CHECK(b == 0x101000);
    CHECK(b != a);

    /* Free + realloc: first-fit returns the same lowest frame. */
    CHECK(pmm_free_page(a) == 0);
    CHECK(pmm_free_page_count() == 16383);
    CHECK(pmm_alloc_page() == a);

    /* Double free and bad frees are rejected and change nothing. */
    CHECK(pmm_free_page(a) == 0);
    CHECK(pmm_free_page(a) == -1);
    CHECK(pmm_free_page(0) == -1);
    CHECK(pmm_free_page(0x100123) == -1);       /* unaligned */
    CHECK(pmm_free_page(0x4000000) == -1);      /* reserved hole */
    CHECK(pmm_free_page(0x100000000ULL) == -1); /* beyond the map */
    CHECK(pmm_free_page_count() == 16383);           /* only b still held... */
    /* b (0x101000) is still allocated; free it for a clean slate. */
    CHECK(pmm_free_page(b) == 0);
    CHECK(pmm_free_page_count() == 16384);

    /* No duplicates across 128 allocations. */
    for (i = 0; i < 128; i++) {
        seen[i] = pmm_alloc_page();
        CHECK(seen[i] != 0);
        CHECK((seen[i] & 0xFFF) == 0);
        CHECK(!(seen[i] >= 0x4000000 && seen[i] < 0x4100000));
        for (j = 0; j < i; j++)
            CHECK(seen[i] != seen[j]);
    }
    for (i = 0; i < 128; i++)
        CHECK(pmm_free_page(seen[i]) == 0);
    CHECK(pmm_free_page_count() == 16384);

    /* Contiguous run: 4 pages, aligned, freed as a run.
     * Freeing only half a run then the rest must also work. */
    run = pmm_alloc_pages(4);
    CHECK(run == 0x100000);
    CHECK(pmm_free_page_count() == 16380);
    CHECK(pmm_free_pages(run, 2) == 0);
    CHECK(pmm_free_pages(run + 2 * YOUSJ_PAGE_SIZE, 2) == 0);
    CHECK(pmm_free_page_count() == 16384);
    CHECK(pmm_alloc_pages(0) == 0);

    /* Reserve an extra region, then release it back. */
    pmm_reserve_region(0x200000, 0x10000);
    CHECK(pmm_free_page_count() == 16384 - 16);
    pmm_release_region(0x200000, 0x10000);
    CHECK(pmm_free_page_count() == 16384);
    /* The legacy first 1 MiB can never be released back into use. */
    pmm_release_region(0, 0x100000);
    CHECK(pmm_free_page_count() == 16384);

    /* Exhaustion: drain everything, next allocs fail, then refill. */
    {
        uint64_t n = 0, p;
        while ((p = pmm_alloc_page()) != 0)
            n++;
        CHECK(n == 16384);
        CHECK(pmm_free_page_count() == 0);
        CHECK(pmm_alloc_pages(2) == 0);
        /* Give one page back; a 2-page run must still fail, 1 succeeds. */
        CHECK(pmm_free_page(0x100000) == 0);
        CHECK(pmm_alloc_pages(2) == 0);
        CHECK(pmm_alloc_page() == 0x100000);
    }

    /* NULL / empty map: allocator stays inert, never crashes. */
    pmm_init(NULL);
    CHECK(pmm_alloc_page() == 0);
    CHECK(pmm_total_page_count() == 0);

    if (failures == 0)
        printf("test_pmm: all checks PASS (total=%llu pages on the fake 66 MiB map, free accounting exact)\n",
               (unsigned long long)16896);
    else
        printf("test_pmm: %d FAILURES\n", failures);
    return failures ? 1 : 0;
}

/* Yousj OS - physical page frame allocator (bitmap PMM).
 *
 * Written from scratch by Muse (kernel core). No Linux source, no libc:
 * this file must compile both freestanding (-ffreestanding, kernel) and
 * hosted (host-side unit tests in tests/test_pmm.c), so it uses only
 * the freestanding headers from <yousj/...> and its own helpers.
 *
 * Design, deliberately simple for stage 1:
 *   - Two bitmaps, one bit per page frame each:
 *       g_usable: 1 = the boot map called this frame YOUSJ_MEM_USABLE
 *                 (minus the legacy first 1 MiB, cleared at init).
 *       g_used:   1 = currently allocated or reserved, 0 = free.
 *     A frame can be handed out only when usable && !used. Two bitmaps
 *     are needed, not one: with a single "used" bit a reserved hole
 *     looks exactly like an allocated page, and freeing it would hand
 *     MMIO / ROM space to some unsuspecting caller. The host test
 *     caught exactly that bug in the first version -- the hole case
 *     in tests/test_pmm.c is now a regression test.
 *   - First-fit scan from frame 0. O(total_pages) worst case per
 *     allocation; correctness first, a free-list/buddy allocator can
 *     replace the internals later behind this same header.
 *   - Invariant: g_free_pages always equals the number of frames with
 *     usable && !used. mark_page() is the only place either bitmap's
 *     used bit changes for a usable frame, and it maintains the count,
 *     so the stats can never drift from the bitmaps.
 */
#include <yousj/pmm.h>

static uint8_t  g_used[YOUSJ_PMM_BITMAP_BYTES];
static uint8_t  g_usable[YOUSJ_PMM_BITMAP_BYTES];
static uint64_t g_total_pages;
static uint64_t g_free_pages;
static bool     g_truncated;
static bool     g_ready;

static uint64_t align_down(uint64_t v, uint64_t a) { return v & ~(a - 1); }
static uint64_t align_up(uint64_t v, uint64_t a)   { return (v + a - 1) & ~(a - 1); }

static bool page_is_used(uint64_t page)
{
    return (g_used[page / 8] >> (page % 8)) & 1u;
}

static bool page_is_usable(uint64_t page)
{
    return (g_usable[page / 8] >> (page % 8)) & 1u;
}

static void set_usable(uint64_t page, bool usable)
{
    if (usable)
        g_usable[page / 8] |= (uint8_t)(1u << (page % 8));
    else
        g_usable[page / 8] &= (uint8_t)~(1u << (page % 8));
}

/* Set one frame's used bit. The free count moves only for frames that
 * are usable -- a reserved hole flipping its used bit is not a change
 * in how much memory can be handed out. */
static void mark_page(uint64_t page, bool used)
{
    bool was_used = page_is_used(page);
    if (used == was_used)
        return;
    if (used)
        g_used[page / 8] |= (uint8_t)(1u << (page % 8));
    else
        g_used[page / 8] &= (uint8_t)~(1u << (page % 8));
    if (page_is_usable(page)) {
        if (used)
            g_free_pages--;
        else
            g_free_pages++;
    }
}

/* Page range shared by mark_range() and the usable-map builder.
 * reserve (used=true): round edges outward, cover partial pages.
 * release: round edges inward, only whole pages. Fills first/last
 * (inclusive page indices, clamped to the managed range); returns
 * false when the range covers no managed page. */
static bool range_pages(uint64_t base, uint64_t length, bool used,
                        uint64_t *first_out, uint64_t *last_out)
{
    uint64_t first, last;
    if (length == 0 || g_total_pages == 0)
        return false;
    if (used) {
        first = align_down(base, YOUSJ_PAGE_SIZE) / YOUSJ_PAGE_SIZE;
        last  = align_down(base + length - 1, YOUSJ_PAGE_SIZE) / YOUSJ_PAGE_SIZE;
    } else {
        if (base + length < YOUSJ_PAGE_SIZE)
            return false;
        first = align_up(base, YOUSJ_PAGE_SIZE) / YOUSJ_PAGE_SIZE;
        last  = align_down(base + length - 1, YOUSJ_PAGE_SIZE) / YOUSJ_PAGE_SIZE;
        if (last < first)
            return false;
    }
    if (first >= g_total_pages)
        return false;
    if (last >= g_total_pages)
        last = g_total_pages - 1;
    *first_out = first;
    *last_out = last;
    return true;
}

static void mark_range(uint64_t base, uint64_t length, bool used)
{
    uint64_t first, last, p;
    if (!range_pages(base, length, used, &first, &last))
        return;
    for (p = first; p <= last; p++)
        mark_page(p, used);
}

void pmm_init(const struct yousj_boot_info *boot_info)
{
    uint64_t i, max_phys = 0;

    for (i = 0; i < YOUSJ_PMM_BITMAP_BYTES; i++) {
        g_used[i]   = 0xFF; /* everything used until proven usable */
        g_usable[i] = 0x00;
    }
    g_total_pages = 0;
    g_free_pages  = 0;
    g_truncated   = false;
    g_ready       = false;

    if (boot_info == NULL || boot_info->mem_entry_count == 0)
        return;

    for (i = 0; i < boot_info->mem_entry_count && i < YOUSJ_MAX_MEM_ENTRIES; i++) {
        uint64_t end = boot_info->mem[i].base + boot_info->mem[i].length;
        if (end > max_phys)
            max_phys = end;
    }
    if (max_phys == 0)
        return;

    g_total_pages = align_up(max_phys, YOUSJ_PAGE_SIZE) / YOUSJ_PAGE_SIZE;
    if (g_total_pages > YOUSJ_PMM_MAX_PAGES) {
        g_total_pages = YOUSJ_PMM_MAX_PAGES;
        g_truncated = true;
    }

    /* 1. Record which frames are usable at all (whole pages only). */
    for (i = 0; i < boot_info->mem_entry_count && i < YOUSJ_MAX_MEM_ENTRIES; i++) {
        uint64_t first, last, p;
        if (boot_info->mem[i].type != YOUSJ_MEM_USABLE)
            continue;
        if (range_pages(boot_info->mem[i].base, boot_info->mem[i].length,
                        false, &first, &last)) {
            for (p = first; p <= last; p++)
                set_usable(p, true);
        }
    }
    /* 2. With the usable map in place, releasing those frames moves
     *    the free count to exactly the usable total. */
    for (i = 0; i < boot_info->mem_entry_count && i < YOUSJ_MAX_MEM_ENTRIES; i++) {
        if (boot_info->mem[i].type == YOUSJ_MEM_USABLE)
            mark_range(boot_info->mem[i].base, boot_info->mem[i].length, false);
    }

    /* 3. Frame 0 guards the NULL page; the first 1 MiB is x86 legacy
     *    territory (IVT/BDA, VGA, ROM holes) -- reserve it, then clear
     *    its usable bits so it can never be freed back into use. */
    mark_range(0, 0x100000, true);
    {
        uint64_t p;
        for (p = 0; p < 0x100000 / YOUSJ_PAGE_SIZE && p < g_total_pages; p++)
            set_usable(p, false);
    }

    g_ready = true;
}

uint64_t pmm_alloc_page(void)
{
    return pmm_alloc_pages(1);
}

uint64_t pmm_alloc_pages(uint64_t count)
{
    uint64_t run_start = 0, run_len = 0, p;

    if (!g_ready || count == 0 || count > g_free_pages)
        return 0;

    for (p = 0; p < g_total_pages; p++) {
        if (!page_is_usable(p) || page_is_used(p)) {
            run_len = 0;
            continue;
        }
        if (run_len == 0)
            run_start = p;
        run_len++;
        if (run_len == count) {
            uint64_t i;
            for (i = 0; i < count; i++)
                mark_page(run_start + i, true);
            return run_start * YOUSJ_PAGE_SIZE;
        }
    }
    return 0;
}

int pmm_free_page(uint64_t phys_addr)
{
    return pmm_free_pages(phys_addr, 1);
}

int pmm_free_pages(uint64_t phys_addr, uint64_t count)
{
    uint64_t first, i;

    if (!g_ready || count == 0)
        return -1;
    if (phys_addr == 0 || (phys_addr & (YOUSJ_PAGE_SIZE - 1)) != 0)
        return -1;
    first = phys_addr / YOUSJ_PAGE_SIZE;
    if (first >= g_total_pages || count > g_total_pages - first)
        return -1;
    /* Validate the whole run first, so a bad free changes nothing:
     * every frame must be usable AND currently allocated. A reserved
     * hole is used-but-not-usable and fails here, as does a double
     * free (free-but-usable). */
    for (i = 0; i < count; i++) {
        if (!page_is_usable(first + i) || !page_is_used(first + i))
            return -1;
    }
    for (i = 0; i < count; i++)
        mark_page(first + i, false);
    return 0;
}

void pmm_reserve_region(uint64_t base, uint64_t length)
{
    if (g_ready)
        mark_range(base, length, true);
}

void pmm_release_region(uint64_t base, uint64_t length)
{
    if (!g_ready)
        return;
    /* mark_page() counts only usable frames, and the legacy 1 MiB has
     * its usable bits cleared, so this can never resurrect a hole or
     * the legacy area. */
    mark_range(base, length, false);
}

uint64_t pmm_total_page_count(void) { return g_total_pages; }
uint64_t pmm_free_page_count(void)  { return g_free_pages; }
/* "Used" = everything not hand-out-able: allocations + reserved. */
uint64_t pmm_used_page_count(void)  { return g_total_pages - g_free_pages; }
bool     pmm_truncated(void)   { return g_truncated; }

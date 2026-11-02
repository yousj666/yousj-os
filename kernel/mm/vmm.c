/* Yousj OS - virtual memory manager: four-level page-table walking,
 * mapping and unmapping. Written from scratch by Muse (kernel core).
 *
 * Steps 1-2 of docs/VMM_DESIGN.md's landing order: all logic here is
 * hardware-free (no CR3, no invlpg -- a map into a space that is not
 * loaded needs no TLB work, and spaces that ARE loaded do not exist
 * yet). The hardware step is vmm_load_cr3() in the arch file, unused
 * until the bootloader paging contract lands.
 *
 * Everything goes through the registered backend (see vmm.h): table
 * pages come from the backend allocator and are only ever touched
 * through backend.table_ptr(). That is what lets tests/test_vmm.c run
 * this exact file against a fake RAM array on the host.
 */
#include <yousj/vmm.h>
#include <yousj/user.h> /* YOUSJ_USER_BASE */

static struct vmm_backend g_backend;
static bool g_backend_ready;
static struct vmm_space *g_kernel_master;

static void mem_zero(uint64_t *p, uint64_t words)
{
    uint64_t i;
    for (i = 0; i < words; i++)
        p[i] = 0;
}

static void mem_copy(uint64_t *dst, const uint64_t *src, uint64_t words)
{
    uint64_t i;
    for (i = 0; i < words; i++)
        dst[i] = src[i];
}

void vmm_use_backend(const struct vmm_backend *backend)
{
    if (backend == NULL) {
        g_backend_ready = false;
        return;
    }
    g_backend = *backend;
    g_backend_ready = true;
}

/* --- kernel backend: PMM + identity access ------------------------ */

static uint64_t kernel_alloc(void) { return pmm_alloc_page(); }
static void kernel_free(uint64_t phys) { (void)pmm_free_page(phys); }
static uint64_t *kernel_table_ptr(uint64_t phys)
{
    /* Valid only while an identity (or HHDM-offset) mapping for the
     * physical memory containing page tables is in effect. That is
     * true in the boot environment today; when the higher-half switch
     * happens this becomes HHDM_BASE + phys (VMM_DESIGN.md sec. 6). */
    return (uint64_t *)(uintptr_t)phys;
}

void vmm_use_kernel_backend(void)
{
    static const struct vmm_backend kb = {
        kernel_alloc, kernel_free, kernel_table_ptr
    };
    vmm_use_backend(&kb);
}

/* --- pure PTE encode/decode --------------------------------------- */

uint64_t vmm_pte_encode(uint64_t phys_page, uint32_t abstract_flags)
{
    uint64_t pte = (phys_page & YOUSJ_PTE_ADDR_MASK) | YOUSJ_PTE_PRESENT;
    if (abstract_flags & YOUSJ_VMM_WRITE)
        pte |= YOUSJ_PTE_WRITABLE;
    if (abstract_flags & YOUSJ_VMM_USER)
        pte |= YOUSJ_PTE_USER;
    if (!(abstract_flags & YOUSJ_VMM_EXEC))
        pte |= YOUSJ_PTE_NX;
    return pte;
}

uint32_t vmm_pte_decode_flags(uint64_t pte)
{
    uint32_t flags = 0;
    if (pte & YOUSJ_PTE_PRESENT)
        flags |= YOUSJ_VMM_READ; /* a present page is always readable */
    if (pte & YOUSJ_PTE_WRITABLE)
        flags |= YOUSJ_VMM_WRITE;
    if (pte & YOUSJ_PTE_USER)
        flags |= YOUSJ_VMM_USER;
    if (!(pte & YOUSJ_PTE_NX))
        flags |= YOUSJ_VMM_EXEC;
    return flags;
}

/* --- walking -------------------------------------------------------- */

static uint64_t *table_of(uint64_t phys_table)
{
    return g_backend.table_ptr(phys_table);
}

/* Intermediate entries are permissive (design sec. 6.1): real access
 * control lives in the leaf. USER is set on intermediates only for
 * user-half addresses, or the CPU would let Ring 3 walk kernel tables
 * paths -- the leaf still blocks it, but defence in depth is free. */
static uint64_t intermediate_pte(uint64_t child_phys, uint64_t virt)
{
    uint64_t pte = (child_phys & YOUSJ_PTE_ADDR_MASK)
                 | YOUSJ_PTE_PRESENT | YOUSJ_PTE_WRITABLE;
    if (virt < YOUSJ_USER_END)
        pte |= YOUSJ_PTE_USER;
    return pte;
}

/* Log of intermediate tables created by one vmm_map() call, so a
 * failure can roll back exactly -- and only -- what this call made.
 * 128 entries supports a single map of tens of MiB; exceeding it is a
 * clean -EINVAL rollback, not corruption (stage-1 limit, see vmm_map). */
#define VMM_CREATE_LOG_MAX 128
struct create_log {
    uint64_t phys[VMM_CREATE_LOG_MAX];
    uint64_t count;
};

/* Walk to the leaf entry slot for `virt`, creating missing levels.
 * Returns the slot pointer, or NULL on allocation failure / log full
 * (both are reported the same way to the caller, which rolls back). */
static uint64_t *ensure_leaf_slot(struct vmm_space *space, uint64_t virt,
                                  struct create_log *log)
{
    uint64_t table_phys = space->pml4_phys;
    uint32_t indices[3];
    int level;

    indices[0] = vmm_pml4_index(virt);
    indices[1] = vmm_pdpt_index(virt);
    indices[2] = vmm_pd_index(virt);

    for (level = 0; level < 3; level++) {
        uint64_t *table = table_of(table_phys);
        uint64_t entry = table[indices[level]];
        if (entry & YOUSJ_PTE_PRESENT) {
            if (entry & YOUSJ_PTE_HUGE)
                return NULL; /* huge pages are not created by this VMM yet */
            table_phys = entry & YOUSJ_PTE_ADDR_MASK;
            continue;
        }
        /* Missing level: allocate, ZERO FIRST, then link (design
         * sec. 6.1 -- linking an unzeroed table would let the CPU
         * interpret garbage as valid mappings). */
        {
            uint64_t child = g_backend.alloc_page();
            uint64_t *child_table;
            if (child == 0)
                return NULL;
            if (log->count >= VMM_CREATE_LOG_MAX) {
                g_backend.free_page(child);
                return NULL;
            }
            child_table = table_of(child);
            mem_zero(child_table, YOUSJ_VMM_ENTRIES);
            table[indices[level]] = intermediate_pte(child, virt);
            log->phys[log->count++] = child;
            table_phys = child;
        }
    }
    return &table_of(table_phys)[vmm_pt_index(virt)];
}

/* Walk without creating. Returns the leaf entry value, or 0 when any
 * level (or the leaf) is not present / is a huge page we do not make. */
static uint64_t walk_leaf(const struct vmm_space *space, uint64_t virt)
{
    uint64_t table_phys = space->pml4_phys;
    uint32_t indices[4];
    int level;

    indices[0] = vmm_pml4_index(virt);
    indices[1] = vmm_pdpt_index(virt);
    indices[2] = vmm_pd_index(virt);
    indices[3] = vmm_pt_index(virt);

    for (level = 0; level < 4; level++) {
        uint64_t entry = table_of(table_phys)[indices[level]];
        if (!(entry & YOUSJ_PTE_PRESENT))
            return 0;
        if (level == 3)
            return entry;
        if (entry & YOUSJ_PTE_HUGE)
            return 0;
        table_phys = entry & YOUSJ_PTE_ADDR_MASK;
    }
    return 0;
}

/* Undo a partial vmm_map: clear the leaves this call wrote (given as a
 * base virt + page count), then free the tables it created, deepest
 * first (reverse creation order frees children before parents). */
static void rollback_map(struct vmm_space *space, uint64_t virt,
                         uint64_t pages_done, struct create_log *log)
{
    uint64_t i;
    for (i = 0; i < pages_done; i++) {
        uint64_t table_phys = space->pml4_phys;
        uint32_t idx[3];
        int level, ok = 1;
        idx[0] = vmm_pml4_index(virt + i * YOUSJ_PAGE_SIZE);
        idx[1] = vmm_pdpt_index(virt + i * YOUSJ_PAGE_SIZE);
        idx[2] = vmm_pd_index(virt + i * YOUSJ_PAGE_SIZE);
        for (level = 0; level < 3; level++) {
            uint64_t entry = table_of(table_phys)[idx[level]];
            if (!(entry & YOUSJ_PTE_PRESENT)) { ok = 0; break; }
            table_phys = entry & YOUSJ_PTE_ADDR_MASK;
        }
        if (ok)
            table_of(table_phys)[vmm_pt_index(virt + i * YOUSJ_PAGE_SIZE)] = 0;
    }
    while (log->count > 0)
        g_backend.free_page(log->phys[--log->count]);
}

/* --- validation ----------------------------------------------------- */

static bool is_canonical_half(uint64_t virt)
{
    /* low canonical half or high canonical half */
    return virt < YOUSJ_USER_END || virt >= YOUSJ_HHDM_BASE;
}

static int validate_range(uint64_t virt, uint64_t length)
{
    uint64_t last;
    if (length == 0)
        return -YOUSJ_EINVAL;
    if ((virt & (YOUSJ_PAGE_SIZE - 1)) != 0 || (length & (YOUSJ_PAGE_SIZE - 1)) != 0)
        return -YOUSJ_EINVAL;
    if (!is_canonical_half(virt))
        return -YOUSJ_EINVAL;
    last = virt + length - 1;
    if (last < virt)
        return -YOUSJ_EINVAL; /* wrapped */
    if (!is_canonical_half(last))
        return -YOUSJ_EINVAL;
    /* must not straddle the user/kernel boundary */
    if ((virt < YOUSJ_USER_END) != (last < YOUSJ_USER_END))
        return -YOUSJ_EINVAL;
    return 0;
}

/* --- spaces --------------------------------------------------------- */

int vmm_create_space(struct vmm_space *out)
{
    uint64_t pml4;
    if (!g_backend_ready || out == NULL)
        return -YOUSJ_EINVAL;
    pml4 = g_backend.alloc_page();
    if (pml4 == 0)
        return -YOUSJ_ENOMEM;
    mem_zero(table_of(pml4), YOUSJ_VMM_ENTRIES);
    out->pml4_phys = pml4;
    if (g_kernel_master != NULL && g_kernel_master != out) {
        /* Share the kernel half (entries 256-511) with the master. */
        uint64_t *dst = table_of(pml4);
        const uint64_t *src = table_of(g_kernel_master->pml4_phys);
        mem_copy(dst + 256, src + 256, 256);
    }
    return 0;
}

static bool table_all_empty(const uint64_t *table)
{
    uint32_t i;
    for (i = 0; i < YOUSJ_VMM_ENTRIES; i++) {
        if (table[i] != 0)
            return false;
    }
    return true;
}

/* Recursively free tables under `entry` (a non-leaf entry at the given
 * depth: 0 = points to PDPT, 1 = to PD, 2 = to PT). The pointed-to
 * table page itself is freed after its children. */
static void free_table_tree(uint64_t table_phys, int depth)
{
    uint64_t *table = table_of(table_phys);
    if (depth < 2) {
        uint32_t i;
        for (i = 0; i < YOUSJ_VMM_ENTRIES; i++) {
            if (table[i] & YOUSJ_PTE_PRESENT)
                free_table_tree(table[i] & YOUSJ_PTE_ADDR_MASK, depth + 1);
        }
    }
    g_backend.free_page(table_phys);
}

void vmm_destroy_space(struct vmm_space *space)
{
    uint64_t *pml4;
    uint32_t i;
    if (!g_backend_ready || space == NULL || space == g_kernel_master)
        return;
    if (space->pml4_phys == 0)
        return;
    pml4 = table_of(space->pml4_phys);
    /* User half only: the kernel half is shared with the master and
     * its tables are never owned by this space. Entry 0 needs the
     * same care in the transition period: when it is byte-identical
     * to the master's entry 0, it is the SHARED kernel identity
     * subtree (see vmm_share_kernel_identity) and must not be
     * freed either. */
    for (i = 0; i < 256; i++) {
        if (pml4[i] & YOUSJ_PTE_PRESENT) {
            if (i == 0 && g_kernel_master != NULL &&
                pml4[0] == table_of(g_kernel_master->pml4_phys)[0])
                continue;
            free_table_tree(pml4[i] & YOUSJ_PTE_ADDR_MASK, 0);
            pml4[i] = 0;
        }
    }
    g_backend.free_page(space->pml4_phys);
    space->pml4_phys = 0;
}

/* --- owned-space pool (M5) -------------------------------------- */

#define YOUSJ_SPACE_POOL 32
static struct vmm_space g_space_pool[YOUSJ_SPACE_POOL];
static bool             g_space_pool_used[YOUSJ_SPACE_POOL];

struct vmm_space *vmm_space_alloc(void)
{
    uint32_t i;
    for (i = 0; i < YOUSJ_SPACE_POOL; i++) {
        if (!g_space_pool_used[i]) {
            g_space_pool_used[i] = true;
            g_space_pool[i].pml4_phys = 0;
            return &g_space_pool[i];
        }
    }
    return NULL;
}

void vmm_space_release(struct vmm_space *space)
{
    uint32_t i;
    for (i = 0; i < YOUSJ_SPACE_POOL; i++) {
        if (&g_space_pool[i] == space)
            g_space_pool_used[i] = false;
    }
}

/* Free every present 4 KiB leaf under one PML4 entry (the tables
 * themselves are destroy_space()'s job). */
static void free_data_tree(uint64_t table_phys, int depth)
{
    uint64_t *table = table_of(table_phys);
    uint32_t i;
    for (i = 0; i < YOUSJ_VMM_ENTRIES; i++) {
        uint64_t e = table[i];
        if (!(e & YOUSJ_PTE_PRESENT))
            continue;
        if (depth < 2)
            free_data_tree(e & YOUSJ_PTE_ADDR_MASK, depth + 1);
        else
            g_backend.free_page(e & YOUSJ_PTE_ADDR_MASK);
    }
}

void vmm_free_user_data(struct vmm_space *space)
{
    uint64_t *pml4;
    uint64_t idx;
    if (!g_backend_ready || space == NULL || space->pml4_phys == 0)
        return;
    pml4 = table_of(space->pml4_phys);
    idx = (YOUSJ_USER_BASE >> 39) & 0x1FFULL;
    if (pml4[idx] & YOUSJ_PTE_PRESENT)
        free_data_tree(pml4[idx] & YOUSJ_PTE_ADDR_MASK, 0);
}

/* Enumerate the present leaves of the user window and deep-copy
 * them into dst. The enumeration walks the tables directly; the
 * per-page flags/phys come from vmm_translate so the PTE <->
 * abstract-flag conversion lives in exactly one place. */
static int clone_window_level(struct vmm_space *dst,
                              struct vmm_space *src,
                              uint64_t table_phys, int depth,
                              uint64_t va_base)
{
    uint64_t *table = table_of(table_phys);
    uint32_t i;
    for (i = 0; i < YOUSJ_VMM_ENTRIES; i++) {
        uint64_t e = table[i];
        uint64_t va = va_base + ((uint64_t)i << (39 - 9 * depth));
        if (!(e & YOUSJ_PTE_PRESENT))
            continue;
        if (depth < 3) {
            int rc = clone_window_level(dst, src,
                                        e & YOUSJ_PTE_ADDR_MASK,
                                        depth + 1, va);
            if (rc != 0)
                return rc;
        } else {
            uint64_t phys = 0, new_phys;
            uint32_t flags = 0;
            if (vmm_translate(src, va, &phys, &flags) != 0)
                return -YOUSJ_ENOMEM;
            new_phys = g_backend.alloc_page();
            if (new_phys == 0)
                return -YOUSJ_ENOMEM;
            /* Identity period: a physical frame is directly
             * dereferenceable from the kernel. */
            {
                const uint8_t *sp = (const uint8_t *)(uintptr_t)phys;
                uint8_t *dp = (uint8_t *)(uintptr_t)new_phys;
                uint32_t b;
                for (b = 0; b < YOUSJ_PAGE_SIZE; b++)
                    dp[b] = sp[b];
            }
            if (vmm_map(dst, va, new_phys, YOUSJ_PAGE_SIZE, flags) != 0) {
                g_backend.free_page(new_phys);
                return -YOUSJ_ENOMEM;
            }
        }
    }
    return 0;
}

int vmm_clone_user_window(struct vmm_space *dst, struct vmm_space *src)
{
    uint64_t *pml4;
    uint64_t idx;
    if (!g_backend_ready || dst == NULL || src == NULL ||
        dst->pml4_phys == 0 || src->pml4_phys == 0)
        return -YOUSJ_ENOMEM;
    pml4 = table_of(src->pml4_phys);
    idx = (YOUSJ_USER_BASE >> 39) & 0x1FFULL;
    if (!(pml4[idx] & YOUSJ_PTE_PRESENT))
        return 0; /* empty window: nothing to copy */
    return clone_window_level(dst, src,
                              pml4[idx] & YOUSJ_PTE_ADDR_MASK, 1,
                              idx << 39);
}

void vmm_set_kernel_master(struct vmm_space *space)
{
    g_kernel_master = space;
}

void vmm_share_kernel_identity(struct vmm_space *space)
{
    if (!g_backend_ready || space == NULL || g_kernel_master == NULL ||
        space == g_kernel_master)
        return;
    /* Transition arrangement (identity-kernel period): the kernel
     * occupies the low half under PML4 entry 0, so every user
     * space must see that exact subtree to be able to run kernel
     * code at all after a CR3 switch. The entries carry no USER
     * flag, so ring 3 cannot touch them; they point at the
     * master's tables, which vmm_destroy_space() now recognises
     * and refuses to free. When the kernel moves to the higher
     * half (VMM_DESIGN sec. 6.2), create-space already shares the
     * kernel half and this helper goes away. */
    table_of(space->pml4_phys)[0] = table_of(g_kernel_master->pml4_phys)[0];
}

struct vmm_space *vmm_kernel_master(void)
{
    return g_kernel_master;
}

/* --- map / unmap / translate / protect ------------------------------ */

int vmm_map(struct vmm_space *space, uint64_t virt, uint64_t phys,
            uint64_t length, uint32_t flags)
{
    struct create_log log;
    uint64_t pages, i;
    int rc;

    if (!g_backend_ready || space == NULL || space->pml4_phys == 0)
        return -YOUSJ_EINVAL;
    rc = validate_range(virt, length);
    if (rc != 0)
        return rc;
    if ((phys & (YOUSJ_PAGE_SIZE - 1)) != 0)
        return -YOUSJ_EINVAL;
    if (phys + length < phys)
        return -YOUSJ_EINVAL;

    pages = length / YOUSJ_PAGE_SIZE;

    /* Pre-check: any already-present leaf fails the whole call before
     * anything is modified (design sec. 5.2, -EEXIST). */
    for (i = 0; i < pages; i++) {
        if (walk_leaf(space, virt + i * YOUSJ_PAGE_SIZE) & YOUSJ_PTE_PRESENT)
            return -YOUSJ_EEXIST;
    }

    log.count = 0;
    for (i = 0; i < pages; i++) {
        uint64_t *slot = ensure_leaf_slot(space, virt + i * YOUSJ_PAGE_SIZE, &log);
        if (slot == NULL) {
            int err = (log.count >= VMM_CREATE_LOG_MAX) ? -YOUSJ_EINVAL : -YOUSJ_ENOMEM;
            rollback_map(space, virt, i, &log);
            return err;
        }
        *slot = vmm_pte_encode(phys + i * YOUSJ_PAGE_SIZE, flags);
    }
    return 0;
}

static bool free_empty_parents(struct vmm_space *space, uint64_t virt)
{
    /* After a leaf was cleared: if its PT is now empty, free it and
     * clear the PD entry; then likewise PD -> PDPT -> PML4. */
    uint64_t phys_path[3];   /* PDPT, PD, PT physical addresses */
    uint64_t *tables[4];     /* PML4, PDPT, PD, PT pointers */
    uint32_t idx[3];
    int level;

    tables[0] = table_of(space->pml4_phys);
    idx[0] = vmm_pml4_index(virt);
    idx[1] = vmm_pdpt_index(virt);
    idx[2] = vmm_pd_index(virt);

    {
        uint64_t entry = tables[0][idx[0]];
        if (!(entry & YOUSJ_PTE_PRESENT))
            return false;
        phys_path[0] = entry & YOUSJ_PTE_ADDR_MASK;
    }
    tables[1] = table_of(phys_path[0]);
    {
        uint64_t entry = tables[1][idx[1]];
        if (!(entry & YOUSJ_PTE_PRESENT))
            return false;
        phys_path[1] = entry & YOUSJ_PTE_ADDR_MASK;
    }
    tables[2] = table_of(phys_path[1]);
    {
        uint64_t entry = tables[2][idx[2]];
        if (!(entry & YOUSJ_PTE_PRESENT))
            return false;
        phys_path[2] = entry & YOUSJ_PTE_ADDR_MASK;
    }
    tables[3] = table_of(phys_path[2]);

    /* level: 3=PT in tables[3], parent slot tables[2][idx[2]], page phys_path[2] */
    for (level = 3; level >= 1; level--) {
        if (!table_all_empty(tables[level]))
            return true; /* something still lives here; stop */
        g_backend.free_page(phys_path[level - 1]);
        tables[level - 1][idx[level - 1]] = 0;
    }
    return true;
}

int vmm_unmap(struct vmm_space *space, uint64_t virt, uint64_t length)
{
    uint64_t pages, i;
    int rc;

    if (!g_backend_ready || space == NULL || space->pml4_phys == 0)
        return -YOUSJ_EINVAL;
    rc = validate_range(virt, length);
    if (rc != 0)
        return rc;

    pages = length / YOUSJ_PAGE_SIZE;
    for (i = 0; i < pages; i++) {
        uint64_t addr = virt + i * YOUSJ_PAGE_SIZE;
        uint64_t table_phys = space->pml4_phys;
        uint32_t idx[3];
        int level, present = 1;

        idx[0] = vmm_pml4_index(addr);
        idx[1] = vmm_pdpt_index(addr);
        idx[2] = vmm_pd_index(addr);
        for (level = 0; level < 3; level++) {
            uint64_t entry = table_of(table_phys)[idx[level]];
            if (!(entry & YOUSJ_PTE_PRESENT)) { present = 0; break; }
            table_phys = entry & YOUSJ_PTE_ADDR_MASK;
        }
        if (!present)
            continue; /* holes inside the range are allowed */
        table_of(table_phys)[vmm_pt_index(addr)] = 0;
        free_empty_parents(space, addr);
    }
    return 0;
}

int vmm_translate(struct vmm_space *space, uint64_t virt,
                  uint64_t *phys_out, uint32_t *flags_out)
{
    uint64_t leaf;
    if (!g_backend_ready || space == NULL || space->pml4_phys == 0)
        return -YOUSJ_EINVAL;
    leaf = walk_leaf(space, virt);
    if (!(leaf & YOUSJ_PTE_PRESENT))
        return -YOUSJ_ENOENT;
    if (phys_out != NULL)
        *phys_out = leaf & YOUSJ_PTE_ADDR_MASK;
    if (flags_out != NULL)
        *flags_out = vmm_pte_decode_flags(leaf);
    return 0;
}

int vmm_protect(struct vmm_space *space, uint64_t virt, uint64_t length,
                uint32_t flags)
{
    uint64_t pages, i;
    int rc;

    if (!g_backend_ready || space == NULL || space->pml4_phys == 0)
        return -YOUSJ_EINVAL;
    rc = validate_range(virt, length);
    if (rc != 0)
        return rc;

    pages = length / YOUSJ_PAGE_SIZE;
    /* All-or-nothing: verify every page is mapped first. */
    for (i = 0; i < pages; i++) {
        if (!(walk_leaf(space, virt + i * YOUSJ_PAGE_SIZE) & YOUSJ_PTE_PRESENT))
            return -YOUSJ_ENOENT;
    }
    for (i = 0; i < pages; i++) {
        uint64_t addr = virt + i * YOUSJ_PAGE_SIZE;
        uint64_t table_phys = space->pml4_phys;
        uint32_t idx[3];
        int level;
        idx[0] = vmm_pml4_index(addr);
        idx[1] = vmm_pdpt_index(addr);
        idx[2] = vmm_pd_index(addr);
        for (level = 0; level < 3; level++)
            table_phys = table_of(table_phys)[idx[level]] & YOUSJ_PTE_ADDR_MASK;
        {
            uint64_t *slot = &table_of(table_phys)[vmm_pt_index(addr)];
            *slot = vmm_pte_encode(*slot & YOUSJ_PTE_ADDR_MASK, flags);
        }
    }
    return 0;
}

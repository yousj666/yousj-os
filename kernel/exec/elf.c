/* Yousj OS - static ELF64 loader. See elf.h for the contract.
 * Written by Muse (coordinator).
 */
#include <yousj/types.h>
#include <yousj/elf.h>
#include <yousj/vmm.h>
#include <yousj/pmm.h>
#include <yousj/user.h>

/* ELF64 structures (the format's fixed layouts). */
struct elf64_ehdr {
    uint8_t  ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t phoff;
    uint64_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
} __attribute__((packed));

struct elf64_phdr {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t vaddr;
    uint64_t paddr;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t align;
} __attribute__((packed));

#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define ET_EXEC    2
#define EM_X86_64  62
#define PT_LOAD    1
#define PF_X 1
#define PF_W 2
#define PF_R 4

static uint32_t seg_vmm_flags(const struct elf64_phdr *ph)
{
    uint32_t f = YOUSJ_VMM_USER;
    if (ph->flags & PF_R)
        f |= YOUSJ_VMM_READ;
    if (ph->flags & PF_W)
        f |= YOUSJ_VMM_WRITE;
    if (ph->flags & PF_X)
        f |= YOUSJ_VMM_EXEC;
    return f;
}

int elf_load_user_image(const uint8_t *image, uint64_t size,
                        struct vmm_space *space,
                        uint64_t *entry_out, uint64_t *brk_out)
{
    const struct elf64_ehdr *eh;
    uint64_t brk = 0;
    uint32_t i;

    if (image == NULL || space == NULL || size < sizeof(*eh))
        return -1;
    eh = (const struct elf64_ehdr *)image;
    if (eh->ident[0] != 0x7F || eh->ident[1] != 'E' ||
        eh->ident[2] != 'L' || eh->ident[3] != 'F' ||
        eh->ident[4] != ELFCLASS64 || eh->ident[5] != ELFDATA2LSB ||
        eh->type != ET_EXEC || eh->machine != EM_X86_64 ||
        eh->phentsize != sizeof(struct elf64_phdr) || eh->phnum == 0 ||
        eh->phoff > size ||
        (uint64_t)eh->phnum * sizeof(struct elf64_phdr) > size - eh->phoff)
        return -1;
    if (eh->entry < YOUSJ_USER_BASE || eh->entry >= YOUSJ_USER_LIMIT)
        return -1;

    for (i = 0; i < eh->phnum; i++) {
        const struct elf64_phdr *ph = (const struct elf64_phdr *)
            (image + eh->phoff + (uint64_t)i * sizeof(*ph));
        uint64_t seg_start, seg_end, page;
        uint32_t want;

        if (ph->type != PT_LOAD)
            continue;
        if (ph->memsz == 0)
            continue; /* empty segment (e.g. an all-bss data
                       * segment with no content in this image) */
        if (ph->memsz < ph->filesz)
            return -1;
        if (ph->offset > size || ph->filesz > size - ph->offset)
            return -1;
        seg_start = ph->vaddr;
        seg_end = ph->vaddr + ph->memsz;
        /* A PT_LOAD lying entirely below the window is the ELF
         * headers coverage segment GNU ld adds to some images;
         * nothing in the image references it (entry and all
         * sections live in the window), so it is skipped. A
         * segment straddling the base still fails below. */
        if (seg_end <= YOUSJ_USER_BASE)
            continue;
        if (seg_start < YOUSJ_USER_BASE || seg_end > YOUSJ_USER_LIMIT ||
            seg_end < seg_start)
            return -1;
        want = seg_vmm_flags(ph);

        for (page = seg_start & ~(YOUSJ_PAGE_SIZE - 1);
             page < seg_end; page += YOUSJ_PAGE_SIZE) {
            uint64_t frame = 0;
            uint32_t existing_flags = 0;
            uint64_t copy_lo, copy_hi, j;

            if (vmm_translate(space, page, &frame, &existing_flags) == 0) {
                /* Page already present (shared with a previous
                 * segment): widen permissions to the union; the
                 * bytes below simply overwrite the overlap. */
                uint32_t uni = existing_flags | want;
                if (uni != existing_flags &&
                    vmm_protect(space, page, YOUSJ_PAGE_SIZE, uni) != 0)
                    return -1;
            } else {
                uint64_t k;
                frame = pmm_alloc_page();
                if (frame == 0)
                    return -1;
                for (k = 0; k < YOUSJ_PAGE_SIZE; k++)
                    ((uint8_t *)(uintptr_t)frame)[k] = 0;
                if (vmm_map(space, page, frame, YOUSJ_PAGE_SIZE,
                            want) != 0) {
                    (void)pmm_free_page(frame);
                    return -1;
                }
            }
            /* Copy the intersection of this page with the
             * segment's file-backed part [vaddr, vaddr+filesz). */
            copy_lo = page;
            if (copy_lo < ph->vaddr)
                copy_lo = ph->vaddr;
            copy_hi = page + YOUSJ_PAGE_SIZE;
            if (copy_hi > ph->vaddr + ph->filesz)
                copy_hi = ph->vaddr + ph->filesz;
            for (j = copy_lo; j < copy_hi; j++)
                ((uint8_t *)(uintptr_t)frame)[j - page] =
                    image[ph->offset + (j - ph->vaddr)];
        }
        if (seg_end > brk)
            brk = seg_end;
    }

    if (brk == 0)
        return -1; /* no PT_LOAD at all */
    *entry_out = eh->entry;
    *brk_out = brk;
    return 0;
}

/* --- initial stack ------------------------------------------------------ */

#define YOUSJ_AT_NULL   0
#define YOUSJ_AT_PAGESZ 6
#define YOUSJ_STACK_PAGES 4

int elf_build_initial_stack(struct vmm_space *space, uint64_t stack_top,
                            const char *const *argv,
                            const char *const *envp,
                            uint64_t *sp_out)
{
    uint64_t top_frame = pmm_alloc_page();
    uint8_t *page;
    uint64_t str_off = YOUSJ_PAGE_SIZE; /* strings grow down from here */
    uint64_t argc = 0, envc = 0, i, block_qwords, block_off;
    uint64_t *block;
    uint64_t argv_ptrs[16];
    uint64_t envp_ptrs[16];

    if (top_frame == 0)
        return -1;
    page = (uint8_t *)(uintptr_t)top_frame;
    for (i = 0; i < YOUSJ_PAGE_SIZE; i++)
        page[i] = 0;

    if (argv != NULL)
        while (argv[argc] != NULL && argc < 16)
            argc++;
    if (envp != NULL)
        while (envp[envc] != NULL && envc < 16)
            envc++;

    /* Lay the strings down from the top of the page (argv first,
     * then envp), recording each string's user VA. */
    for (i = 0; i < argc + envc; i++) {
        const char *str = i < argc ? argv[i] : envp[i - argc];
        uint64_t len = 0, j;
        while (str[len] != '\0')
            len++;
        if (str_off < len + 1 + 512)
            return -1; /* keep strings + block inside the top page */
        str_off -= len + 1;
        for (j = 0; j <= len; j++)
            page[str_off + j] = (uint8_t)str[j];
        if (i < argc)
            argv_ptrs[i] = stack_top - YOUSJ_PAGE_SIZE + str_off;
        else
            envp_ptrs[i - argc] = stack_top - YOUSJ_PAGE_SIZE + str_off;
    }

    /* The block sits below the strings, 16-byte aligned. */
    block_qwords = 1 + argc + 1 + envc + 1 + 4;
    block_off = (str_off & ~15ULL) - block_qwords * 8;
    block = (uint64_t *)(uintptr_t)(top_frame + block_off);
    i = 0;
    block[i++] = argc;
    {
        uint64_t k;
        for (k = 0; k < argc; k++)
            block[i++] = argv_ptrs[k];
        block[i++] = 0;
        for (k = 0; k < envc; k++)
            block[i++] = envp_ptrs[k];
        block[i++] = 0;
    }
    block[i++] = YOUSJ_AT_PAGESZ;
    block[i++] = YOUSJ_PAGE_SIZE;
    block[i++] = YOUSJ_AT_NULL;
    block[i++] = 0;

    if (vmm_map(space, stack_top - YOUSJ_PAGE_SIZE, top_frame,
                YOUSJ_PAGE_SIZE,
                YOUSJ_VMM_READ | YOUSJ_VMM_WRITE | YOUSJ_VMM_USER) != 0)
        return -1;
    {
        uint64_t p;
        for (p = 1; p < YOUSJ_STACK_PAGES; p++) {
            uint64_t frame = pmm_alloc_page();
            uint64_t j;
            if (frame == 0)
                return -1;
            for (j = 0; j < YOUSJ_PAGE_SIZE; j++)
                ((uint8_t *)(uintptr_t)frame)[j] = 0;
            if (vmm_map(space,
                        stack_top - (p + 1) * YOUSJ_PAGE_SIZE,
                        frame, YOUSJ_PAGE_SIZE,
                        YOUSJ_VMM_READ | YOUSJ_VMM_WRITE |
                        YOUSJ_VMM_USER) != 0)
                return -1;
        }
    }
    *sp_out = stack_top - YOUSJ_PAGE_SIZE + block_off;
    return 0;
}

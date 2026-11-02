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
        if (ph->memsz < ph->filesz || ph->memsz == 0)
            return -1;
        if (ph->offset > size || ph->filesz > size - ph->offset)
            return -1;
        seg_start = ph->vaddr;
        seg_end = ph->vaddr + ph->memsz;
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

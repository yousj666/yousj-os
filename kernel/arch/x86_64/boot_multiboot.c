/* Yousj OS - Multiboot1 info -> struct yousj_boot_info conversion.
 * Written by Muse (coordinator). Bring-up only: QEMU's -kernel
 * loader hands us a Multiboot1 info block; the product bootloader
 * (UEFI, see bootloader/) will fill the same struct from the UEFI
 * memory map instead. Keeping the conversion behind the boot.h
 * contract means kmain never knows which path produced its map.
 *
 * Multiboot1 info offsets used here (from the Multiboot spec):
 *   +0  flags (bit 6 = mmap_* fields valid)
 *   +44 mmap_length, +48 mmap_addr
 * Each mmap record: u32 size (excluding itself), u64 base, u64
 * length, u32 type (1 = available RAM); stride is size + 4.
 */
#include <yousj/types.h>
#include <yousj/boot.h>

/* This file is compiled -m32 into the stub while the kernel reads
 * the struct as -m64 code. Every u64 field of boot.h happens to land
 * on an 8-byte boundary under both ABIs; pin the total size so a
 * future edit to boot.h that breaks that property fails loudly
 * here instead of corrupting the hand-off silently. */
_Static_assert(sizeof(struct yousj_boot_info) == 3376,
               "yousj_boot_info layout diverged between 32/64-bit builds");

static void mb_memset(void *dst, uint8_t value, uint64_t count)
{
    uint8_t *p = (uint8_t *)dst;
    while (count--)
        *p++ = value;
}

static uint32_t rd32(uint64_t addr) { return *(const volatile uint32_t *)(uintptr_t)addr; }
static uint64_t rd64(uint64_t addr) { return *(const volatile uint64_t *)(uintptr_t)addr; }

void mb1_build_boot_info(uint64_t mb_info_addr, struct yousj_boot_info *out)
{
    uint32_t flags;
    uint64_t i;

    mb_memset(out, 0, sizeof(*out));
    out->magic = YOUSJ_BOOT_MAGIC;
    out->version = 1;
    out->mem_entry_count = 0;

    if (mb_info_addr == 0)
        return;
    flags = rd32(mb_info_addr);
    if (!(flags & (1u << 6)))
        return; /* no memory map: leave the count at 0, kmain will say so via PMM totals */

    {
        uint64_t mmap_length = rd32(mb_info_addr + 44);
        uint64_t p = rd32(mb_info_addr + 48);
        uint64_t end = p + mmap_length;

        while (p + 24 <= end && out->mem_entry_count < YOUSJ_MAX_MEM_ENTRIES) {
            uint32_t record_size = rd32(p);
            uint64_t base = rd64(p + 4);
            uint64_t length = rd64(p + 12);
            uint32_t type = rd32(p + 20);
            struct yousj_mem_entry *e;

            if (record_size < 20)
                break; /* corrupt map: stop instead of wandering off */
            if (length == 0) {
                p += record_size + 4;
                continue;
            }
            e = &out->mem[out->mem_entry_count++];
            e->base = base;
            e->length = length;
            e->type = (type == 1) ? YOUSJ_MEM_USABLE : YOUSJ_MEM_RESERVED;
            e->reserved = 0;
            p += record_size + 4;
        }
    }

    /* The kernel image itself (loaded at 1 MiB by the -kernel loader)
     * sits inside a Multiboot "available" region. Protect the first
     * 16 MiB so the PMM can never hand out the kernel, the early page
     * tables, or the boot stack. The UEFI path gets this protection
     * from its own map marking (bootloader/boot_info.c); this crude
     * range is acceptable here because this file is smoke-test only
     * and QEMU's default RAM layout is known. Entries overlapping the
     * protected range are trimmed, not split: the usable region above
     * 1 MiB in QEMU's map is a single record starting at 1 MiB, and
     * trimming its head is exactly right for it. */
    for (i = 0; i < out->mem_entry_count; i++) {
        struct yousj_mem_entry *e = &out->mem[i];
        uint64_t e_end = e->base + e->length;
        const uint64_t prot_end = 16ULL * 1024 * 1024;
        if (e->type != YOUSJ_MEM_USABLE)
            continue;
        if (e->base < prot_end && e_end > e->base) {
            if (e_end <= prot_end) {
                e->type = YOUSJ_MEM_RESERVED;
            } else {
                e->length = e_end - prot_end;
                e->base = prot_end;
            }
        }
    }
}

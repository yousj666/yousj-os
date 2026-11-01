/* Yousj OS - 32-bit Multiboot stub logic (QEMU smoke boot only).
 * Written by Muse (coordinator).
 *
 * stub_main() does three things, all in 32-bit protected mode:
 *   1. finds the first Multiboot module (= the kernel ELF64 image
 *      passed with QEMU's -initrd) and loads its PT_LOAD segments
 *      to their physical addresses, zeroing each segment's BSS tail;
 *   2. builds the kernel's struct yousj_boot_info in this stub's own
 *      BSS via the shared mb1_build_boot_info() (compiled -m32 here;
 *      the layout matches the 64-bit side by construction -- every
 *      u64 field of boot.h lands on an 8-byte boundary, and the size
 *      is asserted in boot_multiboot.c);
 *   3. publishes the buffer's physical address in g_boot_info_phys
 *      and returns the kernel's ELF entry address, or 0 on failure
 *      (stub.S then hangs instead of jumping into garbage).
 */
#include <yousj/types.h>
#include <yousj/boot.h>

void mb1_build_boot_info(uint64_t mb_info_addr, struct yousj_boot_info *out);

static struct yousj_boot_info g_boot_info;
uint32_t g_boot_info_phys;

static uint32_t rd32(uint32_t addr) { return *(const volatile uint32_t *)(uintptr_t)addr; }

static void mem_copy(uint8_t *dst, const uint8_t *src, uint64_t n)
{
    while (n--)
        *dst++ = *src++;
}

static void mem_zero(uint8_t *dst, uint64_t n)
{
    while (n--)
        *dst++ = 0;
}

/* ELF64 constants (architectural format definitions). */
#define ELF_PT_LOAD 1u

uint32_t stub_main(uint32_t mb_info)
{
    uint32_t flags, mods_count, mods_addr;
    uint32_t mod_start, mod_end;
    uint64_t phoff, entry;
    uint32_t phentsize, phnum, i;

    if (mb_info == 0)
        return 0;
    flags = rd32(mb_info);
    if (!(flags & (1u << 3)))
        return 0; /* no modules loaded */
    mods_count = rd32(mb_info + 20);
    mods_addr = rd32(mb_info + 24);
    if (mods_count == 0)
        return 0;
    mod_start = rd32(mods_addr);
    mod_end = rd32(mods_addr + 4);
    if (mod_end <= mod_start || mod_end - mod_start < 64)
        return 0;

    /* ELF magic check: 0x7F 'E' 'L' 'F', 64-bit class at offset 4. */
    if (rd32(mod_start) != 0x464C457Fu)
        return 0;
    if (((const uint8_t *)(uintptr_t)mod_start)[4] != 2)
        return 0;

    entry = *(const uint64_t *)(uintptr_t)(mod_start + 24);
    phoff = *(const uint64_t *)(uintptr_t)(mod_start + 32);
    phentsize = *(const uint16_t *)(uintptr_t)(mod_start + 54);
    phnum = *(const uint16_t *)(uintptr_t)(mod_start + 56);

    for (i = 0; i < phnum; i++) {
        uint32_t ph = mod_start + (uint32_t)phoff + i * phentsize;
        uint32_t type = *(const uint32_t *)(uintptr_t)ph;
        uint64_t offset, paddr, filesz, memsz;
        if (type != ELF_PT_LOAD)
            continue;
        offset = *(const uint64_t *)(uintptr_t)(ph + 8);
        paddr  = *(const uint64_t *)(uintptr_t)(ph + 24);
        filesz = *(const uint64_t *)(uintptr_t)(ph + 32);
        memsz  = *(const uint64_t *)(uintptr_t)(ph + 40);
        mem_copy((uint8_t *)(uintptr_t)paddr,
                 (const uint8_t *)(uintptr_t)(mod_start + (uint32_t)offset),
                 filesz);
        if (memsz > filesz)
            mem_zero((uint8_t *)(uintptr_t)(paddr + filesz), memsz - filesz);
    }

    mb1_build_boot_info(mb_info, &g_boot_info);
    g_boot_info_phys = (uint32_t)(uintptr_t)&g_boot_info;
    return (uint32_t)entry;
}

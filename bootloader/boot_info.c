/* Yousj OS - bootloader-side boot_info construction.
 *
 * Written from scratch. Two halves live in this file:
 *
 *   1. Pure conversion (everything down to yousj_boot_info_set_cmdline):
 *      no firmware calls, no globals, no allocation. Data in, struct
 *      out. This half is complete and can be read and tested on its
 *      own; it turns an abstract firmware memory map into the
 *      struct yousj_boot_info the kernel expects (see <yousj/boot.h>).
 *
 *   2. Firmware entry (yousj_bootloader_entry at the bottom): a TODO
 *      skeleton only. The UEFI calls it needs do not exist yet, so
 *      it returns -1 and must not be mistaken for a working
 *      bootloader. See bootloader/DESIGN.md section 3 for the full
 *      step list and section 7 for the TODO list.
 */
#include "boot_info.h"

/* No libc in the bootloader: zero memory with a plain loop. */
static void zero_bytes(void *dst, uint64_t n)
{
    uint8_t *p = (uint8_t *)dst;
    uint64_t i;
    for (i = 0; i < n; i++)
        p[i] = 0;
}

uint32_t yousj_boot_map_uefi_type(uint32_t uefi_type)
{
    /* Numeric values are the UEFI MemoryType enumeration (UEFI spec,
     * "Memory Types" / EFI_MEMORY_DESCRIPTOR). Only the class
     * decision lives here; DESIGN.md section 5.2 gives the reasons. */
    switch (uefi_type) {
    case 7:  /* EfiConventionalMemory */
        return YOUSJ_MEM_USABLE;
    case 1:  /* EfiLoaderCode */
    case 2:  /* EfiLoaderData */
    case 3:  /* EfiBootServicesCode */
    case 4:  /* EfiBootServicesData */
        /* Reclaimable in principle after ExitBootServices, but the
         * loader image, the boot_info struct and the kernel stack
         * live in loader pages; stage 1 does not reclaim any of it. */
        return YOUSJ_MEM_BOOTLOADER;
    case 9:  /* EfiACPIReclaimMemory */
    case 10: /* EfiACPIMemoryNVS */
        return YOUSJ_MEM_ACPI;
    case 11: /* EfiMemoryMappedIO */
    case 12: /* EfiMemoryMappedIOPortSpace */
        return YOUSJ_MEM_MMIO;
    case 0:  /* EfiReservedMemoryType */
    case 5:  /* EfiRuntimeServicesCode */
    case 6:  /* EfiRuntimeServicesData */
    case 8:  /* EfiUnusableMemory */
    case 13: /* EfiPalCode */
    case 14: /* EfiPersistentMemory */
    default: /* OEM/OS-defined ranges (>= 0x80000000) and the unknown */
        return YOUSJ_MEM_RESERVED;
    }
}

/* Append one converted entry, merging it into the previous entry
 * when both share a type and are exactly adjacent (the caller feeds
 * entries in ascending base order, so the previous entry is the
 * only possible merge partner). Returns 0, or -2 when the entry
 * array is full. */
static int append_entry(struct yousj_boot_info *info,
                        uint64_t base, uint64_t length, uint32_t type)
{
    struct yousj_mem_entry *last;

    if (length == 0)
        return 0;

    if (info->mem_entry_count > 0) {
        last = &info->mem[info->mem_entry_count - 1];
        if (last->type == type && last->base + last->length == base) {
            last->length += length;
            return 0;
        }
    }

    if (info->mem_entry_count >= YOUSJ_MAX_MEM_ENTRIES)
        return -2;

    last = &info->mem[info->mem_entry_count];
    last->base = base;
    last->length = length;
    last->type = type;
    last->reserved = 0;
    info->mem_entry_count++;
    return 0;
}

/* Ensure no entry crosses `addr`: if an entry strictly contains it,
 * split that entry into two entries of the same type at `addr`.
 * Returns 0 (including "nothing to split"), or -2 when the split
 * would exceed YOUSJ_MAX_MEM_ENTRIES. */
static int split_at(struct yousj_boot_info *info, uint64_t addr)
{
    uint64_t i, j;

    for (i = 0; i < info->mem_entry_count; i++) {
        struct yousj_mem_entry *e = &info->mem[i];
        uint64_t head_len;

        /* `addr - e->base < e->length` is "addr strictly inside"
         * written so it stays correct even for an entry whose
         * base + length would wrap the address space. */
        if (e->base >= addr || addr - e->base >= e->length)
            continue;

        if (info->mem_entry_count >= YOUSJ_MAX_MEM_ENTRIES)
            return -2;

        /* Shift the tail one slot right and carve the entry in two. */
        for (j = info->mem_entry_count; j > i + 1; j--)
            info->mem[j] = info->mem[j - 1];
        head_len = addr - e->base;
        info->mem[i + 1].base = addr;
        info->mem[i + 1].length = e->length - head_len;
        info->mem[i + 1].type = e->type;
        info->mem[i + 1].reserved = 0;
        e->length = head_len;
        info->mem_entry_count++;
        return 0;
    }
    return 0;
}

int yousj_boot_info_mark_range(struct yousj_boot_info *info,
                               uint64_t base, uint64_t length,
                               uint32_t yousj_type)
{
    uint64_t i;
    int rc;

    if (info == NULL)
        return -1;
    if (length == 0)
        return 0;

    /* Clip a range that would wrap the top of the address space;
     * it then runs "to the top" and needs no split at its end. */
    if (base + length < base)
        length = 0 - base;

    rc = split_at(info, base);
    if (rc != 0)
        return rc;
    if (base + length != 0) { /* 0 here means "clipped to the top" */
        rc = split_at(info, base + length);
        if (rc != 0)
            return rc;
    }

    /* After the two splits, every entry is either fully inside or
     * fully outside the range; retype the inside ones. Adjacent
     * entries that end up sharing a type are deliberately not
     * re-merged: it costs a few entry slots, which the -2 return
     * accounts for, and keeps this function obviously correct. */
    for (i = 0; i < info->mem_entry_count; i++) {
        struct yousj_mem_entry *e = &info->mem[i];
        if (e->base >= base && e->base - base < length)
            e->type = yousj_type;
    }
    return 0;
}

int yousj_boot_info_build(struct yousj_boot_info *info,
                          const struct yousj_fw_mem_entry *fw_entries,
                          uint64_t fw_count,
                          uint64_t kernel_base,
                          uint64_t kernel_length)
{
    uint64_t i;
    int rc;

    if (info == NULL || (fw_entries == NULL && fw_count > 0))
        return -1;

    zero_bytes(info, sizeof(*info));
    info->magic = YOUSJ_BOOT_MAGIC;
    info->version = 1;
    info->mem_entry_count = 0;
    /* fb and rsdp_addr stay zero: the server build sets no graphics
     * mode, and the firmware side fills rsdp_addr separately. */

    for (i = 0; i < fw_count; i++) {
        uint64_t base = fw_entries[i].base;
        uint64_t length = fw_entries[i].length;
        uint32_t type = yousj_boot_map_uefi_type(fw_entries[i].uefi_type);

        if (length == 0)
            continue;
        if (base + length < base) {
            /* A firmware entry wrapping the address space is a
             * broken (or hostile) map: clip it to the top and never
             * let any part of it become USABLE. */
            length = 0 - base;
            type = YOUSJ_MEM_RESERVED;
        }
        rc = append_entry(info, base, length, type);
        if (rc != 0)
            return rc;
    }

    /* The boot contract's hard rule: the physical range holding the
     * kernel image must be non-USABLE in the map, or the PMM will
     * hand kernel pages out as free memory. Mark it explicitly even
     * though ranges the map never called USABLE are already safe. */
    if (kernel_length > 0) {
        rc = yousj_boot_info_mark_range(info, kernel_base, kernel_length,
                                        YOUSJ_MEM_RESERVED);
        if (rc != 0)
            return rc;
    }
    return 0;
}

void yousj_boot_info_set_cmdline(struct yousj_boot_info *info,
                                 const char *cmdline)
{
    uint64_t i;

    if (info == NULL)
        return;
    for (i = 0; i < YOUSJ_CMDLINE_MAX - 1; i++) {
        if (cmdline == NULL || cmdline[i] == '\0')
            break;
        info->cmdline[i] = cmdline[i];
    }
    info->cmdline[i] = '\0';
}

/* ------------------------------------------------------------------ */
/* Firmware side: TODO skeleton. Nothing below this line can boot.    */
/* ------------------------------------------------------------------ */

int yousj_bootloader_entry(void *image_handle, void *system_table)
{
    (void)image_handle;
    (void)system_table;

    /* TODO (DESIGN.md sections 3 and 7, TODO-1..TODO-5): implement
     * the UEFI application flow, in this exact order:
     *
     *   1. Save ImageHandle / SystemTable; log one line via ConOut
     *      (console output dies at ExitBootServices).
     *   2. Open the ESP via LoadedImage + SimpleFileSystem, read the
     *      kernel image file fully into a pool buffer.
     *   3. AllocatePages for the kernel image; place it per its
     *      ELF64 PT_LOAD program headers (p_paddr, zero the
     *      p_memsz tail) -- minimal ELF loader, TODO-3. Remember
     *      [kernel_base, kernel_end).
     *   4. AllocatePages for the boot_info struct itself and for a
     *      kernel stack; remember both ranges.
     *   5. Walk the UEFI configuration table for the ACPI 2.0/1.0
     *      GUIDs -> boot_info.rsdp_addr (0 when absent).
     *   6. Convert LoadOptions (UTF-16) to ASCII and call
     *      yousj_boot_info_set_cmdline().
     *   7. GetMemoryMap, two-call sizing dance, striding by the
     *      firmware's DescriptorSize (never sizeof), requiring
     *      DescriptorVersion == 1. Translate descriptors into
     *      struct yousj_fw_mem_entry[] (page count -> bytes).
     *      TODO-5: sort by base if a firmware ever returns them
     *      out of order.
     *   8. yousj_boot_info_build(&info, fw, n, kernel_base,
     *      kernel_end - kernel_base), then
     *      yousj_boot_info_mark_range(..., YOUSJ_MEM_BOOTLOADER)
     *      for the boot_info and stack ranges from step 4.
     *      Any failure here: print via ConOut and halt -- do not
     *      jump with a map that might free the kernel's own pages.
     *   9. ExitBootServices with the step-7 MapKey. On
     *      INVALID_PARAMETER, redo steps 7-8 and retry exactly
     *      once (TODO-4). After a successful exit: no firmware
     *      calls, no allocation, no ConOut, ever again.
     *  10. cli; load RSP with the step-4 stack top (16-byte
     *      aligned); load RDI with the boot_info address; jmp to
     *      the kernel entry (kmain, per <yousj/boot.h>).
     *
     * Until those steps exist, report "not implemented" honestly. */
    return -1;
}

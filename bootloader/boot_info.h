/* Yousj OS - bootloader-side boot_info construction (interface).
 *
 * Written from scratch. This header is private to the bootloader; it
 * is NOT a kernel interface. The only contract with the kernel stays
 * <yousj/boot.h>: the struct defined there is what this code fills.
 *
 * The functions split into two groups:
 *
 *   Pure conversion (implemented in boot_info.c, no firmware calls,
 *   no globals, independently readable and host-testable):
 *     yousj_boot_map_uefi_type()
 *     yousj_boot_info_build()
 *     yousj_boot_info_mark_range()
 *     yousj_boot_info_set_cmdline()
 *
 *   Firmware entry (declared here, body still a TODO skeleton in
 *   boot_info.c -- it must NOT be treated as a working bootloader):
 *     yousj_bootloader_entry()
 */
#ifndef YOUSJ_BOOTLOADER_BOOT_INFO_H
#define YOUSJ_BOOTLOADER_BOOT_INFO_H

#include <yousj/types.h>
#include <yousj/boot.h>

/* One firmware memory-map entry, reduced to the three fields the
 * conversion needs. The UEFI-specific walk (descriptor stride,
 * version check, page-count-to-bytes expansion) happens on the
 * firmware side and produces an array of these; everything from
 * here on is firmware-agnostic.
 *
 * `uefi_type` is the raw UEFI MemoryType value (UINT32 in the UEFI
 * spec), kept uninterpreted so the mapping table lives in exactly
 * one place: yousj_boot_map_uefi_type().
 *
 * Precondition for the conversion: entries are sorted by ascending
 * `base` (UEFI memory maps are in practice; see the TODO about
 * sorting in boot_info.c). Overlapping entries are not expected;
 * adjacent entries are merged when they share a type. */
struct yousj_fw_mem_entry {
    uint64_t base;
    uint64_t length;    /* in bytes (UEFI page counts already expanded) */
    uint32_t uefi_type; /* raw UEFI MemoryType */
    uint32_t reserved;
};

/* Map a raw UEFI MemoryType value to a yousj_mem_type value.
 *
 * Policy (see bootloader/DESIGN.md section 5.2): only
 * EfiConventionalMemory (7) becomes YOUSJ_MEM_USABLE. Every other
 * known type maps to its closest non-usable class, and every
 * unknown value maps to YOUSJ_MEM_RESERVED -- fail closed, because
 * the kernel PMM hands out exactly the ranges marked USABLE and
 * nothing else. */
uint32_t yousj_boot_map_uefi_type(uint32_t uefi_type);

/* Build a complete struct yousj_boot_info from a firmware map.
 *
 * Steps: zero the struct, stamp magic + version, convert and merge
 * the firmware entries, then -- if kernel_length > 0 -- force the
 * kernel image range [kernel_base, kernel_base + kernel_length) to
 * YOUSJ_MEM_RESERVED via yousj_boot_info_mark_range(). The kernel
 * range must never be handed out by the PMM, and the boot contract
 * makes that the bootloader's responsibility (see pmm.h).
 *
 * framebuffer and rsdp_addr are left zero here; the firmware side
 * fills rsdp_addr from the UEFI configuration table (and, in a
 * later desktop stage, fb from GOP) before jumping to the kernel.
 * cmdline is filled via yousj_boot_info_set_cmdline().
 *
 * Returns 0 on success, -1 on a NULL argument, -2 if the converted
 * map does not fit in YOUSJ_MAX_MEM_ENTRIES entries. On -2 the
 * caller must halt, not jump: a truncated map could drop the very
 * RESERVED entry that protects the kernel image. */
int yousj_boot_info_build(struct yousj_boot_info *info,
                          const struct yousj_fw_mem_entry *fw_entries,
                          uint64_t fw_count,
                          uint64_t kernel_base,
                          uint64_t kernel_length);

/* Retype the part of the described memory map that intersects
 * [base, base + length) to `yousj_type` (a yousj_mem_type value).
 *
 * Entries straddling a range boundary are split, so afterwards no
 * entry crosses either boundary and every entry fully inside the
 * range carries the new type. Entries outside the map's described
 * ranges are not invented -- memory the map never called USABLE is
 * already safe with the PMM; this function exists so the map states
 * the protection explicitly (and so PMM statistics see the true
 * top of memory).
 *
 * Returns 0 on success (including "range intersects nothing"),
 * -1 on a NULL info, -2 if splitting would exceed
 * YOUSJ_MAX_MEM_ENTRIES. */
int yousj_boot_info_mark_range(struct yousj_boot_info *info,
                               uint64_t base, uint64_t length,
                               uint32_t yousj_type);

/* Copy an ASCII command line into info->cmdline, truncating to
 * YOUSJ_CMDLINE_MAX - 1 characters and always NUL-terminating.
 * A NULL cmdline produces an empty string. (The UTF-16 LoadOptions
 * string the firmware actually hands over is converted to ASCII on
 * the firmware side before calling this.) */
void yousj_boot_info_set_cmdline(struct yousj_boot_info *info,
                                 const char *cmdline);

/* Bootloader firmware entry point -- TODO, NOT YET IMPLEMENTED.
 *
 * The real signature will take the UEFI ImageHandle and
 * EFI_SYSTEM_TABLE pointer once the bootloader adopts UEFI types
 * (DESIGN.md TODO-1); void* keeps this skeleton free of any UEFI
 * header dependency. The current body in boot_info.c only lists the
 * required steps and returns -1. It cannot boot anything. */
int yousj_bootloader_entry(void *image_handle, void *system_table);

#endif /* YOUSJ_BOOTLOADER_BOOT_INFO_H */

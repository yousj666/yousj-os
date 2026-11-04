/* Yousj OS - boot information contract.
 *
 * This header is THE interface between the bootloader (Claude Code's area)
 * and the kernel core (Muse's area). The bootloader fills one
 * struct yousj_boot_info, puts its address in RDI (first SysV argument),
 * and jumps to kmain(). Neither side may change this layout without
 * coordinator approval; Bootloader and kernel are built separately and
 * only agree through this struct.
 *
 * Written from scratch. Magic value chosen for Yousj: ASCII "YOUSJOS1"
 * read as a little-endian u64 is not used; we use a plain u32 tag below.
 */
#ifndef YOUSJ_BOOT_H
#define YOUSJ_BOOT_H

#include <yousj/types.h>

#define YOUSJ_BOOT_MAGIC 0x594F5553u /* "YOUS" */

enum yousj_mem_type {
    YOUSJ_MEM_USABLE   = 1,
    YOUSJ_MEM_RESERVED = 2,
    YOUSJ_MEM_ACPI     = 3,
    YOUSJ_MEM_MMIO     = 4,
    YOUSJ_MEM_BOOTLOADER = 5,
};

struct yousj_mem_entry {
    uint64_t base;
    uint64_t length;
    uint32_t type;      /* enum yousj_mem_type */
    uint32_t reserved;
};

struct yousj_framebuffer {
    uint64_t addr;      /* physical address of the linear framebuffer */
    uint32_t width;
    uint32_t height;
    uint32_t pitch;     /* bytes per scanline */
    uint8_t  bpp;
    /* reserved[0] (M81): pixel channel order for 32bpp --
     * 0 = lowest byte is red (UEFI pixel format 0, RGBx),
     * 1 = lowest byte is blue (format 1, BGRx). */
    uint8_t  reserved[3];
};

#define YOUSJ_MAX_MEM_ENTRIES 128
#define YOUSJ_CMDLINE_MAX     256

struct yousj_boot_info {
    uint32_t magic;                 /* YOUSJ_BOOT_MAGIC */
    uint32_t version;               /* struct version, currently 1 */
    uint64_t mem_entry_count;
    struct yousj_mem_entry mem[YOUSJ_MAX_MEM_ENTRIES];
    struct yousj_framebuffer fb;    /* all zero if no graphics mode set */
    uint64_t rsdp_addr;             /* ACPI RSDP physical address, 0 if none */
    char     cmdline[YOUSJ_CMDLINE_MAX];
};

#endif /* YOUSJ_BOOT_H */

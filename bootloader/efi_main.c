/* Yousj OS - UEFI bootloader: real implementation.
 *
 * Written from scratch (types in uefi.h are the spec's published
 * layouts; no gnu-efi, no external code). This is the program the
 * firmware loads as \EFI\YOUSJ\BOOTX64.EFI. What it does, in order:
 *
 *   1. Opens the ESP it was loaded from and reads the kernel image
 *      (\EFI\YOUSJ\YOUSJ.KRN, falling back to \YOUSJ.KRN).
 *   2. Loads the kernel's ELF64 PT_LOAD segments to their physical
 *      addresses (claiming those exact pages from the firmware
 *      first, so nothing else can be handed the same pages), and
 *      finds the kernel's UEFI entry symbol to jump to.
 *   3. Allocates pages for struct yousj_boot_info and a kernel
 *      stack, finds the ACPI RSDP, converts LoadOptions to an
 *      ASCII command line.
 *   4. Builds the hand-off page tables (see build_handoff_tables):
 *      a 1 GiB identity map for the loader's own world plus the
 *      kernel image mapped at its higher-half link base.
 *   5. Fetches the firmware memory map (two-call sizing), converts
 *      it with the pure functions in boot_info.c -- which force the
 *      kernel image range to YOUSJ_MEM_RESERVED -- and marks the
 *      boot_info / stack / loader-image ranges YOUSJ_MEM_BOOTLOADER.
 *   6. ExitBootServices (one full retry with a fresh map on
 *      INVALID_PARAMETER, exactly as the spec's dance requires).
 *   7. cli, CR3 = hand-off tables, RDI = boot_info, jmp to the
 *      kernel entry (which switches to its own stack in the
 *      image's BSS before calling kmain).
 *
 * Every failure before step 6 halts with a message on the firmware
 * console: fail closed, never jump with a half-built boot_info --
 * the kernel trusts the map completely (see kernel/mm/pmm.c).
 *
 * Kernel link contract (M6, coordinated with the kernel's VMM
 * hardware step, as this file's earlier revision anticipated):
 *   - The kernel is linked higher-half: every PT_LOAD must share
 *     one vaddr-paddr delta, 0xFFFFFFFF7FF00000 (image at virtual
 *     0xFFFFFFFF80000000, physical 1 MiB), enforced in
 *     load_kernel_image(). Segments are still loaded by p_paddr;
 *     step 4's tables make the virtual addresses real.
 *   - The jump target is the ELF symbol `_kentry64_rdi`, the
 *     dedicated UEFI entry the earlier revision foresaw: it takes
 *     boot_info in RDI (boot.h's contract), reloads segments and
 *     switches to the kernel's own stack, then calls kmain. Its
 *     Multiboot sibling `_kentry64` differs only in reading the
 *     boot_info pointer from physical 0x9008, where that chain's
 *     stub publishes it.
 */
#include <yousj/types.h>
#include <yousj/boot.h>
#include "uefi.h"
#include "boot_info.h"

/* ---------------------------------------------------------------- */
/* Small freestanding helpers (no libc exists in a UEFI app here).  */
/* ---------------------------------------------------------------- */

static void mem_copy(void *dst, const void *src, uint64_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--)
        *d++ = *s++;
}

static void mem_zero(void *dst, uint64_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n--)
        *d++ = 0;
}

static int guid_equal(const struct efi_guid *a, const struct efi_guid *b)
{
    const uint8_t *x = (const uint8_t *)a;
    const uint8_t *y = (const uint8_t *)b;
    uint32_t i;
    for (i = 0; i < sizeof(*a); i++)
        if (x[i] != y[i])
            return 0;
    return 1;
}

static uint64_t align_up_u64(uint64_t v, uint64_t a)
{
    return (v + a - 1) & ~(a - 1);
}

/* ---------------------------------------------------------------- */
/* Console logging (firmware ConOut; dead after ExitBootServices).  */
/* ---------------------------------------------------------------- */

static efi_handle_t                    g_image_handle;
static struct efi_system_table        *g_st;
static struct efi_boot_services       *g_bs;
static struct efi_loaded_image_protocol *g_loaded;

static void con_print(const char *s)
{
    /* Convert ASCII to CHAR16 in chunks; '\n' becomes CRLF. */
    efi_char16_t buf[160];
    uint32_t n = 0;
    if (g_st == NULL || g_st->con_out == NULL)
        return;
    while (*s) {
        char c = *s++;
        if (c == '\n') {
            if (n > 158)
                break;
            buf[n++] = '\r';
        }
        if (n > 158)
            break;
        buf[n++] = (efi_char16_t)(uint8_t)c;
        if (n == 159) {
            buf[n] = 0;
            g_st->con_out->output_string(g_st->con_out, buf);
            n = 0;
        }
    }
    if (n) {
        buf[n] = 0;
        g_st->con_out->output_string(g_st->con_out, buf);
    }
}

static void con_print_hex64(uint64_t v)
{
    static const char digits[] = "0123456789abcdef";
    char buf[19];
    int shift;
    buf[0] = '0';
    buf[1] = 'x';
    for (shift = 60; shift >= 0; shift -= 4)
        buf[2 + (60 - shift) / 4] = digits[(v >> shift) & 0xF];
    buf[18] = 0;
    con_print(buf);
}

/* Fail closed: say why on the console, then stop forever. Only
 * used BEFORE a successful ExitBootServices, while ConOut lives. */
static void fail(const char *msg, efi_status_t status)
{
    con_print("\nYOUSJ BOOT FAIL: ");
    con_print(msg);
    if (status != ~(efi_status_t)0) {
        con_print(" (EFI status ");
        con_print_hex64(status);
        con_print(")");
    }
    con_print("\nHalting; the kernel was NOT started.\n");
    for (;;)
        __asm__ volatile("cli; hlt");
}

/* ---------------------------------------------------------------- */
/* Raw COM1 marker, used once after ExitBootServices. Port I/O      */
/* only, no interrupts, no firmware: if the kernel then dies        */
/* instantly, this line on the serial log proves the handoff was    */
/* reached. (The kernel's own driver is kernel/drivers/serial.c;    */
/* a separate binary cannot link it, hence these few lines here.)   */
/* ---------------------------------------------------------------- */

static inline void raw_outb(uint16_t port, uint8_t value)
{
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t raw_inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void raw_serial_marker(void)
{
    const char *s = "YOUSJ-UEFI: boot services exited, jumping to kmain\r\n";
    raw_outb(0x3f9, 0x00);
    raw_outb(0x3fb, 0x80);
    raw_outb(0x3f8, 0x01); /* 115200 */
    raw_outb(0x3f9, 0x00);
    raw_outb(0x3fb, 0x03); /* 8N1 */
    raw_outb(0x3fa, 0xc7); /* FIFO on/clear */
    raw_outb(0x3fc, 0x0b); /* DTR + RTS + OUT2 */
    while (*s) {
        while ((raw_inb(0x3fd) & 0x20) == 0)
            ;
        raw_outb(0x3f8, (uint8_t)*s++);
    }
}

/* ---------------------------------------------------------------- */
/* ELF64 reading (offsets per the ELF spec, same style as the       */
/* coordinator's Multiboot stub loader in kernel/arch/x86_64).      */
/* ---------------------------------------------------------------- */

static uint16_t rd16(const uint8_t *p) { return *(const uint16_t *)p; }
static uint32_t rd32(const uint8_t *p) { return *(const uint32_t *)p; }
static uint64_t rd64(const uint8_t *p) { return *(const uint64_t *)p; }

#define ELF_PT_LOAD   1u
#define ELF_SHT_SYMTAB 2u
#define ELF_ET_EXEC   2u
#define ELF_EM_X86_64 62u

/* The kernel's higher-half link contract (kernel/arch/x86_64/
 * link.ld): virtual base 0xFFFFFFFF80000000, physical base 1 MiB,
 * one uniform delta for every loadable segment. */
#define KERNEL_V2P_DELTA 0xFFFFFFFF7FF00000ULL
#define KERNEL_VIRT_BASE 0xFFFFFFFF80000000ULL

struct kernel_image_info {
    uint64_t base;   /* first claimed page of the loaded image */
    uint64_t length; /* claimed range length (page-rounded) */
    uint64_t entry;  /* VIRTUAL address of the _kentry64_rdi symbol */
};

/* Find a symbol by name in the ELF symbol table; returns its
 * value or 0. */
static uint64_t elf_find_symbol(const uint8_t *img, uint64_t size,
                                const char *want, uint64_t want_len)
{
    uint64_t shoff = rd64(img + 40);
    uint32_t shentsize = rd16(img + 58);
    uint32_t shnum = rd16(img + 60);
    uint32_t i;

    if (shoff == 0 || shentsize < 64 || shnum == 0)
        return 0;
    if (shoff + (uint64_t)shnum * shentsize > size)
        return 0;

    for (i = 0; i < shnum; i++) {
        const uint8_t *sh = img + shoff + (uint64_t)i * shentsize;
        const uint8_t *strsh, *sym, *str;
        uint64_t sym_off, sym_size, sym_entsize, str_off, str_size, j;

        if (rd32(sh + 4) != ELF_SHT_SYMTAB)
            continue;
        if (rd32(sh + 40) >= shnum) /* sh_link -> string table */
            return 0;
        strsh = img + shoff + (uint64_t)rd32(sh + 40) * shentsize;
        sym_off = rd64(sh + 24);
        sym_size = rd64(sh + 32);
        sym_entsize = rd64(sh + 56);
        str_off = rd64(strsh + 24);
        str_size = rd64(strsh + 32);
        if (sym_entsize < 24 || sym_off + sym_size > size ||
            str_off + str_size > size)
            return 0;
        sym = img + sym_off;
        str = img + str_off;
        for (j = 0; j + sym_entsize <= sym_size; j += sym_entsize) {
            uint32_t name_off = rd32(sym + j);
            uint64_t value = rd64(sym + j + 8);
            const char *name;
            uint64_t k;
            if (rd16(sym + j + 6) == 0) /* SHN_UNDEF: not defined here */
                continue;
            if (name_off >= str_size)
                continue;
            name = (const char *)(str + name_off);
            /* Bounded compare against the wanted name. The loop
             * MUST stop at k == want_len: the literal's own NUL
             * would EQUAL the name's terminator, so an unbounded
             * loop overshoots by one (reading past the literal)
             * and the exact match is never reported. Coordinator
             * fix on acceptance; verified against a real kernel
             * ELF. */
            k = 0;
            while (k < want_len && k < str_size - name_off &&
                   name[k] == want[k])
                k++;
            if (k == want_len &&
                (name_off + want_len >= str_size ||
                 name[want_len] == '\0'))
                return value;
        }
        return 0; /* one symtab is all an EXEC image has */
    }
    return 0;
}

/* Validate the ELF, claim its destination pages at exactly the
 * linked physical addresses, copy the segments, zero BSS tails. */
static void load_kernel_image(const uint8_t *img, uint64_t size,
                              struct kernel_image_info *out)
{
    uint64_t phoff, base = ~(uint64_t)0, end = 0, addr;
    uint32_t phentsize, phnum, i;
    efi_status_t status;

    if (size < 64 || rd32(img) != 0x464C457Fu || img[4] != 2 || img[5] != 1)
        fail("kernel image is not a little-endian ELF64 file", ~(efi_status_t)0);
    if (rd16(img + 16) != ELF_ET_EXEC || rd16(img + 18) != ELF_EM_X86_64)
        fail("kernel image is not an x86_64 EXEC file", ~(efi_status_t)0);

    phoff = rd64(img + 32);
    phentsize = rd16(img + 54);
    phnum = rd16(img + 56);
    if (phentsize < 56 || phnum == 0 || phnum > 128 ||
        phoff + (uint64_t)phnum * phentsize > size)
        fail("kernel ELF program headers are out of bounds", ~(efi_status_t)0);

    /* First pass: validate every PT_LOAD and find the span. */
    for (i = 0; i < phnum; i++) {
        const uint8_t *ph = img + phoff + (uint64_t)i * phentsize;
        uint64_t offset, vaddr, paddr, filesz, memsz;
        if (rd32(ph) != ELF_PT_LOAD)
            continue;
        offset = rd64(ph + 8);
        vaddr = rd64(ph + 16);
        paddr = rd64(ph + 24);
        filesz = rd64(ph + 32);
        memsz = rd64(ph + 40);
        if (memsz == 0)
            continue;
        if (filesz > memsz || offset > size || filesz > size - offset)
            fail("kernel PT_LOAD segment escapes the image file", ~(efi_status_t)0);
        if (paddr + memsz < paddr)
            fail("kernel PT_LOAD physical range wraps", ~(efi_status_t)0);
        if (vaddr - paddr != KERNEL_V2P_DELTA)
            fail("kernel PT_LOAD vaddr-paddr delta is not the "
                 "higher-half contract delta (image at virtual "
                 "0xFFFFFFFF80000000, physical 1 MiB)",
                 ~(efi_status_t)0);
        if (paddr < base)
            base = paddr;
        if (paddr + memsz > end)
            end = paddr + memsz;
    }
    if (base == ~(uint64_t)0 || end <= base)
        fail("kernel ELF has no loadable segments", ~(efi_status_t)0);

    /* Claim exactly [align_down(base), align_up(end)) so no other
     * allocation (in particular our own later ones) can land on
     * kernel pages, and so the final memory map can state this
     * range truthfully. Failure means something else owns the
     * pages: stop instead of overwriting it. */
    addr = base & ~(EFI_PAGE_SIZE - 1);
    status = g_bs->allocate_pages(EFI_ALLOCATE_ADDRESS, EFI_LOADER_DATA,
                                  (align_up_u64(end, EFI_PAGE_SIZE) - addr) /
                                      EFI_PAGE_SIZE,
                                  &addr);
    if (EFI_ERROR(status))
        fail("AllocatePages for the kernel image range failed", status);
    out->base = addr;
    out->length = align_up_u64(end, EFI_PAGE_SIZE) - addr;

    /* Second pass: copy + zero. */
    for (i = 0; i < phnum; i++) {
        const uint8_t *ph = img + phoff + (uint64_t)i * phentsize;
        uint64_t offset, paddr, filesz, memsz;
        if (rd32(ph) != ELF_PT_LOAD)
            continue;
        offset = rd64(ph + 8);
        paddr = rd64(ph + 24);
        filesz = rd64(ph + 32);
        memsz = rd64(ph + 40);
        if (memsz == 0)
            continue;
        mem_copy((void *)(uintptr_t)paddr, img + offset, filesz);
        if (memsz > filesz)
            mem_zero((void *)(uintptr_t)(paddr + filesz), memsz - filesz);
    }

    out->entry = elf_find_symbol(img, size, "_kentry64_rdi", 13);
    if (out->entry == 0 || out->entry < base + KERNEL_V2P_DELTA ||
        out->entry >= end + KERNEL_V2P_DELTA)
        fail("_kentry64_rdi symbol not found in the kernel image "
             "(symbol table stripped? see DESIGN.md proposals)",
             ~(efi_status_t)0);
}

/* ---------------------------------------------------------------- */
/* Kernel file loading from the ESP we were booted from.            */
/* ---------------------------------------------------------------- */

static const efi_char16_t *const KERNEL_PATHS[] = {
    (const efi_char16_t *)u"\\EFI\\YOUSJ\\YOUSJ.KRN",
    (const efi_char16_t *)u"\\YOUSJ.KRN",
};

static void read_kernel_file(uint8_t **out_buf, uint64_t *out_size)
{
    static const struct efi_guid fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    static const struct efi_guid li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    struct efi_simple_file_system_protocol *fs;
    struct efi_file_protocol *root = NULL, *file = NULL;
    efi_status_t status;
    uint64_t size, to_read;
    uint32_t i;

    status = g_bs->handle_protocol(g_image_handle, &li_guid,
                                   (void **)&g_loaded);
    if (EFI_ERROR(status) || g_loaded == NULL)
        fail("HandleProtocol(LoadedImage) failed", status);
    status = g_bs->handle_protocol(g_loaded->device_handle, &fs_guid,
                                   (void **)&fs);
    if (EFI_ERROR(status))
        fail("HandleProtocol(SimpleFileSystem) failed", status);
    status = fs->open_volume(fs, &root);
    if (EFI_ERROR(status))
        fail("OpenVolume on the boot ESP failed", status);

    for (i = 0; i < sizeof(KERNEL_PATHS) / sizeof(KERNEL_PATHS[0]); i++) {
        status = root->open(root, &file, KERNEL_PATHS[i],
                            EFI_FILE_MODE_READ, 0);
        if (!EFI_ERROR(status))
            break;
        file = NULL;
    }
    if (file == NULL)
        fail("kernel file not found (\\EFI\\YOUSJ\\YOUSJ.KRN)", status);

    /* File size without GetInfo: seek to EOF and ask where we are. */
    status = file->set_position(file, EFI_FILE_POSITION_END);
    if (EFI_ERROR(status))
        fail("SetPosition(EOF) on the kernel file failed", status);
    status = file->get_position(file, &size);
    if (EFI_ERROR(status))
        fail("GetPosition on the kernel file failed", status);
    status = file->set_position(file, 0);
    if (EFI_ERROR(status))
        fail("SetPosition(0) on the kernel file failed", status);
    if (size < 64 || size > 64ULL * 1024 * 1024)
        fail("kernel file size is implausible", ~(efi_status_t)0);

    status = g_bs->allocate_pool(EFI_LOADER_DATA, size, (void **)out_buf);
    if (EFI_ERROR(status))
        fail("AllocatePool for the kernel file failed", status);
    to_read = size;
    status = file->read(file, &to_read, *out_buf);
    if (EFI_ERROR(status) || to_read != size)
        fail("short read on the kernel file", status);

    file->close(file);
    root->close(root);
    *out_size = size;
}

/* ---------------------------------------------------------------- */
/* ACPI RSDP from the system configuration table (0 when absent).   */
/* ---------------------------------------------------------------- */

static uint64_t find_rsdp(void)
{
    static const struct efi_guid acpi20 = EFI_ACPI_20_TABLE_GUID;
    static const struct efi_guid acpi10 = EFI_ACPI_10_TABLE_GUID;
    uint64_t i;
    uint64_t fallback = 0;

    for (i = 0; i < g_st->number_of_table_entries; i++) {
        const struct efi_configuration_table *t =
            &g_st->configuration_table[i];
        if (guid_equal(&t->guid, &acpi20))
            return (uint64_t)(uintptr_t)t->vendor_table;
        if (guid_equal(&t->guid, &acpi10))
            fallback = (uint64_t)(uintptr_t)t->vendor_table;
    }
    return fallback;
}

/* ---------------------------------------------------------------- */
/* One get-map + convert + build round. Returns the MapKey that     */
/* ExitBootServices must be called with. Any failure halts (we are  */
/* still in boot services, so halting beats exiting with a bad map).*/
/*                                                                  */
/* The map buffer and the converted-entry array are pool memory     */
/* and are deliberately NOT freed: freeing pool pages changes the   */
/* memory map and would invalidate the MapKey we are about to use.  */
/* They stay mapped as EfiBootServicesData -> YOUSJ_MEM_BOOTLOADER  */
/* in the final map, which is exactly what they are.                */
/* ---------------------------------------------------------------- */

static uint64_t fetch_map_and_build(struct yousj_boot_info *info,
                                    const struct kernel_image_info *kimg,
                                    uint64_t boot_info_base,
                                    uint64_t boot_info_len,
                                    uint64_t stack_base,
                                    uint64_t stack_len,
                                    uint64_t rsdp,
                                    const char *cmdline_ascii)
{
    uint64_t map_size = 0, map_key = 0, desc_size = 0;
    uint32_t desc_version = 0;
    efi_status_t status;
    uint8_t *map_buf;
    struct yousj_fw_mem_entry *fw;
    uint64_t fw_cap, fw_count = 0, i;
    int rc, attempt;

    /* Two-call sizing, with an internal retry: our own pool
     * allocations below can grow the map between the query and
     * the real call, so BUFFER_TOO_SMALL here just loops.
     *
     * HARD INVARIANT (coordinator fix on acceptance): BOTH buffers
     * -- the raw map and the converted-entry array -- are allocated
     * BEFORE the successful GetMemoryMap call. Any pool allocation
     * after that call can change the memory map and invalidate the
     * MapKey, and the caller's ExitBootServices retry would repeat
     * the same order and fail the same way. The conversion array's
     * capacity derives from the buffer size and the smallest legal
     * descriptor stride, so it is always sufficient. */
    for (attempt = 0; attempt < 3; attempt++) {
        uint64_t buf_size;
        map_size = 0;
        status = g_bs->get_memory_map(&map_size, NULL, &map_key,
                                      &desc_size, &desc_version);
        if (status != EFI_BUFFER_TOO_SMALL || map_size == 0)
            fail("GetMemoryMap sizing call failed", status);
        buf_size = map_size + 4096; /* generous slack for churn */
        status = g_bs->allocate_pool(EFI_LOADER_DATA, buf_size,
                                     (void **)&map_buf);
        if (EFI_ERROR(status))
            fail("AllocatePool for the memory map failed", status);
        fw_cap = buf_size / sizeof(struct efi_memory_descriptor) + 4;
        status = g_bs->allocate_pool(EFI_LOADER_DATA,
                                     fw_cap * sizeof(*fw), (void **)&fw);
        if (EFI_ERROR(status))
            fail("AllocatePool for converted map failed", status);
        map_size = buf_size;
        status = g_bs->get_memory_map(&map_size, (void *)map_buf,
                                      &map_key, &desc_size,
                                      &desc_version);
        if (status == EFI_BUFFER_TOO_SMALL) {
            g_bs->free_pool(map_buf);
            g_bs->free_pool(fw);
            continue;
        }
        if (EFI_ERROR(status))
            fail("GetMemoryMap failed", status);
        break;
    }
    if (attempt == 3)
        fail("GetMemoryMap never fit its buffer", ~(efi_status_t)0);
    if (desc_version != EFI_MEMORY_DESCRIPTOR_VERSION ||
        desc_size < sizeof(struct efi_memory_descriptor))
        fail("unknown EFI memory descriptor format", ~(efi_status_t)0);

    /* Convert descriptors into the abstract array boot_info.c
     * consumes (allocated above, before the fetch -- see the
     * invariant note). No allocation of any kind from here on. */
    for (i = 0; i + desc_size <= map_size; i += desc_size) {
        const struct efi_memory_descriptor *d =
            (const struct efi_memory_descriptor *)(map_buf + i);
        if (d->number_of_pages == 0)
            continue;
        if (fw_count >= fw_cap)
            fail("converted memory map overflowed its array",
                 ~(efi_status_t)0);
        fw[fw_count].base = d->physical_start;
        /* Saturate instead of wrapping; boot_info.c forces any
         * entry whose base+length wraps to RESERVED anyway. */
        fw[fw_count].length =
            d->number_of_pages > (UINT64_MAX >> 12)
                ? ~(uint64_t)0 - d->physical_start
                : d->number_of_pages * EFI_PAGE_SIZE;
        fw[fw_count].uefi_type = d->type;
        fw[fw_count].reserved = 0;
        fw_count++;
    }

    /* The pure conversion wants ascending base order (UEFI maps
     * arrive sorted in practice; sort anyway -- cheap at this
     * size and removes a silent assumption). Insertion sort. */
    for (i = 1; i < fw_count; i++) {
        struct yousj_fw_mem_entry key = fw[i];
        uint64_t j = i;
        while (j > 0 && fw[j - 1].base > key.base) {
            fw[j] = fw[j - 1];
            j--;
        }
        fw[j] = key;
    }

    rc = yousj_boot_info_build(info, fw, fw_count, kimg->base,
                               kimg->length);
    if (rc != 0)
        fail("boot_info build failed (map does not fit boot.h)",
             ~(efi_status_t)0);

    /* boot_info itself and the kernel stack must survive the PMM:
     * state their protection explicitly, like the kernel range. */
    rc = yousj_boot_info_mark_range(info, boot_info_base, boot_info_len,
                                    YOUSJ_MEM_BOOTLOADER);
    if (rc == 0)
        rc = yousj_boot_info_mark_range(info, stack_base, stack_len,
                                        YOUSJ_MEM_BOOTLOADER);
    if (rc == 0 && g_loaded->image_base != NULL)
        rc = yousj_boot_info_mark_range(
            info, (uint64_t)(uintptr_t)g_loaded->image_base,
            g_loaded->image_size, YOUSJ_MEM_BOOTLOADER);
    if (rc != 0)
        fail("boot_info range marking failed", ~(efi_status_t)0);

    info->rsdp_addr = rsdp;
    yousj_boot_info_set_cmdline(info, cmdline_ascii);
    return map_key;
}

/* ---------------------------------------------------------------- */
/* Hand-off page tables (M6). Six pages, allocated as               */
/* EFI_LOADER_DATA so the converted memory map calls them           */
/* BOOTLOADER and the kernel PMM never hands them out:              */
/*   [0] PML4:  [0] -> identity PDPT, [511] -> kernel PDPT          */
/*   [1] identity PDPT: [0] -> identity PD                          */
/*   [2] identity PD: 512 x 2 MiB pages covering [0, 1 GiB)         */
/*   [3] kernel PDPT: [510] -> kernel PD                            */
/*   [4] kernel PD: [0] -> kernel PT                                */
/*   [5] kernel PT: the image window at 4 KiB granularity           */
/* The identity half exists only so this loader's own code, its     */
/* stack and the boot_info page stay reachable between loading CR3  */
/* and jumping; the kernel discards these tables at vmm_hw_init()   */
/* and rebuilds the layout in its own master space. The image is    */
/* mapped with 4 KiB leaves because it is 1 MiB-based: a 2 MiB      */
/* huge-page entry cannot represent it (the base's low bits would   */
/* land in the entry's reserved field and fault).                   */
/* ---------------------------------------------------------------- */

#define HANDOFF_TABLE_PAGES 6
static uint64_t g_handoff_pml4; /* physical; set by the builder */

static void build_handoff_tables(const struct kernel_image_info *kimg,
                                 uint64_t tables_phys)
{
    uint64_t *pml4   = (uint64_t *)(uintptr_t)(tables_phys + 0 * EFI_PAGE_SIZE);
    uint64_t *pdpt_i = (uint64_t *)(uintptr_t)(tables_phys + 1 * EFI_PAGE_SIZE);
    uint64_t *pd_i   = (uint64_t *)(uintptr_t)(tables_phys + 2 * EFI_PAGE_SIZE);
    uint64_t *pdpt_k = (uint64_t *)(uintptr_t)(tables_phys + 3 * EFI_PAGE_SIZE);
    uint64_t *pd_k   = (uint64_t *)(uintptr_t)(tables_phys + 4 * EFI_PAGE_SIZE);
    uint64_t *pt_k   = (uint64_t *)(uintptr_t)(tables_phys + 5 * EFI_PAGE_SIZE);
    uint64_t i, pages = kimg->length / EFI_PAGE_SIZE;

    if (pages > 512)
        fail("kernel image exceeds the 2 MiB hand-off window",
             ~(efi_status_t)0);

    mem_zero(pml4, HANDOFF_TABLE_PAGES * EFI_PAGE_SIZE);

    for (i = 0; i < 512; i++)
        pd_i[i] = (i * 0x200000ULL) | 0x83; /* identity, 2 MiB, RW */
    pdpt_i[0] = (tables_phys + 2 * EFI_PAGE_SIZE) | 0x3;
    pml4[0] = (tables_phys + 1 * EFI_PAGE_SIZE) | 0x3;

    for (i = 0; i < pages; i++)
        pt_k[i] = (kimg->base + i * EFI_PAGE_SIZE) | 0x3;
    pd_k[0] = (tables_phys + 5 * EFI_PAGE_SIZE) | 0x3;
    pdpt_k[510] = (tables_phys + 4 * EFI_PAGE_SIZE) | 0x3;
    pml4[511] = (tables_phys + 3 * EFI_PAGE_SIZE) | 0x3;

    g_handoff_pml4 = tables_phys;
}

/* ---------------------------------------------------------------- */
/* The handoff. Interrupts off FIRST (firmware interrupt state is   */
/* not ours anymore), one raw-serial breadcrumb, then registers,    */
/* CR3 and jump. RSP is set to stack_top - 8 (the alignment a       */
/* normal `call` would produce); the kernel entry stub replaces     */
/* it with the kernel's own stack before calling kmain, and kmain   */
/* never returns.                                                   */
/*                                                                  */
/* GDT contract (found the hard way in OVMF testing, on BOTH      */
/* sides in parallel): the kernel's IDT entries hardcode selector  */
/* 0x08 as its code64 descriptor, and EDK2's GDT does not have a   */
/* 64-bit code descriptor at 0x08, so under the raw firmware GDT   */
/* the first timer interrupt triple-faulted. The kernel has since  */
/* grown its own GDT (kernel/arch/x86_64/gdt.c, installed at the   */
/* very start of kmain), so it no longer depends on this. This     */
/* loader still installs the same three descriptors before         */
/* jumping -- the boot contract "flat GDT, code64 at 0x08, data    */
/* at 0x10" then holds from the kernel's first instruction rather  */
/* than from gdt_init(), whoever boots. Descriptor values are      */
/* bit-identical to the stub's (stub.S) and the kernel's.          */
/* ---------------------------------------------------------------- */

static const uint64_t g_kernel_gdt[3] = {
    0x0000000000000000ULL, /* 0x00: null */
    0x00AF9A000000FFFFULL, /* 0x08: code64, ring 0 */
    0x00CF92000000FFFFULL, /* 0x10: data, ring 0, flat */
};

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static void jump_to_kernel(const struct yousj_boot_info *info,
                           uint64_t stack_top, uint64_t entry,
                           uint64_t pml4_phys)
{
    /* Filled at runtime, not by a link-time initializer: the GDTR
     * base is an absolute address and must not depend on the
     * firmware applying PE base relocations for us. */
    static struct gdt_ptr gdtr;

    gdtr.limit = sizeof(g_kernel_gdt) - 1;
    gdtr.base = (uint64_t)(uintptr_t)g_kernel_gdt;

    __asm__ volatile("cli");
    raw_serial_marker();
    __asm__ volatile(
        "lgdt (%0)\n\t"
        "mov $0x10, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "mov %%ax, %%ss\n\t"
        /* reload CS to 0x08 via a far return */
        "pushq $0x08\n\t"
        "lea 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        "cld\n\t"
        "mov %1, %%rsp\n\t"
        "mov %2, %%rdi\n\t"
        /* Switch to the hand-off tables LAST: the instructions
         * above still execute at this loader's own addresses,
         * which the identity half of the new tables keeps mapped,
         * and the very next fetch is the kernel entry at its
         * higher-half base. The entry stub replaces RSP with the
         * kernel stack before anything is pushed. */
        "mov %4, %%rax\n\t"
        "mov %%rax, %%cr3\n\t"
        "jmp *%3\n\t"
        :
        : "r"(&gdtr), "r"(stack_top - 8),
          "r"((uint64_t)(uintptr_t)info), "r"(entry),
          "r"(pml4_phys)
        : "rax", "memory");
    for (;;)
        __asm__ volatile("hlt");
}

/* ---------------------------------------------------------------- */
/* Entry point (UEFI calling convention).                           */
/* ---------------------------------------------------------------- */

efi_status_t EFIAPI efi_main(efi_handle_t image_handle,
                             struct efi_system_table *system_table)
{
    uint8_t *kernel_file;
    uint64_t kernel_file_size;
    struct kernel_image_info kimg;
    struct yousj_boot_info *info;
    uint64_t addr, info_pages, stack_base, stack_pages, rsdp;
    char cmdline_ascii[YOUSJ_CMDLINE_MAX];
    efi_status_t status;
    int attempt;

    g_image_handle = image_handle;
    g_st = system_table;
    if (system_table == NULL || system_table->boot_services == NULL)
        return EFI_LOAD_ERROR; /* nowhere to even print to */
    g_bs = system_table->boot_services;

    con_print("\nYousj OS UEFI bootloader\n");

    /* 1-2. Read and load the kernel image. */
    read_kernel_file(&kernel_file, &kernel_file_size);
    load_kernel_image(kernel_file, kernel_file_size, &kimg);
    /* The file buffer has served its purpose; freeing it now is
     * safe because the final memory map is fetched afterwards. */
    g_bs->free_pool(kernel_file);
    con_print("Yousj OS: kernel loaded, entry ");
    con_print_hex64(kimg.entry);
    con_print("\n");

    /* 3. boot_info and kernel stack pages. */
    info_pages = align_up_u64(sizeof(*info), EFI_PAGE_SIZE) / EFI_PAGE_SIZE;
    addr = 0;
    status = g_bs->allocate_pages(EFI_ALLOCATE_ANY_PAGES, EFI_LOADER_DATA,
                                  info_pages, &addr);
    if (EFI_ERROR(status))
        fail("AllocatePages for boot_info failed", status);
    info = (struct yousj_boot_info *)(uintptr_t)addr;

    stack_pages = 4; /* 16 KiB, same size as the kernel's stub stack */
    addr = 0;
    status = g_bs->allocate_pages(EFI_ALLOCATE_ANY_PAGES, EFI_LOADER_DATA,
                                  stack_pages, &addr);
    if (EFI_ERROR(status))
        fail("AllocatePages for the kernel stack failed", status);
    stack_base = addr;

    /* 4. Hand-off page tables (identity + the kernel image at its
     * higher-half base). Built now, while boot services can still
     * allocate; loaded into CR3 only after ExitBootServices. */
    addr = 0;
    status = g_bs->allocate_pages(EFI_ALLOCATE_ANY_PAGES, EFI_LOADER_DATA,
                                  HANDOFF_TABLE_PAGES, &addr);
    if (EFI_ERROR(status))
        fail("AllocatePages for the hand-off tables failed", status);
    build_handoff_tables(&kimg, addr);

    rsdp = find_rsdp();

    /* LoadOptions (CHAR16, usually the loader's own name/args) ->
     * ASCII for boot.h's cmdline. Non-ASCII becomes '?'. */
    {
        const efi_char16_t *opts =
            (const efi_char16_t *)g_loaded->load_options;
        uint32_t max_chars = g_loaded->load_options_size / 2;
        uint32_t i;
        for (i = 0; i < max_chars && i < YOUSJ_CMDLINE_MAX - 1; i++) {
            efi_char16_t ch = opts ? opts[i] : 0;
            if (ch == 0)
                break;
            cmdline_ascii[i] = (ch >= 32 && ch < 127) ? (char)ch : '?';
        }
        cmdline_ascii[i] = '\0';
    }

    /* 5-6. Fresh map + build per attempt; exit with its MapKey.
     * A failed exit leaves boot services alive, so the retry can
     * (and must) redo the whole fetch: the map may have changed. */
    for (attempt = 0; attempt < 2; attempt++) {
        uint64_t map_key = fetch_map_and_build(
            info, &kimg, (uint64_t)(uintptr_t)info,
            info_pages * EFI_PAGE_SIZE, stack_base,
            stack_pages * EFI_PAGE_SIZE, rsdp, cmdline_ascii);
        status = g_bs->exit_boot_services(image_handle, map_key);
        if (!EFI_ERROR(status))
            break;
        if (attempt == 0)
            con_print("Yousj OS: ExitBootServices retrying with a "
                      "fresh memory map\n");
    }
    if (EFI_ERROR(status))
        fail("ExitBootServices failed twice", status);

    /* 7. Gone. */
    jump_to_kernel(info, stack_base + stack_pages * EFI_PAGE_SIZE,
                   kimg.entry, g_handoff_pml4);
    return EFI_LOAD_ERROR; /* unreachable */
}

/* Yousj OS - kernel entry point. Written by Muse (coordinator) as the
 * stage 0/1 core. Server-first: output goes to the serial console ONLY.
 * No framebuffer, no graphics - a headless server does not need them,
 * and serial is where a server kernel is debugged anyway.
 *
 * The bootloader hands us struct yousj_boot_info* in RDI (see boot.h).
 */
#include <yousj/types.h>
#include <yousj/boot.h>
#include <yousj/pmm.h>
#include <yousj/vmm.h>
#include <drivers/serial.h> /* kernel/drivers/serial.h; -I kernel in Makefile */

static void halt_forever(void)
{
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

/* Kernel entry. boot_info may be NULL in the earliest bring-up builds,
 * in which case we say so on serial and stop instead of crashing blind. */
void kmain(struct yousj_boot_info *boot_info)
{
    serial_init();
    serial_puts("\nYousj OS: kmain reached (server-first build)\n");

    if (boot_info == NULL) {
        serial_puts("Yousj OS: no boot_info, halting\n");
        halt_forever();
    }
    if (boot_info->magic != YOUSJ_BOOT_MAGIC) {
        serial_puts("Yousj OS: bad boot_info magic, halting\n");
        halt_forever();
    }

    serial_puts("Yousj OS: boot_info OK, memory map received\n");

    /* Stage 1, step 1 (done 2026-11-01): physical page allocator. */
    pmm_init(boot_info);
    serial_puts("Yousj OS: PMM total pages=");
    serial_put_u64(pmm_total_page_count());
    serial_puts(" free pages=");
    serial_put_u64(pmm_free_page_count());
    if (pmm_truncated())
        serial_puts(" (map truncated to bitmap capacity)");
    serial_putc('\n');

    /* PMM self-test on real hardware state: allocate one page, show
     * it, give it back. A failure here means the boot map lied. */
    {
        uint64_t page = pmm_alloc_page();
        if (page == 0) {
            serial_puts("Yousj OS: PMM self-test FAILED (no free page)\n");
            halt_forever();
        }
        serial_puts("Yousj OS: PMM self-test alloc ");
        serial_put_hex64(page);
        serial_putc('\n');
        if (pmm_free_page(page) != 0) {
            serial_puts("Yousj OS: PMM self-test FAILED (free)\n");
            halt_forever();
        }
        serial_puts("Yousj OS: PMM self-test PASS\n");
    }

    /* Stage 1, step 2 (logic done 2026-11-01): the VMM's page-table
     * walking/mapping is exercised here as a pure software self-test
     * on a throwaway address space. We deliberately do NOT load the
     * new space into CR3 yet: switching page tables waits for the
     * bootloader paging contract (docs/VMM_DESIGN.md sec. 2.3). */
    {
        struct vmm_space space;
        uint64_t data_page, translated = 0;
        int ok = 1;

        vmm_use_kernel_backend();
        if (vmm_create_space(&space) != 0)
            ok = 0;
        data_page = ok ? pmm_alloc_page() : 0;
        if (data_page == 0)
            ok = 0;
        if (ok && vmm_map(&space, 0x400000, data_page, YOUSJ_PAGE_SIZE,
                          YOUSJ_VMM_READ | YOUSJ_VMM_WRITE) != 0)
            ok = 0;
        if (ok && (vmm_translate(&space, 0x400000, &translated, NULL) != 0 ||
                   translated != data_page))
            ok = 0;
        if (ok && vmm_unmap(&space, 0x400000, YOUSJ_PAGE_SIZE) != 0)
            ok = 0;
        if (data_page != 0)
            (void)pmm_free_page(data_page);
        if (ok) {
            vmm_destroy_space(&space);
            serial_puts("Yousj OS: VMM self-test PASS (no CR3 switch yet)\n");
        } else {
            serial_puts("Yousj OS: VMM self-test FAILED, halting\n");
            halt_forever();
        }
    }

    /* Next steps, in order (owned by coordinator):
     * 3. bootloader contract + CR3 switch to the kernel's own tables
     *    (hardware start decided by the user 2026-11-01: virtual
     *    machine / QEMU first, real hardware later)
     * 4. scheduler + first kernel thread
     * 5. console input, then jump to userland init/shell (server userland)
     */
    serial_puts("Yousj OS: stage 1 PMM+VMM logic done, halting for now\n");
    halt_forever();
}

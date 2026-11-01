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

#define SERIAL_PORT 0x3f8 /* COM1 */

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void serial_init(void)
{
    outb(SERIAL_PORT + 1, 0x00); /* disable interrupts */
    outb(SERIAL_PORT + 3, 0x80); /* DLAB on */
    outb(SERIAL_PORT + 0, 0x01); /* divisor 1 = 115200 baud */
    outb(SERIAL_PORT + 1, 0x00);
    outb(SERIAL_PORT + 3, 0x03); /* 8 bits, no parity, 1 stop bit */
    outb(SERIAL_PORT + 2, 0xc7); /* FIFO on, clear, 14-byte threshold */
    outb(SERIAL_PORT + 4, 0x0b); /* DTR + RTS + OUT2 */
}

static void serial_putc(char c)
{
    while ((inb(SERIAL_PORT + 5) & 0x20) == 0) {
        /* wait for transmitter holding register empty */
    }
    outb(SERIAL_PORT, (uint8_t)c);
}

static void serial_puts(const char *s)
{
    while (*s) {
        if (*s == '\n')
            serial_putc('\r');
        serial_putc(*s++);
    }
}

static void serial_put_u64(uint64_t v)
{
    char buf[20];
    int i = 0;
    if (v == 0) {
        serial_putc('0');
        return;
    }
    while (v > 0) {
        buf[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (i > 0)
        serial_putc(buf[--i]);
}

static void serial_put_hex64(uint64_t v)
{
    static const char digits[] = "0123456789abcdef";
    int shift;
    serial_puts("0x");
    for (shift = 60; shift >= 0; shift -= 4)
        serial_putc(digits[(v >> shift) & 0xF]);
}

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

    /* Next steps, in order (owned by coordinator):
     * 2. virtual memory (identity + kernel map)
     * 3. scheduler + first kernel thread
     * 4. console input, then jump to userland init/shell (server userland)
     */
    serial_puts("Yousj OS: stage 1 PMM done, next is VMM, halting for now\n");
    halt_forever();
}

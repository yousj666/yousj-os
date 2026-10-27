/* Yousj OS - kernel entry point. Written by Muse (coordinator) as the
 * stage 0/1 core. Server-first: output goes to the serial console ONLY.
 * No framebuffer, no graphics - a headless server does not need them,
 * and serial is where a server kernel is debugged anyway.
 *
 * The bootloader hands us struct yousj_boot_info* in RDI (see boot.h).
 */
#include <yousj/types.h>
#include <yousj/boot.h>

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
    /* Next steps, in order (owned by coordinator):
     * 1. physical page allocator over boot_info->mem
     * 2. virtual memory (identity + kernel map)
     * 3. scheduler + first kernel thread
     * 4. console input, then jump to userland init/shell (server userland)
     */
    serial_puts("Yousj OS: stage 1 work starts here, halting for now\n");
    halt_forever();
}

/* Yousj OS - COM1 serial console driver (16550 UART, polled TX).
 *
 * Written from scratch. Extracted, behaviour unchanged, from the
 * static helpers that kernel/arch/x86_64/kmain.c carried inline in
 * stage 0/1; see serial.h for the rationale and the contract.
 *
 * Register access is x86 port I/O. Offsets below are from the COM1
 * base port and follow the classic 16550 register map:
 *   +0  RBR (read) / THR (write), or divisor low  when DLAB=1
 *   +1  IER,                        or divisor high when DLAB=1
 *   +2  FCR (write)
 *   +3  LCR (bit 7 = DLAB)
 *   +4  MCR
 *   +5  LSR (bit 5 = transmitter holding register empty)
 */
#include "serial.h"

#define SERIAL_PORT YOUSJ_SERIAL_COM1_PORT

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

void serial_init(void)
{
    outb(SERIAL_PORT + 1, 0x00); /* disable interrupts */
    outb(SERIAL_PORT + 3, 0x80); /* DLAB on */
    outb(SERIAL_PORT + 0, 0x01); /* divisor 1 = 115200 baud */
    outb(SERIAL_PORT + 1, 0x00);
    outb(SERIAL_PORT + 3, 0x03); /* 8 bits, no parity, 1 stop bit */
    outb(SERIAL_PORT + 2, 0xc7); /* FIFO on, clear, 14-byte threshold */
    outb(SERIAL_PORT + 4, 0x0b); /* DTR + RTS + OUT2 */
}

void serial_putc(char c)
{
    while ((inb(SERIAL_PORT + 5) & 0x20) == 0) {
        /* wait for transmitter holding register empty */
    }
    outb(SERIAL_PORT, (uint8_t)c);
}

void serial_puts(const char *s)
{
    while (*s) {
        if (*s == '\n')
            serial_putc('\r');
        serial_putc(*s++);
    }
}

void serial_put_u64(uint64_t v)
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

void serial_put_hex64(uint64_t v)
{
    static const char digits[] = "0123456789abcdef";
    int shift;
    serial_puts("0x");
    for (shift = 60; shift >= 0; shift -= 4)
        serial_putc(digits[(v >> shift) & 0xF]);
}

int serial_rx_ready(void)
{
    /* LSR bit 0 without consuming the byte (poll/select). */
    return (inb(SERIAL_PORT + 5) & 0x01) != 0;
}

int serial_getc_nonblock(void)
{
    /* LSR (offset 5) bit 0 = data ready in the receive register. */
    if ((inb(SERIAL_PORT + 5) & 0x01) == 0)
        return -1;
    return (int)inb(SERIAL_PORT + 0);
}

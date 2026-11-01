/* Yousj OS - x86 port I/O helpers (header-only).
 * Written from scratch by Muse (coordinator).
 */
#ifndef YOUSJ_IO_H
#define YOUSJ_IO_H

#include <yousj/types.h>

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

/* A write to the POST diagnostic port, the traditional tiny delay
 * used between PIC/PIT setup steps. */
static inline void io_wait(void)
{
    outb(0x80, 0);
}

#endif /* YOUSJ_IO_H */

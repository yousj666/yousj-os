/* Yousj OS - COM1 serial console driver (interface).
 *
 * Written from scratch. This is the formal driver version of the
 * serial logic that stage 0/1 first carried inline in
 * kernel/arch/x86_64/kmain.c; the behaviour (port, baud rate, framing,
 * FIFO setup, CRLF handling) is intentionally identical so the
 * coordinator can swap the inline version for this driver without
 * any observable change on the serial line.
 *
 * Server-first rationale: a headless server has no display to print
 * to, but it virtually always has a COM port (physical, BMC SOL, or
 * emulated by a hypervisor). All early kernel output goes here.
 *
 * The driver is polled, not interrupt-driven: serial_init() leaves
 * UART interrupts disabled, exactly like the inline version did.
 * An interrupt-driven RX path belongs to the console-input stage and
 * will be added behind this same header when the coordinator asks.
 */
#ifndef YOUSJ_DRIVERS_SERIAL_H
#define YOUSJ_DRIVERS_SERIAL_H

#include <yousj/types.h>

/* COM1 base I/O port. The only UART this driver drives in stage 1. */
#define YOUSJ_SERIAL_COM1_PORT 0x3f8

/* Initialise COM1: 115200 baud, 8 data bits, no parity, 1 stop bit
 * (8N1), FIFO enabled and cleared with a 14-byte receive threshold,
 * DTR + RTS + OUT2 asserted, UART interrupts disabled.
 * Safe to call once at kernel entry, before any other serial call. */
void serial_init(void);

/* Write one character to COM1, busy-waiting until the transmitter
 * holding register is empty. A '\n' written through this function is
 * sent as-is; use serial_puts() for CRLF translation. */
void serial_putc(char c);

/* Write a NUL-terminated string. Each '\n' is preceded by a '\r',
 * matching the inline kmain behaviour, so lines render correctly on
 * a plain terminal watching the serial port. */
void serial_puts(const char *s);

/* Convenience helpers carried over from the inline kmain code, so
 * existing call sites keep working after the swap:
 * serial_put_u64() prints a decimal value with no padding;
 * serial_put_hex64() prints "0x" followed by 16 lowercase hex
 * digits, most significant nibble first. */
void serial_put_u64(uint64_t v);
void serial_put_hex64(uint64_t v);

#endif /* YOUSJ_DRIVERS_SERIAL_H */

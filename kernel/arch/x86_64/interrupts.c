/* Yousj OS - IDT, 8259 PIC and PIT setup; exception dump; IRQ0.
 * Written from scratch by Muse (coordinator), milestone M2.
 * Port numbers, command words and the PIT base frequency are the
 * hardware's architectural constants for the PC platform.
 */
#include <yousj/types.h>
#include <yousj/interrupts.h>
#include <yousj/io.h>
#include <yousj/sched.h>
#include <drivers/serial.h>

extern const uint64_t exc_table[32];
extern void irq0_stub(void);

/* --- IDT -------------------------------------------------------------- */

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct idt_entry g_idt[256];

static void idt_set_gate(int vec, uint64_t handler)
{
    struct idt_entry *e = &g_idt[vec];
    e->offset_low = (uint16_t)(handler & 0xFFFF);
    e->selector = 0x08; /* code64 descriptor in the active (stub) GDT;
                           a kernel GDT replaces it together with the
                           UEFI bootloader chain, and this constant
                           moves with it */
    e->ist = 0;
    e->type_attr = 0x8E; /* present, DPL 0, 64-bit interrupt gate */
    e->offset_mid = (uint16_t)((handler >> 16) & 0xFFFF);
    e->offset_high = (uint32_t)((handler >> 32) & 0xFFFFFFFFu);
    e->reserved = 0;
}

/* --- 8259 PIC --------------------------------------------------------- */

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1
#define PIC_EOI   0x20

static void pic_remap_all_masked(void)
{
    uint8_t mask1 = inb(PIC1_DATA);
    uint8_t mask2 = inb(PIC2_DATA);
    (void)mask1;
    (void)mask2;

    outb(PIC1_CMD, 0x11);  io_wait(); /* ICW1: initialise, ICW4 follows */
    outb(PIC2_CMD, 0x11);  io_wait();
    outb(PIC1_DATA, 0x20); io_wait(); /* ICW2: master vector base 0x20 */
    outb(PIC2_DATA, 0x28); io_wait(); /* ICW2: slave vector base 0x28 */
    outb(PIC1_DATA, 0x04); io_wait(); /* ICW3: slave on IRQ2 */
    outb(PIC2_DATA, 0x02); io_wait(); /* ICW3: slave cascade id */
    outb(PIC1_DATA, 0x01); io_wait(); /* ICW4: 8086 mode */
    outb(PIC2_DATA, 0x01); io_wait();

    outb(PIC1_DATA, 0xFF); /* every IRQ masked until explicitly started */
    outb(PIC2_DATA, 0xFF);
}

/* --- PIT ---------------------------------------------------------------- */

#define PIT_CH0 0x40
#define PIT_CMD 0x43
#define PIT_BASE_HZ 1193182ULL

static void pit_start(uint32_t hz)
{
    uint32_t divisor = (uint32_t)(PIT_BASE_HZ / hz);
    outb(PIT_CMD, 0x36); /* channel 0, low+high byte, mode 3 (square) */
    outb(PIT_CH0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CH0, (uint8_t)((divisor >> 8) & 0xFF));
}

/* --- public entry points -------------------------------------------------- */

void interrupts_init(void)
{
    int i;
    struct idt_ptr ptr;

    for (i = 0; i < 32; i++)
        idt_set_gate(i, exc_table[i]);
    idt_set_gate(0x20, (uint64_t)(uintptr_t)&irq0_stub);

    ptr.limit = (uint16_t)(sizeof(g_idt) - 1);
    ptr.base = (uint64_t)(uintptr_t)&g_idt;
    __asm__ volatile("lidt %0" : : "m"(ptr));

    pic_remap_all_masked();
    /* Interrupts stay OFF here; timer_start() performs the sti. */
}

void timer_start(void)
{
    pit_start(YOUSJ_HZ);
    outb(PIC1_DATA, 0xFE); /* unmask IRQ0 (timer) only */
    __asm__ volatile("sti");
}

void irq0_handler(void)
{
    outb(PIC1_CMD, PIC_EOI);
    sched_tick();
}

/* --- exception dump -------------------------------------------------------- */

static bool vec_has_errcode(uint64_t vec)
{
    /* Architectural: these vectors push a hardware error code. */
    return vec == 8 || (vec >= 10 && vec <= 14) || vec == 17 ||
           vec == 21 || vec == 29 || vec == 30;
}

void exception_handler(uint64_t *stack)
{
    uint64_t vec = stack[0];
    uint64_t rip = vec_has_errcode(vec) ? stack[2] : stack[1];
    uint64_t err = vec_has_errcode(vec) ? stack[1] : 0;

    serial_puts("\nYousj OS: EXCEPTION vector ");
    serial_put_u64(vec);
    serial_puts(" error=");
    serial_put_hex64(err);
    serial_puts(" at RIP=");
    serial_put_hex64(rip);
    serial_puts(" -- halting\n");
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

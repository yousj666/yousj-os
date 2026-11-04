/* Yousj OS - interrupt infrastructure (stage 1, milestone M2).
 *
 * Written from scratch by Muse (coordinator), per
 * docs/SCHED_DESIGN.md sec. 4.2 step two: IDT with serial exception
 * dumps, 8259 PIC remapped to vectors 0x20-0x2F, PIT channel 0 as
 * the 100 Hz tick source feeding sched_tick().
 *
 * Bring-up ordering contract (kmain):
 *   1. interrupts_init() installs the IDT and remaps the PIC with
 *      every IRQ masked. Interrupts remain OFF. From this point on,
 *      enabling interrupts anywhere (e.g. in the scheduler's thread
 *      trampoline) is safe: no source can fire yet.
 *   2. timer_start() programs the PIT, unmasks IRQ0 only, and
 *      enables interrupts. Preemption is live from here.
 */
#ifndef YOUSJ_INTERRUPTS_H
#define YOUSJ_INTERRUPTS_H

#include <yousj/types.h>

#define YOUSJ_HZ 100 /* PIT frequency: one tick = 10 ms (design sec. 4.2) */

void interrupts_init(void);
void timer_start(void);

/* M79: device IRQs on the 8259 pair. irq_register() points
 * the line's IDT gate at its stub, unmasks the line (and
 * the cascade for lines 8-15) and records the handler.
 * irq_dispatch() is called from the generic stubs with the
 * line number; it runs the handler (which acknowledges
 * its device) and then sends the EOI(s). Returns false
 * for an unusable line (0 is the timer's, >=16 invalid). */
bool irq_register(uint8_t irq, void (*handler)(void));
void irq_dispatch(uint64_t irq);

/* Called from the assembly stubs in isr.S. exception_handler()
 * receives the stack pointer at stub entry (vector number on top),
 * dumps the fault over serial and halts; it never returns. */
void exception_handler(uint64_t *stack);
void irq0_handler(void);

#endif /* YOUSJ_INTERRUPTS_H */

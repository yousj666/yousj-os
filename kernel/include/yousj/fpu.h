/* Yousj OS - x87/SSE state management (M8).
 *
 * The kernel itself is built -mno-sse/-mno-mmx and never touches
 * the FPU; this layer exists so USER tasks can use x87/SSE
 * freely, each with its own saved state. Switching is eager:
 * arch_on_task_switch() saves the outgoing user task's state
 * and restores the incoming one's on every schedule (512-byte
 * FXSAVE areas, one embedded in each task). CR0.TS-based lazy
 * switching is deliberately not used -- eager costs a bounded
 * ~200 cycles per switch and has no fault-path surprises.
 *
 * The pristine image (what a brand-new or freshly exec'd task
 * starts with) is captured from the CPU itself at fpu_init_cpu()
 * time: fninit for the x87 side, an explicit MXCSR default of
 * 0x1F80 (all SSE exceptions masked, round-to-nearest) for the
 * SSE side -- the state userland expects, and the reason an SSE
 * divide-by-zero yields infinity instead of a #XM fault.
 */
#ifndef YOUSJ_FPU_H
#define YOUSJ_FPU_H

#include <yousj/types.h>

#define YOUSJ_FX_SIZE 512 /* FXSAVE/FXRSTOR area, 16-byte aligned */

/* One-time CPU enablement: CR0.EM off / MP+NE on, CR4.OSFXSR +
 * OSXMMEXCPT on, then capture the pristine image. Call once,
 * before any user task is created. */
void fpu_init_cpu(void);

/* Fill `area` (YOUSJ_FX_SIZE bytes, 16-byte aligned) with the
 * pristine image. */
void fpu_init_area(uint8_t *area);

/* Save/restore the live CPU state to/from an FXSAVE area. */
void fpu_save(uint8_t *area);
void fpu_restore(const uint8_t *area);

#endif /* YOUSJ_FPU_H */

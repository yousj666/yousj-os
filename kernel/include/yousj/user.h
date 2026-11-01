/* Yousj OS - user-process bring-up interface (stage 1, M3/M4).
 *
 * Written from scratch by Muse (coordinator), following
 * docs/SYSCALL_DESIGN.md (entry mechanism, batch order) and
 * docs/SCHED_DESIGN.md sec. 5.4/7 (user task trampoline, iretq
 * landing). This header covers the hand-off between kmain (which
 * builds a user address space and a task), the syscall layer
 * (which owns the entry machinery) and the first user program.
 *
 * User virtual layout for the bring-up stage (identity-kernel
 * transition period -- the kernel still lives in the low half, so
 * the user program gets a PML4 slot far away from it):
 *
 *   YOUSJ_USER_CODE_VA    program image (flat binary in M3, ELF in M4)
 *   YOUSJ_USER_STACK_TOP  top of the user stack (grows down)
 *   YOUSJ_USER_LIMIT      end of the sanctioned user window; the
 *                         syscall layer validates user pointers
 *                         against [YOUSJ_USER_BASE, YOUSJ_USER_LIMIT)
 */
#ifndef YOUSJ_USER_H
#define YOUSJ_USER_H

#include <yousj/types.h>

#define YOUSJ_USER_BASE      0x8000000000ULL
#define YOUSJ_USER_CODE_VA   YOUSJ_USER_BASE
#define YOUSJ_USER_STACK_TOP 0x8000010000ULL
#define YOUSJ_USER_LIMIT     YOUSJ_USER_STACK_TOP

/* Landing data for a user task's first run, passed as the arg of
 * user_task_start(). */
struct user_start {
    uint64_t ip; /* first ring-3 instruction (image entry) */
    uint64_t sp; /* initial ring-3 stack pointer */
};

/* One-time syscall entry setup: EFER.SCE, STAR, LSTAR, SFMASK and
 * the kernel GS base (cpu_local). Call before the first user task
 * is scheduled. */
void syscall_init(void);

/* Kernel-thread entry that performs the ring drop for a user task
 * (records ip/sp on the task, then enters user mode; never
 * returns -- the task leaves through the exit syscall). */
void user_task_start(void *arg);

/* Assembly (syscall_entry.S): switch to user mode at ip/sp with
 * the user segment selectors; never returns. */
void enter_user_mode(uint64_t ip, uint64_t sp);

#endif /* YOUSJ_USER_H */

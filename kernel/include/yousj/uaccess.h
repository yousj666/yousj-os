/* uaccess.h - user-access fault recovery (M24).
 *
 * Kernel code dereferences user pointers after range
 * checks, but a range check cannot prove the pages are
 * present or writable (a pointer at a read-only page, or
 * at never-mapped window space, passes user_range_ok and
 * then faults in supervisor mode). Before M24 that fault
 * halted the whole kernel -- any program could kill the
 * machine with one bad argument (M21 found this by
 * accident through wait4's rusage).
 *
 * The fix is a per-task recovery pad: while a syscall
 * runs, the dispatcher arms the pad with a setjmp in its
 * own frame. If a kernel-mode fault on a user address
 * cannot be resolved by demand paging, the fault path
 * longjmps back to the pad and the syscall returns
 * -EFAULT. User-mode faults never take that path, so a
 * genuine SEGV is unaffected.
 *
 * Usage:
 *   if (UACCESS_TRY(me))
 *       return -YOUSJ_EFAULT;   // recovered fault
 *   ... dispatch / copies ...
 *   UACCESS_END(me);
 *
 * The setjmp must live in a frame that stays on the stack
 * for the entire armed region. Locals written between the
 * TRY and a possible recovery and read afterwards must be
 * volatile (setjmp rules). */
#ifndef YOUSJ_UACCESS_H
#define YOUSJ_UACCESS_H

#include <yousj/sched.h>

#define UACCESS_TRY(task)                                             \
    ((task)->uaccess_armed = 1,                                       \
     __builtin_setjmp((void *)(task)->uaccess_jb))

#define UACCESS_END(task) ((task)->uaccess_armed = 0)

#endif /* YOUSJ_UACCESS_H */

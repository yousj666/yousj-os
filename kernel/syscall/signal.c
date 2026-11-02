/* Yousj OS - signal machinery (M7). Written by Muse
 * (coordinator).
 *
 * Implements the delivery model documented in <yousj/signal.h>:
 * signals are queued by signal_send() and acted on only at the
 * two return-to-user gates (syscall exit, timer-IRQ return).
 * The gates share gate_run(), which works on a register snapshot
 * so the two call sites -- the dispatcher's saved frame and the
 * IRQ stub's register pieces -- see exactly the same logic.
 *
 * Stage-1 simplifications, deliberate and documented:
 *   - standard signals only; standard signals coalesce (one
 *     pending bit per signal, last sender wins), as on Linux;
 *   - no credentials: any task may signal any user task;
 *   - no alternate signal stack (sigaltstack is -ENOSYS);
 *   - "core dump" dispositions only set the wait-status bit;
 *     no core file is written (there is no filesystem yet);
 *   - SA_RESTART re-executes the interrupted syscall from
 *     scratch (rt_sigreturn rewinds RIP onto the syscall
 *     instruction), so a restarted nanosleep sleeps its full
 *     interval again; Linux resumes only the remainder. The
 *     user-visible contract (call completes, handler ran once)
 *     is unchanged.
 */
#include <yousj/types.h>
#include <yousj/signal.h>
#include <yousj/sched.h>
#include <yousj/user.h>
#include <stddef.h> /* offsetof */

#define YOUSJ_EINTR  4
#define YOUSJ_ESRCH  3
#define YOUSJ_EINVAL 22
#define YOUSJ_EFAULT 14
#define YOUSJ_ERESTARTSYS 512 /* internal only; never a user return */

/* --- dispositions ---------------------------------------------------- */

enum disp {
    DISP_TERM,    /* default: terminate */
    DISP_CORE,    /* default: terminate + core status bit */
    DISP_IGNORE,  /* default (or SIG_IGN): discard */
    DISP_STOP,    /* default: stop the task */
    DISP_CONT,    /* default: continue (send side already acted) */
    DISP_HANDLER  /* caught: deliver to the user handler */
};

static const uint8_t g_default_disp[YOUSJ_NSIG] = {
    [YOUSJ_SIGHUP]    = DISP_TERM,   [YOUSJ_SIGINT]  = DISP_TERM,
    [YOUSJ_SIGQUIT]   = DISP_CORE,   [YOUSJ_SIGILL]  = DISP_CORE,
    [YOUSJ_SIGTRAP]   = DISP_CORE,   [YOUSJ_SIGABRT] = DISP_CORE,
    [YOUSJ_SIGBUS]    = DISP_CORE,   [YOUSJ_SIGFPE]  = DISP_CORE,
    [YOUSJ_SIGKILL]   = DISP_TERM,   [YOUSJ_SIGUSR1] = DISP_TERM,
    [YOUSJ_SIGSEGV]   = DISP_CORE,   [YOUSJ_SIGUSR2] = DISP_TERM,
    [YOUSJ_SIGPIPE]   = DISP_TERM,   [YOUSJ_SIGALRM] = DISP_TERM,
    [YOUSJ_SIGTERM]   = DISP_TERM,   [YOUSJ_SIGSTKFLT] = DISP_TERM,
    [YOUSJ_SIGCHLD]   = DISP_IGNORE, [YOUSJ_SIGCONT] = DISP_CONT,
    [YOUSJ_SIGSTOP]   = DISP_STOP,   [YOUSJ_SIGTSTP] = DISP_STOP,
    [YOUSJ_SIGTTIN]   = DISP_STOP,   [YOUSJ_SIGTTOU] = DISP_STOP,
    [YOUSJ_SIGURG]    = DISP_IGNORE, [YOUSJ_SIGXCPU] = DISP_CORE,
    [YOUSJ_SIGXFSZ]   = DISP_CORE,   [YOUSJ_SIGVTALRM] = DISP_TERM,
    [YOUSJ_SIGPROF]   = DISP_TERM,   [YOUSJ_SIGWINCH]  = DISP_IGNORE,
    [YOUSJ_SIGIO]     = DISP_TERM,   [YOUSJ_SIGPWR]    = DISP_TERM,
    [YOUSJ_SIGSYS]    = DISP_CORE,
};

static enum disp disposition_of(struct task *t, int sig)
{
    if (sig == YOUSJ_SIGKILL)
        return DISP_TERM; /* uncatchable, unblockable */
    if (sig == YOUSJ_SIGSTOP)
        return DISP_STOP;
    if (t->sig_act[sig].handler == YOUSJ_SIG_IGN)
        return DISP_IGNORE;
    if (t->sig_act[sig].handler != YOUSJ_SIG_DFL)
        return DISP_HANDLER;
    return (enum disp)g_default_disp[sig];
}

int signal_is_core(int sig)
{
    if (sig < 1 || sig >= YOUSJ_NSIG)
        return 0;
    return g_default_disp[sig] == DISP_CORE;
}

/* Pending signals a gate would act on. SIGKILL/SIGSTOP ignore
 * the blocked mask by definition. Ignore-disposition signals
 * never reach the pending mask (signal_send discards them), so
 * they need no filtering here. */
static uint64_t deliverable_mask(struct task *t)
{
    uint64_t d = t->sig_pending & ~t->sig_blocked;
    if (t->sig_pending & yousj_sig_bit(YOUSJ_SIGKILL))
        d |= yousj_sig_bit(YOUSJ_SIGKILL);
    if (t->sig_pending & yousj_sig_bit(YOUSJ_SIGSTOP))
        d |= yousj_sig_bit(YOUSJ_SIGSTOP);
    return d;
}

int signal_deliverable_pending(struct task *t)
{
    if (t == NULL || t->mm == NULL)
        return 0;
    return deliverable_mask(t) != 0;
}

static int is_stop_signal(int sig)
{
    return sig == YOUSJ_SIGSTOP || sig == YOUSJ_SIGTSTP ||
           sig == YOUSJ_SIGTTIN || sig == YOUSJ_SIGTTOU;
}

#define STOP_MASK (yousj_sig_bit(YOUSJ_SIGSTOP) | \
                   yousj_sig_bit(YOUSJ_SIGTSTP) | \
                   yousj_sig_bit(YOUSJ_SIGTTIN) | \
                   yousj_sig_bit(YOUSJ_SIGTTOU))
#define UNBLOCKABLE_MASK (yousj_sig_bit(YOUSJ_SIGKILL) | \
                          yousj_sig_bit(YOUSJ_SIGSTOP))
#define VALID_MASK 0x7FFFFFFFULL /* bits for signals 1..31 */

/* --- sending ----------------------------------------------------------- */

int signal_send(struct task *t, int sig, int from_tid)
{
    if (t == NULL || !t->in_use || sig < 1 || sig >= YOUSJ_NSIG)
        return -YOUSJ_ESRCH;
    if (t->mm == NULL)
        return -YOUSJ_EINVAL; /* kernel threads: no signal gates */

    /* Ignored signals are discarded at the source (Linux drops
     * them without waking anyone). SIGKILL/SIGSTOP can never be
     * ignored, so they skip this test. */
    if (sig != YOUSJ_SIGKILL && sig != YOUSJ_SIGSTOP &&
        disposition_of(t, sig) == DISP_IGNORE)
        return 0;

    /* SIGCONT and the stop signals cancel each other (Linux). */
    if (sig == YOUSJ_SIGCONT)
        t->sig_pending &= ~STOP_MASK;
    if (is_stop_signal(sig))
        t->sig_pending &= ~yousj_sig_bit(YOUSJ_SIGCONT);

    t->sig_pending |= yousj_sig_bit(sig);
    t->sig_sender[sig] = from_tid;

    if (sig == YOUSJ_SIGCONT)
        sched_cont_task(t);
    if (sig == YOUSJ_SIGKILL && t->sig_stopped)
        sched_cont_task(t); /* unstick: it dies at its gate */

    /* Interrupt an in-progress wait when something deliverable
     * now exists. The waiter re-checks its own condition first
     * (wait4 scans for its zombie before honouring sig_wake), so
     * an optimistic wake is never a lost wake. Parked (stopped)
     * tasks are excluded: a queued signal never un-parks them --
     * only SIGCONT (or SIGKILL, handled above) releases a stop,
     * and a signal queued meanwhile is gated when they resume. */
    if (t->state == TASK_BLOCKED && !t->sig_parked &&
        deliverable_mask(t) != 0) {
        t->sig_wake = true;
        t->wake_tick = 0;
        sched_wake(t);
    }
    return 0;
}

/* --- inheritance ------------------------------------------------------- */

void signal_clone_inherit(struct task *child, struct task *parent)
{
    int i;
    child->sig_blocked = parent->sig_blocked;
    for (i = 0; i < YOUSJ_NSIG; i++)
        child->sig_act[i] = parent->sig_act[i];
    child->sig_pending = 0; /* pending signals are not inherited */
}

void signal_exec_reset(struct task *t)
{
    int i;
    for (i = 0; i < YOUSJ_NSIG; i++) {
        if (t->sig_act[i].handler != YOUSJ_SIG_IGN)
            t->sig_act[i].handler = YOUSJ_SIG_DFL;
    }
    /* Blocked mask and pending signals survive exec (Linux). */
}

/* --- the delivery gate --------------------------------------------------- */

/* A full ring-3 register snapshot, the currency between
 * gate_run() and its two call sites. */
struct sig_snapshot {
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp, rip, eflags;
};

static void mem_zero_bytes(void *dst, uint64_t n)
{
    uint8_t *p = (uint8_t *)dst;
    while (n--)
        *p++ = 0;
}

/* Build the rt_sigframe on the user stack and redirect *snap
 * into the handler. Returns 0 on success, -1 when delivery is
 * impossible (no restorer, or the frame would leave the user
 * window) -- the caller then falls back to the default action. */
static int build_frame(struct task *me, int sig,
                       struct sig_snapshot *snap, int restart,
                       uint64_t syscall_nr)
{
    struct yousj_sigaction *act = &me->sig_act[sig];
    uint64_t flen = sizeof(struct yousj_rt_sigframe);
    uint64_t f, x;
    struct yousj_rt_sigframe *fr;
    struct yousj_sigcontext *mc;

    if (act->restorer == 0)
        return -1;
    f = (snap->rsp - flen) & ~15ULL;
    x = f - 8;
    if (!yousj_user_range_ok(x, flen + 8))
        return -1;

    fr = (struct yousj_rt_sigframe *)(uintptr_t)f;
    mem_zero_bytes(fr, flen);
    fr->pretcode = act->restorer;
    fr->uc.uc_flags = restart ? YOUSJ_UC_RESTART : 0;
    fr->uc.uc_sigmask = me->sig_blocked;
    mc = &fr->uc.uc_mcontext;
    mc->r8 = snap->r8;   mc->r9 = snap->r9;
    mc->r10 = snap->r10; mc->r11 = snap->r11;
    mc->r12 = snap->r12; mc->r13 = snap->r13;
    mc->r14 = snap->r14; mc->r15 = snap->r15;
    mc->rdi = snap->rdi; mc->rsi = snap->rsi;
    mc->rbp = snap->rbp; mc->rbx = snap->rbx;
    mc->rdx = snap->rdx; mc->rax = snap->rax;
    mc->rcx = snap->rcx; mc->rsp = snap->rsp;
    mc->rip = snap->rip; mc->eflags = snap->eflags;
    mc->cs = 0x23; /* canonical user selectors (GDT final layout) */
    mc->ss = 0x1B;
    fr->info.si_signo = sig;
    fr->info.si_code = -1; /* SI_USER */
    fr->info.si_pid = me->sig_sender[sig];
    fr->info.si_uid = 0;
    if (sig == YOUSJ_SIGCHLD)
        fr->info.si_status = me->sig_chld_status;
    fr->restart_nr = syscall_nr;
    *(uint64_t *)(uintptr_t)x = act->restorer;

    /* The signal is masked while its handler runs (plus the
     * action's own mask), unless SA_NODEFER. */
    if (!(act->flags & YOUSJ_SA_NODEFER))
        me->sig_blocked |= yousj_sig_bit(sig);
    me->sig_blocked |= act->mask;
    me->sig_blocked &= ~UNBLOCKABLE_MASK;
    if (act->flags & YOUSJ_SA_RESETHAND)
        act->handler = YOUSJ_SIG_DFL;

    snap->rip = act->handler;
    snap->rsp = x;
    snap->rdi = (uint64_t)sig;
    snap->rsi = f + offsetof(struct yousj_rt_sigframe, info);
    snap->rdx = f + offsetof(struct yousj_rt_sigframe, uc);
    return 0;
}

/* Run the gate over *snap for the current task. `result` is the
 * interrupted syscall's outcome on the syscall path (possibly
 * the internal -ERESTARTSYS marker), or a scratch 0 on the IRQ
 * path, where restart cannot apply. Returns 1 when a handler
 * frame was built into *snap (caller writes the snapshot back),
 * 0 when nothing was delivered. May block (stop) or not return
 * (terminate). *result_io is updated for EINTR conversion. */
static int gate_run(struct task *me, struct sig_snapshot *snap,
                    long *result_io, uint64_t syscall_nr)
{
    for (;;) {
        uint64_t d;
        int sig;
        enum disp disp;

        /* A stop order that arrived while this task was blocked
         * somewhere else takes effect here, before any return
         * to ring 3. sched_self_park() returns after SIGCONT. */
        if (me->sig_stopped && !me->sig_parked) {
            sched_self_park();
            continue;
        }

        d = deliverable_mask(me);
        if (d == 0)
            return 0;
        sig = (int)__builtin_ctzll(d) + 1;
        disp = disposition_of(me, sig);

        switch (disp) {
        case DISP_IGNORE:
        case DISP_CONT:
            me->sig_pending &= ~yousj_sig_bit(sig);
            continue;
        case DISP_STOP:
            me->sig_pending &= ~yousj_sig_bit(sig);
            sched_self_park();
            if (*result_io == -YOUSJ_ERESTARTSYS)
                *result_io = -YOUSJ_EINTR;
            continue;
        case DISP_TERM:
        case DISP_CORE:
            process_exit(0, sig);
            __builtin_unreachable();
        case DISP_HANDLER: {
            int restart = 0;
            struct sig_snapshot s2 = *snap;

            me->sig_pending &= ~yousj_sig_bit(sig);
            if (*result_io == -YOUSJ_ERESTARTSYS) {
                if (me->sig_act[sig].flags & YOUSJ_SA_RESTART)
                    restart = 1;
                else
                    *result_io = -YOUSJ_EINTR;
            }
            /* The mcontext must carry the value ring 3 will
             * finally see for the interrupted call: the restart
             * marker for a restartable interruption, else the
             * (already EINTR-converted) result. The IRQ path has
             * no interrupted call, so its rax is the user's. */
            s2.rax = restart ? (uint64_t)(int64_t)-YOUSJ_ERESTARTSYS
                             : (syscall_nr != ~(uint64_t)0
                                    ? (uint64_t)(int64_t)*result_io
                                    : snap->rax);
            if (build_frame(me, sig, &s2, restart, syscall_nr) != 0) {
                /* No restorer / no stack room: the handler can
                 * never run, so the signal's default action is
                 * all that is left. */
                process_exit(0, sig);
                __builtin_unreachable();
            }
            *snap = s2;
            return 1;
        }
        }
    }
}

/* --- gate 1: syscall exit ---------------------------------------------- */

long signal_syscall_exit(struct yousj_syscall_ctx *ctx, long result)
{
    struct task *me = sched_current();
    struct sig_snapshot snap;
    uint64_t *fr;

    if (me == NULL || me->mm == NULL)
        return result;
    me->sig_wake = false; /* any leftover wake marker is spent */

    if (!me->sig_stopped && deliverable_mask(me) == 0 &&
        result != -YOUSJ_ERESTARTSYS)
        return result;

    fr = ctx->frame;
    snap.r9  = fr[YOUSJ_FR_R9];
    snap.rsp = fr[YOUSJ_FR_RSP];
    snap.r8  = fr[YOUSJ_FR_R8];
    snap.r10 = fr[YOUSJ_FR_R10];
    snap.rdx = fr[YOUSJ_FR_RDX];
    snap.rsi = fr[YOUSJ_FR_RSI];
    snap.rdi = fr[YOUSJ_FR_RDI];
    snap.rip = fr[YOUSJ_FR_RIP];
    snap.rcx = fr[YOUSJ_FR_RIP]; /* syscall destroyed user rcx */
    snap.eflags = fr[YOUSJ_FR_RFLAGS];
    /* User r11 carries no value across a syscall either: the
     * hardware itself loaded it with RFLAGS at entry, which is
     * exactly what the frame slot holds. */
    snap.r11 = fr[YOUSJ_FR_RFLAGS];
    snap.rbx = ctx->callee[0];
    snap.rbp = ctx->callee[1];
    snap.r12 = ctx->callee[2];
    snap.r13 = ctx->callee[3];
    snap.r14 = ctx->callee[4];
    snap.r15 = ctx->callee[5];
    snap.rax = (uint64_t)(int64_t)result;

    if (gate_run(me, &snap, &result, ctx->no)) {
        fr[YOUSJ_FR_RIP] = snap.rip;
        fr[YOUSJ_FR_RSP] = snap.rsp;
        fr[YOUSJ_FR_RDI] = snap.rdi;
        fr[YOUSJ_FR_RSI] = snap.rsi;
        fr[YOUSJ_FR_RDX] = snap.rdx;
    }
    /* The restart marker must never leak to ring 3 as a return
     * value: it only ever lives inside a signal frame. */
    if (result == -YOUSJ_ERESTARTSYS)
        result = -YOUSJ_EINTR;
    return result;
}

/* --- gate 2: timer-IRQ return -------------------------------------------- */

void signal_irq_check(uint64_t *saved, uint64_t *hw, uint64_t *callee_mem)
{
    struct task *me = sched_current();
    struct sig_snapshot snap;
    long scratch = 0;

    if (me == NULL || me->mm == NULL)
        return;
    if (hw[1] != 0x23)
        return; /* kernel frame (or QEMU's fabricated 0x1B was
                   already canonicalized to 0x23 by irq0_stub);
                   a user task inside a syscall is gated at its
                   syscall exit instead */
    if (!me->sig_stopped && deliverable_mask(me) == 0)
        return;

    /* saved[] memory order (irq0_stub pushes): [0] r11,
     * [1] r10, [2] r9, [3] r8, [4] rdi, [5] rsi, [6] rdx,
     * [7] rcx, [8] rax. callee_mem[]: [0] r15 ... [5] rbx. */
    snap.r11 = saved[0];
    snap.r10 = saved[1];
    snap.r9  = saved[2];
    snap.r8  = saved[3];
    snap.rdi = saved[4];
    snap.rsi = saved[5];
    snap.rdx = saved[6];
    snap.rcx = saved[7];
    snap.rax = saved[8];
    snap.r15 = callee_mem[0];
    snap.r14 = callee_mem[1];
    snap.r13 = callee_mem[2];
    snap.r12 = callee_mem[3];
    snap.rbp = callee_mem[4];
    snap.rbx = callee_mem[5];
    snap.rip = hw[0];
    snap.eflags = hw[2];
    snap.rsp = hw[3];

    if (gate_run(me, &snap, &scratch, ~(uint64_t)0)) {
        hw[0] = snap.rip;
        hw[3] = snap.rsp;
        saved[4] = snap.rdi;
        saved[5] = snap.rsi;
        saved[6] = snap.rdx;
    }
}

/* --- signal syscalls ----------------------------------------------------- */

static long copy_action_in(struct yousj_sigaction *dst, uint64_t uptr)
{
    if (!yousj_user_range_ok(uptr, sizeof(*dst)))
        return -YOUSJ_EFAULT;
    *dst = *(const struct yousj_sigaction *)(uintptr_t)uptr;
    return 0;
}

static long copy_action_out(uint64_t uptr,
                            const struct yousj_sigaction *src)
{
    if (!yousj_user_range_ok(uptr, sizeof(*src)))
        return -YOUSJ_EFAULT;
    *(struct yousj_sigaction *)(uintptr_t)uptr = *src;
    return 0;
}

long sys_rt_sigaction(long sig, uint64_t act_ptr, uint64_t old_ptr,
                      uint64_t sigsetsize)
{
    struct task *me = sched_current();
    struct yousj_sigaction in;
    long rc;

    if (me == NULL || me->mm == NULL)
        return -YOUSJ_EFAULT;
    if (sig < 1 || sig >= YOUSJ_NSIG || sigsetsize != 8)
        return -YOUSJ_EINVAL;
    if (act_ptr != 0 && (sig == YOUSJ_SIGKILL || sig == YOUSJ_SIGSTOP))
        return -YOUSJ_EINVAL; /* uncatchable */
    if (old_ptr != 0) {
        rc = copy_action_out(old_ptr, &me->sig_act[sig]);
        if (rc != 0)
            return rc;
    }
    if (act_ptr != 0) {
        rc = copy_action_in(&in, act_ptr);
        if (rc != 0)
            return rc;
        if (in.handler != YOUSJ_SIG_DFL && in.handler != YOUSJ_SIG_IGN &&
            !yousj_user_range_ok(in.handler, 1))
            return -YOUSJ_EINVAL;
        if (in.restorer != 0 && !yousj_user_range_ok(in.restorer, 1))
            return -YOUSJ_EINVAL;
        in.mask &= VALID_MASK & ~UNBLOCKABLE_MASK;
        me->sig_act[sig] = in;
    }
    return 0;
}

long sys_rt_sigprocmask(long how, uint64_t set_ptr, uint64_t old_ptr,
                        uint64_t sigsetsize)
{
    struct task *me = sched_current();
    uint64_t set;

    if (me == NULL || me->mm == NULL)
        return -YOUSJ_EFAULT;
    if (sigsetsize != 8)
        return -YOUSJ_EINVAL;
    if (old_ptr != 0) {
        if (!yousj_user_range_ok(old_ptr, 8))
            return -YOUSJ_EFAULT;
        *(uint64_t *)(uintptr_t)old_ptr = me->sig_blocked;
    }
    if (set_ptr != 0) {
        if (!yousj_user_range_ok(set_ptr, 8))
            return -YOUSJ_EFAULT;
        set = *(const uint64_t *)(uintptr_t)set_ptr;
        set &= VALID_MASK & ~UNBLOCKABLE_MASK;
        switch (how) {
        case YOUSJ_SIG_BLOCK:   me->sig_blocked |= set;  break;
        case YOUSJ_SIG_UNBLOCK: me->sig_blocked &= ~set; break;
        case YOUSJ_SIG_SETMASK: me->sig_blocked = set;   break;
        default: return -YOUSJ_EINVAL;
        }
    }
    return 0;
}

long sys_rt_sigpending(uint64_t set_ptr, uint64_t sigsetsize)
{
    struct task *me = sched_current();

    if (me == NULL || me->mm == NULL)
        return -YOUSJ_EFAULT;
    if (sigsetsize != 8)
        return -YOUSJ_EINVAL;
    if (!yousj_user_range_ok(set_ptr, 8))
        return -YOUSJ_EFAULT;
    *(uint64_t *)(uintptr_t)set_ptr = me->sig_pending & VALID_MASK;
    return 0;
}

long sys_rt_sigreturn(struct yousj_syscall_ctx *ctx)
{
    struct task *me = sched_current();
    uint64_t f = ctx->frame[YOUSJ_FR_RSP];
    const struct yousj_rt_sigframe *fr;
    const struct yousj_sigcontext *mc;
    uint64_t *slots = ctx->frame;

    if (me == NULL || me->mm == NULL)
        return -YOUSJ_EFAULT;
    /* The restorer entered the syscall with RSP == frame base
     * (see the frame contract in <yousj/signal.h>). */
    if (!yousj_user_range_ok(f, sizeof(struct yousj_rt_sigframe)))
        return -YOUSJ_EFAULT;
    fr = (const struct yousj_rt_sigframe *)(uintptr_t)f;
    mc = &fr->uc.uc_mcontext;
    /* The context in the frame is user-writable, so its return
     * address and stack must stay inside the user window, and
     * RFLAGS is rebuilt from the harmless bits (status flags +
     * DF/TF) with IF forced on -- the rest of RFLAGS is not
     * userland's to choose. */
    if (!yousj_user_range_ok(mc->rip, 1) ||
        !yousj_user_range_ok(mc->rsp, 8))
        return -YOUSJ_EFAULT;

    me->sig_blocked = fr->uc.uc_sigmask & VALID_MASK &
                      ~UNBLOCKABLE_MASK;

    slots[YOUSJ_FR_RIP] = mc->rip;
    slots[YOUSJ_FR_RSP] = mc->rsp;
    slots[YOUSJ_FR_RFLAGS] = (mc->eflags & 0xFD5ULL) | 0x202ULL;
    slots[YOUSJ_FR_RDI] = mc->rdi;
    slots[YOUSJ_FR_RSI] = mc->rsi;
    slots[YOUSJ_FR_RDX] = mc->rdx;
    slots[YOUSJ_FR_R10] = mc->r10;
    slots[YOUSJ_FR_R8] = mc->r8;
    slots[YOUSJ_FR_R9] = mc->r9;
    ctx->callee[0] = mc->rbx;
    ctx->callee[1] = mc->rbp;
    ctx->callee[2] = mc->r12;
    ctx->callee[3] = mc->r13;
    ctx->callee[4] = mc->r14;
    ctx->callee[5] = mc->r15;

    if (fr->uc.uc_flags & YOUSJ_UC_RESTART) {
        /* Re-execute the interrupted syscall: RIP back onto the
         * syscall instruction (2 bytes), rax = its number; the
         * argument registers were restored just above. */
        slots[YOUSJ_FR_RIP] = mc->rip - 2;
        return (long)fr->restart_nr;
    }
    return (long)mc->rax;
}

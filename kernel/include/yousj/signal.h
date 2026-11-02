/* Yousj OS - signal ABI and kernel signal interface (M7).
 *
 * Numbers, flag values and structure layouts follow the Linux
 * x86-64 ABI (the same policy as syscall.h: compatible numbers
 * and layouts, implementation written from scratch). Only the
 * standard signal set (1..31) exists at this stage; real-time
 * signals are not implemented.
 *
 * Delivery model: pending signals are acted on at two return-to-
 * user gates -- the syscall exit gate and the timer-IRQ return
 * gate -- never asynchronously in arbitrary kernel context.
 * Default dispositions (terminate/stop/continue/ignore) are
 * executed by the kernel at the gate; a caught signal redirects
 * the interrupted context into the handler through an
 * rt_sigframe built on the user stack, and rt_sigreturn()
 * restores the saved context from that frame.
 */
#ifndef YOUSJ_SIGNAL_H
#define YOUSJ_SIGNAL_H

#include <yousj/types.h>

/* Signal numbers (Linux x86-64). */
#define YOUSJ_SIGHUP  1
#define YOUSJ_SIGINT  2
#define YOUSJ_SIGQUIT 3
#define YOUSJ_SIGILL  4
#define YOUSJ_SIGTRAP 5
#define YOUSJ_SIGABRT 6
#define YOUSJ_SIGBUS  7
#define YOUSJ_SIGFPE  8
#define YOUSJ_SIGKILL 9
#define YOUSJ_SIGUSR1 10
#define YOUSJ_SIGSEGV 11
#define YOUSJ_SIGUSR2 12
#define YOUSJ_SIGPIPE 13
#define YOUSJ_SIGALRM 14
#define YOUSJ_SIGTERM 15
#define YOUSJ_SIGSTKFLT 16
#define YOUSJ_SIGCHLD 17
#define YOUSJ_SIGCONT 18
#define YOUSJ_SIGSTOP 19
#define YOUSJ_SIGTSTP 20
#define YOUSJ_SIGTTIN 21
#define YOUSJ_SIGTTOU 22
#define YOUSJ_SIGURG  23
#define YOUSJ_SIGXCPU 24
#define YOUSJ_SIGXFSZ 25
#define YOUSJ_SIGVTALRM 26
#define YOUSJ_SIGPROF 27
#define YOUSJ_SIGWINCH 28
#define YOUSJ_SIGIO   29
#define YOUSJ_SIGPWR  30
#define YOUSJ_SIGSYS  31

#define YOUSJ_NSIG 32 /* valid signals are 1..31; slot 0 unused */

static inline uint64_t yousj_sig_bit(int sig)
{
    return 1ULL << (sig - 1);
}

/* Handler field values (as in Linux: 0 = default, 1 = ignore). */
#define YOUSJ_SIG_DFL 0ULL
#define YOUSJ_SIG_IGN 1ULL

/* sa_flags subset (Linux values). */
#define YOUSJ_SA_SIGINFO   0x00000004u
#define YOUSJ_SA_RESTORER  0x04000000u
#define YOUSJ_SA_RESTART   0x10000000u
#define YOUSJ_SA_NODEFER   0x40000000u
#define YOUSJ_SA_RESETHAND 0x80000000u

/* rt_sigprocmask how values (Linux). */
#define YOUSJ_SIG_BLOCK   0
#define YOUSJ_SIG_UNBLOCK 1
#define YOUSJ_SIG_SETMASK 2

/* The kernel sigaction, exactly as rt_sigaction copies it in and
 * out (the rt form: callers must pass sigsetsize == 8, so sa_mask
 * is a single 64-bit mask). The task's per-signal table stores
 * this same struct. */
struct yousj_sigaction {
    uint64_t handler;  /* YOUSJ_SIG_DFL / YOUSJ_SIG_IGN / user VA */
    uint32_t flags;    /* YOUSJ_SA_* */
    uint32_t reserved;
    uint64_t restorer; /* user VA of the rt_sigreturn trampoline */
    uint64_t mask;     /* extra signals blocked while handler runs */
};

/* Register context saved in a signal frame. Field order mirrors
 * the Linux kernel's struct sigcontext (r8..rip, eflags, then the
 * segment/auxiliary tail), so the layout will not need to change
 * when real Linux userland arrives. */
struct yousj_sigcontext {
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp, rip;
    uint64_t eflags;
    uint16_t cs, gs, fs, ss;
    uint32_t reserved_seg;
    uint64_t err, trapno, oldmask, cr2;
    uint64_t fpstate; /* always 0: no FPU state in frames yet */
    uint64_t reserved1[8];
};

struct yousj_ucontext {
    uint64_t uc_flags;
    uint64_t uc_link;
    uint64_t uc_stack_sp;
    int32_t  uc_stack_flags;
    int32_t  uc_stack_pad;
    uint64_t uc_stack_size;
    struct yousj_sigcontext uc_mcontext;
    uint64_t uc_sigmask; /* blocked mask at delivery time */
};

/* siginfo: fixed 128-byte Linux layout. Only the fields named
 * here are filled (signo/errno/code always; pid/uid for kill and
 * SIGCHLD; status for SIGCHLD); the rest stays zero. */
struct yousj_siginfo {
    int32_t  si_signo;
    int32_t  si_errno;
    int32_t  si_code;
    int32_t  pad0;
    int32_t  si_pid;    /* sender tid (kill) / child tid (SIGCHLD) */
    uint32_t si_uid;
    int32_t  si_status; /* SIGCHLD: child wait status word */
    int32_t  pad1;
    uint8_t  rest[128 - 32];
};

/* What the kernel builds on the user stack at delivery. The
 * handler is entered with RSP pointing 8 bytes BELOW this struct
 * (the slot there holds the restorer address, as a call would
 * leave it), and the struct itself starts at that RSP + 8 -- which
 * is exactly the user RSP the restorer presents to rt_sigreturn,
 * so the kernel finds the frame again without any user help.
 * restart_nr is a Yousj-private tail field: when uc_flags carries
 * YOUSJ_UC_RESTART, rt_sigreturn rewinds RIP onto the interrupted
 * syscall instruction and returns restart_nr as rax, which makes
 * the entry stub re-execute the syscall (SA_RESTART). */
#define YOUSJ_UC_RESTART (1ULL << 63)

struct yousj_rt_sigframe {
    uint64_t pretcode; /* = restorer VA (informational) */
    struct yousj_ucontext uc;
    struct yousj_siginfo info;
    uint64_t restart_nr;
};

/* The syscall context the entry shim (syscall_entry.S) hands to
 * the dispatcher: arguments as registers carried them, a pointer
 * to the 9 entry-pushed frame slots, and the user callee-saved
 * registers the dispatcher chain preserved. Field offsets are
 * CONTRACT with the shim's stack layout. */
struct yousj_syscall_ctx {
    uint64_t no;        /* +0 */
    uint64_t a1, a2, a3; /* +8, +16, +24 */
    uint64_t a4, a5, a6; /* +32, +40, +48 */
    uint64_t *frame;    /* +56: slots below */
    uint64_t callee[6]; /* +64: rbx, rbp, r12, r13, r14, r15 */
};

/* Entry frame slot indices (syscall_entry.S push order). */
#define YOUSJ_FR_R9    0
#define YOUSJ_FR_RSP   1
#define YOUSJ_FR_R8    2
#define YOUSJ_FR_R10   3
#define YOUSJ_FR_RDX   4
#define YOUSJ_FR_RSI   5
#define YOUSJ_FR_RDI   6
#define YOUSJ_FR_RIP   7 /* rcx slot: user RIP */
#define YOUSJ_FR_RFLAGS 8 /* r11 slot: user RFLAGS */

struct task; /* <yousj/sched.h> */

/* Queue a signal on a task and apply the immediate effects
 * (waking a blocked task whose wait the signal interrupts, un-
 * parking on SIGCONT, unsticking a stopped task for SIGKILL).
 * Returns 0, or a negative errno (-ESRCH/-EINVAL shaped as the
 * kernel's small positive-magnitude style used in syscall.c). */
int signal_send(struct task *t, int sig, int from_tid);

/* Nonzero when the task has a pending signal that is not blocked
 * and whose disposition is not "ignore" -- i.e. a delivery gate
 * would act on something. Blocking syscalls consult this to
 * decide on an -ERESTARTSYS return. */
int signal_deliverable_pending(struct task *t);

/* Nonzero for signals whose default action dumps core (their
 * wait4 status word carries the 0x80 bit). */
int signal_is_core(int sig);

/* The dispatcher's syscall-exit gate (see delivery model above).
 * `result` is the syscall's return value, possibly the internal
 * -ERESTARTSYS marker from an interrupted blocking call. Returns
 * the final rax for the return to ring 3; may not return at all
 * (default terminate / SIGKILL / init panic). */
long signal_syscall_exit(struct yousj_syscall_ctx *ctx, long result);

/* Timer-IRQ return gate, called from irq0_stub's tail with the
 * interrupted context in pieces: saved = the 9 pushed caller-
 * saved registers in memory order ([0] = r11 ... [8] = rax),
 * hw = the hardware frame (rip, cs, rflags, rsp, ss), callee_mem
 * = the 6 extra pushes the stub made ([0] = r15 ... [40] = rbx).
 * Only acts when the interrupted frame is a ring-3 frame of a
 * user task; may block (stop) or exit (terminate) the task. */
void signal_irq_check(uint64_t *saved, uint64_t *hw,
                      uint64_t *callee_mem);

/* Signal syscall handlers (dispatched from syscall.c). */
long sys_rt_sigaction(long sig, uint64_t act_ptr, uint64_t old_ptr,
                      uint64_t sigsetsize);
long sys_rt_sigprocmask(long how, uint64_t set_ptr, uint64_t old_ptr,
                        uint64_t sigsetsize);
long sys_rt_sigpending(uint64_t set_ptr, uint64_t sigsetsize);
long sys_rt_sigreturn(struct yousj_syscall_ctx *ctx);

/* Inheritance rules: clone copies masks + dispositions and clears
 * pending; execve resets caught handlers to default (ignored
 * stays ignored), keeps masks and pending. */
void signal_clone_inherit(struct task *child, struct task *parent);
void signal_exec_reset(struct task *t);

/* The one process-exit path, shared by exit/exit_group (sig ==
 * 0) and by fatal-signal delivery (sig != 0, status ignored).
 * Lives in syscall.c; records the wait status accordingly,
 * notifies the parent with SIGCHLD and never returns. */
void process_exit(long status, int sig);

/* The user-range validation rule, exported from syscall.c so the
 * signal frame builder checks pointers the same way. */
int yousj_user_range_ok(uint64_t ptr, uint64_t len);

#endif /* YOUSJ_SIGNAL_H */

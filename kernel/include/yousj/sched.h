/* Yousj OS - scheduler and task structure (stage 1).
 *
 * Written from scratch by Muse (kernel core), implementing
 * docs/SCHED_DESIGN.md milestones M0/M1: kernel threads first,
 * cooperative round-robin, no timer yet (M2 adds IDT/PIC/PIT
 * preemption). Interface adopted by the coordinator with two
 * simplifications against the design draft, both deliberate:
 *   - ctx is the FIRST field of struct task, so ctx.rsp sits at
 *     offset 0 and context_switch.S needs no magic offsets (the
 *     design's offsetof static assert pins this in sched.c);
 *   - task storage is a static pool (no kernel heap exists yet),
 *     and thread stacks are handed to kthread_create() by the
 *     caller (kthread_create_auto() allocates 16 KiB from the PMM).
 *
 * A task is one execution flow. mm == NULL means a kernel thread,
 * borrowing whatever address space is current; user processes
 * (with their own vmm_space and a Ring 3 landing) arrive at M3+.
 */
#ifndef YOUSJ_SCHED_H
#define YOUSJ_SCHED_H

#include <yousj/types.h>
#include <yousj/signal.h> /* per-task signal state (M7) */
#include <yousj/fpu.h>    /* per-task FXSAVE area size (M8) */

struct yousj_file; /* M9: open-file objects (fs.h) */

#define YOUSJ_NR_OPEN 16  /* fds per task (M9) */
#define YOUSJ_CWD_MAX 64  /* cwd path buffer, incl. NUL (M9) */

struct vmm_space; /* from <yousj/vmm.h>; tasks only hold the pointer */

enum task_state {
    TASK_READY = 0,
    TASK_RUNNING,
    TASK_BLOCKED,
    TASK_ZOMBIE,
    TASK_DEAD
};

/* Callee-saved context: the assembly switch saves rbx/rbp/r12-r15
 * on the task's own stack and only rsp lives here (design sec. 5.1).
 * MUST stay the first member of struct task, with rsp first inside. */
struct task_context {
    uint64_t rsp;
};

struct task {
    struct task_context ctx;      /* offset 0 -- see header note */
    uint64_t        tid;          /* idle = 0, then 1, 2, ... */
    uint64_t        parent_tid;
    enum task_state state;
    char            name[16];
    uint64_t        timeslice_left; /* ticks; unused until M2 preemption */
    uint64_t        total_ticks;
    struct task    *rq_next;
    struct task    *all_next;
    struct vmm_space *mm;         /* NULL = kernel thread */
    uint64_t        kernel_stack_top;
    void          (*entry)(void *arg); /* kernel threads only */
    void           *arg;
    int             exit_status;
    uint64_t        user_ip;      /* user tasks: first ring-3 RIP */
    uint64_t        user_sp;      /* user tasks: initial ring-3 RSP */
    uint64_t        brk_start;    /* user tasks: heap base (ELF M4) */
    uint64_t        brk_end;      /* user tasks: current break */
    uint64_t        fs_base;      /* user tasks: TLS FS base
                                     (arch_prctl ARCH_SET_FS) */
    uint64_t        mmap_next;    /* user tasks: next free VA for
                                     anonymous mmap (bump pointer) */
    /* M24: user-access fault recovery. While a syscall
     * runs, uaccess_armed is 1 and uaccess_jb holds a
     * setjmp landing pad in the dispatcher frame; a
     * kernel-mode fault on a user address that demand
     * paging cannot resolve longjmps back there and the
     * syscall returns -EFAULT instead of halting the
     * kernel. Reset on task creation (a copied pad points
     * into the parent's kernel stack). */
    uint64_t        uaccess_jb[16];
    int             uaccess_armed;
    /* M14: mmap regions. A file-backed VMA holds a reference
     * on the open-file object (file_get at mmap time). Pages
     * are populated eagerly at mmap; MAP_SHARED regions write
     * back on msync/munmap/exec/exit. */
#define YOUSJ_MAX_VMA 16
    struct yousj_vma {
        uint64_t base;
        uint64_t len;      /* page-rounded length */
        uint64_t file_off; /* page-aligned file offset */
        struct yousj_file *file; /* NULL = anonymous */
        uint32_t prot;           /* M17: raw prot bits */
        bool     shared;
        bool     used;
    } vmas[YOUSJ_MAX_VMA];
    uint64_t        wake_tick;    /* nonzero while sleeping: tick at
                                     which sched_tick() wakes the
                                     task (M2). Cleared on wake. */
    int64_t         wait_target;  /* wait4 (M5): 0 = not waiting,
                                     -1 = any child, >0 = that child
                                     tid. sched_exit_current() wakes
                                     a blocked parent whose target
                                     matches the exiting task. */
    uint64_t        kernel_stack_base; /* PMM base of the owned
                                     kernel stack (auto-created
                                     tasks); 0 = not owned, never
                                     freed by the reaper. */
    /* M5 clone: the ring-3 register snapshot a cloned child
     * resumes with (rax is forced to 0 by the landing). Field
     * order is the user_frame_resume() ABI in syscall_entry.S:
     * rdi, rsi, rdx, rcx (user RIP), r10, r8, r9, r11 (user
     * RFLAGS), rsp, then (M7) the callee-saved set rbx, rbp,
     * r12-r15. The callee-saved fields were an M5 gap -- the
     * snapshot originally read only the entry frame, so a child
     * that kept executing (instead of exec'ing at once, the only
     * M5-tested path) resumed with garbage in the registers its
     * compiled code keeps locals in. */
    uint64_t        resume_rdi;
    uint64_t        resume_rsi;
    uint64_t        resume_rdx;
    uint64_t        resume_rcx;
    uint64_t        resume_r10;
    uint64_t        resume_r8;
    uint64_t        resume_r9;
    uint64_t        resume_r11;
    uint64_t        resume_rsp;
    uint64_t        resume_rbx;
    uint64_t        resume_rbp;
    uint64_t        resume_r12;
    uint64_t        resume_r13;
    uint64_t        resume_r14;
    uint64_t        resume_r15;
    bool            has_resume;
    bool            mm_owned;     /* mm points at a pool space from
                                     vmm_space_alloc(): the exit
                                     path destroys it (M5). */
    /* M7 signal state (see <yousj/signal.h>). Meaningful only
     * for user tasks (mm != NULL); kernel threads never pass a
     * delivery gate, so their bits simply never move. */
    uint64_t        sig_pending;  /* queued signals, bit (sig-1) */
    uint64_t        sig_blocked;  /* blocked mask, same encoding */
    struct yousj_sigaction sig_act[YOUSJ_NSIG]; /* dispositions */
    int             sig_killed;   /* when a signal ended this
                                     task: its number, else 0;
                                     wait4 builds the status
                                     word from it (M7) */
    int32_t         sig_sender[YOUSJ_NSIG]; /* last sender tid per
                                     signal (siginfo si_pid) */
    int32_t         sig_chld_status; /* wait-status word of the
                                     most recent child event, for
                                     SIGCHLD's si_status */
    bool            sig_stopped;  /* a stop disposition is in
                                     effect (ps shows 'T') */
    bool            sig_parked;   /* the stop took the task off
                                     the run queue (or blocked it
                                     at a delivery gate); only
                                     SIGCONT releases it */
    bool            sig_wake;     /* the current block was cut
                                     short by signal_send(), so
                                     the interrupted syscall can
                                     tell a signal wake from a
                                     normal completion */
    /* M8: this task's x87/SSE state (FXSAVE image). Pristine
     * at creation (fpu_init_area via a weak hook in sched.c),
     * inherited across clone, reset across execve, saved and
     * restored eagerly by arch_on_task_switch() -- but only
     * for user tasks; the kernel never touches the FPU. */
    _Alignas(16) uint8_t fx_area[YOUSJ_FX_SIZE];
    /* M9: the fd table. Entries are shared open-file objects
     * (fork copies the table and bumps each refcount, so the
     * offset is shared exactly like Linux). NULL = free slot;
     * for fds 0-2 a NULL slot means the implicit console unless
     * the matching fd_console_closed bit is set (a task that
     * never had console fds installed still gets stdio). */
    struct yousj_file *fd_file[YOUSJ_NR_OPEN];
    uint8_t         fd_console_closed; /* bits for fds 0,1,2 */
    uint16_t        fd_cloexec;        /* per-fd close-on-exec
                                          bits (M10) */
    char            cwd_path[YOUSJ_CWD_MAX]; /* "" = "/" */
    bool            in_use;
};

#define YOUSJ_SCHED_MAX_TASKS   32
#define YOUSJ_SCHED_STACK_SIZE  (16ULL * 1024ULL)
#define YOUSJ_SCHED_SLICE_TICKS 5 /* design sec. 4.2; applies from M2 */

/* Implemented in assembly (kernel/arch/x86_64/context_switch.S):
 * saves callee-saved registers on prev's stack, stores rsp into
 * prev->ctx (offset 0), loads next->ctx, restores, and returns into
 * next's saved (or fabricated first-run) context. */
void context_switch(struct task *prev, struct task *next);

/* First-run landing pad for kernel threads (see sched.c): runs the
 * task's entry function, then exits it. Exposed so host tests can
 * verify fabricated stack frames point here. */
void sched_kthread_trampoline(void);

/* Initialise the scheduler and create the idle task (tid 0) around
 * the calling execution flow. Call once, before kthread_create(). */
void sched_init(void);

struct task *sched_current(void);
struct task *sched_idle(void);

/* Create a kernel thread over caller-provided stack memory
 * (stack_mem .. stack_mem+stack_size). Returns NULL when the pool
 * is full or arguments are bad. The thread starts READY, queued. */
struct task *kthread_create(const char *name, void (*entry)(void *arg),
                            void *arg, void *stack_mem, uint64_t stack_size);
/* Same, with a fresh 16 KiB stack allocated from the PMM. */
struct task *kthread_create_auto(const char *name, void (*entry)(void *arg),
                                 void *arg);

/* Core scheduling point (cooperative in M1): move current to the
 * back of the run queue when it stays runnable, pick the head,
 * context-switch. All run-queue manipulation in sched.c assumes
 * single-CPU with interrupts off -- true throughout M1, where
 * interrupts are never enabled; M2 must keep the same discipline
 * around these calls (design sec. 4.2). */
void sched_schedule(void);
void sched_yield(void);

/* Block the current task and schedule another; returns when the
 * task has been woken and rescheduled. */
void sched_block_current(void);
/* Move a BLOCKED task back to READY at the queue tail. No-op for
 * tasks in other states. */
void sched_wake(struct task *task);

/* Exit the current task (state ZOMBIE, exit_status recorded) and
 * schedule another. Never returns; the idle task may not exit. */
void sched_exit_current(int status);

/* M17: page-fault handoff from pf_stub (isr.S) to the VMA
 * layer. yousj_page_fault returns only for resolved faults. */
struct yousj_pf_info {
    uint64_t cr2, err, rip, cs, rsp;
};
void yousj_page_fault(struct yousj_pf_info *info);

/* Count tasks currently in the given state (pool scan; M0/M1 scale). */
uint64_t sched_count_state(enum task_state state);

/* Find a live task by tid (NULL when absent). The returned pointer
 * is invalidated if the task exits or is killed. */
struct task *sched_find_task(int tid);
void         sched_set_name(struct task *t, const char *name);
/* M5 process genealogy (SCHED_DESIGN sec. 6.3): zombie-child
 * lookup and child existence under the wait4 pid filter
 * (pid == -1 matches any child), orphan adoption, and the
 * reaper that frees a zombie's kernel stack and pool slot. */
struct task *sched_find_zombie_child(uint64_t parent_tid, int pid);
int          sched_has_children(uint64_t parent_tid, int pid);
void         sched_adopt_children(uint64_t from_tid, uint64_t to_tid);
int          sched_reap_zombie(struct task *victim);

/* Stage-1 kill ( backs the kill syscall and the M4 acceptance
 * scenario): forcibly retire a READY or BLOCKED task -- it is
 * unlinked from every scheduler structure and its pool slot is
 * freed, so it vanishes completely (no zombie residue: kernel-side
 * demo tasks have no parent to reap them). Refuses (-1) the idle
 * task, the caller itself, RUNNING and ZOMBIE tasks. The victim's
 * kernel stack pages are deliberately leaked for now (no safe
 * reclamation while ITS context may sit in IRQ frames); callers
 * must not hold pointers to the victim afterwards. */
int sched_kill_task(struct task *victim);

/* Format a one-line-per-task snapshot ("<tid> <state> <name>\n",
 * state in {R running, r ready, S sleeping, Z zombie}) into buf,
 * in ascending tid order, truncating at cap bytes; returns the
 * number of bytes written. Backs the native ps syscall. */
uint64_t sched_snapshot_tasks(char *buf, uint64_t cap);

/* --- M2: tick-driven preemption and sleeping -------------------------
 * sched_tick() is called from the timer IRQ handler (100 Hz) with
 * interrupts off. It advances the uptime counter, wakes sleepers
 * whose wake_tick has arrived, charges the current task's slice and
 * triggers a reschedule when the slice expires (or whenever the
 * idle task is on CPU and work is queued). */
void     sched_tick(void);
uint64_t sched_uptime_ticks(void);

/* Sleep for at least the given number of ticks (10 ms each at
 * 100 Hz), via the BLOCKED state plus wake_tick; nanosleep's kernel
 * side will build on this. Sleeping in the idle task is a no-op
 * yield -- idle must always stay able to run the fallback loop. */
void sched_sleep_ticks(uint64_t ticks);

/* M7 stop/continue mechanics (driven by signal.c):
 * sched_park_task() applies a stop to a READY task (off the run
 * queue, parked) or merely flags a BLOCKED/RUNNING one (the flag
 * takes effect at its next delivery gate). sched_cont_task()
 * clears the stop and releases a parked task. sched_self_park()
 * stops the CURRENT task at a delivery gate and returns only
 * after a SIGCONT released it. */
void sched_park_task(struct task *t);
void sched_cont_task(struct task *t);
void sched_self_park(void);

/* Signal-aware sleep for nanosleep: like sched_sleep_ticks(),
 * but returns 1 when signal_send() cut the sleep short (the
 * sig_wake flag is consumed) and 0 when the full time elapsed --
 * including when the sleep was entered with a signal already
 * pending-woken. */
int sched_sleep_ticks_sig(uint64_t ticks);

#endif /* YOUSJ_SCHED_H */

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
     * RFLAGS), rsp. */
    uint64_t        resume_rdi;
    uint64_t        resume_rsi;
    uint64_t        resume_rdx;
    uint64_t        resume_rcx;
    uint64_t        resume_r10;
    uint64_t        resume_r8;
    uint64_t        resume_r9;
    uint64_t        resume_r11;
    uint64_t        resume_rsp;
    bool            has_resume;
    bool            mm_owned;     /* mm points at a pool space from
                                     vmm_space_alloc(): the exit
                                     path destroys it (M5). */
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

/* Count tasks currently in the given state (pool scan; M0/M1 scale). */
uint64_t sched_count_state(enum task_state state);

/* Find a live task by tid (NULL when absent). The returned pointer
 * is invalidated if the task exits or is killed. */
struct task *sched_find_task(int tid);
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

#endif /* YOUSJ_SCHED_H */

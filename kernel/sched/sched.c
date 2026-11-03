/* Yousj OS - scheduler core: task pool, run queue, state machine,
 * cooperative round-robin. Written from scratch by Muse (kernel core),
 * per docs/SCHED_DESIGN.md milestones M0 (this logic, host-tested)
 * and M1 (kernel threads in the real kernel).
 *
 * This file is deliberately free of hardware and architecture code:
 * the only external act is context_switch() (assembly in the kernel
 * build, a recording fake in the host test) and, for
 * kthread_create_auto(), one PMM call. Everything else is list and
 * state manipulation that tests/test_sched.c can drive on the host.
 */
#include <yousj/sched.h>
#include <yousj/signal.h> /* signal_deliverable_pending (M57) */
#include <yousj/pmm.h>
#include <yousj/vmm.h> /* vmm_space layout + CR3 hooks (weak, below) */
#include <stddef.h> /* offsetof */

/* Address-space and stack-pointer plumbing lives in the kernel's
 * arch/syscall layers. These references are WEAK so the host unit
 * test -- which links sched.c without vmm.c/gdt code -- sees them
 * as absent and skips both actions; in the kernel they resolve to
 * the real implementations. The update discipline itself (what is
 * written, when, from which single source value) is defined here
 * and in docs/SYSCALL_DESIGN.md sec. 3.3. */
#pragma weak vmm_kernel_master
#pragma weak vmm_load_cr3
void arch_on_task_switch(struct task *prev, struct task *next);
#pragma weak arch_on_task_switch
/* M8: pristine FPU image for new tasks; weak for the host test,
 * which links sched.c without the FPU layer. */
void fpu_init_area(uint8_t *area);
#pragma weak fpu_init_area
/* signal.h already declares this; weak for the host test,
 * which links sched.c without signal.c (M57). */
#pragma weak signal_deliverable_pending

/* The PML4 phys currently loaded in CR3, as far as this cache
 * knows. The cache key is the PHYS, never the struct vmm_space
 * pointer: pool slots are recycled (exit/exec release, clone
 * allocs first-free), so a fresh space can sit on the exact
 * pointer a just-destroyed space had -- comparing pointers once
 * let a clone child run in its parent's tables after execve had
 * loaded CR3 directly without telling the scheduler (M25
 * postmortem). Every direct vmm_load_cr3() outside the switch
 * below must pair with sched_note_cr3() to keep this truthful. */
static uint64_t g_current_cr3;

/* The contract context_switch.S relies on: ctx.rsp is at offset 0. */
_Static_assert(offsetof(struct task, ctx) == 0,
               "task ctx must be the first field (context_switch.S)");
_Static_assert(offsetof(struct task_context, rsp) == 0,
               "task_context rsp must be its first field");

static struct task  g_tasks[YOUSJ_SCHED_MAX_TASKS];
static struct task *g_all_head;
static struct task *g_rq_head;
static struct task *g_rq_tail;
static struct task *g_current;
static struct task *g_idle;
static uint64_t     g_next_tid;

/* --- small helpers (no libc in freestanding builds) ---------------- */

static void mem_zero(void *dst, uint64_t n)
{
    uint8_t *p = (uint8_t *)dst;
    while (n--)
        *p++ = 0;
}

static void name_copy(char *dst, const char *src)
{
    uint64_t i = 0;
    if (src != NULL) {
        for (; i < 15 && src[i] != '\0'; i++)
            dst[i] = src[i];
    }
    dst[i] = '\0';
}

/* --- run queue ------------------------------------------------------ */

static void rq_enqueue_tail(struct task *t)
{
    t->rq_next = NULL;
    if (g_rq_tail != NULL)
        g_rq_tail->rq_next = t;
    else
        g_rq_head = t;
    g_rq_tail = t;
}

static struct task *rq_dequeue_head(void)
{
    struct task *t = g_rq_head;
    if (t == NULL)
        return NULL;
    g_rq_head = t->rq_next;
    if (g_rq_head == NULL)
        g_rq_tail = NULL;
    t->rq_next = NULL;
    return t;
}

/* --- lifecycle -------------------------------------------------------- */

void sched_init(void)
{
    struct task *idle;
    uint64_t i;

    for (i = 0; i < YOUSJ_SCHED_MAX_TASKS; i++)
        g_tasks[i].in_use = false;
    g_all_head = NULL;
    g_rq_head = g_rq_tail = NULL;
    g_next_tid = 0;
    g_current_cr3 = 0;

    idle = &g_tasks[0];
    mem_zero(idle, sizeof(*idle));
    idle->in_use = true;
    idle->tid = g_next_tid++; /* 0 */
    idle->parent_tid = 0;
    idle->state = TASK_RUNNING;
    name_copy(idle->name, "idle");
    idle->mm = NULL;
    idle->all_next = NULL;
    g_all_head = idle;
    g_idle = idle;
    g_current = idle;
}

struct task *sched_current(void) { return g_current; }
struct task *sched_idle(void)    { return g_idle; }

struct task *kthread_create(const char *name, void (*entry)(void *arg),
                            void *arg, void *stack_mem, uint64_t stack_size)
{
    struct task *t = NULL;
    uint64_t i;
    uint64_t *frame;
    uint64_t stack_top;

    if (entry == NULL || stack_mem == NULL || stack_size < 4096)
        return NULL;
    for (i = 0; i < YOUSJ_SCHED_MAX_TASKS; i++) {
        if (!g_tasks[i].in_use) {
            t = &g_tasks[i];
            break;
        }
    }
    if (t == NULL)
        return NULL;

    mem_zero(t, sizeof(*t));
    t->in_use = true;
    /* M55: umask/cwd live in the fsstate object now --
     * NULL here; user_task_start births it 0022 + "/",
     * clone overwrites from the parent. */
    t->tid = g_next_tid++;
    t->tgid = t->tid; /* M53: own group until a THREAD clone */
    t->pgid = t->tid; /* M65: own process group and session
                       * until clone inherits the creator's */
    t->sid = t->tid;
    t->parent_tid = g_current != NULL ? g_current->tid : 0;
    t->state = TASK_READY;
    name_copy(t->name, name);
    t->entry = entry;
    t->arg = arg;
    t->mm = NULL;
    /* Every task starts life with the pristine FPU image (M8);
     * clone overwrites it with the parent's state, execve
     * resets it. Doing it here, at creation, means the eager
     * fxrstor on the task's first schedule-in can never load an
     * uninitialised (all-zero, exceptions-unmasked) image. */
    if (fpu_init_area != NULL)
        fpu_init_area(t->fx_area);

    /* Fabricate the first-run stack frame (design sec. 5.4), in the
     * exact layout context_switch() will pop on its first switch to
     * this task. From ctx.rsp upward:
     *   [r15][r14][r13][r12][rbx][rbp][return address = trampoline]
     * The effective top is (16-aligned base - 8): after the final
     * ret pops the return address, rsp must be 8 mod 16 at the
     * trampoline's entry, exactly as after a normal call, or every
     * nested call frame in the thread is misaligned by 8. */
    stack_top = (((uint64_t)(uintptr_t)stack_mem + stack_size) & ~15ULL) - 8;
    frame = (uint64_t *)(uintptr_t)(stack_top - 7 * 8);
    frame[0] = 0; /* r15 */
    frame[1] = 0; /* r14 */
    frame[2] = 0; /* r13 */
    frame[3] = 0; /* r12 */
    frame[4] = 0; /* rbx */
    frame[5] = 0; /* rbp */
    frame[6] = (uint64_t)(uintptr_t)&sched_kthread_trampoline;
    t->ctx.rsp = (uint64_t)(uintptr_t)frame;
    t->kernel_stack_top = stack_top;

    t->all_next = g_all_head;
    g_all_head = t;
    rq_enqueue_tail(t);
    return t;
}

struct task *kthread_create_auto(const char *name, void (*entry)(void *arg),
                                 void *arg)
{
    uint64_t stack_phys = pmm_alloc_pages(YOUSJ_SCHED_STACK_SIZE / YOUSJ_PAGE_SIZE);
    if (stack_phys == 0)
        return NULL;
    /* The stack is addressed through the HHDM (M6): the pointer
     * handed down is HHDM_BASE + stack_phys, and kernel_stack_top
     * inherits it, so TSS rsp0 and the syscall entry switch to a
     * stack that is mapped in every address space. No guard page
     * yet: VMM_DESIGN/SCHED_DESIGN note this as an M1-accepted
     * gap; the guard arrives with per-space kernel stacks. */
    {
        struct task *t = kthread_create(name, entry, arg,
                                        yousj_phys_to_virt(stack_phys),
                                        YOUSJ_SCHED_STACK_SIZE);
        if (t != NULL)
            t->kernel_stack_base = stack_phys; /* reaper frees it */
        return t;
    }
}

void sched_kthread_trampoline(void)
{
    /* Runs as the "return address" of a fabricated frame, with
     * current already set to this task by sched_schedule().
     * Interrupts are enabled here (design sec. 5.4): kmain installs
     * the IDT with every IRQ masked before any thread runs, so sti
     * is safe from the very first thread; before timer_start() no
     * source can actually fire. Cooperative M1 threads therefore
     * also run with IF=1 but unperturbed, and from M2 on the timer
     * can preempt any thread from its first instruction. */
#ifdef YOUSJ_KERNEL
    __asm__ volatile("sti");
#endif
    struct task *me = g_current;
    if (me != NULL && me->entry != NULL)
        me->entry(me->arg);
    sched_exit_current(0);
    for (;;) {
        /* unreachable */
    }
}

/* --- scheduling core -------------------------------------------------- */

void sched_schedule(void)
{
    struct task *prev = g_current;
    struct task *next;

    if (prev == NULL)
        return;

    /* Queue manipulation must be atomic against the timer IRQ
     * (M2): from here to the matching restore, interrupts are off.
     * The saved flags live on this call's own stack frame, so a
     * task switched out mid-schedule restores ITS OWN flags when it
     * resumes here -- the discipline is per-call, not global. On
     * the host this guard is compiled out (YOUSJ_KERNEL undefined);
     * cli would fault in user mode, and the host test is
     * single-threaded anyway. */
#ifdef YOUSJ_KERNEL
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags));
#endif

    /* A still-runnable non-idle current goes to the queue tail
     * (round-robin). Blocked/zombie currents simply fall off. The
     * idle task is never queued; it is the fallback, per design. */
    if (prev->state == TASK_RUNNING && prev != g_idle) {
        prev->state = TASK_READY;
        rq_enqueue_tail(prev);
    }

    next = rq_dequeue_head();
    if (next == NULL) {
        /* Queue empty. A runnable prev was re-queued above, so it
         * would have been dequeued again -- reaching here means
         * prev is idle (nothing to do: stay) or prev just blocked
         * or exited (fall back to idle). */
        if (prev == g_idle) {
            prev->state = TASK_RUNNING;
            goto out;
        }
        if (prev->state == TASK_RUNNING)
            goto out; /* safety net; not expected to be reachable */
        next = g_idle;
    }

    if (next == prev) {
        prev->state = TASK_RUNNING;
        goto out;
    }

    next->state = TASK_RUNNING;
    next->timeslice_left = YOUSJ_SCHED_SLICE_TICKS;
    g_current = next;

    /* Address space switch (SCHED_DESIGN 5.3): kernel threads
     * borrow the kernel master space, user tasks run in their own.
     * CR3 is written only on an actual space change, and BEFORE
     * the context switch, so the switch itself already executes
     * under the incoming space -- safe because the kernel half
     * (identity subtree) is present in every space. */
    {
        struct vmm_space *target = next->mm != NULL ? next->mm
            : (vmm_kernel_master != NULL ? vmm_kernel_master() : NULL);
        if (target != NULL && target->pml4_phys != g_current_cr3 &&
            vmm_load_cr3 != NULL) {
            vmm_load_cr3(target->pml4_phys);
            g_current_cr3 = target->pml4_phys;
        }
    }
    /* TSS.rsp0 + syscall kernel_rsp track the incoming task's
     * kernel stack (single update point, SYSCALL_DESIGN 3.3).
     * The hook also moves the FPU state (M8) and so needs the
     * outgoing task as well. */
    if (arch_on_task_switch != NULL)
        arch_on_task_switch(prev, next);

    context_switch(prev, next);
    /* When prev runs again, it resumes here. */

out:
#ifdef YOUSJ_KERNEL
    __asm__ volatile("push %0; popfq" : : "r"(flags) : "memory");
#endif
    ;
}

/* Record a CR3 load performed outside sched_schedule() (the
 * direct loads in process exit and execve commit). Without this
 * the switch cache above goes stale and a later space whose
 * PML4 phys matches the stale value would skip its load. */
void sched_note_cr3(uint64_t pml4_phys)
{
    g_current_cr3 = pml4_phys;
}

void sched_yield(void)
{
    sched_schedule();
}

void sched_block_current(void)
{
    if (g_current == NULL || g_current == g_idle)
        return;
    g_current->state = TASK_BLOCKED;
    sched_schedule();
}

void sched_wake(struct task *task)
{
    if (task == NULL || task->state != TASK_BLOCKED)
        return;
    task->state = TASK_READY;
    rq_enqueue_tail(task);
}

/* M65: the wait4 pid filter, shared by the zombie scan,
 * the has-children scan and the exit wake: -1 = any child,
 * >0 = that tid, 0 = children in the WAITER's process
 * group, <-1 = children in group -pid. */
static int wait_matches(struct task *parent, struct task *child,
                        int pid)
{
    if (pid == -1)
        return 1;
    if (pid > 0)
        return child->tid == (uint64_t)pid;
    if (pid == 0)
        return child->pgid == parent->pgid;
    return child->pgid == (uint64_t)(-(int64_t)pid);
}

void sched_exit_current(int status)
{
    if (g_current == NULL || g_current == g_idle) {
        for (;;) {
            /* idle may not exit; treat as a fatal bug and stop */
        }
    }
    g_current->exit_status = status;
    g_current->state = TASK_ZOMBIE;
    /* Wake a parent blocked in wait4 on this task (M5): the
     * parent re-scans for its zombie child when it runs.
     * M65: the target is the raw wait4 pid argument, matched
     * with the same group-aware rule as the child scans. */
    {
        struct task *parent = sched_find_task((int)g_current->parent_tid);
        if (parent != NULL && parent->state == TASK_BLOCKED &&
            parent->wait_active &&
            wait_matches(parent, g_current,
                         (int)parent->wait_target))
            sched_wake(parent);
    }
    sched_schedule();
    for (;;) {
        /* A zombie is never dequeued again, so sched_schedule()
         * can never legitimately switch back here. */
    }
}

uint64_t sched_count_state(enum task_state state)
{
    uint64_t i, n = 0;
    for (i = 0; i < YOUSJ_SCHED_MAX_TASKS; i++) {
        if (g_tasks[i].in_use && g_tasks[i].state == state)
            n++;
    }
    return n;
}

struct task *sched_find_task(int tid)
{
    uint64_t i;
    if (tid < 0)
        return NULL;
    for (i = 0; i < YOUSJ_SCHED_MAX_TASKS; i++) {
        if (g_tasks[i].in_use && g_tasks[i].tid == (uint64_t)tid)
            return &g_tasks[i];
    }
    return NULL;
}

/* M28: enumerate live tasks for /proc/<pid>. Fills up to
 * cap tids in ascending order and returns the count
 * written. Zombies count as live (they exist until reaped,
 * and ps shows them too). */
uint32_t sched_live_tids(uint64_t *tids, uint32_t cap)
{
    uint32_t n = 0;
    uint64_t want, i;
    for (want = 0; want < g_next_tid && n < cap; want++) {
        for (i = 0; i < YOUSJ_SCHED_MAX_TASKS; i++) {
            if (g_tasks[i].in_use &&
                (uint64_t)g_tasks[i].tid == want) {
                tids[n] = want;
                n++;
                break;
            }
        }
    }
    return n;
}

void sched_set_name(struct task *t, const char *name)
{
    if (t != NULL)
        name_copy(t->name, name);
}

struct task *sched_find_zombie_child(struct task *parent, int pid)
{
    uint64_t i;
    for (i = 0; i < YOUSJ_SCHED_MAX_TASKS; i++) {
        struct task *t = &g_tasks[i];
        if (t->in_use && t->parent_tid == parent->tid &&
            t->state == TASK_ZOMBIE &&
            wait_matches(parent, t, pid))
            return t;
    }
    return NULL;
}

int sched_has_children(struct task *parent, int pid)
{
    uint64_t i;
    for (i = 0; i < YOUSJ_SCHED_MAX_TASKS; i++) {
        struct task *t = &g_tasks[i];
        if (t->in_use && t->parent_tid == parent->tid &&
            wait_matches(parent, t, pid))
            return 1;
    }
    return 0;
}

int sched_adopt_children(uint64_t from_tid, uint64_t to_tid)
{
    uint64_t i;
    int zombies = 0;
    for (i = 0; i < YOUSJ_SCHED_MAX_TASKS; i++) {
        if (g_tasks[i].in_use && g_tasks[i].parent_tid == from_tid) {
            g_tasks[i].parent_tid = to_tid;
            if (g_tasks[i].state == TASK_ZOMBIE)
                zombies++;
        }
    }
    return zombies;
}

/* Unlink t from the all-tasks list (the pool array is scanned
 * directly everywhere else, so this is list hygiene). */
static void all_unlink(struct task *t)
{
    struct task *prev = NULL, *cur = g_all_head;
    while (cur != NULL) {
        if (cur == t) {
            if (prev != NULL)
                prev->all_next = t->all_next;
            else
                g_all_head = t->all_next;
            break;
        }
        prev = cur;
        cur = cur->all_next;
    }
    t->all_next = NULL;
}

int sched_reap_zombie(struct task *victim)
{
    if (victim == NULL || !victim->in_use ||
        victim->state != TASK_ZOMBIE)
        return -1;
    if (victim->kernel_stack_base != 0) {
        pmm_free_pages(victim->kernel_stack_base,
                       YOUSJ_SCHED_STACK_SIZE / YOUSJ_PAGE_SIZE);
        victim->kernel_stack_base = 0;
    }
    all_unlink(victim);
    victim->in_use = false;
    return 0;
}

/* Remove t from the run queue if present (READY tasks only are
 * ever queued). */
static void rq_remove(struct task *t)
{
    struct task *prev = NULL, *cur = g_rq_head;
    while (cur != NULL) {
        if (cur == t) {
            if (prev != NULL)
                prev->rq_next = cur->rq_next;
            else
                g_rq_head = cur->rq_next;
            if (g_rq_tail == cur)
                g_rq_tail = prev;
            cur->rq_next = NULL;
            return;
        }
        prev = cur;
        cur = cur->rq_next;
    }
}

int sched_kill_task(struct task *victim)
{
    struct task *prev = NULL, *cur;
#ifdef YOUSJ_KERNEL
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags));
#endif
    if (victim == NULL || victim == g_idle || victim == g_current ||
        !victim->in_use ||
        (victim->state != TASK_READY && victim->state != TASK_BLOCKED)) {
#ifdef YOUSJ_KERNEL
        __asm__ volatile("push %0; popfq" : : "r"(flags) : "memory");
#endif
        return -1;
    }
    if (victim->state == TASK_READY)
        rq_remove(victim);
    /* Unlink from the all-tasks list; the tick's wake scan walks
     * the pool array and skips !in_use slots, so clearing in_use
     * also cancels any pending wake-up. */
    cur = g_all_head;
    while (cur != NULL) {
        if (cur == victim) {
            if (prev != NULL)
                prev->all_next = cur->all_next;
            else
                g_all_head = cur->all_next;
            break;
        }
        prev = cur;
        cur = cur->all_next;
    }
    victim->all_next = NULL;
    victim->wake_tick = 0;
    victim->state = TASK_ZOMBIE; /* for any stale reader, briefly */
    victim->in_use = false;
#ifdef YOUSJ_KERNEL
    __asm__ volatile("push %0; popfq" : : "r"(flags) : "memory");
#endif
    return 0;
}

/* --- task snapshot (native ps) -------------------------------------- */

struct snap_writer {
    char    *buf;
    uint64_t cap;
    uint64_t len;
};

static void snap_putc(struct snap_writer *w, char c)
{
    if (w->len < w->cap)
        w->buf[w->len] = c;
    w->len++;
}

static void snap_puts(struct snap_writer *w, const char *s)
{
    while (*s != '\0')
        snap_putc(w, *s++);
}

static void snap_put_u64(struct snap_writer *w, uint64_t v)
{
    char tmp[20];
    int n = 0;
    if (v == 0) {
        snap_putc(w, '0');
        return;
    }
    while (v > 0) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n > 0)
        snap_putc(w, tmp[--n]);
}

static char snap_state_char(enum task_state s)
{
    switch (s) {
    case TASK_RUNNING: return 'R';
    case TASK_READY:   return 'r';
    case TASK_BLOCKED: return 'S';
    case TASK_ZOMBIE:  return 'Z';
    case TASK_DEAD:    return 'D';
    }
    return '?';
}

uint64_t sched_snapshot_tasks(char *buf, uint64_t cap)
{
    struct snap_writer w;
    uint64_t want;
#ifdef YOUSJ_KERNEL
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags));
#endif
    w.buf = buf;
    w.cap = cap;
    w.len = 0;
    /* Ascending tid order: tids are handed out monotonically, and
     * the pool is small, so a linear find per tid is fine. */
    for (want = 0; want < g_next_tid; want++) {
        struct task *t = NULL;
        uint64_t i;
        for (i = 0; i < YOUSJ_SCHED_MAX_TASKS; i++) {
            if (g_tasks[i].in_use && (uint64_t)g_tasks[i].tid == want) {
                t = &g_tasks[i];
                break;
            }
        }
        if (t == NULL)
            continue;
        snap_put_u64(&w, (uint64_t)t->tid);
        snap_putc(&w, ' ');
        snap_putc(&w, t->sig_stopped ? 'T' : snap_state_char(t->state));
        snap_putc(&w, ' ');
        snap_puts(&w, t->name);
        snap_putc(&w, '\n');
    }
#ifdef YOUSJ_KERNEL
    __asm__ volatile("push %0; popfq" : : "r"(flags) : "memory");
#endif
    return w.len < w.cap ? w.len : w.cap;
}

/* --- M2: tick, preemption, sleeping -------------------------------------- */

static uint64_t g_ticks;

uint64_t sched_uptime_ticks(void)
{
    return g_ticks;
}

static uint64_t g_idle_ticks;

uint64_t sched_idle_ticks(void)
{
    return g_idle_ticks;
}

void sched_tick(void)
{
    uint64_t i;

    g_ticks++;

    /* Wake due sleepers first, so a reschedule below can pick them. */
    for (i = 0; i < YOUSJ_SCHED_MAX_TASKS; i++) {
        struct task *t = &g_tasks[i];
        if (t->in_use && t->state == TASK_BLOCKED && t->wake_tick != 0 &&
            g_ticks >= t->wake_tick) {
            t->wake_tick = 0;
            sched_wake(t);
        }
    }

    if (g_current == NULL)
        return;
    g_current->total_ticks++;

    if (g_current == g_idle) {
        g_idle_ticks++;
        /* Idle never burns a slice; just give queued work a chance. */
        sched_schedule();
        return;
    }
    if (g_current->timeslice_left > 0)
        g_current->timeslice_left--;
    if (g_current->timeslice_left == 0)
        sched_schedule();
}

void sched_sleep_ticks(uint64_t ticks)
{
    if (g_current == NULL || g_current == g_idle || ticks == 0) {
        sched_yield();
        return;
    }
    /* wake_tick is written before the state store; a tick landing
     * between the two sees a RUNNING task with a future wake_tick
     * and ignores it (wake scan only touches BLOCKED tasks). */
    g_current->wake_tick = g_ticks + ticks;
    g_current->state = TASK_BLOCKED;
    sched_schedule();
    /* Resumed after sched_tick() woke us and the scheduler picked
     * us again; wake_tick was cleared by the wake scan. */
}

/* --- M7: stop/continue and signal-aware sleeping --------------------- */

void sched_park_task(struct task *t)
{
#ifdef YOUSJ_KERNEL
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags));
#endif
    if (t != NULL && t->in_use && t->state != TASK_ZOMBIE) {
        t->sig_stopped = true;
        if (t->state == TASK_READY) {
            rq_remove(t);
            t->state = TASK_BLOCKED;
            t->sig_parked = true;
        }
        /* A BLOCKED or RUNNING target keeps its state: the flag
         * alone is the stop order, and its next delivery gate
         * (or sched_self_park from its own exit check) enforces
         * it. Parking a task out from under an arbitrary block
         * would corrupt the wait it is in. */
    }
#ifdef YOUSJ_KERNEL
    __asm__ volatile("push %0; popfq" : : "r"(flags) : "memory");
#endif
}

void sched_cont_task(struct task *t)
{
#ifdef YOUSJ_KERNEL
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags));
#endif
    if (t != NULL && t->in_use) {
        t->sig_stopped = false;
        if (t->sig_parked) {
            t->sig_parked = false;
            if (t->state == TASK_BLOCKED)
                sched_wake(t);
        }
    }
#ifdef YOUSJ_KERNEL
    __asm__ volatile("push %0; popfq" : : "r"(flags) : "memory");
#endif
}

void sched_self_park(void)
{
    struct task *me = g_current;

    if (me == NULL || me == g_idle)
        return;
    me->sig_stopped = true;
    me->sig_parked = true;
    sched_block_current();
    /* sched_cont_task() cleared both flags and woke us. */
}

int sched_sleep_ticks_sig(uint64_t ticks)
{
    uint64_t deadline;

    if (g_current == NULL || g_current == g_idle || ticks == 0)
        return 0;
    deadline = g_ticks + ticks;
    for (;;) {
        /* M57: a signal that was already deliverable when the
         * sleep began counts too -- signal_send only latches
         * sig_wake for tasks it finds BLOCKED, so a victim
         * killed while still READY (a just-created thread in
         * exit_group is the canonical case) would otherwise
         * sleep the full term with SIGKILL pending. */
        if (g_current->sig_wake ||
            (signal_deliverable_pending != NULL &&
             signal_deliverable_pending(g_current))) {
            g_current->sig_wake = false;
            return 1;
        }
        if (g_ticks >= deadline)
            return 0;
        g_current->wake_tick = deadline;
        g_current->state = TASK_BLOCKED;
        sched_schedule();
        /* On resume the loop re-checks: signal wake (sig_wake
         * set, wake_tick cleared by signal_send) or deadline. */
    }
}

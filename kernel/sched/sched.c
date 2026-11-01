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
#include <yousj/pmm.h>
#include <stddef.h> /* offsetof */

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
    t->tid = g_next_tid++;
    t->parent_tid = g_current != NULL ? g_current->tid : 0;
    t->state = TASK_READY;
    name_copy(t->name, name);
    t->entry = entry;
    t->arg = arg;
    t->mm = NULL;

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
    /* Early kernels run identity-mapped (stub tables / future HHDM),
     * so the physical address is the pointer. No guard page yet:
     * VMM_DESIGN/SCHED_DESIGN note this as an M1-accepted gap; the
     * guard arrives when the kernel switches to its own tables. */
    return kthread_create(name, entry, arg,
                          (void *)(uintptr_t)stack_phys,
                          YOUSJ_SCHED_STACK_SIZE);
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
    context_switch(prev, next);
    /* When prev runs again, it resumes here. */

out:
#ifdef YOUSJ_KERNEL
    __asm__ volatile("push %0; popfq" : : "r"(flags) : "memory");
#endif
    ;
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

void sched_exit_current(int status)
{
    if (g_current == NULL || g_current == g_idle) {
        for (;;) {
            /* idle may not exit; treat as a fatal bug and stop */
        }
    }
    g_current->exit_status = status;
    g_current->state = TASK_ZOMBIE;
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

/* --- M2: tick, preemption, sleeping -------------------------------------- */

static uint64_t g_ticks;

uint64_t sched_uptime_ticks(void)
{
    return g_ticks;
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

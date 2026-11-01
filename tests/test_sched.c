/* Yousj OS - host-side unit test for the scheduler logic (M0).
 * The file under test is exactly kernel/sched/sched.c. The assembly
 * context_switch() is replaced here by a recording fake; a longjmp
 * escape lets the test survive sched_exit_current(), which by
 * design never returns in the real kernel.
 *
 * The script below drives tasks in the order the round-robin queue
 * dictates and asserts every (prev -> next) switch pair, the
 * block/wake skip behaviour, zombie exclusion, and the idle
 * fallback. The expected queue evolution is worked out in the
 * comments so a failure points at the exact transition that broke.
 */
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include <yousj/sched.h>
#include <yousj/pmm.h> /* linked only so kthread_create_auto resolves */

static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

/* --- recording fake context switch --------------------------------- */
#define SWITCH_LOG_MAX 64
static struct task *sw_prev[SWITCH_LOG_MAX];
static struct task *sw_next[SWITCH_LOG_MAX];
static int sw_count;
static jmp_buf exit_escape;
static int longjmp_on_switch;

void context_switch(struct task *prev, struct task *next)
{
    CHECK(prev != next);
    if (sw_count < SWITCH_LOG_MAX) {
        sw_prev[sw_count] = prev;
        sw_next[sw_count] = next;
    }
    sw_count++;
    if (longjmp_on_switch)
        longjmp(exit_escape, 1);
}

static void check_switch(int idx, struct task *prev, struct task *next)
{
    CHECK(idx < sw_count);
    if (idx < sw_count) {
        CHECK(sw_prev[idx] == prev);
        CHECK(sw_next[idx] == next);
    }
}

static _Alignas(16) uint8_t stack_a[8192];
static _Alignas(16) uint8_t stack_b[8192];
static _Alignas(16) uint8_t stack_c[8192];
static _Alignas(16) uint8_t stack_x[8192];

static void dummy_entry(void *arg) { (void)arg; }

int main(void)
{
    struct task *boot, *a, *b, *c, *x;

    sched_init();
    boot = sched_current();
    CHECK(boot != NULL && boot->tid == 0);
    CHECK(boot == sched_idle());
    CHECK(boot->state == TASK_RUNNING);

    a = kthread_create("A", dummy_entry, NULL, stack_a, sizeof(stack_a));
    b = kthread_create("B", dummy_entry, NULL, stack_b, sizeof(stack_b));
    c = kthread_create("C", dummy_entry, NULL, stack_c, sizeof(stack_c));
    CHECK(a != NULL && b != NULL && c != NULL);
    CHECK(a->tid == 1 && b->tid == 2 && c->tid == 3);
    CHECK(a->state == TASK_READY);
    /* Bad creations are refused. */
    CHECK(kthread_create("bad", NULL, NULL, stack_a, sizeof(stack_a)) == NULL);
    CHECK(kthread_create("bad", dummy_entry, NULL, stack_a, 128) == NULL);

    /* Fabricated first-run frame: six zero register slots, then the
     * trampoline as the return address (design sec. 5.4). */
    {
        uint64_t *frame = (uint64_t *)(uintptr_t)a->ctx.rsp;
        int i;
        for (i = 0; i < 6; i++)
            CHECK(frame[i] == 0);
        CHECK(frame[6] == (uint64_t)(uintptr_t)&sched_kthread_trampoline);
        /* Effective top is 8 mod 16 so rsp is call-aligned (8 mod 16)
         * at the trampoline's entry -- see sched.c frame comment. */
        CHECK((a->kernel_stack_top & 15) == 8);
    }

    /* Round-robin: boot -> A -> B -> C -> A. Queue evolution:
     * [A,B,C] -> [B,C] -> [C,A] -> [A,B] -> [B,C]. */
    sched_schedule();
    check_switch(0, boot, a);
    CHECK(sched_current() == a);
    sched_yield();
    check_switch(1, a, b);
    sched_yield();
    check_switch(2, b, c);
    sched_yield();
    check_switch(3, c, a);
    CHECK(sched_current() == a);

    /* Block A: it leaves the rotation until woken.
     * After block: queue [C]; yields then cycle B -> C -> B. */
    sched_block_current();
    check_switch(4, a, b);
    CHECK(a->state == TASK_BLOCKED);
    sched_yield();
    check_switch(5, b, c);
    sched_yield();
    check_switch(6, c, b);
    CHECK(sched_current() == b);

    /* Wake A: it re-enters at the tail and is reached in order.
     * Queue [C] + wake -> [C,A]; yields: B->[C,A,B] pick C,
     * C->[A,B,C] pick A. */
    sched_wake(a);
    CHECK(a->state == TASK_READY);
    sched_yield();
    check_switch(7, b, c);
    sched_yield();
    check_switch(8, c, a);
    CHECK(sched_current() == a);

    /* Exit A (status 42): zombie, never scheduled again. The fake
     * switch longjmps out before exit's never-return loop. */
    longjmp_on_switch = 1;
    if (setjmp(exit_escape) == 0) {
        sched_exit_current(42);
        CHECK(0 && "sched_exit_current returned");
    }
    longjmp_on_switch = 0;
    check_switch(9, a, b);
    CHECK(a->state == TASK_ZOMBIE);
    CHECK(a->exit_status == 42);
    CHECK(sched_current() == b);
    CHECK(sched_count_state(TASK_ZOMBIE) == 1);
    /* Two more yields cycle B -> C -> B with A absent. */
    sched_yield();
    check_switch(10, b, c);
    sched_yield();
    check_switch(11, c, b);

    /* Idle fallback: fresh scheduler, one task that exits with an
     * empty queue -- control must land back on the idle task, and a
     * further schedule() with nothing to run is a no-op. */
    sched_init();
    boot = sched_current();
    x = kthread_create("X", dummy_entry, NULL, stack_x, sizeof(stack_x));
    CHECK(x != NULL && x->tid == 1);
    sw_count = 0;
    sched_schedule();
    check_switch(0, boot, x);
    longjmp_on_switch = 1;
    if (setjmp(exit_escape) == 0) {
        sched_exit_current(0);
        CHECK(0 && "sched_exit_current returned");
    }
    longjmp_on_switch = 0;
    check_switch(1, x, boot);
    CHECK(sched_current() == boot);
    {
        int before = sw_count;
        sched_schedule();
        CHECK(sw_count == before); /* nobody to run: no switch */
        CHECK(sched_current() == boot);
    }

    if (failures == 0)
        printf("test_sched: all checks PASS (RR order, block/wake, zombie exclusion, idle fallback, fabricated frames)\n");
    else
        printf("test_sched: %d FAILURES\n", failures);
    return failures ? 1 : 0;
}

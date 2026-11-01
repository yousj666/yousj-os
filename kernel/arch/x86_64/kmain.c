/* Yousj OS - kernel entry point. Written by Muse (coordinator) as the
 * stage 0/1 core. Server-first: output goes to the serial console ONLY.
 * No framebuffer, no graphics - a headless server does not need them,
 * and serial is where a server kernel is debugged anyway.
 *
 * The bootloader hands us struct yousj_boot_info* in RDI (see boot.h).
 */
#include <yousj/types.h>
#include <yousj/boot.h>
#include <yousj/pmm.h>
#include <yousj/vmm.h>
#include <yousj/sched.h>
#include <yousj/interrupts.h>
#include <yousj/gdt.h>
#include <yousj/user.h>
#include <drivers/serial.h> /* kernel/drivers/serial.h; -I kernel in Makefile */

/* M1 demo threads (SCHED_DESIGN.md milestone M1): two cooperative
 * kernel threads printing in alternation via sched_yield(). */
struct demo_arg {
    char letter;
    volatile int *count;
};

static void demo_thread(void *arg)
{
    struct demo_arg *d = (struct demo_arg *)arg;
    int i;
    for (i = 0; i < 10; i++) {
        serial_puts("Yousj OS: thread ");
        serial_putc(d->letter);
        serial_puts(" iter ");
        serial_put_u64((uint64_t)i);
        serial_putc('\n');
        (*d->count)++;
        sched_yield();
    }
}

/* M2 demo threads (SCHED_DESIGN.md milestone M2): two busy spinners
 * that never yield -- only the timer can move them -- and a sleeper
 * that must wake three times, once per second. */
struct spin_arg {
    volatile uint64_t *count;
};

static void spin_thread(void *arg)
{
    struct spin_arg *s = (struct spin_arg *)arg;
    for (;;)
        (*s->count)++;
}

static void sleeper_thread(void *arg)
{
    int i;
    (void)arg;
    for (i = 0; i < 3; i++) {
        serial_puts("Yousj OS: thread S sleeping 1s\n");
        sched_sleep_ticks(100);
        serial_puts("Yousj OS: thread S woke\n");
    }
}

/* M2 monitor: a normal thread doing the observing. It cannot be
 * kmain/idle doing this -- with two always-runnable spinners the
 * run queue never empties, so the idle fallback would starve and
 * never print anything (observed on the first M2 attempt, where
 * the sleeper finished but no PASS line ever appeared). */
struct monitor_arg {
    struct task *sleeper;
    volatile uint64_t *spin_c;
    volatile uint64_t *spin_d;
};

static enum task_state task_state_of(struct task *t)
{
    return ((volatile struct task *)t)->state;
}

static void monitor_thread(void *arg)
{
    struct monitor_arg *m = (struct monitor_arg *)arg;
    uint64_t c1, d1, c2, d2, waited = 0;

    while (task_state_of(m->sleeper) != TASK_ZOMBIE && waited < 3000) {
        sched_sleep_ticks(10);
        waited += 10;
    }
    c1 = *m->spin_c;
    d1 = *m->spin_d;
    sched_sleep_ticks(100); /* sample over 1 s */
    c2 = *m->spin_c;
    d2 = *m->spin_d;

    if (task_state_of(m->sleeper) == TASK_ZOMBIE && c2 > c1 && d2 > d1) {
        serial_puts("Yousj OS: sched M2 self-test PASS (preemption: spin-C +");
        serial_put_u64(c2 - c1);
        serial_puts(", spin-D +");
        serial_put_u64(d2 - d1);
        serial_puts(" in 1s; sleeper woke 3x and exited)\n");
    } else {
        serial_puts("Yousj OS: sched M2 self-test FAILED\n");
    }
    /* Test over: just exit. The spinners would run forever, but
     * they only burn their own timeslices; the M3 launcher thread
     * (created alongside) notices this thread's zombie and carries
     * the boot sequence on. Orchestration must live in a queued
     * thread like this one: once immortal threads exist, the idle
     * task (kmain) is only a fallback and would never run again to
     * notice anything -- the exact trap the M2 design warned about
     * when it made the observer a monitor thread, not idle. */
}

static void halt_forever(void)
{
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}


struct m3_launcher_arg {
    struct task *m2_monitor;
};

/* Stage 1, step 5 (SCHED_DESIGN milestone M3): the first ring-3
 * program, orchestrated from a queued kernel thread (see the
 * monitor's comment for why idle cannot do this job). Waits for
 * the M2 monitor to finish, then gives one user task its own
 * address space (kernel identity subtree shared, image + stack at
 * YOUSJ_USER_BASE), lands it in ring 3 via iretq, and requires it
 * to print its pid through the write syscall and exit with status
 * 42. The two M2 spinners keep looping in the background --
 * background noise the scheduler just has to live with now. On the
 * verdict it stops the machine: this is the last boot milestone
 * for now. */
static void m3_launcher_thread(void *arg)
{
    struct m3_launcher_arg *larg = (struct m3_launcher_arg *)arg;
    extern const uint8_t _binary_m3_user_bin_start[];
    extern const uint8_t _binary_m3_user_bin_end[];
    static struct vmm_space uspace;
    static struct user_start ustart;
    struct task *ut;
    uint64_t code_size = (uint64_t)(_binary_m3_user_bin_end -
                                    _binary_m3_user_bin_start);
    uint64_t code_pages = (code_size + YOUSJ_PAGE_SIZE - 1) /
                          YOUSJ_PAGE_SIZE;
    uint64_t i;
    int ok = 1;

    while (task_state_of(larg->m2_monitor) != TASK_ZOMBIE)
        sched_sleep_ticks(10);

    if (vmm_create_space(&uspace) != 0)
        ok = 0;
    if (ok)
        vmm_share_kernel_identity(&uspace);
    /* Image pages: RWX for now (the flat binary keeps its strings
     * in the same page); W^X separation arrives when the ELF
     * loader distinguishes segments (M4). */
    for (i = 0; ok && i < code_pages; i++) {
        uint64_t frame = pmm_alloc_page();
        const uint8_t *src = _binary_m3_user_bin_start +
                             i * YOUSJ_PAGE_SIZE;
        uint64_t chunk = code_size - i * YOUSJ_PAGE_SIZE;
        uint64_t j;
        if (frame == 0) { ok = 0; break; }
        if (chunk > YOUSJ_PAGE_SIZE)
            chunk = YOUSJ_PAGE_SIZE;
        for (j = 0; j < chunk; j++)
            ((uint8_t *)(uintptr_t)frame)[j] = src[j];
        if (vmm_map(&uspace, YOUSJ_USER_CODE_VA + i * YOUSJ_PAGE_SIZE,
                    frame, YOUSJ_PAGE_SIZE,
                    YOUSJ_VMM_READ | YOUSJ_VMM_WRITE |
                    YOUSJ_VMM_EXEC | YOUSJ_VMM_USER) != 0)
            ok = 0;
    }
    /* Stack: 4 pages below YOUSJ_USER_STACK_TOP, RW, no exec. */
    for (i = 0; ok && i < 4; i++) {
        uint64_t frame = pmm_alloc_page();
        if (frame == 0) { ok = 0; break; }
        if (vmm_map(&uspace,
                    YOUSJ_USER_STACK_TOP - (i + 1) * YOUSJ_PAGE_SIZE,
                    frame, YOUSJ_PAGE_SIZE,
                    YOUSJ_VMM_READ | YOUSJ_VMM_WRITE |
                    YOUSJ_VMM_USER) != 0)
            ok = 0;
    }
    if (!ok) {
        serial_puts("Yousj OS: sched M3 FAILED (user space setup)\n");
        halt_forever();
    }

    syscall_init();
    ustart.ip = YOUSJ_USER_CODE_VA;
    ustart.sp = YOUSJ_USER_STACK_TOP;

    /* Create + bind the address space atomically w.r.t. the
     * timer: the new task must never be scheduled before its
     * mm is attached. */
    __asm__ volatile("cli");
    ut = kthread_create_auto("user-smoke", user_task_start, &ustart);
    if (ut != NULL)
        ut->mm = &uspace;
    __asm__ volatile("sti");
    if (ut == NULL) {
        serial_puts("Yousj OS: sched M3 FAILED (task create)\n");
        halt_forever();
    }
    while (task_state_of(ut) != TASK_ZOMBIE)
        sched_sleep_ticks(5);
    if (ut->exit_status == 42) {
        serial_puts("Yousj OS: sched M3 self-test PASS "
                    "(ring3 program ran, syscalls served, exit 42)\n");
    } else {
        serial_puts("Yousj OS: sched M3 self-test FAILED (exit status ");
        serial_put_u64((uint64_t)(uint32_t)ut->exit_status);
        serial_puts(")\n");
        halt_forever();
    }
    /* The user space is deliberately NOT destroyed: its entry 0
     * is shared with the master (destroy handles that), but the
     * image/stack frames have no owner-tracking yet. Stage-1
     * smoke leak; reaper arrives with real process teardown. */

    /* All boot self-tests passed. What remains of stage 1, in
     * order (owned by coordinator): M4 (ELF init as PID 1), M5
     * (clone/exec split + standalone shell), console input. */
    serial_puts("Yousj OS: all stage-1 boot self-tests done, halting for now\n");
    halt_forever();
}

/* Kernel entry. boot_info may be NULL in the earliest bring-up builds,
 * in which case we say so on serial and stop instead of crashing blind. */
void kmain(struct yousj_boot_info *boot_info)
{
    serial_init();
    serial_puts("\nYousj OS: kmain reached (server-first build)\n");

    /* Own GDT first (the boot environment's table is not ours to
     * trust -- under UEFI its selector 0x08 is not even a 64-bit
     * code segment), then IDT + PIC as early as possible (all IRQs
     * masked, interrupts still off): from here on, a fault anywhere
     * below produces a serial exception dump instead of a silent
     * triple fault, and thread trampolines may safely sti (M1
     * threads run before timer_start(), when no source can fire
     * yet). */
    gdt_init();
    interrupts_init();

    if (boot_info == NULL) {
        serial_puts("Yousj OS: no boot_info, halting\n");
        halt_forever();
    }
    if (boot_info->magic != YOUSJ_BOOT_MAGIC) {
        serial_puts("Yousj OS: bad boot_info magic, halting\n");
        halt_forever();
    }

    serial_puts("Yousj OS: boot_info OK, memory map received\n");

    /* Stage 1, step 1 (done 2026-11-01): physical page allocator. */
    pmm_init(boot_info);
    serial_puts("Yousj OS: PMM total pages=");
    serial_put_u64(pmm_total_page_count());
    serial_puts(" free pages=");
    serial_put_u64(pmm_free_page_count());
    if (pmm_truncated())
        serial_puts(" (map truncated to bitmap capacity)");
    serial_putc('\n');

    /* PMM self-test on real hardware state: allocate one page, show
     * it, give it back. A failure here means the boot map lied. */
    {
        uint64_t page = pmm_alloc_page();
        if (page == 0) {
            serial_puts("Yousj OS: PMM self-test FAILED (no free page)\n");
            halt_forever();
        }
        serial_puts("Yousj OS: PMM self-test alloc ");
        serial_put_hex64(page);
        serial_putc('\n');
        if (pmm_free_page(page) != 0) {
            serial_puts("Yousj OS: PMM self-test FAILED (free)\n");
            halt_forever();
        }
        serial_puts("Yousj OS: PMM self-test PASS\n");
    }

    /* Stage 1, step 2 (logic done 2026-11-01): the VMM's page-table
     * walking/mapping is exercised here as a pure software self-test
     * on a throwaway address space. We deliberately do NOT load the
     * new space into CR3 yet: switching page tables waits for the
     * bootloader paging contract (docs/VMM_DESIGN.md sec. 2.3). */
    {
        struct vmm_space space;
        uint64_t data_page, translated = 0;
        int ok = 1;

        vmm_use_kernel_backend();
        if (vmm_create_space(&space) != 0)
            ok = 0;
        data_page = ok ? pmm_alloc_page() : 0;
        if (data_page == 0)
            ok = 0;
        if (ok && vmm_map(&space, 0x400000, data_page, YOUSJ_PAGE_SIZE,
                          YOUSJ_VMM_READ | YOUSJ_VMM_WRITE) != 0)
            ok = 0;
        if (ok && (vmm_translate(&space, 0x400000, &translated, NULL) != 0 ||
                   translated != data_page))
            ok = 0;
        if (ok && vmm_unmap(&space, 0x400000, YOUSJ_PAGE_SIZE) != 0)
            ok = 0;
        if (data_page != 0)
            (void)pmm_free_page(data_page);
        if (ok) {
            vmm_destroy_space(&space);
            serial_puts("Yousj OS: VMM self-test PASS (no CR3 switch yet)\n");
        } else {
            serial_puts("Yousj OS: VMM self-test FAILED, halting\n");
            halt_forever();
        }
    }

    /* Stage 1, step 2b (VMM hardware step 1): switch CR3 from the
     * boot stub's tables to a kernel-owned master space (identity
     * map of all RAM, built by vmm_hw_init). Everything after this
     * block -- scheduler, threads, interrupts -- runs on OUR page
     * tables; the M1/M2 self-tests below double as the regression
     * proof. The post-switch check maps a scratch page at 2 GiB
     * (above RAM, previously unmapped in both table sets), writes
     * through the NEW tables, and translates it back. */
    {
        uint64_t old_cr3 = vmm_read_cr3();
        uint64_t ram_top = pmm_total_page_count() * YOUSJ_PAGE_SIZE;
        uint64_t frame, translated = 0;
        volatile uint64_t *scratch = (volatile uint64_t *)(uintptr_t)0x80000000ULL;
        int ok = 1;

        if (vmm_hw_init(ram_top) != 0)
            ok = 0;
        frame = ok ? pmm_alloc_page() : 0;
        if (frame == 0)
            ok = 0;
        if (ok && vmm_map(vmm_kernel_master(), 0x80000000ULL, frame,
                          YOUSJ_PAGE_SIZE,
                          YOUSJ_VMM_READ | YOUSJ_VMM_WRITE) != 0)
            ok = 0;
        if (ok) {
            *scratch = 0xA5A5DEADBEEF5A5AULL;
            if (*scratch != 0xA5A5DEADBEEF5A5AULL)
                ok = 0;
        }
        if (ok && (vmm_translate(vmm_kernel_master(), 0x80000000ULL,
                                 &translated, NULL) != 0 ||
                   translated != frame))
            ok = 0;
        if (ok && vmm_unmap(vmm_kernel_master(), 0x80000000ULL,
                            YOUSJ_PAGE_SIZE) != 0)
            ok = 0;
        if (frame != 0)
            (void)pmm_free_page(frame);
        if (ok) {
            serial_puts("Yousj OS: VMM hw-init PASS (CR3 ");
            serial_put_hex64(old_cr3);
            serial_puts(" -> ");
            serial_put_hex64(vmm_read_cr3());
            serial_puts(", kernel master identity map live)\n");
        } else {
            serial_puts("Yousj OS: VMM hw-init FAILED, halting\n");
            halt_forever();
        }
    }

    /* Stage 1, step 3 (SCHED_DESIGN milestone M1): cooperative
     * kernel threads. kmain itself becomes the idle task; the two
     * demo threads must strictly alternate A/B for 10 rounds and
     * exit as zombies, which the idle loop below waits for. */
    {
        static volatile int count_a, count_b;
        static struct demo_arg arg_a, arg_b;
        struct task *ta, *tb;

        arg_a.letter = 'A';
        arg_a.count = &count_a;
        arg_b.letter = 'B';
        arg_b.count = &count_b;

        sched_init();
        ta = kthread_create_auto("thread-A", demo_thread, &arg_a);
        tb = kthread_create_auto("thread-B", demo_thread, &arg_b);
        if (ta == NULL || tb == NULL) {
            serial_puts("Yousj OS: sched M1 FAILED (thread create)\n");
            halt_forever();
        }
        while (sched_count_state(TASK_ZOMBIE) < 2)
            sched_schedule();
        if (count_a == 10 && count_b == 10) {
            serial_puts("Yousj OS: sched M1 self-test PASS "
                        "(threads A/B alternated 10 rounds, both exited)\n");
        } else {
            serial_puts("Yousj OS: sched M1 self-test FAILED (counts)\n");
            halt_forever();
        }
    }

    /* Stage 1, step 4 (SCHED_DESIGN milestone M2): timer
     * preemption. Two busy threads never yield; both counters must
     * still advance, and the sleeper must complete 3 sleep/wake
     * cycles. The monitor thread (not idle -- see its comment)
     * verifies and prints the verdict, then exits; the M3 launcher
     * thread created alongside it picks the sequence up from
     * there, and kmain settles into pure idle. */
    {
        static volatile uint64_t spin_c, spin_d;
        static struct spin_arg carg, darg;
        static struct monitor_arg marg;
        static struct m3_launcher_arg larg;
        struct task *tc, *td, *ts, *tm, *t3;

        carg.count = &spin_c;
        darg.count = &spin_d;
        tc = kthread_create_auto("spin-C", spin_thread, &carg);
        td = kthread_create_auto("spin-D", spin_thread, &darg);
        ts = kthread_create_auto("sleeper-S", sleeper_thread, NULL);
        marg.sleeper = ts;
        marg.spin_c = &spin_c;
        marg.spin_d = &spin_d;
        tm = kthread_create_auto("monitor-M", monitor_thread, &marg);
        larg.m2_monitor = tm;
        t3 = kthread_create_auto("m3-launcher", m3_launcher_thread, &larg);
        if (tc == NULL || td == NULL || ts == NULL || tm == NULL ||
            t3 == NULL) {
            serial_puts("Yousj OS: sched M2 FAILED (thread create)\n");
            halt_forever();
        }

        timer_start(); /* PIT 100 Hz live: preemption from here */

        /* Idle from here on: the monitor drives the M2 test, the
         * launcher drives everything after it. */
        for (;;)
            __asm__ volatile("hlt" : : : "memory");
    }

}

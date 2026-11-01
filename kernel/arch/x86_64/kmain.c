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
    /* Test over: stop the world (the spinners would run forever). */
    for (;;)
        __asm__ volatile("cli; hlt");
}

static void halt_forever(void)
{
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

/* Kernel entry. boot_info may be NULL in the earliest bring-up builds,
 * in which case we say so on serial and stop instead of crashing blind. */
void kmain(struct yousj_boot_info *boot_info)
{
    serial_init();
    serial_puts("\nYousj OS: kmain reached (server-first build)\n");

    /* IDT + PIC go in as early as possible (all IRQs masked,
     * interrupts still off): from here on, a fault anywhere below
     * produces a serial exception dump instead of a silent triple
     * fault, and thread trampolines may safely sti (M1 threads run
     * before timer_start(), when no source can fire yet). */
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
     * verifies and prints the verdict, then stops the world. */
    {
        static volatile uint64_t spin_c, spin_d;
        static struct spin_arg carg, darg;
        static struct monitor_arg marg;
        struct task *tc, *td, *ts, *tm;

        carg.count = &spin_c;
        darg.count = &spin_d;
        tc = kthread_create_auto("spin-C", spin_thread, &carg);
        td = kthread_create_auto("spin-D", spin_thread, &darg);
        ts = kthread_create_auto("sleeper-S", sleeper_thread, NULL);
        marg.sleeper = ts;
        marg.spin_c = &spin_c;
        marg.spin_d = &spin_d;
        tm = kthread_create_auto("monitor-M", monitor_thread, &marg);
        if (tc == NULL || td == NULL || ts == NULL || tm == NULL) {
            serial_puts("Yousj OS: sched M2 FAILED (thread create)\n");
            halt_forever();
        }

        timer_start(); /* PIT 100 Hz live: preemption from here */

        /* Idle from here on: the monitor drives the test. */
        for (;;)
            __asm__ volatile("hlt" : : : "memory");
    }

    /* Unreachable while the M2 demo above idles forever; kept as
     * the halt target for the milestone that replaces the demo.
     * Next steps, in order (owned by coordinator):
     * 5. CR3 switch to the kernel's own tables (VMM hardware step)
     * 6. M3: Ring 3 smoke (flat user binary, iretq, getpid/write/exit)
     * 7. console input, then userland init/shell (server userland)
     */
    halt_forever();
}

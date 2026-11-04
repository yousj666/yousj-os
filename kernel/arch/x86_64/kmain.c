/* Yousj OS - kernel entry point. Written by Muse (coordinator) as the
 * stage 0/1 core. Server-first: output goes to the serial console ONLY.
 * No framebuffer, no graphics - a headless server does not need them,
 * and serial is where a server kernel is debugged anyway.
 *
 * The bootloader hands us struct yousj_boot_info* in RDI (see boot.h).
 */
#include <yousj/types.h>
#include <yousj/time.h>
#include <yousj/boot.h>
#include <yousj/pmm.h>
#include <yousj/vmm.h>
#include <yousj/sched.h>
#include <yousj/interrupts.h>
#include <yousj/gdt.h>
#include <yousj/fpu.h>
#include <yousj/fs.h>
#include <yousj/user.h>
#include <yousj/elf.h>
#include <yousj/net.h>
#include <yousj/fb.h>
#include <yousj/ps2mouse.h>
#include <yousj/wm.h>
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

static volatile int g_spin_stop;

static void spin_thread(void *arg)
{
    struct spin_arg *s = (struct spin_arg *)arg;
    /* M25: once the monitor has its verdict it raises
     * g_spin_stop and the spinners retire THEMSELVES via
     * the ordinary exit path (honest zombies the launcher
     * reaps, like every other boot thread) instead of
     * burning timeslices forever. */
    while (!g_spin_stop)
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
    /* M25: the test is over -- tell the spinners to retire
     * (see spin_thread) instead of burning timeslices
     * forever; until now the machine was never actually
     * idle at rest, which /proc/uptime's idle field made
     * impossible to ignore. */
    g_spin_stop = 1;
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


struct boot_launcher_arg {
    struct task *m2_monitor;
};

/* M4 demo victim: sleeps in 60-second stretches forever, so it is
 * always in S state in ps and always killable. Exists purely so
 * the M4 acceptance flow (ps -> kill -> ps) has a target. */
static void demo_sleeper_thread(void *arg)
{
    (void)arg;
    for (;;)
        sched_sleep_ticks(6000);
}

/* Build init's user stack in its space: 4 pages below
 * YOUSJ_INIT_STACK_TOP, the top page preloaded with the Linux
 * initial-stack block. Layout (offsets from the stack top):
 *   -16        "/init" string (argv[0])
 *   -80 .. -16 the qword block the crt0 reads:
 *              argc=1, argv[0]=top-16, argv[1]=NULL, envp=NULL,
 *              auxv AT_PAGESZ(6)=4096, auxv AT_NULL(0)=0
 * The initial RSP is top-80 (16-byte aligned, as crt0 expects). */
/* Boot orchestrator for everything after M2 (a queued kernel
 * thread -- see the monitor's comment for why idle cannot do this
 * job). First the M3 smoke: one user task gets its own address
 * space (kernel half shared from the master, image + stack at
 * YOUSJ_USER_BASE), lands in ring 3 via iretq, and must print its
 * pid through the write syscall and exit with status 42. Then M4:
 * the ELF init is loaded into its own space and started as the
 * init task, and this thread retires. The two M2 spinners keep
 * looping throughout -- background noise the scheduler just has
 * to live with. */
static void boot_launcher_thread(void *arg)
{
    struct boot_launcher_arg *larg = (struct boot_launcher_arg *)arg;
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
            ((uint8_t *)yousj_phys_to_virt(frame))[j] = src[j];
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
    /* The user space is deliberately NOT destroyed: the
     * image/stack frames have no owner-tracking yet. Stage-1
     * smoke leak; reaper arrives with real process teardown. */

    
    /* Stage 1, step 6 (SCHED_DESIGN milestone M4): init formally
     * takes office. A demo sleeper is parked first (so the M4
     * acceptance scenario -- ps, kill the sleeper, ps again -- can
     * be driven from init's console), then the embedded static ELF
     * init is loaded into its own space per SCHED_DESIGN sec. 7
     * and started; init owns the console from there and this
     * thread retires. */
    {
        static struct vmm_space ispace;
        static struct user_start istart;
        struct task *itask;
        uint64_t entry = 0, img_brk = 0, heap_base;
        uint64_t iphdr = 0, iphnum = 0; /* M56: for the auxv */
        int ok = 1;

        /* The ps/kill acceptance target: a kernel thread that sleeps in long stretches, so it shows up as S in ps. */
        if (kthread_create_auto("demo-sleeper", demo_sleeper_thread,
                                NULL) == NULL)
            ok = 0;
        if (ok && vmm_create_space(&ispace) != 0)
            ok = 0;
        if (ok) {
            /* M11: /init comes out of the ramfs like any execve
             * target (it arrived via the initramfs); stage it in
             * a contiguous PMM run for the ELF loader. */
            struct yousj_ramfs_node *inode = fs_lookup("/init");
            uint64_t isize = inode != NULL ? fs_node_size(inode) : 0;
            uint64_t ipages =
                (isize + YOUSJ_PAGE_SIZE - 1) / YOUSJ_PAGE_SIZE;
            uint64_t istage = ipages != 0 ? pmm_alloc_pages(ipages) : 0;

            if (inode == NULL || istage == 0)
                ok = 0;
            if (ok) {
                uint8_t *ibuf =
                    (uint8_t *)yousj_phys_to_virt(istage);
                if (fs_node_read_all(inode, ibuf, isize) !=
                    (long)isize)
                    ok = 0;
                if (ok && elf_load_user_image(ibuf, isize, &ispace,
                                              ~0ULL,
                                              &entry, &img_brk,
                                              &iphdr, &iphnum) != 0)
                    ok = 0;
                pmm_free_pages(istage, ipages);
            }
        }
        /* User stack: 4 pages below YOUSJ_INIT_STACK_TOP; the top
         * page carries the Linux initial-stack block (argc/argv/
         * envp/auxv) built by elf_build_initial_stack(). */
        if (ok) {
            static const char *const init_argv[] = { "/init", NULL };
            if (elf_build_initial_stack(&ispace, YOUSJ_INIT_STACK_TOP,
                                        init_argv, NULL,
                                        iphdr, iphnum, entry, 0,
                                        &istart.sp) != 0)
                ok = 0;
        }
        if (!ok) {
            serial_puts("Yousj OS: sched M4 FAILED (init load)\n");
            halt_forever();
        }
        heap_base = (img_brk + YOUSJ_PAGE_SIZE - 1) &
                    ~(YOUSJ_PAGE_SIZE - 1);
        istart.ip = entry;

        __asm__ volatile("cli");
        itask = kthread_create_auto("init", user_task_start, &istart);
        if (itask != NULL) {
            itask->mm = &ispace;
            itask->brk_start = heap_base;
            itask->brk_end = heap_base;
            syscall_set_init_task(itask);
        }
        __asm__ volatile("sti");
        if (itask == NULL) {
            serial_puts("Yousj OS: sched M4 FAILED (init create)\n");
            halt_forever();
        }
    
        serial_puts("Yousj OS: boot self-tests done, init started "
                    "(ELF, milestone M4)\n");
        /* Retire: init owns the machine now. Remaining stage-1
         * work (owned by coordinator): M5 (clone/exec split +
         * standalone shell). */
    }
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

    /* M8: enable x87/SSE and capture the pristine per-task FPU
     * image. Must precede the first task creation: sched.c seeds
     * every new task's fx_area from this image. */
    fpu_init_cpu();
    serial_puts("Yousj OS: FPU init PASS (SSE on, per-task FXSAVE state)\n");

    if (boot_info == NULL) {
        serial_puts("Yousj OS: no boot_info, halting\n");
        halt_forever();
    }
    if (boot_info->magic != YOUSJ_BOOT_MAGIC) {
        serial_puts("Yousj OS: bad boot_info magic, halting\n");
        halt_forever();
    }

    serial_puts("Yousj OS: boot_info OK, memory map received\n");

    /* M81: snapshot the framebuffer handoff NOW, while boot_info
     * is still reachable through the hand-off identity map; the
     * master space that replaces it has no identity map, and the
     * fb_init() call below happens long after the switch. */
    struct yousj_framebuffer boot_fb = boot_info->fb;

    /* M23: seed the wall clock from the CMOS RTC before
     * anything can ask for a timestamp. */
    time_init();

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

    /* Stage 1, step 2b (M6 layout switch): switch CR3 from the
     * boot chain's early tables to the kernel-owned master space
     * (HHDM + higher-half image, NO identity map -- built by
     * vmm_hw_init). Everything after this block -- scheduler,
     * threads, interrupts -- runs on OUR page tables; the
     * M1/M2 self-tests below double as the regression proof.
     * The post-switch check exercises the new layout only: a
     * fresh frame written and read back through its HHDM
     * address, that address translated in the master, and the
     * kernel image base translated to its physical 1 MiB. */
    {
        uint64_t old_cr3 = vmm_read_cr3();
        uint64_t ram_top = pmm_total_page_count() * YOUSJ_PAGE_SIZE;
        uint64_t frame, translated = 0;
        int ok = 1;

        if (vmm_hw_init(ram_top) != 0)
            ok = 0;
        frame = ok ? pmm_alloc_page() : 0;
        if (frame == 0)
            ok = 0;
        if (ok) {
            volatile uint64_t *hw =
                (volatile uint64_t *)yousj_phys_to_virt(frame);
            *hw = 0xA5A5DEADBEEF5A5AULL;
            if (*hw != 0xA5A5DEADBEEF5A5AULL)
                ok = 0;
        }
        if (ok && (vmm_translate(vmm_kernel_master(),
                                 YOUSJ_HHDM_BASE + frame,
                                 &translated, NULL) != 0 ||
                   translated != frame))
            ok = 0;
        if (ok && (vmm_translate(vmm_kernel_master(),
                                 YOUSJ_KERNEL_BASE,
                                 &translated, NULL) != 0 ||
                   translated != 0x100000ULL))
            ok = 0;
        if (frame != 0)
            (void)pmm_free_page(frame);
        if (ok) {
            serial_puts("Yousj OS: VMM hw-init PASS (CR3 ");
            serial_put_hex64(old_cr3);
            serial_puts(" -> ");
            serial_put_hex64(vmm_read_cr3());
            serial_puts(", HHDM + higher-half master live, "
                        "no identity map)\n");
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

        /* M9: the ramfs exists before any task does; the
         * embedded program images become its first files. */
        fs_init();
        net_init(); /* M72: probe the NIC (absent is fine) */
        fb_init(&boot_fb); /* M81: light the screen if GOP gave one */
        ps2mouse_init(); /* M84: AUX port on, mouse reporting */
        wm_init(); /* M85/M91: paint the desktop (icons + taskbar) */
        if (fs_load_initramfs() < 0) {
            serial_puts("Yousj OS: initramfs FAILED (corrupt archive)\n");
            halt_forever();
        }
        serial_puts("Yousj OS: ramfs init PASS (initramfs unpacked)\n");
        {
            int mrc = fs_mount_data();
            if (mrc == 1)
                serial_puts("Yousj OS: yfs mounted on /data\n");
            else if (mrc == 2)
                serial_puts("Yousj OS: yfs formatted and mounted on /data\n");
            else if (mrc == 3)
                serial_puts("Yousj OS: yfs disk has an old format version; not mounted (v4 required, disk left untouched)\n");
            else if (mrc == 0)
                serial_puts("Yousj OS: no data disk (yfs not mounted)\n");
            else
                serial_puts("Yousj OS: yfs mount FAILED (I/O error)\n");
        }

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
        static struct boot_launcher_arg larg;
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
        t3 = kthread_create_auto("boot-launcher", boot_launcher_thread, &larg);
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

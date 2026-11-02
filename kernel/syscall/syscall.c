/* Yousj OS - syscall entry setup and dispatcher.
 * Written from scratch by Muse (coordinator), per
 * docs/SYSCALL_DESIGN.md: `syscall`/`sysret` entry only, Linux
 * x86-64 numbering and argument order, negative-errno returns,
 * -ENOSYS for anything not yet implemented (never fake success).
 *
 * Batch 0 (M3): write, getpid, exit/exit_group.
 * Batch 1 (M4): read (fd 0 = serial console), nanosleep, uname,
 * brk, mmap/munmap (anonymous only), arch_prctl (FS only), kill
 * (stage-1 semantics, see sys_kill), plus the native ps snapshot
 * call. Batch 2 (clone/execve/wait4) lands with M5.
 *
 * All user pointers are validated against the sanctioned window
 * [YOUSJ_USER_BASE, YOUSJ_USER_LIMIT) before dereference, and all
 * handlers run on the caller's own address space, so validated
 * pointers are then used directly.
 */
#include <yousj/types.h>
#include <yousj/syscall.h>
#include <yousj/sched.h>
#include <yousj/user.h>
#include <yousj/gdt.h>
#include <yousj/vmm.h>
#include <yousj/elf.h>
#include <yousj/pmm.h>
#include <drivers/serial.h>

/* Per-CPU data reached via GS after swapgs (SYSCALL_DESIGN 3.3).
 * Field offsets are CONTRACT with syscall_entry.S:
 *   +0 kernel_rsp, +8 user_rsp_save, +16 current_task,
 *   +24 syscall_frame. */
struct cpu_local {
    uint64_t kernel_rsp;
    uint64_t user_rsp_save;
    uint64_t current_task;
    uint64_t syscall_frame; /* entry-pushed user frame (clone) */
};

static struct cpu_local g_cpu_local __attribute__((aligned(64)));
static struct task *g_init_task;

static void user_gs_normalize(void);

void syscall_set_init_task(struct task *t)
{
    g_init_task = t;
}

#define MSR_FS_BASE 0xC0000100u

static void wrmsr(uint32_t msr, uint64_t value);

/* Called from sched_schedule() on every switch (weak hook, see
 * sched.c): TSS.rsp0 and cpu_local.kernel_rsp must always name the
 * CURRENT task's kernel stack, with one value from one place --
 * the task's kernel_stack_top -- per the design's update rule.
 * The FS base follows the incoming task (TLS for user tasks, 0
 * for kernel threads, whose structs are zero-initialised). */
void arch_on_task_switch(struct task *next)
{
    tss_set_rsp0(next->kernel_stack_top);
    g_cpu_local.kernel_rsp = next->kernel_stack_top;
    g_cpu_local.current_task = (uint64_t)(uintptr_t)next;
    wrmsr(MSR_FS_BASE, next->fs_base);
}

/* --- MSR setup ------------------------------------------------------ */

#define MSR_EFER           0xC0000080u
#define MSR_STAR           0xC0000081u
#define MSR_LSTAR          0xC0000082u
#define MSR_SFMASK         0xC0000084u
#define MSR_GS_BASE        0xC0000101u
#define MSR_KERNEL_GS_BASE 0xC0000102u
#define EFER_SCE           (1ULL << 0)

extern void syscall_entry(void);

static void wrmsr(uint32_t msr, uint64_t value)
{
    __asm__ volatile("wrmsr" : : "c"(msr),
                     "a"((uint32_t)(value & 0xFFFFFFFFu)),
                     "d"((uint32_t)(value >> 32)));
}

static uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

void syscall_init(void)
{
    /* STAR: per the SDM, syscall takes CS/SS from [63:48]
     * (0x08 kernel code / 0x10 kernel data) and sysret takes
     * them from [47:32] (+16/+8 with RPL forced to 3: 0x23
     * user code / 0x1B user data). QEMU TCG derives the sysret
     * selectors from [63:48] instead, so on QEMU a task comes
     * back from a syscall running with CS=0x1B / SS=0x13
     * (fabricated caches; execution is unaffected) and a timer
     * frame captured in ring 3 carries those values, which
     * iretq would reject against the real GDT. irq0_stub
     * canonicalizes ring-3 frames (CS=0x23, SS=0x1B) before
     * iretq, which is a no-op on silicon. */
    wrmsr(MSR_STAR, (0x08ULL << 48) | (0x10ULL << 32));
    wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)&syscall_entry);
    wrmsr(MSR_SFMASK, 0x700); /* IF + TF + DF masked on entry */
    wrmsr(MSR_KERNEL_GS_BASE, (uint64_t)(uintptr_t)&g_cpu_local);
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);
}

/* --- shared helpers --------------------------------------------------- */

/* Errno values are Linux's (the ABI promises the same numbers). */
#define YOUSJ_EPERM  1
#define YOUSJ_ESRCH  3
#define YOUSJ_EBADF  9
#define YOUSJ_ECHILD 10
#define YOUSJ_ENOENT 2
#define YOUSJ_ENOMEM 12
#define YOUSJ_EFAULT 14
#define YOUSJ_ENODEV 19
#define YOUSJ_EINVAL 22
#define YOUSJ_ENOSYS 38

/* The one user-pointer rule (SYSCALL_DESIGN 4.3): the whole range
 * must lie in the sanctioned window. */
static int user_range_ok(uint64_t ptr, uint64_t len)
{
    if (ptr < YOUSJ_USER_BASE || ptr >= YOUSJ_USER_LIMIT)
        return 0;
    if (len > YOUSJ_USER_LIMIT - ptr)
        return 0;
    return 1;
}

static void mem_fill(void *dst, uint8_t v, uint64_t n)
{
    uint8_t *p = (uint8_t *)dst;
    while (n--)
        *p++ = v;
}


/* Map one fresh zeroed frame into the caller's space at va. */
static int map_zero_page(struct vmm_space *space, uint64_t va,
                         uint32_t flags)
{
    uint64_t frame = pmm_alloc_page();
    if (frame == 0)
        return -1;
    mem_fill(yousj_phys_to_virt(frame), 0, YOUSJ_PAGE_SIZE);
    if (vmm_map(space, va, frame, YOUSJ_PAGE_SIZE, flags) != 0) {
        (void)pmm_free_page(frame);
        return -1;
    }
    return 0;
}

/* Unmap one page in the caller's space and free its frame. */
static void unmap_free_page(struct vmm_space *space, uint64_t va)
{
    uint64_t frame = 0;
    if (vmm_translate(space, va, &frame, NULL) == 0) {
        if (vmm_unmap(space, va, YOUSJ_PAGE_SIZE) == 0)
            (void)pmm_free_page(frame);
    }
}

/* --- batch 0 ---------------------------------------------------------- */

static long sys_write(long fd, uint64_t buf, uint64_t len)
{
    uint64_t i;

    if (fd != 1 && fd != 2)
        return -YOUSJ_EBADF;
    if (len > 8192)
        len = 8192;
    if (!user_range_ok(buf, len))
        return -YOUSJ_EFAULT;
    for (i = 0; i < len; i++)
        serial_putc(((const char *)(uintptr_t)buf)[i]);
    return (long)len;
}

static void sys_exit_common(long status)
{
    struct task *me = sched_current();

    if (g_init_task != NULL && me == g_init_task) {
        /* Linux: "Attempted to kill init". Stage 1 has no way to
         * recover the console once init is gone, so this is a
         * panic, exactly as the design prescribes. */
        serial_puts("\nYousj OS: PANIC: Attempted to kill init (exit ");
        serial_put_u64((uint64_t)status);
        serial_puts("), halting\n");
        for (;;)
            __asm__ volatile("cli; hlt");
    }

    /* Process teardown (SCHED_DESIGN 6.1): an owned address
     * space is destroyed here, while this task is still the one
     * running -- detach it from the task first and switch CR3
     * to the master, so no later schedule can ever reload the
     * dying space, then free its data frames and its tables.
     * Non-owned spaces (the boot images) are deliberately kept,
     * as before. This task's kernel stack survives until the
     * parent reaps the zombie in wait4. */
    if (me->mm != NULL && me->mm_owned) {
        struct vmm_space *sp = me->mm;
        struct vmm_space *master = vmm_kernel_master();

        me->mm = NULL;
        me->mm_owned = false;
        if (master != NULL)
            vmm_load_cr3(master->pml4_phys);
        vmm_free_user_data(sp);
        vmm_destroy_space(sp);
        vmm_space_release(sp);
    }

    /* Orphans are adopted by init, which reaps them (6.1). */
    if (g_init_task != NULL)
        sched_adopt_children(me->tid, g_init_task->tid);

    sched_exit_current((int)status);
}

/* --- batch 2: the process family (M5) ----------------------------------- */

#define YOUSJ_WNOHANG 1
#define YOUSJ_RUSAGE_SIZE 144 /* struct rusage, zero-filled */

static long sys_wait4(long pid, uint64_t status_ptr, long options,
                      uint64_t rusage_ptr)
{
    struct task *me = sched_current();

    if (pid == 0 || pid < -1)
        return -YOUSJ_EINVAL; /* no process groups in this subset */
    if (options & ~YOUSJ_WNOHANG)
        return -YOUSJ_EINVAL;

    for (;;) {
        struct task *z = sched_find_zombie_child(me->tid, (int)pid);

        if (z != NULL) {
            int raw = z->exit_status;
            uint64_t ztid = z->tid;

            /* Validate and fill the outputs BEFORE reaping: on
             * EFAULT the zombie stays for a corrected retry. */
            if (status_ptr != 0) {
                if (!user_range_ok(status_ptr, 4))
                    return -YOUSJ_EFAULT;
                *(int *)(uintptr_t)status_ptr = raw << 8;
            }
            if (rusage_ptr != 0) {
                uint64_t i;
                if (!user_range_ok(rusage_ptr, YOUSJ_RUSAGE_SIZE))
                    return -YOUSJ_EFAULT;
                for (i = 0; i < YOUSJ_RUSAGE_SIZE; i++)
                    ((uint8_t *)(uintptr_t)rusage_ptr)[i] = 0;
            }
            sched_reap_zombie(z);
            return (long)ztid;
        }
        if (!sched_has_children(me->tid, (int)pid))
            return -YOUSJ_ECHILD;
        if (options & YOUSJ_WNOHANG)
            return 0;
        /* Sleep until a matching child exits: sched_exit_current
         * wakes us through wait_target. */
        me->wait_target = pid;
        sched_block_current();
        me->wait_target = 0;
    }
}

/* clone(56), fork-style only (SCHED_DESIGN 6.2): flags may carry
 * nothing but the exit signal (0 or SIGCHLD); a non-NULL child
 * stack, CLONE_VM/VFORK/THREAD and friends are -EINVAL, never
 * silently ignored. The child gets a deep copy of the user
 * window and resumes at the parent's syscall return point with
 * rax = 0, from the register frame the entry pushed. */
#define YOUSJ_SIGCHLD 17

extern void user_frame_resume(uint64_t *resume_fields);

static void clone_child_start(void *arg)
{
    struct task *me = sched_current();

    (void)arg;
    user_gs_normalize();
    user_frame_resume(&me->resume_rdi);
    for (;;) {
        /* unreachable */
    }
}

static long sys_clone(long flags, uint64_t child_stack)
{
    struct task *me = sched_current();
    const uint64_t *frame;
    struct vmm_space *space;
    struct task *child;
    uint64_t flags_saved;

    if (me->mm == NULL)
        return -YOUSJ_EINVAL; /* kernel threads cannot fork */
    if (child_stack != 0)
        return -YOUSJ_EINVAL;
    if ((flags & ~0xFFL) != 0 ||
        ((flags & 0xFF) != 0 && (flags & 0xFF) != YOUSJ_SIGCHLD))
        return -YOUSJ_EINVAL;
    frame = (const uint64_t *)(uintptr_t)g_cpu_local.syscall_frame;
    if (frame == NULL)
        return -YOUSJ_EINVAL;

    space = vmm_space_alloc();
    if (space == NULL)
        return -YOUSJ_ENOMEM;
    if (vmm_create_space(space) != 0) {
        vmm_space_release(space);
        return -YOUSJ_ENOMEM;
    }
    if (vmm_clone_user_window(space, me->mm) != 0) {
        vmm_free_user_data(space);
        vmm_destroy_space(space);
        vmm_space_release(space);
        return -YOUSJ_ENOMEM;
    }

    /* Create + bind under cli, as in the M4 boot wiring: the
     * child must not be scheduled before its space and resume
     * frame are in place. */
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags_saved) : : "memory");
    child = kthread_create_auto("clone", clone_child_start, NULL);
    if (child != NULL) {
        child->mm = space;
        child->mm_owned = true;
        child->brk_start = me->brk_start;
        child->brk_end = me->brk_end;
        child->fs_base = me->fs_base;
        child->mmap_next = me->mmap_next;
        child->resume_r9  = frame[0];
        child->resume_r8  = frame[16 / 8];
        child->resume_r10 = frame[24 / 8];
        child->resume_rdx = frame[32 / 8];
        child->resume_rsi = frame[40 / 8];
        child->resume_rdi = frame[48 / 8];
        child->resume_rcx = frame[56 / 8];
        child->resume_r11 = frame[64 / 8];
        child->resume_rsp = g_cpu_local.user_rsp_save;
        child->has_resume = true;
    }
    __asm__ volatile("push %0; popfq" : : "r"(flags_saved) : "memory");

    if (child == NULL) {
        vmm_free_user_data(space);
        vmm_destroy_space(space);
        vmm_space_release(space);
        return -YOUSJ_ENOMEM;
    }
    return (long)child->tid;
}


/* --- execve: the embedded image registry -------------------------------- */

/* Until a filesystem exists, execve resolves paths against the
 * images embedded in the kernel (the same transition option (b)
 * the boot init uses, SCHED_DESIGN sec. 7.1). */
extern const uint8_t _binary_yousj_init_elf_start[];
extern const uint8_t _binary_yousj_init_elf_end[];
extern const uint8_t _binary_hello_elf_start[];
extern const uint8_t _binary_hello_elf_end[];
extern const uint8_t _binary_sh_elf_start[];
extern const uint8_t _binary_sh_elf_end[];

struct exec_image {
    const char   *path;
    const uint8_t *start;
    const uint8_t *end;
};

static const struct exec_image g_exec_images[] = {
    { "/init",      _binary_yousj_init_elf_start, _binary_yousj_init_elf_end },
    { "/bin/hello", _binary_hello_elf_start,      _binary_hello_elf_end },
    { "/bin/sh",    _binary_sh_elf_start,         _binary_sh_elf_end },
};

static int kstreq(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* Bounded copies out of user memory (the caller's space is
 * active, so user pointers dereference directly after the
 * range check). */
static long copy_user_string(uint64_t uptr, char *dst, uint64_t cap)
{
    uint64_t i;
    if (!user_range_ok(uptr, 1))
        return -YOUSJ_EFAULT;
    for (i = 0; i + 1 < cap; i++) {
        char ch = ((const char *)(uintptr_t)uptr)[i];
        dst[i] = ch;
        if (ch == '\0')
            return 0;
    }
    dst[cap - 1] = '\0';
    return 0; /* truncated: acceptable for paths at this stage */
}

static long sys_execve(uint64_t path_ptr, uint64_t argv_ptr,
                       uint64_t envp_ptr)
{
    struct task *me = sched_current();
    static char path_buf[128];
    static char arg_store[8][128];
    static char env_store[4][128];
    static const char *argv_k[9];
    static const char *envp_k[5];
    const struct exec_image *img = NULL;
    struct vmm_space *space, *old;
    uint64_t entry = 0, img_brk = 0, heap_base, new_sp = 0;
    uint64_t i;

    if (me->mm == NULL)
        return -YOUSJ_EINVAL;
    if (copy_user_string(path_ptr, path_buf, sizeof(path_buf)) != 0)
        return -YOUSJ_EFAULT;
    for (i = 0; i < sizeof(g_exec_images) / sizeof(g_exec_images[0]); i++) {
        if (kstreq(path_buf, g_exec_images[i].path)) {
            img = &g_exec_images[i];
            break;
        }
    }
    if (img == NULL)
        return -YOUSJ_ENOENT;

    /* Copy argv/envp out of the old image BEFORE building the
     * new space; everything the new image needs must be in
     * kernel hands by the commit point. */
    argv_k[0] = NULL;
    if (argv_ptr != 0) {
        uint64_t n = 0;
        if (!user_range_ok(argv_ptr, 8))
            return -YOUSJ_EFAULT;
        while (n < 8) {
            uint64_t uptr = ((const uint64_t *)(uintptr_t)argv_ptr)[n];
            if (uptr == 0)
                break;
            if (copy_user_string(uptr, arg_store[n],
                                 sizeof(arg_store[n])) != 0)
                return -YOUSJ_EFAULT;
            argv_k[n] = arg_store[n];
            n++;
        }
        argv_k[n] = NULL;
    }
    envp_k[0] = NULL;
    if (envp_ptr != 0) {
        uint64_t n = 0;
        if (!user_range_ok(envp_ptr, 8))
            return -YOUSJ_EFAULT;
        while (n < 4) {
            uint64_t uptr = ((const uint64_t *)(uintptr_t)envp_ptr)[n];
            if (uptr == 0)
                break;
            if (copy_user_string(uptr, env_store[n],
                                 sizeof(env_store[n])) != 0)
                return -YOUSJ_EFAULT;
            envp_k[n] = env_store[n];
            n++;
        }
        envp_k[n] = NULL;
    }
    if (argv_k[0] == NULL)
        argv_k[0] = path_buf; /* argv[0] defaults to the path */

    /* Build the new image on the side; on any failure the
     * current image is untouched (SYSCALL_DESIGN batch 2). */
    space = vmm_space_alloc();
    if (space == NULL)
        return -YOUSJ_ENOMEM;
    if (vmm_create_space(space) != 0)
        goto fail_release;
    if (elf_load_user_image(img->start,
                            (uint64_t)(img->end - img->start),
                            space, &entry, &img_brk) != 0)
        goto fail_destroy;
    heap_base = (img_brk + YOUSJ_PAGE_SIZE - 1) & ~(YOUSJ_PAGE_SIZE - 1);
    if (elf_build_initial_stack(space, YOUSJ_INIT_STACK_TOP,
                                argv_k, envp_k, &new_sp) != 0)
        goto fail_destroy;

    /* Commit: swap the space, drop the old one, land. This
     * syscall never returns on success. The old boot space
     * (non-owned, kmain's static) is destroyed the same way:
     * its frames and tables are ordinary PMM property, and no
     * other task references it. */
    old = me->mm;
    me->mm = space;
    me->mm_owned = true;
    me->brk_start = heap_base;
    me->brk_end = heap_base;
    me->mmap_next = YOUSJ_MMAP_BASE;
    me->fs_base = 0;
    me->has_resume = false;
    {
        const char *base = path_buf;
        const char *p = path_buf;
        while (*p != '\0') {
            if (*p == '/')
                base = p + 1;
            p++;
        }
        sched_set_name(me, base);
    }
    vmm_load_cr3(space->pml4_phys);
    if (old != NULL && old != space) {
        vmm_free_user_data(old);
        vmm_destroy_space(old);
        /* No-op for the boot space: release only matches pool
         * slots, and the boot ispace is kmain's static struct. */
        vmm_space_release(old);
    }
    user_gs_normalize();
    enter_user_mode(entry, new_sp);
    __builtin_unreachable();

fail_destroy:
    vmm_free_user_data(space);
    vmm_destroy_space(space);
fail_release:
    vmm_space_release(space);
    return -YOUSJ_ENOMEM;
}

/* --- batch 1 ------------------------------------------------------------ */

static long sys_read(long fd, uint64_t buf, uint64_t count)
{
    uint64_t got = 0;

    if (fd != 0)
        return -YOUSJ_EBADF;
    if (count == 0)
        return 0;
    if (count > 65536)
        count = 65536;
    if (!user_range_ok(buf, count))
        return -YOUSJ_EFAULT;
    /* Block for the first byte (poll the UART, sleeping between
     * checks so other tasks keep the CPU), then drain whatever is
     * already buffered, up to count. No line discipline yet: the
     * caller (init) does its own line editing and echo. */
    while (got == 0) {
        int c = serial_getc_nonblock();
        if (c >= 0) {
            ((char *)(uintptr_t)buf)[got++] = (char)c;
            break;
        }
        sched_sleep_ticks(2);
    }
    while (got < count) {
        int c = serial_getc_nonblock();
        if (c < 0)
            break;
        ((char *)(uintptr_t)buf)[got++] = (char)c;
    }
    return (long)got;
}

static long sys_nanosleep(uint64_t req_ptr)
{
    const uint64_t *ts;
    uint64_t ticks;

    if (!user_range_ok(req_ptr, 16))
        return -YOUSJ_EFAULT;
    ts = (const uint64_t *)(uintptr_t)req_ptr;
    if ((int64_t)ts[0] < 0 || ts[1] >= 1000000000ULL)
        return -YOUSJ_EINVAL;
    /* 100 Hz tick = 10 ms; round the sub-second part up so a
     * sleep never returns early. */
    ticks = ts[0] * 100 + (ts[1] + 9999999ULL) / 10000000ULL;
    if (ticks > 0)
        sched_sleep_ticks(ticks);
    return 0; /* no signals exist yet, so no -EINTR path */
}

static void uts_copy_field(char *dst, const char *src)
{
    int i = 0;
    while (i < 64 && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    while (i < 65)
        dst[i++] = '\0';
}

static long sys_uname(uint64_t ptr)
{
    char *u;
    if (!user_range_ok(ptr, 6 * 65))
        return -YOUSJ_EFAULT;
    u = (char *)(uintptr_t)ptr;
    /* The honest name (SYSCALL_DESIGN sec. 7): programs that check
     * sysname learn they are on Yousj, not on Linux. */
    uts_copy_field(u + 0 * 65, "Yousj");
    uts_copy_field(u + 1 * 65, "yousj");
    uts_copy_field(u + 2 * 65, "0.1.0");
    uts_copy_field(u + 3 * 65, "Yousj OS stage-1 (M4)");
    uts_copy_field(u + 4 * 65, "x86_64");
    uts_copy_field(u + 5 * 65, "");
    return 0;
}

#define YOUSJ_BRK_MAX_GROWTH (64ULL * 1024 * 1024)
#define YOUSJ_MMAP_MAX_LEN   (64ULL * 1024 * 1024)

static long sys_brk(uint64_t addr)
{
    struct task *me = sched_current();
    uint64_t old_pg, new_pg, pg;

    if (me->mm == NULL || me->brk_start == 0)
        return -YOUSJ_EINVAL;
    if (addr == 0)
        return (long)me->brk_end;
    if (addr < me->brk_start || addr > me->brk_start + YOUSJ_BRK_MAX_GROWTH)
        return (long)me->brk_end; /* Linux: failure returns the old break */
    old_pg = (me->brk_end + YOUSJ_PAGE_SIZE - 1) / YOUSJ_PAGE_SIZE;
    new_pg = (addr + YOUSJ_PAGE_SIZE - 1) / YOUSJ_PAGE_SIZE;
    for (pg = old_pg; pg < new_pg; pg++) {
        if (map_zero_page(me->mm, pg * YOUSJ_PAGE_SIZE,
                          YOUSJ_VMM_READ | YOUSJ_VMM_WRITE |
                          YOUSJ_VMM_USER) != 0)
            return (long)me->brk_end; /* partial growth: keep old break */
    }
    for (pg = new_pg; pg < old_pg; pg++)
        unmap_free_page(me->mm, pg * YOUSJ_PAGE_SIZE);
    me->brk_end = addr;
    return (long)addr;
}

static long sys_mmap(uint64_t addr, uint64_t len, uint64_t prot,
                     uint64_t flags, long fd)
{
    struct task *me = sched_current();
    uint64_t base, pg, npages;
    uint32_t vflags = YOUSJ_VMM_USER;

    (void)addr; /* hints are ignored; MAP_FIXED is not supported */
    (void)fd;
    if (me->mm == NULL)
        return -YOUSJ_EINVAL;
    if ((flags & 0x20) == 0) /* MAP_ANONYMOUS: file maps need a VFS */
        return -YOUSJ_ENODEV;
    if ((flags & 0x02) == 0) /* MAP_PRIVATE only (no MAP_SHARED) */
        return -YOUSJ_EINVAL;
    if (len == 0 || len > YOUSJ_MMAP_MAX_LEN)
        return -YOUSJ_EINVAL;
    if (prot & 0x1)
        vflags |= YOUSJ_VMM_READ;
    if (prot & 0x2)
        vflags |= YOUSJ_VMM_WRITE;
    if (prot & 0x4)
        vflags |= YOUSJ_VMM_EXEC;
    if (me->mmap_next == 0)
        me->mmap_next = YOUSJ_MMAP_BASE;
    base = me->mmap_next;
    npages = (len + YOUSJ_PAGE_SIZE - 1) / YOUSJ_PAGE_SIZE;
    if (base + npages * YOUSJ_PAGE_SIZE > YOUSJ_USER_LIMIT)
        return -YOUSJ_ENOMEM;
    for (pg = 0; pg < npages; pg++) {
        if (map_zero_page(me->mm, base + pg * YOUSJ_PAGE_SIZE,
                          vflags) != 0) {
            while (pg > 0) {
                pg--;
                unmap_free_page(me->mm, base + pg * YOUSJ_PAGE_SIZE);
            }
            return -YOUSJ_ENOMEM;
        }
    }
    me->mmap_next = base + npages * YOUSJ_PAGE_SIZE + YOUSJ_PAGE_SIZE;
    return (long)base;
}

static long sys_munmap(uint64_t addr, uint64_t len)
{
    struct task *me = sched_current();
    uint64_t pg, npages;

    if (me->mm == NULL || len == 0 || (addr & (YOUSJ_PAGE_SIZE - 1)) != 0 ||
        !user_range_ok(addr, len))
        return -YOUSJ_EINVAL;
    npages = (len + YOUSJ_PAGE_SIZE - 1) / YOUSJ_PAGE_SIZE;
    for (pg = 0; pg < npages; pg++)
        unmap_free_page(me->mm, addr + pg * YOUSJ_PAGE_SIZE);
    return 0; /* bump pointer is not reclaimed (stage-1 note) */
}

#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003

static long sys_arch_prctl(long code, uint64_t addr)
{
    struct task *me = sched_current();

    switch (code) {
    case ARCH_SET_FS:
        me->fs_base = addr;
        wrmsr(MSR_FS_BASE, addr);
        return 0;
    case ARCH_GET_FS:
        if (!user_range_ok(addr, 8))
            return -YOUSJ_EFAULT;
        *(uint64_t *)(uintptr_t)addr = me->fs_base;
        return 0;
    default:
        return -YOUSJ_EINVAL;
    }
}

static long sys_kill(long pid, long sig)
{
    struct task *victim;

    if (sig < 0 || sig > 64)
        return -YOUSJ_EINVAL;
    victim = sched_find_task((int)pid);
    if (victim == NULL)
        return -YOUSJ_ESRCH;
    /* Stage-1 semantics (sched_kill_task): only READY/BLOCKED
     * tasks can be retired, and retirement is total. Real signal
     * delivery arrives with the process family in M5. */
    if (sched_kill_task(victim) != 0)
        return -YOUSJ_EPERM;
    return 0;
}

static long sys_ps_snapshot(uint64_t buf, uint64_t len)
{
    if (len == 0)
        return 0;
    if (!user_range_ok(buf, len))
        return -YOUSJ_EFAULT;
    return (long)sched_snapshot_tasks((char *)(uintptr_t)buf, len);
}

/* --- dispatcher --------------------------------------------------------- */

long yousj_syscall_dispatch(long no, long a1, long a2, long a3,
                            long a4, long a5, long a6)
{
    switch (no) {
    case YOUSJ_SYS_read:
        return sys_read(a1, (uint64_t)a2, (uint64_t)a3);
    case YOUSJ_SYS_write:
        return sys_write(a1, (uint64_t)a2, (uint64_t)a3);
    case YOUSJ_SYS_mmap:
        return sys_mmap((uint64_t)a1, (uint64_t)a2, (uint64_t)a3,
                        (uint64_t)a4, a5);
    case YOUSJ_SYS_munmap:
        return sys_munmap((uint64_t)a1, (uint64_t)a2);
    case YOUSJ_SYS_brk:
        return sys_brk((uint64_t)a1);
    case YOUSJ_SYS_arch_prctl:
        return sys_arch_prctl(a1, (uint64_t)a2);
    case YOUSJ_SYS_nanosleep:
        return sys_nanosleep((uint64_t)a1);
    case YOUSJ_SYS_getpid:
        return (long)sched_current()->tid;
    case YOUSJ_SYS_kill:
        return sys_kill(a1, a2);
    case YOUSJ_SYS_uname:
        return sys_uname((uint64_t)a1);
    case YOUSJ_SYS_wait4:
        return sys_wait4(a1, (uint64_t)a2, a3, (uint64_t)a4);
    case YOUSJ_SYS_clone:
        return sys_clone(a1, (uint64_t)a2);
    case YOUSJ_SYS_execve:
        return sys_execve((uint64_t)a1, (uint64_t)a2, (uint64_t)a3);
    case YOUSJ_SYS_exit:
    case YOUSJ_SYS_exit_group:
        sys_exit_common(a1);
        __builtin_unreachable();
    case YOUSJ_SYS_NATIVE_PS_SNAPSHOT:
        return sys_ps_snapshot((uint64_t)a1, (uint64_t)a2);
    case YOUSJ_SYS_NATIVE_PMM_FREE:
        return (long)pmm_free_page_count();
    default:
        (void)a4;
        (void)a5;
        (void)a6;
        return -YOUSJ_ENOSYS;
    }
}


/* --- user task landing -------------------------------------------------- */

/* Normalize the GS pair for a ring-3 landing. User tasks always
 * run in ring 3 with GS base 0 (nothing can change it there: no
 * WRGSBASE, no SET_GS), so the pair has exactly two states --
 * ring 3: GS=0, KGSB=&g_cpu_local; kernel: GS=&g_cpu_local,
 * KGSB=0 -- and swapgs toggles between them at syscall
 * entry/exit. A syscall that never returns (exit/exit_group)
 * leaves the pair in the kernel state, which once poisoned the
 * next task's first swapgs (it installed 0 as the kernel GS
 * base and gs:0 read physical address 0). Never rely on the
 * inherited state. Safe in kernel context because no kernel
 * code dereferences GS outside syscall entry (g_cpu_local is
 * an ordinary C global). */
static void user_gs_normalize(void)
{
    wrmsr(MSR_GS_BASE, 0);
    wrmsr(MSR_KERNEL_GS_BASE, (uint64_t)(uintptr_t)&g_cpu_local);
}

void user_task_start(void *arg)
{
    struct user_start *u = (struct user_start *)arg;
    struct task *me = sched_current();

    me->user_ip = u->ip;
    me->user_sp = u->sp;
    /* kernel_rsp / TSS.rsp0 were set by arch_on_task_switch when
     * this task was scheduled in. The address space switch also
     * happened in sched_schedule(). */
    user_gs_normalize();
    enter_user_mode(u->ip, u->sp);
    for (;;) {
        /* unreachable: leaving ring 3 happens via syscalls only */
    }
}

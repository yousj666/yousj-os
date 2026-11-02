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
#include <yousj/signal.h>
#include <yousj/fpu.h>
#include <yousj/fs.h>
#include <yousj/sched.h>
#include <yousj/user.h>
#include <yousj/gdt.h>
#include <yousj/vmm.h>
#include <yousj/elf.h>
#include <yousj/pmm.h>
#include <drivers/serial.h>

static void vma_release_in(struct vmm_space *sp,
                           struct yousj_vma *v, bool unmap_pages);
static void vmas_release_all(struct task *me,
                             struct vmm_space *sp);

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
static struct yousj_file *fd_file_of(struct task *t, long fd); /* M9, defined with sys_read */
static void resolve_path(struct task *t, const char *path, char *out);
static long sys_read_pipe(struct yousj_file *f, uint64_t buf, uint64_t count);
static long sys_write_pipe(struct yousj_file *f, uint64_t buf, uint64_t len);

/* Called from sched_schedule() on every switch (weak hook, see
 * sched.c): TSS.rsp0 and cpu_local.kernel_rsp must always name the
 * CURRENT task's kernel stack, with one value from one place --
 * the task's kernel_stack_top -- per the design's update rule.
 * The FS base follows the incoming task (TLS for user tasks, 0
 * for kernel threads, whose structs are zero-initialised).
 *
 * M8 FPU switching lives here too: the outgoing user task's
 * x87/SSE state is saved to its fx_area and the incoming user
 * task's restored. Kernel threads never touch the FPU, so a
 * kernel interlude leaves the hardware state alone -- the
 * owning user task's area was saved before any other user task
 * could be restored over it. This hook runs on the outgoing
 * task's context with the incoming address space loaded; both
 * areas are kernel BSS, mapped in every space. */
void arch_on_task_switch(struct task *prev, struct task *next)
{
    if (prev != NULL && prev != next && prev->mm != NULL)
        fpu_save(prev->fx_area);
    if (next != prev && next->mm != NULL)
        fpu_restore(next->fx_area);
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
#define YOUSJ_EIO_L 5    /* M9 */
#define YOUSJ_ENOEXEC_L 8 /* M9 */
#define YOUSJ_ENOTDIR 20 /* M9 */
#define YOUSJ_EISDIR 21  /* M9 */
#define YOUSJ_EINVAL 22
#define YOUSJ_EMFILE 24  /* M9 */
#define YOUSJ_EAGAIN 11  /* M10 */
#define YOUSJ_ESPIPE 29  /* M9 */
#define YOUSJ_EPIPE 32   /* M10 */
#define YOUSJ_ERANGE 34  /* M9 */
#define YOUSJ_ENOSYS 38
#define YOUSJ_ERESTARTSYS 512 /* internal: interrupted blocking call;
                                 converted by the signal gate, never
                                 a user-visible return (signal.c) */

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

int yousj_user_range_ok(uint64_t ptr, uint64_t len)
{
    return user_range_ok(ptr, len);
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

static long sys_write_console(uint64_t buf, uint64_t len)
{
    uint64_t i;

    if (len > 8192)
        len = 8192;
    if (!user_range_ok(buf, len))
        return -YOUSJ_EFAULT;
    for (i = 0; i < len; i++)
        serial_putc(((const char *)(uintptr_t)buf)[i]);
    return (long)len;
}

static long sys_write(long fd, uint64_t buf, uint64_t len)
{
    struct yousj_file *f = fd_file_of(sched_current(), fd);

    if (f == NULL)
        return -YOUSJ_EBADF;
    if (f->pipe != NULL)
        return sys_write_pipe(f, buf, len);
    if (f->backend == 0 && f->node == NULL)
        return sys_write_console(buf, len);
    if (len == 0)
        return 0;
    if (!user_range_ok(buf, len))
        return -YOUSJ_EFAULT;
    return file_write(f, (const uint8_t *)(uintptr_t)buf, len);
}

/* The one process-exit path: exit/exit_group arrive with
 * sig == 0 and a status; fatal signal delivery (signal.c) with
 * sig != 0. Records how the task died for wait4, tears the
 * address space down, and never returns. */
void process_exit(long status, int sig)
{
    struct task *me = sched_current();

    if (g_init_task != NULL && me == g_init_task) {
        /* Linux: "Attempted to kill init". Stage 1 has no way to
         * recover the console once init is gone, so this is a
         * panic, exactly as the design prescribes. */
        if (sig != 0) {
            serial_puts("\nYousj OS: PANIC: Attempted to kill init "
                        "(signal ");
            serial_put_u64((uint64_t)sig);
            serial_puts("), halting\n");
        } else {
            serial_puts("\nYousj OS: PANIC: Attempted to kill init "
                        "(exit ");
            serial_put_u64((uint64_t)status);
            serial_puts("), halting\n");
        }
        for (;;)
            __asm__ volatile("cli; hlt");
    }
    me->sig_killed = sig;

    /* M14: release VMAs first -- MAP_SHARED regions write
     * back while the address space is still intact, and each
     * VMA's own file reference drops here (the fd references
     * below are separate). Pages die with the space. */
    vmas_release_all(me, me->mm);

    /* M9: close every open file (shared objects are refcounted;
     * the console singleton ignores the put). */
    {
        int i;
        for (i = 0; i < YOUSJ_NR_OPEN; i++) {
            if (me->fd_file[i] != NULL) {
                file_put(me->fd_file[i]);
                me->fd_file[i] = NULL;
            }
        }
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

    /* SIGCHLD to the parent (M7), carrying the same status word
     * wait4 will report. It is queued before the zombie transition
     * below; sched_exit_current() additionally wakes a parent
     * blocked in wait4 on us, so the notification cannot be lost
     * even when the parent has no SIGCHLD handler. */
    {
        struct task *parent = sched_find_task((int)me->parent_tid);
        if (parent != NULL && parent->mm != NULL) {
            parent->sig_chld_status = sig != 0
                ? (sig | (signal_is_core(sig) ? 0x80 : 0))
                : (int)(status << 8);
            (void)signal_send(parent, YOUSJ_SIGCHLD, (int)me->tid);
        }
    }

    sched_exit_current(sig != 0 ? 0 : (int)status);
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
            /* Status word (Linux layout): exit code in the high
             * byte for exit(); the signal number in the low 7
             * bits (plus the 0x80 core bit) for a signal death. */
            int word = z->sig_killed != 0
                ? (z->sig_killed |
                   (signal_is_core(z->sig_killed) ? 0x80 : 0))
                : (z->exit_status << 8);
            uint64_t ztid = z->tid;

            /* Validate and fill the outputs BEFORE reaping: on
             * EFAULT the zombie stays for a corrected retry. */
            if (status_ptr != 0) {
                if (!user_range_ok(status_ptr, 4))
                    return -YOUSJ_EFAULT;
                *(int *)(uintptr_t)status_ptr = word;
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
        /* A signal wake is honoured only after the zombie scan
         * above had its chance (a child that exited is reported
         * first, exactly like Linux). */
        if (me->sig_wake) {
            me->sig_wake = false;
            if (signal_deliverable_pending(me))
                return -YOUSJ_ERESTARTSYS;
        }
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

static long sys_clone(struct yousj_syscall_ctx *ctx, long flags,
                      uint64_t child_stack)
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
    frame = ctx->frame;
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
        /* Callee-saved set, captured by the entry shim (M7):
         * without it the child resumes with garbage in exactly
         * the registers compiled code keeps its locals in. */
        child->resume_rbx = ctx->callee[0];
        child->resume_rbp = ctx->callee[1];
        child->resume_r12 = ctx->callee[2];
        child->resume_r13 = ctx->callee[3];
        child->resume_r14 = ctx->callee[4];
        child->resume_r15 = ctx->callee[5];
        child->has_resume = true;
        /* FPU state is inherited across clone (M8): the child's
         * area (pristine since creation) becomes a copy of the
         * parent's LIVE state. The parent is the running task,
         * so its live registers -- not its possibly stale saved
         * area -- are the truth: save first, then copy. */
        fpu_save(me->fx_area);
        {
            uint64_t i;
            for (i = 0; i < YOUSJ_FX_SIZE; i++)
                child->fx_area[i] = me->fx_area[i];
        }
        signal_clone_inherit(child, me);
        /* M9: the fd table is copied and every open-file object
         * shared (refcount++), so offsets are shared with the
         * parent exactly like Linux. cwd is a value copy. */
        {
            int i;
            for (i = 0; i < YOUSJ_NR_OPEN; i++) {
                child->fd_file[i] = me->fd_file[i];
                file_get(child->fd_file[i]);
            }
            child->fd_console_closed = me->fd_console_closed;
            child->fd_cloexec = me->fd_cloexec;
            /* M14: VMAs copy with the address space (the pages
             * themselves are deep-copied by the window clone);
             * file backings gain a reference per child VMA. */
            for (i = 0; i < YOUSJ_MAX_VMA; i++) {
                child->vmas[i] = me->vmas[i];
                if (child->vmas[i].used &&
                    child->vmas[i].file != NULL)
                    file_get(child->vmas[i].file);
            }
            for (i = 0; i < YOUSJ_CWD_MAX; i++)
                child->cwd_path[i] = me->cwd_path[i];
        }
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
    uint64_t stage_phys = 0, stage_len = 0, stage_pages = 0;
    struct vmm_space *space, *old;
    uint64_t entry = 0, img_brk = 0, heap_base, new_sp = 0;

    if (me->mm == NULL)
        return -YOUSJ_EINVAL;
    if (copy_user_string(path_ptr, path_buf, sizeof(path_buf)) != 0)
        return -YOUSJ_EFAULT;
    /* M9: the image comes from the ramfs. Resolve the path,
     * stage the file into a contiguous PMM run (the HHDM makes
     * contiguous physical pages contiguous virtually, which is
     * what the ELF loader's pointer-based parser needs). */
    {
        static char resolved[256];
        bool is_dir = false;
        uint64_t len = 0, pages;
        long got;

        resolve_path(me, path_buf, resolved);
        /* Either backend: ramfs or the /data disk (M12). */
        if (!fs_path_stat(resolved, &is_dir, &len))
            return -YOUSJ_ENOENT;
        if (is_dir)
            return -YOUSJ_EISDIR;
        if (len == 0)
            return -YOUSJ_ENOEXEC_L;
        pages = (len + YOUSJ_PAGE_SIZE - 1) / YOUSJ_PAGE_SIZE;
        stage_phys = pmm_alloc_pages(pages);
        if (stage_phys == 0)
            return -YOUSJ_ENOMEM;
        got = fs_read_all_path(
            resolved, (uint8_t *)yousj_phys_to_virt(stage_phys),
            len);
        if (got != (long)len) {
            pmm_free_pages(stage_phys, pages);
            return got < 0 ? got : -YOUSJ_EIO_L;
        }
        stage_len = len;
        stage_pages = pages;
    }

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
    if (elf_load_user_image(
            (const uint8_t *)yousj_phys_to_virt(stage_phys),
            stage_len, space, &entry, &img_brk) != 0) {
        pmm_free_pages(stage_phys, stage_pages);
        goto fail_destroy;
    }
    pmm_free_pages(stage_phys, stage_pages); /* the loader copied
                                              * everything out */
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
    signal_exec_reset(me);
    /* M10: close-on-exec descriptors die at the commit point. */
    {
        int i;
        for (i = 0; i < YOUSJ_NR_OPEN; i++) {
            if ((me->fd_cloexec & (uint16_t)(1u << i)) &&
                me->fd_file[i] != NULL) {
                file_put(me->fd_file[i]);
                me->fd_file[i] = NULL;
            }
        }
        me->fd_cloexec = 0;
    }
    fpu_init_area(me->fx_area); /* FPU state resets on exec (M8) */
    fpu_restore(me->fx_area);   /* the new image starts pristine */
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
    /* M14: release the old image's VMAs (shared file regions
     * write back against the old space before it dies). */
    vmas_release_all(me, old != NULL ? old : me->mm);
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
    if (stage_phys != 0)
        pmm_free_pages(stage_phys, stage_pages);
    vmm_space_release(space);
    return -YOUSJ_ENOMEM;
}

/* --- batch 1 ------------------------------------------------------------ */

/* --- M9: the fd layer -------------------------------------------------
 * fd_file_of() resolves a descriptor to its open-file object.
 * Slots 0-2 with no object mean the implicit console (tasks
 * whose stdio was never explicitly opened) unless the task
 * closed that slot.
 */
static struct yousj_file *fd_file_of(struct task *t, long fd)
{
    struct yousj_file *f;
    if (t == NULL || fd < 0 || fd >= YOUSJ_NR_OPEN)
        return NULL;
    f = t->fd_file[fd];
    if (f == NULL && fd < 3 && !(t->fd_console_closed & (uint8_t)(1u << fd)))
        return fs_console_file();
    return f;
}

/* Resolve `path` (kernel memory) against the task's cwd into
 * `out` (at least 256 bytes). */
static void resolve_path(struct task *t, const char *path, char *out)
{
    uint64_t i = 0, j;
    if (path[0] != '/') {
        const char *cwd = (t != NULL && t->cwd_path[0] != '\0')
                              ? t->cwd_path
                              : "/";
        for (j = 0; cwd[j] != '\0' && i < 255; j++)
            out[i++] = cwd[j];
        if (i == 0 || out[i - 1] != '/')
            out[i++] = '/';
    }
    for (j = 0; path[j] != '\0' && i < 255; j++)
        out[i++] = path[j];
    out[i] = '\0';
}

static long sys_read_console(uint64_t buf, uint64_t count)
{
    uint64_t got = 0;

    if (count == 0)
        return 0;
    if (count > 65536)
        count = 65536;
    if (!user_range_ok(buf, count))
        return -YOUSJ_EFAULT;
    /* Block for the first byte (poll the UART, sleeping between
     * checks so other tasks keep the CPU), then drain whatever
     * is already buffered, up to count. No line discipline: the
     * caller (the shell) does its own line editing and echo. */
    while (got == 0) {
        int c = serial_getc_nonblock();
        if (c >= 0) {
            ((char *)(uintptr_t)buf)[got++] = (char)c;
            break;
        }
        sched_sleep_ticks(2);
        /* M7: a deliverable signal interrupts the wait; the
         * dispatcher-side gate turns this into -EINTR or a
         * restart, per the handler's SA_RESTART. */
        if (signal_deliverable_pending(sched_current())) {
            sched_current()->sig_wake = false;
            return -YOUSJ_ERESTARTSYS;
        }
    }
    while (got < count) {
        int c = serial_getc_nonblock();
        if (c < 0)
            break;
        ((char *)(uintptr_t)buf)[got++] = (char)c;
    }
    return (long)got;
}

static long sys_read(long fd, uint64_t buf, uint64_t count)
{
    struct yousj_file *f = fd_file_of(sched_current(), fd);

    if (f == NULL)
        return -YOUSJ_EBADF;
    if (f->pipe != NULL)
        return sys_read_pipe(f, buf, count);
    if (f->backend == 0 && f->node == NULL)
        return sys_read_console(buf, count);
    if (count == 0)
        return 0;
    if (!user_range_ok(buf, count))
        return -YOUSJ_EFAULT;
    return file_read(f, (uint8_t *)(uintptr_t)buf, count);
}

static long fd_alloc(struct task *t, struct yousj_file *f)
{
    long fd;
    for (fd = 0; fd < YOUSJ_NR_OPEN; fd++) {
        if (t->fd_file[fd] != NULL)
            continue;
        /* A NULL stdio slot that was never closed is the
         * implicit console -- occupied, not free. A closed one
         * is fair game. */
        if (fd < 3 && !(t->fd_console_closed & (uint8_t)(1u << fd)))
            continue;
        t->fd_file[fd] = f;
        if (fd < 3)
            t->fd_console_closed &= (uint8_t)~(1u << fd);
        t->fd_cloexec &= (uint16_t)~(1u << fd);
        return fd;
    }
    return -YOUSJ_EMFILE;
}

static long sys_openat(long dirfd, uint64_t path_ptr, long flags)
{
    struct task *me = sched_current();
    static char path_buf[256];
    static char resolved[256];
    struct yousj_file *f;
    long rc, fd;

    if (dirfd != YOUSJ_AT_FDCWD)
        return -YOUSJ_EBADF; /* no dirfd-relative opens yet */
    if (copy_user_string(path_ptr, path_buf, sizeof(path_buf)) != 0)
        return -YOUSJ_EFAULT;
    resolve_path(me, path_buf, resolved);
    rc = fs_open_path(resolved, flags, &f);
    if (rc != 0)
        return rc;
    fd = fd_alloc(me, f);
    if (fd < 0)
        file_put(f);
    else if (flags & YOUSJ_O_CLOEXEC)
        me->fd_cloexec |= (uint16_t)(1u << fd);
    return fd;
}

static long sys_close(long fd)
{
    struct task *me = sched_current();
    struct yousj_file *f;

    if (fd < 0 || fd >= YOUSJ_NR_OPEN)
        return -YOUSJ_EBADF;
    f = me->fd_file[fd];
    if (f == NULL) {
        /* Closing an implicit console slot is legal and final:
         * the fd stops resolving to the console. */
        if (fd < 3 && !(me->fd_console_closed & (uint8_t)(1u << fd))) {
            me->fd_console_closed |= (uint8_t)(1u << fd);
            return 0;
        }
        return -YOUSJ_EBADF;
    }
    me->fd_file[fd] = NULL;
    file_put(f);
    return 0;
}

static long sys_lseek(long fd, long off, long whence)
{
    struct yousj_file *f = fd_file_of(sched_current(), fd);
    if (f == NULL)
        return -YOUSJ_EBADF;
    if (f->backend == 0 && f->node == NULL)
        return -YOUSJ_ESPIPE; /* console and pipes: not seekable */
    return file_seek(f, off, whence);
}

static long sys_getdents64(long fd, uint64_t buf, uint64_t count)
{
    struct yousj_file *f = fd_file_of(sched_current(), fd);
    if (f == NULL)
        return -YOUSJ_EBADF;
    if (!user_range_ok(buf, count))
        return -YOUSJ_EFAULT;
    return file_getdents(f, (uint8_t *)(uintptr_t)buf, count);
}

static long sys_mkdir_sys(uint64_t path_ptr)
{
    struct task *me = sched_current();
    static char path_buf[256];
    static char resolved[256];

    if (copy_user_string(path_ptr, path_buf, sizeof(path_buf)) != 0)
        return -YOUSJ_EFAULT;
    resolve_path(me, path_buf, resolved);
    return fs_mkdir_path(resolved);
}

static long sys_chdir_sys(uint64_t path_ptr)
{
    struct task *me = sched_current();
    static char path_buf[256];
    static char resolved[256];
    uint64_t i;

    if (copy_user_string(path_ptr, path_buf, sizeof(path_buf)) != 0)
        return -YOUSJ_EFAULT;
    resolve_path(me, path_buf, resolved);
    {
        bool is_dir = false;
        uint64_t sz = 0;
        if (!fs_path_stat(resolved, &is_dir, &sz))
            return -YOUSJ_ENOENT;
        if (!is_dir)
            return -YOUSJ_ENOTDIR;
    }
    for (i = 0; i < YOUSJ_CWD_MAX - 1 && resolved[i] != '\0'; i++)
        me->cwd_path[i] = resolved[i];
    me->cwd_path[i] = '\0';
    return 0;
}

static long sys_getcwd_sys(uint64_t buf, uint64_t size)
{
    struct task *me = sched_current();
    const char *cwd = me->cwd_path[0] != '\0' ? me->cwd_path : "/";
    uint64_t len = 0, i;

    while (cwd[len] != '\0')
        len++;
    if (size < len + 1)
        return -YOUSJ_ERANGE;
    if (!user_range_ok(buf, len + 1))
        return -YOUSJ_EFAULT;
    for (i = 0; i <= len; i++)
        ((char *)(uintptr_t)buf)[i] = cwd[i];
    return (long)len;
}

/* --- M16: symlink / readlink -------------------------- */

static long sys_symlink(uint64_t target_ptr, uint64_t link_ptr)
{
    char target[264], linkpath[264], resolved[264];
    struct task *me = sched_current();
    long n;
    n = copy_user_string(target_ptr, target, sizeof target);
    if (n != 0)
        return n;
    n = copy_user_string(link_ptr, linkpath, sizeof linkpath);
    if (n != 0)
        return n;
    resolve_path(me, linkpath, resolved);
    return fs_symlink_path(target, resolved);
}

static long sys_symlinkat(uint64_t target_ptr, long dirfd,
                          uint64_t link_ptr)
{
    if (dirfd != YOUSJ_AT_FDCWD)
        return -YOUSJ_EBADF;
    return sys_symlink(target_ptr, link_ptr);
}

static long sys_readlink(uint64_t path_ptr, uint64_t buf_ptr,
                         uint64_t cap)
{
    char path[264], resolved[264], target[264];
    struct task *me = sched_current();
    long n;
    n = copy_user_string(path_ptr, path, sizeof path);
    if (n != 0)
        return n;
    if (!user_range_ok(buf_ptr, 1) || cap == 0)
        return -YOUSJ_EFAULT;
    if (cap > sizeof target)
        cap = sizeof target;
    resolve_path(me, path, resolved);
    n = fs_readlink_path(resolved, target, cap);
    if (n < 0)
        return n;
    if (!user_range_ok(buf_ptr, (uint64_t)n))
        return -YOUSJ_EFAULT;
    {
        uint64_t i;
        for (i = 0; i < (uint64_t)n; i++)
            ((char *)(uintptr_t)buf_ptr)[i] = target[i];
    }
    return n;
}

/* --- M15: poll / select ------------------------------------------
 * One readiness probe serves the whole family. Regular files
 * are always ready; a pipe read end is readable with data or
 * at HUP, a pipe write end is writable with space and errors
 * when readerless; the console reports input via the UART
 * status register without consuming the byte. Waiting uses
 * the same 2-tick poll + -ERESTARTSYS protocol as pipe I/O,
 * so the signal gate produces Linux's EINTR/restart split. */

#define POLLIN_K  0x0001
#define POLLPRI_K 0x0002
#define POLLOUT_K 0x0004
#define POLLERR_K 0x0008
#define POLLHUP_K 0x0010
#define POLLNVAL_K 0x0020

static short fd_ready(struct task *me, long fd, short events)
{
    struct yousj_file *f = fd_file_of(me, fd);
    short rev = 0;

    if (f == NULL)
        return POLLNVAL_K;
    if (f->pipe == NULL && f->backend == 0 && f->node == NULL) {
        /* the console: fd 0 is the readable side */
        if (fd == 0) {
            if ((events & POLLIN_K) && serial_rx_ready())
                rev |= POLLIN_K;
        } else {
            if (events & POLLOUT_K)
                rev |= POLLOUT_K;
        }
        return rev;
    }
    if (f->pipe != NULL) {
        struct yousj_pipe *p = f->pipe;
        long acc = f->flags & YOUSJ_O_ACCMODE;
        if (acc == YOUSJ_O_RDONLY) {
            if (p->count > 0)
                rev |= POLLIN_K;
            if (p->writers == 0)
                rev |= POLLHUP_K;
        } else if (acc == YOUSJ_O_WRONLY) {
            if (p->readers == 0)
                rev |= POLLERR_K | POLLHUP_K;
            else if (p->count < YOUSJ_PIPE_CAP)
                rev |= POLLOUT_K;
        }
        /* ERR/HUP/NVAL are reported regardless of the mask */
        return (short)((rev & (POLLERR_K | POLLHUP_K)) |
                       (rev & events));
    }
    /* regular file on either backend */
    return (short)(events & (POLLIN_K | POLLOUT_K));
}

struct pollfd_k {
    int32_t fd;
    int16_t events;
    int16_t revents;
};

#define POLL_MAX_NFDS 64
static struct pollfd_k g_pollfds[POLL_MAX_NFDS];

static long poll_core(uint64_t fds_ptr, uint64_t nfds,
                      int64_t timeout_ms)
{
    struct task *me = sched_current();
    uint64_t deadline = 0;
    uint64_t i;
    bool infinite = timeout_ms < 0;

    if (nfds > POLL_MAX_NFDS)
        return -YOUSJ_EINVAL;
    if (nfds > 0 && !user_range_ok(fds_ptr, nfds * 8))
        return -YOUSJ_EFAULT;
    for (i = 0; i < nfds; i++)
        g_pollfds[i] = ((const struct pollfd_k *)(uintptr_t)fds_ptr)[i];
    if (!infinite)
        deadline = sched_uptime_ticks() +
                   (uint64_t)((timeout_ms + 9) / 10);
    for (;;) {
        long ready = 0;
        for (i = 0; i < nfds; i++) {
            short rev = 0;
            if (g_pollfds[i].fd >= 0)
                rev = fd_ready(me, g_pollfds[i].fd,
                               g_pollfds[i].events);
            g_pollfds[i].revents = rev;
            if (rev != 0)
                ready++;
        }
        if (ready > 0 || (!infinite &&
                          sched_uptime_ticks() >= deadline)) {
            for (i = 0; i < nfds; i++)
                ((struct pollfd_k *)(uintptr_t)fds_ptr)[i].revents =
                    g_pollfds[i].revents;
            return ready;
        }
        if (signal_deliverable_pending(me)) {
            me->sig_wake = false;
            return -YOUSJ_ERESTARTSYS;
        }
        sched_sleep_ticks(2);
    }
}

static long sys_poll_sys(uint64_t fds_ptr, uint64_t nfds,
                         long timeout_ms)
{
    return poll_core(fds_ptr, nfds, timeout_ms);
}

static int64_t timespec_ms(uint64_t ptr, bool *bad)
{
    const uint64_t *ts;
    *bad = false;
    if (!user_range_ok(ptr, 16)) {
        *bad = true;
        return 0;
    }
    ts = (const uint64_t *)(uintptr_t)ptr;
    if ((int64_t)ts[0] < 0 || ts[1] >= 1000000000ULL) {
        *bad = true;
        return 0;
    }
    return (int64_t)ts[0] * 1000 +
           (int64_t)((ts[1] + 999999ULL) / 1000000ULL);
}

static long sys_ppoll_sys(uint64_t fds_ptr, uint64_t nfds,
                          uint64_t ts_ptr)
{
    bool bad;
    int64_t ms;
    if (ts_ptr == 0)
        return poll_core(fds_ptr, nfds, -1);
    ms = timespec_ms(ts_ptr, &bad);
    if (bad)
        return -YOUSJ_EINVAL;
    return poll_core(fds_ptr, nfds, ms);
}

/* select: three 1024-bit sets in static storage */
#define SEL_WORDS 16
static uint64_t g_sel_in[3][SEL_WORDS];
static uint64_t g_sel_out[3][SEL_WORDS];

static int sel_get(const uint64_t *set, long fd)
{
    return (int)((set[fd / 64] >> (fd % 64)) & 1ULL);
}

static void sel_put(uint64_t *set, long fd)
{
    set[fd / 64] |= 1ULL << (fd % 64);
}

static long select_core(long nfds, uint64_t rptr, uint64_t wptr,
                        uint64_t eptr, int64_t timeout_ms,
                        uint64_t tv_out_ptr)
{
    struct task *me = sched_current();
    uint64_t deadline = 0;
    bool infinite = timeout_ms < 0;
    long fd;
    int s, w;

    if (nfds < 0 || nfds > 1024)
        return -YOUSJ_EINVAL;
    for (s = 0; s < 3; s++)
        for (w = 0; w < SEL_WORDS; w++)
            g_sel_in[s][w] = 0;
    {
        const uint64_t *srcs[3];
        srcs[0] = (const uint64_t *)(uintptr_t)rptr;
        srcs[1] = (const uint64_t *)(uintptr_t)wptr;
        srcs[2] = (const uint64_t *)(uintptr_t)eptr;
        for (s = 0; s < 3; s++) {
            if (srcs[s] == NULL)
                continue;
            if (!user_range_ok((uint64_t)(uintptr_t)srcs[s],
                               ((nfds + 63) / 64) * 8))
                return -YOUSJ_EFAULT;
            for (w = 0; w < (nfds + 63) / 64; w++)
                g_sel_in[s][w] = srcs[s][w];
        }
    }
    if (!infinite)
        deadline = sched_uptime_ticks() +
                   (uint64_t)((timeout_ms + 9) / 10);
    for (;;) {
        long ready = 0;
        for (s = 0; s < 3; s++)
            for (w = 0; w < SEL_WORDS; w++)
                g_sel_out[s][w] = 0;
        for (fd = 0; fd < nfds; fd++) {
            int want_r = rptr && sel_get(g_sel_in[0], fd);
            int want_w = wptr && sel_get(g_sel_in[1], fd);
            int want_e = eptr && sel_get(g_sel_in[2], fd);
            short events = 0, rev;
            if (!want_r && !want_w && !want_e)
                continue;
            if (want_r)
                events |= POLLIN_K;
            if (want_w)
                events |= POLLOUT_K;
            if (want_e)
                events |= POLLPRI_K;
            rev = fd_ready(me, fd, events);
            if (rev & POLLNVAL_K)
                return -YOUSJ_EBADF; /* Linux select rule */
            if (rev & (POLLIN_K | POLLHUP_K)) {
                if (want_r) {
                    sel_put(g_sel_out[0], fd);
                    ready++;
                }
            }
            if (rev & POLLOUT_K) {
                if (want_w) {
                    sel_put(g_sel_out[1], fd);
                    ready++;
                }
            }
            if (rev & (POLLERR_K | POLLPRI_K)) {
                if (want_e) {
                    sel_put(g_sel_out[2], fd);
                    ready++;
                }
                if ((rev & POLLERR_K) && want_w &&
                    !(rev & POLLOUT_K)) {
                    sel_put(g_sel_out[1], fd);
                    ready++;
                }
            }
        }
        if (ready > 0 ||
            (!infinite && sched_uptime_ticks() >= deadline)) {
            uint64_t *dsts[3];
            dsts[0] = (uint64_t *)(uintptr_t)rptr;
            dsts[1] = (uint64_t *)(uintptr_t)wptr;
            dsts[2] = (uint64_t *)(uintptr_t)eptr;
            for (s = 0; s < 3; s++) {
                if (dsts[s] == NULL)
                    continue;
                for (w = 0; w < SEL_WORDS; w++)
                    dsts[s][w] = w < (nfds + 63) / 64
                                     ? g_sel_out[s][w]
                                     : 0;
            }
            if (ready == 0 && tv_out_ptr != 0) {
                /* Linux writes the unslept remainder back */
                uint64_t *tv = (uint64_t *)(uintptr_t)tv_out_ptr;
                uint64_t rem = deadline > sched_uptime_ticks()
                                   ? deadline - sched_uptime_ticks()
                                   : 0;
                tv[0] = rem / 100;
                tv[1] = (rem % 100) * 10000;
            }
            return ready;
        }
        if (signal_deliverable_pending(me)) {
            me->sig_wake = false;
            return -YOUSJ_ERESTARTSYS;
        }
        sched_sleep_ticks(2);
    }
}

static long sys_select_sys(long nfds, uint64_t r, uint64_t w,
                           uint64_t e, uint64_t tv_ptr)
{
    int64_t ms = -1;
    if (tv_ptr != 0) {
        const uint64_t *tv;
        if (!user_range_ok(tv_ptr, 16))
            return -YOUSJ_EFAULT;
        tv = (const uint64_t *)(uintptr_t)tv_ptr;
        if ((int64_t)tv[0] < 0 || tv[1] >= 1000000ULL)
            return -YOUSJ_EINVAL;
        ms = (int64_t)tv[0] * 1000 +
             (int64_t)((tv[1] + 999ULL) / 1000ULL);
    }
    return select_core(nfds, r, w, e, ms, tv_ptr);
}

static long sys_pselect_sys(long nfds, uint64_t r, uint64_t w,
                            uint64_t e, uint64_t ts_ptr)
{
    bool bad;
    int64_t ms = -1;
    if (ts_ptr != 0) {
        ms = timespec_ms(ts_ptr, &bad);
        if (bad)
            return -YOUSJ_EINVAL;
    }
    return select_core(nfds, r, w, e, ms, 0);
}

/* --- M13: unlink / rmdir / rename ------------------------------- */

static long sys_unlink_sys(uint64_t path_ptr)
{
    struct task *me = sched_current();
    static char path_buf[256];
    static char resolved[256];

    if (copy_user_string(path_ptr, path_buf, sizeof(path_buf)) != 0)
        return -YOUSJ_EFAULT;
    resolve_path(me, path_buf, resolved);
    return fs_unlink_path(resolved);
}

static long sys_rmdir_sys(uint64_t path_ptr)
{
    struct task *me = sched_current();
    static char path_buf[256];
    static char resolved[256];

    if (copy_user_string(path_ptr, path_buf, sizeof(path_buf)) != 0)
        return -YOUSJ_EFAULT;
    resolve_path(me, path_buf, resolved);
    return fs_rmdir_path(resolved);
}

static long sys_rename_sys(uint64_t old_ptr, uint64_t new_ptr)
{
    struct task *me = sched_current();
    static char old_buf[256];
    static char new_buf[256];
    static char rold[256];
    static char rnew[256];

    if (copy_user_string(old_ptr, old_buf, sizeof(old_buf)) != 0)
        return -YOUSJ_EFAULT;
    if (copy_user_string(new_ptr, new_buf, sizeof(new_buf)) != 0)
        return -YOUSJ_EFAULT;
    resolve_path(me, old_buf, rold);
    resolve_path(me, new_buf, rnew);
    return fs_rename_path(rold, rnew);
}

/* --- M10: pipes and descriptor duplication ----------------------- */

static long sys_read_pipe(struct yousj_file *f, uint64_t buf,
                          uint64_t count)
{
    struct task *me = sched_current();
    struct yousj_pipe *p = f->pipe;

    if ((f->flags & YOUSJ_O_ACCMODE) != YOUSJ_O_RDONLY)
        return -YOUSJ_EBADF;
    if (count == 0)
        return 0;
    if (!user_range_ok(buf, count))
        return -YOUSJ_EFAULT;
    for (;;) {
        if (p->count > 0) {
            uint64_t n = count < p->count ? count : p->count;
            uint64_t i;
            for (i = 0; i < n; i++) {
                ((uint8_t *)(uintptr_t)buf)[i] = p->buf[p->head];
                p->head = (p->head + 1) % YOUSJ_PIPE_CAP;
            }
            p->count -= (uint32_t)n;
            return (long)n;
        }
        if (p->writers == 0)
            return 0; /* EOF: every write end is gone */
        if (f->flags & YOUSJ_O_NONBLOCK)
            return -YOUSJ_EAGAIN;
        if (signal_deliverable_pending(me)) {
            me->sig_wake = false;
            return -YOUSJ_ERESTARTSYS;
        }
        sched_sleep_ticks(2); /* poll, like the console reader */
    }
}

static long sys_write_pipe(struct yousj_file *f, uint64_t buf,
                           uint64_t len)
{
    struct task *me = sched_current();
    struct yousj_pipe *p = f->pipe;
    uint64_t written = 0;

    if ((f->flags & YOUSJ_O_ACCMODE) != YOUSJ_O_WRONLY)
        return -YOUSJ_EBADF;
    if (len == 0)
        return 0;
    if (!user_range_ok(buf, len))
        return -YOUSJ_EFAULT;
    for (;;) {
        uint64_t space, chunk, i;
        if (p->readers == 0) {
            if (written == 0) {
                /* Linux: SIGPIPE first, then -EPIPE. A task that
                 * catches/ignores SIGPIPE sees the plain error. */
                signal_send(me, YOUSJ_SIGPIPE, me->tid);
                return -YOUSJ_EPIPE;
            }
            return (long)written;
        }
        space = YOUSJ_PIPE_CAP - p->count;
        if (space > 0) {
            chunk = len - written < space ? len - written : space;
            for (i = 0; i < chunk; i++) {
                p->buf[p->tail] =
                    ((const uint8_t *)(uintptr_t)buf)[written + i];
                p->tail = (p->tail + 1) % YOUSJ_PIPE_CAP;
            }
            p->count += (uint32_t)chunk;
            written += chunk;
            if (written == len)
                return (long)written;
            continue;
        }
        if (f->flags & YOUSJ_O_NONBLOCK)
            return written > 0 ? (long)written : -YOUSJ_EAGAIN;
        if (signal_deliverable_pending(me)) {
            me->sig_wake = false;
            return written > 0 ? (long)written : -YOUSJ_ERESTARTSYS;
        }
        sched_sleep_ticks(2);
    }
}

static long sys_pipe2_sys(uint64_t fds_ptr, long flags)
{
    struct task *me = sched_current();
    struct yousj_file *rd, *wr;
    long rc, fdr, fdw;
    int32_t *out;

    if (flags & ~(YOUSJ_O_CLOEXEC | YOUSJ_O_NONBLOCK))
        return -YOUSJ_EINVAL;
    if (!user_range_ok(fds_ptr, 8))
        return -YOUSJ_EFAULT;
    rc = fs_pipe_create(&rd, &wr);
    if (rc != 0)
        return rc;
    if (flags & YOUSJ_O_NONBLOCK) {
        rd->flags |= YOUSJ_O_NONBLOCK;
        wr->flags |= YOUSJ_O_NONBLOCK;
    }
    fdr = fd_alloc(me, rd);
    if (fdr < 0) {
        file_put(rd);
        file_put(wr);
        return fdr;
    }
    fdw = fd_alloc(me, wr);
    if (fdw < 0) {
        me->fd_file[fdr] = NULL;
        file_put(rd);
        file_put(wr);
        return fdw;
    }
    if (flags & YOUSJ_O_CLOEXEC) {
        me->fd_cloexec |= (uint16_t)(1u << fdr);
        me->fd_cloexec |= (uint16_t)(1u << fdw);
    }
    out = (int32_t *)(uintptr_t)fds_ptr;
    out[0] = (int32_t)fdr;
    out[1] = (int32_t)fdw;
    return 0;
}

static long sys_dup_sys(long oldfd)
{
    struct task *me = sched_current();
    struct yousj_file *f = fd_file_of(me, oldfd);
    long fd;

    if (f == NULL)
        return -YOUSJ_EBADF;
    file_get(f);
    fd = fd_alloc(me, f);
    if (fd < 0)
        file_put(f);
    return fd;
}

static long sys_dup3_sys(long oldfd, long newfd, long flags)
{
    struct task *me = sched_current();
    struct yousj_file *f, *old;

    if (flags & ~YOUSJ_O_CLOEXEC)
        return -YOUSJ_EINVAL;
    if (newfd < 0 || newfd >= YOUSJ_NR_OPEN)
        return -YOUSJ_EBADF;
    f = fd_file_of(me, oldfd);
    if (f == NULL)
        return -YOUSJ_EBADF;
    if (oldfd == newfd)
        return newfd;
    old = me->fd_file[newfd];
    if (old != NULL)
        file_put(old);
    me->fd_file[newfd] = f;
    file_get(f);
    if (newfd < 3)
        me->fd_console_closed &= (uint8_t)~(1u << newfd);
    if (flags & YOUSJ_O_CLOEXEC)
        me->fd_cloexec |= (uint16_t)(1u << newfd);
    else
        me->fd_cloexec &= (uint16_t)~(1u << newfd);
    return newfd;
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
    if (ticks > 0) {
        /* Sleep against an absolute deadline so a signal wake
         * can be told apart from elapsed time, and a spurious
         * early wake simply re-enters the remaining sleep. */
        uint64_t deadline = sched_uptime_ticks() + ticks;
        for (;;) {
            uint64_t now = sched_uptime_ticks();
            int woken;
            if (now >= deadline)
                break;
            woken = sched_sleep_ticks_sig(deadline - now);
            if (woken && signal_deliverable_pending(sched_current()))
                return -YOUSJ_ERESTARTSYS;
        }
    }
    return 0;
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

#define PROT_READ_K  0x1
#define PROT_WRITE_K 0x2
#define MAP_SHARED_K  0x01
#define MAP_PRIVATE_K 0x02
#define MAP_ANON_K    0x20
#define EACCES_K 13

/* Write back the intersection of [start, start+len) with the
 * VMA's pages into the backing file. The pages are reached
 * through the VMM (translate + HHDM), never by assuming the
 * caller's CR3. */
static void vma_writeback_range(struct vmm_space *sp,
                                struct yousj_vma *v,
                                uint64_t start, uint64_t len)
{
    uint64_t va, end;

    if (v->file == NULL || !v->shared || sp == NULL)
        return;
    if (start < v->base) {
        len -= v->base - start;
        start = v->base;
    }
    end = start + len;
    if (end > v->base + v->len)
        end = v->base + v->len;
    for (va = start; va < end; va += YOUSJ_PAGE_SIZE) {
        uint64_t phys, chunk = end - va;
        if (chunk > YOUSJ_PAGE_SIZE)
            chunk = YOUSJ_PAGE_SIZE;
        if (vmm_translate(sp, va, &phys, NULL) != 0)
            continue;
        fs_pwrite_file(v->file, v->file_off + (va - v->base),
                       yousj_phys_to_virt(phys), chunk);
    }
}

static void vma_release_in(struct vmm_space *sp,
                           struct yousj_vma *v, bool unmap_pages)
{
    uint64_t pg;

    if (!v->used)
        return;
    vma_writeback_range(sp, v, v->base, v->len);
    if (unmap_pages && sp != NULL) {
        for (pg = 0; pg < v->len / YOUSJ_PAGE_SIZE; pg++)
            unmap_free_page(sp, v->base + pg * YOUSJ_PAGE_SIZE);
    }
    if (v->file != NULL)
        file_put(v->file);
    v->used = false;
    v->file = NULL;
}

static void vmas_release_all(struct task *me, struct vmm_space *sp)
{
    int i;
    for (i = 0; i < YOUSJ_MAX_VMA; i++)
        if (me->vmas[i].used)
            vma_release_in(sp, &me->vmas[i], false);
}

static long sys_mmap(uint64_t addr, uint64_t len, uint64_t prot,
                     uint64_t flags, long fd, uint64_t offset)
{
    struct task *me = sched_current();
    struct yousj_file *file = NULL;
    struct yousj_vma *vma = NULL;
    uint64_t base, pg, npages;
    uint32_t vflags = YOUSJ_VMM_USER;
    bool anon = (flags & MAP_ANON_K) != 0;
    bool shared = (flags & MAP_SHARED_K) != 0;
    int i;

    (void)addr; /* hints are ignored; MAP_FIXED is unsupported */
    if (me->mm == NULL)
        return -YOUSJ_EINVAL;
    if (len == 0 || len > YOUSJ_MMAP_MAX_LEN)
        return -YOUSJ_EINVAL;
    if (shared && (flags & MAP_PRIVATE_K))
        return -YOUSJ_EINVAL;
    if (!anon && !shared && !(flags & MAP_PRIVATE_K))
        return -YOUSJ_EINVAL;
    if (!anon) {
        long acc;
        file = fd_file_of(me, fd);
        if (file == NULL)
            return -YOUSJ_EBADF;
        if (file->pipe != NULL)
            return -YOUSJ_ENODEV;
        if (file->backend == 0 && file->node == NULL)
            return -YOUSJ_ENODEV; /* console */
        if (fs_file_is_dir(file))
            return -EACCES_K; /* directories cannot be mapped */
        if ((offset & (YOUSJ_PAGE_SIZE - 1)) != 0)
            return -YOUSJ_EINVAL;
        acc = file->flags & YOUSJ_O_ACCMODE;
        if ((prot & PROT_READ_K) && acc == YOUSJ_O_WRONLY)
            return -EACCES_K;
        if (shared && (prot & PROT_WRITE_K) &&
            acc == YOUSJ_O_RDONLY)
            return -EACCES_K;
    }
    for (i = 0; i < YOUSJ_MAX_VMA; i++) {
        if (!me->vmas[i].used) {
            vma = &me->vmas[i];
            break;
        }
    }
    if (vma == NULL)
        return -YOUSJ_ENOMEM;
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
                unmap_free_page(me->mm,
                                base + pg * YOUSJ_PAGE_SIZE);
            }
            return -YOUSJ_ENOMEM;
        }
    }
    if (file != NULL) {
        /* Eager population (stage-1 model: no demand paging):
         * copy the file's bytes into the fresh pages; anything
         * past EOF stays zero. The mapping is a snapshot --
         * later write()s to the file are not visible in it. */
        file_get(file);
        for (pg = 0; pg < npages; pg++) {
            uint64_t phys;
            if (vmm_translate(me->mm,
                              base + pg * YOUSJ_PAGE_SIZE,
                              &phys, NULL) == 0)
                fs_pread_file(file, offset + pg * YOUSJ_PAGE_SIZE,
                              yousj_phys_to_virt(phys),
                              YOUSJ_PAGE_SIZE);
        }
    }
    vma->base = base;
    vma->len = npages * YOUSJ_PAGE_SIZE;
    vma->file_off = offset;
    vma->file = file;
    vma->shared = shared;
    vma->used = true;
    me->mmap_next = base + npages * YOUSJ_PAGE_SIZE + YOUSJ_PAGE_SIZE;
    return (long)base;
}

static long sys_munmap(uint64_t addr, uint64_t len)
{
    struct task *me = sched_current();
    uint64_t end;
    int i;

    if (me->mm == NULL || len == 0 ||
        (addr & (YOUSJ_PAGE_SIZE - 1)) != 0 ||
        !user_range_ok(addr, len))
        return -YOUSJ_EINVAL;
    len = (len + YOUSJ_PAGE_SIZE - 1) & ~(YOUSJ_PAGE_SIZE - 1ULL);
    end = addr + len;
    /* Stage-1 rule: a VMA is released whole or not at all --
     * partial overlaps are refused rather than split. Ranges
     * that touch no VMA (brk, ELF images) are a no-op success,
     * as in Linux. */
    for (i = 0; i < YOUSJ_MAX_VMA; i++) {
        struct yousj_vma *v = &me->vmas[i];
        if (!v->used)
            continue;
        if (addr < v->base + v->len && v->base < end) {
            if (addr > v->base || end < v->base + v->len)
                return -YOUSJ_EINVAL;
        }
    }
    for (i = 0; i < YOUSJ_MAX_VMA; i++) {
        struct yousj_vma *v = &me->vmas[i];
        if (v->used && addr <= v->base &&
            v->base + v->len <= end)
            vma_release_in(me->mm, v, true);
    }
    return 0;
}

static long sys_msync(uint64_t addr, uint64_t len, uint64_t flags)
{
    struct task *me = sched_current();
    int i;

    (void)flags; /* MS_SYNC and MS_ASYNC both write back now */
    if (me->mm == NULL ||
        (addr & (YOUSJ_PAGE_SIZE - 1)) != 0 || len == 0)
        return -YOUSJ_EINVAL;
    for (i = 0; i < YOUSJ_MAX_VMA; i++) {
        struct yousj_vma *v = &me->vmas[i];
        if (v->used && addr >= v->base &&
            addr < v->base + v->len) {
            vma_writeback_range(me->mm, v, addr, len);
            return 0;
        }
    }
    return -YOUSJ_ENOMEM; /* never mapped */
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

    if (sig < 0 || sig >= YOUSJ_NSIG)
        return -YOUSJ_EINVAL;
    victim = sched_find_task((int)pid);
    if (victim == NULL || victim->state == TASK_ZOMBIE)
        return -YOUSJ_ESRCH;
    if (sig == 0)
        return 0; /* existence check only (Linux) */
    /* Kernel threads have no signal gates, so for them kill
     * keeps its stage-1 meaning (sched_kill_task: total,
     * immediate retirement) for any signal. User tasks get real
     * queued delivery (M7). */
    if (victim->mm == NULL) {
        if (sched_kill_task(victim) != 0)
            return -YOUSJ_EPERM;
        return 0;
    }
    return signal_send(victim, (int)sig,
                       (int)sched_current()->tid);
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

/* The switch itself. Arguments arrive via the entry shim's
 * context (syscall_entry.S); results flow back through
 * yousj_syscall_dispatch(), which adds the signal exit gate. */
static long dispatch_inner(struct yousj_syscall_ctx *ctx)
{
    long no = (long)ctx->no;
    uint64_t a1 = ctx->a1, a2 = ctx->a2, a3 = ctx->a3;
    uint64_t a4 = ctx->a4, a5 = ctx->a5;

    switch (no) {
    case YOUSJ_SYS_read:
        return sys_read((long)a1, a2, a3);
    case YOUSJ_SYS_write:
        return sys_write((long)a1, a2, a3);
    case YOUSJ_SYS_open:
        return sys_openat(YOUSJ_AT_FDCWD, a1, (long)a2);
    case YOUSJ_SYS_openat:
        return sys_openat((long)a1, a2, (long)a3);
    case YOUSJ_SYS_close:
        return sys_close((long)a1);
    case YOUSJ_SYS_lseek:
        return sys_lseek((long)a1, (long)a2, (long)a3);
    case YOUSJ_SYS_getdents64:
        return sys_getdents64((long)a1, a2, a3);
    case YOUSJ_SYS_mkdir:
        return sys_mkdir_sys(a1);
    case YOUSJ_SYS_chdir:
        return sys_chdir_sys(a1);
    case YOUSJ_SYS_getcwd:
        return sys_getcwd_sys(a1, a2);
    case YOUSJ_SYS_poll:
        return sys_poll_sys(a1, a2, (long)a3);
    case YOUSJ_SYS_ppoll:
        return sys_ppoll_sys(a1, a2, a3);
    case YOUSJ_SYS_select:
        return sys_select_sys((long)a1, a2, a3, a4, a5);
    case YOUSJ_SYS_pselect6:
        return sys_pselect_sys((long)a1, a2, a3, a4, a5);
    case YOUSJ_SYS_unlink:
        return sys_unlink_sys(a1);
    case YOUSJ_SYS_symlink:
        return sys_symlink(a1, a2);
    case YOUSJ_SYS_readlink:
        return sys_readlink(a1, a2, a3);
    case YOUSJ_SYS_symlinkat:
        return sys_symlinkat(a1, (long)a2, a3);
    case YOUSJ_SYS_unlinkat:
        return (long)a1 == YOUSJ_AT_FDCWD
                   ? sys_unlink_sys(a2)
                   : -YOUSJ_EBADF; /* real dirfds unsupported */
    case YOUSJ_SYS_rmdir:
        return sys_rmdir_sys(a1);
    case YOUSJ_SYS_rename:
        return sys_rename_sys(a1, a2);
    case YOUSJ_SYS_pipe:
        return sys_pipe2_sys(a1, 0);
    case YOUSJ_SYS_pipe2:
        return sys_pipe2_sys(a1, (long)a2);
    case YOUSJ_SYS_dup:
        return sys_dup_sys((long)a1);
    case YOUSJ_SYS_dup2:
        return sys_dup3_sys((long)a1, (long)a2, 0);
    case YOUSJ_SYS_dup3:
        return sys_dup3_sys((long)a1, (long)a2, (long)a3);
    case YOUSJ_SYS_mmap:
        return sys_mmap(a1, a2, a3, a4, (long)a5, ctx->a6);
    case YOUSJ_SYS_msync:
        return sys_msync(a1, a2, a3);
    case YOUSJ_SYS_munmap:
        return sys_munmap(a1, a2);
    case YOUSJ_SYS_brk:
        return sys_brk(a1);
    case YOUSJ_SYS_arch_prctl:
        return sys_arch_prctl((long)a1, a2);
    case YOUSJ_SYS_nanosleep:
        return sys_nanosleep(a1);
    case YOUSJ_SYS_getpid:
        return (long)sched_current()->tid;
    case YOUSJ_SYS_kill:
        return sys_kill((long)a1, (long)a2);
    case YOUSJ_SYS_uname:
        return sys_uname(a1);
    case YOUSJ_SYS_wait4:
        return sys_wait4((long)a1, a2, (long)a3, a4);
    case YOUSJ_SYS_clone:
        return sys_clone(ctx, (long)a1, a2);
    case YOUSJ_SYS_execve:
        return sys_execve(a1, a2, a3);
    case YOUSJ_SYS_rt_sigaction:
        return sys_rt_sigaction((long)a1, a2, a3, a4);
    case YOUSJ_SYS_rt_sigprocmask:
        return sys_rt_sigprocmask((long)a1, a2, a3, a4);
    case YOUSJ_SYS_rt_sigpending:
        return sys_rt_sigpending(a1, a2);
    case YOUSJ_SYS_sigaltstack:
        return -YOUSJ_ENOSYS; /* no alternate stacks yet (M7 note) */
    case YOUSJ_SYS_exit:
    case YOUSJ_SYS_exit_group:
        process_exit((long)a1, 0);
        __builtin_unreachable();
    case YOUSJ_SYS_NATIVE_PS_SNAPSHOT:
        return sys_ps_snapshot(a1, a2);
    case YOUSJ_SYS_NATIVE_PMM_FREE:
        return (long)pmm_free_page_count();
    default:
        return -YOUSJ_ENOSYS;
    }
}

long yousj_syscall_dispatch(struct yousj_syscall_ctx *ctx)
{
    /* rt_sigreturn restores a saved ring-3 context wholesale;
     * the exit gate would only corrupt it, so it bypasses the
     * gate entirely (pending signals are picked up at the next
     * syscall or timer return instead). */
    if ((long)ctx->no == YOUSJ_SYS_rt_sigreturn)
        return sys_rt_sigreturn(ctx);
    return signal_syscall_exit(ctx, dispatch_inner(ctx));
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
    /* M9: a boot-created user task starts life with the console
     * on fds 0-2 (later tasks inherit stdio through clone). */
    if (me->fd_file[0] == NULL && me->fd_file[1] == NULL &&
        me->fd_file[2] == NULL) {
        me->fd_file[0] = fs_console_file();
        me->fd_file[1] = fs_console_file();
        me->fd_file[2] = fs_console_file();
    }
    /* kernel_rsp / TSS.rsp0 were set by arch_on_task_switch when
     * this task was scheduled in. The address space switch also
     * happened in sched_schedule(). */
    user_gs_normalize();
    enter_user_mode(u->ip, u->sp);
    for (;;) {
        /* unreachable: leaving ring 3 happens via syscalls only */
    }
}

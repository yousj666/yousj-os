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
#include <yousj/time.h>
#include <yousj/uaccess.h>
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
static long pipe_read_mem(struct yousj_file *f, uint8_t *dst,
                          uint64_t count);
static long sys_write_pipe(struct yousj_file *f, uint64_t buf, uint64_t len);
static long pipe_write_mem(struct yousj_file *f,
                           const uint8_t *src, uint64_t len);

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
#define YOUSJ_EACCES 13 /* M33 (EACCES_K below is the mmap-local twin) */
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
        if (vmm_unmap(space, va, YOUSJ_PAGE_SIZE) == 0) {
            /* The current CPU may hold a stale translation
             * for this VA: without the flush, a same-task
             * access after a shrink+regrow (or unmap+remap)
             * silently reads the freed frame's old content
             * instead of faulting in the new page (M22). */
            vmm_flush_tlb_va(va);
            (void)pmm_free_page(frame);
        }
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
        if (master != NULL) {
            vmm_load_cr3(master->pml4_phys);
            sched_note_cr3(master->pml4_phys);
        }
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
        /* The child's RSP is the parent's RSP at THIS
         * syscall's entry, taken from the entry frame (slot
         * YOUSJ_FR_RSP) -- NOT g_cpu_local.user_rsp_save:
         * that per-CPU slot belongs to whichever task entered
         * a syscall most recently and is not guaranteed to
         * still name this caller by the time the long window
         * clone above finishes. The entry shim saves the RSP
         * on this task's own kernel stack for exactly this
         * reason (see syscall_entry.S). */
        child->resume_rsp = frame[YOUSJ_FR_RSP];
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
        /* M28: comm is inherited across clone, as on Linux;
         * execve renames to the image basename. Without
         * this every forked child was named "clone" (the
         * create call's placeholder), which is what ps and
         * /proc/<pid>/status would forever show for
         * not-yet-exec'd children. */
        sched_set_name(child, me->name);
        /* M31: cmdline is inherited too (a forked child
         * that has not exec'd shows its parent's argv in
         * /proc, as on Linux). */
        {
            uint32_t i;
            for (i = 0; i < me->cmdline_len; i++)
                child->cmdline[i] = me->cmdline[i];
            child->cmdline_len = me->cmdline_len;
        }
        child->umask = me->umask; /* M37 */
        /* M34: environ inherits the same way. */
        {
            uint32_t i;
            for (i = 0; i < me->env_len; i++)
                child->env[i] = me->env[i];
            child->env_len = me->env_len;
        }
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
    static char exec_resolved[256]; /* M35: for me->exe_path */
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
        {
            uint64_t i;
            for (i = 0; i < sizeof(exec_resolved) - 1 &&
                        resolved[i] != '\0'; i++)
                exec_resolved[i] = resolved[i];
            exec_resolved[i] = '\0';
        }
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
    /* M31: retain the argv for /proc/<pid>/cmdline,
     * NUL-joined like Linux; truncated at the field size. */
    {
        uint32_t w = 0;
        int ai;
        for (ai = 0; argv_k[ai] != NULL; ai++) {
            const char *a = argv_k[ai];
            uint32_t j;
            for (j = 0; a[j] != '\0' && w + 1 < sizeof(me->cmdline); j++)
                me->cmdline[w++] = a[j];
            if (w + 1 < sizeof(me->cmdline))
                me->cmdline[w++] = '\0';
            else
                break;
        }
        if (w == 0 && sizeof(me->cmdline) > 1)
            me->cmdline[w++] = '\0';
        me->cmdline_len = w;
    }
    /* M34: retain envp the same way for /proc/<pid>/
     * environ. execve caps envp at 4 x 128 bytes, so the
     * 512-byte field is lossless in practice. */
    {
        uint32_t w = 0;
        int ei;
        for (ei = 0; envp_k[ei] != NULL; ei++) {
            const char *a = envp_k[ei];
            uint32_t j;
            for (j = 0; a[j] != '\0' && w + 1 < sizeof(me->env);
                 j++)
                me->env[w++] = a[j];
            if (w + 1 < sizeof(me->env))
                me->env[w++] = '\0';
            else
                break;
        }
        me->env_len = w;
    }
    /* M35: retain the resolved exec path for
     * /proc/<pid>/exe. */
    {
        uint32_t i;
        for (i = 0; i < sizeof(me->exe_path) - 1 &&
                    exec_resolved[i] != '\0'; i++)
            me->exe_path[i] = exec_resolved[i];
        me->exe_path[i] = '\0';
    }
    /* M14: release the old image's VMAs (shared file regions
     * write back against the old space before it dies). */
    vmas_release_all(me, old != NULL ? old : me->mm);
    vmm_load_cr3(space->pml4_phys);
    sched_note_cr3(space->pml4_phys);
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
/* M31: /proc/self names the CALLER. The fs layer is
 * caller-agnostic, so the substitution happens in the
 * syscall layer, the one place that knows the task:
 * "/proc/self[/...]" becomes "/proc/<tid>[/...]" before
 * the fs ever sees the path. A real file cannot be
 * shadowed -- the generated /proc owns that prefix. */
static void proc_self_rewrite(struct task *t, char *out)
{
    uint64_t j, k = 0, tid;
    char tmp[256];
    const char *pfx = "/proc/";
    char digits[20];
    int nd = 0;
    if (t == NULL)
        return;
    if (!(out[0] == '/' && out[1] == 'p' && out[2] == 'r' &&
          out[3] == 'o' && out[4] == 'c' && out[5] == '/' &&
          out[6] == 's' && out[7] == 'e' && out[8] == 'l' &&
          out[9] == 'f' &&
          (out[10] == '\0' || out[10] == '/')))
        return;
    tid = t->tid;
    while (pfx[k] != '\0') {
        tmp[k] = pfx[k];
        k++;
    }
    if (tid == 0)
        digits[nd++] = '0';
    while (tid > 0) {
        digits[nd++] = (char)('0' + (tid % 10));
        tid /= 10;
    }
    while (nd > 0)
        tmp[k++] = digits[--nd];
    for (j = 10; out[j] != '\0' && k < 255; j++)
        tmp[k++] = out[j];
    tmp[k] = '\0';
    for (j = 0; j <= k; j++)
        out[j] = tmp[j];
}

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
    proc_self_rewrite(t, out);
}

/* M30: resolve a possibly dirfd-relative path to an
 * absolute path in out[256]. Absolute paths pass through
 * (the dirfd is ignored, as on Linux); AT_FDCWD means the
 * caller's cwd; any other dirfd must name an open
 * directory, whose recorded open path becomes the base.
 * The base is the path the directory was OPENED by: if
 * the directory is renamed afterwards, resolution follows
 * the name, not the inode -- a documented divergence from
 * Linux at this stage (the fs layer is path-based). */
static long resolve_at(struct task *me, long dirfd,
                       const char *path, char *out)
{
    uint64_t i = 0, j;
    struct yousj_file *f;
    const char *base;
    if (path[0] == '/') {
        for (j = 0; path[j] != '\0' && i < 255; j++)
            out[i++] = path[j];
        out[i] = '\0';
        proc_self_rewrite(me, out);
        return 0;
    }
    if (dirfd == YOUSJ_AT_FDCWD) {
        resolve_path(me, path, out);
        return 0;
    }
    f = fd_file_of(me, dirfd);
    if (f == NULL)
        return -YOUSJ_EBADF;
    if (!fs_file_is_dir(f))
        return -YOUSJ_ENOTDIR;
    base = f->open_path;
    if (base[0] == '\0')
        return -YOUSJ_ENOTDIR; /* not opened by path */
    for (j = 0; base[j] != '\0' && i < 255; j++)
        out[i++] = base[j];
    if (i == 0 || out[i - 1] != '/')
        out[i++] = '/';
    for (j = 0; path[j] != '\0' && i < 255; j++)
        out[i++] = path[j];
    out[i] = '\0';
    proc_self_rewrite(me, out);
    return 0;
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

static long sys_openat(long dirfd, uint64_t path_ptr, long flags,
                       long mode)
{
    struct task *me = sched_current();
    static char path_buf[256];
    static char resolved[256];
    struct yousj_file *f;
    long rc, fd;

    if (copy_user_string(path_ptr, path_buf, sizeof(path_buf)) != 0)
        return -YOUSJ_EFAULT;
    rc = resolve_at(me, dirfd, path_buf, resolved);
    if (rc != 0)
        return rc;
    /* M37: the O_CREAT mode, through the caller's
     * umask. A caller passing 0 gets the historical
     * default (our own programs pass 0 casually; Linux
     * would make a 000 file, a footgun not worth
     * importing). */
    {
        uint32_t eff = 0;
        if ((flags & YOUSJ_O_CREAT) && mode != 0)
            eff = (uint32_t)mode & ~me->umask & 07777u;
        rc = fs_open_path(resolved, flags, eff, &f);
    }
    if (rc != 0)
        return rc;
    /* M30: remember the path for dirfd-relative calls
     * against this object (directories especially). */
    {
        uint64_t i;
        for (i = 0; resolved[i] != '\0' && i < 255; i++)
            f->open_path[i] = resolved[i];
        f->open_path[i] = '\0';
    }
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

/* --- M40: sendfile -----------------------------------
 * Kernel-bounced copy (a stack buffer, so a blocking
 * pipe write cannot interleave with another sendfile's
 * data). The input must be a file -- pipes as input are
 * EINVAL, the historical sendfile contract; splice
 * remains the pipe-to-pipe tool and is still ENOSYS.
 * With an offset pointer the input offset is untouched
 * and the pointer is advanced by what was copied; with
 * NULL the input's own offset advances. */
static long sys_sendfile(long out_fd, long in_fd,
                         uint64_t off_ptr, uint64_t count)
{
    struct task *me = sched_current();
    struct yousj_file *in = fd_file_of(me, in_fd);
    struct yousj_file *out = fd_file_of(me, out_fd);
    uint8_t kbuf[1024];
    uint64_t done = 0;
    int64_t off = 0;
    long result;

    if (in == NULL || out == NULL)
        return -YOUSJ_EBADF;
    if (in->pipe != NULL)
        return -YOUSJ_EINVAL;
    if (off_ptr != 0) {
        /* M42 note: fs_pread_file does no access check of
         * its own, so the positional form guards here. */
        if ((in->flags & YOUSJ_O_ACCMODE) == YOUSJ_O_WRONLY)
            return -YOUSJ_EBADF;
        if (!user_range_ok(off_ptr, 8))
            return -YOUSJ_EFAULT;
        off = *(const int64_t *)(uintptr_t)off_ptr;
        if (off < 0)
            return -YOUSJ_EINVAL;
    }
    result = 0;
    while (done < count) {
        uint64_t want = count - done;
        long n, w;
        if (want > sizeof(kbuf))
            want = sizeof(kbuf);
        if (off_ptr != 0)
            n = fs_pread_file(in, (uint64_t)off + done,
                              kbuf, want);
        else
            n = file_read(in, kbuf, want);
        if (n < 0) {
            result = n;
            goto out;
        }
        if (n == 0)
            break;
        if (out->pipe != NULL)
            w = pipe_write_mem(out, kbuf, (uint64_t)n);
        else if (out->backend == 0 && out->node == NULL)
            w = -YOUSJ_EINVAL; /* the console cannot take it */
        else
            w = file_write(out, kbuf, (uint64_t)n);
        if (w < 0) {
            result = w;
            goto out;
        }
        done += (uint64_t)w;
        if (w < n)
            break; /* short write: report what moved */
    }
    result = (long)done;
out:
    if (off_ptr != 0 && done > 0) {
        int64_t noff = off + (int64_t)done;
        *(int64_t *)(uintptr_t)off_ptr = noff;
    }
    return done > 0 ? (long)done : result;
}

/* --- M42: splice -------------------------------------
 * Same bounce discipline as sendfile, generalized:
 * either side may be the pipe (at least one must be).
 * Offsets follow the sendfile rule per side -- a pipe
 * side must pass NULL, a file side is positional with
 * a pointer and offset-carried without. SPLICE_F_*
 * flags are accepted; blocking behavior follows the
 * descriptors (documented simplification). */
static long splice_off_read(uint64_t ptr, bool is_pipe,
                            int64_t *out)
{
    if (is_pipe) {
        if (ptr != 0)
            return -YOUSJ_EINVAL;
        *out = -1;
        return 0;
    }
    if (ptr == 0) {
        *out = -1;
        return 0;
    }
    if (!user_range_ok(ptr, 8))
        return -YOUSJ_EFAULT;
    *out = *(const int64_t *)(uintptr_t)ptr;
    if (*out < 0)
        return -YOUSJ_EINVAL;
    return 0;
}

static long sys_splice(long fd_in, uint64_t off_in_ptr,
                       long fd_out, uint64_t off_out_ptr,
                       uint64_t len, long flags)
{
    struct task *me = sched_current();
    struct yousj_file *in = fd_file_of(me, fd_in);
    struct yousj_file *out = fd_file_of(me, fd_out);
    uint8_t kbuf[1024];
    uint64_t done = 0;
    int64_t off_in = -1, off_out = -1;
    long rc, result = 0;

    if (in == NULL || out == NULL)
        return -YOUSJ_EBADF;
    if (flags & ~(0x1L | 0x2L | 0x4L | 0x8L))
        return -YOUSJ_EINVAL;
    if (in->pipe == NULL && out->pipe == NULL)
        return -YOUSJ_EINVAL; /* splice needs a pipe */
    rc = splice_off_read(off_in_ptr, in->pipe != NULL,
                         &off_in);
    if (rc != 0)
        return rc;
    rc = splice_off_read(off_out_ptr, out->pipe != NULL,
                         &off_out);
    if (rc != 0)
        return rc;
    /* the positional helpers do no access checks */
    if (off_in >= 0 &&
        (in->flags & YOUSJ_O_ACCMODE) == YOUSJ_O_WRONLY)
        return -YOUSJ_EBADF;
    if (off_out >= 0 &&
        (out->flags & YOUSJ_O_ACCMODE) == YOUSJ_O_RDONLY)
        return -YOUSJ_EBADF;
    while (done < len) {
        uint64_t want = len - done;
        long n, w;
        if (want > sizeof(kbuf))
            want = sizeof(kbuf);
        if (in->pipe != NULL)
            n = pipe_read_mem(in, kbuf, want);
        else if (off_in >= 0)
            n = fs_pread_file(in, (uint64_t)off_in + done,
                              kbuf, want);
        else
            n = file_read(in, kbuf, want);
        if (n < 0) {
            result = n;
            goto out;
        }
        if (n == 0)
            break;
        if (out->pipe != NULL)
            w = pipe_write_mem(out, kbuf, (uint64_t)n);
        else if (off_out >= 0)
            w = fs_pwrite_file(out, (uint64_t)off_out + done,
                               kbuf, (uint64_t)n);
        else if (out->backend == 0 && out->node == NULL)
            w = -YOUSJ_EINVAL; /* the console again */
        else
            w = file_write(out, kbuf, (uint64_t)n);
        if (w < 0) {
            result = w;
            goto out;
        }
        done += (uint64_t)w;
        if (w < n)
            break;
    }
    result = (long)done;
out:
    if (done > 0) {
        if (off_in_ptr != 0 && off_in >= 0)
            *(int64_t *)(uintptr_t)off_in_ptr =
                off_in + (int64_t)done;
        if (off_out_ptr != 0 && off_out >= 0)
            *(int64_t *)(uintptr_t)off_out_ptr =
                off_out + (int64_t)done;
    }
    return done > 0 ? (long)done : result;
}

static long sys_mkdir_sys(uint64_t path_ptr, long mode)
{
    struct task *me = sched_current();
    static char path_buf[256];
    static char resolved[256];

    if (copy_user_string(path_ptr, path_buf, sizeof(path_buf)) != 0)
        return -YOUSJ_EFAULT;
    resolve_path(me, path_buf, resolved);
    return fs_mkdir_path(resolved, mode != 0
                                       ? ((uint32_t)mode &
                                          ~me->umask & 07777u)
                                       : 0);
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

static short fd_ready_file(struct yousj_file *f, long fd,
                           short events);

/* --- M20: the stat family ------------------------------ */

struct stat_k {
    uint64_t dev;
    uint64_t ino;
    uint64_t nlink;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t pad0;
    uint64_t rdev;
    int64_t size;
    int64_t blksize;
    int64_t blocks;
    uint64_t atime, atime_nsec;
    uint64_t mtime, mtime_nsec;
    uint64_t ctime, ctime_nsec;
    int64_t unused[3];
};

static long fill_stat(uint64_t stat_ptr,
                      const struct yousj_stat_info *info)
{
    struct stat_k *st;
    if (!user_range_ok(stat_ptr, sizeof(struct stat_k)))
        return -YOUSJ_EFAULT;
    st = (struct stat_k *)(uintptr_t)stat_ptr;
    {
        uint8_t *p = (uint8_t *)st;
        uint64_t i;
        for (i = 0; i < sizeof(*st); i++)
            p[i] = 0;
    }
    st->ino = info->ino;
    st->nlink = ((info->mode & 0170000u) == 0040000u) ? 2 : 1;
    st->mode = info->mode;
    st->size = (int64_t)info->size;
    st->blksize = 4096;
    st->blocks = (int64_t)((info->size + 511) / 512);
    /* M26: the fs tracks the three independently where it
     * can (ramfs); single-stamp backends alias in fs.c. */
    st->atime = (int64_t)info->atime;
    st->mtime = (int64_t)info->mtime;
    st->ctime = (int64_t)info->ctime;
    return 0;
}

static long stat_abs_path(const char *abs, uint64_t stat_ptr,
                          bool follow)
{
    struct yousj_stat_info info;
    if (!fs_path_stat2(abs, follow, &info))
        return -YOUSJ_ENOENT;
    return fill_stat(stat_ptr, &info);
}

static long sys_stat_path(uint64_t path_ptr, uint64_t stat_ptr,
                          bool follow)
{
    struct task *me = sched_current();
    char path_buf[256];
    static char resolved[256];
    if (copy_user_string(path_ptr, path_buf,
                         sizeof(path_buf)) != 0)
        return -YOUSJ_EFAULT;
    if (path_buf[0] == '\0')
        return -YOUSJ_ENOENT;
    resolve_path(me, path_buf, resolved);
    return stat_abs_path(resolved, stat_ptr, follow);
}

static long sys_fstat(long fd, uint64_t stat_ptr)
{
    struct task *me = sched_current();
    struct yousj_file *f = fd_file_of(me, fd);
    struct yousj_stat_info info;
    if (f == NULL)
        return -YOUSJ_EBADF;
    if (!fs_file_stat(f, &info))
        return -YOUSJ_EBADF;
    return fill_stat(stat_ptr, &info);
}

#define AT_FDCWD_K        (-100)
#define AT_SYMLINK_NOFOLLOW_K 0x100
struct timeval_k {
    int64_t sec;
    int64_t usec;
};
struct timespec_k {
    int64_t sec;
    int64_t nsec;
};
#define AT_EMPTY_PATH_K   0x1000

static long sys_newfstatat(long dirfd, uint64_t path_ptr,
                           uint64_t stat_ptr, long flags)
{
    if (flags & ~(AT_SYMLINK_NOFOLLOW_K | AT_EMPTY_PATH_K))
        return -YOUSJ_EINVAL;
    if (!user_range_ok(path_ptr, 1))
        return -YOUSJ_EFAULT;
    if (*(const char *)(uintptr_t)path_ptr == '\0') {
        /* empty path: only meaningful with AT_EMPTY_PATH,
         * where the dirfd itself is the subject */
        if (!(flags & AT_EMPTY_PATH_K))
            return -YOUSJ_ENOENT;
        if (dirfd == AT_FDCWD_K)
            return -YOUSJ_ENOENT;
        return sys_fstat(dirfd, stat_ptr);
    }
    if (dirfd != AT_FDCWD_K) {
        /* M30: a real dirfd base */
        struct task *me = sched_current();
        char path_buf[256];
        static char resolved[256];
        long rc;
        if (copy_user_string(path_ptr, path_buf,
                             sizeof(path_buf)) != 0)
            return -YOUSJ_EFAULT;
        rc = resolve_at(me, dirfd, path_buf, resolved);
        if (rc != 0)
            return rc;
        return stat_abs_path(resolved, stat_ptr,
                             !(flags & AT_SYMLINK_NOFOLLOW_K));
    }
    return sys_stat_path(path_ptr, stat_ptr,
                         !(flags & AT_SYMLINK_NOFOLLOW_K));
}

/* --- M26: utimensat / futimens ------------------------- */

#define UTIME_NOW_K  1073741823 /* (1 << 30) - 1 */
#define UTIME_OMIT_K 1073741822 /* (1 << 30) - 2 */

/* Decode one user timespec into a yousj_utime_op. tv_sec is
 * ignored for the two specials, as on Linux. Explicit stamps
 * keep second granularity (the fs stores seconds); the nsec
 * field is still range-validated. */
static long utime_op_decode(const struct timespec_k *ts,
                            struct yousj_utime_op *op)
{
    if (ts->nsec == UTIME_NOW_K) {
        op->kind = YOUSJ_UTIME_NOW;
        op->sec = 0;
        return 0;
    }
    if (ts->nsec == UTIME_OMIT_K) {
        op->kind = YOUSJ_UTIME_OMIT;
        op->sec = 0;
        return 0;
    }
    if (ts->nsec < 0 || ts->nsec > 999999999 || ts->sec < 0)
        return -YOUSJ_EINVAL;
    op->kind = YOUSJ_UTIME_SET;
    op->sec = ts->sec;
    return 0;
}

static long utime_ops_decode(uint64_t times_ptr,
                             struct yousj_utime_op *aop,
                             struct yousj_utime_op *mop)
{
    const struct timespec_k *ts;
    long rc;
    if (times_ptr == 0) {
        aop->kind = YOUSJ_UTIME_NOW;
        aop->sec = 0;
        mop->kind = YOUSJ_UTIME_NOW;
        mop->sec = 0;
        return 0;
    }
    if (!user_range_ok(times_ptr, 2 * sizeof(struct timespec_k)))
        return -YOUSJ_EFAULT;
    ts = (const struct timespec_k *)(uintptr_t)times_ptr;
    rc = utime_op_decode(&ts[0], aop);
    if (rc != 0)
        return rc;
    return utime_op_decode(&ts[1], mop);
}

static long sys_futimens(long fd, uint64_t times_ptr)
{
    struct task *me = sched_current();
    struct yousj_file *f = fd_file_of(me, fd);
    struct yousj_utime_op aop, mop;
    long rc;
    if (f == NULL)
        return -YOUSJ_EBADF;
    rc = utime_ops_decode(times_ptr, &aop, &mop);
    if (rc != 0)
        return rc;
    return fs_utimens_file(f, &aop, &mop);
}

static long sys_utimensat(long dirfd, uint64_t path_ptr,
                          uint64_t times_ptr, long flags)
{
    struct task *me = sched_current();
    char path_buf[256];
    static char resolved[256];
    struct yousj_utime_op aop, mop;
    long rc;
    if (flags & ~(AT_SYMLINK_NOFOLLOW_K | AT_EMPTY_PATH_K))
        return -YOUSJ_EINVAL;
    if (!user_range_ok(path_ptr, 1))
        return -YOUSJ_EFAULT;
    if (*(const char *)(uintptr_t)path_ptr == '\0') {
        /* empty path: mirror sys_newfstatat's rule */
        if (!(flags & AT_EMPTY_PATH_K))
            return -YOUSJ_ENOENT;
        if (dirfd == AT_FDCWD_K)
            return -YOUSJ_ENOENT;
        return sys_futimens(dirfd, times_ptr);
    }
    rc = utime_ops_decode(times_ptr, &aop, &mop);
    if (rc != 0)
        return rc;
    if (copy_user_string(path_ptr, path_buf,
                         sizeof(path_buf)) != 0)
        return -YOUSJ_EFAULT;
    if (dirfd == AT_FDCWD_K) {
        resolve_path(me, path_buf, resolved);
    } else {
        /* M30: a real dirfd base */
        rc = resolve_at(me, dirfd, path_buf, resolved);
        if (rc != 0)
            return rc;
    }
    return fs_utimens_path(resolved,
                           !(flags & AT_SYMLINK_NOFOLLOW_K),
                           &aop, &mop);
}

/* --- M18: epoll ---------------------------------------- */

#define EPOLL_CLOEXEC_K 0x80000 /* == YOUSJ_O_CLOEXEC */

struct epoll_event_k {
    uint32_t events;
    uint32_t pad;
    uint64_t data;
};

static long sys_epoll_create1(long flags)
{
    struct task *me = sched_current();
    struct yousj_file *f;
    long rc, fd;
    if (flags & ~EPOLL_CLOEXEC_K)
        return -YOUSJ_EINVAL;
    rc = fs_epoll_create(&f);
    if (rc != 0)
        return rc;
    fd = fd_alloc(me, f);
    if (fd < 0) {
        file_put(f);
        return fd;
    }
    if (flags & EPOLL_CLOEXEC_K)
        me->fd_cloexec |= (uint16_t)(1u << fd);
    return fd;
}

static long sys_epoll_ctl(long epfd, long op, long fd,
                          uint64_t event_ptr)
{
    struct task *me = sched_current();
    struct yousj_file *epf = fd_file_of(me, epfd);
    struct yousj_file *target;
    struct epoll_event_k ev = {0, 0, 0};
    if (epf == NULL || epf->epoll == NULL)
        return -YOUSJ_EBADF;
    target = fd_file_of(me, fd);
    if (target == NULL)
        return -YOUSJ_EBADF;
    if (target->epoll != NULL)
        return -YOUSJ_EINVAL; /* no nested epoll sets */
    if (op != 2) { /* ADD/MOD carry an event; DEL ignores it */
        if (!user_range_ok(event_ptr, sizeof ev))
            return -YOUSJ_EFAULT;
        ev = *(const struct epoll_event_k *)(uintptr_t)event_ptr;
    }
    return fs_epoll_ctl(epf, op, (int)fd, target, ev.events,
                        ev.data);
}

static long sys_epoll_wait(long epfd, uint64_t events_ptr,
                           long maxevents, long timeout_ms)
{
    struct task *me = sched_current();
    struct yousj_file *epf = fd_file_of(me, epfd);
    uint64_t deadline = 0;
    if (epf == NULL || epf->epoll == NULL)
        return -YOUSJ_EBADF;
    if (maxevents <= 0)
        return -YOUSJ_EINVAL;
    if (!user_range_ok(events_ptr,
                       (uint64_t)maxevents * sizeof(struct epoll_event_k)))
        return -YOUSJ_EFAULT;
    if (timeout_ms > 0)
        deadline = sched_uptime_ticks() +
                   ((uint64_t)timeout_ms + 9) / 10;
    for (;;) {
        int i, n = 0, cnt = fs_epoll_count(epf);
        struct epoll_event_k *out =
            (struct epoll_event_k *)(uintptr_t)events_ptr;
        for (i = 0; i < cnt && n < maxevents; i++) {
            struct yousj_epoll_entry e;
            short rev;
            if (!fs_epoll_entry(epf, i, &e))
                break;
            rev = fd_ready_file(e.file, e.fd,
                                (short)e.events);
            if (rev != 0) {
                out[n].events = (uint32_t)rev;
                out[n].pad = 0;
                out[n].data = e.data;
                n++;
            }
        }
        if (n > 0)
            return n;
        if (timeout_ms == 0)
            return 0;
        if (timeout_ms > 0 &&
            sched_uptime_ticks() >= deadline)
            return 0;
        if (signal_deliverable_pending(me))
            return -YOUSJ_ERESTARTSYS;
        sched_sleep_ticks(2);
    }
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
    char target[264], linkpath[264], resolved[264];
    struct task *me = sched_current();
    long n;
    if (dirfd == YOUSJ_AT_FDCWD)
        return sys_symlink(target_ptr, link_ptr);
    /* M30: the dirfd bases the LINK path; the target is
     * stored verbatim, as always. */
    n = copy_user_string(target_ptr, target, sizeof target);
    if (n != 0)
        return n;
    n = copy_user_string(link_ptr, linkpath, sizeof linkpath);
    if (n != 0)
        return n;
    n = resolve_at(me, dirfd, linkpath, resolved);
    if (n != 0)
        return n;
    return fs_symlink_path(target, resolved);
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

static short fd_ready_file(struct yousj_file *f, long fd,
                           short events)
{
    short rev = 0;

    if (f == NULL)
        return POLLNVAL_K;
    if (f->epoll != NULL)
        return 0; /* epoll sets are never ready themselves */
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

static short fd_ready(struct task *me, long fd, short events)
{
    return fd_ready_file(fd_file_of(me, fd), fd, events);
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

/* M30: unlinkat with a real dirfd (flags keep the historic
 * behaviour of being ignored: no AT_REMOVEDIR yet). */
static long sys_unlinkat(long dirfd, uint64_t path_ptr)
{
    struct task *me = sched_current();
    static char path_buf[256];
    static char resolved[256];
    long rc;

    if (copy_user_string(path_ptr, path_buf, sizeof(path_buf)) != 0)
        return -YOUSJ_EFAULT;
    rc = resolve_at(me, dirfd, path_buf, resolved);
    if (rc != 0)
        return rc;
    return fs_unlink_path(resolved);
}

/* --- M33: the remaining *at family ---------------------------
 * mkdirat/readlinkat are thin resolve_at wrappers.
 * renameat2's RENAME_NOREPLACE is a pre-stat on the
 * target (lstat semantics: any existing name refuses,
 * even a dangling link) -- honest best-effort, the fs
 * layer has no atomic test-and-rename. link/linkat
 * answer -EPERM: ramfs names live inside the node
 * (one parent, one name), so hard links need a
 * directory-entry redesign, not a wrapper. */

static long sys_mkdirat(long dirfd, uint64_t path_ptr,
                        long mode)
{
    struct task *me = sched_current();
    static char pb[256], rp[256];
    long rc;
    if (copy_user_string(path_ptr, pb, sizeof pb) != 0)
        return -YOUSJ_EFAULT;
    rc = resolve_at(me, dirfd, pb, rp);
    if (rc != 0)
        return rc;
    return fs_mkdir_path(rp, mode != 0
                                 ? ((uint32_t)mode &
                                    ~me->umask & 07777u)
                                 : 0);
}

static long sys_readlinkat(long dirfd, uint64_t path_ptr,
                           uint64_t buf_ptr, uint64_t cap)
{
    char path[264], target[264];
    static char rp[264];
    struct task *me = sched_current();
    long n, rc;
    n = copy_user_string(path_ptr, path, sizeof path);
    if (n != 0)
        return n;
    if (!user_range_ok(buf_ptr, 1) || cap == 0)
        return -YOUSJ_EFAULT;
    if (cap > sizeof target)
        cap = sizeof target;
    rc = resolve_at(me, dirfd, path, rp);
    if (rc != 0)
        return rc;
    n = fs_readlink_path(rp, target, cap);
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

#define RENAME_NOREPLACE_K 1
static long sys_renameat(long odfd, uint64_t op, long ndfd,
                         uint64_t np, long flags)
{
    struct task *me = sched_current();
    static char ob[256], nb[256], ro[256], rn[256];
    long rc;
    if (flags & ~RENAME_NOREPLACE_K)
        return -YOUSJ_EINVAL;
    if (copy_user_string(op, ob, sizeof ob) != 0 ||
        copy_user_string(np, nb, sizeof nb) != 0)
        return -YOUSJ_EFAULT;
    rc = resolve_at(me, odfd, ob, ro);
    if (rc != 0)
        return rc;
    rc = resolve_at(me, ndfd, nb, rn);
    if (rc != 0)
        return rc;
    if (flags & RENAME_NOREPLACE_K) {
        struct yousj_stat_info info;
        if (fs_path_stat2(rn, false, &info))
            return -YOUSJ_EEXIST;
    }
    return fs_rename_path(ro, rn);
}

/* faccessat family + access(21). No uid model exists
 * (everyone is uid 0), so the requested R_OK/W_OK/X_OK
 * bits are checked against the file's owner permission
 * triplet literally. AT_EACCESS is accepted as a
 * no-op. AT_EMPTY_PATH with an empty path stats the
 * dirfd itself, like Linux. */
static long sys_faccessat(long dirfd, uint64_t path_ptr,
                          long mode, long flags)
{
    struct task *me = sched_current();
    static char pb[256], rp[256];
    struct yousj_stat_info info;
    long rc;
    if (mode & ~7L)
        return -YOUSJ_EINVAL;
    if (flags & ~(0x100L | 0x200L | 0x1000L))
        return -YOUSJ_EINVAL;
    if (copy_user_string(path_ptr, pb, sizeof pb) != 0)
        return -YOUSJ_EFAULT;
    if (pb[0] == '\0') {
        struct yousj_file *f;
        uint64_t i;
        if (!(flags & 0x1000L))
            return -YOUSJ_ENOENT;
        f = fd_file_of(me, dirfd);
        if (f == NULL)
            return -YOUSJ_EBADF;
        if (f->open_path[0] == '\0')
            return -YOUSJ_ENOENT;
        for (i = 0; i < sizeof rp - 1 && f->open_path[i]; i++)
            rp[i] = f->open_path[i];
        rp[i] = '\0';
    } else {
        rc = resolve_at(me, dirfd, pb, rp);
        if (rc != 0)
            return rc;
    }
    if (!fs_path_stat2(rp, !(flags & 0x100L), &info))
        return -YOUSJ_ENOENT;
    if (mode == 0)
        return 0;
    if ((mode & ~((long)((info.mode >> 6) & 7))) != 0)
        return -YOUSJ_EACCES;
    return 0;
}

/* --- M36: chmod / chown family -------------------------
 * The fs layer stores real permission bits now; chown
 * validates and succeeds (no uid model). *at forms ride
 * resolve_at like the rest of the family. */
static long sys_chmod_path(uint64_t path_ptr, long mode,
                           bool follow)
{
    struct task *me = sched_current();
    static char pb[256], rp[256];
    if (copy_user_string(path_ptr, pb, sizeof pb) != 0)
        return -YOUSJ_EFAULT;
    resolve_path(me, pb, rp);
    return fs_chmod_path(rp, (uint32_t)mode, follow);
}

static long sys_fchmodat(long dirfd, uint64_t path_ptr,
                         long mode, long flags)
{
    struct task *me = sched_current();
    static char pb[256], rp[256];
    long rc;
    if (flags & ~(0x100L | 0x1000L))
        return -YOUSJ_EINVAL;
    if (copy_user_string(path_ptr, pb, sizeof pb) != 0)
        return -YOUSJ_EFAULT;
    if (pb[0] == '\0' && (flags & 0x1000L)) {
        struct yousj_file *f = fd_file_of(me, dirfd);
        if (f == NULL)
            return -YOUSJ_EBADF;
        return fs_chmod_file(f, (uint32_t)mode);
    }
    rc = resolve_at(me, dirfd, pb, rp);
    if (rc != 0)
        return rc;
    /* chmod follows links even with AT_SYMLINK_NOFOLLOW
     * (the link itself has no changeable mode here). */
    return fs_chmod_path(rp, (uint32_t)mode, true);
}

static long sys_fchmod(long fd, long mode)
{
    struct task *me = sched_current();
    struct yousj_file *f = fd_file_of(me, fd);
    if (f == NULL)
        return -YOUSJ_EBADF;
    return fs_chmod_file(f, (uint32_t)mode);
}

static long sys_chown_path(uint64_t path_ptr, bool follow)
{
    struct task *me = sched_current();
    static char pb[256], rp[256];
    if (copy_user_string(path_ptr, pb, sizeof pb) != 0)
        return -YOUSJ_EFAULT;
    resolve_path(me, pb, rp);
    return fs_chown_path(rp, follow);
}

static long sys_fchownat(long dirfd, uint64_t path_ptr,
                         long flags)
{
    struct task *me = sched_current();
    static char pb[256], rp[256];
    long rc;
    if (flags & ~(0x100L | 0x1000L))
        return -YOUSJ_EINVAL;
    if (copy_user_string(path_ptr, pb, sizeof pb) != 0)
        return -YOUSJ_EFAULT;
    if (pb[0] == '\0' && (flags & 0x1000L)) {
        if (fd_file_of(me, dirfd) == NULL)
            return -YOUSJ_EBADF;
        return 0;
    }
    rc = resolve_at(me, dirfd, pb, rp);
    if (rc != 0)
        return rc;
    return fs_chown_path(rp, !(flags & 0x100L));
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

    if (!user_range_ok(buf, count))
        return -YOUSJ_EFAULT;
    return pipe_read_mem(f, (uint8_t *)(uintptr_t)buf, count);
}

/* M42 split: the pipe-read loop into a memory sink;
 * sendfile's sibling splice() reads through it. */
static long pipe_read_mem(struct yousj_file *f, uint8_t *dst,
                          uint64_t count)
{
    struct task *me = sched_current();
    struct yousj_pipe *p = f->pipe;

    if ((f->flags & YOUSJ_O_ACCMODE) != YOUSJ_O_RDONLY)
        return -YOUSJ_EBADF;
    if (count == 0)
        return 0;
    for (;;) {
        if (p->count > 0) {
            uint64_t n = count < p->count ? count : p->count;
            uint64_t i;
            for (i = 0; i < n; i++) {
                dst[i] = p->buf[p->head];
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

    if (!user_range_ok(buf, len))
        return -YOUSJ_EFAULT;
    return pipe_write_mem(f, (const uint8_t *)(uintptr_t)buf,
                          len);
}

/* M40 split: the pipe-write loop over a memory source.
 * sys_write_pipe validates the user range first;
 * sendfile feeds it a kernel buffer instead. */
static long pipe_write_mem(struct yousj_file *f,
                           const uint8_t *src, uint64_t len)
{
    struct task *me = sched_current();
    struct yousj_pipe *p = f->pipe;
    uint64_t written = 0;

    if ((f->flags & YOUSJ_O_ACCMODE) != YOUSJ_O_WRONLY)
        return -YOUSJ_EBADF;
    if (len == 0)
        return 0;
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
                p->buf[p->tail] = src[written + i];
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
    if (addr < me->brk_start || addr > me->brk_start + YOUSJ_BRK_MAX_GROWTH ||
        addr > YOUSJ_MMAP_BASE)
        return (long)me->brk_end; /* Linux: failure returns the old break;
                                      the heap never crosses into the
                                      mmap region (M22) */
    old_pg = (me->brk_end + YOUSJ_PAGE_SIZE - 1) / YOUSJ_PAGE_SIZE;
    new_pg = (addr + YOUSJ_PAGE_SIZE - 1) / YOUSJ_PAGE_SIZE;
    /* M22: growth is pure bookkeeping -- heap pages are
     * materialized by the fault path on first touch (see
     * the heap branch in yousj_page_fault). Shrinking drops
     * whatever pages above the new break were materialized;
     * unmap_free_page is a no-op for untouched ones. The
     * page straddling a non-aligned new break stays. */
    for (pg = new_pg; pg < old_pg; pg++)
        unmap_free_page(me->mm, pg * YOUSJ_PAGE_SIZE);
    me->brk_end = addr;
    return (long)addr;
}

#define PROT_READ_K  0x1
#define PROT_WRITE_K 0x2
#define PROT_EXEC_K  0x4
#define MAP_SHARED_K  0x01
#define MAP_PRIVATE_K 0x02
#define MAP_ANON_K    0x20
#define EACCES_K 13

static uint32_t vma_abstract_flags(struct yousj_vma *v,
                                   bool writable)
{
    uint32_t f = YOUSJ_VMM_USER;
    if (v->prot & PROT_READ_K)
        f |= YOUSJ_VMM_READ;
    if (v->prot & 0x4)
        f |= YOUSJ_VMM_EXEC;
    if (writable)
        f |= YOUSJ_VMM_WRITE;
    return f;
}

/* Write back the intersection of [start, start+len) with the
 * VMA's pages into the backing file -- M17: only pages that
 * are present AND hardware-dirty; a written-back page's
 * dirty bit is cleared so the next msync only does new work.
 * Pages are reached through the VMM (translate + HHDM),
 * never by assuming the caller's CR3. */
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
        uint64_t phys, raw, chunk = end - va;
        if (chunk > YOUSJ_PAGE_SIZE)
            chunk = YOUSJ_PAGE_SIZE;
        if (vmm_query_raw(sp, va, &phys, &raw) != 0)
            continue; /* never touched: nothing to write */
        if (!(raw & YOUSJ_PTE_DIRTY))
            continue;
        fs_pwrite_file(v->file, v->file_off + (va - v->base),
                       yousj_phys_to_virt(phys), chunk);
        vmm_protect(sp, va, YOUSJ_PAGE_SIZE,
                    vma_abstract_flags(v, true));
        vmm_flush_tlb_va(va);
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
    if (me->mmap_next == 0)
        me->mmap_next = YOUSJ_MMAP_BASE;
    base = me->mmap_next;
    npages = (len + YOUSJ_PAGE_SIZE - 1) / YOUSJ_PAGE_SIZE;
    if (base + npages * YOUSJ_PAGE_SIZE > YOUSJ_USER_LIMIT)
        return -YOUSJ_ENOMEM;
    /* M17: registration only. No page is allocated or filled
     * here; the page-fault path (yousj_page_fault) material-
     * izes pages on first touch, reading the CURRENT file
     * content at that moment. */
    if (file != NULL)
        file_get(file);
    vma->base = base;
    vma->len = npages * YOUSJ_PAGE_SIZE;
    vma->file_off = offset;
    vma->file = file;
    vma->prot = (uint32_t)prot;
    vma->shared = shared;
    vma->used = true;
    me->mmap_next = base + npages * YOUSJ_PAGE_SIZE + YOUSJ_PAGE_SIZE;
    return (long)base;
}

/* --- M21: mprotect -------------------------------------- */

static struct yousj_vma *vma_free_slot(struct task *me)
{
    int i;
    for (i = 0; i < YOUSJ_MAX_VMA; i++)
        if (!me->vmas[i].used)
            return &me->vmas[i];
    return NULL;
}

/* Re-point every MATERIALIZED page in [lo, hi) at the new
 * protection. Unmaterialized pages have no PTE to change --
 * the VMA's prot governs their eventual fault-in. A page
 * dropping to PROT_NONE keeps its frame: vmm_protect with
 * no USER bit leaves it present but supervisor-only, so
 * any user access faults (and the fault path rejects it
 * against the VMA's prot), while a later re-protect
 * restores access with the contents intact. */
static void vma_apply_prot(struct task *me, uint64_t lo,
                           uint64_t hi, uint32_t prot)
{
    uint32_t afl = 0;
    uint64_t va = lo;
    if (prot & PROT_READ_K)
        afl |= YOUSJ_VMM_READ;
    if (prot & PROT_WRITE_K)
        afl |= YOUSJ_VMM_WRITE;
    if (prot & PROT_EXEC_K)
        afl |= YOUSJ_VMM_EXEC;
    if (prot != 0)
        afl |= YOUSJ_VMM_USER;
    while (va < hi) {
        uint64_t phys, raw;
        if (vmm_query_raw(me->mm, va, &phys, &raw) == 0 &&
            (raw & YOUSJ_PTE_PRESENT)) {
            uint64_t run = va;
            while (run < hi) {
                uint64_t p2, r2;
                if (vmm_query_raw(me->mm, run, &p2, &r2) != 0 ||
                    !(r2 & YOUSJ_PTE_PRESENT))
                    break;
                run += YOUSJ_PAGE_SIZE;
            }
            vmm_protect(me->mm, va, run - va, afl);
            while (va < run) {
                vmm_flush_tlb_va(va);
                va += YOUSJ_PAGE_SIZE;
            }
        } else {
            va += YOUSJ_PAGE_SIZE;
        }
    }
}

static long sys_mprotect(uint64_t addr, uint64_t len,
                         uint64_t prot)
{
    struct task *me = sched_current();
    uint64_t end, cur;
    int i;

    if (me->mm == NULL || len == 0 ||
        (addr & (YOUSJ_PAGE_SIZE - 1)) != 0 ||
        !user_range_ok(addr, len))
        return -YOUSJ_EINVAL;
    prot &= (PROT_READ_K | PROT_WRITE_K | PROT_EXEC_K);
    len = (len + YOUSJ_PAGE_SIZE - 1) & ~(YOUSJ_PAGE_SIZE - 1ULL);
    end = addr + len;
    /* Coverage: every page of the range must belong to a
     * VMA (Linux: ENOMEM). The ELF image and stacks are not
     * VMAs here, so protecting them reports ENOMEM. */
    cur = addr;
    while (cur < end) {
        struct yousj_vma *hit = NULL;
        for (i = 0; i < YOUSJ_MAX_VMA; i++) {
            struct yousj_vma *v = &me->vmas[i];
            if (v->used && cur >= v->base &&
                cur < v->base + v->len) {
                hit = v;
                break;
            }
        }
        if (hit == NULL)
            return -YOUSJ_ENOMEM;
        cur = hit->base + hit->len;
    }
    /* Split at the edges and set the covered prot. */
    for (i = 0; i < YOUSJ_MAX_VMA; i++) {
        struct yousj_vma *v = &me->vmas[i];
        uint64_t vlo, vhi, mlo, mhi;
        if (!v->used)
            continue;
        vlo = v->base;
        vhi = v->base + v->len;
        if (addr >= vhi || end <= vlo)
            continue;
        mlo = addr > vlo ? addr : vlo;
        mhi = end < vhi ? end : vhi;
        if (mhi < vhi) {
            /* right remainder keeps the old prot */
            struct yousj_vma *r = vma_free_slot(me);
            if (r == NULL)
                return -YOUSJ_ENOMEM;
            *r = *v;
            r->base = mhi;
            r->len = vhi - mhi;
            r->file_off = v->file_off + (mhi - vlo);
            if (r->file != NULL)
                file_get(r->file);
        }
        if (mlo > vlo) {
            /* left remainder keeps the old prot */
            struct yousj_vma *l = vma_free_slot(me);
            if (l == NULL)
                return -YOUSJ_ENOMEM;
            *l = *v;
            l->len = mlo - vlo;
            if (l->file != NULL)
                file_get(l->file);
            v->file_off += (mlo - vlo);
        }
        v->base = mlo;
        v->len = mhi - mlo;
        v->prot = (uint32_t)prot;
        vma_apply_prot(me, mlo, mhi, (uint32_t)prot);
    }
    return 0;
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

/* --- M17: demand paging ----------------------------------
 * yousj_page_fault is called from pf_stub (isr.S) with the
 * fault description; it returns ONLY when the fault was
 * resolved and the access may be retried. Unresolvable
 * faults from user mode kill the task with SIGSEGV through
 * the one exit path; kernel faults keep the legacy
 * dump-and-halt. */

static struct yousj_vma *vma_find(struct task *me, uint64_t va)
{
    int i;
    for (i = 0; i < YOUSJ_MAX_VMA; i++) {
        struct yousj_vma *v = &me->vmas[i];
        if (v->used && va >= v->base && va < v->base + v->len)
            return v;
    }
    return NULL;
}

static bool fault_in_vma(struct task *me, struct yousj_vma *v,
                         uint64_t va, uint64_t err)
{
    uint64_t page = va & ~(YOUSJ_PAGE_SIZE - 1);
    bool write = (err & 0x2) != 0;
    bool prot_write = (v->prot & PROT_WRITE_K) != 0;
    uint64_t phys, raw;

    if (vmm_query_raw(me->mm, page, &phys, &raw) == 0) {
        /* Present page: only a write to a read-only PRIVATE
         * file page under PROT_WRITE resolves here. Frames
         * are never aliased (fork deep-copies; there is no
         * page cache), so copy-on-write is a permission
         * flip, never a copy. */
        if (write && !(raw & YOUSJ_PTE_WRITABLE) &&
            v->file != NULL && !v->shared && prot_write) {
            if (vmm_protect(me->mm, page, YOUSJ_PAGE_SIZE,
                            vma_abstract_flags(v, true)) != 0)
                return false;
            vmm_flush_tlb_va(page);
            return true;
        }
        return false;
    }
    /* Not present. Linux ordering: the access must be one
     * the mapping's protection allows at all. */
    if ((v->prot & (PROT_READ_K | PROT_WRITE_K)) == 0)
        return false;
    if (write && !prot_write)
        return false;
    {
        /* Anonymous and shared pages go in with their final
         * permissions; private file pages go in read-only
         * and upgrade through the COW branch above. */
        bool writable = prot_write &&
                        (v->file == NULL || v->shared);
        if (map_zero_page(me->mm, page,
                          vma_abstract_flags(v, writable)) != 0)
            return false;
        if (v->file != NULL) {
            if (vmm_translate(me->mm, page, &phys, NULL) != 0)
                return false;
            /* A short read at EOF leaves the rest zero --
             * exactly the bytes Linux exposes there. */
            fs_pread_file(v->file,
                          v->file_off + (page - v->base),
                          yousj_phys_to_virt(phys),
                          YOUSJ_PAGE_SIZE);
            if (write && !v->shared && prot_write) {
                /* First touch is a write on a private file
                 * page: the frame was just created for this
                 * task alone, so flipping it writable IS
                 * the copy-on-write. */
                if (vmm_protect(me->mm, page, YOUSJ_PAGE_SIZE,
                                vma_abstract_flags(v,
                                                   true)) != 0)
                    return false;
                vmm_flush_tlb_va(page);
            }
        }
        return true;
    }
}

void yousj_page_fault(struct yousj_pf_info *info)
{
    struct task *me = sched_current();

    if (me != NULL && me->mm != NULL) {
        struct yousj_vma *v = vma_find(me, info->cr2);
        if (v != NULL &&
            fault_in_vma(me, v, info->cr2, info->err))
            return; /* resolved: the stub retries the access */
        /* M22: the heap. Pages between brk_start and the
         * page-rounded break materialize as anonymous
         * read/write pages on first touch, like a private
         * anonymous VMA that needs no slot. */
        if (me->brk_start != 0 && info->cr2 >= me->brk_start &&
            info->cr2 < ((me->brk_end + YOUSJ_PAGE_SIZE - 1) &
                          ~(YOUSJ_PAGE_SIZE - 1ULL)) &&
            map_zero_page(me->mm,
                          info->cr2 &
                              ~(YOUSJ_PAGE_SIZE - 1ULL),
                          YOUSJ_VMM_READ | YOUSJ_VMM_WRITE |
                          YOUSJ_VMM_USER) == 0)
            return;
    }
    /* M24: a kernel-mode access to a user address that
     * demand paging could not resolve, while the running
     * syscall armed its recovery pad: unwind to the pad
     * and let the syscall return -EFAULT instead of
     * halting the machine. The ring check keeps this from
     * ever swallowing a genuine user-mode SEGV. */
    if ((info->cs & 3) == 0 && me != NULL && me->uaccess_armed &&
        info->cr2 >= YOUSJ_USER_BASE && info->cr2 < YOUSJ_USER_LIMIT) {
        me->uaccess_armed = 0;
        __builtin_longjmp((void *)me->uaccess_jb, 1);
    }
    if ((info->cs & 3) == 3 && me != NULL && me->mm != NULL) {
        serial_puts("\nYousj OS: task ");
        serial_puts(me->name);
        serial_puts(" killed by SIGSEGV at ");
        serial_put_hex64(info->cr2);
        serial_puts("\n");
        process_exit(0, YOUSJ_SIGSEGV); /* never returns */
    }
    serial_puts("\nYousj OS: EXCEPTION vector 14 error=");
    serial_put_hex64(info->err);
    serial_puts(" at RIP=");
    serial_put_hex64(info->rip);
    serial_puts(" cr2=");
    serial_put_hex64(info->cr2);
    serial_puts(" -- halting\n");
    for (;;)
        __asm__ volatile("cli; hlt");
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
        return sys_openat(YOUSJ_AT_FDCWD, a1, (long)a2,
                          (long)a3);
    case YOUSJ_SYS_openat:
        return sys_openat((long)a1, a2, (long)a3,
                          (long)a4);
    case YOUSJ_SYS_close:
        return sys_close((long)a1);
    case YOUSJ_SYS_lseek:
        return sys_lseek((long)a1, (long)a2, (long)a3);
    case YOUSJ_SYS_getdents64:
        return sys_getdents64((long)a1, a2, a3);
    case YOUSJ_SYS_mkdir:
        return sys_mkdir_sys(a1, (long)a2);
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
    case YOUSJ_SYS_stat:
        return sys_stat_path(a1, a2, true);
    case YOUSJ_SYS_lstat:
        return sys_stat_path(a1, a2, false);
    case YOUSJ_SYS_fstat:
        return sys_fstat((long)a1, a2);
    case YOUSJ_SYS_newfstatat:
        return sys_newfstatat((long)a1, a2, a3, (long)a4);
    case YOUSJ_SYS_futimens:
        return sys_futimens((long)a1, a2);
    case YOUSJ_SYS_utimensat:
        return sys_utimensat((long)a1, a2, a3, (long)a4);
    case YOUSJ_SYS_epoll_create1:
        return sys_epoll_create1((long)a1);
    case YOUSJ_SYS_epoll_ctl:
        return sys_epoll_ctl((long)a1, (long)a2, (long)a3, a4);
    case YOUSJ_SYS_epoll_wait:
        return sys_epoll_wait((long)a1, a2, (long)a3, (long)a4);
    case YOUSJ_SYS_epoll_pwait:
        return sys_epoll_wait((long)a1, a2, (long)a3, (long)a4);
    case YOUSJ_SYS_symlink:
        return sys_symlink(a1, a2);
    case YOUSJ_SYS_readlink:
        return sys_readlink(a1, a2, a3);
    case YOUSJ_SYS_symlinkat:
        return sys_symlinkat(a1, (long)a2, a3);
    case YOUSJ_SYS_unlinkat:
        return sys_unlinkat((long)a1, a2);
    case YOUSJ_SYS_mkdirat:
        return sys_mkdirat((long)a1, a2, (long)a3);
    case YOUSJ_SYS_readlinkat:
        return sys_readlinkat((long)a1, a2, a3, a4);
    case YOUSJ_SYS_renameat:
        return sys_renameat((long)a1, a2, (long)a3, a4, 0);
    case YOUSJ_SYS_renameat2:
        return sys_renameat((long)a1, a2, (long)a3, a4,
                            (long)a5);
    case YOUSJ_SYS_access:
        return sys_faccessat(YOUSJ_AT_FDCWD, a1, (long)a2, 0);
    case YOUSJ_SYS_faccessat:
        return sys_faccessat((long)a1, a2, (long)a3, 0);
    case YOUSJ_SYS_faccessat2:
        return sys_faccessat((long)a1, a2, (long)a3,
                             (long)a4);
    case YOUSJ_SYS_link:
    case YOUSJ_SYS_linkat:
        return -YOUSJ_EPERM; /* M33: no hard links (see above) */
    case YOUSJ_SYS_chmod:
        return sys_chmod_path(a1, (long)a2, true);
    case YOUSJ_SYS_fchmod:
        return sys_fchmod((long)a1, (long)a2);
    case YOUSJ_SYS_fchmodat:
        return sys_fchmodat((long)a1, a2, (long)a3,
                            (long)a4);
    case YOUSJ_SYS_chown:
        return sys_chown_path(a1, true);
    case YOUSJ_SYS_lchown:
        return sys_chown_path(a1, false);
    case YOUSJ_SYS_fchown:
        return fd_file_of(sched_current(), (long)a1) != NULL
                   ? 0
                   : -YOUSJ_EBADF;
    case YOUSJ_SYS_fchownat:
        return sys_fchownat((long)a1, a2, (long)a5);
    case YOUSJ_SYS_sendfile:
        return sys_sendfile((long)a1, (long)a2, a3, a4);
    case YOUSJ_SYS_splice:
        return sys_splice((long)a1, a2, (long)a3, a4, a5,
                          (long)ctx->a6);
    case YOUSJ_SYS_umask: {
        struct task *t = sched_current();
        long old = (long)t->umask;
        t->umask = (uint32_t)a1 & 0777u;
        return old;
    }
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
    case YOUSJ_SYS_mprotect:
        return sys_mprotect(a1, a2, a3);
    case YOUSJ_SYS_munmap:
        return sys_munmap(a1, a2);
    case YOUSJ_SYS_brk:
        return sys_brk(a1);
    case YOUSJ_SYS_time: {
        uint64_t now = yousj_now_unix();
        if (a1 != 0) {
            if (!user_range_ok(a1, 8))
                return -YOUSJ_EFAULT;
            *(uint64_t *)(uintptr_t)a1 = now;
        }
        return (long)now;
    }
    case YOUSJ_SYS_gettimeofday: {
        uint64_t sec, nsec;
        struct timeval_k *tv;
        if (a1 == 0)
            return 0;
        if (!user_range_ok(a1, sizeof(*tv)))
            return -YOUSJ_EFAULT;
        yousj_now_timespec(&sec, &nsec);
        tv = (struct timeval_k *)(uintptr_t)a1;
        tv->sec = (int64_t)sec;
        tv->usec = (int64_t)(nsec / 1000);
        return 0;
    }
    case YOUSJ_SYS_clock_gettime: {
        uint64_t sec, nsec;
        struct timespec_k *ts;
        if (a2 == 0)
            return -YOUSJ_EFAULT;
        if (!user_range_ok(a2, sizeof(*ts)))
            return -YOUSJ_EFAULT;
        if (a1 == 0) /* CLOCK_REALTIME */
            yousj_now_timespec(&sec, &nsec);
        else if (a1 == 1) /* CLOCK_MONOTONIC */
            yousj_mono_timespec(&sec, &nsec);
        else
            return -YOUSJ_EINVAL;
        ts = (struct timespec_k *)(uintptr_t)a2;
        ts->sec = (int64_t)sec;
        ts->nsec = (int64_t)nsec;
        return 0;
    }
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
        return sys_sigaltstack(a1, a2);
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
    struct task *me = sched_current();
    volatile long rc;

    /* rt_sigreturn restores a saved ring-3 context wholesale;
     * the exit gate would only corrupt it, so it bypasses the
     * gate entirely (pending signals are picked up at the next
     * syscall or timer return instead). It arms its own
     * recovery pad in signal.c. */
    if ((long)ctx->no == YOUSJ_SYS_rt_sigreturn)
        return sys_rt_sigreturn(ctx);
    if (me == NULL)
        return signal_syscall_exit(ctx, dispatch_inner(ctx));
    /* M24: the whole dispatch runs under the recovery pad.
     * Every user-memory dereference in every handler --
     * path strings, data buffers, out-params -- turns into
     * a plain -EFAULT when it faults unresolvably, instead
     * of halting the kernel. A recovered return skips the
     * signal exit gate; pending signals are picked up at
     * the next syscall or timer return. */
    me->uaccess_armed = 0;
    if (UACCESS_TRY(me))
        return -YOUSJ_EFAULT;
    rc = dispatch_inner(ctx);
    UACCESS_END(me);
    return signal_syscall_exit(ctx, rc);
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

/* Yousj OS - syscall entry setup and dispatcher (batch 0).
 * Written from scratch by Muse (coordinator), per
 * docs/SYSCALL_DESIGN.md: `syscall`/`sysret` entry only, Linux
 * x86-64 numbering and argument order, negative-errno returns,
 * -ENOSYS for anything not yet implemented (never fake success).
 *
 * Batch 0 serves exactly what the M3 ring-3 smoke needs:
 *   write(1)      fd 1/2 -> serial console
 *   getpid(39)    current task's tid
 *   exit(60) / exit_group(231)   -> sched_exit_current()
 * Everything else returns -ENOSYS until its batch lands.
 */
#include <yousj/types.h>
#include <yousj/syscall.h>
#include <yousj/sched.h>
#include <yousj/user.h>
#include <yousj/gdt.h>
#include <drivers/serial.h>

/* Per-CPU data reached via GS after swapgs (SYSCALL_DESIGN 3.3).
 * Field offsets are CONTRACT with syscall_entry.S:
 *   +0 kernel_rsp, +8 user_rsp_save, +16 current_task. */
struct cpu_local {
    uint64_t kernel_rsp;
    uint64_t user_rsp_save;
    uint64_t current_task;
};

static struct cpu_local g_cpu_local __attribute__((aligned(64)));

/* Called from sched_schedule() on every switch (weak hook, see
 * sched.c): TSS.rsp0 and cpu_local.kernel_rsp must always name the
 * CURRENT task's kernel stack, with one value from one place --
 * the task's kernel_stack_top -- per the design's update rule. */
void arch_on_task_switch(struct task *next)
{
    tss_set_rsp0(next->kernel_stack_top);
    g_cpu_local.kernel_rsp = next->kernel_stack_top;
    g_cpu_local.current_task = (uint64_t)(uintptr_t)next;
}

/* --- MSR setup ------------------------------------------------------ */

#define MSR_EFER           0xC0000080u
#define MSR_STAR           0xC0000081u
#define MSR_LSTAR          0xC0000082u
#define MSR_SFMASK         0xC0000084u
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
    /* STAR: syscall loads CS = [31:0] (= 0x08 kernel code, SS =
     * +8 kernel data); sysret loads SS = [47:32] + 8 (= 0x18 user
     * data) and CS = [47:32] + 16 (= 0x20 user code). */
    wrmsr(MSR_STAR, (0x10ULL << 32) | (0x08ULL << 0));
    wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)&syscall_entry);
    wrmsr(MSR_SFMASK, 0x700); /* IF + TF + DF masked on entry */
    wrmsr(MSR_KERNEL_GS_BASE, (uint64_t)(uintptr_t)&g_cpu_local);
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);
}

/* --- dispatcher ------------------------------------------------------- */

/* Errno values are Linux's (the ABI promises the same numbers). */
#define YOUSJ_EBADF  9
#define YOUSJ_EFAULT 14
#define YOUSJ_ENOSYS 38

static long sys_write(long fd, uint64_t buf, uint64_t len)
{
    uint64_t i;

    if (fd != 1 && fd != 2)
        return -YOUSJ_EBADF;
    /* The console is slow; a runaway length is a bug, not a
     * feature. Write at most 8 KiB per call, like a real partial
     * write -- callers loop. */
    if (len > 8192)
        len = 8192;
    /* Validate the WHOLE range against the sanctioned user window
     * before touching a byte: user pointers are never trusted
     * (SYSCALL_DESIGN 4.3). Runs on the caller's own address
     * space, so after validation the bytes are read directly. */
    if (buf < YOUSJ_USER_BASE || buf >= YOUSJ_USER_LIMIT ||
        len > YOUSJ_USER_LIMIT - buf)
        return -YOUSJ_EFAULT;
    for (i = 0; i < len; i++)
        serial_putc(((const char *)(uintptr_t)buf)[i]);
    return (long)len;
}

long yousj_syscall_dispatch(long no, long a1, long a2, long a3,
                            long a4, long a5, long a6)
{
    (void)a4;
    (void)a5;
    (void)a6;
    switch (no) {
    case YOUSJ_SYS_write:
        return sys_write(a1, (uint64_t)a2, (uint64_t)a3);
    case YOUSJ_SYS_getpid:
        return (long)sched_current()->tid;
    case YOUSJ_SYS_exit:
    case YOUSJ_SYS_exit_group:
        sched_exit_current((int)a1);
        __builtin_unreachable();
    default:
        return -YOUSJ_ENOSYS;
    }
}

/* --- user task landing -------------------------------------------------- */

void user_task_start(void *arg)
{
    struct user_start *u = (struct user_start *)arg;
    struct task *me = sched_current();

    me->user_ip = u->ip;
    me->user_sp = u->sp;
    /* kernel_rsp / TSS.rsp0 were set by arch_on_task_switch when
     * this task was scheduled in. Nothing else to prepare: the
     * address space switch also happened in sched_schedule(). */
    enter_user_mode(u->ip, u->sp);
    for (;;) {
        /* unreachable: leaving ring 3 happens via syscalls only */
    }
}

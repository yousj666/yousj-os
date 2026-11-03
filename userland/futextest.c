/* Yousj OS - M50 futex acceptance suite (userland).
 *
 *   mismatch   WAIT on a word whose value already moved
 *              returns -EAGAIN at once
 *   wake-none  WAKE with nobody waiting wakes 0
 *   bad-op     unknown op -> -ENOSYS; misaligned word
 *              -> -EINVAL; FUTEX_PRIVATE_FLAG accepted
 *   timeout    WAIT with a 120 ms timespec returns
 *              -ETIMEDOUT after roughly that long
 *   signal     a task truly blocked in WAIT (no timeout)
 *              is released by a signal: handler runs and
 *              the call returns -EINTR
 *   xwake      M51: a CLONE_VM sibling sharing this
 *              address space writes the word and WAKEs;
 *              the parent's WAIT returns 0 and sees the
 *              write -- the cross-task wake M50 deferred
 *
 * Prints "futextest: <name> PASS"/"FAIL", then
 * "FUTEXTEST DONE"; exit 0 iff all passed.
 */
#include <stdint.h>
#include <stddef.h>

#define SYS_read         0
#define SYS_write        1
#define SYS_close        3
#define SYS_nanosleep    35
#define SYS_clone        56
#define SYS_kill         62
#define SYS_wait4        61
#define SYS_gettimeofday 96
#define SYS_getppid      110
#define SYS_rt_sigaction 13
#define SYS_exit_group   231
#define SYS_futex        202

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_PRIVATE 0x80

#define SIGUSR1 10

static long sys_call1(long nr, long a1)
{
    long ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a1)
                     : "rcx", "r11", "memory");
    return ret;
}

static long sys_call3(long nr, long a1, long a2, long a3)
{
    long ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return ret;
}

static long sys_call4(long nr, long a1, long a2, long a3, long a4)
{
    long ret;
    register long r10 __asm__("r10") = a4;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a1), "S"(a2), "d"(a3),
                       "r"(r10)
                     : "rcx", "r11", "memory");
    return ret;
}

static long sys_call6(long nr, long a1, long a2, long a3,
                      long a4, long a5, long a6)
{
    long ret;
    register long r10 __asm__("r10") = a4;
    register long r8 __asm__("r8") = a5;
    register long r9 __asm__("r9") = a6;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a1), "S"(a2), "d"(a3),
                       "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return ret;
}

static void out(const char *s)
{
    long n = 0;
    while (s[n])
        n++;
    if (n > 0)
        sys_call3(SYS_write, 1, (long)(uintptr_t)s, n);
}

static int g_fails;
static void report(const char *name, int ok)
{
    out("futextest: ");
    out(name);
    out(ok ? " PASS\n" : " FAIL\n");
    if (!ok)
        g_fails++;
}

static void sleep_ms(long ms)
{
    long t[2];
    t[0] = ms / 1000;
    t[1] = (ms % 1000) * 1000000L;
    sys_call3(SYS_nanosleep, (long)(uintptr_t)t, 0, 0);
}

static void die(long code)
{
    sys_call1(SYS_exit_group, code);
    for (;;) {
    }
}

static long futex(int32_t *uaddr, long op, long val,
                  const long *timeout)
{
    return sys_call6(SYS_futex, (long)(uintptr_t)uaddr, op,
                     val, (long)(uintptr_t)timeout, 0, 0);
}

static int32_t word;

/* M51 xwake: the sibling thread's shared word, its stack,
 * and the asm clone trampoline. A CLONE_VM child resumes
 * on a FRESH stack with no C frame above it, so the clone
 * syscall itself must be issued from asm: the parent path
 * returns to C normally, the child path calls thread_main
 * as a fresh root and never returns through a frame. */
static int32_t xword;
static uint8_t cstack[8192];

/* not static, and marked used: its only caller is the
 * asm trampoline below, which the compiler cannot see */
__attribute__((used)) void thread_main(void)
{
    sleep_ms(150);
    xword = 1;
    futex(&xword, FUTEX_WAKE, 1, NULL);
    die(0);
}

extern long clone_vm_call(long flags, uint64_t stack);
__asm__(
    ".globl clone_vm_call\n"
    "clone_vm_call:\n"
    "    mov $56, %rax\n" /* clone */
    "    syscall\n"
    "    test %rax, %rax\n"
    "    jz 1f\n"
    "    ret\n" /* parent: pid in rax */
    "1:  call thread_main\n"
    "    mov $231, %rax\n" /* exit_group(0) if it returns */
    "    xor %rdi, %rdi\n"
    "    syscall\n"
    "    ud2\n");

static volatile int g_got_usr1;
static void usr1_handler(long sig, void *info, void *uctx)
{
    (void)sig;
    (void)info;
    (void)uctx;
    g_got_usr1 = 1;
}

__asm__(
    ".globl fx_restorer50\n"
    "fx_restorer50:\n"
    "    mov $15, %rax\n" /* rt_sigreturn */
    "    syscall\n"
    "    ud2\n");

struct k_sigaction {
    uint64_t handler;
    uint32_t flags;
    uint32_t reserved;
    uint64_t restorer;
    uint64_t mask;
};

static long now_ms(void)
{
    long tv[2];
    sys_call3(SYS_gettimeofday, (long)(uintptr_t)tv, 0, 0);
    return tv[0] * 1000 + tv[1] / 1000;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    long rc;

    /* mismatch: the value already moved */
    word = 1;
    rc = futex(&word, FUTEX_WAIT, 0, NULL);
    report("mismatch", rc == -11);

    /* wake-none: nobody is waiting */
    word = 0;
    rc = futex(&word, FUTEX_WAKE, 1, NULL);
    report("wake-none", rc == 0);

    /* bad-op: unknown op, misalignment, PRIVATE accepted */
    rc = futex(&word, 7, 0, NULL);
    {
        int ok = rc == -38;
        rc = sys_call6(SYS_futex,
                       (long)(uintptr_t)((char *)&word + 2),
                       FUTEX_WAIT, 0, 0, 0, 0);
        ok = ok && rc == -22;
        word = 5;
        rc = futex(&word, FUTEX_WAIT | FUTEX_PRIVATE, 0,
                   NULL);
        ok = ok && rc == -11;
        report("bad-op", ok);
    }

    /* timeout: 120 ms, returns -ETIMEDOUT, not early */
    {
        long ts[2];
        long t0, t1;
        ts[0] = 0;
        ts[1] = 120000000L;
        word = 0;
        t0 = now_ms();
        rc = futex(&word, FUTEX_WAIT, 0, ts);
        t1 = now_ms();
        report("timeout", rc == -110 && t1 - t0 >= 100 &&
               t1 - t0 <= 5000);
    }

    /* signal: a no-timeout WAIT is released by SIGUSR1 */
    {
        extern char fx_restorer50[];
        struct k_sigaction act;
        long c;
        act.handler = (uint64_t)(uintptr_t)usr1_handler;
        act.flags = 0x04000004u; /* SA_RESTORER | SA_SIGINFO,
                                  * no SA_RESTART -> -EINTR */
        act.reserved = 0;
        act.restorer = (uint64_t)(uintptr_t)fx_restorer50;
        act.mask = 0;
        if (sys_call4(SYS_rt_sigaction, SIGUSR1,
                      (long)(uintptr_t)&act, 0, 8) != 0) {
            report("signal", 0);
        } else {
            word = 0;
            g_got_usr1 = 0;
            c = sys_call4(SYS_clone, 17, 0, 0, 0);
            if (c == 0) {
                long ppid = sys_call1(SYS_getppid, 0);
                sleep_ms(200);
                sys_call3(SYS_kill, ppid, SIGUSR1, 0);
                die(0);
            }
            rc = futex(&word, FUTEX_WAIT, 0, NULL);
            {
                long st = 0;
                sys_call4(SYS_wait4, c,
                          (long)(uintptr_t)&st, 0, 0);
            }
            report("signal", rc == -4 && g_got_usr1 == 1);
        }
    }

    /* xwake: a CLONE_VM sibling wakes our WAIT for real */
    {
        uint64_t top =
            (uint64_t)(uintptr_t)(cstack + sizeof(cstack)) &
            ~15ULL;
        long c;
        xword = 0; /* materialize the page before sharing */
        c = clone_vm_call(17 | 0x100, top); /* SIGCHLD|VM */
        if (c <= 0) {
            report("xwake", 0);
        } else {
            long st = 0;
            rc = futex(&xword, FUTEX_WAIT, 0, NULL);
            sys_call4(SYS_wait4, c, (long)(uintptr_t)&st, 0,
                      0);
            report("xwake", rc == 0 && xword == 1);
        }
    }

    out("FUTEXTEST DONE\n");
    return g_fails == 0 ? 0 : 1;
}

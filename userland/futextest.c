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
 *
 * Prints "futextest: <name> PASS"/"FAIL", then
 * "FUTEXTEST DONE"; exit 0 iff all passed.
 * Cross-task WAKE needs a genuinely shared frame, which
 * arrives with shared address spaces; it is not faked
 * here.
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

    out("FUTEXTEST DONE\n");
    return g_fails == 0 ? 0 : 1;
}

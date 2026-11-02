/* Yousj OS - sigtest: the M7 signal-mechanism acceptance program.
 *
 * A freestanding static ELF on raw syscalls, like hello.c. Run
 * with no arguments it executes the whole suite and prints one
 * "SIGTEST <name> PASS" (or FAIL) line per scenario; the harness
 * (tools/qemu-signal-test.py) drives it through the shell and
 * checks the lines. Internal modes (victim-pre / victim) exist
 * for the execve handler-reset scenario.
 *
 * The signal ABI pieces below mirror kernel/include/yousj/
 * signal.h -- userland keeps its own copy, as with the syscall
 * numbers in the other userland programs.
 */
#include <stddef.h>
#include <stdint.h>

/* --- raw syscalls ------------------------------------------------------- */

static long sys_call3(long n, long a, long b, long c)
{
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return r;
}

static long sys_call4(long n, long a, long b, long c, long d)
{
    long r;
    register long r10 __asm__("r10") = d;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10)
                     : "rcx", "r11", "memory");
    return r;
}

#define SYS_rt_sigaction    13
#define SYS_rt_sigprocmask  14
#define SYS_rt_sigpending   127
#define SYS_write           1
#define SYS_nanosleep       35
#define SYS_getpid          39
#define SYS_clone           56
#define SYS_execve          59
#define SYS_wait4           61
#define SYS_kill            62
#define SYS_exit_group      231

#define SIGKILL 9
#define SIGUSR1 10
#define SIGALRM 14
#define SIGTERM 15
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19

#define SA_SIGINFO   0x00000004u
#define SA_RESTORER  0x04000000u
#define SA_RESTART   0x10000000u

#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

#define WNOHANG 1
#define EINTR_RET (-4L)

struct k_sigaction {
    uint64_t handler;
    uint32_t flags;
    uint32_t reserved;
    uint64_t restorer;
    uint64_t mask;
};

struct k_siginfo {
    int32_t signo, serrno, code, pad0;
    int32_t pid;
    uint32_t uid;
    int32_t status;
    int32_t pad1;
    uint8_t rest[128 - 32];
};

/* The restorer: the address a handler "returns" to. It must
 * issue rt_sigreturn with the stack pointer exactly as the
 * handler's return left it (== the signal frame base), so it is
 * three instructions and touches nothing. */
__asm__(".text\n"
        ".globl yousj_sig_restorer\n"
        "yousj_sig_restorer:\n"
        "    mov $15, %rax\n"
        "    syscall\n"
        "    ud2\n");
extern void yousj_sig_restorer(void);

/* --- tiny libc ---------------------------------------------------------- */

static size_t slen(const char *s)
{
    size_t n = 0;
    while (s[n] != '\0')
        n++;
    return n;
}

static void out(const char *s)
{
    sys_call3(SYS_write, 1, (long)(uintptr_t)s, (long)slen(s));
}


static void report(const char *name, int ok)
{
    out("SIGTEST ");
    out(name);
    out(ok ? " PASS\n" : " FAIL\n");
}

static void out_long(long v)
{
    char buf[24];
    int i = (int)sizeof(buf) - 1;
    unsigned long u = v < 0 ? (unsigned long)(-(v + 1)) + 1
                            : (unsigned long)v;
    buf[i] = '\0';
    if (u == 0)
        buf[--i] = '0';
    while (u > 0) {
        buf[--i] = (char)('0' + (u % 10));
        u /= 10;
    }
    if (v < 0)
        buf[--i] = '-';
    out(&buf[i]);
}

/* Like report(), but a failure carries a diagnostic number
 * (usually the raw wait4 status word). */
static void report2(const char *name, int ok, long detail)
{
    if (ok) {
        report(name, 1);
        return;
    }
    out("SIGTEST ");
    out(name);
    out(" FAIL detail=");
    out_long(detail);
    out("\n");
}

static long getpid(void)
{
    return sys_call3(SYS_getpid, 0, 0, 0);
}

static void exit_now(long code)
{
    sys_call3(SYS_exit_group, code, 0, 0);
    __builtin_unreachable();
}

static void sleep_ms(long ms)
{
    uint64_t ts[2];
    ts[0] = (uint64_t)(ms / 1000);
    ts[1] = (uint64_t)(ms % 1000) * 1000000ULL;
    sys_call3(SYS_nanosleep, (long)(uintptr_t)ts, 0, 0);
}

/* nanosleep that reports interruption: 0 = slept fully,
 * EINTR_RET = returned -EINTR. */
static long sleep_ms_eintr(long ms)
{
    uint64_t ts[2];
    ts[0] = (uint64_t)(ms / 1000);
    ts[1] = (uint64_t)(ms % 1000) * 1000000ULL;
    return sys_call3(SYS_nanosleep, (long)(uintptr_t)ts, 0, 0);
}

static long kill_pid(long pid, long sig)
{
    return sys_call3(SYS_kill, pid, sig, 0);
}

static long fork_(void)
{
    return sys_call3(SYS_clone, 17 /* SIGCHLD */, 0, 0);
}

static long wait4_(long pid, int *status, long options)
{
    return sys_call4(SYS_wait4, pid, (long)(uintptr_t)status,
                     options, 0);
}

static int w_exited(int st)   { return (st & 0x7f) == 0; }
static int w_code(int st)     { return (st >> 8) & 0xff; }
static int w_signaled(int st) { return (st & 0x7f) != 0; }
static int w_sig(int st)      { return st & 0x7f; }

static long sigaction_(long sig, void *handler, uint32_t extra_flags,
                       struct k_sigaction *old)
{
    struct k_sigaction act;
    act.handler = (uint64_t)(uintptr_t)handler;
    act.flags = extra_flags | SA_RESTORER;
    act.reserved = 0;
    act.restorer = (uint64_t)(uintptr_t)&yousj_sig_restorer;
    act.mask = 0;
    return sys_call4(SYS_rt_sigaction, sig,
                     (long)(uintptr_t)&act,
                     (long)(uintptr_t)old, 8);
}

static long sigdfl_(long sig)
{
    struct k_sigaction act;
    act.handler = 0; /* SIG_DFL */
    act.flags = 0;
    act.reserved = 0;
    act.restorer = 0;
    act.mask = 0;
    return sys_call4(SYS_rt_sigaction, sig, (long)(uintptr_t)&act, 0, 8);
}

static long sigprocmask_(long how, uint64_t *set, uint64_t *old)
{
    return sys_call4(SYS_rt_sigprocmask, how,
                     (long)(uintptr_t)set, (long)(uintptr_t)old, 8);
}

static uint64_t sigbit(long sig) { return 1ULL << (sig - 1); }

/* --- handlers ------------------------------------------------------------- */

static volatile long g_term_count;
static volatile long g_term_sig;
static volatile long g_term_from;

static void term_handler(int sig, struct k_siginfo *info, void *uc)
{
    (void)uc;
    g_term_sig = sig;
    g_term_from = info != NULL ? info->pid : -1;
    g_term_count++;
}

static volatile long g_usr1_count;
static void usr1_handler(int sig)
{
    (void)sig;
    g_usr1_count++;
}

static volatile long g_alrm_count;
static void alrm_handler(int sig)
{
    (void)sig;
    g_alrm_count++;
}

static volatile long g_chld_count;
static volatile long g_chld_pid;
static volatile long g_chld_status;
static void chld_handler(int sig, struct k_siginfo *info, void *uc)
{
    (void)sig;
    (void)uc;
    g_chld_pid = info->pid;
    g_chld_status = info->status;
    g_chld_count++;
}

/* For the exec-reset scenario: if this handler EVER runs, the
 * handler survived an execve, which is exactly the bug. */
static void fail_handler(int sig)
{
    (void)sig;
    out("SIGTEST exec-reset FAIL (handler survived exec)\n");
    exit_now(3);
}

/* For the IRQ-delivery scenario: the spinning child is stopped
 * dead by its own handler (called from the timer gate). */
static void spin_handler(int sig)
{
    (void)sig;
    exit_now(42);
}

/* --- scenarios -------------------------------------------------------------- */

static void cleanup_child(long pid)
{
    int st;
    kill_pid(pid, SIGKILL);
    wait4_(pid, &st, 0);
}

/* T1: caught signal end to end (frame, siginfo, sigreturn) and
 * register context preservation across the handler. */
static void t_handler(void)
{
    long me = getpid();
    volatile long guard = 0x5AFE5AFE;
    long before = guard;

    g_term_count = 0;
    if (sigaction_(SIGTERM, (void *)&term_handler, SA_SIGINFO,
                   NULL) != 0) {
        report("handler", 0);
        return;
    }
    kill_pid(me, SIGTERM);
    report("handler",
           g_term_count == 1 && g_term_sig == SIGTERM &&
           g_term_from == me && guard == before);
}

/* T2: block -> pending -> unblock delivers. */
static void t_mask(void)
{
    long me = getpid();
    uint64_t set = sigbit(SIGUSR1), pending = 0;
    int ok;

    g_usr1_count = 0;
    sigaction_(SIGUSR1, (void *)&usr1_handler, 0, NULL);
    sigprocmask_(SIG_BLOCK, &set, NULL);
    kill_pid(me, SIGUSR1);
    ok = g_usr1_count == 0;
    sys_call3(SYS_rt_sigpending, (long)(uintptr_t)&pending, 8, 0);
    ok = ok && (pending & sigbit(SIGUSR1)) != 0;
    sigprocmask_(SIG_UNBLOCK, &set, NULL);
    /* Delivery happens at the unblocking syscall's exit gate. */
    ok = ok && g_usr1_count == 1;
    report("mask", ok);
}

/* T3: default disposition terminates a sleeping child. */
static void t_default_term(void)
{
    long pid = fork_();
    int st = 0;
    long r;

    if (pid == 0) {
        sigdfl_(SIGTERM); /* the suite's own handler is inherited */
        sleep_ms(30000);
        exit_now(1);
    }
    sleep_ms(200);
    kill_pid(pid, SIGTERM);
    r = wait4_(pid, &st, 0);
    if (r != pid) {
        report2("default-term", 0, r);
        cleanup_child(pid);
        return;
    }
    report2("default-term", w_signaled(st) && w_sig(st) == SIGTERM, st);
}

/* T4: stop holds a queued SIGTERM until SIGCONT releases it. */
static void t_stop_cont(void)
{
    long pid = fork_();
    int st = 0;
    long r;
    int ok = 1;

    if (pid == 0) {
        sigdfl_(SIGTERM);
        sleep_ms(30000);
        exit_now(1);
    }
    sleep_ms(200);
    kill_pid(pid, SIGSTOP);
    sleep_ms(300);
    r = wait4_(pid, &st, WNOHANG);
    ok = ok && r == 0;                  /* stopped, not dead */
    kill_pid(pid, SIGTERM);
    sleep_ms(300);
    r = wait4_(pid, &st, WNOHANG);
    ok = ok && r == 0;                  /* TERM pends while stopped */
    kill_pid(pid, SIGCONT);
    r = wait4_(pid, &st, 0);
    ok = ok && r == pid && w_signaled(st) && w_sig(st) == SIGTERM;
    if (r != pid)
        cleanup_child(pid);
    report2("stop-cont", ok, st);
}

/* T5: SIGKILL works and cannot be caught or blocked. */
static void t_sigkill(void)
{
    struct k_sigaction act;
    uint64_t set = sigbit(SIGKILL) | sigbit(SIGUSR1), old = 0;
    long pid, r;
    int st = 0;
    int ok = 1;

    act.handler = (uint64_t)(uintptr_t)&usr1_handler;
    act.flags = SA_RESTORER;
    act.reserved = 0;
    act.restorer = (uint64_t)(uintptr_t)&yousj_sig_restorer;
    act.mask = 0;
    ok = ok && sys_call4(SYS_rt_sigaction, SIGKILL,
                         (long)(uintptr_t)&act, 0, 8) == -22;
    sigprocmask_(SIG_BLOCK, &set, NULL);
    sigprocmask_(SIG_BLOCK, NULL, &old); /* set NULL: read back */
    /* USR1 (blockable) stuck; SIGKILL was silently stripped. */
    ok = ok && (old & sigbit(SIGUSR1)) != 0 &&
         (old & sigbit(SIGKILL)) == 0;
    set = sigbit(SIGUSR1);
    sigprocmask_(SIG_UNBLOCK, &set, NULL);

    pid = fork_();
    if (pid == 0) {
        sleep_ms(30000);
        exit_now(1);
    }
    sleep_ms(200);
    kill_pid(pid, SIGKILL);
    r = wait4_(pid, &st, 0);
    ok = ok && r == pid && w_signaled(st) && w_sig(st) == SIGKILL;
    if (r != pid)
        cleanup_child(pid);
    report2("sigkill", ok, st);
}

/* T6: nanosleep interrupted -> -EINTR when no SA_RESTART. */
static void t_eintr(void)
{
    long pid = fork_();
    int st = 0;
    long r;

    if (pid == 0) {
        long rc;
        sigaction_(SIGALRM, (void *)&alrm_handler, 0, NULL);
        rc = sleep_ms_eintr(10000);
        exit_now(rc == EINTR_RET && g_alrm_count == 1 ? 7 : 6);
    }
    sleep_ms(300);
    kill_pid(pid, SIGALRM);
    r = wait4_(pid, &st, 0);
    if (r != pid) {
        report2("eintr", 0, r);
        cleanup_child(pid);
        return;
    }
    report2("eintr", w_exited(st) && w_code(st) == 7, st);
}

/* T7: with SA_RESTART the interrupted sleep resumes and the
 * handler still ran exactly once. */
static void t_restart(void)
{
    long pid = fork_();
    int st = 0;
    long r;

    if (pid == 0) {
        long rc;
        g_alrm_count = 0;
        sigaction_(SIGALRM, (void *)&alrm_handler, SA_RESTART, NULL);
        rc = sleep_ms_eintr(1200);
        exit_now(rc == 0 && g_alrm_count == 1 ? 8 : 9);
    }
    sleep_ms(300);
    kill_pid(pid, SIGALRM);
    r = wait4_(pid, &st, 0);
    if (r != pid) {
        report2("restart", 0, r);
        cleanup_child(pid);
        return;
    }
    report2("restart", w_exited(st) && w_code(st) == 8, st);
}

/* T8: SIGCHLD with siginfo (child pid + wait status). */
static void t_sigchld(void)
{
    long pid;
    int st = 0;
    long r;
    int i;

    g_chld_count = 0;
    sigaction_(SIGCHLD, (void *)&chld_handler, SA_SIGINFO, NULL);
    pid = fork_();
    if (pid == 0)
        exit_now(33);
    /* The SIGCHLD interrupts our polling sleep and is delivered
     * at its exit gate. */
    for (i = 0; i < 50 && g_chld_count == 0; i++)
        sleep_ms(100);
    r = wait4_(pid, &st, 0);
    report("sigchld",
           g_chld_count == 1 && g_chld_pid == pid &&
           g_chld_status == (33 << 8) &&
           r == pid && w_exited(st) && w_code(st) == 33);
}

/* T9: a pure-spinning child (never syscalls) still gets its
 * handler -- via the timer-IRQ gate. */
static void t_irq_deliver(void)
{
    long pid = fork_();
    int st = 0;
    long r;

    if (pid == 0) {
        volatile uint64_t spin = 0;
        sigaction_(SIGTERM, (void *)&spin_handler, 0, NULL);
        for (;;)
            spin++;
    }
    sleep_ms(300);
    kill_pid(pid, SIGTERM);
    r = wait4_(pid, &st, 0);
    if (r != pid) {
        report2("irq-deliver", 0, r);
        cleanup_child(pid);
        return;
    }
    report2("irq-deliver", w_exited(st) && w_code(st) == 42, st);
}

/* T10: clone inherits the blocked mask and dispositions;
 * pending signals are not inherited (the child queues its own
 * only after the unblock). */
static void t_clone_inherit(void)
{
    uint64_t set = sigbit(SIGUSR1);
    long pid;
    int st = 0;
    long r;

    g_usr1_count = 0;
    sigaction_(SIGUSR1, (void *)&usr1_handler, 0, NULL);
    sigprocmask_(SIG_BLOCK, &set, NULL);
    pid = fork_();
    if (pid == 0) {
        uint64_t uset = sigbit(SIGUSR1);
        kill_pid(getpid(), SIGUSR1);
        if (g_usr1_count != 0)
            exit_now(6); /* mask was not inherited */
        sigprocmask_(SIG_UNBLOCK, &uset, NULL);
        exit_now(g_usr1_count == 1 ? 5 : 6);
    }
    sigprocmask_(SIG_UNBLOCK, &set, NULL);
    r = wait4_(pid, &st, 0);
    if (r != pid) {
        report2("clone-inherit", 0, r);
        cleanup_child(pid);
        return;
    }
    report2("clone-inherit", w_exited(st) && w_code(st) == 5, st);
}

/* T11 driver: execve resets the caught handler (child modes
 * below do the work). */
static void t_exec_reset(void)
{
    static const char *argv_pre[3];
    long pid = fork_();
    int st = 0;
    long r;

    if (pid == 0) {
        argv_pre[0] = "sigtest";
        argv_pre[1] = "victim-pre";
        argv_pre[2] = NULL;
        sys_call3(SYS_execve, (long)(uintptr_t)"/bin/sigtest",
                  (long)(uintptr_t)argv_pre, 0);
        exit_now(4); /* exec failed */
    }
    r = wait4_(pid, &st, 0);
    if (r != pid) {
        report2("exec-reset", 0, r);
        cleanup_child(pid);
        return;
    }
    report2("exec-reset", w_signaled(st) && w_sig(st) == SIGTERM, st);
}

static int streq(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

int main(int argc, char **argv)
{
    if (argc > 1 && streq(argv[1], "victim-pre")) {
        static const char *argv_v[3];
        sigaction_(SIGTERM, (void *)&fail_handler, 0, NULL);
        argv_v[0] = "sigtest";
        argv_v[1] = "victim";
        argv_v[2] = NULL;
        sys_call3(SYS_execve, (long)(uintptr_t)"/bin/sigtest",
                  (long)(uintptr_t)argv_v, 0);
        exit_now(4);
    }
    if (argc > 1 && streq(argv[1], "victim")) {
        /* If execve kept the handler, this kill runs
         * fail_handler (exit 3). Correct behaviour: the handler
         * is gone, default SIGTERM kills us. */
        kill_pid(getpid(), SIGTERM);
        out("SIGTEST exec-reset FAIL (survived)\n");
        exit_now(4);
    }

    out("sigtest: M7 signal suite\n");
    t_handler();
    t_mask();
    t_default_term();
    t_stop_cont();
    t_sigkill();
    t_eintr();
    t_restart();
    t_sigchld();
    t_irq_deliver();
    t_clone_inherit();
    t_exec_reset();
    out("SIGTEST DONE\n");
    return 0;
}

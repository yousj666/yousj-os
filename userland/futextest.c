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

static long sys_call2(long nr, long a1, long a2)
{
    long ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a1), "S"(a2)
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

/* M52 join: the full pthread-style contract. join_word is
 * both the CHILD_SETTID target (the kernel writes the
 * child's tid there during clone) and the child's
 * set_tid_address word (the kernel zeroes it and futex-
 * wakes it at exit). ptid_word catches PARENT_SETTID.
 * The trampoline carries all five clone arguments. */
#define SYS_arch_prctl 158
#define SYS_set_tid_address 218
#define ARCH_GET_FS 0x1003

static int32_t join_word;
static int32_t ptid_word;
static int32_t join_result;
static uint64_t fs_seen;
static uint8_t tls_area[64];

__attribute__((used)) void thread_main2(void)
{
    uint64_t fs = 0;
    sys_call1(SYS_set_tid_address,
              (long)(uintptr_t)&join_word);
    sys_call3(SYS_arch_prctl, ARCH_GET_FS,
              (long)(uintptr_t)&fs, 0);
    fs_seen = fs;
    sleep_ms(150);
    join_result = 42;
    die(0);
}

/* M53 sighand: two handlers, one shared table. The parent
 * installs A; the CLONE_SIGHAND child installs B into the
 * same table; the parent's own signal must then run B. */
#define SYS_getpid 39
#define SYS_gettid 186

static volatile int flag_a;
static volatile int flag_b;
static volatile int b_ready;
static volatile long child_pid_seen;
static volatile long child_tid_seen;

static void handler_a(long sig, void *info, void *uctx)
{
    (void)sig; (void)info; (void)uctx;
    flag_a = 1;
}

static void handler_b(long sig, void *info, void *uctx)
{
    (void)sig; (void)info; (void)uctx;
    flag_b = 1;
}

/* M54 files: a CLONE_FILES child writes through the
 * parent's pipe fd, then closes the read end IN THE SHARED
 * TABLE -- the parent's matching fd must die with it. */
#define SYS_read 0
#define SYS_close 3
#define SYS_pipe 22

static volatile int pipe_rd;
static volatile int pipe_wr;
static volatile int files_done1;
static volatile int files_go2;
static volatile int files_done2;

__attribute__((used)) void thread_main4(void)
{
    char b = 'F';
    sys_call3(SYS_write, pipe_wr, (long)(uintptr_t)&b, 1);
    files_done1 = 1;
    while (!files_go2)
        sleep_ms(5);
    sys_call1(SYS_close, pipe_rd);
    files_done2 = 1;
    die(0);
}

extern long clone_files_call(long flags, uint64_t stack);
__asm__(
    ".globl clone_files_call\n"
    "clone_files_call:\n"
    "    mov $56, %rax\n" /* clone */
    "    syscall\n"
    "    test %rax, %rax\n"
    "    jz 1f\n"
    "    ret\n"
    "1:  call thread_main4\n"
    "    mov $231, %rax\n"
    "    xor %rdi, %rdi\n"
    "    syscall\n"
    "    ud2\n");

/* M55 clonefs: a CLONE_FS child chdirs and sets umask;
 * the parent's own cwd and mask must move with it. */
#define SYS_chdir 80
#define SYS_mkdir 83
#define SYS_getcwd 79
#define SYS_umask 95

static volatile int fs_done;
static char cwd_buf[64];

__attribute__((used)) void thread_main5(void)
{
    sys_call2(SYS_chdir, (long)(uintptr_t)"/fstest", 0);
    sys_call1(SYS_umask, 077);
    fs_done = 1;
    sleep_ms(300);
    die(0);
}

extern long clone_fs_call(long flags, uint64_t stack);
__asm__(
    ".globl clone_fs_call\n"
    "clone_fs_call:\n"
    "    mov $56, %rax\n" /* clone */
    "    syscall\n"
    "    test %rax, %rax\n"
    "    jz 1f\n"
    "    ret\n"
    "1:  call thread_main5\n"
    "    mov $231, %rax\n"
    "    xor %rdi, %rdi\n"
    "    syscall\n"
    "    ud2\n");

/* thread_main3 lives below, after struct k_sigaction. */

extern long clone_thread_call(long flags, uint64_t stack);
__asm__(
    ".globl clone_thread_call\n"
    "clone_thread_call:\n"
    "    mov $56, %rax\n" /* clone */
    "    syscall\n"
    "    test %rax, %rax\n"
    "    jz 1f\n"
    "    ret\n"
    "1:  call thread_main3\n"
    "    mov $231, %rax\n"
    "    xor %rdi, %rdi\n"
    "    syscall\n"
    "    ud2\n");

extern long clone_vm_call5(long flags, uint64_t stack,
                           void *ptid, void *ctid,
                           uint64_t tls);
__asm__(
    ".globl clone_vm_call5\n"
    "clone_vm_call5:\n"
    "    mov %rcx, %r10\n" /* arg4 -> child_tidptr slot */
    "    mov $56, %rax\n" /* clone */
    "    syscall\n"
    "    test %rax, %rax\n"
    "    jz 1f\n"
    "    ret\n" /* parent: pid in rax */
    "1:  call thread_main2\n"
    "    mov $231, %rax\n"
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

__attribute__((used)) void thread_main3(void)
{
    extern char fx_restorer50[];
    struct k_sigaction act;
    child_pid_seen = sys_call1(SYS_getpid, 0);
    child_tid_seen = sys_call1(SYS_gettid, 0);
    act.handler = (uint64_t)(uintptr_t)handler_b;
    act.flags = 0x04000004u; /* SA_RESTORER | SA_SIGINFO */
    act.reserved = 0;
    act.restorer = (uint64_t)(uintptr_t)fx_restorer50;
    act.mask = 0;
    sys_call4(SYS_rt_sigaction, SIGUSR1,
              (long)(uintptr_t)&act, 0, 8);
    b_ready = 1;
    sleep_ms(400);
    die(0);
}

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

    /* join: the pthread exit handshake, end to end */
    {
        uint64_t top =
            (uint64_t)(uintptr_t)(cstack + sizeof(cstack)) &
            ~15ULL;
        long c;
        int ok;
        join_word = 0;
        ptid_word = 0;
        join_result = 0;
        fs_seen = 0;
        /* SIGCHLD | VM | SETTLS | PARENT_SETTID |
         * CHILD_CLEARTID | CHILD_SETTID */
        c = clone_vm_call5(17 | 0x100 | 0x80000 | 0x100000 |
                           0x200000 | 0x1000000,
                           top, &ptid_word, &join_word,
                           (uint64_t)(uintptr_t)tls_area);
        if (c <= 0) {
            report("join", 0);
        } else {
            long st = 0;
            /* the writebacks land during clone itself */
            ok = ptid_word == (int32_t)c &&
                 join_word == (int32_t)c;
            /* pthread_join's sleep: wait on the tid word
             * until the exit path zeroes it */
            while (join_word != 0) {
                long v = join_word;
                if (futex(&join_word, FUTEX_WAIT, v, NULL) ==
                    -4)
                    break; /* interrupted: a failure here */
            }
            sys_call4(SYS_wait4, c, (long)(uintptr_t)&st, 0,
                      0);
            ok = ok && join_word == 0 && join_result == 42 &&
                 fs_seen == (uint64_t)(uintptr_t)tls_area;
            report("join", ok);
        }
    }

    /* sighand: CLONE_SIGHAND + CLONE_THREAD semantics */
    {
        extern char fx_restorer50[];
        struct k_sigaction act;
        uint64_t top =
            (uint64_t)(uintptr_t)(cstack + sizeof(cstack)) &
            ~15ULL;
        long c;
        int ok, spins;
        act.flags = 0x04000004u; /* SA_RESTORER | SA_SIGINFO */
        act.reserved = 0;
        act.restorer = (uint64_t)(uintptr_t)fx_restorer50;
        act.mask = 0;
        act.handler = (uint64_t)(uintptr_t)handler_a;
        if (sys_call4(SYS_rt_sigaction, SIGUSR1,
                      (long)(uintptr_t)&act, 0, 8) != 0) {
            report("sighand", 0);
        } else {
            flag_a = 0;
            flag_b = 0;
            b_ready = 0;
            child_pid_seen = 0;
            child_tid_seen = 0;
            /* SIGCHLD | VM | SIGHAND(0x800) | THREAD(0x10000) */
            c = clone_thread_call(17 | 0x100 | 0x800 |
                                  0x10000, top);
            if (c <= 0) {
                report("sighand", 0);
            } else {
                long st = 0;
                long mytid = sys_call1(SYS_gettid, 0);
                for (spins = 0; spins < 300 && !b_ready;
                     spins++)
                    sleep_ms(10);
                /* The child replaced the SIGUSR1 disposition
                 * in the SHARED table: our own signal must
                 * now run ITS handler, not ours. */
                sys_call3(SYS_kill, mytid, SIGUSR1, 0);
                sys_call4(SYS_wait4, c,
                          (long)(uintptr_t)&st, 0, 0);
                ok = flag_b == 1 && flag_a == 0 &&
                     child_pid_seen == mytid &&
                     child_tid_seen == c;
                report("sighand", ok);
            }
        }
    }

    /* files: CLONE_FILES shares the fd table itself */
    {
        int pfds[2];
        uint64_t top =
            (uint64_t)(uintptr_t)(cstack + sizeof(cstack)) &
            ~15ULL;
        long c;
        int ok1, ok2, spins;
        char ch = 0;
        long n;
        if (sys_call1(SYS_pipe, (long)(uintptr_t)pfds) != 0) {
            report("files", 0);
        } else {
            pipe_rd = pfds[0];
            pipe_wr = pfds[1];
            files_done1 = 0;
            files_go2 = 0;
            files_done2 = 0;
            /* SIGCHLD | VM | FILES(0x400) | THREAD(0x10000) */
            c = clone_files_call(17 | 0x100 | 0x400 | 0x10000,
                                 top);
            if (c <= 0) {
                report("files", 0);
            } else {
                long st = 0;
                for (spins = 0; spins < 300 && !files_done1;
                     spins++)
                    sleep_ms(10);
                n = sys_call3(SYS_read, pipe_rd,
                              (long)(uintptr_t)&ch, 1);
                ok1 = n == 1 && ch == 'F';
                files_go2 = 1;
                for (spins = 0; spins < 300 && !files_done2;
                     spins++)
                    sleep_ms(10);
                /* The child closed pipe_rd in the SHARED
                 * table: our own read end is gone too. A
                 * copied table would still read fine. */
                n = sys_call3(SYS_read, pipe_rd,
                              (long)(uintptr_t)&ch, 1);
                ok2 = n == -9; /* EBADF */
                sys_call4(SYS_wait4, c,
                          (long)(uintptr_t)&st, 0, 0);
                sys_call1(SYS_close, pipe_wr);
                report("files", ok1 && ok2);
            }
        }
    }

    /* clonefs: CLONE_FS shares cwd + umask */
    {
        uint64_t top =
            (uint64_t)(uintptr_t)(cstack + sizeof(cstack)) &
            ~15ULL;
        long c;
        int ok, spins;
        sys_call2(SYS_mkdir, (long)(uintptr_t)"/fstest", 0755);
        fs_done = 0;
        /* SIGCHLD | VM | FS(0x200) | THREAD(0x10000) */
        c = clone_fs_call(17 | 0x100 | 0x200 | 0x10000, top);
        if (c <= 0) {
            report("clonefs", 0);
        } else {
            long st = 0;
            long old_umask;
            for (spins = 0; spins < 300 && !fs_done; spins++)
                sleep_ms(10);
            cwd_buf[0] = '\0';
            sys_call2(SYS_getcwd, (long)(uintptr_t)cwd_buf,
                      sizeof(cwd_buf));
            /* The child's chdir moved OUR cwd too, and
             * its umask(077) is the mask we now hold. */
            old_umask = sys_call1(SYS_umask, 0);
            sys_call1(SYS_umask, 0022);
            sys_call2(SYS_chdir, (long)(uintptr_t)"/", 0);
            ok = cwd_buf[0] == '/' && cwd_buf[1] == 'f' &&
                 cwd_buf[2] == 's' && old_umask == 077;
            sys_call4(SYS_wait4, c,
                      (long)(uintptr_t)&st, 0, 0);
            report("clonefs", ok);
        }
    }

    out("FUTEXTEST DONE\n");
    return g_fails == 0 ? 0 : 1;
}

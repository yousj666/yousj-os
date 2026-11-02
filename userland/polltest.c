/* Yousj OS - polltest (M15): poll/select acceptance suite.
 *
 * Prints "polltest: NAME PASS"/"polltest: NAME FAIL", then
 * "POLLTEST DONE"; exit 0 iff all passed.
 */
#include <stdint.h>
#include <stddef.h>

#define SYS_read    0
#define SYS_write   1
#define SYS_open    2
#define SYS_close   3
#define SYS_poll    7
#define SYS_pipe    22
#define SYS_select  23
#define SYS_clone   56
#define SYS_wait4   61
#define SYS_kill    62
#define SYS_rt_sigaction 13
#define SYS_nanosleep 35
#define SYS_ppoll   271
#define SYS_exit_group 231

#define O_RDONLY 0
#define O_NONBLOCK 0x800
#define F_SETFL 4
#define SYS_fcntl 72

#define POLLIN  0x0001
#define POLLOUT 0x0004
#define POLLERR 0x0008
#define POLLHUP 0x0010

#define SIGUSR1 10
#define CLONE_VM_ONLY_FLAGS 0 /* plain fork semantics here */

struct pollfd {
    int fd;
    short events;
    short revents;
};

/* Layout must match the kernel's struct yousj_sigaction
 * EXACTLY: flags and reserved are 32-bit fields. Getting
 * this wrong shifts restorer/mask and the kernel records
 * restorer=0 -- the handler then cannot be delivered and
 * the signal takes its default action. */
struct k_sigaction {
    uint64_t handler;
    uint32_t flags;
    uint32_t reserved;
    uint64_t restorer;
    uint64_t mask;
};

#define SA_RESTORER 0x04000000u

__asm__(".text\n"
        ".globl yousj_sig_restorer\n"
        "yousj_sig_restorer:\n"
        "    mov $15, %rax\n"
        "    syscall\n");
extern void yousj_sig_restorer(void);

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

static long sys_call5(long nr, long a1, long a2, long a3,
                      long a4, long a5)
{
    long ret;
    register long r10 __asm__("r10") = a4;
    register long r8 __asm__("r8") = a5;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a1), "S"(a2), "d"(a3),
                       "r"(r10), "r"(r8)
                     : "rcx", "r11", "memory");
    return ret;
}

static unsigned long str_len_(const char *s)
{
    unsigned long n = 0;
    while (s[n])
        n++;
    return n;
}

static void out(const char *s)
{
    sys_call3(SYS_write, 1, (long)(uintptr_t)s, (long)str_len_(s));
}

static int g_fails;

static void report(const char *name, int ok)
{
    out("polltest: ");
    out(name);
    out(ok ? " PASS\n" : " FAIL\n");
    if (!ok)
        g_fails++;
}

static void sleep_ms(long ms)
{
    uint64_t ts[2];
    ts[0] = (uint64_t)(ms / 1000);
    ts[1] = (uint64_t)(ms % 1000) * 1000000ULL;
    sys_call3(SYS_nanosleep, (long)(uintptr_t)ts, 0, 0);
}

static void out_long(long v)
{
    char tmp[24];
    int i = 0;
    if (v == 0) {
        out("0");
        return;
    }
    if (v < 0) {
        out("-");
        v = -v;
    }
    while (v > 0) {
        tmp[i++] = (char)('0' + v % 10);
        v /= 10;
    }
    while (i > 0) {
        char c[2];
        c[0] = tmp[--i];
        c[1] = 0;
        out(c);
    }
}

static void usr1_handler(long sig)
{
    (void)sig;
}

int main(void)
{
    struct pollfd pfd;
    long r;
    int fds[2];

    out("polltest: M15 poll/select suite\n");

    /* 1: a regular file is always ready; ppoll agrees */
    {
        long fd = sys_call3(SYS_open,
                            (long)(uintptr_t)"/etc/motd",
                            O_RDONLY, 0);
        uint64_t ts[2] = {0, 0};
        int ok = 0;
        if (fd >= 0) {
            pfd.fd = (int)fd;
            pfd.events = POLLIN | POLLOUT;
            pfd.revents = 0;
            r = sys_call3(SYS_poll, (long)(uintptr_t)&pfd, 1, 0);
            ok = r == 1 && (pfd.revents & POLLIN) &&
                 (pfd.revents & POLLOUT);
            pfd.revents = 0;
            r = sys_call4(SYS_ppoll, (long)(uintptr_t)&pfd, 1,
                          (long)(uintptr_t)ts, 0);
            ok = ok && r == 1 && (pfd.revents & POLLIN);
            sys_call3(SYS_close, fd, 0, 0);
        }
        report("file-ready", ok);
    }

    /* 2: empty pipe read end times out cleanly */
    sys_call3(SYS_pipe, (long)(uintptr_t)fds, 0, 0);
    pfd.fd = fds[0];
    pfd.events = POLLIN;
    pfd.revents = 0;
    r = sys_call3(SYS_poll, (long)(uintptr_t)&pfd, 1, 150);
    report("pipe-timeout", r == 0 && pfd.revents == 0);

    /* 3: data makes it readable; writer close raises HUP */
    sys_call3(SYS_write, fds[1], (long)(uintptr_t)"x", 1);
    pfd.revents = 0;
    r = sys_call3(SYS_poll, (long)(uintptr_t)&pfd, 1, 0);
    {
        int ok = r == 1 && (pfd.revents & POLLIN);
        char c;
        sys_call3(SYS_read, fds[0], (long)(uintptr_t)&c, 1);
        sys_call3(SYS_close, fds[1], 0, 0);
        pfd.revents = 0;
        r = sys_call3(SYS_poll, (long)(uintptr_t)&pfd, 1, 0);
        ok = ok && r == 1 && (pfd.revents & POLLHUP);
        report("pipe-data-hup", ok);
    }
    sys_call3(SYS_close, fds[0], 0, 0);

    /* 4: write end: writable, full pipe is not, readerless
     * pipe errors */
    sys_call3(SYS_pipe, (long)(uintptr_t)fds, 0, 0);
    pfd.fd = fds[1];
    pfd.events = POLLOUT;
    pfd.revents = 0;
    r = sys_call3(SYS_poll, (long)(uintptr_t)&pfd, 1, 0);
    {
        int ok = r == 1 && (pfd.revents & POLLOUT);
        static char block[4096];
        long w;
        sys_call4(SYS_fcntl, fds[1], F_SETFL, O_NONBLOCK, 0);
        w = sys_call3(SYS_write, fds[1],
                      (long)(uintptr_t)block, 4096);
        ok = ok && w == 4096;
        pfd.revents = 0;
        r = sys_call3(SYS_poll, (long)(uintptr_t)&pfd, 1, 0);
        ok = ok && r == 0;
        sys_call3(SYS_close, fds[0], 0, 0);
        pfd.revents = 0;
        r = sys_call3(SYS_poll, (long)(uintptr_t)&pfd, 1, 0);
        ok = ok && r == 1 && (pfd.revents & POLLERR);
        report("pipe-write", ok);
    }
    sys_call3(SYS_close, fds[1], 0, 0);

    /* 5: select sees the same world */
    sys_call3(SYS_pipe, (long)(uintptr_t)fds, 0, 0);
    sys_call3(SYS_write, fds[1], (long)(uintptr_t)"y", 1);
    {
        uint64_t rset[16];
        uint64_t tv[2] = {0, 0};
        int i, ok;
        for (i = 0; i < 16; i++)
            rset[i] = 0;
        rset[fds[0] / 64] |= 1ULL << (fds[0] % 64);
        r = sys_call5(SYS_select, fds[0] + 1,
                      (long)(uintptr_t)rset, 0, 0,
                      (long)(uintptr_t)tv);
        ok = r == 1 &&
             (rset[fds[0] / 64] >> (fds[0] % 64)) & 1ULL;
        report("select-basic", ok);
    }
    sys_call3(SYS_close, fds[0], 0, 0);
    sys_call3(SYS_close, fds[1], 0, 0);

    /* 6: a signal interrupts a long poll with EINTR. Two
     * pipes: the child polls the quiet one and handshakes on
     * the other (writing into the polled pipe would make it
     * readable and defeat the test). */
    sys_call3(SYS_pipe, (long)(uintptr_t)fds, 0, 0);
    {
        int ctl[2];
        sys_call3(SYS_pipe, (long)(uintptr_t)ctl, 0, 0);
        long pid = sys_call3(SYS_clone, 17 /* SIGCHLD */, 0, 0);
        if (pid == 0) {
            struct k_sigaction act;
            struct pollfd cpfd;
            long pr;
            act.handler = (uint64_t)(uintptr_t)usr1_handler;
            act.flags = SA_RESTORER; /* no SA_RESTART: EINTR */
            act.reserved = 0;
            act.restorer = (uint64_t)(uintptr_t)yousj_sig_restorer;
            act.mask = 0;
            {
                long sr = sys_call4(SYS_rt_sigaction, SIGUSR1,
                                    (long)(uintptr_t)&act, 0, 8);
                char mark = sr == 0 ? 'R' : 'F';
                sys_call3(SYS_write, ctl[1],
                          (long)(uintptr_t)&mark, 1);
            }
            cpfd.fd = fds[0];
            cpfd.events = POLLIN;
            cpfd.revents = 0;
            pr = sys_call3(SYS_poll, (long)(uintptr_t)&cpfd, 1,
                           8000);
            sys_call3(SYS_exit_group,
                      pr == -4 ? 42 : pr == 0 ? 44 : pr > 0 ? 45 : 46,
                      0, 0);
            for (;;)
                ;
        }
        {
            int status = 0;
            char mark = 0;
            /* wait until the child says its handler is in */
            sys_call3(SYS_read, ctl[0], (long)(uintptr_t)&mark, 1);
            out("polltest: eintr child sigaction ");
            out(mark == 'R' ? "ok\n" : "FAILED\n");
            sleep_ms(100);
            sys_call3(SYS_kill, pid, SIGUSR1, 0);
            sys_call4(SYS_wait4, pid, (long)(uintptr_t)&status,
                      0, 0);
            report("eintr",
                   ((status >> 8) & 0xFF) == 42);
        }
    }
    sys_call3(SYS_close, fds[0], 0, 0);
    sys_call3(SYS_close, fds[1], 0, 0);

    out("POLLTEST DONE\n");
    return g_fails == 0 ? 0 : 1;
}

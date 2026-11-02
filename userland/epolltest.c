/* epolltest - M18 acceptance scenarios for epoll.
 *
 * basic, timeout, two-fds, mod-del, hup, eintr.
 * Prints "epolltest: NAME PASS/FAIL" per scenario and
 * "EPOLLTEST DONE" at the end; exit 0 iff all pass.
 * Freestanding, Linux ABI. */
#include <stdint.h>

#define SYS_read         0
#define SYS_write        1
#define SYS_close        3
#define SYS_pipe         22
#define SYS_clone        56
#define SYS_wait4        61
#define SYS_kill         62
#define SYS_nanosleep    35
#define SYS_rt_sigaction 13
#define SYS_exit_group   231
#define SYS_epoll_wait   232
#define SYS_epoll_ctl    233
#define SYS_epoll_create1 291

#define EPOLLIN  0x001
#define EPOLLOUT 0x004
#define EPOLLHUP 0x010
#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

#define SIGUSR1 10

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
    out("epolltest: ");
    out(name);
    out(ok ? " PASS\n" : " FAIL\n");
    if (!ok)
        g_fails++;
}

struct epoll_event {
    uint32_t events;
    uint32_t pad;
    uint64_t data;
};

static long ep_ctl(long ep, long op, long fd, uint32_t events,
                   uint64_t data)
{
    struct epoll_event ev;
    ev.events = events;
    ev.pad = 0;
    ev.data = data;
    return sys_call4(SYS_epoll_ctl, ep, op, fd,
                     (long)(uintptr_t)&ev);
}

static void sleep_ms(long ms)
{
    long ts[2];
    ts[0] = ms / 1000;
    ts[1] = (ms % 1000) * 1000000L;
    sys_call3(SYS_nanosleep, (long)(uintptr_t)ts, 0, 0);
}

/* Signal plumbing for the eintr scenario (layout must match
 * the kernel's struct yousj_sigaction exactly: flags and
 * reserved are 32-bit fields). */
struct k_sigaction {
    uint64_t handler;
    uint32_t flags;
    uint32_t reserved;
    uint64_t restorer;
    uint64_t mask;
};

#define SA_RESTORER 0x04000000u

__asm__(
    ".globl yousj_sig_restorer\n"
    "yousj_sig_restorer:\n"
    "    mov $15, %rax\n"
    "    syscall\n");

extern char yousj_sig_restorer[];

static void usr1_handler(long sig)
{
    (void)sig;
}

int main(void)
{
    int fds[2];
    long ep;

    out("epolltest: M18 epoll suite\n");
    sys_call3(SYS_pipe, (long)(uintptr_t)fds, 0, 0);
    ep = sys_call3(SYS_epoll_create1, 0, 0, 0);

    /* 1: basic level-triggered readiness + data round-trip */
    {
        struct epoll_event out_ev[4];
        long n0, n1;
        ep_ctl(ep, EPOLL_CTL_ADD, fds[0], EPOLLIN, 0xAA);
        n0 = sys_call4(SYS_epoll_wait, ep,
                       (long)(uintptr_t)out_ev, 4, 0);
        sys_call3(SYS_write, fds[1], (long)(uintptr_t)"x", 1);
        n1 = sys_call4(SYS_epoll_wait, ep,
                       (long)(uintptr_t)out_ev, 4, 1000);
        report("basic", ep >= 0 && n0 == 0 && n1 == 1 &&
               (out_ev[0].events & EPOLLIN) != 0 &&
               out_ev[0].data == 0xAA);
        /* drain */
        {
            char c;
            sys_call3(SYS_read, fds[0], (long)(uintptr_t)&c, 1);
        }
    }

    /* 2: timeout on an empty pipe */
    {
        struct epoll_event out_ev[2];
        long n = sys_call4(SYS_epoll_wait, ep,
                           (long)(uintptr_t)out_ev, 2, 120);
        report("timeout", n == 0);
    }

    /* 3: two watched fds; only the written one reports */
    {
        int f2[2];
        struct epoll_event out_ev[4];
        long n;
        sys_call3(SYS_pipe, (long)(uintptr_t)f2, 0, 0);
        ep_ctl(ep, EPOLL_CTL_ADD, f2[0], EPOLLIN, 2);
        sys_call3(SYS_write, f2[1], (long)(uintptr_t)"y", 1);
        n = sys_call4(SYS_epoll_wait, ep,
                      (long)(uintptr_t)out_ev, 4, 1000);
        report("two-fds", n == 1 && out_ev[0].data == 2);
        ep_ctl(ep, EPOLL_CTL_DEL, f2[0], 0, 0);
        sys_call3(SYS_close, f2[0], 0, 0);
        sys_call3(SYS_close, f2[1], 0, 0);
    }

    /* 4: DEL silences an interest; MOD replaces the data */
    {
        struct epoll_event out_ev[2];
        long n1, n2;
        ep_ctl(ep, EPOLL_CTL_DEL, fds[0], 0, 0);
        sys_call3(SYS_write, fds[1], (long)(uintptr_t)"z", 1);
        n1 = sys_call4(SYS_epoll_wait, ep,
                       (long)(uintptr_t)out_ev, 2, 0);
        ep_ctl(ep, EPOLL_CTL_ADD, fds[0], EPOLLIN, 7);
        ep_ctl(ep, EPOLL_CTL_MOD, fds[0], EPOLLIN, 9);
        n2 = sys_call4(SYS_epoll_wait, ep,
                       (long)(uintptr_t)out_ev, 2, 1000);
        report("mod-del", n1 == 0 && n2 == 1 &&
               out_ev[0].data == 9);
        {
            char c;
            sys_call3(SYS_read, fds[0], (long)(uintptr_t)&c, 1);
        }
    }

    /* 5: hangup surfaces as EPOLLHUP */
    {
        int f3[2];
        struct epoll_event out_ev[2];
        long n;
        sys_call3(SYS_pipe, (long)(uintptr_t)f3, 0, 0);
        ep_ctl(ep, EPOLL_CTL_ADD, f3[0], EPOLLIN, 5);
        sys_call3(SYS_close, f3[1], 0, 0);
        n = sys_call4(SYS_epoll_wait, ep,
                      (long)(uintptr_t)out_ev, 2, 1000);
        report("hup", n == 1 &&
               (out_ev[0].events & EPOLLHUP) != 0);
        ep_ctl(ep, EPOLL_CTL_DEL, f3[0], 0, 0);
        sys_call3(SYS_close, f3[0], 0, 0);
    }

    /* 6: a signal interrupts a long wait with EINTR. Two
     * pipes again: the child waits on the quiet one and
     * handshakes on the other. */
    {
        int quiet[2], ctl[2];
        sys_call3(SYS_pipe, (long)(uintptr_t)quiet, 0, 0);
        sys_call3(SYS_pipe, (long)(uintptr_t)ctl, 0, 0);
        {
            long qep = sys_call3(SYS_epoll_create1, 0, 0, 0);
            long pid;
            ep_ctl(qep, EPOLL_CTL_ADD, quiet[0], EPOLLIN, 1);
            pid = sys_call3(SYS_clone, 17 /* SIGCHLD */, 0, 0);
            if (pid == 0) {
                struct k_sigaction act;
                struct epoll_event out_ev[2];
                long r;
                char mark;
                act.handler = (uint64_t)(uintptr_t)usr1_handler;
                act.flags = SA_RESTORER; /* no SA_RESTART */
                act.reserved = 0;
                act.restorer =
                    (uint64_t)(uintptr_t)yousj_sig_restorer;
                act.mask = 0;
                r = sys_call4(SYS_rt_sigaction, SIGUSR1,
                              (long)(uintptr_t)&act, 0, 8);
                mark = r == 0 ? 'R' : 'F';
                sys_call3(SYS_write, ctl[1],
                          (long)(uintptr_t)&mark, 1);
                r = sys_call4(SYS_epoll_wait, qep,
                              (long)(uintptr_t)out_ev, 2, 8000);
                sys_call3(SYS_exit_group, r == -4 ? 42 : 43,
                          0, 0);
            }
            {
                int status = 0;
                char mark = 0;
                sys_call3(SYS_read, ctl[0],
                          (long)(uintptr_t)&mark, 1);
                sleep_ms(100);
                sys_call3(SYS_kill, pid, SIGUSR1, 0);
                sys_call3(SYS_wait4, pid,
                          (long)(uintptr_t)&status, 0);
                report("eintr", mark == 'R' &&
                       ((status >> 8) & 0xFF) == 42);
            }
            sys_call3(SYS_close, qep, 0, 0);
        }
        sys_call3(SYS_close, quiet[0], 0, 0);
        sys_call3(SYS_close, quiet[1], 0, 0);
        sys_call3(SYS_close, ctl[0], 0, 0);
        sys_call3(SYS_close, ctl[1], 0, 0);
    }

    out("EPOLLTEST DONE\n");
    return g_fails == 0 ? 0 : 1;
}

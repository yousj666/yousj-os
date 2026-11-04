/* Yousj OS - M87 PTY acceptance suite.
 *
 *   create    open /dev/ptmx, pty_slave gives a distinct fd;
 *             a second pty_slave on the same master fails
 *   flow      bytes cross both ways unchanged
 *   poll      readiness follows the data (slave readable
 *             after a master write, idle ends time out idle)
 *   block     a forked reader blocks on the slave until the
 *             parent writes through the master
 *   eof       closing the master drains then EOFs the slave;
 *             writing the orphaned slave fails
 *   pairs     two pairs are independent
 *   notpty    pty_slave on a non-PTY fd fails; on a bad fd
 *             fails EBADF
 *
 * Prints "PTYTEST <name> PASS"/"PTYTEST <name> FAIL
 * detail=", then "PTYTEST DONE"; exit 0 iff all passed.
 */
#include <stdint.h>
#include <stddef.h>

#define SYS_read      0
#define SYS_write     1
#define SYS_open      2
#define SYS_close     3
#define SYS_poll      7
#define SYS_rt_sigaction 13
#define SYS_clone     56
#define SYS_wait4     61
#define SYS_nanosleep 35
#define SYS_exit      60
#define SYS_exit_group 231
#define SYS_pty_slave 0x1005

#define O_RDWR 2
#define CLONE_FLAGS_PARENT_SETTID 0x00100000

#define POLLIN 0x1

static long sys1(long n, long a)
{
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a)
                     : "rcx", "r11", "memory");
    return r;
}
static long sys2(long n, long a, long b)
{
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b)
                     : "rcx", "r11", "memory");
    return r;
}
static long sys3(long n, long a, long b, long c)
{
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return r;
}
static long sys4(long n, long a, long b, long c, long d)
{
    long r;
    register long r10 __asm__("r10") = d;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10)
                     : "rcx", "r11", "memory");
    return r;
}

static uint64_t slen(const char *s)
{
    uint64_t n = 0;
    while (s[n])
        n++;
    return n;
}
static void emit(const char *s)
{
    sys3(SYS_write, 1, (long)s, (long)slen(s));
}
static int seq(const char *a, const char *b, uint64_t n)
{
    uint64_t i;
    for (i = 0; i < n; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

static int g_fail;
static void verdict(const char *name, int ok, long detail)
{
    if (!ok)
        g_fail = 1;
    emit("PTYTEST ");
    emit(name);
    emit(ok ? " PASS" : " FAIL");
    if (!ok) {
        char num[24];
        int i = 0;
        uint64_t v;
        emit(" detail=");
        if (detail < 0) {
            emit("-");
            v = (uint64_t)(-detail);
        } else {
            v = (uint64_t)detail;
        }
        do {
            num[i++] = (char)('0' + (v % 10));
            v /= 10;
        } while (v);
        while (i > 0) {
            char c[2] = { num[--i], 0 };
            emit(c);
        }
    }
    emit("\n");
}

struct pollfd {
    int fd;
    short events;
    short revents;
};

struct k_sigaction {
    uint64_t handler;
    uint32_t flags;
    uint32_t reserved;
    uint64_t restorer;
    uint64_t mask;
};

/* The orphan-write check expects EPIPE back, not death. */
static void ignore_sigpipe(void)
{
    struct k_sigaction act;
    act.handler = 1; /* SIG_IGN */
    act.flags = 0;
    act.reserved = 0;
    act.restorer = 0;
    act.mask = 0;
    sys4(SYS_rt_sigaction, 13 /* SIGPIPE */, (long)&act, 0, 8);
}

static void msleep(long ms)
{
    struct {
        int64_t sec;
        int64_t nsec;
    } ts;
    ts.sec = ms / 1000;
    ts.nsec = (ms % 1000) * 1000000;
    sys2(SYS_nanosleep, (long)&ts, 0);
}

void main(void)
{
    char buf[64];
    long m, s, rc;

    ignore_sigpipe();

    /* create */
    m = sys3(SYS_open, (long)"/dev/ptmx", O_RDWR, 0);
    s = m >= 0 ? sys1(SYS_pty_slave, m) : -1;
    verdict("create", m >= 0 && s >= 0 && s != m,
            m < 0 ? m : s);
    rc = s >= 0 ? sys1(SYS_pty_slave, m) : -999;
    verdict("create-once", rc == -22 /* EINVAL */, rc);

    /* flow, both ways */
    rc = sys3(SYS_write, m, (long)"hello", 5);
    rc = sys3(SYS_read, s, (long)buf, 5);
    verdict("flow-m2s", rc == 5 && seq(buf, "hello", 5), rc);
    rc = sys3(SYS_write, s, (long)"world!", 6);
    rc = sys3(SYS_read, m, (long)buf, 6);
    verdict("flow-s2m", rc == 6 && seq(buf, "world!", 6), rc);

    /* poll readiness */
    {
        struct pollfd p;
        p.fd = (int)s;
        p.events = POLLIN;
        p.revents = 0;
        rc = sys3(SYS_poll, (long)&p, 1, 50);
        verdict("poll-idle", rc == 0, rc);
        sys3(SYS_write, m, (long)"x", 1);
        p.revents = 0;
        rc = sys3(SYS_poll, (long)&p, 1, 1000);
        verdict("poll-ready",
                rc == 1 && (p.revents & POLLIN) != 0, rc);
        sys3(SYS_read, s, (long)buf, 1); /* drain */
    }

    /* a second, independent pair */
    {
        long m2 = sys3(SYS_open, (long)"/dev/ptmx", O_RDWR, 0);
        long s2 = m2 >= 0 ? sys1(SYS_pty_slave, m2) : -1;
        int ok = m2 >= 0 && s2 >= 0;
        if (ok) {
            sys3(SYS_write, m2, (long)"two", 3);
            rc = sys3(SYS_read, s2, (long)buf, 3);
            ok = rc == 3 && seq(buf, "two", 3);
            /* nothing leaked into pair one */
            struct pollfd p;
            p.fd = (int)s;
            p.events = POLLIN;
            p.revents = 0;
            ok = ok && sys3(SYS_poll, (long)&p, 1, 0) == 0;
        }
        verdict("pairs", ok, m2 < 0 ? m2 : s2);
        if (s2 >= 0)
            sys1(SYS_close, s2);
        if (m2 >= 0)
            sys1(SYS_close, m2);
    }

    /* blocking read across fork: the child waits on the
     * slave; the parent feeds it through the master. */
    {
        long pid = sys3(SYS_clone, 17 /* SIGCHLD */, 0, 0);
        if (pid == 0) {
            char cb[4];
            long n = sys3(SYS_read, s, (long)cb, 4);
            sys1(SYS_exit,
                 n == 4 && seq(cb, "ping", 4) ? 42 : 43);
        }
        msleep(150);
        sys3(SYS_write, m, (long)"ping", 4);
        {
            int status = 0;
            long w = sys4(SYS_wait4, pid, (long)&status, 0, 0);
            verdict("block", w == pid &&
                    ((status >> 8) & 0xff) == 42,
                    (status >> 8) & 0xff);
        }
    }

    /* EOF: close the master, the slave drains then EOFs */
    sys3(SYS_write, m, (long)"tail", 4);
    sys1(SYS_close, m);
    rc = sys3(SYS_read, s, (long)buf, 4);
    {
        long rc2 = sys3(SYS_read, s, (long)buf, 4);
        verdict("eof", rc == 4 && seq(buf, "tail", 4) &&
                rc2 == 0, rc2);
    }
    rc = sys3(SYS_write, s, (long)"z", 1);
    verdict("orphan-write", rc < 0, rc);
    sys1(SYS_close, s);

    /* pty_slave rejects non-PTY and bad fds */
    {
        long f = sys3(SYS_open, (long)"/dev/null", 0, 0);
        rc = f >= 0 ? sys1(SYS_pty_slave, f) : -999;
        verdict("notpty", rc == -22, rc);
        if (f >= 0)
            sys1(SYS_close, f);
        rc = sys1(SYS_pty_slave, 9999);
        verdict("badfd", rc == -9 /* EBADF */, rc);
    }

    emit("PTYTEST DONE\n");
    sys1(SYS_exit_group, g_fail ? 1 : 0);
}

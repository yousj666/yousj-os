/* Yousj OS - M68 named AF_UNIX socket acceptance suite.
 *
 *   reject    bad family/type/protocol, ENOTSOCK on a plain
 *             file, ENOTCONN reads, listen before bind
 *   bindstat  bind creates a S_IFSOCK node; rebinding the same
 *             path fails EADDRINUSE; the node cannot be opened
 *             (ENXIO); fstat on the endpoint reports S_IFSOCK
 *   refused   connect with nobody listening (bound only, a
 *             plain file, a missing path)
 *   e2e       listen/connect/accept in one task, data both
 *             ways, poll readiness on the listener, accept's
 *             peer-address writeback
 *   queue     the backlog fills: a third connect gets EAGAIN
 *             until an accept frees a slot
 *   nbaccept  a nonblocking listener with an empty queue
 *             fails accept with EAGAIN
 *   close     peer close gives EOF one way and EPIPE the
 *             other; a dead listener refuses through its
 *             leftover node; unlink removes the path
 *   fork      the classic shape: forked child connects and
 *             writes, parent accepts and reads
 *
 * Prints "SOCKTEST <name> PASS"/"SOCKTEST <name> FAIL
 * detail=", then "SOCKTEST DONE"; exit 0 iff all passed.
 */
#include <stdint.h>
#include <stddef.h>

#define SYS_read         0
#define SYS_write        1
#define SYS_open         2
#define SYS_close        3
#define SYS_stat         4
#define SYS_fstat        5
#define SYS_poll         7
#define SYS_rt_sigaction 13
#define SYS_socket       41
#define SYS_connect      42
#define SYS_accept       43
#define SYS_bind         49
#define SYS_listen       50
#define SYS_clone        56
#define SYS_wait4        61
#define SYS_unlink       87
#define SYS_exit_group   231

#define AF_UNIX      1
#define SOCK_STREAM  1
#define SOCK_NONBLOCK_K 0x800

#define O_RDONLY 0

#define SIGPIPE 13
#define SIG_IGN 1

#define S_IFMT_K   0170000u
#define S_IFSOCK_K 0140000u

struct k_sigaction {
    uint64_t handler;
    uint32_t flags;
    uint32_t reserved;
    uint64_t restorer;
    uint64_t mask;
};

struct stat_k {
    uint64_t dev, ino, nlink;
    uint32_t mode, uid, gid, pad0;
    uint64_t rdev;
    int64_t size, blksize, blocks;
    uint64_t atime, atime_nsec, mtime, mtime_nsec;
    uint64_t ctime, ctime_nsec;
    int64_t unused[3];
};

struct sockaddr_un_k {
    uint16_t family;
    char path[108];
};

struct pfd_k {
    int32_t fd;
    int16_t events, revents;
};

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
                     : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
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

static void out_long(long v)
{
    char buf[24];
    int i = 0;
    unsigned long u;
    if (v < 0) {
        out("-");
        u = (unsigned long)(-(v + 1)) + 1;
    } else {
        u = (unsigned long)v;
    }
    if (u == 0) {
        out("0");
        return;
    }
    while (u > 0) {
        buf[i++] = (char)('0' + (u % 10));
        u /= 10;
    }
    while (i > 0) {
        char c = buf[--i];
        sys_call3(SYS_write, 1, (long)(uintptr_t)&c, 1);
    }
}

static int failures = 0;

static void report2(const char *name, int ok, long detail)
{
    out("SOCKTEST ");
    out(name);
    if (ok) {
        out(" PASS\n");
    } else {
        out(" FAIL detail=");
        out_long(detail);
        out("\n");
        failures++;
    }
}

static void fill_addr(struct sockaddr_un_k *a, const char *path)
{
    int i;
    a->family = AF_UNIX;
    for (i = 0; i < 107 && path[i]; i++)
        a->path[i] = path[i];
    a->path[i] = '\0';
}

static long do_bind(long fd, const char *path)
{
    struct sockaddr_un_k a;
    fill_addr(&a, path);
    return sys_call3(SYS_bind, fd, (long)(uintptr_t)&a,
                     (long)sizeof(a));
}

static long do_connect(long fd, const char *path)
{
    struct sockaddr_un_k a;
    fill_addr(&a, path);
    return sys_call3(SYS_connect, fd, (long)(uintptr_t)&a,
                     (long)sizeof(a));
}

static void ignore_sigpipe(void)
{
    struct k_sigaction act;
    act.handler = SIG_IGN;
    act.flags = 0;
    act.reserved = 0;
    act.restorer = 0;
    act.mask = 0;
    sys_call4(SYS_rt_sigaction, SIGPIPE, (long)(uintptr_t)&act, 0, 8);
}

static void t_reject(void)
{
    int ok = 1;
    long r, fd;
    char buf[4];

    r = sys_call3(SYS_socket, 2, SOCK_STREAM, 0);
    ok = ok && r == -97; /* AF_INET: EAFNOSUPPORT */
    if (!ok) { report2("reject", 0, r); return; }
    r = sys_call3(SYS_socket, AF_UNIX, 2, 0);
    ok = ok && r == -93; /* SOCK_DGRAM: EPROTONOSUPPORT */
    r = sys_call3(SYS_socket, AF_UNIX, SOCK_STREAM, 7);
    ok = ok && r == -93; /* nonzero protocol */
    fd = sys_call3(SYS_open, (long)(uintptr_t)"/bin/hello",
                   O_RDONLY, 0);
    ok = ok && fd >= 0;
    r = do_bind(fd, "/tmp/nope.sock");
    ok = ok && r == -88; /* ENOTSOCK */
    sys_call3(SYS_close, fd, 0, 0);
    fd = sys_call3(SYS_socket, AF_UNIX, SOCK_STREAM, 0);
    ok = ok && fd >= 0;
    r = sys_call3(SYS_read, fd, (long)(uintptr_t)buf, 1);
    ok = ok && r == -107; /* ENOTCONN */
    r = sys_call3(SYS_listen, fd, 4, 0);
    ok = ok && r == -22; /* listen before bind: EINVAL */
    r = sys_call3(SYS_accept, fd, 0, 0);
    ok = ok && r == -22; /* not listening: EINVAL */
    sys_call3(SYS_close, fd, 0, 0);
    report2("reject", ok, r);
}

static void t_bindstat(void)
{
    struct stat_k st;
    int ok = 1;
    long r, s, s2;

    s = sys_call3(SYS_socket, AF_UNIX, SOCK_STREAM, 0);
    ok = ok && s >= 0;
    r = do_bind(s, "/s1.sock");
    ok = ok && r == 0;
    if (!ok) { report2("bindstat", 0, r); return; }
    r = sys_call3(SYS_stat, (long)(uintptr_t)"/s1.sock",
                  (long)(uintptr_t)&st, 0);
    ok = ok && r == 0 && (st.mode & S_IFMT_K) == S_IFSOCK_K;
    r = sys_call3(SYS_fstat, s, (long)(uintptr_t)&st, 0);
    ok = ok && r == 0 && (st.mode & S_IFMT_K) == S_IFSOCK_K;
    s2 = sys_call3(SYS_socket, AF_UNIX, SOCK_STREAM, 0);
    r = do_bind(s2, "/s1.sock");
    ok = ok && r == -98; /* EADDRINUSE */
    r = sys_call3(SYS_open, (long)(uintptr_t)"/s1.sock",
                  O_RDONLY, 0);
    ok = ok && r == -6; /* ENXIO: sockets are not openable */
    sys_call3(SYS_close, s2, 0, 0);
    sys_call3(SYS_close, s, 0, 0);
    sys_call3(SYS_unlink, (long)(uintptr_t)"/s1.sock", 0, 0);
    report2("bindstat", ok, r);
}

static void t_refused(void)
{
    int ok = 1;
    long r, s, c;

    s = sys_call3(SYS_socket, AF_UNIX, SOCK_STREAM, 0);
    r = do_bind(s, "/s2.sock");
    ok = ok && r == 0;
    c = sys_call3(SYS_socket, AF_UNIX, SOCK_STREAM, 0);
    r = do_connect(c, "/s2.sock");
    ok = ok && r == -111; /* bound, not listening */
    sys_call3(SYS_close, c, 0, 0);
    c = sys_call3(SYS_socket, AF_UNIX, SOCK_STREAM, 0);
    r = do_connect(c, "/tmp/never.sock");
    ok = ok && r == -2; /* ENOENT */
    sys_call3(SYS_close, c, 0, 0);
    c = sys_call3(SYS_socket, AF_UNIX, SOCK_STREAM, 0);
    r = do_connect(c, "/bin/hello");
    ok = ok && r == -111; /* a plain file is not a socket */
    sys_call3(SYS_close, c, 0, 0);
    sys_call3(SYS_close, s, 0, 0);
    sys_call3(SYS_unlink, (long)(uintptr_t)"/s2.sock", 0, 0);
    report2("refused", ok, r);
}

static void t_e2e(void)
{
    struct sockaddr_un_k pa;
    struct pfd_k pfd;
    uint32_t alen;
    char buf[16];
    int ok = 1;
    long r, s, c, a;

    s = sys_call3(SYS_socket, AF_UNIX, SOCK_STREAM, 0);
    ok = ok && do_bind(s, "/s3.sock") == 0;
    ok = ok && sys_call3(SYS_listen, s, 4, 0) == 0;
    if (!ok) { report2("e2e", 0, -1); return; }
    pfd.fd = (int32_t)s;
    pfd.events = 1;
    pfd.revents = 0;
    r = sys_call3(SYS_poll, (long)(uintptr_t)&pfd, 1, 0);
    ok = ok && r == 0; /* nobody queued yet */
    c = sys_call3(SYS_socket, AF_UNIX, SOCK_STREAM, 0);
    r = do_connect(c, "/s3.sock");
    ok = ok && r == 0;
    pfd.revents = 0;
    r = sys_call3(SYS_poll, (long)(uintptr_t)&pfd, 1, 0);
    ok = ok && r == 1 && (pfd.revents & 1); /* pending: readable */
    alen = 0;
    a = sys_call3(SYS_accept, s, (long)(uintptr_t)&pa,
                  (long)(uintptr_t)&alen);
    ok = ok && a >= 0 && alen == 2 && pa.family == AF_UNIX;
    r = sys_call3(SYS_write, c, (long)(uintptr_t)"hello", 5);
    ok = ok && r == 5;
    r = sys_call3(SYS_read, a, (long)(uintptr_t)buf, 5);
    ok = ok && r == 5 && buf[0] == 'h' && buf[4] == 'o';
    r = sys_call3(SYS_write, a, (long)(uintptr_t)"world", 5);
    ok = ok && r == 5;
    r = sys_call3(SYS_read, c, (long)(uintptr_t)buf, 5);
    ok = ok && r == 5 && buf[0] == 'w' && buf[4] == 'd';
    sys_call3(SYS_close, a, 0, 0);
    sys_call3(SYS_close, c, 0, 0);
    sys_call3(SYS_close, s, 0, 0);
    sys_call3(SYS_unlink, (long)(uintptr_t)"/s3.sock", 0, 0);
    report2("e2e", ok, r);
}

static void t_queue(void)
{
    int ok = 1;
    long r, s, c1, c2, c3, a;

    s = sys_call3(SYS_socket, AF_UNIX, SOCK_STREAM, 0);
    ok = ok && do_bind(s, "/s4.sock") == 0;
    ok = ok && sys_call3(SYS_listen, s, 2, 0) == 0;
    if (!ok) { report2("queue", 0, -1); return; }
    c1 = sys_call3(SYS_socket, AF_UNIX, SOCK_STREAM, 0);
    ok = ok && do_connect(c1, "/s4.sock") == 0;
    c2 = sys_call3(SYS_socket, AF_UNIX, SOCK_STREAM, 0);
    ok = ok && do_connect(c2, "/s4.sock") == 0;
    c3 = sys_call3(SYS_socket, AF_UNIX, SOCK_STREAM, 0);
    r = do_connect(c3, "/s4.sock");
    ok = ok && r == -11; /* backlog full: EAGAIN */
    a = sys_call3(SYS_accept, s, 0, 0);
    ok = ok && a >= 0;
    r = do_connect(c3, "/s4.sock");
    ok = ok && r == 0; /* a slot freed up */
    sys_call3(SYS_close, a, 0, 0);
    a = sys_call3(SYS_accept, s, 0, 0);
    if (a >= 0)
        sys_call3(SYS_close, a, 0, 0);
    a = sys_call3(SYS_accept, s, 0, 0);
    if (a >= 0)
        sys_call3(SYS_close, a, 0, 0);
    sys_call3(SYS_close, c1, 0, 0);
    sys_call3(SYS_close, c2, 0, 0);
    sys_call3(SYS_close, c3, 0, 0);
    sys_call3(SYS_close, s, 0, 0);
    sys_call3(SYS_unlink, (long)(uintptr_t)"/s4.sock", 0, 0);
    report2("queue", ok, r);
}

static void t_nbaccept(void)
{
    int ok = 1;
    long r, s;

    s = sys_call3(SYS_socket, AF_UNIX,
                  SOCK_STREAM | SOCK_NONBLOCK_K, 0);
    ok = ok && s >= 0;
    ok = ok && do_bind(s, "/s5.sock") == 0;
    ok = ok && sys_call3(SYS_listen, s, 2, 0) == 0;
    r = sys_call3(SYS_accept, s, 0, 0);
    ok = ok && r == -11; /* EAGAIN */
    sys_call3(SYS_close, s, 0, 0);
    sys_call3(SYS_unlink, (long)(uintptr_t)"/s5.sock", 0, 0);
    report2("nbaccept", ok, r);
}

static void t_close(void)
{
    char buf[8];
    int ok = 1;
    long r, s, c, a;

    ignore_sigpipe();
    s = sys_call3(SYS_socket, AF_UNIX, SOCK_STREAM, 0);
    ok = ok && do_bind(s, "/s6.sock") == 0;
    ok = ok && sys_call3(SYS_listen, s, 2, 0) == 0;
    c = sys_call3(SYS_socket, AF_UNIX, SOCK_STREAM, 0);
    ok = ok && do_connect(c, "/s6.sock") == 0;
    a = sys_call3(SYS_accept, s, 0, 0);
    ok = ok && a >= 0;
    if (!ok) { report2("close", 0, -1); return; }
    /* the client hangs up: the server side drains to EOF */
    ok = ok && sys_call3(SYS_write, c,
                         (long)(uintptr_t)"bye", 3) == 3;
    sys_call3(SYS_close, c, 0, 0);
    r = sys_call3(SYS_read, a, (long)(uintptr_t)buf, 8);
    ok = ok && r == 3;
    r = sys_call3(SYS_read, a, (long)(uintptr_t)buf, 8);
    ok = ok && r == 0; /* EOF */
    /* writing back to the departed peer: EPIPE */
    r = sys_call3(SYS_write, a, (long)(uintptr_t)"x", 1);
    ok = ok && r == -32;
    sys_call3(SYS_close, a, 0, 0);
    /* the listener dies; its node stays but refuses */
    sys_call3(SYS_close, s, 0, 0);
    c = sys_call3(SYS_socket, AF_UNIX, SOCK_STREAM, 0);
    r = do_connect(c, "/s6.sock");
    ok = ok && r == -111;
    sys_call3(SYS_close, c, 0, 0);
    /* unlink the node: the path itself is gone */
    sys_call3(SYS_unlink, (long)(uintptr_t)"/s6.sock", 0, 0);
    c = sys_call3(SYS_socket, AF_UNIX, SOCK_STREAM, 0);
    r = do_connect(c, "/s6.sock");
    ok = ok && r == -2; /* ENOENT */
    sys_call3(SYS_close, c, 0, 0);
    report2("close", ok, r);
}

static void t_fork(void)
{
    char buf[8];
    int ok = 1;
    long r, s, a, pid, st;

    s = sys_call3(SYS_socket, AF_UNIX, SOCK_STREAM, 0);
    ok = ok && do_bind(s, "/s7.sock") == 0;
    ok = ok && sys_call3(SYS_listen, s, 2, 0) == 0;
    if (!ok) { report2("fork", 0, -1); return; }
    pid = sys_call4(SYS_clone, 17, 0, 0, 0); /* SIGCHLD */
    if (pid == 0) {
        long c = sys_call3(SYS_socket, AF_UNIX, SOCK_STREAM, 0);
        if (c < 0 || do_connect(c, "/s7.sock") != 0)
            sys_call3(SYS_exit_group, 9, 0, 0);
        sys_call3(SYS_write, c, (long)(uintptr_t)"kid", 3);
        sys_call3(SYS_exit_group, 0, 0, 0);
    }
    ok = ok && pid > 0;
    a = sys_call3(SYS_accept, s, 0, 0);
    ok = ok && a >= 0;
    r = a >= 0 ? sys_call3(SYS_read, a, (long)(uintptr_t)buf, 3)
               : -1;
    ok = ok && r == 3 && buf[0] == 'k' && buf[2] == 'd';
    st = 0;
    r = sys_call4(SYS_wait4, pid, (long)(uintptr_t)&st, 0, 0);
    ok = ok && r == pid && ((st >> 8) & 0xff) == 0;
    if (a >= 0)
        sys_call3(SYS_close, a, 0, 0);
    sys_call3(SYS_close, s, 0, 0);
    sys_call3(SYS_unlink, (long)(uintptr_t)"/s7.sock", 0, 0);
    report2("fork", ok, r);
}

int main(void)
{
    out("socktest: M68 named AF_UNIX sockets\n");
    t_reject();
    t_bindstat();
    t_refused();
    t_e2e();
    t_queue();
    t_nbaccept();
    t_close();
    t_fork();
    out("SOCKTEST DONE\n");
    return failures ? 1 : 0;
}

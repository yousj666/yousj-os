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
#define SYS_sendto       44
#define SYS_recvfrom     45
#define SYS_sendmsg      46
#define SYS_recvmsg      47
#define SYS_shutdown     48
#define SYS_bind         49
#define SYS_listen       50
#define SYS_getsockname  51
#define SYS_getpeername  52
#define SYS_socketpair   53
#define SYS_setsockopt   54
#define SYS_getsockopt   55
#define SYS_fcntl        72
#define SYS_accept4      288
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

static long sys_call6(long nr, long a1, long a2, long a3, long a4,
                      long a5, long a6)
{
    long ret;
    register long r10 __asm__("r10") = a4;
    register long r8 __asm__("r8") = a5;
    register long r9 __asm__("r9") = a6;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10),
                       "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return ret;
}

#define MSG_PEEK_K     2
#define MSG_DONTWAIT_K 0x40
#define MSG_WAITALL_K  0x100
#define MSG_NOSIGNAL_K 0x4000

struct iovec_k {
    uint64_t base;
    uint64_t len;
};

struct msghdr_k {
    uint64_t name;
    int32_t namelen;
    int32_t pad0;
    uint64_t iov;
    uint64_t iovlen;
    uint64_t control;
    uint64_t controllen;
    int32_t flags;
    int32_t pad1;
};

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
    r = sys_call3(SYS_socket, AF_UNIX, 3, 0);
    ok = ok && r == -93; /* SOCK_RAW: EPROTONOSUPPORT */
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

static void t_msgio(void)
{
    struct sockaddr_un_k peer;
    struct iovec_k iov[2];
    struct msghdr_k mh;
    char buf[16], b1[4], b2[4];
    uint32_t plen;
    int ok = 1;
    int sv[2];
    long r, s, pid, st;

    /* the M69 surface rides on an M67 pair */
    r = sys_call4(SYS_socketpair, AF_UNIX, SOCK_STREAM, 0,
                  (long)(uintptr_t)sv);
    ok = ok && r == 0;
    if (!ok) { report2("msgio", 0, r); return; }

    /* plain sendto/recvfrom, peer address reported */
    r = sys_call6(SYS_sendto, sv[0], (long)(uintptr_t)"abc", 3,
                  0, 0, 0);
    ok = ok && r == 3;
    plen = sizeof(peer);
    r = sys_call6(SYS_recvfrom, sv[1], (long)(uintptr_t)buf, 3,
                  0, (long)(uintptr_t)&peer,
                  (long)(uintptr_t)&plen);
    ok = ok && r == 3 && buf[0] == 'a' && buf[2] == 'c' &&
         plen == 2 && peer.family == AF_UNIX;

    /* a destination on a connected stream: EISCONN */
    r = sys_call6(SYS_sendto, sv[0], (long)(uintptr_t)"x", 1, 0,
                  (long)(uintptr_t)&peer, 2);
    ok = ok && r == -106;

    /* PEEK does not consume; DONTWAIT on a drained pair */
    r = sys_call6(SYS_sendto, sv[0], (long)(uintptr_t)"peekab",
                  6, 0, 0, 0);
    ok = ok && r == 6;
    r = sys_call6(SYS_recvfrom, sv[1], (long)(uintptr_t)buf, 6,
                  MSG_PEEK_K, 0, 0);
    ok = ok && r == 6 && buf[5] == 'b';
    r = sys_call6(SYS_recvfrom, sv[1], (long)(uintptr_t)buf, 6,
                  MSG_PEEK_K, 0, 0);
    ok = ok && r == 6 && buf[0] == 'p';
    r = sys_call6(SYS_recvfrom, sv[1], (long)(uintptr_t)buf, 6,
                  0, 0, 0);
    ok = ok && r == 6;
    r = sys_call6(SYS_recvfrom, sv[1], (long)(uintptr_t)buf, 6,
                  MSG_DONTWAIT_K, 0, 0);
    ok = ok && r == -11; /* EAGAIN */

    /* WAITALL waits for the rest of the message */
    ok = ok && sys_call3(SYS_write, sv[0],
                         (long)(uintptr_t)"par", 3) == 3;
    pid = sys_call4(SYS_clone, 17, 0, 0, 0);
    if (pid == 0) {
        sys_call3(SYS_write, sv[0], (long)(uintptr_t)"t!", 2);
        sys_call3(SYS_exit_group, 0, 0, 0);
    }
    ok = ok && pid > 0;
    r = sys_call6(SYS_recvfrom, sv[1], (long)(uintptr_t)buf, 5,
                  MSG_WAITALL_K, 0, 0);
    ok = ok && r == 5 && buf[0] == 'p' && buf[3] == 't' &&
         buf[4] == '!';
    st = 0;
    sys_call4(SYS_wait4, pid, (long)(uintptr_t)&st, 0, 0);

    /* msg forms: gather two iovecs, scatter them back */
    iov[0].base = (uint64_t)(uintptr_t)"msg-";
    iov[0].len = 4;
    iov[1].base = (uint64_t)(uintptr_t)"ok";
    iov[1].len = 2;
    mh.name = 0;
    mh.namelen = 0;
    mh.pad0 = 0;
    mh.iov = (uint64_t)(uintptr_t)iov;
    mh.iovlen = 2;
    mh.control = 0;
    mh.controllen = 0;
    mh.flags = 0;
    mh.pad1 = 0;
    r = sys_call3(SYS_sendmsg, sv[0], (long)(uintptr_t)&mh, 0);
    ok = ok && r == 6;
    iov[0].base = (uint64_t)(uintptr_t)b1;
    iov[0].len = 4;
    iov[1].base = (uint64_t)(uintptr_t)b2;
    iov[1].len = 2;
    mh.name = (uint64_t)(uintptr_t)&peer;
    mh.namelen = (int32_t)sizeof(peer);
    mh.flags = 0;
    r = sys_call3(SYS_recvmsg, sv[1], (long)(uintptr_t)&mh, 0);
    ok = ok && r == 6 && b1[0] == 'm' && b1[3] == '-' &&
         b2[0] == 'o' && b2[1] == 'k' && mh.namelen == 2 &&
         peer.family == AF_UNIX;

    /* names: anonymous ends report an empty path, a fresh
     * socket has no name at all, a bound one reports its own */
    plen = sizeof(peer);
    r = sys_call3(SYS_getsockname, sv[0],
                  (long)(uintptr_t)&peer, (long)(uintptr_t)&plen);
    ok = ok && r == 0 && plen == 2 && peer.family == AF_UNIX;
    plen = sizeof(peer);
    r = sys_call3(SYS_getpeername, sv[0],
                  (long)(uintptr_t)&peer, (long)(uintptr_t)&plen);
    ok = ok && r == 0 && plen == 2;
    s = sys_call3(SYS_socket, AF_UNIX, SOCK_STREAM, 0);
    plen = sizeof(peer);
    r = sys_call3(SYS_getsockname, s, (long)(uintptr_t)&peer,
                  (long)(uintptr_t)&plen);
    ok = ok && r == -22; /* EINVAL: unbound */
    ok = ok && do_bind(s, "/s9.sock") == 0;
    plen = sizeof(peer);
    r = sys_call3(SYS_getsockname, s, (long)(uintptr_t)&peer,
                  (long)(uintptr_t)&plen);
    ok = ok && r == 0 && plen == 11 && peer.path[0] == '/' &&
         peer.path[7] == 'k'; /* "/s9.sock" + NUL */
    sys_call3(SYS_close, s, 0, 0);
    sys_call3(SYS_unlink, (long)(uintptr_t)"/s9.sock", 0, 0);

    /* NOSIGNAL: the EPIPE return without the signal path */
    sys_call3(SYS_close, sv[1], 0, 0);
    r = sys_call6(SYS_sendto, sv[0], (long)(uintptr_t)"x", 1,
                  MSG_NOSIGNAL_K, 0, 0);
    ok = ok && r == -32;
    sys_call3(SYS_close, sv[0], 0, 0);
    report2("msgio", ok, r);
}

/* M70: SOCK_DGRAM -- named delivery with sender reporting,
 * packet boundaries, truncation, peer filtering, pair mode */
static void t_dgram(void)
{
    struct sockaddr_un_k dst, from;
    char buf[32], big[1030];
    uint32_t flen;
    int ok = 1;
    int dp[2];
    long r, s, c, a, a2;

    s = sys_call3(SYS_socket, AF_UNIX, 2, 0);
    c = sys_call3(SYS_socket, AF_UNIX, 2, 0);
    ok = ok && s >= 0 && c >= 0;
    fill_addr(&dst, "/d1.sock");
    r = sys_call6(SYS_sendto, c, (long)(uintptr_t)"hi", 2, 0,
                  (long)(uintptr_t)&dst, 110);
    ok = ok && r == -2; /* no such path yet: ENOENT */
    ok = ok && do_bind(s, "/d1.sock") == 0;
    ok = ok && do_bind(c, "/d2.sock") == 0;

    /* named delivery, sender identified by its bound path */
    r = sys_call6(SYS_sendto, c, (long)(uintptr_t)"hello", 5, 0,
                  (long)(uintptr_t)&dst, 110);
    ok = ok && r == 5;
    flen = sizeof(from);
    r = sys_call6(SYS_recvfrom, s, (long)(uintptr_t)buf, 32, 0,
                  (long)(uintptr_t)&from,
                  (long)(uintptr_t)&flen);
    ok = ok && r == 5 && buf[0] == 'h' && buf[4] == 'o' &&
         flen == 11 && from.path[1] == 'd' && from.path[2] == '2';
    fill_addr(&dst, "/d2.sock");
    r = sys_call6(SYS_sendto, s, (long)(uintptr_t)"world!", 6, 0,
                  (long)(uintptr_t)&dst, 110);
    ok = ok && r == 6;
    r = sys_call6(SYS_recvfrom, c, (long)(uintptr_t)buf, 32, 0,
                  0, 0);
    ok = ok && r == 6 && buf[5] == '!';

    /* boundaries: two packets never merge into one read */
    fill_addr(&dst, "/d1.sock");
    sys_call6(SYS_sendto, c, (long)(uintptr_t)"AA", 2, 0,
              (long)(uintptr_t)&dst, 110);
    sys_call6(SYS_sendto, c, (long)(uintptr_t)"BBB", 3, 0,
              (long)(uintptr_t)&dst, 110);
    r = sys_call6(SYS_recvfrom, s, (long)(uintptr_t)buf, 32, 0,
                  0, 0);
    ok = ok && r == 2;
    r = sys_call6(SYS_recvfrom, s, (long)(uintptr_t)buf, 32, 0,
                  0, 0);
    ok = ok && r == 3 && buf[2] == 'B';

    /* truncation: the packet is consumed, MSG_TRUNC reports
     * its true length */
    sys_call6(SYS_sendto, c, (long)(uintptr_t)"0123456789", 10,
              0, (long)(uintptr_t)&dst, 110);
    r = sys_call6(SYS_recvfrom, s, (long)(uintptr_t)buf, 4,
                  0x20 /* MSG_TRUNC */, 0, 0);
    ok = ok && r == 10;

    /* PEEK at a packet, then take it, then nothing left */
    sys_call6(SYS_sendto, c, (long)(uintptr_t)"pk", 2, 0,
              (long)(uintptr_t)&dst, 110);
    r = sys_call6(SYS_recvfrom, s, (long)(uintptr_t)buf, 32,
                  MSG_PEEK_K, 0, 0);
    ok = ok && r == 2 && buf[0] == 'p';
    r = sys_call6(SYS_recvfrom, s, (long)(uintptr_t)buf, 32, 0,
                  0, 0);
    ok = ok && r == 2;
    r = sys_call6(SYS_recvfrom, s, (long)(uintptr_t)buf, 32,
                  MSG_DONTWAIT_K, 0, 0);
    ok = ok && r == -11;

    /* an anonymous sender reports an empty path; connect sets
     * the default destination */
    a = sys_call3(SYS_socket, AF_UNIX, 2, 0);
    ok = ok && a >= 0;
    sys_call6(SYS_sendto, a, (long)(uintptr_t)"anon", 4, 0,
              (long)(uintptr_t)&dst, 110);
    flen = sizeof(from);
    r = sys_call6(SYS_recvfrom, s, (long)(uintptr_t)buf, 32, 0,
                  (long)(uintptr_t)&from,
                  (long)(uintptr_t)&flen);
    ok = ok && r == 4 && flen == 2;
    r = sys_call6(SYS_sendto, a, (long)(uintptr_t)"x", 1, 0, 0,
                  0);
    ok = ok && r == -89; /* EDESTADDRREQ before connect */
    r = sys_call3(SYS_connect, a, (long)(uintptr_t)&dst, 110);
    ok = ok && r == 0;
    r = sys_call6(SYS_sendto, a, (long)(uintptr_t)"aimed", 5, 0,
                  0, 0);
    ok = ok && r == 5;
    r = sys_call6(SYS_recvfrom, s, (long)(uintptr_t)buf, 32, 0,
                  0, 0);
    ok = ok && r == 5 && buf[0] == 'a';

    /* connected receivers filter: after s aims at c, a packet
     * from a third party is dropped silently */
    fill_addr(&dst, "/d2.sock");
    r = sys_call3(SYS_connect, s, (long)(uintptr_t)&dst, 110);
    ok = ok && r == 0;
    a2 = sys_call3(SYS_socket, AF_UNIX, 2, 0);
    fill_addr(&dst, "/d1.sock");
    r = sys_call6(SYS_sendto, a2, (long)(uintptr_t)"drop", 4, 0,
                  (long)(uintptr_t)&dst, 110);
    ok = ok && r == 4; /* sent fine, dropped at the receiver */
    sys_call6(SYS_sendto, c, (long)(uintptr_t)"keep", 4, 0,
              (long)(uintptr_t)&dst, 110);
    r = sys_call6(SYS_recvfrom, s, (long)(uintptr_t)buf, 32, 0,
                  0, 0);
    ok = ok && r == 4 && buf[0] == 'k';
    r = sys_call6(SYS_recvfrom, s, (long)(uintptr_t)buf, 32,
                  MSG_DONTWAIT_K, 0, 0);
    ok = ok && r == -11;

    /* names on datagrams */
    flen = sizeof(from);
    r = sys_call3(SYS_getpeername, s, (long)(uintptr_t)&from,
                  (long)(uintptr_t)&flen);
    ok = ok && r == 0 && flen == 11 && from.path[2] == '2';
    flen = sizeof(from);
    r = sys_call3(SYS_getpeername, c, (long)(uintptr_t)&from,
                  (long)(uintptr_t)&flen);
    ok = ok && r == -107; /* c never connected */

    /* service limits */
    r = sys_call3(SYS_listen, s, 2, 0);
    ok = ok && r == -95; /* EOPNOTSUPP */
    r = sys_call6(SYS_sendto, c, (long)(uintptr_t)big, 1025, 0,
                  (long)(uintptr_t)&dst, 110);
    ok = ok && r == -90; /* EMSGSIZE */

    /* datagram socketpair: aimed at each other from birth */
    r = sys_call4(SYS_socketpair, AF_UNIX, 2, 0,
                  (long)(uintptr_t)dp);
    ok = ok && r == 0;
    r = sys_call6(SYS_sendto, dp[0], (long)(uintptr_t)"pr", 2,
                  0, 0, 0);
    ok = ok && r == 2;
    flen = sizeof(from);
    r = sys_call6(SYS_recvfrom, dp[1], (long)(uintptr_t)buf, 32,
                  0, (long)(uintptr_t)&from,
                  (long)(uintptr_t)&flen);
    ok = ok && r == 2 && buf[1] == 'r' && flen == 2;
    sys_call3(SYS_close, dp[0], 0, 0);
    sys_call3(SYS_close, dp[1], 0, 0);

    sys_call3(SYS_close, s, 0, 0);
    sys_call3(SYS_close, c, 0, 0);
    sys_call3(SYS_close, a, 0, 0);
    sys_call3(SYS_close, a2, 0, 0);
    sys_call3(SYS_unlink, (long)(uintptr_t)"/d1.sock", 0, 0);
    sys_call3(SYS_unlink, (long)(uintptr_t)"/d2.sock", 0, 0);
    report2("dgram", ok, r);
}

/* M71: the SOL_SOCKET ledger and accept4 */
static long getopt_i32(long fd, long name, int32_t *out)
{
    uint32_t len = 4;
    long r = sys_call6(SYS_getsockopt, fd, 1, name,
                       (long)(uintptr_t)out,
                       (long)(uintptr_t)&len, 0);
    if (r == 0 && len != 4)
        return -1000;
    return r;
}

static void t_sockopt(void)
{
    struct sockaddr_un_k un;
    int32_t v;
    int ok = 1;
    long r, s, d, fd, cli, acc;

    s = sys_call3(SYS_socket, AF_UNIX, SOCK_STREAM, 0);
    d = sys_call3(SYS_socket, AF_UNIX, 2, 0);
    ok = ok && s >= 0 && d >= 0;

    /* a plain file is not a socket */
    fd = sys_call3(SYS_open, (long)(uintptr_t)"/sockopt.tmp",
                   0x41, 0644);
    v = 1;
    r = sys_call6(SYS_setsockopt, fd, 1, 2, (long)(uintptr_t)&v,
                  4, 0);
    ok = ok && r == -88; /* ENOTSOCK */
    sys_call3(SYS_close, fd, 0, 0);
    sys_call3(SYS_unlink, (long)(uintptr_t)"/sockopt.tmp", 0, 0);

    /* the boolean ledger round-trips */
    v = 1;
    r = sys_call6(SYS_setsockopt, s, 1, 2 /* REUSEADDR */,
                  (long)(uintptr_t)&v, 4, 0);
    ok = ok && r == 0;
    v = 0;
    ok = ok && getopt_i32(s, 2, &v) == 0 && v == 1;
    v = 0;
    sys_call6(SYS_setsockopt, s, 1, 2, (long)(uintptr_t)&v, 4,
              0);
    v = 9;
    ok = ok && getopt_i32(s, 2, &v) == 0 && v == 0;

    /* buffer sizes record and report; an untouched datagram
     * socket reports the default */
    v = 8192;
    r = sys_call6(SYS_setsockopt, s, 1, 8 /* RCVBUF */,
                  (long)(uintptr_t)&v, 4, 0);
    ok = ok && r == 0;
    v = 0;
    ok = ok && getopt_i32(s, 8, &v) == 0 && v == 8192;
    v = 0;
    ok = ok && getopt_i32(d, 8, &v) == 0 && v == 4096;

    /* type and the (always clear) error slot */
    v = 0;
    ok = ok && getopt_i32(s, 3, &v) == 0 && v == 1;
    v = 0;
    ok = ok && getopt_i32(d, 3, &v) == 0 && v == 2;
    v = 7;
    ok = ok && getopt_i32(s, 4, &v) == 0 && v == 0;

    /* refusals: unknown name, wrong level, short value,
     * setting a read-only option */
    v = 1;
    r = sys_call6(SYS_setsockopt, s, 1, 999,
                  (long)(uintptr_t)&v, 4, 0);
    ok = ok && r == -92; /* ENOPROTOOPT */
    r = sys_call6(SYS_setsockopt, s, 6 /* IPPROTO_TCP */, 2,
                  (long)(uintptr_t)&v, 4, 0);
    ok = ok && r == -92;
    r = sys_call6(SYS_setsockopt, s, 1, 2,
                  (long)(uintptr_t)&v, 2, 0);
    ok = ok && r == -22; /* EINVAL */
    r = sys_call6(SYS_setsockopt, s, 1, 3 /* SO_TYPE */,
                  (long)(uintptr_t)&v, 4, 0);
    ok = ok && r == -92;
    r = getopt_i32(s, 999, &v);
    ok = ok && r == -92;

    /* accept4: flags land on the accepted socket */
    ok = ok && do_bind(s, "/o1.sock") == 0;
    ok = ok && sys_call3(SYS_listen, s, 2, 0) == 0;
    cli = sys_call3(SYS_socket, AF_UNIX, SOCK_STREAM, 0);
    fill_addr(&un, "/o1.sock");
    ok = ok && sys_call3(SYS_connect, cli,
                         (long)(uintptr_t)&un, 110) == 0;
    acc = sys_call4(SYS_accept4, s, 0, 0,
                    0x800 | 0x80000); /* NONBLOCK|CLOEXEC */
    ok = ok && acc >= 0;
    r = sys_call3(SYS_fcntl, acc, 3 /* F_GETFL */, 0);
    ok = ok && (r & 0x800) != 0; /* O_NONBLOCK */
    r = sys_call3(SYS_fcntl, acc, 1 /* F_GETFD */, 0);
    ok = ok && (r & 1) != 0; /* FD_CLOEXEC */
    r = sys_call4(SYS_accept4, s, 0, 0, 0x4000);
    ok = ok && r == -22; /* unknown flag bits */
    sys_call3(SYS_close, acc, 0, 0);
    sys_call3(SYS_close, cli, 0, 0);
    sys_call3(SYS_close, s, 0, 0);
    sys_call3(SYS_close, d, 0, 0);
    sys_call3(SYS_unlink, (long)(uintptr_t)"/o1.sock", 0, 0);
    report2("sockopt", ok, r);
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
    t_msgio();
    t_dgram();
    t_sockopt();
    out("SOCKTEST DONE\n");
    return failures ? 1 : 0;
}

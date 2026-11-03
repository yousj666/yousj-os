/* Yousj OS - M10 pipe/dup acceptance suite (userland).
 *
 *   basic      write then read back through one pipe, same task
 *   block-eof  child writes and closes; parent drains the data
 *              then reads EOF (0) once every write end is gone
 *   epipe      with SIGPIPE ignored, writing to a reader-less
 *              pipe returns -EPIPE instead of killing us
 *   full       a nonblocking pipe takes exactly 4096 bytes,
 *              then writes report -EAGAIN; draining restores it
 *   dup        dup/dup2 share the object (offset moves for
 *              both), close of one reference keeps it alive,
 *              bad descriptors report -EBADF
 *   cloexec    a pipe2(O_CLOEXEC) read end vanishes across
 *              execve (the exec'd mode reports EBADF), while a
 *              plain fd survives
 *
 * Prints "PIPETEST <name> PASS"/"PIPETEST <name> FAIL detail=",
 * then "PIPETEST DONE"; exit 0 iff all passed.
 */
#include <stdint.h>
#include <stddef.h>

#define SYS_read         0
#define SYS_write        1
#define SYS_open         2
#define SYS_close        3
#define SYS_lseek        8
#define SYS_rt_sigaction 13
#define SYS_pipe         22
#define SYS_sendfile     40
#define SYS_splice       275
#define SYS_tee          276
#define SYS_vmsplice     278
#define SYS_unlink       87
#define SYS_dup          32
#define SYS_dup2         33
#define SYS_clone        56
#define SYS_execve       59
#define SYS_wait4        61
#define SYS_exit_group   231
#define SYS_pipe2        293
#define SYS_socketpair   53
#define SYS_shutdown     48
#define SYS_poll         7

#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#define O_CREAT   0x40
#define O_TRUNC   0x200
#define O_NONBLOCK 0x800
#define O_CLOEXEC 0x80000

#define SIGPIPE 13
#define SIG_IGN 1

#define SEEK_CUR 1

struct k_sigaction {
    uint64_t handler;
    uint32_t flags;
    uint32_t reserved;
    uint64_t restorer;
    uint64_t mask;
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
        buf[i++] = (char)('0' + (int)(u % 10));
        u /= 10;
    }
    while (i > 0) {
        char c[2];
        c[0] = buf[--i];
        c[1] = '\0';
        out(c);
    }
}

static int g_fails;
static void report2(const char *name, int ok, long detail)
{
    out("PIPETEST ");
    out(name);
    out(ok ? " PASS" : " FAIL detail=");
    if (!ok)
        out_long(detail);
    out("\n");
    if (!ok)
        g_fails++;
}

static void exit_now(long code)
{
    sys_call3(SYS_exit_group, code, 0, 0);
    for (;;) {
    }
}

static int bytes_eq(const char *a, const char *b, long n)
{
    long i;
    for (i = 0; i < n; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

static void t_basic(void)
{
    int fds[2] = { -1, -1 };
    char buf[16];
    long r = sys_call3(SYS_pipe, (long)(uintptr_t)fds, 0, 0);
    long w, n;
    if (r != 0) {
        report2("basic", 0, r);
        return;
    }
    w = sys_call3(SYS_write, fds[1], (long)(uintptr_t)"pipe-ok", 7);
    n = sys_call3(SYS_read, fds[0], (long)(uintptr_t)buf, (long)sizeof(buf));
    report2("basic", w == 7 && n == 7 && bytes_eq(buf, "pipe-ok", 7), n);
    sys_call3(SYS_close, fds[0], 0, 0);
    sys_call3(SYS_close, fds[1], 0, 0);
}

static void t_block_eof(void)
{
    int fds[2] = { -1, -1 };
    long pid, st = 0;
    char buf[8];
    long n1, n2;
    if (sys_call3(SYS_pipe, (long)(uintptr_t)fds, 0, 0) != 0) {
        report2("block-eof", 0, -1);
        return;
    }
    pid = sys_call3(SYS_clone, 17, 0, 0);
    if (pid == 0) {
        sys_call3(SYS_close, fds[0], 0, 0);
        sys_call3(SYS_write, fds[1], (long)(uintptr_t)"AB", 2);
        sys_call3(SYS_close, fds[1], 0, 0);
        exit_now(0);
    }
    if (pid < 0) {
        report2("block-eof", 0, pid);
        return;
    }
    sys_call3(SYS_close, fds[1], 0, 0); /* our write end goes too,
                                         * or EOF can never come */
    n1 = sys_call3(SYS_read, fds[0], (long)(uintptr_t)buf, (long)sizeof(buf));
    n2 = sys_call3(SYS_read, fds[0], (long)(uintptr_t)buf, (long)sizeof(buf));
    sys_call4(SYS_wait4, pid, (long)(uintptr_t)&st, 0, 0);
    sys_call3(SYS_close, fds[0], 0, 0);
    report2("block-eof",
            n1 == 2 && bytes_eq(buf, "AB", 2) && n2 == 0, n2);
}

static void t_epipe(void)
{
    struct k_sigaction act;
    int fds[2] = { -1, -1 };
    long w;
    act.handler = SIG_IGN;
    act.flags = 0;
    act.reserved = 0;
    act.restorer = 0;
    act.mask = 0;
    sys_call4(SYS_rt_sigaction, SIGPIPE, (long)(uintptr_t)&act, 0, 8);
    if (sys_call3(SYS_pipe, (long)(uintptr_t)fds, 0, 0) != 0) {
        report2("epipe", 0, -1);
        return;
    }
    sys_call3(SYS_close, fds[0], 0, 0); /* no readers left */
    w = sys_call3(SYS_write, fds[1], (long)(uintptr_t)"x", 1);
    sys_call3(SYS_close, fds[1], 0, 0);
    report2("epipe", w == -32, w); /* -EPIPE */
}

static void t_full(void)
{
    int fds[2] = { -1, -1 };
    static char big[4096];
    static char back[4096];
    long i, w = 0, w2, n = 0, n2;
    for (i = 0; i < 4096; i++)
        big[i] = (char)(i * 7 + 1);
    if (sys_call4(SYS_pipe2, (long)(uintptr_t)fds, O_NONBLOCK, 0, 0) != 0) {
        report2("full", 0, -1);
        return;
    }
    while (w < 4096) {
        long k = sys_call3(SYS_write, fds[1], (long)(uintptr_t)(big + w),
                           4096 - w);
        if (k <= 0)
            break;
        w += k;
    }
    w2 = sys_call3(SYS_write, fds[1], (long)(uintptr_t)"z", 1);
    while (n < w) {
        long k = sys_call3(SYS_read, fds[0], (long)(uintptr_t)(back + n),
                           4096 - n);
        if (k <= 0)
            break;
        n += k;
    }
    n2 = sys_call3(SYS_read, fds[0], (long)(uintptr_t)back, 1);
    sys_call3(SYS_close, fds[0], 0, 0);
    sys_call3(SYS_close, fds[1], 0, 0);
    report2("full", w == 4096 && w2 == -11 && n == 4096 &&
            bytes_eq(back, big, 4096) && n2 == -11, w2);
}

static void t_dup(void)
{
    long fd = sys_call3(SYS_open, (long)(uintptr_t)"/dup.txt",
                        O_CREAT | O_RDWR | O_TRUNC, 0644);
    long d1, d2, pos, bad1, bad2, ok;
    char buf[8];
    long n;
    if (fd < 0) {
        report2("dup", 0, fd);
        return;
    }
    sys_call3(SYS_write, fd, (long)(uintptr_t)"ab", 2);
    d1 = sys_call3(SYS_dup, fd, 0, 0);
    sys_call3(SYS_write, d1, (long)(uintptr_t)"cd", 2);
    pos = sys_call3(SYS_lseek, fd, 0, SEEK_CUR); /* shared: 4 */
    d2 = sys_call3(SYS_dup2, d1, 9, 0);
    sys_call3(SYS_close, fd, 0, 0);
    sys_call3(SYS_close, d1, 0, 0);
    sys_call3(SYS_write, d2, (long)(uintptr_t)"e", 1); /* survives */
    sys_call3(SYS_lseek, d2, 0, 0);
    n = sys_call3(SYS_read, d2, (long)(uintptr_t)buf, (long)sizeof(buf));
    bad1 = sys_call3(SYS_dup, 99, 0, 0);
    bad2 = sys_call3(SYS_dup2, 99, 3, 0);
    ok = (d1 >= 0) && (pos == 4) && (d2 == 9) && (n == 5) &&
         bytes_eq(buf, "abcde", 5) && (bad1 == -9) && (bad2 == -9);
    sys_call3(SYS_close, d2, 0, 0);
    report2("dup", ok, pos);
}

/* The exec'd half of the cloexec check: argv[1] is the fd number
 * (as text) of the CLOEXEC pipe end, argv[2] of a plain file.
 * The pipe end must be EBADF; the file must still work. */
static int exec_child_mode(int argc, char **argv)
{
    long pfd = 0, ffd = 0, r1, r2 = -99;
    char ch = 0;
    (void)argc;
    pfd = argv[1][0] - '0';
    ffd = argv[2][0] - '0';
    r1 = sys_call3(SYS_read, pfd, (long)(uintptr_t)&ch, 1);
    r2 = sys_call3(SYS_lseek, ffd, 0, SEEK_CUR);
    exit_now((r1 == -9 && r2 >= 0) ? 0 : 5);
    return 0;
}

/* M67: socketpair -- bidirectional stream, shutdown
 * semantics, close/EOF/EPIPE, poll readiness, and a
 * cross-process exchange through fork inheritance. */
struct pfd_k {
    int32_t fd;
    int16_t events, revents;
};

static void t_sockpair(void)
{
    struct k_sigaction act;
    int sv[2] = { -1, -1 };
    int sv2[2] = { -1, -1 };
    char buf[16];
    long n;
    int ok = 1;
    struct pfd_k pfd;

    act.handler = SIG_IGN; /* SIGPIPE must not kill us */
    act.flags = 0;
    act.reserved = 0;
    act.restorer = 0;
    act.mask = 0;
    sys_call4(SYS_rt_sigaction, SIGPIPE,
              (long)(uintptr_t)&act, 0, 8);

    /* rejects: wrong family, wrong type */
    ok = ok && sys_call4(SYS_socketpair, 2, 1, 0,
                         (long)(uintptr_t)sv) == -97;
    ok = ok && sys_call4(SYS_socketpair, 1, 3, 0,
                         (long)(uintptr_t)sv) == -93;
    if (sys_call4(SYS_socketpair, 1, 1, 0,
                  (long)(uintptr_t)sv) != 0) {
        report2("sockpair", 0, -1);
        return;
    }
    ok = ok && sv[0] >= 0 && sv[1] >= 0 && sv[0] != sv[1];

    /* both directions */
    ok = ok && sys_call3(SYS_write, sv[0],
                         (long)(uintptr_t)"ping", 4) == 4;
    n = sys_call3(SYS_read, sv[1], (long)(uintptr_t)buf, 4);
    ok = ok && n == 4 && buf[0] == 'p' && buf[3] == 'g';
    ok = ok && sys_call3(SYS_write, sv[1],
                         (long)(uintptr_t)"pong!", 5) == 5;
    n = sys_call3(SYS_read, sv[0], (long)(uintptr_t)buf, 5);
    ok = ok && n == 5 && buf[0] == 'p' && buf[4] == '!';

    /* poll */
    ok = ok && sys_call3(SYS_write, sv[0],
                         (long)(uintptr_t)"z", 1) == 1;
    pfd.fd = sv[1];
    pfd.events = 1; /* POLLIN */
    pfd.revents = 0;
    ok = ok && sys_call3(SYS_poll, (long)(uintptr_t)&pfd,
                         1, 0) == 1 && (pfd.revents & 1);
    pfd.fd = sv[0];
    pfd.revents = 0;
    ok = ok && sys_call3(SYS_poll, (long)(uintptr_t)&pfd,
                         1, 0) == 0;
    n = sys_call3(SYS_read, sv[1], (long)(uintptr_t)buf, 1);
    ok = ok && n == 1 && buf[0] == 'z';

    /* shutdown(WR) on sv[0]: sv[1] drains then EOFs,
     * while the reverse direction stays open */
    ok = ok && sys_call3(SYS_write, sv[0],
                         (long)(uintptr_t)"end", 3) == 3;
    ok = ok && sys_call3(SYS_shutdown, sv[0], 1, 0) == 0;
    n = sys_call3(SYS_read, sv[1], (long)(uintptr_t)buf, 8);
    ok = ok && n == 3 && buf[0] == 'e';
    n = sys_call3(SYS_read, sv[1], (long)(uintptr_t)buf, 8);
    ok = ok && n == 0; /* EOF */
    ok = ok && sys_call3(SYS_write, sv[1],
                         (long)(uintptr_t)"back", 4) == 4;
    n = sys_call3(SYS_read, sv[0], (long)(uintptr_t)buf, 4);
    ok = ok && n == 4 && buf[0] == 'b';

    /* shutdown RD */
    ok = ok && sys_call3(SYS_shutdown, sv[1], 0, 0) == 0;
    ok = ok && sys_call3(SYS_write, sv[0],
                         (long)(uintptr_t)"x", 1) == -32;
    sys_call3(SYS_close, sv[0], 0, 0);
    sys_call3(SYS_close, sv[1], 0, 0);

    /* close */
    if (sys_call4(SYS_socketpair, 1, 1, 0,
                  (long)(uintptr_t)sv2) != 0) {
        report2("sockpair", 0, -2);
        return;
    }
    sys_call3(SYS_close, sv2[1], 0, 0);
    n = sys_call3(SYS_read, sv2[0], (long)(uintptr_t)buf,
                  4);
    ok = ok && n == 0;
    ok = ok && sys_call3(SYS_write, sv2[0],
                         (long)(uintptr_t)"x", 1) == -32;
    sys_call3(SYS_close, sv2[0], 0, 0);

    /* across fork */
    if (sys_call4(SYS_socketpair, 1, 1, 0,
                  (long)(uintptr_t)sv2) != 0) {
        report2("sockpair", 0, -3);
        return;
    }
    {
        long c = sys_call4(SYS_clone, 17, 0, 0, 0);
        if (c == 0) {
            sys_call3(SYS_write, sv2[1],
                      (long)(uintptr_t)"kid", 3);
            sys_call3(SYS_exit_group, 0, 0, 0);
        }
        {
            long st = 0;
            n = sys_call3(SYS_read, sv2[0],
                          (long)(uintptr_t)buf, 3);
            ok = ok && n == 3 && buf[0] == 'k' &&
                 buf[2] == 'd';
            sys_call4(SYS_wait4, c, (long)(uintptr_t)&st,
                      0, 0);
        }
    }
    sys_call3(SYS_close, sv2[0], 0, 0);
    sys_call3(SYS_close, sv2[1], 0, 0);
    report2("sockpair", ok, 0);
}

static void t_cloexec(void)
{
    int fds[2] = { -1, -1 };
    long ff, pid, st = 0;
    char a1[2], a2[2];
    const char *cargv[4];
    if (sys_call4(SYS_pipe2, (long)(uintptr_t)fds, O_CLOEXEC, 0, 0) != 0) {
        report2("cloexec", 0, -1);
        return;
    }
    ff = sys_call3(SYS_open, (long)(uintptr_t)"/dup.txt", O_RDWR, 0);
    if (ff < 0 || fds[0] > 9 || ff > 9) {
        report2("cloexec", 0, ff);
        return;
    }
    pid = sys_call3(SYS_clone, 17, 0, 0);
    if (pid == 0) {
        a1[0] = (char)('0' + fds[0]);
        a1[1] = '\0';
        a2[0] = (char)('0' + ff);
        a2[1] = '\0';
        cargv[0] = "/bin/pipetest";
        cargv[1] = a1;
        cargv[2] = a2;
        cargv[3] = NULL;
        sys_call3(SYS_execve, (long)(uintptr_t)"/bin/pipetest",
                  (long)(uintptr_t)cargv, 0);
        exit_now(127);
    }
    if (pid < 0) {
        report2("cloexec", 0, pid);
        return;
    }
    sys_call4(SYS_wait4, pid, (long)(uintptr_t)&st, 0, 0);
    sys_call3(SYS_close, fds[0], 0, 0);
    sys_call3(SYS_close, fds[1], 0, 0);
    sys_call3(SYS_close, ff, 0, 0);
    report2("cloexec", (st & 0xFF) == 0 && ((st >> 8) & 0xFF) == 0, st);
}

/* M40: sendfile from a file into a pipe. */
static void t_sendfile_pipe(void)
{
    int fds[2] = { -1, -1 };
    char buf[16];
    long fd, n, got;
    fd = sys_call3(SYS_open, (long)(uintptr_t)"/sf.txt",
                   0101, 0644);
    sys_call3(SYS_write, fd, (long)(uintptr_t)"sendfile!",
              9);
    sys_call3(SYS_close, fd, 0, 0);
    fd = sys_call3(SYS_open, (long)(uintptr_t)"/sf.txt", 0,
                   0);
    sys_call3(SYS_pipe, (long)(uintptr_t)fds, 0, 0);
    n = sys_call4(SYS_sendfile, fds[1], fd, 0, 9);
    sys_call3(SYS_close, fds[1], 0, 0);
    got = sys_call3(SYS_read, fds[0], (long)(uintptr_t)buf,
                    (long)sizeof(buf));
    report2("sendfile-pipe",
            n == 9 && got == 9 &&
                bytes_eq(buf, "sendfile!", 9),
            n);
    sys_call3(SYS_close, fds[0], 0, 0);
    sys_call3(SYS_close, fd, 0, 0);
    sys_call3(SYS_unlink, (long)(uintptr_t)"/sf.txt", 0, 0);
}

/* M42: splice in all three directions plus the rules. */
static long sys_call6(long nr, long a1, long a2, long a3,
                      long a4, long a5, long a6)
{
    long r;
    register long r10 __asm__("r10") = a4;
    register long r8 __asm__("r8") = a5;
    register long r9 __asm__("r9") = a6;
    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "a"(nr), "D"(a1), "S"(a2), "d"(a3),
                       "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return r;
}

static void t_splice(void)
{
    int p1[2] = { -1, -1 }, p2[2] = { -1, -1 };
    char buf[16];
    long fd, fd2, n, got;
    int64_t off = 0;
    int ok;
    fd = sys_call3(SYS_open, (long)(uintptr_t)"/sp.txt",
                   0101, 0644);
    sys_call3(SYS_write, fd,
              (long)(uintptr_t)"splice-data", 11);
    sys_call3(SYS_close, fd, 0, 0);
    fd = sys_call3(SYS_open, (long)(uintptr_t)"/sp.txt", 0,
                   0);
    sys_call3(SYS_pipe, (long)(uintptr_t)p1, 0, 0);
    sys_call3(SYS_pipe, (long)(uintptr_t)p2, 0, 0);
    /* file -> pipe */
    n = sys_call6(SYS_splice, fd, 0, p1[1], 0, 11, 0);
    ok = n == 11;
    /* pipe -> pipe */
    n = sys_call6(SYS_splice, p1[0], 0, p2[1], 0, 11, 0);
    ok = ok && n == 11;
    /* pipe -> file at an explicit offset */
    fd2 = sys_call3(SYS_open, (long)(uintptr_t)"/sp2.txt",
                    0101, 0644);
    n = sys_call6(SYS_splice, p2[0], 0, fd2,
                  (long)(uintptr_t)&off, 11, 0);
    ok = ok && n == 11 && off == 11;
    sys_call3(SYS_close, fd2, 0, 0);
    fd2 = sys_call3(SYS_open, (long)(uintptr_t)"/sp2.txt",
                    0, 0);
    got = sys_call3(SYS_read, fd2, (long)(uintptr_t)buf,
                    (long)sizeof(buf));
    ok = ok && got == 11 &&
         bytes_eq(buf, "splice-data", 11);
    /* rule: file->file is EINVAL (fd2 still open here) */
    ok = ok && sys_call6(SYS_splice, fd, 0, fd2, 0, 1,
                         0) == -22;
    sys_call3(SYS_close, fd2, 0, 0);
    /* rule: a pipe offset pointer is EINVAL (the
     * file->file rule was checked earlier, while both
     * file descriptors were still open) */
    sys_call3(SYS_write, p1[1], (long)(uintptr_t)"z", 1);
    ok = ok && sys_call6(SYS_splice, p1[0],
                         (long)(uintptr_t)&off, p2[1], 0, 1,
                         0) == -22;
    report2("splice", ok, 0);
    sys_call3(SYS_close, p1[0], 0, 0);
    sys_call3(SYS_close, p1[1], 0, 0);
    sys_call3(SYS_close, p2[0], 0, 0);
    sys_call3(SYS_close, p2[1], 0, 0);
    sys_call3(SYS_close, fd, 0, 0);
    sys_call3(SYS_unlink, (long)(uintptr_t)"/sp.txt", 0, 0);
    sys_call3(SYS_unlink, (long)(uintptr_t)"/sp2.txt", 0, 0);
}

/* M49: SPLICE_F_NONBLOCK is honored -- an empty source
 * and a full destination both report -EAGAIN, and the
 * full-destination failure consumes nothing. */
static void t_splice_nonblock(void)
{
    int p1[2] = { -1, -1 }, p2[2] = { -1, -1 };
    static char fill[4096];
    char buf[16];
    long fd, n;
    int ok;
    sys_call3(SYS_pipe, (long)(uintptr_t)p1, 0, 0);
    sys_call3(SYS_pipe, (long)(uintptr_t)p2, 0, 0);
    /* empty pipe source, NONBLOCK: -EAGAIN, not a hang */
    n = sys_call6(SYS_splice, p1[0], 0, p2[1], 0, 4, 2);
    ok = n == -11;
    /* fill p2 completely through its write end */
    sys_call3(SYS_write, p2[1], (long)(uintptr_t)fill, 4096);
    sys_call3(SYS_write, p1[1], (long)(uintptr_t)"abcd", 4);
    /* full pipe destination, NONBLOCK: -EAGAIN ... */
    n = sys_call6(SYS_splice, p1[0], 0, p2[1], 0, 4, 2);
    ok = ok && n == -11;
    /* ... and the source kept its bytes (no loss) */
    n = sys_call3(SYS_read, p1[0], (long)(uintptr_t)buf, 4);
    ok = ok && n == 4 && bytes_eq(buf, "abcd", 4);
    /* with room again, the same splice succeeds */
    sys_call3(SYS_read, p2[0], (long)(uintptr_t)fill, 16);
    sys_call3(SYS_write, p1[1], (long)(uintptr_t)"wxyz", 4);
    n = sys_call6(SYS_splice, p1[0], 0, p2[1], 0, 4, 2);
    ok = ok && n == 4;
    /* a regular-file source into a completely full pipe
     * also reports -EAGAIN without consuming */
    sys_call3(SYS_read, p2[0], (long)(uintptr_t)fill, 4096);
    fd = sys_call3(SYS_open, (long)(uintptr_t)"/spnb.txt",
                   0101, 0644);
    sys_call3(SYS_write, fd, (long)(uintptr_t)"filedata", 8);
    sys_call3(SYS_close, fd, 0, 0);
    fd = sys_call3(SYS_open, (long)(uintptr_t)"/spnb.txt", 0, 0);
    sys_call3(SYS_write, p2[1], (long)(uintptr_t)fill, 4096);
    n = sys_call6(SYS_splice, fd, 0, p2[1], 0, 8, 2);
    ok = ok && n == -11;
    sys_call3(SYS_close, fd, 0, 0);
    report2("splice-nonblock", ok, 0);
    sys_call3(SYS_close, p1[0], 0, 0);
    sys_call3(SYS_close, p1[1], 0, 0);
    sys_call3(SYS_close, p2[0], 0, 0);
    sys_call3(SYS_close, p2[1], 0, 0);
    sys_call3(SYS_unlink, (long)(uintptr_t)"/spnb.txt", 0, 0);
}

/* M49: tee copies pipe to pipe without consuming. */
static void t_tee(void)
{
    int p1[2] = { -1, -1 }, p2[2] = { -1, -1 };
    char buf[16];
    long fd, n;
    int ok;
    sys_call3(SYS_pipe, (long)(uintptr_t)p1, 0, 0);
    sys_call3(SYS_pipe, (long)(uintptr_t)p2, 0, 0);
    sys_call3(SYS_write, p1[1], (long)(uintptr_t)"tee-data-49",
              11);
    n = sys_call4(SYS_tee, p1[0], p2[1], 11, 0);
    ok = n == 11;
    /* the copy arrived ... */
    n = sys_call3(SYS_read, p2[0], (long)(uintptr_t)buf, 11);
    ok = ok && n == 11 && bytes_eq(buf, "tee-data-49", 11);
    /* ... and the source still holds every byte */
    n = sys_call3(SYS_read, p1[0], (long)(uintptr_t)buf, 11);
    ok = ok && n == 11 && bytes_eq(buf, "tee-data-49", 11);
    /* empty source with NONBLOCK: -EAGAIN */
    n = sys_call4(SYS_tee, p1[0], p2[1], 4, 2);
    ok = ok && n == -11;
    /* only SPLICE_F_NONBLOCK may be set */
    ok = ok && sys_call4(SYS_tee, p1[0], p2[1], 4, 1) == -22;
    /* a file end is not a pipe: -EINVAL */
    fd = sys_call3(SYS_open, (long)(uintptr_t)"/tee.txt",
                   0101, 0644);
    ok = ok && sys_call4(SYS_tee, fd, p2[1], 4, 0) == -22;
    sys_call3(SYS_close, fd, 0, 0);
    report2("tee", ok, 0);
    sys_call3(SYS_close, p1[0], 0, 0);
    sys_call3(SYS_close, p1[1], 0, 0);
    sys_call3(SYS_close, p2[0], 0, 0);
    sys_call3(SYS_close, p2[1], 0, 0);
    sys_call3(SYS_unlink, (long)(uintptr_t)"/tee.txt", 0, 0);
}

/* M49: vmsplice gathers user iovecs into a pipe. */
struct test_iov {
    const char *base;
    uint64_t len;
};

static void t_vmsplice(void)
{
    int p[2] = { -1, -1 };
    static struct test_iov iov[2];
    char buf[16];
    long fd, n;
    int ok;
    iov[0].base = "vm-";
    iov[0].len = 3;
    iov[1].base = "splice!";
    iov[1].len = 7;
    sys_call3(SYS_pipe, (long)(uintptr_t)p, 0, 0);
    n = sys_call4(SYS_vmsplice, p[1], (long)(uintptr_t)iov,
                  2, 0);
    ok = n == 10;
    n = sys_call3(SYS_read, p[0], (long)(uintptr_t)buf, 10);
    ok = ok && n == 10 && bytes_eq(buf, "vm-splice!", 10);
    /* SPLICE_F_GIFT is accepted (data still copied) */
    n = sys_call4(SYS_vmsplice, p[1], (long)(uintptr_t)iov,
                  1, 8);
    ok = ok && n == 3;
    sys_call3(SYS_read, p[0], (long)(uintptr_t)buf, 3);
    /* MOVE is not a vmsplice flag: -EINVAL */
    ok = ok && sys_call4(SYS_vmsplice, p[1],
                         (long)(uintptr_t)iov, 1, 1) == -22;
    /* the destination must be a pipe */
    fd = sys_call3(SYS_open, (long)(uintptr_t)"/vm.txt",
                   0101, 0644);
    ok = ok && sys_call4(SYS_vmsplice, fd,
                         (long)(uintptr_t)iov, 1, 0) == -22;
    sys_call3(SYS_close, fd, 0, 0);
    report2("vmsplice", ok, 0);
    sys_call3(SYS_close, p[0], 0, 0);
    sys_call3(SYS_close, p[1], 0, 0);
    sys_call3(SYS_unlink, (long)(uintptr_t)"/vm.txt", 0, 0);
}

int main(int argc, char **argv)
{
    if (argc > 2 && argv[1][0] >= '0' && argv[1][0] <= '9' &&
        argv[2][0] >= '0' && argv[2][0] <= '9')
        return exec_child_mode(argc, argv);
    out("pipetest: M10 pipe/dup suite\n");
    t_basic();
    t_sendfile_pipe();
    t_splice();
    t_splice_nonblock();
    t_tee();
    t_vmsplice();
    t_block_eof();
    t_epipe();
    t_full();
    t_dup();
    t_cloexec();
    t_sockpair();
    out("PIPETEST DONE\n");
    return g_fails == 0 ? 0 : 1;
}

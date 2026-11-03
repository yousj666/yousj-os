/* M48 acceptance: inotify_init1 / add_watch / rm_watch
 * and the event stream for create, write+close,
 * rename and unlink, on a ramfs directory.
 *
 * Freestanding. Prints "inotifytest: <case> PASS|FAIL"
 * per scenario and "INOTIFYTEST DONE"; exit 0 iff all
 * passed.
 */
#include <stdint.h>
#include <stddef.h>

static long sys_call1(long n, long a1)
{
    long ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(n), "D"(a1)
                     : "rcx", "r11", "memory");
    return ret;
}
static long sys_call2(long n, long a1, long a2)
{
    long ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(n), "D"(a1), "S"(a2)
                     : "rcx", "r11", "memory");
    return ret;
}
static long sys_call3(long n, long a1, long a2, long a3)
{
    long ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return ret;
}
static long sys_call4(long n, long a1, long a2, long a3, long a4)
{
    long ret;
    register long r10 __asm__("r10") = a4;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3),
                       "r"(r10)
                     : "rcx", "r11", "memory");
    return ret;
}

static long sys_call5(long n, long a1, long a2, long a3,
                      long a4, long a5)
{
    long ret;
    register long r10 __asm__("r10") = a4;
    register long r8 __asm__("r8") = a5;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3),
                       "r"(r10), "r"(r8)
                     : "rcx", "r11", "memory");
    return ret;
}

#define SYS_read     0
#define SYS_write    1
#define SYS_open     2
#define SYS_close    3
#define SYS_poll     7
#define SYS_truncate 76
#define SYS_ftruncate 77
#define SYS_mkdir    83
#define SYS_unlink   87
#define SYS_rename   82
#define SYS_renameat2 316
#define SYS_inotify_init1    294
#define SYS_inotify_add_watch 254
#define SYS_inotify_rm_watch 255
#define SYS_utimensat 280

#define IN_NONBLOCK  0x800
#define IN_MODIFY    0x00000002u
#define IN_ATTRIB    0x00000004u
#define IN_CLOSE_WRITE 0x00000008u
#define IN_MOVED_FROM  0x00000040u
#define IN_MOVED_TO    0x00000080u
#define IN_CREATE      0x00000100u
#define IN_DELETE      0x00000200u
#define IN_DELETE_SELF 0x00000400u
#define IN_MOVE_SELF   0x00000800u
#define IN_IGNORED     0x00008000u

static int g_fails = 0;

static void out(const char *s)
{
    long n = 0;
    while (s[n] != '\0')
        n++;
    sys_call3(SYS_write, 1, (long)(uintptr_t)s, n);
}

static void report(const char *name, int ok)
{
    out("inotifytest: ");
    out(name);
    out(ok ? " PASS\n" : " FAIL\n");
    if (!ok)
        g_fails++;
}

struct ev {
    int32_t wd;
    uint32_t mask;
    uint32_t cookie;
    uint32_t len;
};

static int name_is(const char *evbuf, const char *want)
{
    const char *nm = evbuf + 16;
    uint64_t i = 0;
    while (want[i] != '\0' && nm[i] == want[i])
        i++;
    return want[i] == '\0' && nm[i] == '\0';
}

/* Parse one event at *off; returns its total size. */
static long ev_at(const char *buf, long off, struct ev *e,
                  const char **name_out)
{
    const struct ev *src = (const struct ev *)(buf + off);
    *e = *src;
    *name_out = buf + off + 16;
    return 16 + (long)e->len;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    char buf[1024];
    struct ev e;
    const char *nm;
    long ifd, wd_dir, wd_file, n, off;

    sys_call3(SYS_mkdir, (long)(uintptr_t)"/iw", 0755, 0);
    ifd = sys_call1(SYS_inotify_init1, IN_NONBLOCK);
    if (ifd < 0) {
        report("init", 0);
        out("INOTIFYTEST DONE\n");
        return 1;
    }
    report("init", 1);

    /* empty read is EAGAIN in nonblocking mode */
    n = sys_call3(SYS_read, ifd, (long)(uintptr_t)buf,
                  sizeof(buf));
    report("eagain", n == -11);

    wd_dir = sys_call3(SYS_inotify_add_watch, ifd,
                       (long)(uintptr_t)"/iw",
                       IN_CREATE | IN_DELETE | IN_MOVED_FROM |
                           IN_MOVED_TO);
    report("watch-dir", wd_dir == 1);

    /* create: one IN_CREATE event naming the child */
    {
        long fd = sys_call4(SYS_open,
                            (long)(uintptr_t)"/iw/a.txt",
                            0101 /* O_WRONLY|O_CREAT */, 0644, 0);
        if (fd >= 0)
            sys_call1(SYS_close, fd);
        n = sys_call3(SYS_read, ifd, (long)(uintptr_t)buf,
                      sizeof(buf));
        off = ev_at(buf, 0, &e, &nm);
        report("create", n == off && e.wd == 1 &&
               (e.mask & IN_CREATE) != 0 &&
               name_is(buf, "a.txt"));
    }

    wd_file = sys_call3(SYS_inotify_add_watch, ifd,
                        (long)(uintptr_t)"/iw/a.txt",
                        IN_MODIFY | IN_ATTRIB |
                            IN_CLOSE_WRITE |
                            IN_DELETE_SELF | IN_MOVE_SELF);
    report("watch-file", wd_file == 2);

    /* write + close: MODIFY then CLOSE_WRITE, no names */
    {
        long fd = sys_call3(SYS_open,
                            (long)(uintptr_t)"/iw/a.txt", 1,
                            0);
        struct ev e2;
        const char *nm2;
        long off2;
        sys_call3(SYS_write, fd, (long)(uintptr_t)"xyz", 3);
        sys_call1(SYS_close, fd);
        n = sys_call3(SYS_read, ifd, (long)(uintptr_t)buf,
                      sizeof(buf));
        off = ev_at(buf, 0, &e, &nm);
        off2 = off + ev_at(buf, off, &e2, &nm2);
        report("modify-close", n == off2 && e.wd == 2 &&
               (e.mask & IN_MODIFY) != 0 && e.len == 0 &&
               e2.wd == 2 &&
               (e2.mask & IN_CLOSE_WRITE) != 0);
    }

    /* utimensat: a stamp change is IN_ATTRIB (M49) */
    {
        static const long times[4] = { 1234567, 0,
                                       1234568, 0 };
        long rc = sys_call4(SYS_utimensat, -100,
                            (long)(uintptr_t)"/iw/a.txt",
                            (long)(uintptr_t)times, 0);
        n = sys_call3(SYS_read, ifd, (long)(uintptr_t)buf,
                      sizeof(buf));
        off = ev_at(buf, 0, &e, &nm);
        report("utimens", rc == 0 && n == off && e.wd == 2 &&
               (e.mask & IN_ATTRIB) != 0 && e.len == 0);
    }

    /* rename: FROM/TO pair with one cookie on the dir
     * watch, MOVE_SELF on the file watch */
    {
        struct ev e2, e3;
        const char *n2, *n3;
        long o1, o2;
        sys_call2(SYS_rename, (long)(uintptr_t)"/iw/a.txt",
                  (long)(uintptr_t)"/iw/b.txt");
        n = sys_call3(SYS_read, ifd, (long)(uintptr_t)buf,
                      sizeof(buf));
        o1 = ev_at(buf, 0, &e, &nm);
        o2 = o1 + ev_at(buf, o1, &e2, &n2);
        o2 += ev_at(buf, o2, &e3, &n3);
        report("rename", n == o2 && e.wd == 1 &&
               (e.mask & IN_MOVED_FROM) != 0 &&
               name_is(buf, "a.txt") && e2.wd == 1 &&
               (e2.mask & IN_MOVED_TO) != 0 &&
               name_is(buf + o1, "b.txt") &&
               e.cookie != 0 && e.cookie == e2.cookie &&
               e3.wd == 2 && (e3.mask & IN_MOVE_SELF) != 0);
    }

    /* poll readiness: queue empty -> not ready; after a
     * create -> ready */
    {
        struct {
            int fd;
            short events, revents;
        } pfd;
        long r;
        /* a previous run in this boot may have left
         * c.txt behind; remove it and drain whatever
         * the removal queued, so both polls start from
         * a known state */
        sys_call1(SYS_unlink, (long)(uintptr_t)"/iw/c.txt");
        sys_call3(SYS_read, ifd, (long)(uintptr_t)buf,
                  sizeof(buf));
        pfd.fd = ifd;
        pfd.events = 1; /* POLLIN */
        pfd.revents = 0;
        r = sys_call3(SYS_poll, (long)(uintptr_t)&pfd, 1, 0);
        int ok = r == 0;
        {
            long fd = sys_call4(SYS_open,
                                (long)(uintptr_t)"/iw/c.txt",
                                0101, 0644, 0);
            if (fd >= 0)
                sys_call1(SYS_close, fd);
        }
        pfd.revents = 0;
        r = sys_call3(SYS_poll, (long)(uintptr_t)&pfd, 1, 0);
        ok = ok && r == 1 && (pfd.revents & 1) != 0;
        report("poll-ready", ok);
        n = sys_call3(SYS_read, ifd, (long)(uintptr_t)buf,
                      sizeof(buf)); /* drain */
    }

    /* unlink: DELETE on the dir, DELETE_SELF + IGNORED
     * on the file watch, which then no longer exists */
    {
        struct ev e2, e3;
        const char *n2, *n3;
        long o1, o2;
        sys_call1(SYS_unlink, (long)(uintptr_t)"/iw/b.txt");
        n = sys_call3(SYS_read, ifd, (long)(uintptr_t)buf,
                      sizeof(buf));
        o1 = ev_at(buf, 0, &e, &nm);
        o2 = o1 + ev_at(buf, o1, &e2, &n2);
        o2 += ev_at(buf, o2, &e3, &n3);
        int ok = n == o2 && e.wd == 1 &&
                 (e.mask & IN_DELETE) != 0 &&
                 name_is(buf, "b.txt") && e2.wd == 2 &&
                 (e2.mask & IN_DELETE_SELF) != 0 &&
                 e3.wd == 2 && (e3.mask & IN_IGNORED) != 0;
        ok = ok && sys_call2(SYS_inotify_rm_watch, ifd,
                             wd_file) == -22;
        report("unlink", ok);
    }

    /* M62: ftruncate/truncate resize and report
     * IN_MODIFY on the file watch; the grown region
     * reads back as zeroes; a read-only fd refuses */
    {
        long fd = sys_call4(SYS_open,
                            (long)(uintptr_t)"/iw/t.txt",
                            0101, 0644, 0);
        long wd_t, ok = 1, rn;
        char rb[16];
        sys_call3(SYS_write, fd, (long)(uintptr_t)"hello world",
                  11);
        sys_call1(SYS_close, fd);
        sys_call3(SYS_read, ifd, (long)(uintptr_t)buf,
                  sizeof(buf)); /* drain CREATE */
        wd_t = sys_call3(SYS_inotify_add_watch, ifd,
                         (long)(uintptr_t)"/iw/t.txt",
                         IN_MODIFY);
        fd = sys_call3(SYS_open, (long)(uintptr_t)"/iw/t.txt",
                       2 /* O_RDWR */, 0);
        ok = ok && sys_call2(SYS_ftruncate, fd, 5) == 0;
        n = sys_call3(SYS_read, ifd, (long)(uintptr_t)buf,
                      sizeof(buf));
        off = ev_at(buf, 0, &e, &nm);
        ok = ok && n == off && e.wd == wd_t &&
             (e.mask & IN_MODIFY) != 0 && e.len == 0;
        ok = ok && sys_call2(SYS_truncate,
                             (long)(uintptr_t)"/iw/t.txt",
                             8) == 0;
        n = sys_call3(SYS_read, ifd, (long)(uintptr_t)buf,
                      sizeof(buf));
        off = ev_at(buf, 0, &e, &nm);
        ok = ok && n == off && (e.mask & IN_MODIFY) != 0;
        rn = sys_call3(SYS_read, fd, (long)(uintptr_t)rb,
                       sizeof(rb));
        ok = ok && rn == 8 && rb[0] == 'h' && rb[4] == 'o' &&
             rb[5] == 0 && rb[6] == 0 && rb[7] == 0;
        {
            long rfd = sys_call3(SYS_open,
                                 (long)(uintptr_t)"/iw/t.txt",
                                 0, 0);
            ok = ok && sys_call2(SYS_ftruncate, rfd, 1) ==
                 -22; /* EINVAL: not open for writing */
            sys_call1(SYS_close, rfd);
        }
        sys_call1(SYS_close, fd);
        report("truncate", ok);
    }

    /* M62: RENAME_EXCHANGE swaps the contents and
     * reports FROM/TO for both names on the dir watch
     * (one cookie) plus MOVE_SELF on both file watches */
    {
        long fa = sys_call4(SYS_open,
                            (long)(uintptr_t)"/iw/xa",
                            0101, 0644, 0);
        long fb = sys_call4(SYS_open,
                            (long)(uintptr_t)"/iw/xb",
                            0101, 0644, 0);
        long wd_a, wd_b, rc, ok = 1;
        int from_a = 0, to_a = 0, from_b = 0, to_b = 0;
        int self_a = 0, self_b = 0;
        uint32_t cookie = 0;
        char rb[8];
        sys_call3(SYS_write, fa, (long)(uintptr_t)"AAAA", 4);
        sys_call3(SYS_write, fb, (long)(uintptr_t)"BBBB", 4);
        sys_call1(SYS_close, fa);
        sys_call1(SYS_close, fb);
        sys_call3(SYS_read, ifd, (long)(uintptr_t)buf,
                  sizeof(buf)); /* drain CREATEs */
        wd_a = sys_call3(SYS_inotify_add_watch, ifd,
                         (long)(uintptr_t)"/iw/xa",
                         IN_MOVE_SELF);
        wd_b = sys_call3(SYS_inotify_add_watch, ifd,
                         (long)(uintptr_t)"/iw/xb",
                         IN_MOVE_SELF);
        rc = sys_call5(SYS_renameat2, -100,
                       (long)(uintptr_t)"/iw/xa", -100,
                       (long)(uintptr_t)"/iw/xb",
                       2 /* RENAME_EXCHANGE */);
        ok = ok && rc == 0;
        n = sys_call3(SYS_read, ifd, (long)(uintptr_t)buf,
                      sizeof(buf));
        off = 0;
        while (off < n) {
            const char *en;
            long sz = ev_at(buf, off, &e, &en);
            if (e.wd == wd_dir &&
                (e.mask & IN_MOVED_FROM) != 0 &&
                name_is(buf + off, "xa"))
                from_a = 1;
            else if (e.wd == wd_dir &&
                     (e.mask & IN_MOVED_TO) != 0 &&
                     name_is(buf + off, "xa"))
                to_a = 1;
            else if (e.wd == wd_dir &&
                     (e.mask & IN_MOVED_FROM) != 0 &&
                     name_is(buf + off, "xb"))
                from_b = 1;
            else if (e.wd == wd_dir &&
                     (e.mask & IN_MOVED_TO) != 0 &&
                     name_is(buf + off, "xb"))
                to_b = 1;
            else if (e.wd == wd_a &&
                     (e.mask & IN_MOVE_SELF) != 0)
                self_a = 1;
            else if (e.wd == wd_b &&
                     (e.mask & IN_MOVE_SELF) != 0)
                self_b = 1;
            if (e.wd == wd_dir) {
                if (cookie == 0)
                    cookie = e.cookie;
                else if (e.cookie != cookie)
                    ok = 0;
            }
            off += sz;
        }
        ok = ok && from_a && to_a && from_b && to_b &&
             self_a && self_b && cookie != 0;
        fa = sys_call3(SYS_open, (long)(uintptr_t)"/iw/xa",
                       0, 0);
        n = sys_call3(SYS_read, fa, (long)(uintptr_t)rb,
                      sizeof(rb));
        ok = ok && n == 4 && rb[0] == 'B' && rb[3] == 'B';
        sys_call1(SYS_close, fa);
        report("exchange", ok != 0);
    }

    /* rm_watch: succeeds, IGNORED is delivered */
    {
        long rc = sys_call2(SYS_inotify_rm_watch, ifd, wd_dir);
        n = sys_call3(SYS_read, ifd, (long)(uintptr_t)buf,
                      sizeof(buf));
        off = ev_at(buf, 0, &e, &nm);
        report("rm-watch", rc == 0 && n == off && e.wd == 1 &&
               (e.mask & IN_IGNORED) != 0);
    }

    sys_call1(SYS_close, ifd);
    out("INOTIFYTEST DONE\n");
    return g_fails == 0 ? 0 : 1;
}

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

#define SYS_read     0
#define SYS_write    1
#define SYS_open     2
#define SYS_close    3
#define SYS_poll     7
#define SYS_mkdir    83
#define SYS_unlink   87
#define SYS_rename   82
#define SYS_inotify_init1    294
#define SYS_inotify_add_watch 254
#define SYS_inotify_rm_watch 255

#define IN_NONBLOCK  0x800
#define IN_MODIFY    0x00000002u
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
                        IN_MODIFY | IN_CLOSE_WRITE |
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

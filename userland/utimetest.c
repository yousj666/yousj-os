/* utimetest.c - M26 acceptance: utimensat/futimens and
 * independent atime/ctime/mtime tracking.
 *
 *   utimetest [path]         full independence (ramfs)
 *   utimetest [path] alias   single-stamp backend (yfs):
 *                            atime/ctime alias the one mtime
 *
 * Freestanding: raw syscalls only. Exit code is the
 * number of failed scenarios; each prints PASS/FAIL. */
#include <stdint.h>
#include <stddef.h>

#define SYS_read 0
#define SYS_write 1
#define SYS_open 2
#define SYS_close 3
#define SYS_stat 4
#define SYS_fstat 5
#define SYS_lstat 6
#define SYS_time 201
#define SYS_futimens 261
#define SYS_newfstatat 262
#define SYS_utimensat 280
#define SYS_unlink 87
#define SYS_symlink 88

#define AT_FDCWD (-100)
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_EMPTY_PATH 0x1000
#define UTIME_NOW 1073741823
#define UTIME_OMIT 1073741822

static long sys_call1(long n, long a)
{
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a)
                     : "rcx", "r11", "memory");
    return r;
}
static long sys_call2(long n, long a, long b)
{
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b)
                     : "rcx", "r11", "memory");
    return r;
}
static long sys_call3(long n, long a, long b, long c)
{
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c)
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

static uint64_t slen(const char *s)
{
    uint64_t n = 0;
    while (s[n])
        n++;
    return n;
}
static void out(const char *s)
{
    sys_call3(SYS_write, 1, (long)(uintptr_t)s, (long)slen(s));
}
static int fails;
static void report(const char *name, int ok)
{
    out("utimetest: ");
    out(name);
    out(ok ? " PASS\n" : " FAIL\n");
    if (!ok)
        fails++;
}
struct st {
    uint64_t dev, ino;
    uint64_t nlink;
    uint32_t mode;
    uint32_t uid, gid, pad0;
    uint64_t rdev, size;
    int64_t blksize, blocks;
    int64_t atime, atime_nsec, mtime, mtime_nsec, ctime, ctime_nsec;
    int64_t unused[3];
};
struct ts {
    int64_t sec, nsec;
};

static long now_s(void)
{
    return sys_call1(SYS_time, 0);
}
static long stat_of(const char *path, struct st *sb)
{
    return sys_call2(SYS_stat, (long)(uintptr_t)path,
                     (long)(uintptr_t)sb);
}
static long lstat_of(const char *path, struct st *sb)
{
    return sys_call2(SYS_lstat, (long)(uintptr_t)path,
                     (long)(uintptr_t)sb);
}
static long mkfile(const char *path)
{
    long fd = sys_call3(SYS_open, (long)(uintptr_t)path,
                        0x241, 0644); /* WRONLY|CREAT|TRUNC */
    if (fd >= 0) {
        sys_call3(SYS_write, fd, (long)(uintptr_t)"data", 4);
        sys_call1(SYS_close, fd);
    }
    return fd;
}
static long utimens(const char *path, struct ts *ts2, long flags)
{
    return sys_call4(SYS_utimensat, AT_FDCWD,
                     (long)(uintptr_t)path,
                     (long)(uintptr_t)ts2, flags);
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/ut-file";
    int alias = argc > 2 && argv[2][0] == 'a';
    struct st sb;
    struct ts ts2[2];
    long t_start = now_s();
    long fd;

    out("utimetest: M26 utimens suite on ");
    out(path);
    out(alias ? " (alias)\n" : "\n");

    /* 1: explicit set of both stamps */
    sys_call1(SYS_unlink, (long)(uintptr_t)path);
    fd = mkfile(path);
    ts2[0].sec = 946684800;  ts2[0].nsec = 0;   /* 2000 atime */
    ts2[1].sec = 978307200;  ts2[1].nsec = 0;   /* 2001 mtime */
    {
        long rc = utimens(path, ts2, 0);
        stat_of(path, &sb);
        if (alias)
            report("set-explicit", fd >= 0 && rc == 0 &&
                   sb.atime == 978307200 && sb.mtime == 978307200 &&
                   sb.ctime == 978307200);
        else
            report("set-explicit", fd >= 0 && rc == 0 &&
                   sb.atime == 946684800 && sb.mtime == 978307200 &&
                   sb.ctime >= t_start && sb.ctime <= now_s() + 2);
    }

    /* 2: UTIME_OMIT leaves the other stamp alone */
    ts2[0].sec = 0;          ts2[0].nsec = UTIME_OMIT;
    ts2[1].sec = 1000000000; ts2[1].nsec = 0;
    {
        long rc = utimens(path, ts2, 0);
        stat_of(path, &sb);
        if (alias)
            report("omit", rc == 0 && sb.mtime == 1000000000 &&
                   sb.atime == 1000000000);
        else
            report("omit", rc == 0 && sb.atime == 946684800 &&
                   sb.mtime == 1000000000);
    }

    /* 3: NULL times means "now" for both */
    {
        long rc = utimens(path, NULL, 0);
        stat_of(path, &sb);
        report("now-null", rc == 0 &&
               sb.atime >= t_start && sb.atime <= now_s() + 2 &&
               sb.mtime >= t_start && sb.mtime <= now_s() + 2);
    }

    /* 4: futimens on an open fd */
    fd = sys_call3(SYS_open, (long)(uintptr_t)path, 0, 0);
    ts2[0].sec = 123456789;  ts2[0].nsec = 0;
    ts2[1].sec = 0;          ts2[1].nsec = UTIME_NOW;
    {
        long rc = sys_call2(SYS_futimens, fd,
                            (long)(uintptr_t)ts2);
        long src = sys_call2(SYS_fstat, fd,
                             (long)(uintptr_t)&sb);
        sys_call1(SYS_close, fd);
        if (alias)
            report("futimens", rc == 0 && src == 0 &&
                   sb.mtime >= t_start && sb.atime == sb.mtime);
        else
            report("futimens", rc == 0 && src == 0 &&
                   sb.atime == 123456789 &&
                   sb.mtime >= t_start && sb.mtime <= now_s() + 2);
    }

    /* 5: NOFOLLOW stamps the link, not the target (ramfs
     * paths regardless of where the probe file lives) */
    sys_call1(SYS_unlink, (long)(uintptr_t)"/ut-target");
    sys_call1(SYS_unlink, (long)(uintptr_t)"/ut-link");
    mkfile("/ut-target");
    sys_call2(SYS_symlink, (long)(uintptr_t)"/ut-target",
              (long)(uintptr_t)"/ut-link");
    ts2[0].sec = 111111111;  ts2[0].nsec = 0;
    ts2[1].sec = 222222222;  ts2[1].nsec = 0;
    {
        long rc = utimens("/ut-link", ts2, AT_SYMLINK_NOFOLLOW);
        struct st lb, tb;
        lstat_of("/ut-link", &lb);
        stat_of("/ut-target", &tb);
        report("nofollow", rc == 0 &&
               lb.atime == 111111111 && lb.mtime == 222222222 &&
               tb.mtime > 1000000000);
    }
    sys_call1(SYS_unlink, (long)(uintptr_t)"/ut-link");
    sys_call1(SYS_unlink, (long)(uintptr_t)"/ut-target");

    /* 6: a read moves atime (strict), mtime stays put */
    if (!alias) {
        ts2[0].sec = 900000000; ts2[0].nsec = 0;
        ts2[1].sec = 0;         ts2[1].nsec = UTIME_OMIT;
        utimens(path, ts2, 0);
        stat_of(path, &sb);
        long m_before = sb.mtime;
        char b[1];
        fd = sys_call3(SYS_open, (long)(uintptr_t)path, 0, 0);
        long n = fd >= 0 ? sys_call3(SYS_read, fd,
                                     (long)(uintptr_t)b, 1) : -1;
        if (fd >= 0)
            sys_call1(SYS_close, fd);
        stat_of(path, &sb);
        report("read-atime", n == 1 && sb.atime >= t_start &&
               sb.mtime == m_before);
    }

    /* 7: a write moves mtime + ctime, leaves atime;
     * in alias mode all three are the one stamp */
    ts2[0].sec = 800000000;  ts2[0].nsec = 0;
    ts2[1].sec = 800000000;  ts2[1].nsec = 0;
    utimens(path, ts2, 0);
    fd = sys_call3(SYS_open, (long)(uintptr_t)path, 0x401,
                   0644); /* WRONLY|APPEND */
    if (fd >= 0) {
        sys_call3(SYS_write, fd, (long)(uintptr_t)"z", 1);
        sys_call1(SYS_close, fd);
    }
    stat_of(path, &sb);
    if (alias)
        report("write-stamps", fd >= 0 &&
               sb.atime == sb.mtime && sb.mtime >= t_start);
    else
        report("write-stamps", fd >= 0 &&
               sb.atime == 800000000 && sb.mtime >= t_start &&
               sb.ctime >= t_start);

    /* 8: error surface */
    {
        struct ts bad[2];
        bad[0].sec = 100; bad[0].nsec = 1000000000; /* nsec range */
        bad[1].sec = 100; bad[1].nsec = 0;
        long e1 = utimens("/ut-no-such", NULL, 0);
        long e2 = utimens(path, bad, 0);
        bad[0].sec = -5; bad[0].nsec = 0;
        long e3 = utimens(path, bad, 0);
        long e4 = utimens(path, NULL, 0x4000);      /* bad flags */
        long e5 = sys_call2(SYS_futimens, 999,
                            (long)(uintptr_t)NULL);
        long e6 = utimens("/proc/version", NULL, 0); /* ro fs */
        long e7 = sys_call4(SYS_utimensat, 5,
                            (long)(uintptr_t)"rel", 0, 0); /* dirfd */
        report("errors", e1 == -2 && e2 == -22 && e3 == -22 &&
               e4 == -22 && e5 == -9 && e6 == -13 && e7 == -38);
    }

    /* 9: AT_EMPTY_PATH acts on the dirfd itself */
    fd = sys_call3(SYS_open, (long)(uintptr_t)path, 0, 0);
    ts2[0].sec = 555000111;  ts2[0].nsec = 0;
    ts2[1].sec = 0;          ts2[1].nsec = UTIME_OMIT;
    {
        long rc = sys_call4(SYS_utimensat, fd,
                            (long)(uintptr_t)"",
                            (long)(uintptr_t)ts2, AT_EMPTY_PATH);
        long rc2 = sys_call4(SYS_utimensat, fd,
                             (long)(uintptr_t)"",
                             (long)(uintptr_t)ts2, 0);
        if (fd >= 0)
            sys_call1(SYS_close, fd);
        stat_of(path, &sb);
        if (alias)
            report("empty-path", rc == 0 && rc2 == -2);
        else
            report("empty-path", rc == 0 && rc2 == -2 &&
                   sb.atime == 555000111);
    }

    sys_call1(SYS_unlink, (long)(uintptr_t)path);
    out("UTIMETEST DONE\n");
    return fails;
}

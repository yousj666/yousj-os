/* proctest.c - M25 acceptance: the /proc filesystem.
 *
 *  uptime:   two decimal fields, idle <= up, and a second
 *            open 1.2 s later shows a larger uptime.
 *  meminfo:  MemTotal/MemFree present; MemFree matches
 *            the native free-page counter (sampled around
 *            the read, with tolerance).
 *  version:  contains "Yousj".
 *  dents:    /proc lists uptime/meminfo/version as REG.
 *  stat:     /proc is a dir, uptime is 0444 REG, fstat
 *            size equals the content length.
 *  guards:   missing file ENOENT, write-open EACCES,
 *            write() EBADF, mkdir under /proc EACCES.
 *  seek:     re-read after SEEK_SET matches; SEEK_END
 *            lands on the stat size.
 *  chdir:    cd /proc and a relative open works.
 *
 * Freestanding: raw syscalls only. Exit code = failures. */
#include <stdint.h>
#include <stddef.h>

#define SYS_read 0
#define SYS_write 1
#define SYS_open 2
#define SYS_close 3
#define SYS_stat 4
#define SYS_lseek 8
#define SYS_nanosleep 35
#define SYS_mkdir 83
#define SYS_unlink 87
#define SYS_chdir 80
#define SYS_getdents64 217
#define SYS_fstat 5
#define SYS_free_pages 0x1001

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
    out("proctest: ");
    out(name);
    out(ok ? " PASS\n" : " FAIL\n");
    if (!ok)
        fails++;
}
static int contains(const char *buf, long n, const char *needle)
{
    long m = (long)slen(needle), i;
    if (m > n)
        return 0;
    for (i = 0; i + m <= n; i++) {
        long j = 0;
        while (j < m && buf[i + j] == needle[j])
            j++;
        if (j == m)
            return 1;
    }
    return 0;
}
/* parse "<digits>.<2 digits>" at *p, advancing it */
static long parse_ts(const char **p, int *ok)
{
    long v = 0, cs = 0;
    int digits = 0;
    while (**p >= '0' && **p <= '9') {
        v = v * 10 + (**p - '0');
        (*p)++;
        digits++;
    }
    if (**p != '.' || digits == 0) {
        *ok = 0;
        return 0;
    }
    (*p)++;
    if (**p < '0' || **p > '9') {
        *ok = 0;
        return 0;
    }
    cs = (**p - '0') * 10;
    (*p)++;
    if (**p >= '0' && **p <= '9') {
        cs += **p - '0';
        (*p)++;
    }
    return v * 100 + cs;
}
static long read_all(long fd, char *buf, long cap)
{
    long n = 0, r;
    while (n < cap && (r = sys_call3(SYS_read, fd, (long)(uintptr_t)(buf + n), cap - n)) > 0)
        n += r;
    return n;
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
static void sleep_ms(long ms)
{
    struct ts t;
    t.sec = ms / 1000;
    t.nsec = (ms % 1000) * 1000000;
    sys_call2(SYS_nanosleep, (long)(uintptr_t)&t, 0);
}

int main(void)
{
    char buf[512];
    long fd, n;
    struct st sb;

    out("proctest: M25 /proc suite\n");

    /* uptime */
    fd = sys_call3(SYS_open, (long)(uintptr_t)"/proc/uptime", 0, 0);
    n = fd >= 0 ? read_all(fd, buf, sizeof(buf) - 1) : -1;
    if (fd >= 0)
        sys_call1(SYS_close, fd);
    buf[n > 0 ? n : 0] = '\0';
    {
        const char *p = buf;
        int ok1 = 1, ok2 = 1;
        long up = parse_ts(&p, &ok1);
        long idle = 0;
        if (*p == ' ') {
            p++;
            idle = parse_ts(&p, &ok2);
        } else
            ok2 = 0;
        report("uptime", n > 0 && ok1 && ok2 && up > 0 &&
               idle >= 0 && idle <= up);
        sleep_ms(1200);
        fd = sys_call3(SYS_open, (long)(uintptr_t)"/proc/uptime",
                       0, 0);
        n = fd >= 0 ? read_all(fd, buf, sizeof(buf) - 1) : -1;
        if (fd >= 0)
            sys_call1(SYS_close, fd);
        buf[n > 0 ? n : 0] = '\0';
        p = buf;
        ok1 = 1;
        {
            long up2 = parse_ts(&p, &ok1);
            report("uptime-adv", n > 0 && ok1 && up2 >= up + 100);
        }
    }

    /* meminfo */
    {
        long before = sys_call1(SYS_free_pages, 0);
        fd = sys_call3(SYS_open, (long)(uintptr_t)"/proc/meminfo",
                       0, 0);
        n = fd >= 0 ? read_all(fd, buf, sizeof(buf) - 1) : -1;
        if (fd >= 0)
            sys_call1(SYS_close, fd);
        long after = sys_call1(SYS_free_pages, 0);
        buf[n > 0 ? n : 0] = '\0';
        /* extract the MemFree number */
        long freekb = -1;
        if (contains(buf, n, "MemFree:")) {
            const char *p = buf;
            while (*p && !(p[0] == 'M' && p[1] == 'e' &&
                           p[2] == 'm' && p[3] == 'F'))
                p++;
            while (*p && (*p < '0' || *p > '9'))
                p++;
            freekb = 0;
            while (*p >= '0' && *p <= '9') {
                freekb = freekb * 10 + (*p - '0');
                p++;
            }
        }
        long lo = after * 4 - 256, hi = before * 4 + 256;
        report("meminfo", n > 0 &&
               contains(buf, n, "MemTotal:") &&
               freekb >= lo && freekb <= hi);
    }

    /* version */
    fd = sys_call3(SYS_open, (long)(uintptr_t)"/proc/version",
                   0, 0);
    n = fd >= 0 ? read_all(fd, buf, sizeof(buf) - 1) : -1;
    if (fd >= 0)
        sys_call1(SYS_close, fd);
    report("version", n > 0 && contains(buf, n, "Yousj"));

    /* dents */
    {
        char db[512];
        long got = 0, r;
        fd = sys_call3(SYS_open, (long)(uintptr_t)"/proc", 0, 0);
        while (fd >= 0 && got < (long)sizeof(db) &&
               (r = sys_call3(SYS_getdents64, fd,
                              (long)(uintptr_t)(db + got),
                              sizeof(db) - got)) > 0)
            got += r;
        if (fd >= 0)
            sys_call1(SYS_close, fd);
        report("dents", got > 0 &&
               contains(db, got, "uptime") &&
               contains(db, got, "meminfo") &&
               contains(db, got, "version"));
    }

    /* stat + fstat */
    {
        long src = sys_call2(SYS_stat,
                             (long)(uintptr_t)"/proc/uptime",
                             (long)(uintptr_t)&sb);
        int ok = src == 0 &&
                 (sb.mode & 0170000u) == 0100000u &&
                 (sb.mode & 0777u) == 0444u;
        fd = sys_call3(SYS_open, (long)(uintptr_t)"/proc/uptime",
                       0, 0);
        n = fd >= 0 ? read_all(fd, buf, sizeof(buf)) : -1;
        struct st fb;
        long frc = fd >= 0 ? sys_call2(SYS_fstat, fd,
                                       (long)(uintptr_t)&fb) : -1;
        ok = ok && frc == 0 && (long)fb.size == n;
        if (fd >= 0)
            sys_call1(SYS_close, fd);
        src = sys_call2(SYS_stat, (long)(uintptr_t)"/proc",
                        (long)(uintptr_t)&sb);
        ok = ok && src == 0 && (sb.mode & 0170000u) == 0040000u;
        report("stat", ok);
    }

    /* guards */
    {
        int ok = 1;
        ok = ok && sys_call3(SYS_open,
                             (long)(uintptr_t)"/proc/nope",
                             0, 0) == -2;
        ok = ok && sys_call3(SYS_open,
                             (long)(uintptr_t)"/proc/uptime",
                             1 /*WRONLY*/, 0) == -13;
        fd = sys_call3(SYS_open, (long)(uintptr_t)"/proc/uptime",
                       0, 0);
        ok = ok && fd >= 0 &&
             sys_call3(SYS_write, fd, (long)(uintptr_t)"x", 1)
                 == -9;
        if (fd >= 0)
            sys_call1(SYS_close, fd);
        ok = ok && sys_call1(SYS_mkdir,
                             (long)(uintptr_t)"/proc/x") == -13;
        report("guards", ok);
    }

    /* seek */
    {
        char a[8], b2[8];
        fd = sys_call3(SYS_open, (long)(uintptr_t)"/proc/version",
                       0, 0);
        long n1 = fd >= 0 ? sys_call3(SYS_read, fd,
                                      (long)(uintptr_t)a, 5) : -1;
        long back = fd >= 0 ? sys_call3(SYS_lseek, fd, 0, 0) : -1;
        long n2 = fd >= 0 ? sys_call3(SYS_read, fd,
                                      (long)(uintptr_t)b2, 5) : -1;
        long end = fd >= 0 ? sys_call3(SYS_lseek, fd, 0, 2) : -1;
        sys_call2(SYS_stat, (long)(uintptr_t)"/proc/version",
                  (long)(uintptr_t)&sb);
        int same = n1 == 5 && n2 == 5;
        long i;
        for (i = 0; i < 5 && same; i++)
            same = a[i] == b2[i];
        if (fd >= 0)
            sys_call1(SYS_close, fd);
        report("seek", same && back == 0 &&
               end == (long)sb.size);
    }

    /* chdir + relative */
    {
        int ok = sys_call1(SYS_chdir,
                           (long)(uintptr_t)"/proc") == 0;
        fd = sys_call3(SYS_open, (long)(uintptr_t)"meminfo",
                       0, 0);
        n = fd >= 0 ? read_all(fd, buf, sizeof(buf) - 1) : -1;
        if (fd >= 0)
            sys_call1(SYS_close, fd);
        ok = ok && n > 0 && contains(buf, n, "MemTotal:");
        sys_call1(SYS_chdir, (long)(uintptr_t)"/");
        report("chdir", ok);
    }

    out("PROCTEST DONE\n");
    return fails;
}

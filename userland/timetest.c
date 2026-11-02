/* timetest.c - M23 acceptance: RTC-seeded wall clock,
 * gettimeofday/clock_gettime, and stat mtimes that track
 * writes. An optional argv[1] selects the probe file
 * (default /tt-file, the disk suite passes a /data path).
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
#define SYS_nanosleep 35
#define SYS_unlink 87
#define SYS_gettimeofday 96
#define SYS_time 201
#define SYS_clock_gettime 228

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
    out("timetest: ");
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
struct tv {
    int64_t sec, usec;
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

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/tt-file";
    struct st sb;
    long t0, t1;

    out("timetest: M23 clock suite on ");
    out(path);
    out("\n");

    /* 1: time() returns a plausible RTC-seeded epoch */
    t0 = sys_call1(SYS_time, 0);
    report("epoch", t0 > 1700000000L && t0 < 2000000000L);

    /* 2: the pointer form agrees with the return value */
    {
        long stored = 0;
        long r = sys_call1(SYS_time, (long)(uintptr_t)&stored);
        report("time-ptr", r == stored && stored >= t0);
    }

    /* 3: gettimeofday agrees and has a sane usec */
    {
        struct tv v;
        long rc = sys_call2(SYS_gettimeofday,
                            (long)(uintptr_t)&v, 0);
        report("gettimeofday", rc == 0 && v.sec >= t0 &&
               v.sec <= t0 + 5 && v.usec >= 0 && v.usec < 1000000);
    }

    /* 4: clocks -- realtime matches time(); monotonic
     * advances with sleep and stays small (since boot) */
    {
        struct ts rt, m0, m1;
        long rc1 = sys_call2(SYS_clock_gettime, 0,
                             (long)(uintptr_t)&rt);
        long rc2 = sys_call2(SYS_clock_gettime, 1,
                             (long)(uintptr_t)&m0);
        sleep_ms(300);
        long rc3 = sys_call2(SYS_clock_gettime, 1,
                             (long)(uintptr_t)&m1);
        long dms = (long)(m1.sec - m0.sec) * 1000 +
                   (long)(m1.nsec - m0.nsec) / 1000000;
        report("clocks", rc1 == 0 && rc2 == 0 && rc3 == 0 &&
               rt.sec >= t0 && rt.sec <= t0 + 5 &&
               m0.sec >= 0 && m0.sec < 86400 &&
               dms >= 200 && dms <= 3000);
        report("clock-bad", sys_call2(SYS_clock_gettime, 99,
                       (long)(uintptr_t)&rt) == -22);
    }

    /* 5: a fresh file's mtime is "now"; a write 1.2 s
     * later advances it */
    sys_call1(SYS_unlink, (long)(uintptr_t)path);
    t0 = sys_call1(SYS_time, 0);
    long fd = sys_call3(SYS_open, (long)(uintptr_t)path,
                        0x241, 0644); /* WRONLY|CREAT|TRUNC */
    if (fd >= 0) {
        sys_call3(SYS_write, fd, (long)(uintptr_t)"x", 1);
        sys_call1(SYS_close, fd);
    }
    sys_call2(SYS_stat, (long)(uintptr_t)path,
              (long)(uintptr_t)&sb);
    long m_created = (long)sb.mtime;
    /* M29: with independent stamps (yfs v4, ramfs all
     * along), creation sets all three and the first write
     * re-stamps mtime/ctime -- the two operations can
     * straddle a second boundary, so equality becomes a
     * one-second tolerance. (Under the v3 single-stamp
     * alias this check was exact by construction.) */
    int created_ok = fd >= 0 && m_created >= t0 - 1 &&
                     m_created <= t0 + 3 &&
                     sb.atime >= sb.mtime - 1 &&
                     sb.atime <= sb.mtime &&
                     sb.ctime >= sb.mtime &&
                     sb.ctime <= sb.mtime + 1;
    sleep_ms(1200);
    fd = sys_call3(SYS_open, (long)(uintptr_t)path, 0x401,
                   0644); /* WRONLY|APPEND */
    if (fd >= 0) {
        sys_call3(SYS_write, fd, (long)(uintptr_t)"y", 1);
        sys_call1(SYS_close, fd);
    }
    sys_call2(SYS_stat, (long)(uintptr_t)path,
              (long)(uintptr_t)&sb);
    t1 = sys_call1(SYS_time, 0);
    report("mtime", created_ok && (long)sb.mtime >= m_created + 1 &&
           (long)sb.mtime <= t1 + 1 && sb.size == 2);
    sys_call1(SYS_unlink, (long)(uintptr_t)path);

    out("TIMETEST DONE\n");
    return fails;
}

/* pidtest.c - M28 acceptance: /proc/<pid> subdirectories.
 *
 *  root-list:    getdents(/proc) shows our own pid as a
 *                DT_DIR entry.
 *  self-status:  /proc/<me>/status has our exact Pid,
 *                Name pidtest, a PPid and a State line.
 *  self-stat:    the stat line starts with our pid and
 *                (pidtest) and a state letter.
 *  child-status: a sleeping child's status shows our
 *                pid as its PPid.
 *  zombie:       an unreaped dead child is still listed
 *                with State Z; after the reap it is gone.
 *  dead-pid:     opening a reaped pid (dir and status)
 *                is ENOENT.
 *  bad-names:    non-numeric heads, unknown pids, bogus
 *                leaves and over-long paths are ENOENT.
 *  piddir-dents: getdents(/proc/<me>) lists status and
 *                stat as regular files.
 *  stat-modes:   the pid dir stats as 0555 DIR; status
 *                stats as 0444 REG with matching size.
 *
 * Freestanding: raw syscalls only. Exit code = failures. */
#include <stdint.h>
#include <stddef.h>

#define SYS_read 0
#define SYS_write 1
#define SYS_open 2
#define SYS_close 3
#define SYS_stat 4
#define SYS_nanosleep 35
#define SYS_getpid 39
#define SYS_clone 56
#define SYS_execve 59
#define SYS_wait4 61
#define SYS_exit_group 231
#define SYS_getdents64 217

#define ENOENT (-2)

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
    out("pidtest: ");
    out(name);
    out(ok ? " PASS\n" : " FAIL\n");
    if (!ok)
        fails++;
}
static int contains(const char *buf, long n, const char *needle)
{
    long i, j;
    long m = (long)slen(needle);
    if (m == 0 || n < m)
        return 0;
    for (i = 0; i + m <= n; i++) {
        for (j = 0; j < m; j++)
            if (buf[i + j] != needle[j])
                break;
        if (j == m)
            return 1;
    }
    return 0;
}
static long read_all(long fd, char *buf, long cap)
{
    long total = 0, r;
    while (total < cap && (r = sys_call3(SYS_read, fd,
                                         (long)(uintptr_t)(buf + total),
                                         cap - total)) > 0)
        total += r;
    return total;
}
/* Build "/proc/<pid><leaf>" into buf. */
static void pid_path(char *buf, long pid, const char *leaf)
{
    int i = 0;
    long v = pid;
    char tmp[20];
    int n = 0;
    const char *p = "/proc/";
    while (*p)
        buf[i++] = *p++;
    if (v == 0)
        tmp[n++] = '0';
    while (v > 0) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n > 0)
        buf[i++] = tmp[--n];
    while (*leaf)
        buf[i++] = *leaf++;
    buf[i] = '\0';
}
static void fmt_pid_line(char *buf, const char *key, long pid)
{
    /* "Key:\t<pid>" */
    int i = 0;
    long v = pid;
    char tmp[20];
    int n = 0;
    while (key[i]) {
        buf[i] = key[i];
        i++;
    }
    buf[i++] = ':';
    buf[i++] = '\t';
    if (v == 0)
        tmp[n++] = '0';
    while (v > 0) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n > 0)
        buf[i++] = tmp[--n];
    buf[i] = '\0';
}
struct ts {
    int64_t sec, nsec;
};
static void sleep_ms(long ms)
{
    struct ts t;
    t.sec = ms / 1000;
    t.nsec = (ms % 1000) * 1000000;
    sys_call3(SYS_nanosleep, (long)(uintptr_t)&t, 0, 0);
}
static void die(long code)
{
    sys_call1(SYS_exit_group, code);
    for (;;)
        ;
}
/* Scan a getdents64 buffer for an entry name; returns its
 * d_type or -1 when absent. */
static int dent_type(const char *db, long got, const char *name)
{
    long off = 0;
    while (off + 19 <= got) {
        uint16_t reclen = *(const uint16_t *)(db + off + 16);
        const char *nm = db + off + 19;
        uint64_t j = 0;
        if (reclen < 19)
            break;
        while (name[j] != '\0' && nm[j] == name[j])
            j++;
        if (name[j] == '\0' && nm[j] == '\0')
            return db[off + 18];
        off += reclen;
    }
    return -1;
}
static long read_dents(const char *path, char *db, long cap)
{
    long fd = sys_call3(SYS_open, (long)(uintptr_t)path, 0, 0);
    long got = 0, r;
    if (fd < 0)
        return -1;
    while (got < cap &&
           (r = sys_call3(SYS_getdents64, fd,
                          (long)(uintptr_t)(db + got),
                          cap - got)) > 0)
        got += r;
    sys_call1(SYS_close, fd);
    return got;
}
static long read_file(const char *path, char *buf, long cap)
{
    long fd = sys_call3(SYS_open, (long)(uintptr_t)path, 0, 0);
    long n;
    if (fd < 0)
        return fd;
    n = read_all(fd, buf, cap);
    sys_call1(SYS_close, fd);
    return n;
}

int main(int argc, char **argv)
{
    long me = sys_call1(SYS_getpid, 0);
    /* M34: the exec'd child in the environ-child scenario
     * just has to stay alive with its env on display. */
    if (argc > 1 && argv[1][0] == 'e' && argv[1][1] == 'n' &&
        argv[1][2] == 'v' && argv[1][3] == 'c' &&
        argv[1][4] == 'h' && argv[1][5] == 'i' &&
        argv[1][6] == 'l' && argv[1][7] == 'd' &&
        argv[1][8] == '\0') {
        sleep_ms(2500);
        return 0;
    }
    char buf[1024];
    char path[64];
    char key[32];
    long n;

    out("pidtest: M28 per-pid /proc suite\n");

    /* root-list */
    {
        char db[1024];
        char num[16];
        long v = me;
        int i = 0;
        char tmp[20];
        int m = 0;
        long got = read_dents("/proc", db, sizeof(db));
        if (v == 0)
            tmp[m++] = '0';
        while (v > 0) {
            tmp[m++] = (char)('0' + (v % 10));
            v /= 10;
        }
        while (m > 0)
            num[i++] = tmp[--m];
        num[i] = '\0';
        report("root-list", got > 0 &&
               dent_type(db, got, num) == 4 && /* DT_DIR */
               dent_type(db, got, "uptime") == 8); /* DT_REG */
    }

    /* self-status */
    pid_path(path, me, "/status");
    n = read_file(path, buf, sizeof(buf) - 1);
    if (n > 0)
        buf[n] = '\0';
    fmt_pid_line(key, "Pid", me);
    report("self-status", n > 0 &&
           contains(buf, n, key) &&
           contains(buf, n, "Name:\tpidtest") &&
           contains(buf, n, "PPid:\t") &&
           contains(buf, n, "State:\t"));

    /* self-stat */
    pid_path(path, me, "/stat");
    n = read_file(path, buf, sizeof(buf) - 1);
    if (n > 0)
        buf[n] = '\0';
    {
        /* the line must start with the pid digits and
         * contain (pidtest) as the comm field */
        char num[16];
        long v = me;
        int i = 0;
        char tmp[20];
        int m = 0;
        if (v == 0)
            tmp[m++] = '0';
        while (v > 0) {
            tmp[m++] = (char)('0' + (v % 10));
            v /= 10;
        }
        while (m > 0)
            num[i++] = tmp[--m];
        num[i] = '\0';
        report("self-stat", n > 0 &&
               contains(buf, n, "(pidtest)") &&
               contains(buf, n, num) &&
               buf[0] == num[0]);
    }

    /* child-status + zombie + dead-pid */
    {
        long c = sys_call4(SYS_clone, 17, 0, 0, 0);
        int ok_child = 0, ok_zombie = 0, ok_dead = 0;
        if (c == 0) {
            sleep_ms(1200);
            die(0);
        }
        sleep_ms(400); /* child is inside its sleep now */
        pid_path(path, c, "/status");
        n = read_file(path, buf, sizeof(buf) - 1);
        if (n > 0)
            buf[n] = '\0';
        fmt_pid_line(key, "PPid", me);
        ok_child = n > 0 && contains(buf, n, key) &&
                   contains(buf, n, "Name:\tpidtest");
        /* wait for exit but do NOT reap: zombie window */
        sleep_ms(1200);
        n = read_file(path, buf, sizeof(buf) - 1);
        if (n > 0)
            buf[n] = '\0';
        ok_zombie = n > 0 && contains(buf, n, "State:\tZ");
        {
            long st = 0;
            sys_call4(SYS_wait4, c, (long)(uintptr_t)&st, 0, 0);
        }
        {
            long fd = sys_call3(SYS_open,
                                (long)(uintptr_t)path, 0, 0);
            long fd2;
            pid_path(path, c, "");
            fd2 = sys_call3(SYS_open, (long)(uintptr_t)path,
                            0, 0);
            ok_dead = fd == ENOENT && fd2 == ENOENT;
        }
        report("child-status", ok_child);
        report("zombie", ok_zombie);
        report("dead-pid", ok_dead);
    }

    /* bad-names */
    {
        long a = sys_call3(SYS_open,
                           (long)(uintptr_t)"/proc/abc/status",
                           0, 0);
        long b = sys_call3(SYS_open,
                           (long)(uintptr_t)"/proc/99999/status",
                           0, 0);
        pid_path(path, me, "/bogus");
        {
            long cfd = sys_call3(SYS_open,
                                 (long)(uintptr_t)path, 0, 0);
            pid_path(path, me, "/status/extra");
            {
                long d = sys_call3(SYS_open,
                                   (long)(uintptr_t)path, 0, 0);
                report("bad-names", a == ENOENT && b == ENOENT &&
                       cfd == ENOENT && d == ENOENT);
            }
        }
    }

    /* piddir-dents */
    {
        char db[512];
        pid_path(path, me, "");
        {
            long got = read_dents(path, db, sizeof(db));
            report("piddir-dents", got > 0 &&
                   dent_type(db, got, "status") == 8 &&
                   dent_type(db, got, "stat") == 8 &&
                   dent_type(db, got, "cmdline") == 8 &&
                   dent_type(db, got, "environ") == 8 &&
                   dent_type(db, got, ".") == 4);
        }
    }

    /* stat-modes */
    {
        struct st sb;
        int ok;
        pid_path(path, me, "");
        ok = sys_call2(SYS_stat, (long)(uintptr_t)path,
                       (long)(uintptr_t)&sb) == 0 &&
             (sb.mode & 0170000u) == 0040000u &&
             (sb.mode & 0777u) == 0555u;
        pid_path(path, me, "/status");
        n = read_file(path, buf, sizeof(buf));
        ok = ok && sys_call2(SYS_stat, (long)(uintptr_t)path,
                             (long)(uintptr_t)&sb) == 0 &&
             (sb.mode & 0170000u) == 0100000u &&
             (sb.mode & 0777u) == 0444u &&
             (long)sb.size == n;
        report("stat-modes", ok);
    }

    /* M31: /proc/self resolves to the caller */
    n = read_file("/proc/self/status", buf, sizeof(buf) - 1);
    if (n > 0)
        buf[n] = '\0';
    fmt_pid_line(key, "Pid", me);
    {
        char db[512];
        long got = read_dents("/proc/self", db, sizeof(db));
        report("self-link", n > 0 && contains(buf, n, key) &&
               got > 0 && dent_type(db, got, "status") == 8);
    }

    /* M31: cmdline is the exec argv, NUL-joined */
    n = read_file("/proc/self/cmdline", buf, sizeof(buf));
    report("cmdline-self", n == 8 &&
           buf[0] == 'p' && buf[1] == 'i' && buf[2] == 'd' &&
           buf[3] == 't' && buf[4] == 'e' && buf[5] == 's' &&
           buf[6] == 't' && buf[7] == '\0');

    /* M31: a forked child inherits the cmdline */
    {
        long c = sys_call4(SYS_clone, 17, 0, 0, 0);
        int ok = 0;
        if (c == 0) {
            sleep_ms(600);
            die(0);
        }
        sleep_ms(200);
        pid_path(path, c, "/cmdline");
        n = read_file(path, buf, sizeof(buf));
        ok = n == 8 && buf[0] == 'p' && buf[6] == 't' &&
             buf[7] == '\0';
        {
            long st = 0;
            sys_call4(SYS_wait4, c, (long)(uintptr_t)&st, 0, 0);
        }
        report("cmdline-child", ok);
    }

    /* M34: environ is the exec envp, NUL-joined. The
     * shell execs us with an empty env, so ours is empty;
     * a child exec'd with K1/K2 shows exactly those. */
    n = read_file("/proc/self/environ", buf, sizeof(buf));
    report("environ-self", n == 0);
    {
        long c = sys_call4(SYS_clone, 17, 0, 0, 0);
        int ok = 0;
        if (c == 0) {
            static const char *cargv[3];
            static const char *cenv[3];
            cargv[0] = "pidtest";
            cargv[1] = "envchild";
            cargv[2] = 0;
            cenv[0] = "K1=V1";
            cenv[1] = "K2=V2";
            cenv[2] = 0;
            sys_call3(SYS_execve,
                      (long)(uintptr_t)"/bin/pidtest",
                      (long)(uintptr_t)cargv,
                      (long)(uintptr_t)cenv);
            die(90);
        }
        sleep_ms(300);
        pid_path(path, c, "/environ");
        n = read_file(path, buf, sizeof(buf));
        ok = n == 12 && buf[0] == 'K' && buf[1] == '1' &&
             buf[2] == '=' && buf[3] == 'V' && buf[4] == '1' &&
             buf[5] == '\0' && buf[6] == 'K' && buf[7] == '2' &&
             buf[8] == '=' && buf[9] == 'V' &&
             buf[10] == '2' && buf[11] == '\0';
        {
            long st = 0;
            sys_call4(SYS_wait4, c, (long)(uintptr_t)&st, 0, 0);
        }
        report("environ-child", ok);
    }

    out("PIDTEST DONE\n");
    return fails;
}

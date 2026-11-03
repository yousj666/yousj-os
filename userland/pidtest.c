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
#define SYS_lstat 6
#define SYS_chdir 80
#define SYS_readlink 89
#define SYS_nanosleep 35
#define SYS_getpid 39
#define SYS_clone 56
#define SYS_pipe 22
#define SYS_lseek 8
#define SYS_mmap 9
#define SYS_munmap 11
#define SYS_execve 59
#define SYS_wait4 61
#define SYS_exit_group 231
#define SYS_getdents64 217
#define SYS_ioctl 16
#define SYS_kill 62
#define SYS_getpgrp 111
#define SYS_setsid 112
#define SYS_getpgid 121
#define SYS_getsid 124
#define SYS_setpgid 109

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

static long sys_call6(long n, long a, long b, long c, long d,
                      long e, long f)
{
    long r;
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10),
                       "r"(r8), "r"(r9)
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
                   dent_type(db, got, "cwd") == 10 &&
                   dent_type(db, got, "exe") == 10 &&
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

    /* M35: cwd and exe are magic links. */
    {
        char lb[128];
        struct st sb;
        long ln = sys_call3(SYS_readlink,
                            (long)(uintptr_t)"/proc/self/exe",
                            (long)(uintptr_t)lb, sizeof(lb));
        int ok = ln == 12 && lb[0] == '/' && lb[5] == 'p' &&
                 lb[11] == 't';
        sys_call1(SYS_chdir, (long)(uintptr_t)"/bin");
        ln = sys_call3(SYS_readlink,
                       (long)(uintptr_t)"/proc/self/cwd",
                       (long)(uintptr_t)lb, sizeof(lb));
        ok = ok && ln == 4 && lb[0] == '/' && lb[1] == 'b' &&
             lb[2] == 'i' && lb[3] == 'n';
        ok = ok &&
            sys_call3(SYS_lstat,
                      (long)(uintptr_t)"/proc/self/cwd",
                      (long)(uintptr_t)&sb, 0) == 0 &&
            (sb.mode & 0170000) == 0120000;
        ok = ok &&
            sys_call3(SYS_stat,
                      (long)(uintptr_t)"/proc/self/cwd",
                      (long)(uintptr_t)&sb, 0) == 0 &&
            (sb.mode & 0170000) == 0040000;
        ok = ok &&
            sys_call3(SYS_stat,
                      (long)(uintptr_t)"/proc/self/exe",
                      (long)(uintptr_t)&sb, 0) == 0 &&
            (sb.mode & 0170000) == 0100000;
        /* opening cwd opens the directory itself */
        {
            char db[512];
            long fd = sys_call3(SYS_open,
                                (long)(uintptr_t)"/proc/self/cwd",
                                0, 0);
            long got = -1;
            if (fd >= 0) {
                got = sys_call3(SYS_getdents64, fd,
                                (long)(uintptr_t)db,
                                sizeof(db));
                sys_call1(SYS_close, fd);
            }
            ok = ok && got > 0 &&
                 dent_type(db, got, "pidtest") == 8;
        }
        /* a generated file is not a link */
        ok = ok && sys_call3(SYS_readlink,
                             (long)(uintptr_t)"/proc/self/status",
                             (long)(uintptr_t)lb,
                             sizeof(lb)) == -22;
        sys_call1(SYS_chdir, (long)(uintptr_t)"/");
        report("cwd-exe", ok);
    }

    /* M39: the maps file describes the address space. */
    {
        static char mb[2048];
        long maddr = sys_call6(SYS_mmap, 0, 8192, 3, 0x22,
                               -1, 0);
        long fd = sys_call3(SYS_open,
                            (long)(uintptr_t)"/proc/self/maps",
                            0, 0);
        long tot = 0, r;
        if (fd >= 0) {
            while (tot < (long)sizeof(mb) - 1 &&
                   (r = sys_call3(SYS_read, fd,
                                  (long)(uintptr_t)(mb + tot),
                                  sizeof(mb) - 1 - tot)) > 0)
                tot += r;
            sys_call1(SYS_close, fd);
        }
        mb[tot] = '\0';
        report("maps-self",
               maddr == 0x8002000000L && tot > 0 &&
               contains(mb, tot, "/bin/pidtest") &&
               contains(mb, tot, "[stack]") &&
               contains(mb, tot, "8002000000-") &&
               contains(mb, tot, "rw-p"));
        if (maddr > 0)
            sys_call3(SYS_munmap, maddr, 8192, 0);
    }

    /* M41: the fd directory and its links. */
    {
        char lb[300];
        struct st sb;
        long ln, xfd;
        int pfd[2];
        int ok;
        xfd = sys_call3(SYS_open,
                        (long)(uintptr_t)"/bin/pidtest", 0, 0);
        sys_call3(SYS_pipe, (long)(uintptr_t)pfd, 0, 0);
        ln = sys_call3(SYS_readlink,
                       (long)(uintptr_t)"/proc/self/fd/0",
                       (long)(uintptr_t)lb, sizeof(lb));
        ok = ln == 12 && lb[0] == '/' && lb[1] == 'd' &&
             lb[5] == 'c';
        {
            /* M49: the console link's target is a real
             * /dev/console node now -- stat of the link
             * says CHR, and opening the link succeeds
             * (it used to be ENOENT). */
            long cfd = sys_call3(SYS_open,
                                 (long)(uintptr_t)"/proc/self/fd/0",
                                 2 /* O_RDWR */, 0);
            ok = ok && cfd >= 0 &&
                 sys_call3(SYS_stat,
                           (long)(uintptr_t)"/proc/self/fd/0",
                           (long)(uintptr_t)&sb, 0) == 0 &&
                 (sb.mode & 0170000) == 0020000;
            if (cfd >= 0)
                sys_call1(SYS_close, cfd);
        }
        {
            char xp[64];
            const char *pre = "/proc/self/fd/";
            uint64_t j = 0;
            while (pre[j] != '\0') {
                xp[j] = pre[j];
                j++;
            }
            if (xfd >= 10)
                xp[j++] = (char)('0' + xfd / 10);
            xp[j++] = (char)('0' + xfd % 10);
            xp[j] = '\0';
            ln = sys_call3(SYS_readlink,
                           (long)(uintptr_t)xp,
                           (long)(uintptr_t)lb, sizeof(lb));
            ok = ok && ln == 12 && lb[0] == '/' &&
                 lb[5] == 'p';
            ok = ok &&
                 sys_call3(SYS_lstat, (long)(uintptr_t)xp,
                           (long)(uintptr_t)&sb, 0) == 0 &&
                 (sb.mode & 0170000) == 0120000;
            ok = ok &&
                 sys_call3(SYS_stat, (long)(uintptr_t)xp,
                           (long)(uintptr_t)&sb, 0) == 0 &&
                 (sb.mode & 0170000) == 0100000;
        }
        {
            char pp[64];
            const char *pre = "/proc/self/fd/";
            uint64_t j = 0;
            while (pre[j] != '\0') {
                pp[j] = pre[j];
                j++;
            }
            pp[j++] = (char)('0' + pfd[0] % 10);
            pp[j] = '\0';
            ln = sys_call3(SYS_readlink,
                           (long)(uintptr_t)pp,
                           (long)(uintptr_t)lb, sizeof(lb));
            ok = ok && ln > 6 && lb[0] == 'p' &&
                 lb[1] == 'i' && lb[2] == 'p' &&
                 lb[3] == 'e' && lb[4] == ':' &&
                 lb[5] == '[';
        }
        /* the listing itself: fd entry is a directory,
         * the open file appears as a link entry */
        {
            char db[1024];
            long got = read_dents("/proc/self", db,
                                  sizeof(db));
            ok = ok && got > 0 &&
                 dent_type(db, got, "fd") == 4;
            got = read_dents("/proc/self/fd", db,
                             sizeof(db));
            ok = ok && got > 0 &&
                 dent_type(db, got, "0") == 10 &&
                 dent_type(db, got, "1") == 10;
        }
        ok = ok && sys_call3(SYS_open,
                             (long)(uintptr_t)"/proc/self/fd/99",
                             0, 0) == -2;
        if (xfd >= 0)
            sys_call1(SYS_close, xfd);
        sys_call1(SYS_close, pfd[0]);
        sys_call1(SYS_close, pfd[1]);
        report("fd-dir", ok);
    }

    /* M45: fdinfo. */
    {
        char xp[64], ib[256], db[1024];
        const char *pre = "/proc/self/fdinfo/";
        uint64_t j = 0;
        long xfd, n, got;
        int ok;
        while (pre[j] != '\0') {
            xp[j] = pre[j];
            j++;
        }
        xfd = sys_call3(SYS_open,
                        (long)(uintptr_t)"/bin/pidtest", 0, 0);
        if (xfd >= 10)
            xp[j++] = (char)('0' + xfd / 10);
        xp[j++] = (char)('0' + xfd % 10);
        xp[j] = '\0';
        sys_call3(SYS_lseek, xfd, 5, 0);
        n = read_file(xp, ib, sizeof(ib));
        ok = n > 0 && contains(ib, n, "pos:\t5\n") &&
             contains(ib, n, "flags:\t0\n") &&
             contains(ib, n, "mnt_id:\t1\n") &&
             contains(ib, n, "ino:\t");
        got = read_dents("/proc/self/fdinfo", db,
                         sizeof(db));
        ok = ok && got > 0 &&
             dent_type(db, got, "0") == 8;
        got = read_dents("/proc/self", db, sizeof(db));
        ok = ok && got > 0 &&
             dent_type(db, got, "fdinfo") == 4;
        ok = ok && sys_call3(SYS_open,
                             (long)(uintptr_t)
                                 "/proc/self/fdinfo/99",
                             0, 0) == -2;
        if (xfd >= 0)
            sys_call1(SYS_close, xfd);
        report("fdinfo", ok);
    }

    /* M64: vfork. The child borrows this address space
     * (its writes are our writes) and the parent sleeps
     * until the child execs or exits -- so when clone
     * returns here, the flag the child sets just before
     * exec MUST already be set, deterministically. The
     * child never returns past the clone call site: it
     * execs or dies, as vfork children must. */
    {
        static volatile long vf_flag;
        long c, st = 0;
        int ok;
        vf_flag = 0;
        c = sys_call4(SYS_clone,
                      0x4000 | 0x100 | 17, /* VFORK|VM|SIGCHLD */
                      0, 0, 0);
        if (c == 0) {
            static const char *hav[2];
            hav[0] = "hello";
            hav[1] = 0;
            sleep_ms(300);
            vf_flag = 1;
            sys_call3(SYS_execve,
                      (long)(uintptr_t)"/bin/hello",
                      (long)(uintptr_t)hav, 0);
            die(90); /* exec failed */
        }
        ok = c > 0 && vf_flag == 1;
        sys_call4(SYS_wait4, c, (long)(uintptr_t)&st, 0, 0);
        /* /bin/hello exits 7 by design (its main returns
         * 7); that constant is the exec-success witness. */
        ok = ok && ((st >> 8) & 0xFF) == 7;
        /* part 2: exit without exec also releases us */
        vf_flag = 0;
        c = sys_call4(SYS_clone, 0x4000 | 0x100 | 17, 0, 0,
                      0);
        if (c == 0) {
            vf_flag = 7;
            die(3);
        }
        st = 0;
        sys_call4(SYS_wait4, c, (long)(uintptr_t)&st, 0, 0);
        ok = ok && c > 0 && vf_flag == 7 &&
             ((st >> 8) & 0xFF) == 3;
        report("vfork", ok);
    }

    /* M65: process groups and sessions. We start as a
     * member of the shell's group; setsid (we are not the
     * leader) makes us session+group leader, a second
     * setsid must fail. Children can be moved between
     * groups, kill(-pgid) takes a whole group down, and
     * wait4 accepts the group forms 0 and -pgid. */
    {
        long me = sys_call1(SYS_getpid, 0);
        long sid, c1, c2, st = 0;
        int ok = 1;
        int fg = 0;

        ok = ok && sys_call1(SYS_getpgrp, 0) ==
                       sys_call2(SYS_getpgid, 0, 0);
        sid = sys_call1(SYS_setsid, 0);
        ok = ok && sid == me &&
             sys_call2(SYS_getpgid, 0, 0) == me &&
             sys_call2(SYS_getsid, 0, 0) == me;
        ok = ok && sys_call1(SYS_setsid, 0) == -1; /* leader */
        ok = ok && sys_call2(SYS_getpgid, 99999, 0) == -3;

        /* move a child into its own group and back */
        c1 = sys_call4(SYS_clone, 17, 0, 0, 0);
        if (c1 == 0) {
            sleep_ms(1500);
            die(0);
        }
        ok = ok && c1 > 0 &&
             sys_call3(SYS_setpgid, c1, c1, 0) == 0 &&
             sys_call2(SYS_getpgid, c1, 0) == c1 &&
             sys_call3(SYS_setpgid, c1, me, 0) == 0 &&
             sys_call2(SYS_getpgid, c1, 0) == me;
        sys_call4(SYS_wait4, c1, (long)(uintptr_t)&st, 0, 0);

        /* kill(-pgid) kills the whole group by signal */
        c1 = sys_call4(SYS_clone, 17, 0, 0, 0);
        if (c1 == 0) {
            sleep_ms(1500);
            die(0);
        }
        c2 = sys_call4(SYS_clone, 17, 0, 0, 0);
        if (c2 == 0) {
            sleep_ms(1500);
            die(0);
        }
        ok = ok && c1 > 0 && c2 > 0 &&
             sys_call3(SYS_setpgid, c1, c1, 0) == 0 &&
             sys_call3(SYS_setpgid, c2, c1, 0) == 0 &&
             sys_call3(SYS_kill, -c1, 9, 0) == 0;
        st = 0;
        sys_call4(SYS_wait4, c1, (long)(uintptr_t)&st, 0, 0);
        ok = ok && (st & 0x7F) == 9;
        st = 0;
        sys_call4(SYS_wait4, c2, (long)(uintptr_t)&st, 0, 0);
        ok = ok && (st & 0x7F) == 9;
        ok = ok && sys_call3(SYS_kill, -c1, 0, 0) == -3;

        /* wait4(-pgid): reap a child by its group */
        c1 = sys_call4(SYS_clone, 17, 0, 0, 0);
        if (c1 == 0) {
            sleep_ms(200);
            die(5);
        }
        ok = ok && c1 > 0 &&
             sys_call3(SYS_setpgid, c1, c1, 0) == 0;
        st = 0;
        ok = ok &&
             sys_call4(SYS_wait4, -c1, (long)(uintptr_t)&st,
                       0, 0) == c1 &&
             ((st >> 8) & 0xFF) == 5;

        /* wait4(0): any child in OUR group */
        c1 = sys_call4(SYS_clone, 17, 0, 0, 0);
        if (c1 == 0)
            die(6);
        st = 0;
        ok = ok && c1 > 0 &&
             sys_call4(SYS_wait4, 0, (long)(uintptr_t)&st,
                       0, 0) == c1 &&
             ((st >> 8) & 0xFF) == 6;

        /* console foreground group round-trips */
        ok = ok &&
             sys_call4(SYS_ioctl, 0, 0x540F,
                       (long)(uintptr_t)&fg, 0) == 0 && fg >= 1;
        fg = (int)me;
        ok = ok &&
             sys_call4(SYS_ioctl, 0, 0x5410, /* TIOCSPGRP */
                       (long)(uintptr_t)&fg, 0) == 0;
        fg = 0;
        ok = ok &&
             sys_call4(SYS_ioctl, 0, 0x540F,
                       (long)(uintptr_t)&fg, 0) == 0 &&
             fg == (int)me;
        report("groups", ok);
    }

    /* M66: WUNTRACED / WCONTINUED. The child stops
     * itself with SIGSTOP; the parent's untraced wait
     * must surface the stop exactly once, then the
     * SIGCONT release surfaces as 0xFFFF, then the
     * child exits normally. */
    {
        long c, st = 0, r;
        int ok = 1;
        c = sys_call4(SYS_clone, 17, 0, 0, 0);
        if (c == 0) {
            long self = sys_call1(SYS_getpid, 0);
            sleep_ms(200);
            sys_call3(SYS_kill, self, 19, 0); /* SIGSTOP */
            die(4); /* reached only after SIGCONT */
        }
        r = sys_call4(SYS_wait4, c, (long)(uintptr_t)&st, 2,
                      0); /* WUNTRACED */
        ok = ok && r == c && (st & 0xFF) == 0x7F &&
             ((st >> 8) & 0xFF) == 19;
        /* already reported: a WNOHANG re-check finds none */
        st = 0;
        r = sys_call4(SYS_wait4, c, (long)(uintptr_t)&st,
                      2 | 1, 0); /* WUNTRACED|WNOHANG */
        ok = ok && r == 0;
        sys_call3(SYS_kill, c, 18, 0); /* SIGCONT */
        st = 0;
        r = sys_call4(SYS_wait4, c, (long)(uintptr_t)&st, 8,
                      0); /* WCONTINUED */
        ok = ok && r == c && st == 0xFFFF;
        st = 0;
        r = sys_call4(SYS_wait4, c, (long)(uintptr_t)&st, 0,
                      0);
        ok = ok && r == c && ((st >> 8) & 0xFF) == 4;
        report("stopcont", ok);
    }

    out("PIDTEST DONE\n");
    return fails;
}

/* dirfdtest.c - M30 acceptance: dirfd-relative calls.
 *
 *   dirfdtest [base]   (default "/": work in <base>/dfdir)
 *
 *  openat-rel:   openat(dirfd, name) reads the file.
 *  openat-sub:   a nested relative path resolves too.
 *  fdcwd:        AT_FDCWD still means the process cwd.
 *  statat-rel:   newfstatat(dirfd, name) stats the file;
 *                NOFOLLOW reports a symlink as a link.
 *  utimensat-rel: stamps set via dirfd land on the file.
 *  abs-ignores:  an absolute path ignores even a bogus
 *                dirfd.
 *  bad-dfd:      a bogus dirfd is EBADF everywhere.
 *  notdir:       a file fd as dirfd is ENOTDIR.
 *  unlinkat-rel: unlinkat(dirfd, name) removes the file.
 *  symlinkat-rel: the link is created in the dirfd's dir
 *                and reads through.
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
#define SYS_unlink 87
#define SYS_rmdir 84
#define SYS_chdir 80
#define SYS_mkdir 83
#define SYS_symlink 88
#define SYS_openat 257
#define SYS_newfstatat 262
#define SYS_unlinkat 263
#define SYS_symlinkat 266
#define SYS_utimensat 280
#define SYS_access 21
#define SYS_mkdirat 258
#define SYS_renameat 264
#define SYS_linkat 265
#define SYS_readlinkat 267
#define SYS_faccessat 269
#define SYS_renameat2 316
#define SYS_faccessat2 439

#define AT_FDCWD (-100)
#define AT_SYMLINK_NOFOLLOW 0x100

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
static long sys_call5(long n, long a, long b, long c, long d,
                      long e)
{
    long r;
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10),
                       "r"(r8)
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
    out("dirfdtest: ");
    out(name);
    out(ok ? " PASS\n" : " FAIL\n");
    if (!ok)
        fails++;
}
static void join(char *dst, const char *a, const char *b)
{
    uint64_t i = 0, j;
    for (j = 0; a[j]; j++)
        dst[i++] = a[j];
    if (i == 0 || dst[i - 1] != '/')
        dst[i++] = '/';
    for (j = 0; b[j]; j++)
        dst[i++] = b[j];
    dst[i] = '\0';
}
static long mkfile(const char *path, const char *content)
{
    long fd = sys_call3(SYS_open, (long)(uintptr_t)path,
                        0x241, 0644); /* WRONLY|CREAT|TRUNC */
    if (fd >= 0) {
        sys_call3(SYS_write, fd, (long)(uintptr_t)content,
                  (long)slen(content));
        sys_call1(SYS_close, fd);
    }
    return fd;
}
static long read_fd(long fd, char *buf, long cap)
{
    long total = 0, r;
    while (total < cap && (r = sys_call3(SYS_read, fd,
                                         (long)(uintptr_t)(buf + total),
                                         cap - total)) > 0)
        total += r;
    return total;
}
static int streq(const char *a, const char *b, long n)
{
    long i;
    if ((long)slen(b) != n)
        return 0;
    for (i = 0; i < n; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

int main(int argc, char **argv)
{
    const char *base = argc > 1 ? argv[1] : "";
    char dir[128], p[160], buf[64];
    struct st sb;
    long dfd, fd;

    out("dirfdtest: M30 dirfd suite on ");
    out(base[0] ? base : "/");
    out("\n");

    join(dir, base, "dfdir");
    /* clean slate, then build the tree by absolute path */
    join(p, dir, "sub/g.txt");
    sys_call1(SYS_unlink, (long)(uintptr_t)p);
    join(p, dir, "f.txt");
    sys_call1(SYS_unlink, (long)(uintptr_t)p);
    join(p, dir, "ln");
    sys_call1(SYS_unlink, (long)(uintptr_t)p);
    join(p, dir, "ln2");
    sys_call1(SYS_unlink, (long)(uintptr_t)p);
    join(p, dir, "del.txt");
    sys_call1(SYS_unlink, (long)(uintptr_t)p);
    join(p, dir, "r.txt");
    sys_call1(SYS_unlink, (long)(uintptr_t)p);
    join(p, dir, "r2.txt");
    sys_call1(SYS_unlink, (long)(uintptr_t)p);
    join(p, dir, "r3.txt");
    sys_call1(SYS_unlink, (long)(uintptr_t)p);
    join(p, dir, "ln3");
    sys_call1(SYS_unlink, (long)(uintptr_t)p);
    join(p, dir, "sub2");
    sys_call1(SYS_rmdir, (long)(uintptr_t)p);
    join(p, dir, "sub");
    sys_call1(SYS_mkdir, (long)(uintptr_t)dir);
    join(p, dir, "sub");
    sys_call1(SYS_mkdir, (long)(uintptr_t)p);
    join(p, dir, "f.txt");
    mkfile(p, "dirfd-data");
    join(p, dir, "sub/g.txt");
    mkfile(p, "nested");
    join(p, dir, "del.txt");
    mkfile(p, "bye");

    dfd = sys_call3(SYS_open, (long)(uintptr_t)dir, 0, 0);
    if (dfd < 0) {
        report("open-dirfd", 0);
        out("DIRFDTEST DONE\n");
        return fails + 1;
    }
    /* work from a different cwd: the dirfd must carry us */
    sys_call1(SYS_chdir, (long)(uintptr_t)"/");

    /* openat-rel */
    fd = sys_call4(SYS_openat, dfd, (long)(uintptr_t)"f.txt",
                   0, 0);
    {
        long n = fd >= 0 ? read_fd(fd, buf, sizeof(buf)) : -1;
        if (fd >= 0)
            sys_call1(SYS_close, fd);
        report("openat-rel", n == 10 &&
               streq(buf, "dirfd-data", n));
    }

    /* openat-sub */
    fd = sys_call4(SYS_openat, dfd,
                   (long)(uintptr_t)"sub/g.txt", 0, 0);
    {
        long n = fd >= 0 ? read_fd(fd, buf, sizeof(buf)) : -1;
        if (fd >= 0)
            sys_call1(SYS_close, fd);
        report("openat-sub", n == 6 && streq(buf, "nested", n));
    }

    /* fdcwd: chdir into the dir, AT_FDCWD resolves there */
    sys_call1(SYS_chdir, (long)(uintptr_t)dir);
    fd = sys_call4(SYS_openat, AT_FDCWD,
                   (long)(uintptr_t)"f.txt", 0, 0);
    if (fd >= 0)
        sys_call1(SYS_close, fd);
    sys_call1(SYS_chdir, (long)(uintptr_t)"/");
    report("fdcwd", fd >= 0);

    /* statat-rel (+ NOFOLLOW on a link made by absolute path) */
    join(p, dir, "ln");
    sys_call2(SYS_symlink, (long)(uintptr_t)"f.txt",
              (long)(uintptr_t)p);
    {
        long rc = sys_call4(SYS_newfstatat, dfd,
                            (long)(uintptr_t)"f.txt",
                            (long)(uintptr_t)&sb, 0);
        int ok = rc == 0 && sb.size == 10 &&
                 (sb.mode & 0170000u) == 0100000u;
        rc = sys_call4(SYS_newfstatat, dfd,
                       (long)(uintptr_t)"ln",
                       (long)(uintptr_t)&sb,
                       AT_SYMLINK_NOFOLLOW);
        ok = ok && rc == 0 &&
             (sb.mode & 0170000u) == 0120000u && sb.size == 5;
        report("statat-rel", ok);
    }

    /* utimensat-rel */
    {
        struct ts ts2[2];
        long rc;
        ts2[0].sec = 765432100; ts2[0].nsec = 0;
        ts2[1].sec = 0;         ts2[1].nsec = 1073741822; /* OMIT */
        rc = sys_call4(SYS_utimensat, dfd,
                       (long)(uintptr_t)"f.txt",
                       (long)(uintptr_t)ts2, 0);
        sys_call4(SYS_newfstatat, dfd,
                  (long)(uintptr_t)"f.txt",
                  (long)(uintptr_t)&sb, 0);
        report("utimensat-rel", rc == 0 &&
               sb.atime == 765432100);
    }

    /* abs-ignores: bogus dirfd, absolute path */
    join(p, dir, "f.txt");
    fd = sys_call4(SYS_openat, 9999, (long)(uintptr_t)p, 0, 0);
    if (fd >= 0)
        sys_call1(SYS_close, fd);
    report("abs-ignores", fd >= 0);

    /* bad-dfd */
    {
        long a = sys_call4(SYS_openat, 9999,
                           (long)(uintptr_t)"f.txt", 0, 0);
        long b = sys_call4(SYS_newfstatat, 9999,
                           (long)(uintptr_t)"f.txt",
                           (long)(uintptr_t)&sb, 0);
        long c = sys_call3(SYS_unlinkat, 9999,
                           (long)(uintptr_t)"f.txt", 0);
        report("bad-dfd", a == -9 && b == -9 && c == -9);
    }

    /* notdir: a regular file fd as the base */
    {
        long ffd = sys_call4(SYS_openat, dfd,
                             (long)(uintptr_t)"f.txt", 0, 0);
        long a = ffd >= 0 ? sys_call4(SYS_openat, ffd,
                                      (long)(uintptr_t)"x",
                                      0, 0) : -1;
        long b = ffd >= 0 ? sys_call4(SYS_newfstatat, ffd,
                                      (long)(uintptr_t)"x",
                                      (long)(uintptr_t)&sb,
                                      0) : -1;
        if (ffd >= 0)
            sys_call1(SYS_close, ffd);
        report("notdir", a == -20 && b == -20); /* ENOTDIR */
    }

    /* unlinkat-rel */
    {
        long rc = sys_call3(SYS_unlinkat, dfd,
                            (long)(uintptr_t)"del.txt", 0);
        long rc2 = sys_call4(SYS_newfstatat, dfd,
                             (long)(uintptr_t)"del.txt",
                             (long)(uintptr_t)&sb, 0);
        report("unlinkat-rel", rc == 0 && rc2 == -2);
    }

    /* symlinkat-rel */
    {
        long rc = sys_call3(SYS_symlinkat,
                            (long)(uintptr_t)"f.txt", dfd,
                            (long)(uintptr_t)"ln2");
        fd = rc == 0 ? sys_call4(SYS_openat, dfd,
                                 (long)(uintptr_t)"ln2", 0, 0)
                     : -1;
        long n = fd >= 0 ? read_fd(fd, buf, sizeof(buf)) : -1;
        if (fd >= 0)
            sys_call1(SYS_close, fd);
        report("symlinkat-rel", rc == 0 && n == 10 &&
               streq(buf, "dirfd-data", n));
    }

    /* --- M33: the rest of the *at family --- */

    /* mkdirat-rel */
    {
        long rc = sys_call3(SYS_mkdirat, dfd,
                            (long)(uintptr_t)"sub2", 0755);
        long a = sys_call4(SYS_newfstatat, dfd,
                           (long)(uintptr_t)"sub2",
                           (long)(uintptr_t)&sb, 0);
        report("mkdirat-rel", rc == 0 && a == 0 &&
               (sb.mode & 0170000) == 0040000);
    }

    /* renameat-rel */
    join(p, dir, "r.txt");
    mkfile(p, "rr");
    {
        long rc = sys_call4(SYS_renameat, dfd,
                            (long)(uintptr_t)"r.txt", dfd,
                            (long)(uintptr_t)"r2.txt");
        long a = sys_call4(SYS_newfstatat, dfd,
                           (long)(uintptr_t)"r2.txt",
                           (long)(uintptr_t)&sb, 0);
        long b = sys_call4(SYS_newfstatat, dfd,
                           (long)(uintptr_t)"r.txt",
                           (long)(uintptr_t)&sb, 0);
        report("renameat-rel", rc == 0 && a == 0 && b == -2);
    }

    /* renameat2 NOREPLACE: existing target refuses */
    {
        long a = sys_call5(SYS_renameat2, dfd,
                           (long)(uintptr_t)"r2.txt", dfd,
                           (long)(uintptr_t)"f.txt", 1);
        long b = sys_call5(SYS_renameat2, dfd,
                           (long)(uintptr_t)"r2.txt", dfd,
                           (long)(uintptr_t)"r3.txt", 1);
        report("renameat2-noreplace", a == -17 && b == 0);
    }

    /* readlinkat-rel */
    sys_call3(SYS_symlinkat, (long)(uintptr_t)"f.txt", dfd,
              (long)(uintptr_t)"ln3");
    {
        long n = sys_call4(SYS_readlinkat, dfd,
                           (long)(uintptr_t)"ln3",
                           (long)(uintptr_t)buf, sizeof(buf));
        report("readlinkat-rel", n == 5 && streq(buf, "f.txt", n));
    }

    /* faccessat-rel + faccessat2 + access */
    {
        long a = sys_call4(SYS_faccessat, dfd,
                           (long)(uintptr_t)"f.txt", 0, 0);
        long b = sys_call4(SYS_faccessat, dfd,
                           (long)(uintptr_t)"f.txt", 4, 0);
        long c = sys_call4(SYS_faccessat, dfd,
                           (long)(uintptr_t)"f.txt", 1, 0);
        long d = sys_call4(SYS_faccessat, dfd,
                           (long)(uintptr_t)"ghost", 0, 0);
        long e = sys_call4(SYS_faccessat2, dfd,
                           (long)(uintptr_t)"ln3", 4, 0x100);
        join(p, dir, "f.txt");
        long g = sys_call3(SYS_access, (long)(uintptr_t)p,
                           0, 0);
        report("faccessat-rel", a == 0 && b == 0 && c == -13 &&
               d == -2 && e == 0 && g == 0);
    }

    /* linkat: hard links are -EPERM by design (M33) */
    {
        long rc = sys_call5(SYS_linkat, dfd,
                            (long)(uintptr_t)"f.txt", dfd,
                            (long)(uintptr_t)"hard", 0);
        report("linkat-eperm", rc == -1);
    }

    sys_call1(SYS_close, dfd);

    /* teardown: remove the whole tree so a second run in
     * the same boot starts from the same state (and the
     * harness's free-page comparison stays meaningful) */
    join(p, dir, "r3.txt");
    sys_call1(SYS_unlink, (long)(uintptr_t)p);
    join(p, dir, "ln3");
    sys_call1(SYS_unlink, (long)(uintptr_t)p);
    join(p, dir, "sub2");
    sys_call1(SYS_rmdir, (long)(uintptr_t)p);
    join(p, dir, "f.txt");
    sys_call1(SYS_unlink, (long)(uintptr_t)p);
    join(p, dir, "ln");
    sys_call1(SYS_unlink, (long)(uintptr_t)p);
    join(p, dir, "ln2");
    sys_call1(SYS_unlink, (long)(uintptr_t)p);
    join(p, dir, "sub/g.txt");
    sys_call1(SYS_unlink, (long)(uintptr_t)p);
    join(p, dir, "sub");
    sys_call1(SYS_rmdir, (long)(uintptr_t)p);
    sys_call1(SYS_rmdir, (long)(uintptr_t)dir);

    out("DIRFDTEST DONE\n");
    return fails;
}

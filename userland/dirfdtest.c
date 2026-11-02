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
#define SYS_chmod 90
#define SYS_fchmod 91
#define SYS_chown 92
#define SYS_fchownat 260
#define SYS_fchmodat 268
#define SYS_umask 95
#define SYS_lseek 8
#define SYS_getdents64 217
#define SYS_sendfile 40
#define SYS_copy_file_range 326
#define SYS_pipe 22

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
static long sys_call6(long n, long a, long b, long c, long d,
                      long e, long f)
{
    long r;
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c),
                       "r"(r10), "r"(r8), "r"(r9)
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

/* M38: find an entry's d_off cookie in a getdents
 * buffer, or -1. */
static long dent_off(const char *db, long got,
                     const char *name)
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
            return *(const int64_t *)(db + off + 8);
        off += reclen;
    }
    return -1;
}
static long dent_name_at(const char *db, long got, long idx,
                         char *outnm)
{
    long off = 0, k = 0;
    while (off + 19 <= got) {
        uint16_t reclen = *(const uint16_t *)(db + off + 16);
        if (reclen < 19)
            break;
        if (k == idx) {
            uint64_t j = 0;
            const char *nm = db + off + 19;
            while (nm[j] != '\0' && j < 39) {
                outnm[j] = nm[j];
                j++;
            }
            outnm[j] = '\0';
            return 0;
        }
        off += reclen;
        k++;
    }
    return -1;
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
    join(p, dir, "m.txt");
    sys_call1(SYS_unlink, (long)(uintptr_t)p);
    join(p, dir, "m2.txt");
    sys_call1(SYS_unlink, (long)(uintptr_t)p);
    join(p, dir, "mdir");
    sys_call1(SYS_rmdir, (long)(uintptr_t)p);
    join(p, dir, "mdir2");
    sys_call1(SYS_rmdir, (long)(uintptr_t)p);
    join(p, dir, "sf-src");
    sys_call1(SYS_unlink, (long)(uintptr_t)p);
    join(p, dir, "sf-dst");
    sys_call1(SYS_unlink, (long)(uintptr_t)p);
    join(p, dir, "cf-src");
    sys_call1(SYS_unlink, (long)(uintptr_t)p);
    join(p, dir, "cf-dst");
    sys_call1(SYS_unlink, (long)(uintptr_t)p);
    join(p, dir, "r3.txt");
    sys_call1(SYS_unlink, (long)(uintptr_t)p);
    join(p, dir, "ln3");
    sys_call1(SYS_unlink, (long)(uintptr_t)p);
    join(p, dir, "m.txt");
    sys_call1(SYS_unlink, (long)(uintptr_t)p);
    join(p, dir, "m2.txt");
    sys_call1(SYS_unlink, (long)(uintptr_t)p);
    join(p, dir, "mdir");
    sys_call1(SYS_rmdir, (long)(uintptr_t)p);
    join(p, dir, "mdir2");
    sys_call1(SYS_rmdir, (long)(uintptr_t)p);
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

    /* --- M36: chmod / chown --- */
    {
        struct st sb2;
        long m0 = sys_call4(SYS_newfstatat, dfd,
                            (long)(uintptr_t)"f.txt",
                            (long)(uintptr_t)&sb2, 0) == 0
                      ? (sb2.mode & 0777)
                      : -1;
        join(p, dir, "f.txt");
        long c1 = sys_call3(SYS_chmod, (long)(uintptr_t)p,
                            0600, 0);
        sys_call4(SYS_newfstatat, dfd,
                  (long)(uintptr_t)"f.txt",
                  (long)(uintptr_t)&sb2, 0);
        long m1 = sb2.mode & 0777;
        long c2 = sys_call4(SYS_fchmodat, dfd,
                            (long)(uintptr_t)"f.txt", 0640, 0);
        sys_call4(SYS_newfstatat, dfd,
                  (long)(uintptr_t)"f.txt",
                  (long)(uintptr_t)&sb2, 0);
        long m2 = sb2.mode & 0777;
        fd = sys_call4(SYS_openat, dfd,
                       (long)(uintptr_t)"f.txt", 0, 0);
        long c3 = fd >= 0 ? sys_call3(SYS_fchmod, fd, 0755, 0)
                          : -1;
        if (fd >= 0)
            sys_call1(SYS_close, fd);
        sys_call4(SYS_newfstatat, dfd,
                  (long)(uintptr_t)"f.txt",
                  (long)(uintptr_t)&sb2, 0);
        long m3 = sb2.mode & 0777;
        long o1 = sys_call3(SYS_chown, (long)(uintptr_t)p,
                            0, 0);
        long o2 = sys_call5(SYS_fchownat, dfd,
                            (long)(uintptr_t)"f.txt", -1, -1,
                            0);
        long e1 = sys_call3(SYS_chmod,
                            (long)(uintptr_t)"/proc/self/status",
                            0644, 0);
        long e2 = sys_call3(SYS_chmod,
                            (long)(uintptr_t)"/ghost", 0644,
                            0);
        sys_call3(SYS_chmod, (long)(uintptr_t)p, 0644, 0);
        report("chmod-family", m0 == 0644 && c1 == 0 &&
               m1 == 0600 && c2 == 0 && m2 == 0640 &&
               c3 == 0 && m3 == 0755 && o1 == 0 && o2 == 0 &&
               e1 == -13 && e2 == -2);
    }

    /* --- M37: umask + creation modes --- */
    {
        struct st sb2;
        long u0 = sys_call3(SYS_umask, 0027, 0, 0);
        fd = sys_call4(SYS_openat, dfd,
                       (long)(uintptr_t)"m.txt", 0101, 0666);
        if (fd >= 0)
            sys_call1(SYS_close, fd);
        sys_call4(SYS_newfstatat, dfd,
                  (long)(uintptr_t)"m.txt",
                  (long)(uintptr_t)&sb2, 0);
        long mm1 = sb2.mode & 0777;
        long u1 = sys_call3(SYS_umask, 0, 0, 0);
        fd = sys_call4(SYS_openat, dfd,
                       (long)(uintptr_t)"m2.txt", 0101,
                       0600);
        if (fd >= 0)
            sys_call1(SYS_close, fd);
        sys_call4(SYS_newfstatat, dfd,
                  (long)(uintptr_t)"m2.txt",
                  (long)(uintptr_t)&sb2, 0);
        long mm2 = sb2.mode & 0777;
        long d1 = sys_call3(SYS_mkdirat, dfd,
                            (long)(uintptr_t)"mdir", 0777);
        sys_call4(SYS_newfstatat, dfd,
                  (long)(uintptr_t)"mdir",
                  (long)(uintptr_t)&sb2, 0);
        long mm3 = sb2.mode & 0777;
        sys_call3(SYS_umask, 0022, 0, 0);
        long d2 = sys_call3(SYS_mkdirat, dfd,
                            (long)(uintptr_t)"mdir2", 0777);
        sys_call4(SYS_newfstatat, dfd,
                  (long)(uintptr_t)"mdir2",
                  (long)(uintptr_t)&sb2, 0);
        long mm4 = sb2.mode & 0777;
        report("creat-modes", u0 == 0022 && mm1 == 0640 &&
               u1 == 0027 && mm2 == 0600 && d1 == 0 &&
               mm3 == 0777 && d2 == 0 && mm4 == 0755);
    }

    /* --- M38: getdents cookies --- */
    {
        static char names[24][40];
        char db2[2048], one[64], nm[40];
        long totaln = 0, got, cookie = -1, ok = 1;
        fd = sys_call3(SYS_open, (long)(uintptr_t)dir,
                       0, 0);
        got = fd >= 0 ? sys_call3(SYS_getdents64, fd,
                                  (long)(uintptr_t)db2,
                                  sizeof(db2))
                      : -1;
        while (totaln < 24 &&
               dent_name_at(db2, got, totaln,
                            names[totaln]) == 0)
            totaln++;
        /* locate f.txt and its cookie */
        {
            long i, fi = -1;
            for (i = 0; i < totaln; i++) {
                uint64_t j = 0;
                while (names[i][j] == "f.txt"[j] &&
                       names[i][j] != '\0')
                    j++;
                if (names[i][j] == '\0' && j == 5)
                    fi = i;
            }
            ok = ok && fi >= 0 && fi + 1 < totaln;
            cookie = dent_off(db2, got, "f.txt");
            ok = ok && cookie > 0;
            /* seek to the cookie: next read starts at
             * the entry after f.txt */
            ok = ok &&
                 sys_call3(SYS_lseek, fd, cookie, 0) ==
                     cookie;
            got = sys_call3(SYS_getdents64, fd,
                            (long)(uintptr_t)db2,
                            sizeof(db2));
            ok = ok && got > 0 &&
                 dent_name_at(db2, got, 0, nm) == 0;
            {
                uint64_t j = 0;
                const char *want = names[fi + 1];
                while (nm[j] == want[j] && nm[j] != '\0')
                    j++;
                ok = ok && nm[j] == '\0' &&
                     want[j] == '\0';
            }
            /* tiny buffer: EINVAL, not a fake EOF */
            sys_call3(SYS_lseek, fd, 0, 0);
            ok = ok && sys_call3(SYS_getdents64, fd,
                                 (long)(uintptr_t)one,
                                 16) == -22;
            /* rewind still works afterwards */
            got = sys_call3(SYS_getdents64, fd,
                            (long)(uintptr_t)db2,
                            sizeof(db2));
            ok = ok && got > 0 &&
                 dent_name_at(db2, got, 0, nm) == 0 &&
                 nm[0] == names[0][0];
            /* SEEK_END on a directory: EINVAL */
            ok = ok && sys_call3(SYS_lseek, fd, 0, 2) ==
                           -22;
        }
        if (fd >= 0)
            sys_call1(SYS_close, fd);
        report("seekdir", ok);
    }

    /* --- M40: sendfile between files --- */
    {
        char rb[32];
        long ifd, ofd, n;
        int64_t off = 4;
        join(p, dir, "sf-src");
        ifd = sys_call4(SYS_open, (long)(uintptr_t)p,
                        0101, 0644, 0);
        sys_call3(SYS_write, ifd,
                  (long)(uintptr_t)"0123456789ABCDEF", 16);
        sys_call1(SYS_close, ifd);
        ifd = sys_call3(SYS_open, (long)(uintptr_t)p, 0,
                        0);
        join(p, dir, "sf-dst");
        ofd = sys_call4(SYS_open, (long)(uintptr_t)p,
                        0101, 0644, 0);
        n = sys_call4(SYS_sendfile, ofd, ifd,
                      (long)(uintptr_t)&off, 6);
        int ok = n == 6 && off == 10;
        /* input offset untouched by the offset form */
        rb[0] = '?';
        ok = ok && sys_call3(SYS_read, ifd,
                             (long)(uintptr_t)rb, 1) == 1 &&
             rb[0] == '0';
        /* the streaming form resumes at offset 1 */
        n = sys_call4(SYS_sendfile, ofd, ifd, 0, 100);
        ok = ok && n == 15;
        /* EOF: a further copy moves nothing */
        ok = ok && sys_call4(SYS_sendfile, ofd, ifd, 0,
                             10) == 0;
        sys_call1(SYS_close, ifd);
        sys_call1(SYS_close, ofd);
        ofd = sys_call3(SYS_open, (long)(uintptr_t)p, 0,
                        0);
        {
            char all[32];
            long got = sys_call3(SYS_read, ofd,
                                 (long)(uintptr_t)all,
                                 sizeof(all));
            const char *want = "456789123456789ABCDEF";
            uint64_t j = 0;
            ok = ok && got == 21;
            while (ok && j < 21) {
                if (all[j] != want[j])
                    ok = 0;
                j++;
            }
        }
        sys_call1(SYS_close, ofd);
        report("sendfile", ok);
    }

    /* --- M43: copy_file_range --- */
    {
        char rb[32];
        long ifd, ofd, n, i;
        int64_t io = 2, oo = 3;
        int pf[2];
        join(p, dir, "cf-src");
        ifd = sys_call4(SYS_open, (long)(uintptr_t)p,
                        0101, 0644, 0);
        sys_call3(SYS_write, ifd,
                  (long)(uintptr_t)"0123456789ABCDEF", 16);
        sys_call1(SYS_close, ifd);
        ifd = sys_call3(SYS_open, (long)(uintptr_t)p, 0,
                        0);
        join(p, dir, "cf-dst");
        ofd = sys_call4(SYS_open, (long)(uintptr_t)p,
                        0101, 0644, 0);
        n = sys_call6(SYS_copy_file_range, ifd,
                      (long)(uintptr_t)&io, ofd,
                      (long)(uintptr_t)&oo, 5, 0);
        int ok = n == 5 && io == 7 && oo == 8;
        /* streaming form: descriptor offsets rule --
         * the positional leg left ofd's offset at 0, so
         * seek to the end first (8) */
        sys_call3(SYS_lseek, ofd, 8, 0);
        n = sys_call6(SYS_copy_file_range, ifd, 0, ofd, 0,
                      100, 0);
        ok = ok && n == 16;
        /* flags must be zero; pipes may not take part */
        ok = ok && sys_call6(SYS_copy_file_range, ifd, 0,
                             ofd, 0, 1, 1) == -22;
        sys_call3(SYS_pipe, (long)(uintptr_t)pf, 0, 0);
        ok = ok && sys_call6(SYS_copy_file_range, ifd, 0,
                             pf[1], 0, 1, 0) == -22;
        sys_call1(SYS_close, pf[0]);
        sys_call1(SYS_close, pf[1]);
        sys_call1(SYS_close, ifd);
        sys_call1(SYS_close, ofd);
        ofd = sys_call3(SYS_open, (long)(uintptr_t)p, 0,
                        0);
        n = sys_call3(SYS_read, ofd,
                      (long)(uintptr_t)rb, sizeof(rb));
        ok = ok && n == 24 && rb[0] == 0 && rb[1] == 0 &&
             rb[2] == 0 && rb[3] == '2' && rb[7] == '6' &&
             rb[8] == '0' && rb[23] == 'F';
        sys_call1(SYS_close, ofd);
        i = 0; /* silence: i used above via n only */
        report("copy-range", ok);
    }

    sys_call1(SYS_close, dfd);

    /* teardown: remove the whole tree so a second run in
     * the same boot starts from the same state (and the
     * harness's free-page comparison stays meaningful) */
    join(p, dir, "m.txt");
    sys_call1(SYS_unlink, (long)(uintptr_t)p);
    join(p, dir, "m2.txt");
    sys_call1(SYS_unlink, (long)(uintptr_t)p);
    join(p, dir, "mdir");
    sys_call1(SYS_rmdir, (long)(uintptr_t)p);
    join(p, dir, "mdir2");
    sys_call1(SYS_rmdir, (long)(uintptr_t)p);
    join(p, dir, "r3.txt");
    sys_call1(SYS_unlink, (long)(uintptr_t)p);
    join(p, dir, "ln3");
    sys_call1(SYS_unlink, (long)(uintptr_t)p);
    join(p, dir, "m.txt");
    sys_call1(SYS_unlink, (long)(uintptr_t)p);
    join(p, dir, "m2.txt");
    sys_call1(SYS_unlink, (long)(uintptr_t)p);
    join(p, dir, "mdir");
    sys_call1(SYS_rmdir, (long)(uintptr_t)p);
    join(p, dir, "mdir2");
    sys_call1(SYS_rmdir, (long)(uintptr_t)p);
    join(p, dir, "sf-src");
    sys_call1(SYS_unlink, (long)(uintptr_t)p);
    join(p, dir, "sf-dst");
    sys_call1(SYS_unlink, (long)(uintptr_t)p);
    join(p, dir, "cf-src");
    sys_call1(SYS_unlink, (long)(uintptr_t)p);
    join(p, dir, "cf-dst");
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

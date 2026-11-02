/* stattest - M20 acceptance scenarios for the stat family.
 *
 * Usage: stattest <base-dir>   (created fresh; e.g. /stx on
 * ramfs or /data/stx on yfs). Prints "stattest: NAME PASS"
 * lines and "STATTEST DONE"; exit 0 iff all pass.
 * Freestanding, Linux ABI. */
#include <stdint.h>

#define SYS_write    1
#define SYS_open     2
#define SYS_close    3
#define SYS_stat     4
#define SYS_fstat    5
#define SYS_lstat    6
#define SYS_mkdir    83
#define SYS_unlink   87
#define SYS_symlink  88
#define SYS_newfstatat 262
#define SYS_exit_group 231

#define O_RDONLY_DIR 0x10000 /* O_RDONLY|O_DIRECTORY */
#define AT_FDCWD (-100)
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_EMPTY_PATH 0x1000

#define S_IFMT  0xF000u
#define S_IFREG 0x8000u
#define S_IFDIR 0x4000u
#define S_IFLNK 0xA000u
#define S_IFCHR 0x2000u

struct stat_k {
    uint64_t dev, ino, nlink;
    uint32_t mode, uid, gid, pad0;
    uint64_t rdev;
    int64_t size, blksize, blocks;
    uint64_t atime, atime_nsec, mtime, mtime_nsec;
    uint64_t ctime, ctime_nsec;
    int64_t unused[3];
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
                     : "a"(nr), "D"(a1), "S"(a2), "d"(a3),
                       "r"(r10)
                     : "rcx", "r11", "memory");
    return ret;
}

static void out(const char *s)
{
    unsigned long n = 0;
    while (s[n])
        n++;
    sys_call3(SYS_write, 1, (long)(uintptr_t)s, (long)n);
}

static int g_fails;

static void report(const char *name, int ok)
{
    out("stattest: ");
    out(name);
    out(ok ? " PASS\n" : " FAIL\n");
    if (!ok)
        g_fails++;
}

static void join(char *dst, const char *a, const char *b)
{
    int i = 0;
    while (a[i]) {
        dst[i] = a[i];
        i++;
    }
    while (*b)
        dst[i++] = *b++;
    dst[i] = '\0';
}

static uint32_t type_of(const struct stat_k *st)
{
    return st->mode & S_IFMT;
}

int main(int argc, char **argv)
{
    const char *base = argc > 1 ? argv[1] : "/stx";
    char fpath[128], lpath[128], dpath[128], npath[128];
    struct stat_k st, st2;
    long rc;

    out("stattest: M20 stat suite\n");
    join(fpath, base, "/f");
    join(lpath, base, "/l");
    join(dpath, base, "/dangle");
    join(npath, base, "/nope");

    sys_call3(SYS_mkdir, (long)(uintptr_t)base, 0, 0);
    {
        long fd = sys_call3(SYS_open, (long)(uintptr_t)fpath,
                            0x241 /* WRONLY|CREAT|TRUNC */, 0);
        if (fd >= 0) {
            sys_call3(SYS_write, fd, (long)(uintptr_t)"hello",
                      5);
            sys_call3(SYS_close, fd, 0, 0);
        }
    }
    sys_call3(SYS_symlink, (long)(uintptr_t)"f",
              (long)(uintptr_t)lpath, 0);
    sys_call3(SYS_symlink, (long)(uintptr_t)"nope",
              (long)(uintptr_t)dpath, 0);

    /* 1: stat a regular file; fstat agrees */
    rc = sys_call3(SYS_stat, (long)(uintptr_t)fpath,
                   (long)(uintptr_t)&st, 0);
    {
        long fd = sys_call3(SYS_open, (long)(uintptr_t)fpath,
                            0, 0);
        long rc2 = fd >= 0 ? sys_call3(SYS_fstat, fd,
                                       (long)(uintptr_t)&st2,
                                       0)
                           : -1;
        report("file", rc == 0 && type_of(&st) == S_IFREG &&
               st.size == 5 && rc2 == 0 &&
               st2.mode == st.mode && st2.size == st.size &&
               st2.ino == st.ino);
        if (fd >= 0)
            sys_call3(SYS_close, fd, 0, 0);
    }

    /* 2: the link itself vs its target */
    rc = sys_call3(SYS_lstat, (long)(uintptr_t)lpath,
                   (long)(uintptr_t)&st, 0);
    {
        long rcf = sys_call3(SYS_stat, (long)(uintptr_t)lpath,
                             (long)(uintptr_t)&st2, 0);
        report("link", rc == 0 && type_of(&st) == S_IFLNK &&
               st.size == 1 && rcf == 0 &&
               type_of(&st2) == S_IFREG && st2.size == 5);
    }

    /* 3: newfstatat honors AT_SYMLINK_NOFOLLOW */
    rc = sys_call4(SYS_newfstatat, AT_FDCWD,
                   (long)(uintptr_t)lpath,
                   (long)(uintptr_t)&st, AT_SYMLINK_NOFOLLOW);
    {
        long rcf = sys_call4(SYS_newfstatat, AT_FDCWD,
                             (long)(uintptr_t)lpath,
                             (long)(uintptr_t)&st2, 0);
        report("fstatat", rc == 0 && type_of(&st) == S_IFLNK &&
               rcf == 0 && type_of(&st2) == S_IFREG);
    }

    /* 4: directories, missing names, dangling links */
    rc = sys_call3(SYS_stat, (long)(uintptr_t)base,
                   (long)(uintptr_t)&st, 0);
    {
        long rcm = sys_call3(SYS_stat, (long)(uintptr_t)npath,
                             (long)(uintptr_t)&st2, 0);
        long rcd = sys_call3(SYS_lstat, (long)(uintptr_t)dpath,
                             (long)(uintptr_t)&st2, 0);
        report("dir-missing",
               rc == 0 && type_of(&st) == S_IFDIR &&
               rcm == -2 && rcd == 0 &&
               type_of(&st2) == S_IFLNK);
    }

    /* 5: fstat of the console is a character device */
    rc = sys_call3(SYS_fstat, 1, (long)(uintptr_t)&st, 0);
    report("console", rc == 0 && type_of(&st) == S_IFCHR);

    /* 6: AT_EMPTY_PATH stats the directory fd itself */
    {
        long dfd = sys_call3(SYS_open, (long)(uintptr_t)base,
                             O_RDONLY_DIR, 0);
        rc = dfd >= 0
            ? sys_call4(SYS_newfstatat, dfd, (long)(uintptr_t)"",
                        (long)(uintptr_t)&st, AT_EMPTY_PATH)
            : -1;
        report("empty-path", rc == 0 &&
               type_of(&st) == S_IFDIR);
        if (dfd >= 0)
            sys_call3(SYS_close, dfd, 0, 0);
    }

    /* cleanup so reruns start fresh */
    sys_call3(SYS_unlink, (long)(uintptr_t)lpath, 0, 0);
    sys_call3(SYS_unlink, (long)(uintptr_t)dpath, 0, 0);
    sys_call3(SYS_unlink, (long)(uintptr_t)fpath, 0, 0);

    out("STATTEST DONE\n");
    return g_fails == 0 ? 0 : 1;
}

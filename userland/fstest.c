/* Yousj OS - M9 filesystem acceptance suite (userland).
 *
 * Proves the ramfs + fd layer end to end:
 *   persist    a file written by a previous run of this suite
 *              is still there (the ramfs is system-global);
 *              first run creates it, later runs verify it
 *   write-read create, write, close, reopen, byte-compare
 *   append     O_APPEND lands at the end
 *   trunc      O_TRUNC drops the size to zero
 *   seek       SEEK_SET/CUR/END positioning with partial reads
 *   dents      getdents64 lists "/" and "/bin" truthfully
 *   errors     ENOENT / EISDIR / ENOTDIR / EBADF magnitudes
 *   fork-share the open-file offset is shared across clone
 *              (child writes move the parent's position)
 *   mkdir-cwd  mkdir + chdir + getcwd + relative-path create
 *
 * Uses fixed filenames so repeated runs reach a steady state
 * (the harness checks the PMM count stops moving).
 * Prints "FSTEST <name> PASS"/"FSTEST <name> FAIL detail=",
 * then "FSTEST DONE"; exit 0 iff all passed.
 */
#include <stdint.h>
#include <stddef.h>

#define SYS_read       0
#define SYS_write      1
#define SYS_open       2
#define SYS_close      3
#define SYS_lseek      8
#define SYS_clone      56
#define SYS_wait4      61
#define SYS_exit_group 231
#define SYS_getcwd     79
#define SYS_chdir      80
#define SYS_mkdir      83
#define SYS_rename     82
#define SYS_rmdir      84
#define SYS_unlink     87
#define SYS_symlink    88
#define SYS_readlink   89
#define SYS_getdents64 217
#define SYS_openat     257

#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_CREAT     0x40
#define O_TRUNC     0x200
#define O_APPEND    0x400
#define O_DIRECTORY 0x10000
#define AT_FDCWD    (-100)

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define DT_DIR 4
#define DT_REG 8

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
                     : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
                     : "rcx", "r11", "memory");
    return ret;
}

static void out(const char *s)
{
    long n = 0;
    while (s[n])
        n++;
    if (n > 0)
        sys_call3(SYS_write, 1, (long)(uintptr_t)s, n);
}

static void out_long(long v)
{
    char buf[24];
    int i = 0;
    unsigned long u;
    if (v < 0) {
        out("-");
        u = (unsigned long)(-(v + 1)) + 1;
    } else {
        u = (unsigned long)v;
    }
    if (u == 0) {
        out("0");
        return;
    }
    while (u > 0) {
        buf[i++] = (char)('0' + (int)(u % 10));
        u /= 10;
    }
    while (i > 0) {
        char c[2];
        c[0] = buf[--i];
        c[1] = '\0';
        out(c);
    }
}

static int g_fails;
static void report(const char *name, int ok)
{
    out("FSTEST ");
    out(name);
    out(ok ? " PASS\n" : " FAIL\n");
    if (!ok)
        g_fails++;
}

static void report2(const char *name, int ok, long detail)
{
    out("FSTEST ");
    out(name);
    out(ok ? " PASS" : " FAIL detail=");
    if (!ok)
        out_long(detail);
    out("\n");
    if (!ok)
        g_fails++;
}

static void exit_now(long code)
{
    sys_call3(SYS_exit_group, code, 0, 0);
    for (;;) {
    }
}

static long open_(const char *path, long flags)
{
    return sys_call3(SYS_open, (long)(uintptr_t)path, flags, 0644);
}

static int bytes_eq(const char *a, const char *b, long n)
{
    long i;
    for (i = 0; i < n; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

static int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* Read the whole file at path into buf; returns length or <0. */
static long slurp(const char *path, char *buf, long cap)
{
    long fd = open_(path, O_RDONLY);
    long n;
    if (fd < 0)
        return fd;
    n = sys_call3(SYS_read, fd, (long)(uintptr_t)buf, cap);
    sys_call3(SYS_close, fd, 0, 0);
    return n;
}

static void t_persist(void)
{
    char buf[8];
    long n = slurp("/persist.txt", buf, sizeof(buf));
    if (n >= 0) {
        report2("persist", n == 2 && bytes_eq(buf, "P9", 2), n);
        return;
    }
    {
        long fd = open_("/persist.txt", O_CREAT | O_RDWR);
        long w = -1;
        if (fd >= 0) {
            w = sys_call3(SYS_write, fd, (long)(uintptr_t)"P9", 2);
            sys_call3(SYS_close, fd, 0, 0);
        }
        report2("persist", fd >= 0 && w == 2, w);
    }
}

static void t_write_read(void)
{
    long fd, w, n;
    char buf[16];
    fd = open_("/t1.txt", O_CREAT | O_RDWR | O_TRUNC);
    if (fd < 0) {
        report2("write-read", 0, fd);
        return;
    }
    w = sys_call3(SYS_write, fd, (long)(uintptr_t)"hello-fs-9", 10);
    sys_call3(SYS_close, fd, 0, 0);
    n = slurp("/t1.txt", buf, sizeof(buf));
    report2("write-read", w == 10 && n == 10 &&
            bytes_eq(buf, "hello-fs-9", 10), n);
}

static void t_append(void)
{
    long fd, n;
    char buf[16];
    fd = open_("/t1.txt", O_WRONLY | O_APPEND);
    if (fd < 0) {
        report2("append", 0, fd);
        return;
    }
    sys_call3(SYS_write, fd, (long)(uintptr_t)"XY", 2);
    sys_call3(SYS_close, fd, 0, 0);
    n = slurp("/t1.txt", buf, sizeof(buf));
    report2("append", n == 12 && bytes_eq(buf, "hello-fs-9XY", 12), n);
}

static void t_trunc(void)
{
    long fd = open_("/t1.txt", O_RDWR | O_TRUNC);
    long sz;
    if (fd < 0) {
        report2("trunc", 0, fd);
        return;
    }
    sz = sys_call3(SYS_lseek, fd, 0, SEEK_END);
    sys_call3(SYS_close, fd, 0, 0);
    report2("trunc", sz == 0, sz);
}

static void t_seek(void)
{
    long fd = open_("/t2.txt", O_CREAT | O_RDWR | O_TRUNC);
    char buf[8];
    long n1, n2, ok;
    if (fd < 0) {
        report2("seek", 0, fd);
        return;
    }
    sys_call3(SYS_write, fd, (long)(uintptr_t)"0123456789", 10);
    sys_call3(SYS_lseek, fd, 3, SEEK_SET);
    n1 = sys_call3(SYS_read, fd, (long)(uintptr_t)buf, 3);
    ok = (n1 == 3 && bytes_eq(buf, "345", 3));
    sys_call3(SYS_lseek, fd, -2, SEEK_END);
    n2 = sys_call3(SYS_read, fd, (long)(uintptr_t)buf, 2);
    ok = ok && (n2 == 2 && bytes_eq(buf, "89", 2));
    sys_call3(SYS_close, fd, 0, 0);
    report2("seek", ok, n2);
}

/* Scan one directory (freshly opened) for a name+type. */
static int dents_has(const char *path, const char *want, int want_type)
{
    char buf[512];
    long fd = open_(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0)
        return 0;
    /* A directory may hold more entries than one buffer:
     * keep reading until the kernel says it is drained
     * (/bin outgrew 512 bytes of records at M18). */
    for (;;) {
    long n = sys_call3(SYS_getdents64, fd, (long)(uintptr_t)buf,
                       (long)sizeof(buf));
    long off = 0;
    if (n <= 0) {
        sys_call3(SYS_close, fd, 0, 0);
        return 0;
    }
    while (off < n) {
        unsigned short reclen = *(const unsigned short *)(buf + off + 16);
        unsigned char type = (unsigned char)buf[off + 18];
        const char *name = buf + off + 19;
        if (type == want_type && str_eq(name, want)) {
            sys_call3(SYS_close, fd, 0, 0);
            return 1;
        }
        if (reclen == 0)
            break;
        off += reclen;
    }
    }
}

static void t_dents(void)
{
    long root = open_("/", O_RDONLY | O_DIRECTORY);
    long bin = open_("/bin", O_RDONLY | O_DIRECTORY);
    int ok = 1;
    if (root < 0 || bin < 0) {
        report2("dents", 0, root < 0 ? root : bin);
        return;
    }
    ok = ok && dents_has("/", "bin", DT_DIR);
    ok = ok && dents_has("/", "t1.txt", DT_REG);
    ok = ok && dents_has("/bin", "hello", DT_REG);
    ok = ok && dents_has("/bin", "sh", DT_REG);
    ok = ok && dents_has("/bin", "fstest", DT_REG);
    sys_call3(SYS_close, root, 0, 0);
    sys_call3(SYS_close, bin, 0, 0);
    report("dents", ok);
}

static void t_errors(void)
{
    long a = open_("/nope.txt", O_RDONLY);
    long b = open_("/bin", O_WRONLY);
    long c = open_("/bin/hello", O_DIRECTORY);
    long fd = open_("/t1.txt", O_WRONLY);
    char ch = 0;
    long r = -999, g;
    if (fd >= 0) {
        r = sys_call3(SYS_read, fd, (long)(uintptr_t)&ch, 1);
        sys_call3(SYS_close, fd, 0, 0);
    }
    g = sys_call3(SYS_getdents64, 3 + 1000, 0, 0); /* bogus fd */
    (void)g;
    report2("errors", a == -2 && b == -21 && c == -20 && r == -9,
            a * 1000000L + b * 10000L + c * 100L + r);
}

static void t_fork_share(void)
{
    long fd = open_("/share.txt", O_CREAT | O_RDWR | O_TRUNC);
    long pid, st = 0, pos;
    if (fd < 0) {
        report2("fork-share", 0, fd);
        return;
    }
    sys_call3(SYS_write, fd, (long)(uintptr_t)"AB", 2);
    pid = sys_call3(SYS_clone, 17, 0, 0);
    if (pid == 0) {
        sys_call3(SYS_write, fd, (long)(uintptr_t)"CD", 2);
        exit_now(0);
    }
    if (pid < 0) {
        report2("fork-share", 0, pid);
        return;
    }
    sys_call4(SYS_wait4, pid, (long)(uintptr_t)&st, 0, 0);
    pos = sys_call3(SYS_lseek, fd, 0, SEEK_CUR);
    sys_call3(SYS_close, fd, 0, 0);
    /* Shared offset: the child's 2 bytes moved OUR position
     * from 2 to 4. A copied table would still say 2. */
    report2("fork-share", pos == 4 && (st & 0xFF) == 0, pos);
}

static void t_mkdir_cwd(void)
{
    long m = sys_call3(SYS_mkdir, (long)(uintptr_t)"/d1", 0755, 0);
    long cd, fd, n;
    char buf[64];
    int ok;
    if (m != 0 && m != -17) { /* EEXIST on repeat runs is fine */
        report2("mkdir-cwd", 0, m);
        return;
    }
    cd = sys_call3(SYS_chdir, (long)(uintptr_t)"/d1", 0, 0);
    if (cd != 0) {
        report2("mkdir-cwd", 0, cd);
        return;
    }
    n = sys_call3(SYS_getcwd, (long)(uintptr_t)buf, (long)sizeof(buf), 0);
    ok = (n == 3 && str_eq(buf, "/d1"));
    fd = open_("f.txt", O_CREAT | O_RDWR | O_TRUNC);
    if (fd < 0)
        ok = 0;
    else {
        sys_call3(SYS_write, fd, (long)(uintptr_t)"cwd-ok", 6);
        sys_call3(SYS_close, fd, 0, 0);
    }
    {
        char b2[8];
        long n2 = slurp("/d1/f.txt", b2, sizeof(b2));
        ok = ok && (n2 == 6 && bytes_eq(b2, "cwd-ok", 6));
    }
    sys_call3(SYS_chdir, (long)(uintptr_t)"/", 0, 0);
    report2("mkdir-cwd", ok, m);
}

static void t_unlink(void)
{
    long fd = open_("/u.txt", O_CREAT | O_RDWR | O_TRUNC);
    long r1, r2;
    if (fd < 0) {
        report("unlink", 0);
        return;
    }
    sys_call3(SYS_write, fd, (long)(uintptr_t)"data", 4);
    sys_call3(SYS_close, fd, 0, 0);
    r1 = sys_call3(SYS_unlink, (long)(uintptr_t)"/u.txt", 0, 0);
    r2 = open_("/u.txt", O_RDONLY);
    report("unlink", r1 == 0 && r2 == -2);
    if (r2 >= 0)
        sys_call3(SYS_close, r2, 0, 0);
}

static void t_unlink_open(void)
{
    long fd = open_("/u2.txt", O_CREAT | O_RDWR | O_TRUNC);
    char buf[4];
    long n, r;
    if (fd < 0) {
        report("unlink-open", 0);
        return;
    }
    sys_call3(SYS_write, fd, (long)(uintptr_t)"zz", 2);
    r = sys_call3(SYS_unlink, (long)(uintptr_t)"/u2.txt", 0, 0);
    sys_call3(SYS_lseek, fd, 0, SEEK_SET);
    n = sys_call3(SYS_read, fd, (long)(uintptr_t)buf, 2);
    sys_call3(SYS_close, fd, 0, 0);
    report("unlink-open", r == 0 && n == 2 && buf[0] == 'z' &&
           buf[1] == 'z');
}

static void t_rename(void)
{
    long fd = open_("/r1.txt", O_CREAT | O_RDWR | O_TRUNC);
    long r1, r2, gone;
    char buf[4];
    long n;
    if (fd < 0) {
        report("rename", 0);
        return;
    }
    sys_call3(SYS_write, fd, (long)(uintptr_t)"R", 1);
    sys_call3(SYS_close, fd, 0, 0);
    r1 = sys_call3(SYS_rename, (long)(uintptr_t)"/r1.txt",
                   (long)(uintptr_t)"/r2.txt", 0);
    gone = open_("/r1.txt", O_RDONLY);
    if (gone >= 0)
        sys_call3(SYS_close, gone, 0, 0);
    sys_call3(SYS_mkdir, (long)(uintptr_t)"/rd", 0755, 0);
    r2 = sys_call3(SYS_rename, (long)(uintptr_t)"/r2.txt",
                   (long)(uintptr_t)"/rd/r3.txt", 0);
    fd = open_("/rd/r3.txt", O_RDONLY);
    n = fd >= 0 ? sys_call3(SYS_read, fd, (long)(uintptr_t)buf, 1)
                : -1;
    if (fd >= 0)
        sys_call3(SYS_close, fd, 0, 0);
    sys_call3(SYS_unlink, (long)(uintptr_t)"/rd/r3.txt", 0, 0);
    sys_call3(SYS_rmdir, (long)(uintptr_t)"/rd", 0, 0);
    report("rename", r1 == 0 && gone == -2 && r2 == 0 && n == 1 &&
           buf[0] == 'R');
}

static void t_rmdir(void)
{
    long r1, r2, r3;
    long fd;
    sys_call3(SYS_mkdir, (long)(uintptr_t)"/empty1", 0755, 0);
    r1 = sys_call3(SYS_rmdir, (long)(uintptr_t)"/empty1", 0, 0);
    sys_call3(SYS_mkdir, (long)(uintptr_t)"/ne", 0755, 0);
    fd = open_("/ne/f.txt", O_CREAT | O_RDWR);
    if (fd >= 0)
        sys_call3(SYS_close, fd, 0, 0);
    r2 = sys_call3(SYS_rmdir, (long)(uintptr_t)"/ne", 0, 0);
    r3 = sys_call3(SYS_rmdir, (long)(uintptr_t)"/ne/f.txt", 0, 0);
    sys_call3(SYS_unlink, (long)(uintptr_t)"/ne/f.txt", 0, 0);
    sys_call3(SYS_rmdir, (long)(uintptr_t)"/ne", 0, 0);
    report("rmdir", r1 == 0 && r2 == -39 && r3 == -20);
}


/* --- M16: symlinks -------------------------------------- */

static long read_str(const char *path, char *buf, long cap)
{
    long fd = open_(path, O_RDONLY);
    long n;
    if (fd < 0)
        return fd;
    n = sys_call3(SYS_read, fd, (long)(uintptr_t)buf, cap - 1);
    sys_call3(SYS_close, fd, 0, 0);
    if (n >= 0)
        buf[n] = '\0';
    return n;
}

static void t_symlink(void)
{
    char buf[32];
    long fd;
    /* the suite runs twice per boot: clear run 1's links */
    sys_call3(SYS_unlink, (long)(uintptr_t)"/link1", 0, 0);
    sys_call3(SYS_unlink, (long)(uintptr_t)"/link2", 0, 0);
    sys_call3(SYS_unlink, (long)(uintptr_t)"/link3", 0, 0);
    fd = open_("/real.txt", O_CREAT | O_RDWR | O_TRUNC);
    long r;
    if (fd < 0) {
        report("symlink", 0);
        return;
    }
    sys_call3(SYS_write, fd, (long)(uintptr_t)"DATA", 4);
    sys_call3(SYS_close, fd, 0, 0);
    r = sys_call3(SYS_symlink, (long)(uintptr_t)"real.txt",
                  (long)(uintptr_t)"/link1", 0);
    r += sys_call3(SYS_symlink, (long)(uintptr_t)"/real.txt",
                   (long)(uintptr_t)"/link2", 0);
    report("symlink", r == 0 &&
           read_str("/link1", buf, sizeof buf) == 4 &&
           buf[0] == 'D' && buf[3] == 'A' &&
           read_str("/link2", buf, sizeof buf) == 4);
}

static void t_link_chain(void)
{
    char buf[32];
    sys_call3(SYS_symlink, (long)(uintptr_t)"/link1",
              (long)(uintptr_t)"/link3", 0);
    report("link-chain",
           read_str("/link3", buf, sizeof buf) == 4 &&
           buf[0] == 'D');
}

static void t_link_loop(void)
{
    long fd;
    sys_call3(SYS_symlink, (long)(uintptr_t)"/lb",
              (long)(uintptr_t)"/la", 0);
    sys_call3(SYS_symlink, (long)(uintptr_t)"/la",
              (long)(uintptr_t)"/lb", 0);
    fd = open_("/la", O_RDONLY);
    report("link-loop", fd == -40);
    if (fd >= 0)
        sys_call3(SYS_close, fd, 0, 0);
}

static void t_link_dangle(void)
{
    static char buf[64];
    long fd, n;
    sys_call3(SYS_symlink, (long)(uintptr_t)"/no-such",
              (long)(uintptr_t)"/dang", 0);
    fd = open_("/dang", O_RDONLY);
    n = sys_call3(SYS_readlink, (long)(uintptr_t)"/dang",
                  (long)(uintptr_t)buf, sizeof buf - 1);
    if (n >= 0)
        buf[n] = '\0';
    report("link-dangle", fd == -2 && n == 8 &&
           buf[0] == '/' && buf[1] == 'n');
    if (fd >= 0)
        sys_call3(SYS_close, fd, 0, 0);
}

static void t_readlink(void)
{
    static char buf[64];
    long n = sys_call3(SYS_readlink, (long)(uintptr_t)"/link1",
                       (long)(uintptr_t)buf, sizeof buf - 1);
    long bad;
    if (n >= 0)
        buf[n] = '\0';
    bad = sys_call3(SYS_readlink, (long)(uintptr_t)"/real.txt",
                    (long)(uintptr_t)buf, sizeof buf - 1);
    report("readlink", n == 8 && buf[0] == 'r' &&
           buf[7] == 't' && bad == -22);
}

static void t_link_midpath(void)
{
    char buf[32];
    long fd;
    sys_call3(SYS_mkdir, (long)(uintptr_t)"/ld", 0755, 0);
    fd = open_("/ld/inner.txt", O_CREAT | O_RDWR | O_TRUNC);
    if (fd < 0) {
        report("link-midpath", 0);
        return;
    }
    sys_call3(SYS_write, fd, (long)(uintptr_t)"IN", 2);
    sys_call3(SYS_close, fd, 0, 0);
    sys_call3(SYS_symlink, (long)(uintptr_t)"/ld",
              (long)(uintptr_t)"/ldlink", 0);
    report("link-midpath",
           read_str("/ldlink/inner.txt", buf, sizeof buf) == 2 &&
           buf[0] == 'I' && buf[1] == 'N');
}

/* M47: the /dev character devices. */
static void t_devices(void)
{
    char buf[16], buf2[16];
    long fd, n, i;
    int ok = 1;
    fd = open_("/dev/null", 1 /* O_WRONLY */);
    ok = ok && fd >= 0 &&
         sys_call3(SYS_write, fd, (long)(uintptr_t)"hello", 5) == 5;
    if (fd >= 0)
        sys_call3(SYS_close, fd, 0, 0);
    fd = open_("/dev/null", O_RDONLY);
    ok = ok && fd >= 0 &&
         sys_call3(SYS_read, fd, (long)(uintptr_t)buf, 4) == 0;
    if (fd >= 0)
        sys_call3(SYS_close, fd, 0, 0);
    fd = open_("/dev/zero", O_RDONLY);
    n = fd >= 0 ? sys_call3(SYS_read, fd, (long)(uintptr_t)buf, 8) : -1;
    ok = ok && n == 8;
    for (i = 0; i < 8; i++)
        ok = ok && buf[i] == 0;
    if (fd >= 0)
        sys_call3(SYS_close, fd, 0, 0);
    fd = open_("/dev/full", 1);
    ok = ok && fd >= 0 &&
         sys_call3(SYS_write, fd, (long)(uintptr_t)"x", 1) == -28;
    if (fd >= 0)
        sys_call3(SYS_close, fd, 0, 0);
    fd = open_("/dev/urandom", O_RDONLY);
    n = fd >= 0 ? sys_call3(SYS_read, fd, (long)(uintptr_t)buf, 16) : -1;
    {
        long n2 = fd >= 0 ? sys_call3(SYS_read, fd,
                                      (long)(uintptr_t)buf2, 16)
                          : -1;
        int same = 1, allzero = 1;
        for (i = 0; i < 16; i++) {
            if (buf[i] != buf2[i])
                same = 0;
            if (buf[i] != 0)
                allzero = 0;
        }
        ok = ok && n == 16 && n2 == 16 && !same && !allzero;
    }
    if (fd >= 0)
        sys_call3(SYS_close, fd, 0, 0);
    {
        /* stat: character device type bits */
        uint8_t stb[144];
        long rc = sys_call4(262 /* newfstatat */, -100,
                            (long)(uintptr_t)"/dev/null",
                            (long)(uintptr_t)stb, 0);
        uint32_t mode = *(uint32_t *)(stb + 24);
        ok = ok && rc == 0 && (mode & 0170000) == 0020000;
    }
    {
        /* M49: /dev/console is a real node -- stat says
         * CHR, open works, and a write lands on the
         * terminal (the harness greps for the token). */
        uint8_t stb[144];
        long rc = sys_call4(262 /* newfstatat */, -100,
                            (long)(uintptr_t)"/dev/console",
                            (long)(uintptr_t)stb, 0);
        uint32_t mode = *(uint32_t *)(stb + 24);
        long cfd = open_("/dev/console", O_RDWR);
        ok = ok && rc == 0 && (mode & 0170000) == 0020000 &&
             cfd >= 0 &&
             sys_call3(SYS_write, cfd,
                       (long)(uintptr_t)"DEVCON-TOKEN-49\n",
                       16) == 16;
        if (cfd >= 0)
            sys_call3(SYS_close, cfd, 0, 0);
    }
    report("devices", ok);
}

static void t_link_unlink(void)
{
    char buf[32];
    long r = sys_call3(SYS_unlink, (long)(uintptr_t)"/link2",
                       0, 0);
    long fd = open_("/link2", O_RDONLY);
    report("link-unlink", r == 0 && fd == -2 &&
           read_str("/real.txt", buf, sizeof buf) == 4);
    if (fd >= 0)
        sys_call3(SYS_close, fd, 0, 0);
}

int main(void)
{
    long fd;
    out("fstest: M9 filesystem suite\n");
    t_persist();
    t_write_read();
    t_append();
    t_trunc();
    t_seek();
    t_dents();
    t_errors();
    t_fork_share();
    t_mkdir_cwd();
    /* openat smoke: AT_FDCWD behaves like open here. */
    fd = sys_call4(SYS_openat, AT_FDCWD, (long)(uintptr_t)"/t2.txt",
                   O_RDONLY, 0);
    report2("openat", fd >= 0, fd);
    t_unlink();
    t_unlink_open();
    t_rename();
    t_rmdir();
    t_symlink();
    t_link_chain();
    t_link_loop();
    t_link_dangle();
    t_readlink();
    t_link_midpath();
    t_link_unlink();
    t_devices();

    if (fd >= 0)
        sys_call3(SYS_close, fd, 0, 0);
    out("FSTEST DONE\n");
    return g_fails == 0 ? 0 : 1;
}

/* Yousj OS - mmtest (M14): file-backed mmap suite.
 *
 * Prints "mmtest: NAME PASS"/"mmtest: NAME FAIL" per scenario,
 * then "MMTEST DONE"; exit 0 iff all passed. Covers eager
 * population, MAP_PRIVATE isolation, MAP_SHARED write-back
 * (msync and munmap paths), a nonzero file offset, and the
 * same shared flow on the yfs volume at /data. Runs on the
 * ramfs root unless a name says otherwise.
 */
#include <stdint.h>
#include <stddef.h>

#define SYS_read   0
#define SYS_write  1
#define SYS_open   2
#define SYS_close  3
#define SYS_mmap   9
#define SYS_mprotect 10
#define SYS_munmap 11
#define SYS_msync  26
#define SYS_unlink 87
#define SYS_exit_group 231
#define SYS_clone  56
#define SYS_wait4  61
#define SYS_PMM_FREE 0x1001

#define O_RDONLY 0
#define O_RDWR   2
#define O_CREAT  0x40
#define O_TRUNC  0x200

#define PROT_READ  1
#define PROT_WRITE 2
#define MAP_SHARED  0x01
#define MAP_PRIVATE 0x02
#define MAP_ANON    0x20
#define MS_SYNC 16

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

static long sys_mmap(void *addr, unsigned long len, long prot,
                     long flags, long fd, long off)
{
    long ret;
    register long r10 __asm__("r10") = flags;
    register long r8 __asm__("r8") = fd;
    register long r9 __asm__("r9") = off;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(9L), "D"(addr), "S"(len), "d"(prot),
                       "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return ret;
}

static unsigned long str_len_(const char *s)
{
    unsigned long n = 0;
    while (s[n])
        n++;
    return n;
}

static void out(const char *s)
{
    sys_call3(SYS_write, 1, (long)(uintptr_t)s, (long)str_len_(s));
}

static int g_fails;

static void report(const char *name, int ok)
{
    out("mmtest: ");
    out(name);
    out(ok ? " PASS\n" : " FAIL\n");
    if (!ok)
        g_fails++;
}

static long write_file(const char *path, const char *data, long n)
{
    long fd = sys_call3(SYS_open, (long)(uintptr_t)path,
                        O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    sys_call3(SYS_write, fd, (long)(uintptr_t)data, n);
    sys_call3(SYS_close, fd, 0, 0);
    return 0;
}

static long read_file(const char *path, char *buf, long cap)
{
    long fd = sys_call3(SYS_open, (long)(uintptr_t)path,
                        O_RDONLY, 0);
    long n;
    if (fd < 0)
        return -1;
    n = sys_call3(SYS_read, fd, (long)(uintptr_t)buf, cap);
    sys_call3(SYS_close, fd, 0, 0);
    return n;
}

static int bytes_eq(const char *a, const char *b, long n)
{
    long i;
    for (i = 0; i < n; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

/* Shared scenario: map shared, patch byte 0, msync, verify
 * through a fresh read; patch byte 1, munmap, verify again. */
static void shared_flow(const char *path, const char *name)
{
    static const char content[] = "MMAP-CONTENT-0123456789";
    char buf[64];
    long fd, n;
    char *m;

    if (write_file(path, content, (long)sizeof(content) - 1) != 0) {
        report(name, 0);
        return;
    }
    fd = sys_call3(SYS_open, (long)(uintptr_t)path, O_RDWR, 0);
    if (fd < 0) {
        report(name, 0);
        return;
    }
    m = (char *)(uintptr_t)sys_mmap(NULL, 4096,
                                    PROT_READ | PROT_WRITE,
                                    MAP_SHARED, fd, 0);
    if ((long)(uintptr_t)m < 0) {
        report(name, 0);
        return;
    }
    m[0] = 'S';
    sys_call3(SYS_msync, (long)(uintptr_t)m, 4096, MS_SYNC);
    n = read_file(path, buf, sizeof(buf));
    if (n < 2 || buf[0] != 'S' || buf[1] != 'M') {
        report(name, 0);
        return;
    }
    m[1] = 'T';
    sys_call3(SYS_munmap, (long)(uintptr_t)m, 4096, 0);
    n = read_file(path, buf, sizeof(buf));
    sys_call3(SYS_close, fd, 0, 0);
    sys_call3(SYS_unlink, (long)(uintptr_t)path, 0, 0);
    report(name, n >= 2 && buf[0] == 'S' && buf[1] == 'T');
}


/* --- M17: demand paging --------------------------------- */

static long free_pages(void)
{
    return sys_call3(SYS_PMM_FREE, 0, 0, 0);
}

static void t_lazy(void)
{
    static char big[65536];
    long i, fd, f0, f1, f2, f4;
    char *m;
    int ok;
    for (i = 0; i < 65536; i++)
        big[i] = (char)('A' + (i / 4096) % 26);
    write_file("/mm-lazy.bin", big, 65536);
    fd = sys_call3(SYS_open, (long)(uintptr_t)"/mm-lazy.bin",
                   O_RDONLY, 0);
    f0 = free_pages();
    m = (char *)(uintptr_t)sys_mmap(NULL, 65536,
                                    PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE, fd, 0);
    if ((long)(uintptr_t)m < 0) {
        report("lazy", 0);
        return;
    }
    f1 = free_pages();
    /* registration only: no frame may be spent yet */
    ok = (f1 == f0);
    /* first touch materializes a page (plus maybe tables);
     * the load must be volatile or the compiler elides it
     * and nothing ever faults */
    {
        volatile char t = *(volatile char *)m;
        (void)t;
    }
    f2 = free_pages();
    ok = ok && f2 < f1 && f2 >= f1 - 6;
    /* every page faults in with the right content */
    for (i = 0; i < 16 && ok; i++)
        ok = m[i * 4096] == (char)('A' + i % 26);
    sys_call3(SYS_munmap, (long)(uintptr_t)m, 65536, 0);
    f4 = free_pages();
report("lazy", ok && f4 == f0);
    sys_call3(SYS_close, fd, 0, 0);
    sys_call3(SYS_unlink, (long)(uintptr_t)"/mm-lazy.bin", 0, 0);
}

static void t_cow(void)
{
    static char two[8192];
    char buf[16];
    long i, fd;
    char *m;
    int ok;
    for (i = 0; i < 4096; i++)
        two[i] = 'a';
    for (; i < 8192; i++)
        two[i] = 'b';
    write_file("/mm-cow.bin", two, 8192);
    fd = sys_call3(SYS_open, (long)(uintptr_t)"/mm-cow.bin",
                   O_RDWR, 0);
    m = (char *)(uintptr_t)sys_mmap(NULL, 8192,
                                    PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE, fd, 0);
    if ((long)(uintptr_t)m < 0) {
        report("cow-flip", 0);
        return;
    }
    m[0] = 'X';            /* write fault: fill + COW flip */
    ok = m[0] == 'X' && m[4096] == 'b';
    sys_call3(SYS_munmap, (long)(uintptr_t)m, 8192, 0);
    sys_call3(SYS_close, fd, 0, 0);
    read_file("/mm-cow.bin", buf, sizeof buf);
    report("cow-flip", ok && buf[0] == 'a');
    sys_call3(SYS_unlink, (long)(uintptr_t)"/mm-cow.bin", 0, 0);
}

static void t_segv_wild(void)
{
    long pid = sys_call3(SYS_clone, 17 /* SIGCHLD */, 0, 0);
    int status = 0;
    if (pid == 0) {
        volatile char *p = (volatile char *)0x8004000000ULL;
        *p = 1; /* no VMA covers this: SIGSEGV */
        sys_call3(SYS_exit_group, 99, 0, 0);
    }
    sys_call4(SYS_wait4, pid, (long)(uintptr_t)&status, 0, 0);
    report("segv-wild", (status & 0x7F) == 11);
}

static void t_segv_ro(void)
{
    long fd;
    write_file("/mm-ro.bin", "readonly!", 9);
    fd = sys_call3(SYS_open, (long)(uintptr_t)"/mm-ro.bin",
                   O_RDONLY, 0);
    {
        long pid = sys_call3(SYS_clone, 17, 0, 0);
        int status = 0;
        if (pid == 0) {
            char *m = (char *)(uintptr_t)sys_mmap(
                NULL, 4096, PROT_READ, MAP_SHARED, fd, 0);
            if ((long)(uintptr_t)m < 0)
                sys_call3(SYS_exit_group, 98, 0, 0);
            m[0] = 'Z'; /* write to a read-only shared page */
            sys_call3(SYS_exit_group, 99, 0, 0);
        }
        sys_call4(SYS_wait4, pid, (long)(uintptr_t)&status, 0, 0);
        report("segv-ro", (status & 0x7F) == 11);
    }
    sys_call3(SYS_close, fd, 0, 0);
    sys_call3(SYS_unlink, (long)(uintptr_t)"/mm-ro.bin", 0, 0);
}

static void t_mprotect_ro(void)
{
    char *m = (char *)(uintptr_t)sys_mmap(
        NULL, 8192, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANON, -1, 0);
    long rc;
    if ((long)(uintptr_t)m < 0) {
        report("mprotect-ro", 0);
        return;
    }
    m[0] = 'A';
    m[4096] = 'B';
    rc = sys_call3(SYS_mprotect, (long)(uintptr_t)m, 8192,
                   PROT_READ);
    {
        long pid = sys_call3(SYS_clone, 17, 0, 0);
        int status = 0;
        if (pid == 0) {
            m[0] = 'Z'; /* write to a now-read-only page */
            sys_call3(SYS_exit_group, 99, 0, 0);
        }
        sys_call4(SYS_wait4, pid, (long)(uintptr_t)&status, 0, 0);
        rc = rc == 0 && m[0] == 'A' && m[4096] == 'B' &&
             (status & 0x7F) == 11 ? 0 : -1;
    }
    /* and back to writable */
    if (sys_call3(SYS_mprotect, (long)(uintptr_t)m, 8192,
                  PROT_READ | PROT_WRITE) == 0)
        m[0] = 'C';
    report("mprotect-ro", rc == 0 && m[0] == 'C');
    sys_call3(SYS_munmap, (long)(uintptr_t)m, 8192, 0);
}

static void t_mprotect_mid(void)
{
    char *m = (char *)(uintptr_t)sys_mmap(
        NULL, 12288, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANON, -1, 0);
    volatile char *vm = (volatile char *)m;
    int ok;
    if ((long)(uintptr_t)m < 0) {
        report("mprotect-mid", 0);
        return;
    }
    vm[0] = 'a';
    vm[4096] = 'b';
    vm[8192] = 'c';
    ok = sys_call3(SYS_mprotect, (long)(uintptr_t)(m + 4096),
                   4096, 0 /* PROT_NONE */) == 0;
    ok = ok && vm[0] == 'a' && vm[8192] == 'c';
    {
        long pid = sys_call3(SYS_clone, 17, 0, 0);
        int status = 0;
        if (pid == 0) {
            (void)vm[4096]; /* read a PROT_NONE page */
            sys_call3(SYS_exit_group, 99, 0, 0);
        }
        sys_call4(SYS_wait4, pid, (long)(uintptr_t)&status, 0, 0);
        ok = ok && (status & 0x7F) == 11;
    }
    /* re-protect: the contents must have survived */
    ok = ok && sys_call3(SYS_mprotect,
                         (long)(uintptr_t)(m + 4096), 4096,
                         PROT_READ | PROT_WRITE) == 0;
    ok = ok && vm[4096] == 'b';
    /* one munmap across the split pieces */
    ok = ok && sys_call3(SYS_munmap, (long)(uintptr_t)m,
                         12288, 0) == 0;
    report("mprotect-mid", ok);
}

static void t_mprotect_err(void)
{
    char *a = (char *)(uintptr_t)sys_mmap(
        NULL, 4096, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANON, -1, 0);
    char *b = (char *)(uintptr_t)sys_mmap(
        NULL, 4096, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANON, -1, 0);
    int ok = (long)(uintptr_t)a >= 0 &&
             (long)(uintptr_t)b >= 0;
    if (ok) {
        /* the allocator leaves a one-page gap between VMAs:
         * protecting the gap is ENOMEM */
        char *gap = a + 4096;
        if (b == a + 8192)
            ok = sys_call3(SYS_mprotect, (long)(uintptr_t)gap,
                           4096, PROT_READ) == -12;
        else
            ok = 0;
        ok = ok && sys_call3(SYS_mprotect,
                             (long)(uintptr_t)(a + 1), 4096,
                             PROT_READ) == -22;
        ok = ok && sys_call3(SYS_mprotect, (long)(uintptr_t)a,
                             0, PROT_READ) == -22;
        sys_call3(SYS_munmap, (long)(uintptr_t)a, 4096, 0);
        sys_call3(SYS_munmap, (long)(uintptr_t)b, 4096, 0);
    }
    report("mprotect-err", ok);
}

int main(void)
{
    static const char content[] = "MMAP-CONTENT-0123456789";
    char buf[64];
    long fd, n;
    char *m;

    out("mmtest: M14 mmap suite\n");

    /* 1: anonymous mapping still works */
    m = (char *)(uintptr_t)sys_mmap(NULL, 8192,
                                    PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANON, -1, 0);
    if ((long)(uintptr_t)m < 0) {
        report("anon", 0);
    } else {
        long i;
        int ok = 1;
        for (i = 0; i < 8192; i++)
            m[i] = (char)(i & 0x5A);
        for (i = 0; i < 8192; i++)
            if (m[i] != (char)(i & 0x5A))
                ok = 0;
        sys_call3(SYS_munmap, (long)(uintptr_t)m, 8192, 0);
        report("anon", ok);
    }

    /* prepare the shared test file on ramfs */
    write_file("/mm1.txt", content, (long)sizeof(content) - 1);

    /* 2: private read reflects the file, zeros past EOF */
    fd = sys_call3(SYS_open, (long)(uintptr_t)"/mm1.txt",
                   O_RDONLY, 0);
    m = (char *)(uintptr_t)sys_mmap(NULL, 4096, PROT_READ,
                                    MAP_PRIVATE, fd, 0);
    if ((long)(uintptr_t)m < 0) {
        report("priv-read", 0);
    } else {
        int ok = bytes_eq(m, content, (long)sizeof(content) - 1) &&
                 m[sizeof(content) - 1] == 0 &&
                 m[4095] == 0;
        sys_call3(SYS_munmap, (long)(uintptr_t)m, 4096, 0);
        report("priv-read", ok);
    }
    sys_call3(SYS_close, fd, 0, 0);

    /* 3: private writes never reach the file */
    fd = sys_call3(SYS_open, (long)(uintptr_t)"/mm1.txt",
                   O_RDONLY, 0);
    m = (char *)(uintptr_t)sys_mmap(NULL, 4096,
                                    PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE, fd, 0);
    if ((long)(uintptr_t)m < 0) {
        report("priv-write", 0);
    } else {
        m[0] = 'X';
        sys_call3(SYS_munmap, (long)(uintptr_t)m, 4096, 0);
        n = read_file("/mm1.txt", buf, sizeof(buf));
        report("priv-write",
               n >= 1 && buf[0] == 'M');
    }
    sys_call3(SYS_close, fd, 0, 0);

    /* 4: shared write-back on ramfs */
    shared_flow("/mm2.txt", "shared-ramfs");

    /* 5: shared write-back on the disk volume (if present) */
    shared_flow("/data/mm3.txt", "shared-yfs");

    /* 6: nonzero file offset selects the right page */
    {
        static char big[8192];
        long i;
        for (i = 0; i < 4096; i++)
            big[i] = 'a';
        for (i = 4096; i < 8192; i++)
            big[i] = 'b';
        write_file("/mm4.txt", big, 8192);
        fd = sys_call3(SYS_open, (long)(uintptr_t)"/mm4.txt",
                       O_RDONLY, 0);
        m = (char *)(uintptr_t)sys_mmap(NULL, 4096, PROT_READ,
                                        MAP_PRIVATE, fd, 4096);
        if ((long)(uintptr_t)m < 0) {
            report("offset", 0);
        } else {
            int ok = m[0] == 'b' && m[4095] == 'b';
            sys_call3(SYS_munmap, (long)(uintptr_t)m, 4096, 0);
            report("offset", ok);
        }
        sys_call3(SYS_close, fd, 0, 0);
        sys_call3(SYS_unlink, (long)(uintptr_t)"/mm4.txt", 0, 0);
    }
    t_lazy();
    t_cow();
    t_segv_wild();
    t_segv_ro();
    t_mprotect_ro();
    t_mprotect_mid();
    t_mprotect_err();
    sys_call3(SYS_unlink, (long)(uintptr_t)"/mm1.txt", 0, 0);

    out("MMTEST DONE\n");
    return g_fails == 0 ? 0 : 1;
}

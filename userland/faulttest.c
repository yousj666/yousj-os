/* faulttest.c - M24 acceptance: hostile user pointers
 * must produce -EFAULT, never a halted kernel.
 *
 * Scenarios:
 *  rusage-ro:   wait4 rusage aimed at a PROT_READ page
 *               (the exact M21 crash) -> -EFAULT, and the
 *               zombie survives for a corrected retry.
 *  read-ro:     read() INTO a read-only page -> -EFAULT.
 *  read-wild:   read() into never-mapped window space.
 *  stat-ro:     stat() out-buffer on a read-only page.
 *  sleep-wild:  nanosleep() request in unmapped space.
 *  tod-ro:      gettimeofday() out-buffer read-only.
 *  sigret-wild: rt_sigreturn with RSP in unmapped space,
 *               in a child: the kernel must hand back
 *               -EFAULT and the child exits 77.
 *  alive:       ordinary syscalls still work afterwards.
 *
 * Freestanding: raw syscalls only. Exit code = failures. */
#include <stdint.h>
#include <stddef.h>

#define SYS_read 0
#define SYS_write 1
#define SYS_open 2
#define SYS_close 3
#define SYS_stat 4
#define SYS_mmap 9
#define SYS_mprotect 10
#define SYS_clone 56
#define SYS_wait4 61
#define SYS_exit_group 231
#define SYS_nanosleep 35
#define SYS_getpid 39
#define SYS_gettimeofday 96

#define EFAULT_R (-14L)
#define WILD ((uintptr_t)0x8002800000ULL)

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
    out("faulttest: ");
    out(name);
    out(ok ? " PASS\n" : " FAIL\n");
    if (!ok)
        fails++;
}

int main(void)
{
    out("faulttest: M24 hostile pointer suite\n");

    /* A read-only page to aim out-buffers at. */
    long mp = sys_mmap(NULL, 4096, 3 /*RW*/,
                       0x22 /*ANON|PRIVATE*/, -1, 0);
    char *ro = (char *)(uintptr_t)mp;
    if (mp < 0) {
        report("setup", 0);
        out("FAULTTEST DONE\n");
        return 1;
    }
    ro[0] = 1; /* touch so the page is materialized */
    sys_call3(SYS_mprotect, (long)(uintptr_t)ro, 4096, 1 /*READ*/);

    /* rusage-ro: dedicated child that exits at once */
    {
        long pid = sys_call4(SYS_clone, 17 /*SIGCHLD*/, 0, 0, 0);
        if (pid == 0)
            sys_call1(SYS_exit_group, 5);
        int status = 0;
        char rusage[144];
        long r1 = sys_call4(SYS_wait4, pid,
                            (long)(uintptr_t)&status, 0,
                            (long)(uintptr_t)ro);
        long r2 = sys_call4(SYS_wait4, pid,
                            (long)(uintptr_t)&status, 0, 0);
        report("rusage-ro", pid > 0 && r1 == EFAULT_R &&
               r2 == pid && status == (5 << 8));
    }

    /* read-ro / read-wild */
    {
        long fd = sys_call3(SYS_open, (long)(uintptr_t)"/bin/hello",
                            0, 0);
        long r_ro = -999, r_wild = -999;
        if (fd >= 0) {
            r_ro = sys_call3(SYS_read, fd,
                             (long)(uintptr_t)ro, 16);
            r_wild = sys_call3(SYS_read, fd, (long)WILD, 16);
            sys_call1(SYS_close, fd);
        }
        report("read-ro", r_ro == EFAULT_R);
        report("read-wild", r_wild == EFAULT_R);
    }

    /* stat-ro */
    {
        long r = sys_call2(SYS_stat, (long)(uintptr_t)"/bin/hello",
                           (long)(uintptr_t)ro);
        report("stat-ro", r == EFAULT_R);
    }

    /* sleep-wild: request struct in unmapped space */
    {
        long r = sys_call2(SYS_nanosleep, (long)WILD, 0);
        report("sleep-wild", r == EFAULT_R);
    }

    /* tod-ro */
    {
        long r = sys_call2(SYS_gettimeofday,
                           (long)(uintptr_t)ro, 0);
        report("tod-ro", r == EFAULT_R);
    }

    /* sigret-wild in a child */
    {
        long pid = sys_call4(SYS_clone, 17, 0, 0, 0);
        if (pid == 0) {
            __asm__ volatile(
                "movq $0x8002800000, %%rsp\n\t"
                "movq $15, %%rax\n\t" /* rt_sigreturn */
                "syscall\n\t"
                "movq $231, %%rax\n\t" /* exit_group */
                "movq $77, %%rdi\n\t"
                "syscall\n\t"
                "ud2\n\t"
                : : : "memory");
            __builtin_unreachable();
        }
        int status = 0;
        long r = sys_call4(SYS_wait4, pid,
                           (long)(uintptr_t)&status, 0, 0);
        report("sigret-wild", r == pid && status == (77 << 8));
    }

    /* alive: the kernel is unharmed */
    {
        char b[4];
        long fd = sys_call3(SYS_open, (long)(uintptr_t)"/bin/hello",
                            0, 0);
        long n = fd >= 0 ? sys_call3(SYS_read, fd,
                                     (long)(uintptr_t)b, 4) : -1;
        if (fd >= 0)
            sys_call1(SYS_close, fd);
        report("alive", sys_call1(SYS_getpid, 0) > 0 && n == 4);
    }

    out("FAULTTEST DONE\n");
    return fails;
}

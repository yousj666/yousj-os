/* brktest - M22 acceptance scenarios for lazy brk.
 *
 * lazy, churn, limits, segv-past.
 * Prints "brktest: NAME PASS/FAIL" per scenario and
 * "BRKTEST DONE"; exit 0 iff all pass.
 * Freestanding, Linux ABI. */
#include <stdint.h>
#include <stddef.h>

#define SYS_write  1
#define SYS_clone  56
#define SYS_wait4  61
#define SYS_exit_group 231
#define SYS_brk    12
#define SYS_NATIVE_FREE 0x1001

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
    out("brktest: ");
    out(name);
    out(ok ? " PASS\n" : " FAIL\n");
    if (!ok)
        g_fails++;
}

static long brk_(long addr)
{
    return sys_call3(SYS_brk, addr, 0, 0);
}

static long free_pages(void)
{
    return sys_call3(SYS_NATIVE_FREE, 0, 0, 0);
}

int main(void)
{
    long base, f0, f1, f2;
    volatile char *heap;

    out("brktest: M22 lazy brk suite\n");
    base = brk_(0);
    heap = (volatile char *)(uintptr_t)base;
    if (base <= 0) {
        report("lazy", 0);
        report("churn", 0);
        report("limits", 0);
        report("segv-past", 0);
        out("BRKTEST DONE\n");
        return 1;
    }

    /* 1: growth is bookkeeping only; pages arrive on touch */
    f0 = free_pages();
    {
        long rc = brk_(base + 1024 * 1024);
        f1 = free_pages();
        heap[0] = 0x11;
        heap[512 * 1024] = 0x22;
        f2 = free_pages();
        report("lazy", rc == base + 1024 * 1024 && f1 == f0 &&
               f0 - f2 >= 2 && f0 - f2 <= 8 &&
               heap[0] == 0x11 && heap[512 * 1024] == 0x22);
    }

    /* 2: shrink returns the frames; regrown pages are zero */
    brk_(base + 4 * 1024 * 1024);
    heap[2 * 1024 * 1024] = 0x5A;
    {
        long before = free_pages();
        long rc1 = brk_(base);
        long mid = free_pages();
        long rc2 = brk_(base + 4 * 1024 * 1024);
        int zeroed = heap[2 * 1024 * 1024] == 0;
        report("churn", rc1 == base && rc2 == base + 4 * 1024 * 1024 &&
               mid > before && zeroed);
    }
    brk_(base);

    /* 3: out-of-range requests leave the break unchanged
     * (Linux returns the old break rather than an error) */
    {
        long cur = brk_(0);
        long r1 = brk_(base - 4096);
        long r2 = brk_(base + 128 * 1024 * 1024);
        report("limits", cur == base && r1 == base &&
               r2 == base && brk_(0) == base);
    }

    /* 4: the page AT a page-aligned break is out of bounds;
     * the byte just below it is the heap's last valid one */
    {
        long aligned = (base + 8192 + 4095) & ~4095L;
        long pid;
        int status = 0;
        int ok = brk_(aligned) == aligned;
        pid = sys_call3(SYS_clone, 17, 0, 0);
        if (pid == 0) {
            volatile char *past =
                (volatile char *)(uintptr_t)aligned;
            *past = 1; /* one past the break: must fault */
            sys_call3(SYS_exit_group, 99, 0, 0);
        }
        sys_call4(SYS_wait4, pid, (long)(uintptr_t)&status, 0, 0);
        ok = ok && (status & 0x7F) == 11;
        pid = sys_call3(SYS_clone, 17, 0, 0);
        if (pid == 0) {
            volatile char *last =
                (volatile char *)(uintptr_t)(aligned - 1);
            *last = 1; /* the last heap byte: must work */
            sys_call3(SYS_exit_group, 42, 0, 0);
        }
        sys_call4(SYS_wait4, pid, (long)(uintptr_t)&status, 0, 0);
        ok = ok && ((status >> 8) & 0xFF) == 42;
        brk_(base);
        report("segv-past", ok);
    }

    out("BRKTEST DONE\n");
    return g_fails == 0 ? 0 : 1;
}

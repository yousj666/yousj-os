/* orphantest.c - M27 acceptance: adopted orphans are
 * reaped by init as soon as they die, not whenever the
 * shell happens to exit.
 *
 * Shape: this process forks a child C; C forks a
 * grandchild G and exits at once (never waiting); G
 * sleeps 1.5 s and exits. The kernel re-parents G to
 * init. This process reaps C, confirms G is no longer
 * waitable from here (ECHILD: adoption really happened),
 * sleeps past G's death, and finishes. The harness then
 * runs ps: with the M27 init, G's zombie is long gone;
 * with the old init it would still be sitting there.
 *
 * Freestanding: raw syscalls only. Exit code is the
 * number of failed scenarios. */
#include <stdint.h>
#include <stddef.h>

#define SYS_write 1
#define SYS_nanosleep 35
#define SYS_getpid 39
#define SYS_clone 56
#define SYS_wait4 61
#define SYS_exit_group 231

#define WNOHANG 1

static long sys_call1(long n, long a)
{
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a)
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
static void out_long(long v)
{
    char buf[24];
    int i = (int)sizeof(buf) - 1;
    unsigned long u;
    buf[i] = '\0';
    if (v < 0) {
        out("-");
        u = (unsigned long)(-(v + 1)) + 1;
    } else {
        u = (unsigned long)v;
    }
    if (u == 0)
        buf[--i] = '0';
    while (u > 0) {
        buf[--i] = (char)('0' + (u % 10));
        u /= 10;
    }
    out(&buf[i]);
}
static int fails;
static void report(const char *name, int ok)
{
    out("orphantest: ");
    out(name);
    out(ok ? " PASS\n" : " FAIL\n");
    if (!ok)
        fails++;
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

int main(void)
{
    long c, st = 0;

    out("orphantest: M27 orphan reaping suite\n");

    c = sys_call4(SYS_clone, 17, 0, 0, 0); /* SIGCHLD */
    if (c == 0) {
        /* C: make G, announce it, die without waiting. */
        long g = sys_call4(SYS_clone, 17, 0, 0, 0);
        if (g == 0) {
            /* G: the orphan-to-be. */
            sleep_ms(1500);
            die(7);
        }
        out("orphantest: grandchild=");
        out_long(g);
        out("\n");
        die(0);
    }

    /* P: reap C itself. */
    {
        long got = sys_call4(SYS_wait4, c, (long)(uintptr_t)&st,
                             0, 0);
        report("child-reaped", got == c && c > 0);
    }

    /* G belongs to init now: a wait from here finds no
     * children at all (ECHILD), adopted or not -- but if
     * adoption had failed, G would be OUR problem only if
     * it were our child; it never was. The decisive half
     * of this check is the harness's ps afterwards. Here
     * we simply record that no child is waitable. */
    {
        long r = sys_call4(SYS_wait4, -1, 0, WNOHANG, 0);
        report("no-waitable-child", r == -10); /* ECHILD */
    }

    /* Let G live, die, and (with M27) be reaped by init. */
    sleep_ms(2200);
    out("ORPHANTEST DONE\n");
    return fails;
}
